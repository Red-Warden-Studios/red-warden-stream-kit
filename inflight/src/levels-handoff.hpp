/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The Levels panel's audio-thread to UI-thread hand-off. Unlike LevelAccumulator (which keeps the MAXIMUM
// of the magnitude over an interval, right for a watchdog) this keeps the linear POWER of every callback's
// magnitude as a running sum plus a callback count, so the UI can compute a true mean over the interval
// (a loud blip among quiet callbacks must not read as the average). The peak is still a max. Pure: std
// only (plus the helpers in level-accumulator.hpp), no libobs, no Qt, so the unit test builds without OBS.
// The callback side never allocates, never locks and never logs.
#pragma once

#include "level-accumulator.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace inflight {

class LevelsHandoff {
public:
	struct Reading {
		std::int64_t lastCallbackMs = 0; // newest callback since attach; 0 = none
		bool fresh = false;              // at least one callback landed since the previous take()
		float peakDb = -std::numeric_limits<float>::infinity(); // max peak of the window; meaningful when fresh
		float avgDb = -std::numeric_limits<float>::infinity();  // mean-power level of the window; meaningful when fresh
	};

	void setChannels(int n) { channels_.store(LevelAccumulator::clampChannels(n), std::memory_order_relaxed); }
	int channels() const { return channels_.load(std::memory_order_relaxed); }

	// Audio thread. Lock-free. peakMax / magMax are dBFS maxima over the channel slots (LevelAccumulator::maxDb).
	// NaN in either (no valid value) ignores the whole callback and touches nothing, not even lastMs.
	// Digital silence (-inf magnitude) adds 0.0 power but still counts as a callback.
	void onCallback(float peakMax, float magMax, std::int64_t atMs)
	{
		if (std::isnan(peakMax) || std::isnan(magMax))
			return;
		lastMs_.store(atMs, std::memory_order_relaxed);
		float p = peak_.load(std::memory_order_relaxed);
		while (peakMax > p && !peak_.compare_exchange_weak(p, peakMax, std::memory_order_relaxed)) {
		}
		const double power = (std::isinf(magMax) && magMax < 0) ? 0.0 : std::pow(10.0, static_cast<double>(magMax) / 10.0);
		double s = sum_.load(std::memory_order_relaxed);
		while (!sum_.compare_exchange_weak(s, s + power, std::memory_order_relaxed)) {
		}
		count_.fetch_add(1, std::memory_order_release);
	}

	// UI thread. Takes the window accumulated since the previous take(): peak max, mean power as dB, and
	// whether anything arrived. The count, the power sum and the peak are three separate exchanges, so a
	// callback that lands between them can move into the next window (one callback out of the roughly
	// 10 per 200 ms window); for a display this is harmless and the alternative is a lock on the audio thread.
	Reading take()
	{
		const std::uint32_t c = count_.exchange(0, std::memory_order_acquire);
		const double s = sum_.exchange(0.0, std::memory_order_relaxed);
		const float p = peak_.exchange(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
		Reading r;
		r.lastCallbackMs = lastMs_.load(std::memory_order_relaxed);
		if (c == 0)
			return r; // no reading this window: fresh=false, values stay -inf and must not be used
		r.fresh = true;
		r.peakDb = p;
		const double mean = s / static_cast<double>(c);
		r.avgDb = mean > 0.0 ? static_cast<float>(10.0 * std::log10(mean)) : -std::numeric_limits<float>::infinity();
		return r;
	}

	// UI thread, only while no callback can run (before attach / after remove_callback).
	void reset()
	{
		count_.store(0, std::memory_order_relaxed);
		sum_.store(0.0, std::memory_order_relaxed);
		peak_.store(-std::numeric_limits<float>::infinity(), std::memory_order_relaxed);
		lastMs_.store(0, std::memory_order_relaxed);
		channels_.store(LevelAccumulator::kMaxChannels, std::memory_order_relaxed);
	}

private:
	std::atomic<std::uint32_t> count_{0};
	std::atomic<double> sum_{0.0};
	std::atomic<float> peak_{-std::numeric_limits<float>::infinity()};
	std::atomic<std::int64_t> lastMs_{0};
	std::atomic<int> channels_{LevelAccumulator::kMaxChannels};
};

} // namespace inflight
