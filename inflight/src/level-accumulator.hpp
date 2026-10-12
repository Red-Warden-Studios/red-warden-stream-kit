/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Hands level numbers from the audio thread (volmeter callback) to the 1 Hz UI tick. Pure: std only,
// no libobs, no Qt, so the unit test builds without OBS. The callback side never allocates, never
// locks and never logs.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace inflight {

class LevelAccumulator {
public:
	struct Reading {
		std::int64_t lastCallbackMs = 0; // newest callback since attach; 0 = none
		float peakDb = -std::numeric_limits<float>::infinity();
		float avgDb = -std::numeric_limits<float>::infinity();
		bool fresh = false; // false = no callback since the previous take(); peak/avg repeat the old ones
	};

	LevelAccumulator() { reset(); }

	// libobs' MAX_AUDIO_CHANNELS (audio-watch.cpp static_asserts they agree).
	static constexpr int kMaxChannels = 8;

	// Max over the first n values of dBFS. NaN is skipped, -inf is kept (digital silence is a real,
	// measured value), values above 0 clamp to 0. Returns NaN when there is NO valid value at all (every
	// value NaN, or n <= 0): "no reading" is deliberately a different answer from -inf "silence".
	static float maxDb(const float v[], int n)
	{
		float best = std::numeric_limits<float>::quiet_NaN();
		for (int i = 0; i < n; ++i) {
			float x = v[i];
			if (std::isnan(x))
				continue;
			if (x > 0.0f)
				x = 0.0f;
			if (std::isnan(best) || x > best)
				best = x;
		}
		return best;
	}

	// How many channel slots to read: the meter's current channel count, clamped to 1..kMaxChannels.
	// 0 or negative (unknown) reads all kMaxChannels slots.
	static int clampChannels(int n)
	{
		if (n <= 0)
			return kMaxChannels;
		return n > kMaxChannels ? kMaxChannels : n;
	}

	// UI thread (each tick, from obs_volmeter_get_nr_channels). Audio thread reads it (relaxed): libobs
	// clears the unused PEAK slots but not the unused MAGNITUDE slots, so after a stereo -> mono change
	// slot 1 of the magnitude array can hold a stale loud value. A change shows up with up to one tick of lag.
	void setChannels(int n) { channels_.store(clampChannels(n), std::memory_order_relaxed); }
	int channels() const { return channels_.load(std::memory_order_relaxed); }

	// Audio thread. Lock-free. Both maxima live in ONE 64-bit atomic word (peak in the high half, magnitude
	// in the low half), and an all-ones word means "no callback since the last take()". Because the
	// maxima and the "a callback happened" signal are one atomic value, take() can never see one without
	// the other: a callback that lands while take() runs is either wholly in this reading or wholly in the
	// next one. (A separate counter would split: a callback whose maxima were taken but whose count was
	// not would make the next tick report a fresh -inf, i.e. fake digital silence.)
	void onCallback(float peakMax, float magMax, std::int64_t atMs)
	{
		// NaN (from maxDb) = no valid reading. Touch nothing, not even lastMs: a source that only ever
		// emits NaN must age into "no callback" (Unknown), never into -inf "digital silence". If only one of
		// the two is NaN the callback is broken as a whole, so it is ignored as a whole.
		if (std::isnan(peakMax) || std::isnan(magMax))
			return;
		lastMs_.store(atMs, std::memory_order_relaxed);
		std::uint64_t cur = word_.load(std::memory_order_relaxed);
		for (;;) {
			std::uint64_t next;
			if (cur == kEmpty) {
				next = pack(peakMax, magMax);
			} else {
				float p = hi(cur), m = lo(cur);
				if (peakMax > p)
					p = peakMax;
				if (magMax > m)
					m = magMax;
				next = pack(p, m);
			}
			if (word_.compare_exchange_weak(cur, next, std::memory_order_release, std::memory_order_relaxed))
				return;
		}
	}

	// UI thread. Takes everything accumulated since the previous take(). No callback in between:
	// fresh=false and the previous peak/avg, so a tick that falls between callbacks never reads as silence.
	Reading take()
	{
		const std::uint64_t w = word_.exchange(kEmpty, std::memory_order_acquire);
		if (w != kEmpty) {
			prevPeak_ = hi(w);
			prevAvg_ = lo(w);
			haveAny_ = true;
		}
		Reading r;
		r.lastCallbackMs = lastMs_.load(std::memory_order_relaxed);
		r.peakDb = prevPeak_;
		r.avgDb = prevAvg_;
		r.fresh = (w != kEmpty);
		return r;
	}

	// UI thread, only while no callback can run (before attach / after remove_callback).
	void reset()
	{
		word_.store(kEmpty, std::memory_order_relaxed);
		lastMs_.store(0, std::memory_order_relaxed);
		channels_.store(kMaxChannels, std::memory_order_relaxed);
		prevPeak_ = prevAvg_ = -std::numeric_limits<float>::infinity();
		haveAny_ = false;
	}

	bool everHadCallback() const { return haveAny_; }

private:
	static constexpr std::uint64_t kEmpty = ~std::uint64_t(0);

	static std::uint64_t pack(float peak, float mag)
	{
		std::uint32_t a, b;
		std::memcpy(&a, &peak, sizeof a);
		std::memcpy(&b, &mag, sizeof b);
		return (std::uint64_t(a) << 32) | b;
	}
	static float hi(std::uint64_t w)
	{
		const std::uint32_t a = std::uint32_t(w >> 32);
		float f;
		std::memcpy(&f, &a, sizeof f);
		return f;
	}
	static float lo(std::uint64_t w)
	{
		const std::uint32_t b = std::uint32_t(w & 0xFFFFFFFFu);
		float f;
		std::memcpy(&f, &b, sizeof f);
		return f;
	}

	std::atomic<std::uint64_t> word_{~std::uint64_t(0)};
	std::atomic<std::int64_t> lastMs_{0};
	std::atomic<int> channels_{kMaxChannels};
	// UI thread only.
	float prevPeak_ = -std::numeric_limits<float>::infinity();
	float prevAvg_ = -std::numeric_limits<float>::infinity();
	bool haveAny_ = false;
};

// Bounded patience for the "live state unknown" case: pending = true on every tick that is Unknown;
// expired() is true once pending has held continuously for limitMs. Any tick with pending = false
// (Live, Off) resets it. UI thread only.
class GraceTimer {
public:
	bool expired(bool pending, std::int64_t nowMs, std::int64_t limitMs)
	{
		if (!pending) {
			active_ = false;
			return false;
		}
		if (!active_) {
			active_ = true;
			since_ = nowMs;
		}
		return nowMs - since_ >= limitMs;
	}
	void reset() { active_ = false; }

private:
	bool active_ = false;
	std::int64_t since_ = 0;
};

} // namespace inflight
