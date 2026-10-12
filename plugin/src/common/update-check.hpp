/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// "Is there a newer version?" - once per OBS start, never more.
//
// Reads one small JSON file from redwardenstudios.com (see update-manifest.hpp)
// and, if it lists a newer version, the dock shows a one-line notice with a
// Download link. It NEVER downloads or runs anything itself: the user clicks
// through to the website and runs the signed installer, which upgrades in place
// and keeps every setting. Sends nothing but a plain GET whose User-Agent names
// the plugin version. Any failure (offline, timeout, bad JSON) is silent.
//
// The Stream Kit has one update notice: only the Kit dock (src/kit/kit-dock.cpp) starts the checker, whichever
// tools are switched on. Pre-Flight and In-Flight link the class (they hold one so their shutdown stays
// uniform) but never call start(); Multistream no longer holds one.

#include "update-manifest.hpp"

#include <QObject>

#include <memory>
#include <thread>

// Runs the one request on a worker thread and reports back on the UI thread.
//
// The worker alone owns its WinHTTP handles (closing a synchronous handle from
// another thread is unsupported). stop() - called from the dock's shutdown() -
// marks the check cancelled under a lock, so the worker can never post to the
// dock afterwards, then waits up to 1.5 s for it. If the network is still
// stuck, the plugin DLL is pinned in memory and the thread is detached: OBS
// exits without hanging, and the thread's code can't be unloaded under it.
class UpdateChecker : public QObject {
	Q_OBJECT
public:
	explicit UpdateChecker(QObject *parent = nullptr);
	~UpdateChecker() override;

	void start(const QString &currentVersion);
	void stop();

	struct Shared; // state shared with the worker (outlives this object if detached)

signals:
	// Only emitted when a strictly newer version is listed.
	void updateAvailable(const updatecheck::Manifest &m);

private:
	std::thread worker;
	std::shared_ptr<Shared> shared;
};
