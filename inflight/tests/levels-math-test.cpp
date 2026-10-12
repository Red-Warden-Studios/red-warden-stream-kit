/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Tests for levels-math.hpp: peak hold, average smoothing, number formatting, gap wording. Pure (Qt Core
// only, no OBS). No framework: exits non-zero on failure.
#include "levels-handoff.hpp"
#include "levels-math.hpp"

#include <QByteArray>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

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

static bool near(double a, double b, double tol)
{
	return std::fabs(a - b) <= tol;
}

static TrFn idTr()
{
	return [](const char *, const char *fallback) { return QString::fromUtf8(fallback); };
}

static bool is(const QString &s, const char *expect)
{
	return s == QString::fromUtf8(expect);
}

static void testPeakHold()
{
	PeakHold h;
	CHECK(isNegInf(h.value(0)), "empty hold is -inf");
	h.feed(-10.0f, 1000);
	CHECK(near(h.value(1000), -10.0, 1e-6), "peak visible at once");
	CHECK(near(h.value(1000 + 1499), -10.0, 1e-6), "peak still held at 1499 ms");
	CHECK(isNegInf(h.value(1000 + 1500)), "peak gone at 1500 ms (window is < 1500)");
	CHECK(isNegInf(h.value(1000 + 1501)), "peak gone at 1501 ms");

	PeakHold g;
	g.feed(-20.0f, 0);
	g.feed(-5.0f, 200);
	g.feed(-30.0f, 400);
	CHECK(near(g.value(400), -5.0, 1e-6), "max of the window wins");
	CHECK(near(g.value(200 + 1499), -5.0, 1e-6), "the max is held 1499 ms after it was fed");
	CHECK(near(g.value(200 + 1500), -30.0, 1e-6), "after the max expires the next one shows (-30)");
	CHECK(isNegInf(g.value(400 + 1500)), "everything expired -> -inf");

	PeakHold z;
	z.feed(-kInf, 0);
	z.feed(-kInf, 200);
	CHECK(isNegInf(z.value(300)), "hold of all -inf is -inf");
	z.feed(-40.0f, 400);
	CHECK(near(z.value(500), -40.0, 1e-6), "a real peak beats -inf");
	CHECK(isNegInf(z.value(400 + 1500)), "and falls back to -inf when it expires");

	PeakHold n;
	n.feed(kNan, 0);
	CHECK(isNegInf(n.value(0)), "NaN is ignored");
	n.feed(-12.0f, 100);
	n.feed(kNan, 200);
	CHECK(near(n.value(300), -12.0, 1e-6), "NaN does not disturb a held peak");

	// The supported 5 Hz feed: the expiry sequence, one entry leaving at a time.
	PeakHold q;
	const float seq[] = {-30, -10, -20, -25, -40, -35, -50, -45, -60};
	for (int i = 0; i < 9; ++i)
		q.feed(seq[i], i * 200);
	CHECK(q.size() <= PeakHold::capacity(), "5 Hz feed fits the ring");
	CHECK(near(q.value(1600), -10.0, 1e-6), "t=1600: the -10 fed at 200 is held (1400 ms old)");
	q.feed(-70.0f, 1800); // the -10 (1600 ms old) expires as this arrives
	CHECK(near(q.value(1800), -20.0, 1e-6), "t=1800: -10 expired, -20 (fed at 400) shows");
	CHECK(near(q.value(1899), -20.0, 1e-6), "t=1899: -20 still held (1499 ms)");
	CHECK(near(q.value(1900), -25.0, 1e-6), "t=1900: -20 expired, -25 (fed at 600) shows");
	CHECK(near(q.value(2100), -35.0, 1e-6), "t=2100: -25 expired, -35 (fed at 1000) shows");
	CHECK(near(q.value(2699), -45.0, 1e-6), "t=2699: -35 and -50 gone, -45 (fed at 1400) shows");
	CHECK(isNegInf(q.value(1600 + 1500 + 200)) || q.value(3300) < -69.0f, "long after, only the last peak is left");

	PeakHold b; // far faster than 20 Hz: bounded memory, no exactness claimed
	for (int i = 0; i < 1000; ++i)
		b.feed(-10.0f - static_cast<float>(i % 50) * 0.01f, i * 10);
	CHECK(b.size() <= PeakHold::capacity(), "memory stays bounded under fast feeds");

	PeakHold o; // 100 strictly falling peaks inside one window: more than the ring holds
	for (int i = 0; i < 100; ++i)
		o.feed(-10.0f - static_cast<float>(i), i);
	CHECK(o.size() == PeakHold::capacity(), "ring overflow is capped at the capacity");
	CHECK(near(o.value(100), -70.0, 1e-6), "overflow drops the OLDEST entries (kept: the newest 40, oldest -70)");
	PeakHold c;
	c.feed(-3.0f, 5000);
	c.feed(-9.0f, 4000); // clock went backwards
	CHECK(near(c.value(4000), -9.0, 1e-6), "a clock jump back clears the old history");
}

// The panel's real feed rate: every 50 ms (20 Hz).
static void testFastRate()
{
	// Strictly falling peaks every 50 ms: every entry stays in the deque until it expires, the worst case.
	PeakHold h;
	for (int i = 0; i < 30; ++i)
		h.feed(-10.0f - static_cast<float>(i), i * 50); // t = 0 .. 1450
	CHECK(h.size() == 30, "20 Hz x 1.5 s = 30 entries held, all inside the 40-entry ring");
	CHECK(PeakHold::capacity() >= 30 + 5, "the ring has headroom over 30");
	CHECK(near(h.value(1450), -10.0, 1e-6), "t=1450: the first peak (-10 at 0) is still held");
	CHECK(near(h.value(1499), -10.0, 1e-6), "t=1499: held (1499 ms old)");
	CHECK(near(h.value(1500), -11.0, 1e-6), "t=1500: -10 expired exactly at 1500 ms, -11 (fed at 50) shows");
	CHECK(near(h.value(1549), -11.0, 1e-6), "t=1549: -11 held (1499 ms old)");
	CHECK(near(h.value(1550), -12.0, 1e-6), "t=1550: -11 expired, -12 shows");
	h.feed(-60.0f, 1500); // the feed at the boundary drops the expired front
	CHECK(h.size() == 30, "a feed at 1500 ms evicts the entry that just expired (still 30)");
	CHECK(near(h.value(1500), -11.0, 1e-6), "after that feed the held peak is -11");

	// A long steady run of falling peaks never exceeds 30 entries.
	PeakHold s;
	int maxSize = 0;
	for (int i = 0; i < 400; ++i) {
		s.feed(-10.0f - static_cast<float>(i) * 0.01f, i * 50);
		if (s.size() > maxSize)
			maxSize = s.size();
	}
	CHECK(maxSize == 30, "steady 20 Hz feed of falling peaks peaks at 30 entries, never more");

	// A loud peak held 1.5 s then replaced by the quiet one that followed it.
	PeakHold k;
	k.feed(-30.0f, 0);
	k.feed(-5.0f, 50);
	for (int i = 2; i <= 30; ++i)
		k.feed(-30.0f, i * 50); // up to t = 1500, the transient (fed at 50) is 1450 ms old
	CHECK(near(k.value(1549), -5.0, 1e-6), "a transient is held 1499 ms after it was fed");
	CHECK(near(k.value(1550), -30.0, 1e-6), "and gone at 1500 ms: the quiet level shows again");
	k.feed(-30.0f, 1550);
	CHECK(near(k.value(1550), -30.0, 1e-6), "the next feed evicts it for good");

	// Overflow sanity: 100 feeds 1 ms apart (far faster than 20 Hz) - bounded, newest kept, oldest dropped.
	PeakHold o;
	for (int i = 0; i < 100; ++i)
		o.feed(-10.0f - static_cast<float>(i), i);
	CHECK(o.size() == PeakHold::capacity() && near(o.value(99), -70.0, 1e-6),
	      "overflow keeps the newest 40 (oldest shown -70), never crashes");

	// AvgSmoother fed every 50 ms: the 300 ms step response is still 63% (alpha composes exactly).
	AvgSmoother st;
	st.feed(-kInf, 0);
	for (int t = 50; t <= 300; t += 50)
		st.feed(0.0f, t);
	CHECK(near(st.value(), 20.0 * std::log10(1.0 - std::exp(-1.0)), 0.02), "50 ms dt: step response is 63% at 300 ms (-3.98 dB)");

	// ...and agrees with the old 200 ms feed at the same instants (600 ms: 1 - exp(-2)).
	AvgSmoother a50;
	AvgSmoother a200;
	a50.feed(-kInf, 0);
	a200.feed(-kInf, 0);
	for (int t = 50; t <= 600; t += 50)
		a50.feed(-10.0f, t);
	for (int t = 200; t <= 600; t += 200)
		a200.feed(-10.0f, t);
	CHECK(near(a50.value(), a200.value(), 0.05), "20 Hz and 5 Hz feeds give the same smoothed value at 600 ms");

	// Silence snap at 50 ms ticks: -inf for 600 ms of unbroken -inf input.
	AvgSmoother d;
	d.feed(-20.0f, 0);
	for (int t = 50; t <= 600; t += 50)
		d.feed(-kInf, t);
	CHECK(!isNegInf(d.value()), "t=600: only 550 ms of silence, still a decaying number");
	d.feed(-kInf, 650);
	CHECK(isNegInf(d.value()), "t=650: 600 ms of unbroken -inf snaps to -inf");
}

static void testSmoother()
{
	AvgSmoother s;
	CHECK(isNegInf(s.value()), "nothing fed reads -inf");
	s.feed(-20.0f, 0);
	CHECK(near(s.value(), -20.0, 1e-4), "first sample sets the value outright");

	// Step from silence to 0 dBFS: after one time constant the amplitude is 1 - 1/e = 0.632 (-3.98 dB).
	AvgSmoother st;
	st.feed(-kInf, 0);
	for (int t = 10; t <= 300; t += 10)
		st.feed(0.0f, t);
	CHECK(near(st.value(), 20.0 * std::log10(1.0 - std::exp(-1.0)), 0.02), "step response is 63% at 300 ms (-3.98 dB)");

	// Irregular ticks give the same curve as regular ones.
	AvgSmoother ir;
	ir.feed(-kInf, 0);
	const int steps[] = {7, 13, 41, 3, 90, 61, 25, 60}; // sums to 300
	int t = 0;
	for (int d : steps) {
		t += d;
		ir.feed(0.0f, t);
	}
	CHECK(t == 300, "irregular steps sum to 300 ms");
	CHECK(near(ir.value(), st.value(), 0.01), "irregular dt matches regular dt");

	// A single huge dt jumps (nearly) all the way.
	AvgSmoother big;
	big.feed(-kInf, 0);
	big.feed(-10.0f, 10000);
	CHECK(near(big.value(), -10.0, 0.01), "a long gap between samples converges");

	// Decay: a source that falls silent shows -inf within 1 s, never a stuck number.
	AvgSmoother d;
	d.feed(-20.0f, 0);
	d.feed(-kInf, 200);
	CHECK(!isNegInf(d.value()) && d.value() < -20.0f && d.value() > -40.0f,
	      "right after the drop it decays (a number, below -20)");
	d.feed(-kInf, 400);
	d.feed(-kInf, 600);
	d.feed(-kInf, 800);
	CHECK(isNegInf(d.value()), "-inf from the 800 ms tick (600 ms of unbroken -inf)");
	d.feed(-kInf, 1000);
	CHECK(isNegInf(d.value()), "still -inf at 1000 ms, within 1 s of the last sound");

	// Silence that is not yet 600 ms old is not snapped; a sound in between restarts the run.
	AvgSmoother r;
	r.feed(-20.0f, 0);
	r.feed(-kInf, 200);
	r.feed(-20.0f, 400);
	r.feed(-kInf, 600);
	r.feed(-kInf, 1000); // run since 600: 400 ms, not enough
	CHECK(!isNegInf(r.value()), "a broken silent run does not snap early");
	r.feed(-kInf, 1200);
	CHECK(isNegInf(r.value()), "600 ms after the new run began it snaps");

	// Digital silence from the first sample is -inf at once.
	AvgSmoother h;
	h.feed(-kInf, 0);
	CHECK(isNegInf(h.value()), "silent first sample reads -inf");

	// No floor: quiet finite audio is a number, only exact zero is -inf.
	AvgSmoother lo;
	lo.feed(-101.0f, 0);
	CHECK(!isNegInf(lo.value()) && near(lo.value(), -101.0, 1e-3), "-101 dBFS reads a number (no -100 floor)");
	AvgSmoother lo2;
	lo2.feed(-150.0f, 0);
	CHECK(near(lo2.value(), -150.0, 1e-2), "-150 dBFS reads a number too");
	AvgSmoother nn;
	nn.feed(-30.0f, 0);
	nn.feed(kNan, 100);
	CHECK(near(nn.value(), -30.0, 1e-3), "NaN is ignored");

	// Time going backwards: the older feed is ignored (value and lastT_ stay), and the next update
	// measures dt from the last accepted time: feeds at 1000, 500, 1100 -> the last uses dt = 100.
	AvgSmoother bk;
	bk.feed(-20.0f, 1000);
	bk.feed(0.0f, 500);
	CHECK(near(bk.value(), -20.0, 1e-3), "an older feed changes nothing");
	bk.feed(0.0f, 1100);
	const double want = 20.0 * std::log10(0.1 + 0.9 * (1.0 - std::exp(-100.0 / 300.0)));
	CHECK(near(bk.value(), want, 0.01), "the next feed uses dt = 100 from the last accepted time");
}

static void testHandoff()
{
	LevelsHandoff h;
	LevelsHandoff::Reading r = h.take();
	CHECK(!r.fresh, "empty window: no reading");

	// The varying-level case: 9 quiet callbacks at -40 and 1 loud at -10. The mean POWER is
	// (9e-4 + 0.1) / 10 = 0.01009 -> -19.96 dB, not the loudest value (-10).
	for (int i = 0; i < 9; ++i)
		h.onCallback(-35.0f, -40.0f, 100 + i);
	h.onCallback(-6.0f, -10.0f, 120);
	r = h.take();
	CHECK(r.fresh, "window with callbacks is fresh");
	CHECK(near(r.avgDb, 10.0 * std::log10((9e-4 + 0.1) / 10.0), 0.01), "average is the mean power (-19.96 dB)");
	CHECK(r.avgDb < -19.0f && r.avgDb > -21.0f, "average near -20, not the interval maximum (-10)");
	CHECK(near(r.peakDb, -6.0, 1e-6), "peak is the max of the window");
	CHECK(r.lastCallbackMs == 120, "last callback time kept");
	r = h.take();
	CHECK(!r.fresh, "take() cleared the window");
	CHECK(r.lastCallbackMs == 120, "last callback time survives take()");

	h.onCallback(-kInf, -kInf, 200);
	h.onCallback(-kInf, -kInf, 210);
	r = h.take();
	CHECK(r.fresh && isNegInf(r.avgDb) && isNegInf(r.peakDb), "all -inf callbacks: a real reading of silence");

	h.onCallback(-20.0f, -kInf, 300);
	h.onCallback(-20.0f, -20.0f, 310);
	r = h.take();
	CHECK(near(r.avgDb, 10.0 * std::log10(0.005), 0.01), "-inf counts as zero power inside a mean (half silent: -23 dB)");

	h.onCallback(kNan, -20.0f, 400);
	h.onCallback(-20.0f, kNan, 410);
	r = h.take();
	CHECK(!r.fresh && r.lastCallbackMs == 310, "NaN callbacks are ignored entirely (not even lastMs)");

	h.setChannels(0);
	CHECK(h.channels() == LevelAccumulator::kMaxChannels, "unknown channel count reads all slots");
	h.setChannels(2);
	CHECK(h.channels() == 2, "channel count kept");
}

static void testFormat()
{
	CHECK(is(formatDb(-12.4f), "-12.4"), "-12.4 -> \"-12.4\"");
	CHECK(is(formatDb(-12.46f), "-12.5"), "-12.46 rounds to -12.5");
	CHECK(is(formatDb(-12.44f), "-12.4"), "-12.44 rounds to -12.4");
	CHECK(is(formatDb(-12.45f), "-12.4"), "-12.45f (stored as -12.4499998) -> \"-12.4\" (printf rounding of the exact value)");
	CHECK(is(formatDb(0.0f), "0.0"), "0 -> \"0.0\"");
	CHECK(is(formatDb(-0.04f), "0.0"), "-0.04 -> \"0.0\", never \"-0.0\"");
	CHECK(is(formatDb(-60.0f), "-60.0"), "-60 -> \"-60.0\"");
	CHECK(is(formatDb(-kInf), "-inf"), "-inf -> \"-inf\"");
	CHECK(is(formatDb(kNan), "-inf"), "NaN -> \"-inf\"");
	CHECK(is(formatDb(-99.95f), "-100.0") || is(formatDb(-99.95f), "-99.9"), "-99.95 formats as a number");
}

static void testGap()
{
	Gap g = gapDb(-18.0f, -30.4f);
	CHECK(g.valid && near(g.db, 12.4, 1e-4), "mic -18.0 over other -30.4 = +12.4");
	g = gapDb(-30.0f, -26.9f);
	CHECK(g.valid && near(g.db, -3.1, 1e-4), "mic -30.0 under other -26.9 = -3.1");
	CHECK(!gapDb(-kInf, -20.0f).valid, "mic silent -> invalid");
	CHECK(!gapDb(-20.0f, -kInf).valid, "other silent -> invalid");
	CHECK(!gapDb(-kInf, -kInf).valid, "both silent -> invalid");
	CHECK(!gapDb(kNan, -20.0f).valid, "NaN -> invalid");

	const TrFn tr = idTr();
	CHECK(is(gapText("Voice", "Game", gapDb(-18.0f, -30.4f), -30.4f, tr), "Voice is 12.4 dB over Game"), "over text");
	CHECK(is(gapText("Voice", "Game", gapDb(-30.0f, -26.9f), -26.9f, tr), "Voice is 3.1 dB under Game"), "under text");
	CHECK(is(gapText("Voice", "Game", gapDb(-20.0f, -20.03f), -20.03f, tr), "Voice is level with Game"), "level text (|gap| < 0.05)");
	CHECK(is(gapText("Voice", "Game", gapDb(-20.0f, -20.06f), -20.06f, tr), "Voice is 0.1 dB over Game"), "0.06 dB shows as 0.1 over");
	CHECK(is(gapText("Voice", "Game", gapDb(-20.0f, -kInf), -kInf, tr), "Game is silent"), "other silent text");
	CHECK(is(gapText("Voice", "Game", gapDb(-kInf, -20.0f), -20.0f, tr), "Voice is silent"), "mic silent text");
	CHECK(is(gapText("Tone", "Hush", gapDb(-23.0f, -kInf), -kInf, tr), "Hush is silent"), "Hush is silent (the live check's wording)");
	CHECK(is(gapText("Voice", "Game", gapDb(-kInf, -kInf), -kInf, tr), "Game is silent"), "both silent: the other is named");
}

static void testRowKind()
{
	CHECK(rowKind(false, false, false) == RowKind::Missing, "no source, no meter -> Missing");
	CHECK(rowKind(true, false, false) == RowKind::NoReading, "source exists, meter not attached -> NoReading (real name, \"--\")");
	CHECK(rowKind(true, false, true) == RowKind::NoReading, "source exists, meter gone, stale reading flag -> NoReading");
	CHECK(rowKind(true, true, false) == RowKind::NoReading, "metered, no reading yet -> NoReading");
	CHECK(rowKind(true, true, true) == RowKind::Reading, "metered with a reading -> Reading");
	CHECK(rowKind(false, true, true) == RowKind::Reading, "metered implies it exists -> Reading");
	CHECK(rowKind(false, true, false) == RowKind::NoReading, "metered but source not resolved -> never Missing");
}

static void testInstantFill()
{
	CHECK(near(instantFill(true, true, -20.0f), -20.0, 1e-6), "fresh window: the fill is its peak");
	CHECK(isNegInf(instantFill(true, false, -20.0f)), "no fresh reading in the window: the fill drops to -inf");
	CHECK(isNegInf(instantFill(false, true, -20.0f)), "meter stale (unavailable): -inf");
	CHECK(isNegInf(instantFill(false, false, -20.0f)), "neither: -inf");
	CHECK(isNegInf(instantFill(true, true, -kInf)), "a fresh window of digital silence is -inf (nothing drawn)");
	CHECK(isNegInf(instantFill(true, true, kNan)), "NaN is no reading");
	// The hold is independent: a window with no fresh reading leaves PeakHold alone (fill drops, tick stays).
	PeakHold h;
	h.feed(-8.0f, 1000);
	const float fill = instantFill(true, false, -8.0f); // the next 50 ms window had no callback: nothing is fed
	CHECK(isNegInf(fill) && near(h.value(1050), -8.0, 1e-6), "fill drops while the hold tick keeps the peak for 1.5 s");
	CHECK(isNegInf(h.value(1000 + 1500)), "and the tick expires on schedule");
}

static void testLufsMode()
{
	CHECK(std::string(levelsModeKey(LevelsMode::Db)) == "db" && std::string(levelsModeKey(LevelsMode::Lufs)) == "lufs",
	      "mode keys: db / lufs");
	CHECK(levelsModeFromKey("lufs") == LevelsMode::Lufs && levelsModeFromKey("db") == LevelsMode::Db &&
		      levelsModeFromKey("") == LevelsMode::Db && levelsModeFromKey(nullptr) == LevelsMode::Db &&
		      levelsModeFromKey("LUFS") == LevelsMode::Db && levelsModeFromKey("lufsx") == LevelsMode::Db,
	      "mode from key: only exactly \"lufs\" is LUFS, anything else is dB (the default)");
	CHECK(near(clampLufsRef(-16.0), -16.0, 1e-9) && near(clampLufsRef(-99.0), kLufsRefMin, 1e-9) &&
		      near(clampLufsRef(5.0), kLufsRefMax, 1e-9) && near(clampLufsRef(kNan), kLufsRefDefault, 1e-9),
	      "reference clamps to -60..0, NaN -> the -16 default");
	CHECK(is(formatLufs(false, -23.0), "--") && is(formatLufs(false, -std::numeric_limits<double>::infinity()), "--"),
	      "not ready reads \"--\" whatever the value");
	CHECK(is(formatLufs(true, -23.04), "-23.0") && is(formatLufs(true, -std::numeric_limits<double>::infinity()), "-inf"),
	      "ready: one decimal, silence is -inf");
	const TrFn tr = idTr();
	CHECK(is(gapText("Mic/Aux", "Desktop Audio", gapDb(-17.0f, -23.1f), -23.1f, tr, true), "Mic/Aux is 6.1 LU over Desktop Audio"),
	      "LU over text");
	CHECK(is(gapText("Voice", "Game", gapDb(-30.0f, -26.9f), -26.9f, tr, true), "Voice is 3.1 LU under Game"), "LU under text");
	CHECK(is(gapText("Voice", "Game", gapDb(-20.0f, -kInf), -kInf, tr, true), "Game is silent"), "LU gap with a silent source");
	CHECK(is(gapText("Voice", "Game", gapDb(-18.0f, -30.4f), -30.4f, tr), "Voice is 12.4 dB over Game"), "dB wording unchanged");
}

int main()
{
	testLufsMode();
	testInstantFill();
	testRowKind();
	testPeakHold();
	testSmoother();
	testFastRate();
	testHandoff();
	testFormat();
	testGap();
	std::printf("%d checks, %d failed\n", total, failures);
	return failures == 0 ? 0 : 1;
}
