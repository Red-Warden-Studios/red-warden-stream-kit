/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Tests for spsc-ring.hpp (single-threaded semantics, then a two-thread stress) and for the LUFS worker
// (loudness-worker.hpp) driven with synthetic rings. Pure std, no OBS, no Qt. No framework: exits non-zero on
// failure.
#include "loudness-worker.hpp"
#include "spsc-ring.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace inflight;

static int failures = 0;
static int total = 0;
#define CHECK(cond, msg)                                                      \
	do {                                                                  \
		total++;                                                      \
		if (!(cond)) {                                                \
			std::printf("FAIL: %s  (line %d)\n", msg, __LINE__); \
			failures++;                                           \
		} else {                                                      \
			std::printf("ok:   %s\n", msg);                        \
		}                                                             \
	} while (0)

static void sleepMs(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void testSingleThread()
{
	SpscFrameRing r;
	CHECK(!r.init(0, 10) && !r.init(9, 10) && !r.init(2, 0), "init rejects 0 / 9 channels and 0 frames");
	CHECK(r.init(2, 10), "init 2 ch x 10 frames");
	float a[6] = {1, 2, 3, 4, 5, 6}, b[6] = {-1, -2, -3, -4, -5, -6};
	const float *in[2] = {a, b};
	float oa[16], ob[16];
	float *out[2] = {oa, ob};
	CHECK(r.read(out, 16) == 0, "empty ring reads 0 frames");
	CHECK(r.write(in, 6, 1.0f) && r.writtenFrames() == 6, "write 6 frames");
	CHECK(r.read(out, 4) == 4 && oa[0] == 1 && oa[3] == 4 && ob[3] == -4, "read 4 in order, both channels");
	CHECK(r.write(in, 6, 1.0f), "write 6 more: wraps the 10-frame ring (2 left + 6 = 8)");
	CHECK(r.read(out, 16) == 8, "read the remaining 8");
	CHECK(oa[0] == 5 && oa[1] == 6 && oa[2] == 1 && oa[7] == 6 && ob[2] == -1 && ob[7] == -6, "wrapped data in order");
	CHECK(r.write(in, 6, 1.0f) && !r.write(in, 6, 1.0f), "a block that does not fit is refused");
	CHECK(r.droppedBlocks() == 1 && r.droppedFrames() == 6, "and counted: 1 block, 6 frames");
	CHECK(r.read(out, 16) == 6 && oa[5] == 6, "the refused block left the stored one intact");
	CHECK(r.write(in, 3, 0.5f) && r.read(out, 3) == 3 && oa[1] == 1.0f && ob[2] == -1.5f, "gain multiplies (wrapped path too)");
	CHECK(r.write(in, 3, 0.0f) && r.read(out, 3) == 3 && oa[0] == 0.0f && ob[2] == 0.0f, "gain 0 writes silence");
	const float *half[2] = {a, nullptr};
	CHECK(r.write(half, 4, 1.0f) && r.read(out, 4) == 4 && oa[3] == 4 && ob[0] == 0 && ob[3] == 0, "a null plane is silence");
	CHECK(r.write(nullptr, 2, 1.0f) && r.read(out, 2) == 2 && oa[0] == 0 && ob[1] == 0, "null planes = silence");
	float big[11] = {0};
	const float *bigIn[2] = {big, big};
	CHECK(!r.write(bigIn, 11, 1.0f), "a block bigger than the ring is refused");
	CHECK(r.write(bigIn, 0, 1.0f), "an empty block is a no-op success");
}

// Two threads. The producer only writes when the ring has room (so nothing may be lost); the consumer checks
// that every frame arrives, in order, in both channels. Block sizes are pseudo-random 1..700.
static void testStressNoLoss()
{
	const std::uint64_t kTotal = 3000000;
	SpscFrameRing r;
	r.init(2, 4096);
	std::atomic<bool> producerDone{false};
	const char *sab = std::getenv("RWS_RING_TEST_SABOTAGE");
	const bool sabotage = sab && sab[0] == '1';
	bool sabotaged = false;
	std::thread prod([&] {
		std::vector<float> a(700), b(700);
		std::uint64_t k = 0;
		unsigned s = 1;
		while (k < kTotal) {
			s = s * 1664525u + 1013904223u;
			size_t n = 1 + (s >> 8) % 700;
			if (k + n > kTotal)
				n = static_cast<size_t>(kTotal - k);
			for (size_t i = 0; i < n; i++) {
				a[i] = static_cast<float>((k + i) % 1000003);
				b[i] = -a[i];
			}
			// Fail-proof hook: RWS_RING_TEST_SABOTAGE=1 corrupts one frame in flight; the check below must then fail.
			if (sabotage && !sabotaged && k >= 1500000) {
				a[0] += 1.0f;
				sabotaged = true;
			}
			while (r.capacity() - static_cast<size_t>(r.writtenFrames() - r.readFrames()) < n)
				std::this_thread::yield();
			const float *in[2] = {a.data(), b.data()};
			r.write(in, n, 1.0f);
			k += n;
		}
		producerDone = true;
	});
	std::vector<float> oa(512), ob(512);
	float *out[2] = {oa.data(), ob.data()};
	std::uint64_t got = 0;
	bool inOrder = true;
	while (got < kTotal) {
		const size_t n = r.read(out, 512);
		if (n == 0) {
			std::this_thread::yield();
			continue;
		}
		for (size_t i = 0; i < n; i++) {
			const float want = static_cast<float>((got + i) % 1000003);
			if (oa[i] != want || ob[i] != -want)
				inOrder = false;
		}
		got += n;
	}
	prod.join();
	CHECK(got == kTotal && inOrder, "stress, room-checking producer: 3,000,000 frames, all in order, both channels");
	CHECK(r.droppedBlocks() == 0 && r.droppedFrames() == 0, "stress, room-checking producer: nothing dropped (below capacity)");
}

// The producer never waits and the consumer is slow, on a ring of 15 blocks: blocks are dropped WHOLE, counted,
// and what arrives is an in-order subsequence. received + dropped == written.
static void testStressOverflow()
{
	const std::uint64_t kBlocks = 200000;
	const size_t kBlock = 64;
	SpscFrameRing r;
	r.init(2, 1000);
	std::atomic<bool> producerDone{false};
	std::thread prod([&] {
		std::vector<float> a(kBlock), b(kBlock);
		for (std::uint64_t id = 1; id <= kBlocks; id++) {
			for (size_t i = 0; i < kBlock; i++)
				a[i] = b[i] = static_cast<float>(id);
			const float *in[2] = {a.data(), b.data()};
			r.write(in, kBlock, 1.0f);
		}
		producerDone = true;
	});
	std::vector<float> oa(kBlock * 4), ob(kBlock * 4);
	float *out[2] = {oa.data(), ob.data()};
	std::uint64_t gotBlocks = 0;
	float lastId = 0;
	bool ok = true;
	size_t carry = 0; // frames of the current block seen so far
	for (;;) {
		const size_t n = r.read(out, kBlock * 4);
		if (n == 0) {
			if (producerDone && r.readFrames() == r.writtenFrames())
				break;
			std::this_thread::sleep_for(std::chrono::microseconds(200)); // slow consumer
			continue;
		}
		for (size_t i = 0; i < n; i++) {
			if (oa[i] != ob[i])
				ok = false;
			if (carry == 0) {
				if (!(oa[i] > lastId))
					ok = false; // ids must strictly increase
				lastId = oa[i];
			} else if (oa[i] != lastId) {
				ok = false; // a block is never torn
			}
			carry = (carry + 1) % kBlock;
			if (carry == 0)
				gotBlocks++;
		}
	}
	prod.join();
	std::printf("info:  overflow stress: received %llu blocks, dropped %llu blocks (%llu frames)\n",
		    static_cast<unsigned long long>(gotBlocks), static_cast<unsigned long long>(r.droppedBlocks()),
		    static_cast<unsigned long long>(r.droppedFrames()));
	CHECK(ok && carry == 0, "overflow stress: arrivals are whole blocks, strictly in order, channels equal");
	CHECK(r.droppedBlocks() > 0, "overflow stress: a slow consumer does make the ring overflow");
	CHECK(gotBlocks + r.droppedBlocks() == kBlocks, "overflow stress: received + dropped blocks == written blocks");
	CHECK(r.droppedFrames() == r.droppedBlocks() * kBlock, "overflow stress: dropped frames == dropped blocks x block size");
}

// The worker on synthetic rings: slot 0 gets a stereo 1 kHz sine at -20 dBFS (4.5 s of audio, written faster
// than real time: the windows count samples, not wall time), slot 1 digital silence. Expected: M = S = -20.0
// +-0.1 (stereo, -20 dBFS sine = -20 LUFS, Tech 3341 case 1 scaled), true peak -20 dBTP (+0.2/-0.4), the
// silent slot -inf everywhere, no overflow, then "not active" once the audio stops for the stale limit.
static void testWorker()
{
	const double fs = 48000.0;
	ChannelRole roles[2] = {ChannelRole::Left, ChannelRole::Right};
	LoudnessWorker w;
	CHECK(!w.start(fs, nullptr, 2, 1, false) && !w.start(fs, roles, 0, 1, false) && !w.start(2000.0, roles, 2, 1, false),
	      "worker rejects null roles, 0 channels, 2 kHz");
	CHECK(w.start(fs, roles, 2, 2, true) && w.running() && w.numSlots() == 2, "worker starts with 2 slots, true peak on");
	std::vector<float> tone(480), zero(480, 0.0f);
	std::uint64_t n = 0;
	for (int blk = 0; blk < 450; blk++) {
		for (size_t i = 0; i < 480; i++, n++)
			tone[i] = static_cast<float>(0.1 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(n) / fs));
		const float *t2[2] = {tone.data(), tone.data()};
		const float *z2[2] = {zero.data(), zero.data()};
		// Wait for room first: a refused write counts as an overflow, which is checked below.
		while (w.ring(0)->capacity() - static_cast<size_t>(w.ring(0)->writtenFrames() - w.ring(0)->readFrames()) < 480)
			sleepMs(1);
		w.ring(0)->write(t2, 480, 1.0f);
		w.ring(1)->write(z2, 480, 1.0f);
		if (blk % 20 == 0)
			sleepMs(5);
	}
	for (int i = 0; i < 200 && w.ring(0)->readFrames() != w.ring(0)->writtenFrames(); i++)
		sleepMs(10);
	sleepMs(60);
	const LufsResult a = w.result(0), b = w.result(1);
	std::printf("info:  worker tone: M %.3f S %.3f TP %.3f active=%d | silence: M %.3f S %.3f TP %.3f\n", a.momentary,
		    a.shortTerm, a.tpMax, a.active ? 1 : 0, b.momentary, b.shortTerm, b.tpMax);
	CHECK(a.active && a.momentaryReady && a.shortReady, "worker: tone slot is active and both windows are ready");
	CHECK(std::fabs(a.momentary + 20.0) <= 0.1 && std::fabs(a.shortTerm + 20.0) <= 0.1, "worker: tone M and S = -20.0 LUFS +-0.1");
	CHECK(a.tpValid && a.tpMax <= -20.0 + 0.2 && a.tpMax >= -20.0 - 0.4, "worker: tone true peak -20 dBTP (+0.2/-0.4)");
	CHECK(b.active && b.momentaryReady && std::isinf(b.momentary) && b.momentary < 0 && std::isinf(b.shortTerm) &&
		      b.shortTerm < 0 && b.tpValid && std::isinf(b.tpMax) && b.tpMax < 0,
	      "worker: silent slot reads -inf for M, S and true peak");
	CHECK(a.droppedBlocks == 0 && b.droppedBlocks == 0, "worker: ring overflow count is 0");
	const LoudnessWorker::Cost c = w.cost();
	std::printf("info:  worker cost: mean %.1f us max %.1f us over %llu iterations\n", c.meanUs, c.maxUs,
		    static_cast<unsigned long long>(c.iterations));
	CHECK(c.iterations > 10, "worker: it has been iterating");
	w.setTruePeak(false);
	sleepMs(80);
	CHECK(!w.result(0).tpValid, "worker: true peak off -> not valid");
	sleepMs(LoudnessWorker::kStaleMs + 300);
	CHECK(!w.result(0).active && !w.result(1).active, "worker: no audio for 1.5 s -> not active (\"--\", not silence)");
	w.stop();
	CHECK(!w.running() && w.ring(0) == nullptr, "worker: stop() joins and frees the rings");
	CHECK(w.start(fs, roles, 2, 1, false) && w.numSlots() == 1, "worker: can start again after stop");
	w.stop();
}

// Feeds `blocks` x 480 frames of a -20 dBFS stereo 1 kHz sine into ring `slot` (waits for room first).
static void feedTone(LoudnessWorker &w, int slot, int blocks, std::uint64_t &n)
{
	std::vector<float> tone(480);
	for (int blk = 0; blk < blocks; blk++) {
		for (size_t i = 0; i < 480; i++, n++)
			tone[i] = static_cast<float>(0.1 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(n) / 48000.0));
		const float *t2[2] = {tone.data(), tone.data()};
		while (w.ring(slot)->capacity() - static_cast<size_t>(w.ring(slot)->writtenFrames() - w.ring(slot)->readFrames()) < 480)
			sleepMs(1);
		w.ring(slot)->write(t2, 480, 1.0f);
		if (blk % 20 == 0)
			sleepMs(5);
	}
}

// Item 3 at the worker: a click in the first of five 100 ms blocks written in one go still reaches tpMax.
static void testWorkerTransient()
{
	ChannelRole roles[2] = {ChannelRole::Left, ChannelRole::Right};
	LoudnessWorker w;
	CHECK(w.start(48000.0, roles, 2, 1, true), "transient: worker starts");
	std::vector<float> p(24000, 0.0f);
	p[100] = 0.5f; // -6 dBFS click inside block 0 of 5
	const float *pp[2] = {p.data(), p.data()};
	w.ring(0)->write(pp, p.size(), 1.0f);
	for (int i = 0; i < 100 && w.ring(0)->readFrames() != w.ring(0)->writtenFrames(); i++)
		sleepMs(5);
	sleepMs(60);
	const LufsResult r = w.result(0);
	std::printf("info:  transient in block 0 of 5 fed at once: tpMax %.3f dBTP\n", r.tpMax);
	CHECK(r.tpValid && std::fabs(r.tpMax + 6.0) < 0.5, "transient in the first of five blocks fed at once is in the reported true-peak max");
	w.stop();
}

// Item 4: a ring overflow resets that source's meters ("--" until the windows refill); the drop count stays.
static void testWorkerDrop()
{
	ChannelRole roles[2] = {ChannelRole::Left, ChannelRole::Right};
	LoudnessWorker w;
	w.start(48000.0, roles, 2, 1, true);
	std::uint64_t n = 0;
	feedTone(w, 0, 450, n);
	for (int i = 0; i < 200 && w.ring(0)->readFrames() != w.ring(0)->writtenFrames(); i++)
		sleepMs(10);
	sleepMs(60);
	LufsResult a = w.result(0);
	CHECK(a.momentaryReady && a.shortReady && a.droppedBlocks == 0, "drop: windows ready, no drops before");
	std::vector<float> big(w.ring(0)->capacity() + 1, 0.1f); // larger than the ring: refused and counted
	const float *bp[2] = {big.data(), big.data()};
	CHECK(!w.ring(0)->write(bp, big.size(), 1.0f), "drop: the oversized block is refused");
	sleepMs(80);
	a = w.result(0);
	CHECK(a.droppedBlocks == 1 && !a.momentaryReady && !a.shortReady && !a.tpValid,
	      "drop: the meters reset (M, S, true peak back to not ready) and the drop is still counted");
	feedTone(w, 0, 60, n); // 0.6 s of fresh audio: M (400 ms) is ready again, S (3 s) is not
	for (int i = 0; i < 200 && w.ring(0)->readFrames() != w.ring(0)->writtenFrames(); i++)
		sleepMs(10);
	sleepMs(60);
	a = w.result(0);
	CHECK(a.momentaryReady && !a.shortReady && a.droppedBlocks == 1 && std::fabs(a.momentary + 20.0) <= 0.1,
	      "drop: M refills after 400 ms of new audio, S still filling, count unchanged");
	w.stop();
}

// Item 5: stop() with several full rings abandons the queued audio and returns quickly.
static void testWorkerBoundedStop()
{
	ChannelRole roles[2] = {ChannelRole::Left, ChannelRole::Right};
	LoudnessWorker w;
	w.start(48000.0, roles, 2, 8, true);
	std::vector<float> blk(4800, 0.1f);
	const float *bp[2] = {blk.data(), blk.data()};
	for (int s = 0; s < 8; s++)
		for (int i = 0; i < 10; i++)
			w.ring(s)->write(bp, blk.size(), 1.0f); // 10 x 4800 = 48000 frames = full
	const auto t0 = std::chrono::steady_clock::now();
	w.stop();
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	std::printf("info:  stop() with 8 full rings: %.2f ms\n", ms);
	CHECK(!w.running() && ms < 50.0, "stop() with 8 full rings returns within 50 ms");
}

int main()
{
	testSingleThread();
	testStressNoLoss();
	testStressOverflow();
	testWorker();
	testWorkerTransient();
	testWorkerDrop();
	testWorkerBoundedStop();
	std::printf("%d checks, %d failed\n", total, failures);
	return failures == 0 ? 0 : 1;
}
