/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Tests for loudness.hpp: K-weighting, momentary / short-term loudness, true peak, against EBU Tech 3341
// (v4, November 2023) minimum-requirement signals synthesized in code (no wav files). Pure std, no OBS, no Qt.
// No framework: exits non-zero on failure. "info:" lines are measurements, not checks.
#include "loudness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
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

// Allocation counter: proves feed() never allocates.
static size_t g_allocs = 0;
void *operator new(std::size_t n)
{
	g_allocs++;
	void *p = std::malloc(n ? n : 1);
	if (!p)
		throw std::bad_alloc();
	return p;
}
void operator delete(void *p) noexcept
{
	std::free(p);
}
void operator delete(void *p, std::size_t) noexcept
{
	std::free(p);
}

static const double kPi = 3.14159265358979323846;
typedef std::vector<std::vector<float>> Planes;

static bool isNegInf(double v)
{
	return std::isinf(v) && v < 0;
}

static double dbToLin(double db)
{
	return std::pow(10.0, db / 20.0);
}

static size_t framesOf(double fs, int ms)
{
	return static_cast<size_t>(std::llround(fs * ms / 1000.0));
}

// 1 kHz (or freq) sine of peak level db dBFS; the phase follows the absolute sample index, so a tone cut
// into bursts by silence stays phase-continuous. Written into every plane of `p`, or only plane `only`.
static void appendTone(Planes &p, double fs, double freq, double db, size_t n, int only = -1)
{
	for (size_t c = 0; c < p.size(); c++) {
		std::vector<float> &v = p[c];
		const size_t start = v.size();
		v.resize(start + n, 0.0f);
		if (only >= 0 && static_cast<size_t>(only) != c)
			continue;
		const double amp = dbToLin(db);
		for (size_t i = 0; i < n; i++)
			v[start + i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(start + i) / fs));
	}
}

static void appendSilence(Planes &p, size_t n)
{
	for (size_t c = 0; c < p.size(); c++)
		p[c].resize(p[c].size() + n, 0.0f);
}

static const ChannelRole kStereo[2] = {ChannelRole::Left, ChannelRole::Right};

static LoudnessMeter makeMeter(double fs, const ChannelRole *roles, int channels, int hopMs = 100)
{
	LoudnessMeter m;
	if (!m.configure(fs, roles, channels, hopMs)) {
		std::printf("FAIL: configure(%g, %d, %d) refused\n", fs, channels, hopMs);
		failures++;
	}
	return m;
}

// Feeds `p` in chunks; onChunk(framesFedSoFar) runs after each one.
template <class F> static void feedAll(LoudnessMeter &m, const Planes &p, size_t chunk, F onChunk)
{
	std::vector<const float *> ptr(p.size());
	const size_t totalFrames = p[0].size();
	for (size_t pos = 0; pos < totalFrames; pos += chunk) {
		const size_t n = std::min(chunk, totalFrames - pos);
		for (size_t c = 0; c < p.size(); c++)
			ptr[c] = p[c].data() + pos;
		m.feed(ptr.data(), static_cast<int>(n));
		onChunk(pos + n);
	}
}

// BS.1770 published 48 kHz coefficients (Table 1 pre-filter, Table 2 RLB filter).
static void testCoefficients()
{
	const KWeightCoeffs k = kWeightCoeffs(48000.0);
	const double got[] = {k.shelf.b0, k.shelf.b1, k.shelf.b2, k.shelf.a1, k.shelf.a2,
			      k.highpass.b0, k.highpass.b1, k.highpass.b2, k.highpass.a1, k.highpass.a2};
	const double pub[] = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
			      0.73248077421585, 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
	double worst = 0.0;
	for (int i = 0; i < 10; i++)
		worst = std::max(worst, std::fabs(got[i] - pub[i]) / std::fabs(pub[i]));
	std::printf("info:  worst relative error of derived vs published 48 kHz coefficients: %.3g\n", worst);
	CHECK(worst <= 1e-6, "K-weighting coefficients at 48 kHz match BS.1770 published values (<= 1e-6 relative)");
	const double g1k = kWeightGainDb(k, 48000.0, 1000.0);
	std::printf("info:  K-weighting gain at 1 kHz, 48 kHz: %.4f dB\n", g1k);
	CHECK(std::fabs(g1k - 0.691) < 0.02, "K-weighting gain at 1 kHz is about +0.691 dB");
	const KWeightCoeffs k44 = kWeightCoeffs(44100.0);
	CHECK(std::fabs(kWeightGainDb(k44, 44100.0, 1000.0) - g1k) < 0.01, "K-weighting gain at 1 kHz is rate independent (44.1 kHz)");
	const KWeightCoeffs k96 = kWeightCoeffs(96000.0);
	std::printf("info:  K-weighting gain at 1 kHz: 44.1 kHz %.4f, 96 kHz %.4f dB\n", kWeightGainDb(k44, 44100.0, 1000.0),
		    kWeightGainDb(k96, 96000.0, 1000.0));
	CHECK(std::fabs(kWeightGainDb(k96, 96000.0, 1000.0) - g1k) < 0.02, "K-weighting gain at 1 kHz is rate independent (96 kHz, +-0.02 dB)");
}

// Steady stereo tone: every ready reading must sit within tol of `expect`, final ones included.
static void testSteadyTone(const char *name, double fs, double db, double expect, int seconds)
{
	Planes p(2);
	appendTone(p, fs, 1000.0, db, framesOf(fs, seconds * 1000));
	LoudnessMeter m = makeMeter(fs, kStereo, 2);
	double worstM = 0.0, worstS = 0.0;
	int readM = 0, readS = 0;
	feedAll(m, p, framesOf(fs, 100), [&](size_t) {
		const LoudnessReading a = m.momentary(), b = m.shortTerm();
		if (a.ready) {
			worstM = std::max(worstM, std::fabs(a.lufs - expect));
			readM++;
		}
		if (b.ready) {
			worstS = std::max(worstS, std::fabs(b.lufs - expect));
			readS++;
		}
	});
	std::printf("info:  %s: M %.4f (worst dev %.4f over %d readings), S %.4f (worst dev %.4f over %d)\n", name,
		    m.momentary().lufs, worstM, readM, m.shortTerm().lufs, worstS, readS);
	char msg[160];
	std::snprintf(msg, sizeof msg, "%s: M = %.1f LUFS +-0.1 at every reading", name, expect);
	CHECK(readM > 0 && worstM <= 0.1, msg);
	std::snprintf(msg, sizeof msg, "%s: S = %.1f LUFS +-0.1 at every reading", name, expect);
	CHECK(readS > 0 && worstS <= 0.1, msg);
}

// Tech 3341 v4 cases 9 and 12: a two-level tone pattern whose period equals the window, so the reading is
// constant (-23.0) once the window is full. loMs/hiMs in ms. fromMs: readings before it are not checked.
static void testTonePattern(const char *name, bool shortTerm, int aMs, double aDb, int bMs, double bDb, int repeats,
			    int fromMs)
{
	const double fs = 48000.0;
	Planes p(2);
	for (int r = 0; r < repeats; r++) {
		appendTone(p, fs, 1000.0, aDb, framesOf(fs, aMs));
		appendTone(p, fs, 1000.0, bDb, framesOf(fs, bMs));
	}
	LoudnessMeter m = makeMeter(fs, kStereo, 2);
	double worst = 0.0;
	int n = 0;
	feedAll(m, p, framesOf(fs, 100), [&](size_t fed) {
		const LoudnessReading r = shortTerm ? m.shortTerm() : m.momentary();
		if (r.ready && fed >= framesOf(fs, fromMs)) {
			worst = std::max(worst, std::fabs(r.lufs + 23.0));
			n++;
		}
	});
	std::printf("info:  %s: worst deviation from -23.0 over %d readings: %.4f LU\n", name, n, worst);
	char msg[200];
	std::snprintf(msg, sizeof msg, "%s: %s = -23.0 LUFS +-0.1, constant after the window fills", name,
		      shortTerm ? "S" : "M");
	CHECK(n > 0 && worst <= 0.1, msg);
}

// Tech 3341 v4 cases 10 and 13 (file based: a tone burst after i x stepMs of silence, one file per i, meter
// reset between files) -> the maximum reading must be -23.0 +-0.1. hopMs sets the meter's update interval.
static double burstFilesWorst(bool shortTerm, int toneMs, int stepMs, int hopMs)
{
	const double fs = 48000.0;
	LoudnessMeter m = makeMeter(fs, kStereo, 2, hopMs);
	double worst = 0.0;
	for (int i = 0; i < 20; i++) {
		Planes p(2);
		appendSilence(p, framesOf(fs, i * stepMs));
		appendTone(p, fs, 1000.0, -23.0, framesOf(fs, toneMs));
		appendSilence(p, framesOf(fs, 1000));
		m.reset();
		double mx = loudnessMinusInf();
		feedAll(m, p, framesOf(fs, hopMs), [&](size_t) {
			const LoudnessReading r = shortTerm ? m.shortTerm() : m.momentary();
			if (r.ready)
				mx = std::max(mx, r.lufs);
		});
		worst = std::max(worst, std::fabs(mx + 23.0));
	}
	return worst;
}

// Tech 3341 v4 cases 11 and 14 (live: one file, 20 segments; segment i = i x stepMs silence, a tone of toneMs
// at (-38 + i) dBFS, then silence up to segMs). The maximum reading inside segment i must be -38 + i +-0.1.
static double liveSequenceWorst(bool shortTerm, int toneMs, int stepMs, int segMs, int hopMs)
{
	const double fs = 48000.0;
	Planes p(2);
	for (int i = 0; i < 20; i++) {
		appendSilence(p, framesOf(fs, i * stepMs));
		appendTone(p, fs, 1000.0, -38.0 + i, framesOf(fs, toneMs));
		appendSilence(p, framesOf(fs, segMs - i * stepMs - toneMs));
	}
	LoudnessMeter m = makeMeter(fs, kStereo, 2, hopMs);
	std::vector<double> segMax(20, loudnessMinusInf());
	size_t hop = 0;
	feedAll(m, p, framesOf(fs, hopMs), [&](size_t) {
		hop++;
		const LoudnessReading r = shortTerm ? m.shortTerm() : m.momentary();
		const size_t seg = (hop - 1) * static_cast<size_t>(hopMs) / static_cast<size_t>(segMs);
		if (r.ready && seg < 20)
			segMax[seg] = std::max(segMax[seg], r.lufs);
	});
	double worst = 0.0;
	for (int i = 0; i < 20; i++)
		worst = std::max(worst, std::fabs(segMax[static_cast<size_t>(i)] - (-38.0 + i)));
	return worst;
}

static void testBurstsAndSequences()
{
	double w = burstFilesWorst(true, 3000, 150, 100);
	std::printf("info:  Tech 3341 v4 case 10: worst |max S + 23.0| over 20 files: %.4f LU\n", w);
	CHECK(w <= 0.1, "Tech 3341 v4 case 10: max S = -23.0 LUFS +-0.1 for every start offset (100 ms hop)");
	w = burstFilesWorst(false, 400, 20, 10);
	std::printf("info:  Tech 3341 v4 case 13 (10 ms hop): worst |max M + 23.0|: %.4f LU\n", w);
	CHECK(w <= 0.1, "Tech 3341 v4 case 13: max M = -23.0 LUFS +-0.1 for every start offset (10 ms hop)");
	w = burstFilesWorst(false, 400, 20, 100);
	std::printf("info:  Tech 3341 v4 case 13 with the panel's 100 ms hop: worst |max M + 23.0|: %.4f LU "
		    "(not a check: bursts off the 100 ms grid read low)\n", w);
	w = liveSequenceWorst(true, 3000, 150, 6000, 100);
	std::printf("info:  Tech 3341 v4 case 11: worst |max S - (-38+i)|: %.4f LU\n", w);
	CHECK(w <= 0.1, "Tech 3341 v4 case 11: successive max S = -38.0 ... -19.0 LUFS +-0.1 (100 ms hop)");
	w = liveSequenceWorst(false, 400, 20, 800, 10);
	std::printf("info:  Tech 3341 v4 case 14 (10 ms hop): worst |max M - (-38+i)|: %.4f LU\n", w);
	CHECK(w <= 0.1, "Tech 3341 v4 case 14: successive max M = -38.0 ... -19.0 LUFS +-0.1 (10 ms hop)");
}

static void testRolesAndWeights()
{
	ChannelRole r[kLoudnessMaxChannels];
	CHECK(channelRolesForObsLayout(2, r) == 2 && r[0] == ChannelRole::Left && r[1] == ChannelRole::Right,
	      "OBS stereo = L R");
	CHECK(channelRolesForObsLayout(1, r) == 1 && r[0] == ChannelRole::Center, "OBS mono = Center");
	CHECK(channelRolesForObsLayout(3, r) == 3 && r[2] == ChannelRole::Lfe, "OBS 2.1 = L R LFE");
	CHECK(channelRolesForObsLayout(4, r) == 4 && r[2] == ChannelRole::Center && r[3] == ChannelRole::LeftSurround,
	      "OBS 4.0 = L R C RC(surround)");
	CHECK(channelRolesForObsLayout(5, r) == 5 && r[3] == ChannelRole::Lfe && r[4] == ChannelRole::LeftSurround,
	      "OBS 4.1 = L R C LFE RC(surround)");
	CHECK(channelRolesForObsLayout(6, r) == 6 && r[3] == ChannelRole::Lfe && r[4] == ChannelRole::LeftSurround &&
		      r[5] == ChannelRole::RightSurround,
	      "OBS 5.1 = L R C LFE Ls Rs");
	CHECK(channelRolesForObsLayout(8, r) == 8 && r[3] == ChannelRole::Lfe && r[6] == ChannelRole::LeftSurround &&
		      r[7] == ChannelRole::RightSurround,
	      "OBS 7.1 = L R C LFE Ls Rs Lss Rss");
	CHECK(channelRolesForObsLayout(7, r) == 0 && channelRolesForObsLayout(0, r) == 0 &&
		      channelRolesForObsLayout(9, r) == 0,
	      "no OBS layout with 0, 7 or 9 channels");
	CHECK(channelWeight(ChannelRole::Left) == 1.0 && channelWeight(ChannelRole::Center) == 1.0 &&
		      channelWeight(ChannelRole::Other) == 1.0 && channelWeight(ChannelRole::LeftSurround) == 1.41 &&
		      channelWeight(ChannelRole::RightSurround) == 1.41 && channelWeight(ChannelRole::Lfe) == 0.0,
	      "weights: L R C Other 1.0, Ls Rs 1.41, LFE 0");
}

// 5.1 (OBS order): -23 dBFS 1 kHz in one channel only. Formula, with A the peak amplitude, G the channel
// weight and H the K-weighting power gain at 1 kHz (read from the filter's own response):
//   LUFS = -0.691 + 10 log10( G * (A^2 / 2) * H )
// Ls: G = 1.41 -> about -24.52 LUFS (the stereo -23 case is -0.691 + 10 log10(2 * A^2/2 * H) = -23.0).
static void testSurroundWeighting()
{
	const double fs = 48000.0;
	ChannelRole roles[kLoudnessMaxChannels];
	channelRolesForObsLayout(6, roles);
	const double h = std::pow(10.0, kWeightGainDb(kWeightCoeffs(fs), fs, 1000.0) / 10.0);
	const double a = dbToLin(-23.0);
	struct Case { const char *name; int ch; double g; };
	const Case cases[] = {{"Ls", 4, 1.41}, {"C", 2, 1.0}, {"L", 0, 1.0}};
	for (const Case &c : cases) {
		Planes p(6);
		appendTone(p, fs, 1000.0, -23.0, framesOf(fs, 5000), c.ch);
		LoudnessMeter m = makeMeter(fs, roles, 6);
		feedAll(m, p, framesOf(fs, 100), [](size_t) {});
		const double expect = -0.691 + 10.0 * std::log10(c.g * (a * a / 2.0) * h);
		std::printf("info:  5.1 %s only: M %.4f S %.4f expected %.4f\n", c.name, m.momentary().lufs,
			    m.shortTerm().lufs, expect);
		char msg[120];
		std::snprintf(msg, sizeof msg, "5.1 -23 dBFS in %s only reads the weighted value (+-0.02), M and S", c.name);
		CHECK(std::fabs(m.momentary().lufs - expect) < 0.02 && std::fabs(m.shortTerm().lufs - expect) < 0.02, msg);
		if (c.ch == 4)
			CHECK(std::fabs(expect + 24.518) < 0.03, "Ls-only expectation is about -24.52 LUFS (-23 + 10 log10(1.41/2))");
	}
	Planes p(6);
	appendTone(p, fs, 1000.0, -3.0, framesOf(fs, 5000), 3);
	LoudnessMeter m = makeMeter(fs, roles, 6);
	feedAll(m, p, framesOf(fs, 100), [](size_t) {});
	CHECK(m.momentary().ready && isNegInf(m.momentary().lufs) && m.shortTerm().ready && isNegInf(m.shortTerm().lufs),
	      "5.1 LFE-only (even at -3 dBFS) reads -inf (excluded)");
}

static void testSilenceAndReadiness()
{
	const double fs = 48000.0;
	const size_t hop = framesOf(fs, 100);
	Planes z(2);
	appendSilence(z, hop * 40);
	LoudnessMeter m = makeMeter(fs, kStereo, 2);
	CHECK(!m.momentary().ready && !m.shortTerm().ready, "nothing is ready before any audio");
	std::vector<const float *> ptr = {z[0].data(), z[1].data()};
	m.feed(ptr.data(), static_cast<int>(hop - 1));
	m.feed(ptr.data(), 1);
	CHECK(m.hopsCompleted() == 1, "a hop completes exactly at 100 ms");
	for (int k = 2; k <= 3; k++)
		m.feed(ptr.data(), static_cast<int>(hop));
	CHECK(!m.momentary().ready, "M not ready at 300 ms");
	m.feed(ptr.data(), static_cast<int>(hop));
	CHECK(m.momentary().ready, "M ready at 400 ms");
	for (int k = 5; k <= 29; k++)
		m.feed(ptr.data(), static_cast<int>(hop));
	CHECK(!m.shortTerm().ready, "S not ready at 2.9 s");
	m.feed(ptr.data(), static_cast<int>(hop));
	CHECK(m.shortTerm().ready, "S ready at 3.0 s");
	CHECK(isNegInf(m.momentary().lufs) && isNegInf(m.shortTerm().lufs), "digital silence reads -inf (M and S)");
	m.reset();
	CHECK(!m.momentary().ready && !m.shortTerm().ready && m.hopsCompleted() == 0, "reset() clears readiness");
	// tone, then silence: back to -inf once the window has passed the tone (no -300 LUFS tails)
	Planes p(2);
	appendTone(p, fs, 1000.0, -20.0, framesOf(fs, 1000));
	appendSilence(p, framesOf(fs, 4000));
	LoudnessMeter t = makeMeter(fs, kStereo, 2);
	double mAt15 = 0.0, sAt14 = 0.0, sAt45 = 0.0;
	bool mInf = false, sInf = false;
	feedAll(t, p, hop, [&](size_t fed) {
		if (fed == framesOf(fs, 1400))
			sAt14 = t.shortTerm().ready ? t.shortTerm().lufs : 0.0;
		if (fed == framesOf(fs, 1500)) {
			mAt15 = t.momentary().lufs;
			mInf = isNegInf(mAt15);
		}
		if (fed == framesOf(fs, 4500))
			sInf = isNegInf(t.shortTerm().lufs);
	});
	sAt45 = t.shortTerm().lufs;
	std::printf("info:  after 1 s tone: S at 1.4 s %.2f (not ready -> 0), M at 1.5 s %.2f, S at 5.0 s %.2f\n", sAt14,
		    mAt15, sAt45);
	CHECK(mInf, "M is -inf 0.5 s after a tone stops (no tail numbers)");
	CHECK(sInf, "S is -inf 3.5 s after a tone stops");
	CHECK(std::isinf(sAt45) && sAt45 < 0, "S stays -inf");
}

static void testInvariance()
{
	const double fs = 48000.0;
	Planes p(2);
	appendTone(p, fs, 1000.0, -18.0, framesOf(fs, 700));
	appendTone(p, fs, 440.0, -30.0, framesOf(fs, 4000));
	const size_t chunks[] = {1, 7, 333, 4799, 4800, 100000};
	double ref = 0.0, refS = 0.0;
	bool same = true;
	for (size_t ch : chunks) {
		LoudnessMeter m = makeMeter(fs, kStereo, 2);
		feedAll(m, p, ch, [](size_t) {});
		if (ch == 1) {
			ref = m.momentary().lufs;
			refS = m.shortTerm().lufs;
		} else if (m.momentary().lufs != ref || m.shortTerm().lufs != refS) {
			same = false;
		}
	}
	CHECK(same, "readings are bit-identical for any feed chunk size");
	LoudnessMeter bad;
	CHECK(!bad.configure(48000.0, nullptr, 2) && !bad.configure(48000.0, kStereo, 0) &&
		      !bad.configure(48000.0, kStereo, 9) && !bad.configure(1000.0, kStereo, 2) &&
		      !bad.configure(48000.0, kStereo, 2, 30) && !bad.configure(48000.0, kStereo, 2, 0),
	      "configure rejects null roles, 0 / 9 channels, 1 kHz, hops that do not divide 400 / 3000");
	const float nan = std::nanf("");
	Planes q(2);
	appendTone(q, fs, 1000.0, -20.0, 4800 * 40, 1);
	q[0].assign(q[0].size(), nan);
	LoudnessMeter n = makeMeter(fs, kStereo, 2);
	feedAll(n, q, 4800, [](size_t) {});
	CHECK(std::isfinite(n.shortTerm().lufs) && std::fabs(n.shortTerm().lufs + 23.0) < 0.1,
	      "a NaN input channel does not poison the meter (the other channel still reads)");
}

static double truePeakOf(double fs, const Planes &p, double *sampleDb = nullptr, int *factor = nullptr)
{
	TruePeakMeter tp;
	if (!tp.configure(fs, static_cast<int>(p.size()))) {
		std::printf("FAIL: TruePeakMeter::configure refused\n");
		failures++;
		return 0.0;
	}
	std::vector<const float *> ptr(p.size());
	const size_t totalFrames = p[0].size();
	const size_t chunk = 1000;
	for (size_t pos = 0; pos < totalFrames; pos += chunk) {
		const size_t n = std::min(chunk, totalFrames - pos);
		for (size_t c = 0; c < p.size(); c++)
			ptr[c] = p[c].data() + pos;
		tp.feed(ptr.data(), static_cast<int>(n));
	}
	if (sampleDb)
		*sampleDb = tp.maxSampleDb();
	if (factor)
		*factor = tp.oversampling();
	return tp.maxDbtp();
}

// Stereo sine at fs/divisor, amplitude amp (fraction of full scale), start phase in degrees, 1 s, with an
// optional 10 ms raised-cosine fade in and out (Tech 3341 v4 cases 15-19; the fade shape is my choice, the
// document gives only the 10 ms).
static Planes sineAt(double fs, double divisor, double amp, double phaseDeg, bool fade)
{
	const size_t n = framesOf(fs, 1000), nf = framesOf(fs, 10);
	Planes p(2, std::vector<float>(n));
	for (size_t i = 0; i < n; i++) {
		double g = 1.0;
		if (fade && i < nf)
			g = 0.5 * (1.0 - std::cos(kPi * static_cast<double>(i) / static_cast<double>(nf)));
		if (fade && i >= n - nf)
			g = 0.5 * (1.0 - std::cos(kPi * static_cast<double>(n - 1 - i) / static_cast<double>(nf)));
		const float v = static_cast<float>(g * amp *
			std::sin(2.0 * kPi * static_cast<double>(i) / divisor + phaseDeg * kPi / 180.0));
		p[0][i] = v;
		p[1][i] = v;
	}
	return p;
}

static void checkTp(const char *name, double tp, double expect)
{
	std::printf("info:  %s: true peak %.4f dBTP (expected %.1f, window +0.2/-0.4)\n", name, tp, expect);
	char msg[160];
	std::snprintf(msg, sizeof msg, "%s: true peak %.1f dBTP (+0.2/-0.4)", name, expect);
	CHECK(tp <= expect + 0.2 && tp >= expect - 0.4, msg);
}

// Tech 3341 v4 cases 20-23: fs/6 sine at 0.5 FFS with one fs/4 period (amplitude 1.0) inserted, phase
// continuous, made at 4 x fs, low-passed, and decimated by 4 with sample offset 0..3. The document does not
// say which low-pass; this one is a 401-tap Blackman-windowed sinc, cutoff 21 kHz at the 192 kHz rate.
static Planes insertedPeak(int offset)
{
	const size_t lead = 48000;
	const size_t period = 16; // one fs/4 period at 4 x fs
	std::vector<double> x;
	double phase = 0.0;
	for (size_t i = 0; i < lead + period + lead; i++) {
		const bool burst = i >= lead && i < lead + period;
		x.push_back((burst ? 1.0 : 0.5) * std::sin(phase));
		phase += 2.0 * kPi / (burst ? 16.0 : 24.0);
	}
	const int m = 200;
	std::vector<double> h(2 * m + 1);
	double sum = 0.0;
	for (int k = -m; k <= m; k++) {
		const double fc = 21000.0 / 192000.0;
		const double s = k == 0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * k) / (kPi * k);
		const double w = 0.42 + 0.5 * std::cos(kPi * k / (m + 1)) + 0.08 * std::cos(2.0 * kPi * k / (m + 1));
		h[static_cast<size_t>(k + m)] = s * w;
		sum += s * w;
	}
	std::vector<float> out;
	for (size_t i = static_cast<size_t>(offset); i < x.size(); i += 4) {
		double acc = 0.0;
		for (int k = -m; k <= m; k++) {
			const long long j = static_cast<long long>(i) + k;
			if (j >= 0 && j < static_cast<long long>(x.size()))
				acc += h[static_cast<size_t>(k + m)] * x[static_cast<size_t>(j)];
		}
		out.push_back(static_cast<float>(acc / sum));
	}
	return Planes(2, out);
}

static void testTruePeak()
{
	double sampleDb = 0.0;
	int factor = 0;
	Planes p = sineAt(48000.0, 4.0, 1.0, 45.0, false);
	double tp = truePeakOf(48000.0, p, &sampleDb, &factor);
	std::printf("info:  fs/4, 0 dBFS true peak, 45 deg: sample peak %.4f dB, true peak %.4f dBTP, x%d\n", sampleDb, tp, factor);
	CHECK(std::fabs(sampleDb + 3.0103) < 0.01, "fs/4 45-degree sine: sample peak is -3.01 dB (+-0.7071)");
	CHECK(tp <= 0.2 && tp >= -0.4, "fs/4 45-degree sine: true peak within +0.2/-0.4 dB of 0.0 dBTP");
	CHECK(factor == 4, "48 kHz oversamples x4");
	const struct { const char *name; double div, amp, ph, expect; } c1519[] = {
		{"Tech 3341 v4 case 15 (fs/4, 0.50, 0 deg)", 4.0, 0.5, 0.0, -6.0},
		{"Tech 3341 v4 case 16 (fs/4, 0.50, 45 deg)", 4.0, 0.5, 45.0, -6.0},
		{"Tech 3341 v4 case 17 (fs/6, 0.50, 60 deg)", 6.0, 0.5, 60.0, -6.0},
		{"Tech 3341 v4 case 18 (fs/8, 0.50, 67.5 deg)", 8.0, 0.5, 67.5, -6.0},
		{"Tech 3341 v4 case 19 (fs/4, 1.41, 45 deg)", 4.0, 1.41, 45.0, 3.0}};
	for (const auto &c : c1519)
		checkTp(c.name, truePeakOf(48000.0, sineAt(48000.0, c.div, c.amp, c.ph, true)), c.expect);
	for (int o = 0; o < 4; o++) {
		char name[96];
		std::snprintf(name, sizeof name, "Tech 3341 v4 case %d (inserted fs/4 period, offset %d)", 20 + o, o);
		checkTp(name, truePeakOf(48000.0, insertedPeak(o)), 0.0);
	}
	checkTp("fs/4 0.50 45 deg at 44.1 kHz (x4)", truePeakOf(44100.0, sineAt(44100.0, 4.0, 0.5, 45.0, true)), -6.0);
	checkTp("fs/4 0.50 45 deg at 96 kHz (x2)", truePeakOf(96000.0, sineAt(96000.0, 4.0, 0.5, 45.0, true), nullptr, &factor), -6.0);
	CHECK(factor == 2, "96 kHz oversamples x2");
	truePeakOf(192000.0, sineAt(192000.0, 4.0, 0.5, 45.0, true), nullptr, &factor);
	CHECK(factor == 1, "192 kHz: no oversampling (sample peak)");
	Planes z(2, std::vector<float>(48000, 0.0f));
	CHECK(isNegInf(truePeakOf(48000.0, z)), "true peak of silence is -inf");
}

static void testTruePeakBlocks()
{
	const double fs = 48000.0;
	const size_t hop = framesOf(fs, 100);
	TruePeakMeter tp;
	tp.configure(fs, 2, 100);
	CHECK(!tp.blockReady() && isNegInf(tp.lastBlockDbtp()) && isNegInf(tp.maxDbtp()), "fresh true-peak meter: no block, -inf");
	Planes p(2);
	appendTone(p, fs, 1000.0, -6.0, hop * 3);
	appendSilence(p, hop * 3);
	std::vector<const float *> ptr = {p[0].data(), p[1].data()};
	tp.feed(ptr.data(), static_cast<int>(hop * 2));
	CHECK(tp.blocksCompleted() == 2 && std::fabs(tp.lastBlockDbtp() + 6.0) < 0.2, "100 ms block peak of a -6 dBFS tone is about -6 dBTP");
	std::vector<const float *> ptr2 = {p[0].data() + hop * 3, p[1].data() + hop * 3};
	tp.feed(ptr2.data(), static_cast<int>(hop * 3));
	CHECK(tp.blocksCompleted() == 5 && isNegInf(tp.lastBlockDbtp()), "a silent block after a tone reads -inf (filter tail is under the first 100 ms)");
	CHECK(std::fabs(tp.maxDbtp() + 6.0) < 0.2, "running max keeps the tone's peak");
	tp.reset();
	CHECK(tp.blocksCompleted() == 0 && isNegInf(tp.maxDbtp()), "reset() clears the running max");
}

// Item 3 of the revision: a transient in the first of several blocks fed in ONE call must reach the reported max
// (lastBlockDbtp() would show only the final, silent block).
static void testTakeBlocks()
{
	const double fs = 48000.0;
	const size_t hop = framesOf(fs, 100);
	TruePeakMeter tp;
	tp.configure(fs, 2, 100);
	Planes p(2);
	appendSilence(p, hop * 4);
	for (int c = 0; c < 2; c++) // a -6 dBFS click in the middle of block 1 (of 4)
		p[static_cast<size_t>(c)][hop / 2] = 0.5f;
	std::vector<const float *> ptr = {p[0].data(), p[1].data()};
	tp.feed(ptr.data(), static_cast<int>(hop * 4));
	CHECK(isNegInf(tp.lastBlockDbtp()), "after 4 blocks in one call the LAST block is silent");
	double mx = 0.0;
	const long long n = tp.takeBlocks(&mx);
	CHECK(n == 4 && std::fabs(mx + 6.0) < 0.5, "takeBlocks: 4 blocks completed, max over them includes the transient in block 1");
	CHECK(tp.takeBlocks(&mx) == 0 && isNegInf(mx), "takeBlocks clears: nothing new, -inf");
	tp.feed(ptr.data(), static_cast<int>(hop));
	CHECK(tp.takeBlocks(&mx) == 1, "takeBlocks counts only blocks completed since the last take");
	// The live hop rule: whole frames only (400 ms / 3 s windows exact).
	const ChannelRole st[2] = {ChannelRole::Left, ChannelRole::Right};
	LoudnessMeter m;
	CHECK(m.configure(44100.0, st, 2, 10) && m.configure(44100.0, st, 2, 100) && m.configure(48000.0, st, 2, 10) &&
		      m.configure(48000.0, st, 2, 100),
	      "hop accepted at 44.1 kHz and 48 kHz for 10 ms and 100 ms (whole frames)");
	CHECK(!m.configure(22050.0, st, 2, 10) && !m.configure(44100.0, st, 2, 1) && !m.configure(44100.5, st, 2, 100),
	      "hop rejected when fs x hop is not a whole number of frames (22.05 kHz/10 ms, 44.1 kHz/1 ms, 44100.5 Hz)");
}

static void testNoAllocation()
{
	const double fs = 48000.0;
	Planes p(2);
	appendTone(p, fs, 1000.0, -20.0, framesOf(fs, 1000));
	std::vector<const float *> ptr = {p[0].data(), p[1].data()};
	LoudnessMeter m = makeMeter(fs, kStereo, 2);
	TruePeakMeter tp;
	tp.configure(fs, 2);
	const size_t before = g_allocs;
	for (int i = 0; i < 5; i++) {
		m.feed(ptr.data(), 48000);
		tp.feed(ptr.data(), 48000);
	}
	(void)m.momentary();
	(void)m.shortTerm();
	m.reset();
	tp.reset();
	CHECK(g_allocs == before, "feed / reset / readers allocate nothing");
}

// Cost: stereo 48 kHz, 10 s of noise-like audio, best of 5 runs, in ns per frame per channel.
static void reportCost()
{
	const double fs = 48000.0;
	const size_t n = framesOf(fs, 10000);
	Planes p(2, std::vector<float>(n));
	unsigned s = 12345u;
	for (size_t i = 0; i < n; i++)
		for (int c = 0; c < 2; c++) {
			s = s * 1664525u + 1013904223u;
			p[static_cast<size_t>(c)][i] = (static_cast<float>(s >> 8) / 16777216.0f - 0.5f) * 0.5f;
		}
	std::vector<const float *> ptr = {p[0].data(), p[1].data()};
	LoudnessMeter m = makeMeter(fs, kStereo, 2);
	TruePeakMeter tp;
	tp.configure(fs, 2);
	double bestK = 1e300, bestT = 1e300, sink = 0.0;
	for (int rep = 0; rep < 5; rep++) {
		m.reset();
		auto t0 = std::chrono::steady_clock::now();
		for (size_t pos = 0; pos < n; pos += 4800) {
			const float *q[2] = {ptr[0] + pos, ptr[1] + pos};
			m.feed(q, static_cast<int>(std::min<size_t>(4800, n - pos)));
		}
		auto t1 = std::chrono::steady_clock::now();
		sink += m.shortTerm().lufs;
		tp.reset();
		for (size_t pos = 0; pos < n; pos += 4800) {
			const float *q[2] = {ptr[0] + pos, ptr[1] + pos};
			tp.feed(q, static_cast<int>(std::min<size_t>(4800, n - pos)));
		}
		auto t2 = std::chrono::steady_clock::now();
		sink += tp.maxDbtp();
		bestK = std::min(bestK, std::chrono::duration<double, std::nano>(t1 - t0).count());
		bestT = std::min(bestT, std::chrono::duration<double, std::nano>(t2 - t1).count());
	}
	const double denom = static_cast<double>(n) * 2.0;
	std::printf("COST:  stereo 48 kHz, 10 s: K-weighting + accumulate %.2f ns/frame/channel, true peak (x4) %.2f ns/frame/channel (sink %.1f)\n",
		    bestK / denom, bestT / denom, sink);
}

int main()
{
	testCoefficients();
	testSteadyTone("Tech 3341 v4 case 1 (-23.0 dBFS, 48 kHz)", 48000.0, -23.0, -23.0, 20);
	testSteadyTone("Tech 3341 v4 case 2 (-33.0 dBFS, 48 kHz)", 48000.0, -33.0, -33.0, 20);
	testSteadyTone("Tech 3341 v4 case 1 at 44.1 kHz", 44100.0, -23.0, -23.0, 20);
	testSteadyTone("Tech 3341 v4 case 1 at 96 kHz", 96000.0, -23.0, -23.0, 20);
	testTonePattern("Tech 3341 v4 case 9", true, 1340, -20.0, 1660, -30.0, 5, 3000);
	testTonePattern("Tech 3341 v4 case 12", false, 180, -20.0, 220, -30.0, 25, 1000);
	testBurstsAndSequences();
	testRolesAndWeights();
	testSurroundWeighting();
	testSilenceAndReadiness();
	testInvariance();
	testTruePeak();
	testTruePeakBlocks();
	testTakeBlocks();
	testNoAllocation();
	reportCost();
	std::printf("\n%d checks, %d failures\n", total, failures);
	return failures ? 1 : 0;
}
