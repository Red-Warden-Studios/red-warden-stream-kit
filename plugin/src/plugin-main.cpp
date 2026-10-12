/*
Red Warden Stream Kit
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

// The one OBS module of the Red Warden Stream Kit. It owns every obs_module_* export and nothing
// else: each tool (Multistream, Pre-Flight, In-Flight) keeps its logic in its own tree and is driven through
// <tool>_module::load()/post_load()/unload(); the tools create their panels, and the ONE dock ("RW Stream Kit",
// kit/kit-dock.cpp) hosts them. A tool the user switched off (Kit settings, read here before anything loads)
// is not loaded at all: no widget, no timers, no outputs, no hotkeys. Each tool registers and removes its own
// frontend event callback; the Kit dock has one more.
//
// ---- EXIT and teardown order (audited against OBS 31.1.1 sources: frontend/widgets/OBSBasic.cpp, OBSStudioAPI.cpp,
//      OBSApp.cpp, obs-main.cpp). This is what really happens, in order: ----
//  1. OBSBasic::closeEvent: sendPostedEvents(all), then OnEvent(OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
//     (OBSBasic.cpp:1704), then ClearSceneData() (:1710; scenes and sources released), THEN
//     OnEvent(OBS_FRONTEND_EVENT_EXIT) (:1712), then obs_frontend_set_callbacks_internal(nullptr) (the frontend API is
//     dead from here on), then a QUEUED quit.
//     SCRIPTING_SHUTDOWN is therefore our "let go of sources" signal: every tool's own callback gets it (the order
//     does not matter, nothing there waits for another tool). In-Flight drops the Levels capture (audio callbacks off,
//     loudness worker joined, volmeters detached, source refs released) and the audio watch (volmeters on mic and
//     desktop); Pre-Flight ends a running mic test. Each keeps that state (no new capture can start) and EXIT's
//     shutdown, later, finds it already done (every release is idempotent).
//  2. OBSStudioAPI::on_event calls the callbacks in REVERSE registration order. We register Multistream, Pre-Flight,
//     In-Flight (each in its load()), then the Kit's, so OBS would call Kit, In-Flight, Pre-Flight, Multistream: the
//     opposite of what the tools assume (they need Multistream's gate closed first). kitFrontendEvent below is the
//     first to run and therefore drives EXIT itself, in the order we want; the tools' own callbacks that follow find
//     everything shut down (every shutdown is idempotent) and do nothing:
//       a. Multistream: websocket gate closed (g_exiting), vendor requests unregistered, dock shutdown (dialogs
//          rejected, upload thread joined, every output stopped and released, hotkeys unregistered).
//       b. Pre-Flight: dialogs rejected, update worker stopped, controller shutdown (timer, mic test volmeter, the
//          Multistream probe is told to drop its source: it has no thread).
//       c. In-Flight: dialog rejected, Levels shutdown (audio capture callbacks removed, loudness worker joined,
//          volmeters detached; exactly once), controller shutdown (timer, audio watch, Multistream source dropped: no
//          thread), lights panel, update worker.
//       Pre-Flight and In-Flight no longer have a Multistream probe worker at all: they ask Multistream's dock directly,
//       in this process, on their own UI-thread timers (MultistreamDock::destinationsNow(), wired by kit-dock.cpp), so
//       there is no obs-websocket call to be in flight, nothing to park, nothing to join and nothing to wait for. The
//       obs-websocket vendor stays registered for EXTERNAL clients only; nothing in this DLL calls it.
//       d. Kit dock: 1 Hz timer, update-check thread, Kit settings dialog.
//     NOTHING in a-d runs the Qt event loop: no processEvents, no nested exec(), so no queued call, close or deletion
//     can run in the middle of a shutdown.
//  3. Later, in the queued quit: the application loop ends; QCoreApplication::exec() flushes DeferredDelete and the main
//     window (WA_DeleteOnClose) is deleted: ~OBSBasic runs sendPostedEvents, releases its libobs data, then Qt deletes
//     its children, our QDockWidget among them: KitDock, then in creation order the three tool panels inside its
//     sections (their destructors call shutdown() again: no-ops), then the Levels section holding the LevelsPanel (its
//     destructor finds no capture left). All of this happens BEFORE libobs is shut down.
//  4. ~OBSApp -> obs_shutdown() -> modules unloaded: obs_module_unload below runs only now, inside obs_shutdown()
//     (obs.c:1396-1399: free_module() -> mod->unload(), obs-module.c:590), BEFORE obs_free_data() (obs.c:1404)
//     and before obs-websocket unloads (modules are pushed at the list head, obs-module.c:132-139, so the later-loaded
//     Kit unloads first). The docks are already gone (our QPointers read null), the frontend API is gone, so unload
//     removes callbacks (no-ops), waits for the last websocket request (an external client's) still inside the DLL
//     and logs. Nothing in unload joins or waits for a thread of ours that could be stuck in obs-websocket.
//     Every other worker is joined (or cannot reach OBS) before this point: the loudness worker is joined by
//     Levels shutdown (SCRIPTING_SHUTDOWN / EXIT); the Multistream upload-test thread (WinHTTP only, no OBS API) is
//     cancelled and joined by Multistream's dock shutdown; the update-check workers (WinHTTP only, no OBS API)
//     and Pre-Flight's disk probe (file system only) may outlive a short bounded wait but never call OBS, and
//     the DLL stays pinned for them. The DLL stays mapped (pinned) so a straggler thread runs
//     valid code, but pinning does not keep Qt objects alive: nothing may reach a widget after step 3.

#include "kit-dock.hpp"
#include "kit-paths.hpp"
#include "kit-settings.hpp"
#include "inflight-module.hpp"
#include "multistream-module.hpp"
#include "preflight-module.hpp"

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
	return "Red Warden Stream Kit";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Multistream, Pre-Flight and In-Flight for OBS. By Red Warden Studios.";
}

#ifdef _WIN32
// Keep this DLL mapped until the process exits. OBS only unloads plugins at exit anyway, and other
// threads (obs-websocket's request threads, the update-check worker) can call into our code a moment
// after obs_module_unload; pinned, that late call runs real code instead of jumping into an unmapped
// DLL. Once for the whole Kit (it used to be once per plugin).
static void pinThisModule()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			   reinterpret_cast<LPCWSTR>(&pinThisModule), &self);
}
#endif

// A tool that is switched off (or failed to load) is skipped; unload only touches the tools that loaded.
static bool g_msLoaded = false;
static bool g_pfLoaded = false;
static bool g_ifLoaded = false;
static QPointer<KitDock> g_kitDock; // UI thread only

// The Kit dock's own frontend callback: first-run docking, and the ORDERED EXIT shutdown. OBS calls callbacks in
// reverse registration order and this one is registered last, so it is the first to see EXIT (see the order above):
// it shuts the tools down itself, Multistream first (its websocket gate closes before anything else stops),
// and the tools' own callbacks, called afterwards, find nothing left to do.
static void kitFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		if (g_msLoaded)
			multistream_module::frontend_event(event, nullptr);
		if (g_pfLoaded)
			preflight_module::frontend_event(event, nullptr);
		if (g_ifLoaded)
			inflight_module::frontend_event(event, nullptr);
	}
	if (g_kitDock)
		g_kitDock->handleFrontendEvent(event);
}

bool obs_module_load(void)
{
#ifdef _WIN32
	pinThisModule();
#endif
	// Config folders: every tool keeps its pre-merge plugin_config/<tool>/ folder (see kit-paths.hpp).
	kitpaths::installObsProvider();

	const kitsettings::Settings settings =
		kitsettings::load(QString::fromUtf8(kitpaths::pluginConfigRoot().c_str()));
	if (settings.multistream)
		g_msLoaded = multistream_module::load();
	else
		obs_log(LOG_INFO, "Multistream is switched off in the Kit settings: not loaded");
	if (settings.preflight)
		g_pfLoaded = preflight_module::load();
	else
		obs_log(LOG_INFO, "Pre-Flight is switched off in the Kit settings: not loaded");
	if (settings.inflight)
		g_ifLoaded = inflight_module::load();
	else
		obs_log(LOG_INFO, "In-Flight is switched off in the Kit settings: not loaded");

	// The one dock, registered once at load so OBS restores its saved place. OBS creates a new dock hidden and
	// floating; KitDock docks and shows it on the very first run only (at FINISHED_LOADING).
	KitDock::Panels panels;
	panels.multistream = g_msLoaded ? multistream_module::panel() : nullptr;
	panels.preflight = g_pfLoaded ? preflight_module::panel() : nullptr;
	panels.inflight = g_ifLoaded ? inflight_module::panel() : nullptr;
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	g_kitDock = new KitDock(panels, settings, main);
	g_kitDock->hide();
	if (!obs_frontend_add_dock_by_id(KitDock::kDockId, KitDock::kTitle, g_kitDock.data())) {
		obs_log(LOG_ERROR, "could not register the dock");
		// The panels are children of the dock now: deleting it deletes them, so their callbacks must go
		// first (a callback with a dead panel would be harmless, but the tools say they unloaded).
		delete g_kitDock.data();
		if (g_ifLoaded)
			inflight_module::unload();
		if (g_pfLoaded)
			preflight_module::unload();
		if (g_msLoaded)
			multistream_module::unload();
		g_ifLoaded = g_pfLoaded = g_msLoaded = false;
		return false;
	}
	g_kitDock->prepareFirstRun();
	obs_frontend_add_event_callback(kitFrontendEvent, nullptr);
	obs_log(LOG_INFO, "dock registered (id %s)", KitDock::kDockId);
	obs_log(LOG_INFO, "Red Warden Stream Kit loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	// Multistream registers its obs-websocket vendor here: every module (obs-websocket among them)
	// has loaded by now.
	if (g_msLoaded)
		multistream_module::post_load();
	if (g_pfLoaded)
		preflight_module::post_load();
	if (g_ifLoaded)
		inflight_module::post_load();
}

void obs_module_unload(void)
{
	// Reverse of load. (Every tool's EXIT shutdown already ran at OBS_FRONTEND_EVENT_EXIT; the dock and the
	// panels inside it are deleted by Qt with OBS's main window.)
	obs_frontend_remove_event_callback(kitFrontendEvent, nullptr);
	if (g_ifLoaded)
		inflight_module::unload();
	if (g_pfLoaded)
		preflight_module::unload();
	if (g_msLoaded)
		multistream_module::unload();
	g_ifLoaded = g_pfLoaded = g_msLoaded = false;
	obs_log(LOG_INFO, "Red Warden Stream Kit unloaded");
}
