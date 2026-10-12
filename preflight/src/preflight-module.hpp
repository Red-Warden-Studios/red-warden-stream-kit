/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// Pre-Flight: the dock and shutdown.
// Called by plugin/src/plugin-main.cpp, the Stream Kit's single OBS module. The tool no longer registers
// a dock (the Kit dock hosts its panel); the module entry points that used to live in its plugin-main.cpp live here.

#include <obs-frontend-api.h>

class PreflightDock;

namespace preflight_module {

// Creates the panel (hidden, parented to OBS's main window) and registers the tool's frontend callback.
// The panel is NOT a dock of its own: the Kit dock hosts it. Always true; kept so every tool has the same
// entry points.
bool load();
// The panel after load(), else nullptr. Owned by whoever it is parented to (the Kit dock).
PreflightDock *panel();
// Nothing to do; kept so every tool has the same entry points.
void post_load();
// The tool's own frontend event callback; load() registers it and unload() removes it.
void frontend_event(enum obs_frontend_event event, void *);
void unload();

} // namespace preflight_module
