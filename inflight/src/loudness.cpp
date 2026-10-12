/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "loudness.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace inflight {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLufsOffset = -0.691;
constexpr double kSurroundWeight = 1.41; // BS.1770: +1.5 dB
constexpr double kSilenceFloor = 1e-14;  // weighted mean square under which a hop counts as exact silence
constexpr double kStateFlush = 1e-30;    // filter state under this is zeroed at hop ends (no denormals)
constexpr int kTruePeakTaps = 49;

double lufsFromPower(double power)
{
	return power > 0.0 ? kLufsOffset + 10.0 * std::log10(power) : loudnessMinusInf();
}

double linToDb(double lin)
{
	return lin > 0.0 ? 20.0 * std::log10(lin) : loudnessMinusInf();
}

} // namespace

double channelWeight(ChannelRole role)
{
	switch (role) {
	case ChannelRole::Lfe:
		return 0.0;
	case ChannelRole::LeftSurround:
	case ChannelRole::RightSurround:
		return kSurroundWeight;
	default:
		return 1.0;
	}
}

int channelRolesForObsLayout(int channels, ChannelRole *out)
{
	using R = ChannelRole;
	static const R l1[] = {R::Center};
	static const R l2[] = {R::Left, R::Right};
	static const R l3[] = {R::Left, R::Right, R::Lfe};
	static const R l4[] = {R::Left, R::Right, R::Center, R::LeftSurround};
	static const R l5[] = {R::Left, R::Right, R::Center, R::Lfe, R::LeftSurround};
	static const R l6[] = {R::Left, R::Right, R::Center, R::Lfe, R::LeftSurround, R::RightSurround};
	static const R l8[] = {R::Left,         R::Right,         R::Center,       R::Lfe,
			       R::LeftSurround, R::RightSurround, R::LeftSurround, R::RightSurround};
	const R *src = nullptr;
	switch (channels) {
	case 1: src = l1; break;
	case 2: src = l2; break;
	case 3: src = l3; break;
	case 4: src = l4; break;
	case 5: src = l5; break;
	case 6: src = l6; break;
	case 8: src = l8; break;
	default: return 0;
	}
	if (!out)
		return 0;
	for (int i = 0; i < channels; i++)
		out[i] = src[i];
	return channels;
}

// Bilinear design from the analog prototypes (the parameters libebur128 derives from the BS.1770 48 kHz
// coefficients): shelf f0 = 1681.974450955533 Hz, Q = 0.7071752369554196, gain 3.999843853973347 dB;
// high-pass f0 = 38.13547087602444 Hz, Q = 0.5003270373238773.
KWeightCoeffs kWeightCoeffs(double fs)
{
	KWeightCoeffs c;
	{
		const double f0 = 1681.974450955533;
		const double gainDb = 3.999843853973347;
		const double q = 0.7071752369554196;
		const double k = std::tan(kPi * f0 / fs);
		const double vh = std::pow(10.0, gainDb / 20.0);
		const double vb = std::pow(vh, 0.4996667741545416);
		const double a0 = 1.0 + k / q + k * k;
		c.shelf.b0 = (vh + vb * k / q + k * k) / a0;
		c.shelf.b1 = 2.0 * (k * k - vh) / a0;
		c.shelf.b2 = (vh - vb * k / q + k * k) / a0;
		c.shelf.a1 = 2.0 * (k * k - 1.0) / a0;
		c.shelf.a2 = (1.0 - k / q + k * k) / a0;
	}
	{
		const double f0 = 38.13547087602444;
		const double q = 0.5003270373238773;
		const double k = std::tan(kPi * f0 / fs);
		const double a0 = 1.0 + k / q + k * k;
		c.highpass.b0 = 1.0;
		c.highpass.b1 = -2.0;
		c.highpass.b2 = 1.0;
		c.highpass.a1 = 2.0 * (k * k - 1.0) / a0;
		c.highpass.a2 = (1.0 - k / q + k * k) / a0;
	}
	return c;
}

static double biquadMag(const Biquad &b, double w)
{
	using C = std::complex<double>;
	const C z1 = std::polar(1.0, -w);
	const C z2 = z1 * z1;
	const C num = b.b0 + b.b1 * z1 + b.b2 * z2;
	const C den = 1.0 + b.a1 * z1 + b.a2 * z2;
	return std::abs(num) / std::abs(den);
}

double kWeightGainDb(const KWeightCoeffs &c, double fs, double freqHz)
{
	const double w = 2.0 * kPi * freqHz / fs;
	return 20.0 * std::log10(biquadMag(c.shelf, w) * biquadMag(c.highpass, w));
}

bool LoudnessMeter::configure(double sampleRate, const ChannelRole *roles, int channels, int hopMs)
{
	configured_ = false;
	if (!(sampleRate >= 8000.0 && sampleRate <= 384000.0) || !roles || channels < 1 ||
	    channels > kLoudnessMaxChannels || hopMs < 1 || 400 % hopMs != 0 || 3000 % hopMs != 0)
		return false;
	// The hop must be a whole number of frames, or the 400 ms / 3 s windows (momentaryHops_ x hopFrames_) would
	// not be exact. 44.1 kHz and 48 kHz at 10 ms and 100 ms are; 22.05 kHz at 10 ms (220.5) is rejected.
	const double hopExact = sampleRate * hopMs / 1000.0;
	if (std::fabs(hopExact - std::round(hopExact)) > 1e-6)
		return false;
	const int hopFrames = static_cast<int>(std::lround(hopExact));
	if (hopFrames < 1)
		return false;
	// Note: there is no end-of-input drain (a partial last hop is never evaluated). That is deliberate for a
	// live meter, which never ends; a file analyser would flush it.
	k_ = kWeightCoeffs(sampleRate);
	channels_ = channels;
	hopFrames_ = hopFrames;
	momentaryHops_ = 400 / hopMs;
	shortTermHops_ = 3000 / hopMs;
	ring_.assign(static_cast<size_t>(shortTermHops_), 0.0);
	for (int c = 0; c < kLoudnessMaxChannels; c++) {
		chans_[c] = Chan();
		if (c < channels)
			chans_[c].weight = channelWeight(roles[c]);
	}
	hopFill_ = 0;
	ringPos_ = 0;
	hops_ = 0;
	configured_ = true;
	return true;
}

void LoudnessMeter::reset()
{
	if (!configured_)
		return;
	for (int c = 0; c < channels_; c++) {
		Chan &ch = chans_[c];
		ch.sumSq = ch.s1 = ch.s2 = ch.h1 = ch.h2 = 0.0;
	}
	std::fill(ring_.begin(), ring_.end(), 0.0);
	hopFill_ = 0;
	ringPos_ = 0;
	hops_ = 0;
}

// Runs n samples of one channel through both stages (double precision state, direct form II transposed)
// and returns the updated sum of squares. A null source is silence. LFE channels (weight 0) never get here.
static double runChannel(double sumSq, double &s1, double &s2, double &h1, double &h2, const KWeightCoeffs &k,
			 const float *p, int n)
{
	double t1 = s1, t2 = s2, u1 = h1, u2 = h2, acc = sumSq;
	for (int i = 0; i < n; i++) {
		double x = p ? static_cast<double>(p[i]) : 0.0;
		if (!std::isfinite(x))
			x = 0.0;
		const double y = k.shelf.b0 * x + t1;
		t1 = k.shelf.b1 * x - k.shelf.a1 * y + t2;
		t2 = k.shelf.b2 * x - k.shelf.a2 * y;
		const double z = y + u1; // high-pass b0 = 1, b1 = -2, b2 = 1
		u1 = -2.0 * y - k.highpass.a1 * z + u2;
		u2 = y - k.highpass.a2 * z;
		acc += z * z;
	}
	s1 = t1;
	s2 = t2;
	h1 = u1;
	h2 = u2;
	return acc;
}

void LoudnessMeter::feed(const float *const *planes, int frames)
{
	if (!configured_ || !planes || frames <= 0)
		return;
	int done = 0;
	while (done < frames) {
		const int n = std::min(frames - done, hopFrames_ - hopFill_);
		for (int c = 0; c < channels_; c++) {
			Chan &ch = chans_[c];
			if (ch.weight <= 0.0)
				continue;
			const float *p = planes[c] ? planes[c] + done : nullptr;
			ch.sumSq = runChannel(ch.sumSq, ch.s1, ch.s2, ch.h1, ch.h2, k_, p, n);
		}
		done += n;
		hopFill_ += n;
		if (hopFill_ >= hopFrames_)
			completeHop();
	}
}

void LoudnessMeter::completeHop()
{
	double power = 0.0;
	for (int c = 0; c < channels_; c++) {
		Chan &ch = chans_[c];
		power += ch.weight * (ch.sumSq / hopFrames_);
		ch.sumSq = 0.0;
		if (std::fabs(ch.s1) < kStateFlush)
			ch.s1 = 0.0;
		if (std::fabs(ch.s2) < kStateFlush)
			ch.s2 = 0.0;
		if (std::fabs(ch.h1) < kStateFlush)
			ch.h1 = 0.0;
		if (std::fabs(ch.h2) < kStateFlush)
			ch.h2 = 0.0;
	}
	if (power < kSilenceFloor)
		power = 0.0;
	ring_[static_cast<size_t>(ringPos_)] = power;
	ringPos_ = (ringPos_ + 1) % shortTermHops_;
	hopFill_ = 0;
	hops_++;
}

double LoudnessMeter::windowPower(int hops) const
{
	double sum = 0.0;
	int idx = ringPos_;
	for (int k = 0; k < hops; k++) {
		idx = idx == 0 ? shortTermHops_ - 1 : idx - 1;
		sum += ring_[static_cast<size_t>(idx)];
	}
	return sum / hops;
}

LoudnessReading LoudnessMeter::momentary() const
{
	LoudnessReading r;
	if (configured_ && hops_ >= momentaryHops_) {
		r.ready = true;
		r.lufs = lufsFromPower(windowPower(momentaryHops_));
	}
	return r;
}

LoudnessReading LoudnessMeter::shortTerm() const
{
	LoudnessReading r;
	if (configured_ && hops_ >= shortTermHops_) {
		r.ready = true;
		r.lufs = lufsFromPower(windowPower(shortTermHops_));
	}
	return r;
}

// True peak. BS.1770-4 Annex 2 prints a 48-tap, 4-phase FIR for x4 oversampling at 48 kHz. That table could
// not be fetched while this was written (the ITU PDF was unreachable), and coefficients are never typed in
// from memory, so the interpolator is the one libebur128 uses: a 49-tap sinc (cutoff at the input Nyquist,
// zero crossings every `factor` taps) under a Hann window, split into `factor` polyphase sub-filters. Its
// passband is flat to well beyond fs/4, which is what the Tech 3341 true-peak cases (+0.2/-0.4 dB) test.
bool TruePeakMeter::configure(double sampleRate, int channels, int blockMs)
{
	configured_ = false;
	if (!(sampleRate >= 8000.0 && sampleRate <= 384000.0) || channels < 1 || channels > kLoudnessMaxChannels ||
	    blockMs < 1 || blockMs > 1000)
		return false;
	const int blockFrames = static_cast<int>(std::lround(sampleRate * blockMs / 1000.0));
	if (blockFrames < 1)
		return false;
	factor_ = sampleRate < 96000.0 ? 4 : (sampleRate < 192000.0 ? 2 : 1);
	channels_ = channels;
	blockFrames_ = blockFrames;
	hist_ = factor_ == 1 ? 1 : (kTruePeakTaps + factor_ - 1) / factor_;
	coeff_.assign(static_cast<size_t>(factor_ * hist_), 0.0);
	if (factor_ == 1) {
		coeff_[0] = 1.0;
	} else {
		for (int j = 0; j < kTruePeakTaps; j++) {
			const double m = j - (kTruePeakTaps - 1) / 2.0;
			double c = 1.0;
			if (std::fabs(m) > 1e-9)
				c = std::sin(m * kPi / factor_) / (m * kPi / factor_);
			c *= 0.5 * (1.0 - std::cos(2.0 * kPi * j / (kTruePeakTaps - 1)));
			coeff_[static_cast<size_t>((j % factor_) * hist_ + j / factor_)] = c;
		}
	}
	for (int f = 0; f < 4; f++) {
		lo_[f] = 0;
		hi_[f] = 0;
		if (f >= factor_)
			continue;
		int lo = hist_, hi = -1;
		for (int k = 0; k < hist_; k++) {
			if (std::fabs(coeff_[static_cast<size_t>(f * hist_ + k)]) > 1e-9) {
				lo = std::min(lo, k);
				hi = std::max(hi, k);
			}
		}
		lo_[f] = hi < 0 ? 0 : lo;
		hi_[f] = hi < 0 ? -1 : hi;
	}
	history_.assign(static_cast<size_t>(channels * 2 * hist_), 0.0);
	pos_.assign(static_cast<size_t>(channels), 0);
	blockFill_ = 0;
	blocks_ = 0;
	blockMax_ = lastBlockMax_ = runMax_ = sampleMax_ = 0.0;
	takeMax_ = 0.0;
	takeCount_ = 0;
	configured_ = true;
	return true;
}

void TruePeakMeter::reset()
{
	if (!configured_)
		return;
	std::fill(history_.begin(), history_.end(), 0.0);
	std::fill(pos_.begin(), pos_.end(), 0);
	blockFill_ = 0;
	blocks_ = 0;
	blockMax_ = lastBlockMax_ = runMax_ = sampleMax_ = 0.0;
	takeMax_ = 0.0;
	takeCount_ = 0;
}

long long TruePeakMeter::takeBlocks(double *maxDbtp)
{
	const long long n = takeCount_;
	if (maxDbtp)
		*maxDbtp = n > 0 ? linToDb(takeMax_) : loudnessMinusInf();
	takeCount_ = 0;
	takeMax_ = 0.0;
	return n;
}

void TruePeakMeter::feed(const float *const *planes, int frames)
{
	if (!configured_ || !planes || frames <= 0)
		return;
	int done = 0;
	while (done < frames) {
		const int n = std::min(frames - done, blockFrames_ - blockFill_);
		double peak = blockMax_;
		double samplePeak = sampleMax_;
		for (int c = 0; c < channels_; c++) {
			const float *p = planes[c] ? planes[c] + done : nullptr;
			double *h = &history_[static_cast<size_t>(c * 2 * hist_)];
			int pos = pos_[static_cast<size_t>(c)];
			for (int i = 0; i < n; i++) {
				double x = p ? static_cast<double>(p[i]) : 0.0;
				if (!std::isfinite(x))
					x = 0.0;
				// newest sample at h[pos], older ones at higher indices (mirror copy keeps it contiguous)
				pos = pos == 0 ? hist_ - 1 : pos - 1;
				h[pos] = x;
				h[pos + hist_] = x;
				samplePeak = std::max(samplePeak, std::fabs(x));
				if (factor_ == 1) {
					peak = std::max(peak, std::fabs(x));
					continue;
				}
				for (int f = 0; f < factor_; f++) {
					const double *cf = &coeff_[static_cast<size_t>(f * hist_)];
					double acc = 0.0;
					for (int k = lo_[f]; k <= hi_[f]; k++)
						acc += cf[k] * h[pos + k];
					peak = std::max(peak, std::fabs(acc));
				}
			}
			pos_[static_cast<size_t>(c)] = pos;
		}
		blockMax_ = peak;
		sampleMax_ = samplePeak;
		done += n;
		blockFill_ += n;
		if (blockFill_ >= blockFrames_) {
			lastBlockMax_ = blockMax_;
			runMax_ = std::max(runMax_, blockMax_);
			takeMax_ = std::max(takeMax_, blockMax_);
			takeCount_++;
			blockMax_ = 0.0;
			blockFill_ = 0;
			blocks_++;
		}
	}
}

double TruePeakMeter::lastBlockDbtp() const
{
	return blocks_ > 0 ? linToDb(lastBlockMax_) : loudnessMinusInf();
}

double TruePeakMeter::maxDbtp() const
{
	return linToDb(std::max(runMax_, blockMax_));
}

double TruePeakMeter::maxSampleDb() const
{
	return linToDb(sampleMax_);
}

} // namespace inflight
