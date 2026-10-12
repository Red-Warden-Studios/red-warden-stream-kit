/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Volmeters on the chosen mic and desktop sources, attached only while live. The audio-thread
// callback writes numbers into a LevelAccumulator and does nothing else; the 1 Hz tick reads them.
// Everything here is UI thread only except the static callback.
#pragma once

#include "inflight-eval.hpp"
#include "inflight-settings.hpp"
#include "level-accumulator.hpp"

#include <obs.h>
#include <obs-audio-controls.h>

#include <QString>

#include <memory>

namespace inflight {

constexpr qint64 kUnknownDetachMs = 30000; // continuous Unknown this long -> detach the meters

class AudioWatch {
public:
	enum class Which { Mic = 0, Desktop = 1 };

	AudioWatch();
	~AudioWatch();
	AudioWatch(const AudioWatch &) = delete;
	AudioWatch &operator=(const AudioWatch &) = delete;

	// Once a tick, before gather. Live: attach what is wanted. Off: detach. Unknown: no new attach,
	// and an attached slot stays only for kUnknownDetachMs of continuous Unknown (any Live tick resets
	// that grace). A changed uuid or a removed source detaches at once.
	void sync(const Settings &s, LiveState live, qint64 nowMs);
	// Detach both. Idempotent.
	void detachAll();
	// Sets attached, and (if attached) lastCallbackMs / peakDb / avgDb. Takes the accumulated maxima,
	// so call it once per tick per slot.
	void fill(AudioInput &a, Which w);

	// Test/diagnostic: slot attached right now.
	bool attached(Which w) const { return slot(w).attached; }

private:
	struct Slot {
		QString uuid;                         // the uuid this slot is attached for
		obs_source_t *src = nullptr;          // strong ref while attached
		obs_volmeter_t *vol = nullptr;
		std::unique_ptr<LevelAccumulator> acc; // outlives the callback registration
		bool attached = false;
		QString failedUuid;                   // attach failure already logged for this uuid
		bool firstLogged = false;
		GraceTimer unknownGrace;
	};

	Slot &slot(Which w) { return slots_[static_cast<int>(w)]; }
	const Slot &slot(Which w) const { return slots_[static_cast<int>(w)]; }
	static const char *name(Which w) { return w == Which::Mic ? "mic" : "desktop"; }

	void syncSlot(Which w, const QString &wantUuid, LiveState live, qint64 nowMs);
	void attach(Which w, const QString &uuid);
	void detach(Which w, const char *reason);

	static void onVolmeter(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			       const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);

	Slot slots_[2];
	bool testHooks_ = false;
};

} // namespace inflight
