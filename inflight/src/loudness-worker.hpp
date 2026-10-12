/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The LUFS worker: ONE thread for all the sources of the Levels panel's LUFS mode. Each source has an
// SpscFrameRing (written by OBS's audio thread, see levels-lufs.hpp) and a LoudnessMeter (hop 10 ms) plus,
// when true peak is on, a TruePeakMeter. Every ~10 ms the thread drains the rings, feeds the meters and
// publishes the numbers; the UI reads them at 5-20 Hz. Pure: std + loudness.hpp, no libobs, no Qt, so the
// unit test builds without OBS. The worker touches nothing from libobs.
//
// Ownership / order (the caller's job, levels-lufs.cpp): stop writing into ring(i) (remove the capture
// callbacks) BEFORE stop(); stop() joins the thread and frees the rings.
#pragma once

#include "loudness.hpp"
#include "spsc-ring.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace inflight {

struct LufsResult {
	bool active = false;       // data arrived within the stale limit (a source that is not delivering audio is not silent)
	bool momentaryReady = false;
	bool shortReady = false;
	double momentary = loudnessMinusInf(); // LUFS, -inf = silence
	double shortTerm = loudnessMinusInf();
	bool tpValid = false;      // true peak is on and at least one 100 ms block finished
	double tpMax = loudnessMinusInf(); // dBTP, max over the last ~1.5 s
	std::uint64_t droppedBlocks = 0;   // ring overflows (blocks the audio thread could not store)
	std::uint64_t droppedFrames = 0;
};

class LoudnessWorker {
public:
	static constexpr int kTickMs = 10;        // worker period
	static constexpr int kStaleMs = 1500;     // no audio for this long: the source reads "not active"
	static constexpr int kTpWindowBlocks = 15; // 15 x 100 ms = 1.5 s true-peak window

	LoudnessWorker() = default;
	~LoudnessWorker() { stop(); }
	LoudnessWorker(const LoudnessWorker &) = delete;
	LoudnessWorker &operator=(const LoudnessWorker &) = delete;

	// Allocates everything (rings of ~1 s, meters, scratch) and starts the thread. `slotCount` sources, `channels`
	// planar channels per ring (the OBS output's), `roles` one per channel. Returns false on bad arguments.
	bool start(double sampleRate, const ChannelRole *roles, int channels, int slotCount, bool truePeak);
	// Asks the thread to stop: it abandons any queued audio (checked between reads and between slots, so at most
	// one chunk of 4096 frames is still being processed), then joins and frees the rings. Idempotent.
	void stop();
	bool running() const { return running_; }

	double sampleRate() const { return rate_; }
	int channels() const { return channels_; }
	int numSlots() const { return static_cast<int>(slots_.size()); }
	// The ring of a slot, for the producer. Valid from start() until stop().
	SpscFrameRing *ring(int slot);

	// Any thread. Turning it on starts from a clean true-peak meter.
	void setTruePeak(bool on) { tpWanted_.store(on, std::memory_order_relaxed); }
	bool truePeak() const { return tpWanted_.load(std::memory_order_relaxed); }

	// UI thread: the latest published numbers of a slot (a copy taken under a short lock).
	LufsResult result(int slot) const;
	// Cost of one worker iteration (drain + meters + publish), since start().
	struct Cost {
		double meanUs = 0.0;
		double maxUs = 0.0;
		std::uint64_t iterations = 0;
	};
	Cost cost() const;

private:
	struct Slot {
		SpscFrameRing ring;
		LoudnessMeter meter;
		TruePeakMeter tp;
		bool tpFed = false;           // the true-peak meter has been fed since it was last reset
		long long tpBlocksSeen = 0;
		std::uint64_t lastDropped = 0; // ring drop counter at the last tick (a change resets the meters)
		double tpBlocks[kTpWindowBlocks];
		int tpHead = 0;
		int tpCount = 0;
		std::uint64_t lastRead = 0;   // ring read position at the last data seen
		std::int64_t lastDataNs = 0;
		bool stale = true;
		LufsResult out;               // guarded by LoudnessWorker::mutex_
	};
	void loop();
	void tickSlot(Slot &s, std::int64_t nowNs, bool tpOn);

	double rate_ = 0.0;
	int channels_ = 0;
	bool running_ = false;
	std::vector<std::unique_ptr<Slot>> slots_;
	std::vector<std::vector<float>> scratch_; // planar read buffers, one per channel
	std::vector<float *> scratchPtr_;
	std::vector<const float *> scratchCPtr_;
	std::thread thread_;
	std::mutex wakeMutex_;
	std::condition_variable wake_;
	bool stopFlag_ = false; // guarded by wakeMutex_
	std::atomic<bool> abort_{false}; // set by stop(): checked between reads and slots so queued audio is abandoned
	mutable std::mutex mutex_; // guards Slot::out and the cost fields
	std::atomic<bool> tpWanted_{false};
	bool tpActive_ = false; // worker thread only: the state the true-peak meters currently follow
	double costSumUs_ = 0.0;
	double costMaxUs_ = 0.0;
	std::uint64_t costN_ = 0;
};

} // namespace inflight
