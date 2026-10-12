/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "loudness-worker.hpp"
#include "kit-cost.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace inflight {

namespace {
constexpr size_t kScratchFrames = 4096;

std::int64_t steadyNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}
} // namespace

bool LoudnessWorker::start(double sampleRate, const ChannelRole *roles, int channels, int slotCount, bool truePeak)
{
	stop();
	if (!roles || channels < 1 || channels > kLoudnessMaxChannels || slotCount < 0 || slotCount > 64 ||
	    !(sampleRate >= 8000.0 && sampleRate <= 384000.0))
		return false;
	std::vector<std::unique_ptr<Slot>> made;
	for (int i = 0; i < slotCount; i++) {
		auto s = std::make_unique<Slot>();
		if (!s->ring.init(channels, static_cast<size_t>(sampleRate)) ||
		    !s->meter.configure(sampleRate, roles, channels, kTickMs) || !s->tp.configure(sampleRate, channels, 100))
			return false;
		for (double &d : s->tpBlocks)
			d = loudnessMinusInf();
		made.push_back(std::move(s));
	}
	rate_ = sampleRate;
	channels_ = channels;
	slots_ = std::move(made);
	scratch_.assign(static_cast<size_t>(channels), std::vector<float>(kScratchFrames, 0.0f));
	scratchPtr_.assign(static_cast<size_t>(channels), nullptr);
	scratchCPtr_.assign(static_cast<size_t>(channels), nullptr);
	for (size_t c = 0; c < scratch_.size(); c++) {
		scratchPtr_[c] = scratch_[c].data();
		scratchCPtr_[c] = scratch_[c].data();
	}
	tpWanted_.store(truePeak, std::memory_order_relaxed);
	tpActive_ = false;
	costSumUs_ = costMaxUs_ = 0.0;
	costN_ = 0;
	{
		std::lock_guard<std::mutex> lk(wakeMutex_);
		stopFlag_ = false;
		abort_.store(false, std::memory_order_relaxed);
	}
	running_ = true;
	thread_ = std::thread([this] { loop(); });
	return true;
}

void LoudnessWorker::stop()
{
	if (thread_.joinable()) {
		{
			std::lock_guard<std::mutex> lk(wakeMutex_);
			stopFlag_ = true;
			abort_.store(true, std::memory_order_relaxed); // the worker abandons queued audio at once
		}
		wake_.notify_all();
		thread_.join();
	}
	running_ = false;
	slots_.clear();
}

SpscFrameRing *LoudnessWorker::ring(int slot)
{
	if (slot < 0 || slot >= static_cast<int>(slots_.size()))
		return nullptr;
	return &slots_[static_cast<size_t>(slot)]->ring;
}

LufsResult LoudnessWorker::result(int slot) const
{
	LufsResult r;
	if (slot < 0 || slot >= static_cast<int>(slots_.size()))
		return r;
	const Slot &s = *slots_[static_cast<size_t>(slot)];
	{
		std::lock_guard<std::mutex> lk(mutex_);
		r = s.out;
	}
	r.droppedBlocks = s.ring.droppedBlocks();
	r.droppedFrames = s.ring.droppedFrames();
	return r;
}

LoudnessWorker::Cost LoudnessWorker::cost() const
{
	std::lock_guard<std::mutex> lk(mutex_);
	Cost c;
	c.iterations = costN_;
	c.meanUs = costN_ ? costSumUs_ / static_cast<double>(costN_) : 0.0;
	c.maxUs = costMaxUs_;
	return c;
}

void LoudnessWorker::loop()
{
	std::unique_lock<std::mutex> lk(wakeMutex_);
	while (!stopFlag_) {
		wake_.wait_for(lk, std::chrono::milliseconds(kTickMs), [this] { return stopFlag_; });
		if (stopFlag_)
			break;
		lk.unlock();
		kitcost::Scope costScope(kitcost::LufsWorker);
		const std::int64_t t0 = steadyNs();
		const bool tpOn = tpWanted_.load(std::memory_order_relaxed);
		if (tpOn != tpActive_) {
			tpActive_ = tpOn;
			for (auto &s : slots_) {
				s->tp.reset();
				s->tpFed = false;
				s->tpBlocksSeen = 0;
				s->tpCount = 0;
				s->tpHead = 0;
			}
		}
		for (auto &s : slots_) {
			if (abort_.load(std::memory_order_relaxed))
				break;
			tickSlot(*s, t0, tpOn);
		}
		const double us = static_cast<double>(steadyNs() - t0) / 1000.0;
		{
			std::lock_guard<std::mutex> g(mutex_);
			costSumUs_ += us;
			costMaxUs_ = std::max(costMaxUs_, us);
			costN_++;
		}
		lk.lock();
	}
}

void LoudnessWorker::tickSlot(Slot &s, std::int64_t nowNs, bool tpOn)
{
	const bool incoming = s.ring.writtenFrames() > s.ring.readFrames();
	if (incoming && s.stale && s.lastDataNs != 0) {
		// Audio is back after a gap: the old window no longer describes the signal.
		s.meter.reset();
		s.tp.reset();
		s.tpFed = false;
		s.tpBlocksSeen = 0;
		s.tpCount = 0;
		s.tpHead = 0;
	}
	// A ring overflow (the audio thread had to drop whole blocks) breaks the signal's continuity: the windows no
	// longer describe it. Discard what is queued, reset this source's meters (readings go back to "--" until the
	// windows refill) and keep counting drops (the ring's own counters, never reset).
	const std::uint64_t dropped = s.ring.droppedBlocks();
	if (dropped != s.lastDropped) {
		s.lastDropped = dropped;
		while (!abort_.load(std::memory_order_relaxed) && s.ring.read(scratchPtr_.data(), kScratchFrames) > 0) {
		}
		s.meter.reset();
		s.tp.reset();
		s.tpFed = false;
		s.tpCount = 0;
		s.tpHead = 0;
	}
	for (;;) {
		// Stop requested: abandon the queued audio, the caller is about to join.
		if (abort_.load(std::memory_order_relaxed))
			return;
		const size_t n = s.ring.read(scratchPtr_.data(), kScratchFrames);
		if (n == 0)
			break;
		s.meter.feed(scratchCPtr_.data(), static_cast<int>(n));
		if (tpOn) {
			s.tp.feed(scratchCPtr_.data(), static_cast<int>(n));
			s.tpFed = true;
			// Record after EVERY feed: each completed 100 ms block (its maximum, not just the newest block's)
			// must reach the 1.5 s window, whatever the chunking.
			double mx = loudnessMinusInf();
			long long blocks = s.tp.takeBlocks(&mx);
			if (blocks > kTpWindowBlocks)
				blocks = kTpWindowBlocks;
			for (long long b = 0; b < blocks; b++) {
				s.tpBlocks[s.tpHead] = mx;
				s.tpHead = (s.tpHead + 1) % kTpWindowBlocks;
				s.tpCount = std::min(s.tpCount + 1, kTpWindowBlocks);
			}
		}
	}
	if (incoming) {
		s.stale = false;
		s.lastDataNs = nowNs;
	} else if (!s.stale && nowNs - s.lastDataNs > static_cast<std::int64_t>(kStaleMs) * 1000000) {
		s.stale = true;
	}
	LufsResult r;
	r.active = !s.stale && s.lastDataNs != 0;
	const LoudnessReading m = s.meter.momentary();
	const LoudnessReading st = s.meter.shortTerm();
	r.momentaryReady = m.ready;
	r.momentary = m.lufs;
	r.shortReady = st.ready;
	r.shortTerm = st.lufs;
	if (tpOn && s.tpCount > 0) {
		r.tpValid = true;
		double mx = loudnessMinusInf();
		for (int i = 0; i < s.tpCount; i++)
			mx = std::max(mx, s.tpBlocks[i]);
		r.tpMax = mx;
	}
	std::lock_guard<std::mutex> g(mutex_);
	s.out = r;
}

} // namespace inflight
