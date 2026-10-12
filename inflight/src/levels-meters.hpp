/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Volmeters for the Levels panel: one per wanted source, keyed by source UUID, attached only while the
// panel asks for them. Independent of AudioWatch (a second meter on the mic is cheap). The audio-thread
// callback writes numbers into a LevelAccumulator and does nothing else; everything else here is UI
// thread only.
#pragma once

#include "levels-handoff.hpp"

#include <obs.h>
#include <obs-audio-controls.h>

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <limits>
#include <memory>

namespace inflight {

class LevelsMeters {
public:
	// One slot's reading. Plain numbers only (no QString, no allocation): the 20 Hz tick reads these.
	struct SlotReading {
		bool attached = false;  // a meter is attached for this slot
		float peakDb = -std::numeric_limits<float>::infinity();
		float avgDb = -std::numeric_limits<float>::infinity();
		bool fresh = false;     // callbacks arrived since the previous takeSlot(): peakDb / avgDb are a real reading
		bool available = false; // a callback arrived within kStaleMs; false = no reading (not silence)
	};

	LevelsMeters() = default;
	~LevelsMeters();
	LevelsMeters(const LevelsMeters &) = delete;
	LevelsMeters &operator=(const LevelsMeters &) = delete;

	// Attach a meter for every uuid not yet attached (a uuid that does not resolve to an audio source is
	// skipped, retried next call, logged once), detach every meter whose uuid is not in the list and every
	// source OBS has removed.
	void setWanted(const QStringList &uuids);
	// Detach everything. Idempotent.
	void detachAll();
	// Slots: slot i belongs to the i-th uuid of the LAST setWanted() list (the panel's row order), attached or
	// not. The handle is stable until the next setWanted(). No UUID matching and no allocation per read.
	// Takes what accumulated since the last call for that slot (peak max, mean power), so call it once per
	// tick. A meter with no callback for kStaleMs is `available = false`: unavailable, which is not the same
	// as silent (measured silence arrives as available + fresh + -inf). An empty / out-of-range slot reads
	// attached = false.
	SlotReading takeSlot(int slot, qint64 tMs);
	// The attached source's name ("" when the slot is not attached). Calls libobs and allocates: for the slow
	// (text) beat only, never the fast tick.
	QString sourceName(int slot) const;

	int attachedCount() const { return meters_.size(); }

	static constexpr qint64 kStaleMs = 1500;

private:
	struct Meter {
		QString uuid;
		obs_source_t *src = nullptr; // strong ref while attached
		obs_volmeter_t *vol = nullptr;
		std::unique_ptr<LevelsHandoff> acc; // outlives the callback registration
	};

	void detach(const QString &uuid, const char *reason);
	static void onVolmeter(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			       const float peak[MAX_AUDIO_CHANNELS], const float inputPeak[MAX_AUDIO_CHANNELS]);

	QHash<QString, std::shared_ptr<Meter>> meters_;
	QVector<std::shared_ptr<Meter>> slots_; // wanted order -> meter (null = not attached); rebuilt by setWanted
	QStringList failedLogged_; // uuids whose attach failure is already in the log
};

} // namespace inflight
