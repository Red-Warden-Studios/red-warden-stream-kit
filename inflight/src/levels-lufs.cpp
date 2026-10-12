/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "levels-lufs.hpp"
#include "kit-cost.hpp"

#include <QtGlobal>

#include <media-io/audio-io.h>
#include <plugin-support.h>

namespace inflight {

namespace {
// Test hook only, to prove the live volume-move check can fail: with RWS_INFLIGHT_TEST_LUFS_NOGAIN set, the gain
// atomics are never updated after the first read (no signals, no beat refresh).
bool testNoGainUpdates()
{
	static const bool on = qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST_LUFS_NOGAIN");
	return on;
}
} // namespace

// Audio thread. Lock-free, allocation-free, no logging.
void LevelsLufs::onAudio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	kitcost::Scope costScope(kitcost::LufsCapture); // atomics only
	Tap *t = static_cast<Tap *>(param);
	if (!t || !t->ring || !audio)
		return;
	(void)source;
	// The fader gain is NOT read from libobs here (obs_source_get_volume reads user_volume, which libobs writes
	// unsynchronised from other threads): it is a per-tap atomic kept by the "volume" signal and the 5 Hz beat.
	// OBS applies timestamped volume changes while mixing, so a block-wide gain is an approximation at the
	// instant of a fader move (one callback block, about 10 ms to 20 ms).
	float gain = t->gain.load(std::memory_order_relaxed);
	if (!(gain > 0.0f)) // also catches NaN
		gain = 0.0f;
	// Mute semantics of OBS's own volmeter (obs-audio-controls.c: `muted && !obs_source_muted(source)`): the
	// callback's `muted` flag (mute, disabled, push-to-mute / push-to-talk) zeroes the copy, EXCEPT an explicit
	// user mute, which still shows the incoming level so LUFS mode describes the same signal as dB mode. (The
	// In-Flight Mic light already reports a user mute.)
	if (muted && !t->userMuted.load(std::memory_order_relaxed))
		gain = 0.0f;
	const float *planes[kLoudnessMaxChannels];
	const int ch = t->ring->channels();
	for (int c = 0; c < ch; c++)
		planes[c] = reinterpret_cast<const float *>(audio->data[c]);
	t->ring->write(planes, audio->frames, gain);
}

bool LevelsLufs::start(const QStringList &uuids, bool truePeak)
{
	stop();
	audio_t *audio = obs_get_audio();
	if (!audio)
		return false;
	const double rate = static_cast<double>(audio_output_get_sample_rate(audio));
	const int channels = static_cast<int>(audio_output_get_channels(audio));
	ChannelRole roles[kLoudnessMaxChannels];
	if (channelRolesForObsLayout(channels, roles) != channels) {
		for (int i = 0; i < kLoudnessMaxChannels; i++)
			roles[i] = ChannelRole::Other; // no such OBS layout: weigh every channel 1.0
	}
	if (!worker_.start(rate, roles, channels, static_cast<int>(uuids.size()), truePeak))
		return false;
	uuids_ = uuids;
	tp_ = truePeak;
	for (int i = 0; i < uuids.size(); i++) {
		auto t = std::make_unique<Tap>();
		t->uuid = uuids[i];
		t->ring = worker_.ring(i);
		taps_.push_back(std::move(t));
	}
	for (auto &t : taps_)
		attach(*t);
	obs_log(LOG_INFO, "levels: lufs capture started (%d sources, %.0f Hz, %d ch, true peak %s)",
		static_cast<int>(uuids.size()), rate, channels, truePeak ? "on" : "off");
	return true;
}

void LevelsLufs::attach(Tap &t)
{
	if (t.src || t.uuid.isEmpty())
		return;
	obs_source_t *s = obs_get_source_by_uuid(t.uuid.toUtf8().constData());
	if (!s)
		return;
	if (!(obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO)) {
		obs_source_release(s);
		return;
	}
	t.src = s;
	refreshTap(t);
	signal_handler_t *sh = obs_source_get_signal_handler(s);
	if (sh && !testNoGainUpdates()) {
		signal_handler_connect(sh, "volume", &LevelsLufs::onVolume, &t);
		signal_handler_connect(sh, "mute", &LevelsLufs::onMute, &t);
	}
	// Re-read once after connecting so a change between the first read and the connect is not lost.
	refreshTap(t);
	obs_source_add_audio_capture_callback(s, &LevelsLufs::onAudio, &t);
}

// Any thread (libobs signal). `volume` is the value about to be set (a handler may even override it in libobs).
void LevelsLufs::onVolume(void *param, calldata_t *cd)
{
	Tap *t = static_cast<Tap *>(param);
	if (t && cd)
		t->gain.store(static_cast<float>(calldata_float(cd, "volume")), std::memory_order_relaxed);
}

void LevelsLufs::onMute(void *param, calldata_t *cd)
{
	Tap *t = static_cast<Tap *>(param);
	if (t && cd)
		t->userMuted.store(calldata_bool(cd, "muted"), std::memory_order_relaxed);
}

// UI thread. The fallback for the signals (5 Hz beat) and the first value.
void LevelsLufs::refreshTap(Tap &t)
{
	if (!t.src)
		return;
	t.gain.store(obs_source_get_volume(t.src), std::memory_order_relaxed);
	t.userMuted.store(obs_source_muted(t.src), std::memory_order_relaxed);
}

void LevelsLufs::refreshGains()
{
	if (testNoGainUpdates())
		return;
	for (auto &t : taps_)
		refreshTap(*t);
}

// Order matters: the callbacks come off first (removal blocks until an in-flight callback returns), only then
// does the worker stop and free the rings the callbacks wrote into.
void LevelsLufs::stop()
{
	for (auto &t : taps_) {
		if (t->src) {
			signal_handler_t *sh = obs_source_get_signal_handler(t->src);
			if (sh) { // disconnect before the tap is freed (the handlers hold a pointer to it)
				signal_handler_disconnect(sh, "volume", &LevelsLufs::onVolume, t.get());
				signal_handler_disconnect(sh, "mute", &LevelsLufs::onMute, t.get());
			}
			obs_source_remove_audio_capture_callback(t->src, &LevelsLufs::onAudio, t.get());
			obs_source_release(t->src);
			t->src = nullptr;
		}
		t->ring = nullptr;
	}
	worker_.stop();
	taps_.clear();
}

void LevelsLufs::retryUnattached()
{
	if (!worker_.running())
		return;
	for (auto &t : taps_)
		attach(*t);
}

bool LevelsLufs::restartIfFormatChanged()
{
	if (!worker_.running())
		return false;
	audio_t *audio = obs_get_audio();
	if (!audio)
		return false;
	if (static_cast<double>(audio_output_get_sample_rate(audio)) == worker_.sampleRate() &&
	    static_cast<int>(audio_output_get_channels(audio)) == worker_.channels())
		return false;
	obs_log(LOG_INFO, "levels: audio format changed, restarting lufs capture");
	const QStringList list = uuids_;
	const bool tp = worker_.truePeak();
	start(list, tp);
	return true;
}

bool LevelsLufs::attached(int slot) const
{
	return slot >= 0 && slot < static_cast<int>(taps_.size()) && taps_[static_cast<size_t>(slot)]->src != nullptr;
}

QString LevelsLufs::sourceName(int slot) const
{
	if (!attached(slot))
		return QString();
	const char *n = obs_source_get_name(taps_[static_cast<size_t>(slot)]->src);
	return n ? QString::fromUtf8(n) : QString();
}

int LevelsLufs::attachedCount() const
{
	int n = 0;
	for (const auto &t : taps_)
		n += t->src ? 1 : 0;
	return n;
}

std::uint64_t LevelsLufs::overflowBlocks() const
{
	std::uint64_t sum = 0;
	for (int i = 0; i < static_cast<int>(taps_.size()); i++)
		sum += worker_.result(i).droppedBlocks;
	return sum;
}

} // namespace inflight
