/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "inflight-eval.hpp"
#include "inflight-settings.hpp"

#include <QtGlobal>

#include <vector>

namespace inflight {

class AudioWatch;
class StreamSignals;

// Reads public OBS state once a second and fills the evaluator's Inputs. UI thread only.
class Probe {
public:
	// Queued by the controller from the frontend recording events; handed to the evaluator by the next gather.
	void pushRecordingEvent(RecordingEventKind kind, qint64 atMs);

	// The AudioWatch (already synced this tick) supplies attached / lastCallbackMs / peakDb / avgDb.
	Inputs gather(const Settings &s, qint64 nowMs, LiveState live, StreamSignals &sig, AudioWatch &audio);

private:
	std::vector<RecordingEvent> recordingEvents_;
};

} // namespace inflight
