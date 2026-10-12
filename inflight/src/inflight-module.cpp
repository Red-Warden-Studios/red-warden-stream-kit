/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "inflight-module.hpp"

#include "inflight-dock.hpp"
#include "multistream-live.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QPointer>

namespace inflight_module {

static QPointer<InflightDock> g_dock; // UI thread only

void frontend_event(enum obs_frontend_event event, void *)
{
	// UI thread. EXIT makes the dock shut down its worker and timers before libobs and Qt begin
	// tearing down.
	if (g_dock)
		g_dock->handleFrontendEvent(event);
}

InflightDock *panel()
{
	return g_dock.data();
}

bool load()
{
	// Created hidden and parented to the main window; the Kit dock takes it over (this tool no longer
	// registers a dock of its own).
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	g_dock = new InflightDock(main);
	g_dock->hide();
	obs_frontend_add_event_callback(frontend_event, nullptr);
	obs_log(LOG_INFO, "panel created (shown in the Stream Kit dock)");
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void unload()
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	obs_log(LOG_INFO, "unloaded");
}

void post_load() {}

} // namespace inflight_module
