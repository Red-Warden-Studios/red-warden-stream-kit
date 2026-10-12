/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "audio-watch.hpp"
#include "inflight-clock.hpp"
#include "kit-cost.hpp"

#include <plugin-support.h>

#include <cmath>
#include <cstdio>

namespace inflight {
static_assert(MAX_AUDIO_CHANNELS == LevelAccumulator::kMaxChannels, "channel slot count must match libobs");
namespace {
// "-12.3" or "-inf", never the platform's spelling of infinity.
QString dbText(float v)
{
	if (std::isinf(v) && v < 0)
		return QStringLiteral("-inf");
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(v));
	return QString::fromLatin1(buf);
}
} // namespace

AudioWatch::AudioWatch()
{
	testHooks_ = qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST");
}

AudioWatch::~AudioWatch()
{
	detachAll();
}

// Audio thread. Post-fader peak and magnitude, max over every channel slot. Unused slots read -inf.
// No allocation, no lock, no logging: two small loops and a few atomics.
void AudioWatch::onVolmeter(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			    const float peak[MAX_AUDIO_CHANNELS], const float[])
{
	kitcost::Scope costScope(kitcost::VolmeterWatch); // atomics only
	auto *acc = static_cast<LevelAccumulator *>(param);
	// Only the meter's current channel count (kept up to date by fill()): libobs clears unused peak
	// slots but not unused magnitude slots, so a stale loud slot must not count. All-NaN -> NaN ->
	// onCallback ignores the callback.
	const int n = acc->channels();
	acc->onCallback(LevelAccumulator::maxDb(peak, n), LevelAccumulator::maxDb(magnitude, n), nowMs());
}

void AudioWatch::sync(const Settings &s, LiveState live, qint64 nowMs)
{
	syncSlot(Which::Mic, s.micUuid, live, nowMs);
	syncSlot(Which::Desktop, s.desktopUuid, live, nowMs);
}

void AudioWatch::syncSlot(Which w, const QString &wantUuid, LiveState live, qint64 nowMs)
{
	Slot &sl = slot(w);
	// Continuous Unknown only; Live and Off reset it, and so does a slot that is not attached.
	const bool unknownTooLong =
		sl.unknownGrace.expired(sl.attached && live == LiveState::Unknown, nowMs, kUnknownDetachMs);
	if (sl.attached) {
		if (live == LiveState::Off)
			detach(w, "stream off");
		else if (sl.uuid != wantUuid)
			detach(w, "source changed");
		else if (obs_source_removed(sl.src))
			detach(w, "source removed");
		else if (unknownTooLong)
			detach(w, "live state unknown");
	}
	if (!sl.attached && live == LiveState::Live && !wantUuid.isEmpty())
		attach(w, wantUuid);
}

void AudioWatch::attach(Which w, const QString &uuid)
{
	Slot &sl = slot(w);
	const char *nm = name(w);
	auto fail = [&](const char *why) {
		if (sl.failedUuid != uuid) { // log once per uuid, retry every tick quietly
			sl.failedUuid = uuid;
			obs_log(LOG_INFO, "audio-watch: attach %s failed (%s)", nm, why);
		}
	};

	obs_source_t *src = obs_get_source_by_uuid(uuid.toUtf8().constData()); // new reference
	if (!src)
		return fail("source not found");
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO)) {
		obs_source_release(src);
		return fail("not an audio source");
	}
	obs_volmeter_t *vol = obs_volmeter_create(OBS_FADER_LOG); // same mapping as OBS's mixer
	if (!vol) {
		obs_source_release(src);
		return fail("volmeter create failed");
	}
	auto acc = std::make_unique<LevelAccumulator>();
	obs_volmeter_add_callback(vol, &AudioWatch::onVolmeter, acc.get());
	if (!obs_volmeter_attach_source(vol, src)) {
		obs_volmeter_remove_callback(vol, &AudioWatch::onVolmeter, acc.get());
		obs_volmeter_destroy(vol);
		obs_source_release(src);
		return fail("volmeter attach failed");
	}
	sl.src = src;
	sl.vol = vol;
	sl.acc = std::move(acc);
	sl.acc->setChannels(obs_volmeter_get_nr_channels(vol));
	sl.unknownGrace.reset();
	sl.uuid = uuid;
	sl.attached = true;
	sl.firstLogged = false;
	sl.failedUuid.clear();
	obs_log(LOG_INFO, "audio-watch: attach %s ok", nm);
}

// remove_callback FIRST: it takes the lock the volmeter holds while dispatching, so no callback runs
// after it returns. Only then is it safe to free the accumulator the callback pointed at.
void AudioWatch::detach(Which w, const char *reason)
{
	Slot &sl = slot(w);
	if (!sl.attached && !sl.vol && !sl.src)
		return;
	if (sl.vol) {
		obs_volmeter_remove_callback(sl.vol, &AudioWatch::onVolmeter, sl.acc.get());
		obs_volmeter_detach_source(sl.vol);
		obs_volmeter_destroy(sl.vol);
		sl.vol = nullptr;
	}
	if (sl.src) {
		obs_source_release(sl.src);
		sl.src = nullptr;
	}
	sl.acc.reset();
	const bool was = sl.attached;
	sl.attached = false;
	sl.uuid.clear();
	if (was)
		obs_log(LOG_INFO, "audio-watch: detach %s (%s)", name(w), reason);
}

void AudioWatch::detachAll()
{
	detach(Which::Mic, "shutdown or reset");
	detach(Which::Desktop, "shutdown or reset");
}

void AudioWatch::fill(AudioInput &a, Which w)
{
	Slot &sl = slot(w);
	a.attached = sl.attached;
	a.lastCallbackMs = 0;
	a.peakDb = -std::numeric_limits<float>::infinity();
	a.avgDb = -std::numeric_limits<float>::infinity();
	if (!sl.attached || !sl.acc)
		return;
	// obs_volmeter_get_nr_channels takes no lock: it reads the attached source's speaker layout and
	// OBS's audio info, plain ints, the same call OBS's own mixer makes from the UI thread. The result
	// goes to the callback through an atomic, so the audio thread never touches the volmeter itself.
	sl.acc->setChannels(obs_volmeter_get_nr_channels(sl.vol));
	const LevelAccumulator::Reading r = sl.acc->take();
	a.lastCallbackMs = r.lastCallbackMs;
	a.peakDb = r.peakDb;
	a.avgDb = r.avgDb;
	if (testHooks_ && r.fresh && !sl.firstLogged) {
		sl.firstLogged = true;
		const QByteArray line = QStringLiteral("audio-watch: %1 first reading peak=%2 avg=%3")
						.arg(QString::fromLatin1(name(w)), dbText(r.peakDb), dbText(r.avgDb))
						.toUtf8();
		obs_log(LOG_INFO, "%s", line.constData());
	}
}

} // namespace inflight
