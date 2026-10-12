/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The arithmetic of the Levels panel (dB mode): peak hold, average smoothing, number formatting and the
// gap wording. Pure: std + Qt Core, no libobs, no widgets, so the unit test builds without OBS.
// Times are plain millisecond counts on any steady timeline the caller likes (the panel uses the
// plugin's steady clock); only differences matter.
#pragma once

#include "inflight-ui-logic.hpp"

#include <QString>
#include <QtGlobal>

#include <limits>

namespace inflight {

// Which meter the panel shows: dB (peak and average from the volmeters, the default) or Lufs (momentary and
// short-term loudness from the capture callbacks). The key "levels_mode" stores "db" / "lufs".
enum class LevelsMode { Db, Lufs };
inline const char *levelsModeKey(LevelsMode m)
{
	return m == LevelsMode::Lufs ? "lufs" : "db";
}
// Unknown or empty text falls back to Db.
inline LevelsMode levelsModeFromKey(const char *s)
{
	return (s && s[0] == 'l' && s[1] == 'u' && s[2] == 'f' && s[3] == 's' && s[4] == '\0') ? LevelsMode::Lufs
												 : LevelsMode::Db;
}
constexpr double kLufsRefDefault = -16.0; // the optional reference line's default; never drawn unless switched on
constexpr double kLufsRefMin = -60.0;
constexpr double kLufsRefMax = 0.0;
inline double clampLufsRef(double v)
{
	if (!(v == v))
		return kLufsRefDefault;
	return v < kLufsRefMin ? kLufsRefMin : (v > kLufsRefMax ? kLufsRefMax : v);
}

constexpr qint64 kPeakHoldMs = 1500; // a peak stays on the display this long
constexpr qint64 kAvgTauMs = 300;    // time constant of the average's smoothing
// Digital silence snaps the average to exactly "silent" after this long of unbroken -inf input, so a
// source that went silent reads "-inf" within about a second (smoothing alone would show a slowly
// falling number for seconds).
constexpr qint64 kSilenceSnapMs = 600;

inline float minusInf()
{
	return -std::numeric_limits<float>::infinity();
}

// The highest peak fed within the last kPeakHoldMs: a peak fed at t is visible while now - t < kPeakHoldMs.
// Built for the panel's 20 Hz feed (LevelsPanel::kFastMs = 50 ms, so 30 feeds per 1.5 s window); the ring
// holds kCap = 40 entries (the 30 plus headroom for timer jitter). Within that rate it is an exact
// sliding-window maximum (a monotonic deque). It is NOT exact beyond it: if more than kCap decreasing peaks
// arrive inside one window (a faster feed), the OLDEST entry (the current maximum) is dropped to make room
// (oldest-dropped on overflow), so the hold under-reports. -inf is a
// real value (digital silence); NaN is ignored. A feed with a time earlier than the last one (clock jump)
// clears the history.
class PeakHold {
public:
	void feed(float peakDb, qint64 tMs);
	// -inf when nothing was fed inside the window, or everything inside it is -inf.
	float value(qint64 tMs) const;
	void clear();
	int size() const { return count_; }
	static constexpr int capacity() { return kCap; }

private:
	static constexpr int kCap = 40; // 20 Hz x 1.5 s = 30, plus headroom
	struct Entry {
		qint64 t = 0;
		float db = 0.0f;
	};
	Entry ring_[kCap];
	int head_ = 0;  // index of the oldest (largest peak) entry
	int count_ = 0;
	qint64 lastT_ = 0;
};

// Exponential smoothing of the average, done in linear amplitude (10^(dB/20)) with time constant
// kAvgTauMs; alpha = 1 - exp(-dt / tau) from the real elapsed time, so irregular ticks give the same
// curve as regular ones. The first sample sets the value outright. -inf feeds 0.0 linear. value() is
// "-inf" only when the smoothed amplitude is exactly 0: a silent first sample, or kSilenceSnapMs of unbroken
// -inf input (which sets the amplitude to exactly 0). Quiet but finite audio (-101 dBFS) is a number.
// NaN is ignored, and so is a feed older than the last accepted one (time going backwards).
class AvgSmoother {
public:
	void feed(float avgDb, qint64 tMs);
	float value() const;
	void clear();

private:
	bool have_ = false;
	double lin_ = 0.0;
	qint64 lastT_ = 0;
	qint64 silentSince_ = -1; // time of the first -inf feed of the current unbroken run, -1 = none
};

// "-12.4", "0.0", "-inf". One decimal, rounded by printf (the float's exact binary value, so a
// literal -12.45f, stored as -12.4499998, reads "-12.4"). -inf and NaN read "-inf", never a number;
// a value that rounds to zero reads "0.0" (never "-0.0").
QString formatDb(float db);

// What a Levels row shows. Missing = OBS has no source with that uuid ("(missing source)"). NoReading = the
// source exists (or is being metered) but no number is available yet (meter not attached because the panel
// is not running, just opened, or no callback yet): its real name and "--". Reading = numbers.
// "Not metering" must never read as "does not exist".
enum class RowKind { Missing, NoReading, Reading };
inline RowKind rowKind(bool sourceExists, bool metered, bool haveReading)
{
	if (!sourceExists && !metered)
		return RowKind::Missing;
	if (!metered || !haveReading)
		return RowKind::NoReading;
	return RowKind::Reading;
}

// The bar's fill = the INSTANTANEOUS level of the last meter window, no hold. A window with no fresh reading
// (no callback landed in it, or the meter went stale) has no instantaneous level: the fill drops to -inf
// (nothing drawn) instead of staying where the last window left it; the 1.5 s hold tick is PeakHold's job and
// is not touched by this. NaN is treated as no reading.
inline float instantFill(bool available, bool fresh, float windowPeakDb)
{
	if (!available || !fresh || windowPeakDb != windowPeakDb)
		return minusInf();
	return windowPeakDb;
}

struct Gap {
	bool valid = false; // false when either level is silent
	double db = 0.0;    // mic average minus the other average
};
Gap gapDb(float micAvgDb, float otherAvgDb);

// "<Mic> is 12.4 dB over <Other>", "<Mic> is 3.1 dB under <Other>", "<Mic> is level with <Other>" (|gap|
// below 0.05 dB). When the gap is not valid: "<Other> is silent" if the other source is silent
// (otherAvgDb is -inf), otherwise "<Mic> is silent".
// With lu = true the unit reads "LU" ("<Mic> is 6.1 LU over <Other>"): the LUFS mode's gap, short-term loudness.
QString gapText(const QString &micName, const QString &otherName, const Gap &gap, float otherAvgDb, const TrFn &tr,
		bool lu = false);

// A LUFS reading as text: "--" until the window is ready, then one decimal or "-inf" (formatDb).
QString formatLufs(bool ready, double lufs);

} // namespace inflight
