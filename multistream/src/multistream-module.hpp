/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// Multistream: the dock, its obs-websocket vendor requests and shutdown.
// Called by plugin/src/plugin-main.cpp, the Stream Kit's single OBS module. The tool no longer registers
// a dock (the Kit dock hosts its panel); the module entry points that used to live in its plugin-main.cpp live here.

#include <obs-frontend-api.h>

class MultistreamDock;

namespace multistream_module {

// Creates the Multistream panel (hidden, parented to OBS's main window) and its frontend callback. The
// panel is NOT registered as a dock: the Kit dock (plugin/src/kit/kit-dock.cpp) hosts it. False never today;
// kept so every tool has the same entry points.
bool load();
// The panel after a successful load(), else nullptr. Owned by whoever it is parented to (the Kit dock).
MultistreamDock *panel();
// Registers the obs-websocket vendor requests (every module is loaded by now).
void post_load();
// The tool's own frontend event callback; load() registers it and unload() removes it.
void frontend_event(enum obs_frontend_event event, void *);
void unload();

} // namespace multistream_module
