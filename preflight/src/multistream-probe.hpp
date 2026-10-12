/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "preflight-eval.hpp"

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>
#include <optional>
#include <vector>

namespace preflight {

struct MultistreamResult {
	MultistreamStatus status = MultistreamStatus::Pending;
	QStringList names; // enabled destination names (Ok only)
};

// One Multistream destination as Pre-Flight needs it.
struct MultistreamDestination {
	QString id, name;
	bool enabled = false;
};

// Asks Multistream which destinations are on. Multistream is in this process (the Kit), so the question is a plain
// function call on the UI thread: the Kit hands over a source (setSource) that reads the destinations from
// Multistream's dock, and the controller's 1 s tick calls poll(). No thread, no obs-websocket round trip, nothing to
// wait for at exit. UI thread only.
class MultistreamProbe {
public:
	// Every destination, or std::nullopt when Multistream is not available (switched off in the Kit, absent, or shutting down).
	using Source = std::function<std::optional<std::vector<MultistreamDestination>>()>;

	MultistreamProbe() = default;
	MultistreamProbe(const MultistreamProbe &) = delete;
	MultistreamProbe &operator=(const MultistreamProbe &) = delete;

	// Set (or replace) the source; the next poll() asks at once. Never set = Multistream is not there (NotFound).
	void setSource(Source src);
	void start();
	void poke(); // ask again at the next poll() instead of waiting out the 5 s cadence
	void stop(); // idempotent; later polls return the last answer unchanged and ask nobody
	// Asks again when poked, or every 5 s (every call with the RWS_PREFLIGHT_TEST_HAMMER test hook), and returns the latest answer.
	void poll(qint64 nowMs);
	MultistreamResult result() const { return last_; }

private:
	Source src_;
	MultistreamResult last_; // status Pending until the first poll
	qint64 lastAskMs_ = 0;
	bool started_ = false;
	bool poked_ = true;
	bool hammer_ = false;
	bool logged_ = false;
	bool loggedHook_ = false;
	MultistreamResult loggedResult_;
};

} // namespace preflight
