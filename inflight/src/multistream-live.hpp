/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "inflight-probe-logic.hpp"

#include <QStringList>

#include <functional>
#include <optional>

namespace inflight {

// Asks Multistream whether any destination is live. Multistream is in this process (the Kit), so the question is a
// plain function call on the UI thread: the Kit hands over a source (setSource) that reads the destinations from
// Multistream's dock, and the controller's 1 Hz tick calls poll(). No thread, no obs-websocket round trip, nothing
// to wait for at exit. UI thread only.
class MultistreamLive {
public:
	// The "state" string of every destination ("live", "reconnecting", "off", ...), or std::nullopt when Multistream is
	// not available (switched off in the Kit, absent, or shutting down).
	using Source = std::function<std::optional<QStringList>()>;

	MultistreamLive() = default;
	MultistreamLive(const MultistreamLive &) = delete;
	MultistreamLive &operator=(const MultistreamLive &) = delete;

	// Set (or replace) the source; the next poll() asks at once. Never set = Multistream is not there (NotFound).
	void setSource(Source src);
	void start();
	void stop(); // idempotent; later polls return the last answer unchanged and ask nobody
	// Asks again if the cadence says so (2 s; 10 s while Multistream is not found) and returns the latest answer.
	// An answer is "Ok" (with anyLive) or "NotFound"; Pending only before the first poll after start().
	MsReply poll(qint64 nowMs);
	MsReply result() const { return last_; }

private:
	Source src_;
	MsReply last_; // status Pending until the first poll
	qint64 lastAskMs_ = 0;
	bool started_ = false;
	bool asked_ = false;
	bool logged_ = false;
	bool loggedHook_ = false;
	MsStatus loggedStatus_ = MsStatus::Pending;
	bool loggedLive_ = false;
};

} // namespace inflight
