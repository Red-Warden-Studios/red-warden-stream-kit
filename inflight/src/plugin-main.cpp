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

#include "inflight-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QPointer>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Red Warden In-Flight";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "A live watchdog for your stream. By Red Warden Studios.";
}

static QPointer<InflightDock> g_dock; // UI thread only
static const char *kDockId = "rws-inflight-dock";

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	// UI thread. EXIT makes the dock shut down its worker and timers before libobs and Qt begin
	// tearing down.
	if (g_dock)
		g_dock->handleFrontendEvent(event);
}

#ifdef _WIN32
// Keep this DLL mapped until the process exits: the update worker may still be
// inside our code a moment after obs_module_unload.
static void pinThisModule()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			   reinterpret_cast<LPCWSTR>(&pinThisModule), &self);
}
#endif

bool obs_module_load(void)
{
#ifdef _WIN32
	pinThisModule();
#endif
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	g_dock = new InflightDock(main);
	const char *title = obs_module_text("InFlight.DockTitle");
	if (!title || strcmp(title, "InFlight.DockTitle") == 0)
		title = "In-Flight";
	if (!obs_frontend_add_dock_by_id(kDockId, title, g_dock.data())) {
		obs_log(LOG_ERROR, "could not register the dock");
		delete g_dock.data();
		return false;
	}
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "dock registered (id %s)", kDockId);
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "unloaded");
}
