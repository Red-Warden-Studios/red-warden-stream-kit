/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "stream-signals.hpp"
#include "inflight-clock.hpp"
#include "inflight-probe-logic.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <mutex>

namespace inflight {
namespace {
constexpr size_t kMaxQueued = 64;
}

// Lives as long as the StreamSignals, so a callback that is still running when unbind() returns
// can never touch freed memory. Only the destructor of StreamSignals frees it, after unbind().
struct StreamSignals::Queue {
	std::mutex m;
	std::vector<StreamEvent> events;
	Queue() { events.reserve(kMaxQueued + 1); } // so a push from a libobs thread never allocates

	void push(const StreamEvent &e)
	{
		std::lock_guard<std::mutex> lk(m);
		if (events.size() >= kMaxQueued) {
			// Drop the oldest event that is not a stop: a stop code is the one fact the Stream light
			// cannot do without. Only a queue made entirely of stops loses its oldest stop.
			auto it = events.begin();
			while (it != events.end() &&
			       (it->kind == StreamEventKind::StoppedOk || it->kind == StreamEventKind::StoppedError))
				++it;
			events.erase(it != events.end() ? it : events.begin());
		}
		events.push_back(e);
	}
};

namespace {
// The three callbacks run on libobs threads. They copy the event and (for stop) the code, nothing else.
void onStop(void *data, calldata_t *cd)
{
	auto *q = static_cast<StreamSignals::Queue *>(data);
	const int code = static_cast<int>(calldata_int(cd, "code"));
	StreamEvent e;
	e.kind = stopKindForCode(code);
	e.code = code;
	e.atMs = nowMs();
	q->push(e);
}

void onReconnect(void *data, calldata_t *)
{
	auto *q = static_cast<StreamSignals::Queue *>(data);
	StreamEvent e;
	e.kind = StreamEventKind::Reconnect;
	e.atMs = nowMs();
	q->push(e);
}

void onReconnectSuccess(void *data, calldata_t *)
{
	auto *q = static_cast<StreamSignals::Queue *>(data);
	StreamEvent e;
	e.kind = StreamEventKind::ReconnectSuccess;
	e.atMs = nowMs();
	q->push(e);
}
} // namespace

StreamSignals::StreamSignals() : q_(new Queue) {}

StreamSignals::~StreamSignals()
{
	unbind();
}

void StreamSignals::bind(obs_output_t *out)
{
	output_ = out;
	signal_handler_t *sh = obs_output_get_signal_handler(out);
	signal_handler_connect(sh, "stop", onStop, q_.get());
	signal_handler_connect(sh, "reconnect", onReconnect, q_.get());
	signal_handler_connect(sh, "reconnect_success", onReconnectSuccess, q_.get());
}

void StreamSignals::unbind()
{
	if (!output_)
		return;
	// Disconnect all three BEFORE releasing our reference.
	signal_handler_t *sh = obs_output_get_signal_handler(output_);
	signal_handler_disconnect(sh, "stop", onStop, q_.get());
	signal_handler_disconnect(sh, "reconnect", onReconnect, q_.get());
	signal_handler_disconnect(sh, "reconnect_success", onReconnectSuccess, q_.get());
	obs_output_release(output_);
	output_ = nullptr;
}

void StreamSignals::sync()
{
	obs_output_t *now = obs_frontend_get_streaming_output(); // a new reference, or null
	if (now == output_) {
		if (now)
			obs_output_release(now); // we already hold one
		return;
	}
	const bool replaced = (output_ != nullptr && now != nullptr);
	unbind();
	if (now)
		bind(now);
	if (replaced) {
		StreamEvent e;
		e.kind = StreamEventKind::OutputReplaced;
		e.atMs = nowMs();
		q_->push(e);
	}
}

void StreamSignals::pushStarted(qint64 atMs)
{
	StreamEvent e;
	e.kind = StreamEventKind::Started;
	e.atMs = atMs;
	q_->push(e);
}

std::vector<StreamEvent> StreamSignals::drain()
{
	std::vector<StreamEvent> out;
	std::lock_guard<std::mutex> lk(q_->m);
	out.swap(q_->events);
	q_->events.reserve(kMaxQueued + 1);
	return out;
}

} // namespace inflight
