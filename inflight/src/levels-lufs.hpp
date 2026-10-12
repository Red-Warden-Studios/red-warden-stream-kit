/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The libobs side of the Levels panel's LUFS mode. Per wanted source it registers an audio capture callback
// (obs_source_add_audio_capture_callback) whose audio-thread body only multiplies the planar float frames by
// the source's fader gain (0 while muted) and writes them into that source's SpscFrameRing: no allocation, no
// lock, no logging. One LoudnessWorker thread (loudness-worker.hpp) drains the rings. UI thread only, except
// the callback.
//
// What the capture callback delivers, from libobs (obs-source.c, source_output_audio_data, 31.1.1): the
// source's audio ALREADY converted to the mixer's sample rate and channel count, with balance and force-mono
// and the async audio filters applied, but BEFORE the source's volume (apply_audio_volume runs later, in the
// mixer tick) and with mute passed as a separate flag. So this applies, to match the post-fader dB mode:
//   applied     : the fader volume, from a per-tap atomic kept by the source's "volume" signal and the 5 Hz beat
//                 (never read from libobs on the audio thread: user_volume is unsynchronised), block-wide, so an
//                 approximation at the instant of a fader move (OBS applies timestamped changes while mixing),
//                 the callback's `muted` flag (disabled, push-to-mute / push-to-talk), EXCEPT an explicit user
//                 mute, which still reads the incoming level, exactly as OBS's own volmeter does,
//   NOT applied : the volume fade of a scene transition (get_source_volume's action fades), track / mixer
//                 assignment (a source on no track still reads), sync offset and monitoring (neither changes
//                 what the level is), and anything after the source (the master / track mix, limiter).
// Removal is synchronous: obs_source_remove_audio_capture_callback takes the same audio_cb_mutex the callback
// runs under, so once it returns no callback is in flight and none will start; only then are the rings freed.
#pragma once

#include "loudness-worker.hpp"

#include <obs.h>

#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>
#include <vector>

namespace inflight {

class LevelsLufs {
public:
	LevelsLufs() = default;
	~LevelsLufs() { stop(); }
	LevelsLufs(const LevelsLufs &) = delete;
	LevelsLufs &operator=(const LevelsLufs &) = delete;

	// (Re)start for these uuids (slot i = uuids[i]): reads the mixer's rate and channel count, starts the
	// worker, attaches the capture callbacks that resolve. A uuid that does not resolve to an audio source
	// stays unattached (retryUnattached() tries again). Returns false when OBS has no audio output.
	bool start(const QStringList &uuids, bool truePeak);
	// Remove every callback, then stop the worker, then free the rings. Idempotent.
	void stop();
	bool running() const { return worker_.running(); }

	// For the slow beat: attach slots whose source did not exist before.
	void retryUnattached();
	// If the mixer's rate or channel count differs from the one started with (Settings > Audio applied), restart
	// with the same list. Returns true when it restarted.
	bool restartIfFormatChanged();
	void setTruePeak(bool on) { worker_.setTruePeak(on); }
	// 5 Hz beat: re-read volume and user mute from the sources (fallback for the "volume" / "mute" signals).
	void refreshGains();

	int slotCount() const { return static_cast<int>(taps_.size()); }
	bool attached(int slot) const;
	LufsResult read(int slot) const { return worker_.result(slot); }
	QString sourceName(int slot) const;
	int attachedCount() const;
	LoudnessWorker::Cost workerCost() const { return worker_.cost(); }
	// Ring overflows summed over all slots (blocks dropped because the worker fell behind).
	std::uint64_t overflowBlocks() const;
	double sampleRate() const { return worker_.sampleRate(); }
	int channels() const { return worker_.channels(); }

private:
	struct Tap {
		QString uuid;
		SpscFrameRing *ring = nullptr; // owned by the worker
		obs_source_t *src = nullptr;   // strong ref while the callback is registered
		std::atomic<float> gain{1.0f};      // fader gain, written by the volume signal / beat, read by the callback
		std::atomic<bool> userMuted{false}; // explicit user mute (see onAudio), same
	};
	void attach(Tap &t);
	static void refreshTap(Tap &t);
	static void onVolume(void *param, calldata_t *cd);
	static void onMute(void *param, calldata_t *cd);
	static void onAudio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted);

	LoudnessWorker worker_;
	std::vector<std::unique_ptr<Tap>> taps_;
	QStringList uuids_;
	bool tp_ = false;
};

} // namespace inflight
