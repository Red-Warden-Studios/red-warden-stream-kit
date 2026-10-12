/*
Red Warden Multistream
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

#include "multistream-module.hpp"

#include "multistream-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include "obs-websocket-api.h"

#include <QMainWindow>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QPointer>

namespace multistream_module {

static QPointer<MultistreamDock> g_dock; // UI thread only
static std::atomic<bool> g_exiting{false};
// What obs-websocket's thread may use to reach the dock (see onUiThread below).
static std::mutex g_reqMutex;
static MultistreamDock *g_dockForRequests = nullptr;

static obs_websocket_vendor g_vendor = nullptr;
static const char *kVendorName = "RedWardenMultistream";
static const char *const kVendorRequests[] = {"GetDestinations", "SetDestination", "ToggleDestination",
					      "SetAllDestinations"};

static void unregisterVendor()
{
	if (!g_vendor)
		return;
	for (const char *r : kVendorRequests)
		obs_websocket_vendor_unregister_request(g_vendor, r);
	g_vendor = nullptr;
}

void frontend_event(enum obs_frontend_event event, void *)
{
	if (!g_dock)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		g_dock->onStreamingStarted();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		g_dock->onStreamingStopping();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		g_dock->onProfileChanging();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		g_dock->onProfileChanged();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// Release every output before libobs begins shutting down, and take our
		// request handlers out of obs-websocket while both modules are still
		// loaded (by obs_module_unload, obs-websocket may already be gone).
		{
			// After this, no websocket request can post to the dock.
			std::lock_guard<std::mutex> lock(g_reqMutex);
			g_exiting = true;
			g_dockForRequests = nullptr;
		}
		unregisterVendor();
		g_dock->stateListener = nullptr;
		g_dock->shutdown();
		break;
	default:
		break;
	}
}

MultistreamDock *panel()
{
	return g_dock.data();
}

bool load()
{
	// The widget is created hidden and parented to the main window; the Kit dock takes it over (it is
	// the only dock; this tool no longer registers one of its own).
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	g_dock = new MultistreamDock(main);
	g_dock->hide();
	{
		std::lock_guard<std::mutex> lock(g_reqMutex);
		g_dockForRequests = g_dock.data();
	}
	obs_frontend_add_event_callback(frontend_event, nullptr);
	obs_log(LOG_INFO, "Red Warden Multistream loaded (version %s)", PLUGIN_VERSION);
	obs_log(LOG_INFO, "panel created (shown in the Stream Kit dock)");
	return true;
}

// ---- obs-websocket vendor API ----------------------------------------------
// Lets Streamer.bot, Touch Portal, SAMMI, scripts and Stream Deck plugins list
// destinations and switch them on/off:
//   CallVendorRequest { vendorName: "RedWardenMultistream",
//                       requestType: "GetDestinations" | "SetDestination" |
//                                    "ToggleDestination" | "SetAllDestinations",
//                       requestData: { destination: "<id or name>", enabled: true } }
// and emits a "DestinationChanged" vendor event on every state change.

static void fill(obs_data_t *o, const MultistreamDock::DestinationInfo &i)
{
	obs_data_set_string(o, "id", i.id.toUtf8().constData());
	obs_data_set_string(o, "name", i.name.toUtf8().constData());
	obs_data_set_string(o, "platform", i.platform.toUtf8().constData());
	obs_data_set_bool(o, "enabled", i.enabled);
	obs_data_set_string(o, "state", i.state.toUtf8().constData());
	obs_data_set_double(o, "kbps", i.kbps);
}

// Requests arrive on obs-websocket's thread; the dock lives on the UI thread.
//
// The websocket thread never touches g_dock (a QPointer is not thread-safe). It
// reads g_dockForRequests under g_reqMutex; the UI thread sets that at load and
// clears it at OBS_FRONTEND_EVENT_EXIT under the same lock, so no request can be
// posted to the dock once exit has begun. The work is POSTED, never blocked on
// (the UI thread may be busy exiting), and its result comes back by value
// through a promise, so a late-running call never writes into a returned stack
// frame. A request that gets no answer in 3 s (OBS exiting) reports ok=false.

// Every request handler holds one of these for its whole run, so
// obs_module_unload can wait (briefly) until no websocket thread is inside
// this DLL. obs-websocket calls a handler it copied before we unregistered it,
// so unregistering alone can't guarantee that.
static std::atomic<int> g_inFlight{0};
struct InFlight {
	InFlight() { ++g_inFlight; }
	~InFlight() { --g_inFlight; }
};

// nullopt: OBS is exiting (nothing was or will be done), or the UI thread did
// not answer in 3 s (*timedOut set: the change may still apply).
template<typename R, typename F> static std::optional<R> onUiThread(F fn, bool *timedOut = nullptr)
{
	auto done = std::make_shared<std::promise<R>>();
	std::future<R> result = done->get_future();
	{
		std::lock_guard<std::mutex> lock(g_reqMutex);
		MultistreamDock *dock = g_dockForRequests;
		if (!dock || g_exiting)
			return std::nullopt;
		// The posted call counts as in flight until Qt has run OR discarded it
		// (the token dies with the functor), so unload waits for it too.
		auto token = std::make_shared<InFlight>();
		QMetaObject::invokeMethod(
			dock,
			[done, fn, dock, token] {
				if (g_exiting)
					return; // queued just before exit: skip it (the waiter gives up)
				try {
					done->set_value(fn(dock));
				} catch (...) {
					try {
						done->set_exception(std::current_exception());
					} catch (...) {
					}
				}
			},
			Qt::QueuedConnection);
	}
	// Wait in short slices so a request in flight at exit lets go at once
	// instead of holding this DLL busy for the whole timeout.
	for (int waited = 0; result.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready;
	     waited += 25) {
		if (g_exiting)
			return std::nullopt;
		if (waited >= 3000) {
			if (timedOut)
				*timedOut = true;
			return std::nullopt;
		}
	}
	try {
		return result.get();
	} catch (...) { // fn threw, or the dock went away before running it
		return std::nullopt;
	}
}

static const char *noAnswer(bool timedOut)
{
	if (timedOut)
		return "OBS did not answer in time; the change may still apply";
	return g_exiting ? "OBS is shutting down" : "The request failed";
}

static void req_get(obs_data_t *, obs_data_t *res, void *)
{
	InFlight guard;
	bool timedOut = false;
	const auto list = onUiThread<std::vector<MultistreamDock::DestinationInfo>>(
		[](MultistreamDock *d) { return d->snapshot(); }, &timedOut);
	obs_data_array_t *arr = obs_data_array_create();
	if (list) {
		for (const auto &i : *list) {
			obs_data_t *o = obs_data_create();
			fill(o, i);
			obs_data_array_push_back(arr, o);
			obs_data_release(o);
		}
	}
	obs_data_set_array(res, "destinations", arr);
	obs_data_array_release(arr);
	obs_data_set_bool(res, "ok", list.has_value());
	if (!list)
		obs_data_set_string(res, "error", noAnswer(timedOut));
}

static void set_common(obs_data_t *req, obs_data_t *res, int mode)
{
	const QString key = QString::fromUtf8(obs_data_get_string(req, "destination"));
	if (mode == 1 && !obs_data_get_bool(req, "enabled"))
		mode = 0;
	if (key.isEmpty()) {
		obs_data_set_bool(res, "ok", false);
		obs_data_set_string(res, "error", "requestData.destination (id or name) is required");
		return;
	}
	struct Found {
		bool found = false;
		MultistreamDock::DestinationInfo info;
	};
	bool timedOut = false;
	const auto r = onUiThread<Found>(
		[key, mode](MultistreamDock *d) {
			Found f;
			f.found = d->setEnabledByKey(key, mode, &f.info);
			return f;
		},
		&timedOut);
	const bool found = r && r->found;
	obs_data_set_bool(res, "ok", found);
	if (!found) {
		obs_data_set_string(res, "error", r ? "no destination with that id or name" : noAnswer(timedOut));
		return;
	}
	fill(res, r->info);
}

static void req_set(obs_data_t *req, obs_data_t *res, void *)
{
	InFlight guard;
	set_common(req, res, 1);
}

static void req_toggle(obs_data_t *req, obs_data_t *res, void *)
{
	InFlight guard;
	set_common(req, res, 2);
}

static void req_set_all(obs_data_t *req, obs_data_t *res, void *)
{
	InFlight guard;
	const bool on = obs_data_get_bool(req, "enabled");
	bool timedOut = false;
	const auto ok = onUiThread<bool>(
		[on](MultistreamDock *d) {
			d->setAll(on);
			return true;
		},
		&timedOut);
	obs_data_set_bool(res, "ok", ok.has_value());
	if (!ok)
		obs_data_set_string(res, "error", noAnswer(timedOut));
}
void post_load()
{
	g_vendor = obs_websocket_register_vendor(kVendorName);
	if (!g_vendor) {
		obs_log(LOG_INFO, "obs-websocket not available; remote control disabled");
		return;
	}
	obs_websocket_vendor_register_request(g_vendor, "GetDestinations", req_get, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "SetDestination", req_set, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "ToggleDestination", req_toggle, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "SetAllDestinations", req_set_all, nullptr);
	if (g_dock)
		g_dock->stateListener = [](const MultistreamDock::DestinationInfo &i) {
			if (!g_vendor)
				return;
			obs_data_t *ev = obs_data_create();
			fill(ev, i);
			obs_websocket_vendor_emit_event(g_vendor, "DestinationChanged", ev);
			obs_data_release(ev);
		};
	obs_log(LOG_INFO, "obs-websocket vendor \"%s\" registered", kVendorName);
}

void unload()
{
	// Normally everything was already released at OBS_FRONTEND_EVENT_EXIT. Make
	// sure anyway that no websocket request can reach the dock, then give any
	// handler still running inside this DLL a moment to leave (they bail out
	// within ~25 ms once g_exiting is set) before libobs tears down further.
	{
		std::lock_guard<std::mutex> lock(g_reqMutex);
		g_exiting = true;
		g_dockForRequests = nullptr;
	}
	// Calls still queued for the dock (posted just before exit) run now, on this
	// (UI) thread: ours see g_exiting and do nothing, and their tokens release.
	// This dispatches every pending MetaCall for the dock object only (they are
	// all ours: nothing else posts to our dock). The DLL is pinned (see
	// pinThisModule), so a straggler past the wait below still runs valid code.
	if (g_dock)
		QCoreApplication::sendPostedEvents(g_dock.data(), QEvent::MetaCall);
	for (int i = 0; i < 200 && g_inFlight.load() > 0; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (g_inFlight.load() > 0)
		obs_log(LOG_WARNING, "unloading with %d websocket request(s) still in flight", g_inFlight.load());
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	obs_log(LOG_INFO, "Red Warden Multistream unloaded");
}

} // namespace multistream_module
