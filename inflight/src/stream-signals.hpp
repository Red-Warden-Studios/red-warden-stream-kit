/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "inflight-eval.hpp"

#include <obs.h>

#include <QtGlobal>

#include <memory>
#include <vector>

namespace inflight {

// Holds one strong reference to the frontend's streaming output and listens to its stop, reconnect
// and reconnect_success signals. The callbacks run on libobs threads and only copy the event (and
// the stop code) into a small mutex-guarded queue; the UI thread drains it once a second.
// bind/unbind/sync/drain/pushStarted are UI thread only.
class StreamSignals {
public:
	StreamSignals();
	~StreamSignals();
	StreamSignals(const StreamSignals &) = delete;
	StreamSignals &operator=(const StreamSignals &) = delete;

	// Follows obs_frontend_get_streaming_output(): binds a new output, rebinds a replaced one
	// (queueing OutputReplaced), unbinds when there is none.
	void sync();
	// Disconnects all three signals, then releases the ref. Idempotent.
	void unbind();
	obs_output_t *output() const { return output_; } // borrowed, may be null
	void pushStarted(qint64 atMs);
	// Moves out the queued events, oldest first.
	std::vector<StreamEvent> drain();

	struct Queue; // opaque; heap object the signal callbacks point at

private:
	void bind(obs_output_t *out); // takes over the caller's ref
	std::unique_ptr<Queue> q_;
	obs_output_t *output_ = nullptr;
};

} // namespace inflight
