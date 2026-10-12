/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Loudness maths for the Levels panel's LUFS mode: ITU-R BS.1770 K-weighting, momentary (400 ms) and
// short-term (3 s) loudness, and an opt-in true-peak meter. Pure: std only, no libobs, no Qt, so the unit
// test builds without OBS. No gating and no integrated loudness (v1).
//
// Threading: every object here is single-threaded (the panel's worker thread owns it). Memory is
// allocated in configure() only; feed(), reset() and the readers never allocate.
#pragma once

#include <limits>
#include <vector>

namespace inflight {

inline double loudnessMinusInf()
{
	return -std::numeric_limits<double>::infinity();
}

// What a channel is, for the BS.1770 channel weights. Lfe is excluded from loudness (weight 0);
// LeftSurround / RightSurround weigh 1.41 (+1.5 dB); everything else weighs 1.0.
enum class ChannelRole { Left, Right, Center, Lfe, LeftSurround, RightSurround, Other };

constexpr int kLoudnessMaxChannels = 8;

double channelWeight(ChannelRole role);

// Roles of the planar channels OBS delivers for a speaker layout with this many channels, in OBS order
// (libobs audio-defs / media-io speaker_layout):
//   1 mono    : Center
//   2 stereo  : L R
//   3 2.1     : L R LFE
//   4 4.0     : L R C RC          (RC = rear centre, weighed as a surround)
//   5 4.1     : L R C LFE RC
//   6 5.1     : L R C LFE Ls Rs
//   8 7.1     : L R C LFE Ls Rs Lss Rss  (rear pair, then side pair; all four weigh as surrounds)
// Checked against libobs 31.1.1 (media-io/audio-io.h speaker_layout comments; audio-resampler-ffmpeg.c). The
// header comments and the resampler's mono-upmix matrix ({1,1,0} for 3 channels, {1,1,1,0,1} for 5) agree on
// the order above, so 2.1 has its LFE LAST. One inconsistency inside libobs: for a NON-mono source the same
// file names the 3-channel layout AV_CH_LAYOUT_SURROUND (FL FR FC), so a 2.1 mixer may carry centre content in
// channel 2. 2.1 is a rare output format; the map follows the header (LFE, excluded).
// `out` must hold kLoudnessMaxChannels entries. Returns the channel count, or 0 (out untouched) when no OBS
// layout has that many channels (7 channels, 0, 9 and up).
int channelRolesForObsLayout(int channels, ChannelRole *out);

struct Biquad {
	double b0 = 1.0, b1 = 0.0, b2 = 0.0;
	double a1 = 0.0, a2 = 0.0; // y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
};

struct KWeightCoeffs {
	Biquad shelf;    // stage 1: pre-filter, high shelf (about +4 dB above 2 kHz)
	Biquad highpass; // stage 2: RLB high-pass
};

// K-weighting coefficients derived for `sampleRate` by the bilinear transform of the analog prototypes
// (f0, Q, shelf gain), the same parameters libebur128 uses. At 48 kHz they equal the BS.1770 published
// coefficients to better than 1e-6 relative.
KWeightCoeffs kWeightCoeffs(double sampleRate);

// Magnitude response of the two stages together, in dB, at freqHz (for tests and documentation).
double kWeightGainDb(const KWeightCoeffs &coeffs, double sampleRate, double freqHz);

struct LoudnessReading {
	bool ready = false;                // false until the window has filled: the UI shows "--"
	double lufs = loudnessMinusInf();  // -inf for digital silence (never a large negative number)
};

// K-weighted loudness meter. Planar float input, any sample rate from 8 kHz up, 1..8 channels.
// Mean squares are accumulated per hop (default 100 ms); momentary = the last 400 ms of hops, short-term = the
// last 3 s, both sliding and refreshed at every completed hop:
//   LUFS = -0.691 + 10 log10( sum_i G_i * ms_i )
// A hop whose weighted mean square is below 1e-14 (about -140 LUFS, under the 24-bit noise floor) counts as
// exact silence, so a window of silence and filter tails reads -inf rather than -300.
// Non-finite samples are treated as 0 so one NaN cannot poison the filter state.
class LoudnessMeter {
public:
	// Allocates. hopMs must divide 400 and 3000 (1, 2, 4, 5, 8, 10, 20, 25, 40, 50, 100, 200); the panel uses
	// 100, the finer hops exist for the EBU Tech 3341 tone-burst cases. Returns false (meter unusable) for a
	// bad rate, channel count, role pointer or hop.
	bool configure(double sampleRate, const ChannelRole *roles, int channels, int hopMs = 100);
	// Back to the just-configured state (filters, windows, counters). No allocation.
	void reset();
	// planes[c] points at `frames` samples of channel c; a null plane counts as silence.
	void feed(const float *const *planes, int frames);

	LoudnessReading momentary() const;
	LoudnessReading shortTerm() const;
	long long hopsCompleted() const { return hops_; }
	bool configured() const { return configured_; }
	int channels() const { return channels_; }

private:
	struct Chan {
		double weight = 0.0;
		double sumSq = 0.0;
		double s1 = 0.0, s2 = 0.0; // shelf state (direct form II transposed)
		double h1 = 0.0, h2 = 0.0; // high-pass state
	};
	void completeHop();
	double windowPower(int hops) const;

	bool configured_ = false;
	int channels_ = 0;
	int hopFrames_ = 0;
	int hopFill_ = 0;
	int momentaryHops_ = 0;
	int shortTermHops_ = 0;
	long long hops_ = 0;
	KWeightCoeffs k_;
	Chan chans_[kLoudnessMaxChannels];
	std::vector<double> ring_; // weighted mean square of each of the last shortTermHops_ hops
	int ringPos_ = 0;
};

// True-peak meter (BS.1770 Annex 2): oversamples and reports the largest absolute value. Opt-in and separate
// from LoudnessMeter, which does not own one; when nobody calls feed() it costs nothing.
// Oversampling: below 96 kHz x4 (so 44.1 and 48 kHz), 96 kHz up to 192 kHz x2, 192 kHz and above x1 (the
// annex needs none; the value is then the sample peak). Interpolator: 49-tap Hann-windowed sinc, polyphase,
// the libebur128 design (see loudness.cpp for why this and not the annex's printed table).
class TruePeakMeter {
public:
	// Allocates. blockMs: length of the peak blocks (the panel uses 100). Returns false for bad arguments.
	bool configure(double sampleRate, int channels, int blockMs = 100);
	void reset();
	void feed(const float *const *planes, int frames);

	int oversampling() const { return factor_; }
	bool blockReady() const { return blocks_ > 0; }
	long long blocksCompleted() const { return blocks_; }
	// Largest true peak of the last completed block, dBTP; -inf for silence or when no block completed yet.
	double lastBlockDbtp() const;
	// Running maximum since configure()/reset(), including the block in progress; -inf for silence.
	double maxDbtp() const;
	// Running maximum of the plain sample values, dBFS (a true peak is never below it).
	double maxSampleDb() const;
	// The number of blocks completed since the last take (or reset), and the largest block maximum among them
	// in dBTP (-inf when none or silent). Lets a caller that feeds in big chunks still see the transient of a
	// block that is not the last one completed. Clears the counters.
	long long takeBlocks(double *maxDbtp);
	bool configured() const { return configured_; }

private:
	double takeMax_ = 0.0;     // linear, max of the blocks completed since the last take
	long long takeCount_ = 0;
	bool configured_ = false;
	int channels_ = 0;
	int factor_ = 4;
	int hist_ = 1;  // history length per channel (taps per phase)
	int blockFrames_ = 0;
	int blockFill_ = 0;
	long long blocks_ = 0;
	double blockMax_ = 0.0;     // linear, block in progress
	double lastBlockMax_ = 0.0; // linear, last completed block
	double runMax_ = 0.0;       // linear, completed blocks
	double sampleMax_ = 0.0;    // linear sample peak, everything since reset
	std::vector<double> coeff_;  // factor_ x hist_
	int lo_[4] = {0, 0, 0, 0};   // first / last non-zero tap of each phase
	int hi_[4] = {0, 0, 0, 0};
	std::vector<double> history_; // channels_ x 2*hist_ (each sample stored twice so a window is contiguous)
	std::vector<int> pos_;
};

} // namespace inflight
