/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Test-only CPU cost counters (RWS_KIT_TEST_COST=1, testbed OBS only; the Kit dock switches them on, see kit-dock.cpp).
// Each hook site wraps its work in a kitcost::Scope. Off (the normal case) a scope costs one relaxed load of a bool and
// a branch: no clock read, no write. On, it adds wall time and one call to lock-free atomics, so it is safe on OBS's
// audio thread (no lock, no allocation). The Kit dock logs "kit cost: <name> calls= total_us= max_us=" every 60 s.
// Header-only and free of Qt and libobs, so the unit-test targets that compile tool sources can include it.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace kitcost {

enum Slot : int {
	InflightTick,      // In-Flight 1 Hz controller tick (UI thread)
	PreflightTick,     // Pre-Flight controller tick (UI thread)
	MultistreamStats,  // Multistream dock 1 Hz refresh (UI thread)
	KitTick,           // Kit dock 1 Hz tick (UI thread)
	LightsFlash,       // In-Flight lights panel flash timer (UI thread)
	LevelsFast,        // Levels panel fast timer (UI thread, only while the panel is open)
	LevelsText,        // Levels panel text timer (UI thread)
	LevelsSlow,        // Levels panel 1 Hz recheck (UI thread)
	VolmeterWatch,     // AudioWatch volmeter callback (audio thread)
	VolmeterLevels,    // Levels panel volmeter callback (audio thread)
	LufsCapture,       // Levels LUFS audio capture callback (audio thread)
	LufsWorker,        // Loudness worker thread, one pass (its own thread)
	SlotCount
};

inline const char *slotName(int s)
{
	static const char *const names[SlotCount] = {"inflight-tick",   "preflight-tick", "multistream-stats", "kit-tick",
						     "lights-flash",    "levels-fast",    "levels-text",       "levels-slow",
						     "volmeter-watch",  "volmeter-levels", "lufs-capture",     "lufs-worker"};
	return s >= 0 && s < SlotCount ? names[s] : "?";
}

struct Counter {
	std::atomic<std::uint64_t> calls{0};
	std::atomic<std::uint64_t> totalNs{0};
	std::atomic<std::uint64_t> maxNs{0};
};

inline std::atomic<bool> &onFlag()
{
	static std::atomic<bool> on{false};
	return on;
}
inline Counter *counters()
{
	static Counter c[SlotCount];
	return c;
}
inline void setEnabled(bool on)
{
	onFlag().store(on, std::memory_order_relaxed);
}
inline bool enabled()
{
	return onFlag().load(std::memory_order_relaxed);
}

inline void add(int slot, std::uint64_t ns)
{
	Counter &c = counters()[slot];
	c.calls.fetch_add(1, std::memory_order_relaxed);
	c.totalNs.fetch_add(ns, std::memory_order_relaxed);
	std::uint64_t m = c.maxNs.load(std::memory_order_relaxed);
	while (ns > m && !c.maxNs.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {
	}
}

class Scope {
public:
	explicit Scope(int slot) : slot_(slot), on_(enabled())
	{
		if (on_)
			t0_ = std::chrono::steady_clock::now();
	}
	~Scope()
	{
		if (on_)
			add(slot_, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
									std::chrono::steady_clock::now() - t0_)
									.count()));
	}
	Scope(const Scope &) = delete;
	Scope &operator=(const Scope &) = delete;

private:
	int slot_;
	bool on_;
	std::chrono::steady_clock::time_point t0_{};
};

} // namespace kitcost
