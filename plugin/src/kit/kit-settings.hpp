/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// The Kit's own settings, in the shared file plugin_config/rws-stream-kit/settings.json (the file that
// already holds "check_updates", see update-manifest.hpp). Qt Core only, no OBS: unit-tested by
// tests/kit-settings-test.cpp.
//
//   { "check_updates": true,                        (owned by updatecheck::*SharedCheckUpdates; never touched here)
//     "tools": { "multistream": true, "preflight": true, "inflight": true },
//     "auto_switch_on_live": true,
//     "show_all": false,
//     "multistream_collapsed": false,
//     "first_run_done": false }
//
// Saving is merge-preserving, the same rule saveSharedCheckUpdates uses: the file is read, only the keys
// above are replaced (inside "tools" only the three tool keys), every other key survives, and the write is
// atomic. A missing key, a key of the wrong type or a malformed file reads as the default.

#include <QByteArray>
#include <QString>

// First-run placement (the "first_run_done" marker). The Kit puts its dock in the right-hand area once, on the
// very first run; afterwards OBS's own saved layout rules. The marker lives in the file above, so a failed write
// would make the NEXT launch think it is the first again and drag a dock the user had moved or closed back into
// place. Two guards: the marker is only trusted when it is read back from disk (saveFirstRunDone), and a launch
// that finds OBS's saved layout already holding the dock never forces it (shouldForceDock / dockStateHasDock).

namespace kitsettings {

struct Settings {
	bool multistream = true; // tools.multistream
	bool preflight = true;   // tools.preflight
	bool inflight = true;    // tools.inflight
	bool autoSwitchOnLive = true;
	bool showAll = false;
	bool multistreamCollapsed = false;
	bool firstRunDone = false;

	bool operator==(const Settings &o) const
	{
		return multistream == o.multistream && preflight == o.preflight && inflight == o.inflight &&
		       autoSwitchOnLive == o.autoSwitchOnLive && showAll == o.showAll &&
		       multistreamCollapsed == o.multistreamCollapsed && firstRunDone == o.firstRunDone;
	}
	bool operator!=(const Settings &o) const { return !(*this == o); }
};

// Pure: JSON text -> settings (defaults for anything missing or not a bool).
Settings parse(const QByteArray &json);
// Pure: the new file content. `existing` is the file as it is now (may be empty or malformed).
QByteArray merge(const QByteArray &existing, const Settings &s);

// File access. pluginConfigRoot is ".../plugin_config" (kitpaths::pluginConfigRoot()). load() with an empty
// root returns the defaults; save() with an empty root returns false.
Settings load(const QString &pluginConfigRoot);
bool save(const QString &pluginConfigRoot, const Settings &s);

// Sets s.firstRunDone (in memory, whatever happens: any later successful save then writes it too), saves, and
// reads the file back. True only when the file on disk really says first_run_done=true.
bool saveFirstRunDone(const QString &pluginConfigRoot, Settings &s);

// Pure: does OBS's saved main-window state (QMainWindow::saveState bytes; OBS keeps them base64 in user.ini,
// [BasicWindow] DockState) hold a dock with this object name? Docks are written with their object name as a
// QDataStream QString (4-byte length of the UTF-16 data, then big-endian UTF-16), and that exact string is
// searched for, so "rws-stream-kit" does not match inside a longer name. Empty or garbage input is "no".
bool dockStateHasDock(const QByteArray &base64State, const QString &objectName);

// Pure: should the Kit put its dock in the default place now? Only on a first run (marker not set) where OBS has
// no saved place for the dock; a missing marker with a saved place means the marker was lost, not that the dock is
// new, and the user's layout wins.
bool shouldForceDock(bool firstRunDone, bool savedLayoutHasDock);

} // namespace kitsettings
