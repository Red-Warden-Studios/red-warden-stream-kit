// Tests for level-accumulator.hpp: the audio-thread to UI-thread level hand-off. Pure (no OBS, no Qt).
// No framework: exits non-zero on failure.
#include "level-accumulator.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

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

static const float kInf = std::numeric_limits<float>::infinity();
static const float kNan = std::numeric_limits<float>::quiet_NaN();

static bool isNegInf(float v)
{
	return std::isinf(v) && v < 0;
}

static void testMaxDb()
{
	const float allInf[4] = {-kInf, -kInf, -kInf, -kInf};
	CHECK(isNegInf(LevelAccumulator::maxDb(allInf, 4)), "all -inf stays -inf");
	const float allNan[3] = {kNan, kNan, kNan};
	CHECK(std::isnan(LevelAccumulator::maxDb(allNan, 3)), "all NaN gives NaN (no valid value), not -inf");
	const float nanAndInf[3] = {kNan, -kInf, kNan};
	CHECK(isNegInf(LevelAccumulator::maxDb(nanAndInf, 3)), "a mix of NaN and -inf channels still gives -inf (measured silence)");
	const float mixed[4] = {-kInf, kNan, -23.5f, -40.0f};
	CHECK(LevelAccumulator::maxDb(mixed, 4) == -23.5f, "mixed -inf/NaN/values gives the max value");
	const float above[3] = {-6.0f, 3.5f, kNan};
	CHECK(LevelAccumulator::maxDb(above, 3) == 0.0f, "value above 0 clamps to 0");
	const float one[1] = {-12.0f};
	CHECK(LevelAccumulator::maxDb(one, 1) == -12.0f, "single value");
	CHECK(std::isnan(LevelAccumulator::maxDb(one, 0)), "n = 0 gives NaN (no valid value)");
	// Channel count: a stale loud value beyond the meter's channel count must not count.
	const float stale[8] = {-30.0f, -3.0f, -kInf, -kInf, -kInf, -kInf, -kInf, -kInf};
	CHECK(LevelAccumulator::maxDb(stale, 1) == -30.0f, "mono: the stale loud slot 1 is ignored");
	CHECK(LevelAccumulator::maxDb(stale, 2) == -3.0f, "stereo: slot 1 counts");
	CHECK(LevelAccumulator::clampChannels(0) == 8 && LevelAccumulator::clampChannels(-1) == 8, "unknown channel count reads all slots");
	CHECK(LevelAccumulator::clampChannels(1) == 1 && LevelAccumulator::clampChannels(2) == 2, "channel count 1 and 2 pass through");
	CHECK(LevelAccumulator::clampChannels(99) == 8, "channel count above 8 clamps to 8");
	LevelAccumulator ch;
	CHECK(ch.channels() == 8, "default channel count is all slots");
	ch.setChannels(1);
	CHECK(ch.channels() == 1, "setChannels(1)");
	ch.setChannels(0);
	CHECK(ch.channels() == 8, "setChannels(0) falls back to all slots");
	const float nanFirst[3] = {kNan, -30.0f, -kInf};
	CHECK(LevelAccumulator::maxDb(nanFirst, 3) == -30.0f, "NaN first does not poison the max");
}

static void testTake()
{
	LevelAccumulator a;
	LevelAccumulator::Reading r = a.take();
	CHECK(!r.fresh, "before any callback: not fresh");
	CHECK(isNegInf(r.peakDb) && isNegInf(r.avgDb), "before any callback: -inf");
	CHECK(r.lastCallbackMs == 0, "before any callback: lastCallbackMs 0");

	a.onCallback(-30.0f, -40.0f, 1000);
	a.onCallback(-12.0f, -35.0f, 1100);
	a.onCallback(-20.0f, -50.0f, 1200);
	r = a.take();
	CHECK(r.fresh, "after callbacks: fresh");
	CHECK(r.peakDb == -12.0f, "peak is the max of the callbacks");
	CHECK(r.avgDb == -35.0f, "avg is the max of the callbacks");
	CHECK(r.lastCallbackMs == 1200, "lastCallbackMs is the newest");

	r = a.take();
	CHECK(!r.fresh, "no callback since last take: not fresh");
	CHECK(r.peakDb == -12.0f && r.avgDb == -35.0f, "no callback: previous peak/avg repeated, not -inf");
	CHECK(r.lastCallbackMs == 1200, "no callback: lastCallbackMs keeps the old time");

	a.onCallback(-50.0f, -60.0f, 2300);
	r = a.take();
	CHECK(r.fresh && r.peakDb == -50.0f && r.avgDb == -60.0f, "next window starts clean (not max-ed with the old one)");
	CHECK(r.lastCallbackMs == 2300, "lastCallbackMs advances");
}

static void testSilence()
{
	LevelAccumulator a;
	a.onCallback(-kInf, -kInf, 500);
	a.onCallback(-kInf, -kInf, 600);
	LevelAccumulator::Reading r = a.take();
	CHECK(r.fresh, "silence callbacks are still fresh");
	CHECK(isNegInf(r.peakDb) && isNegInf(r.avgDb), "-inf callbacks stay -inf (digital silence)");
	CHECK(r.lastCallbackMs == 600, "silence: lastCallbackMs set");
	r = a.take();
	CHECK(!r.fresh && isNegInf(r.peakDb), "silence then nothing: previous -inf repeated");

	a.onCallback(-kInf, -kInf, 700);
	a.onCallback(-25.0f, -kInf, 710);
	r = a.take();
	CHECK(r.peakDb == -25.0f && isNegInf(r.avgDb), "peak and avg are maxed independently");

	// An all-NaN callback is "no valid reading": nothing is touched.
	LevelAccumulator b;
	b.onCallback(-18.0f, -21.0f, 900);
	b.take();
	b.onCallback(kNan, kNan, 5000);
	r = b.take();
	CHECK(!r.fresh, "all-NaN callback leaves nothing pending (not fresh)");
	CHECK(r.lastCallbackMs == 900, "all-NaN callback does not move lastCallbackMs");
	CHECK(r.peakDb == -18.0f && r.avgDb == -21.0f, "all-NaN callback does not turn the old reading into -inf");
	LevelAccumulator c;
	c.onCallback(kNan, kNan, 700);
	r = c.take();
	CHECK(!r.fresh && r.lastCallbackMs == 0, "a source that only emits NaN never gets a callback time");
	c.onCallback(-10.0f, kNan, 710);
	r = c.take();
	CHECK(!r.fresh && r.lastCallbackMs == 0, "a callback with one NaN half is ignored as a whole");
}

static void testGrace()
{
	GraceTimer g;
	CHECK(!g.expired(true, 1000, 30000), "grace: first Unknown tick is not expired");
	CHECK(!g.expired(true, 30999, 30000), "grace: 29.999 s of Unknown is not expired");
	CHECK(g.expired(true, 31000, 30000), "grace: 30 s of continuous Unknown expires");
	CHECK(!g.expired(false, 32000, 30000), "grace: a Live tick resets");
	CHECK(!g.expired(true, 33000, 30000), "grace: counting restarts after the reset");
	CHECK(!g.expired(true, 62999, 30000), "grace: restarted count not expired early");
	CHECK(g.expired(true, 63000, 30000), "grace: restarted count expires 30 s after the restart");
	g.reset();
	CHECK(!g.expired(true, 70000, 30000), "grace: reset() starts over");
}

static void testReset()
{
	LevelAccumulator a;
	a.onCallback(-10.0f, -15.0f, 4000);
	a.take();
	a.onCallback(-5.0f, -8.0f, 4100);
	a.reset();
	LevelAccumulator::Reading r = a.take();
	CHECK(!r.fresh, "after reset: nothing pending");
	CHECK(isNegInf(r.peakDb) && isNegInf(r.avgDb), "after reset: previous values are -inf");
	CHECK(r.lastCallbackMs == 0, "after reset: lastCallbackMs 0");
	CHECK(!a.everHadCallback(), "after reset: no callback ever");
}

static void testStress()
{
	LevelAccumulator a;
	std::atomic<bool> stop{false};
	const float kMaxPushed = -9.0f;
	std::thread audio([&] {
		std::int64_t t = 1;
		unsigned i = 0;
		while (!stop.load(std::memory_order_relaxed)) {
			const float v = -60.0f + static_cast<float>(i % 51); // -60 .. -10
			a.onCallback(v, v - 3.0f, t++);
			if (i % 1000 == 999)
				a.onCallback(kMaxPushed, kMaxPushed - 3.0f, t++);
			++i;
		}
	});
	long fresh = 0, bad = 0, silenceFake = 0;
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
	while (std::chrono::steady_clock::now() < end) {
		const LevelAccumulator::Reading r = a.take();
		if (r.fresh) {
			++fresh;
			if (isNegInf(r.peakDb) || isNegInf(r.avgDb))
				++silenceFake; // the writer never pushes -inf: a fresh -inf would be a split
		}
		if (r.peakDb > kMaxPushed || r.avgDb > kMaxPushed - 3.0f + 0.0001f)
			++bad;
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	stop.store(true);
	audio.join();
	std::printf("     stress: fresh=%ld bad=%ld fake-silence=%ld\n", fresh, bad, silenceFake);
	CHECK(fresh > 0, "stress: some readings were fresh");
	CHECK(bad == 0, "stress: no reading above the max ever pushed");
	CHECK(silenceFake == 0, "stress: no fresh reading is ever a fake -inf");
}

int main()
{
	testMaxDb();
	testTake();
	testSilence();
	testGrace();
	testReset();
	testStress();
	std::printf("%s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", total, failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
