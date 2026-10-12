/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "levels-math.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace inflight {
namespace {
bool isMinusInf(float v)
{
	return std::isinf(v) && v < 0;
}
} // namespace

// ---- PeakHold

void PeakHold::clear()
{
	head_ = 0;
	count_ = 0;
	lastT_ = 0;
}

void PeakHold::feed(float peakDb, qint64 tMs)
{
	if (std::isnan(peakDb))
		return;
	if (count_ > 0 && tMs < lastT_)
		clear(); // the clock went backwards: the history means nothing now
	lastT_ = tMs;
	// Oldest entries first: expired ones leave the front.
	while (count_ > 0 && tMs - ring_[head_].t >= kPeakHoldMs) {
		head_ = (head_ + 1) % kCap;
		--count_;
	}
	// Smaller-or-equal peaks that are older than this one can never be the maximum again.
	while (count_ > 0) {
		const int back = (head_ + count_ - 1) % kCap;
		if (ring_[back].db > peakDb)
			break;
		--count_;
	}
	if (count_ == kCap) { // full (feeding faster than the 20 Hz this was built for): drop the oldest, see the header
		head_ = (head_ + 1) % kCap;
		--count_;
	}
	const int at = (head_ + count_) % kCap;
	ring_[at].t = tMs;
	ring_[at].db = peakDb;
	++count_;
}

float PeakHold::value(qint64 tMs) const
{
	for (int i = 0; i < count_; ++i) {
		const Entry &e = ring_[(head_ + i) % kCap];
		if (tMs - e.t < kPeakHoldMs)
			return e.db; // the entries fall in peak, so the first live one is the maximum
	}
	return minusInf();
}

// ---- AvgSmoother

void AvgSmoother::clear()
{
	have_ = false;
	lin_ = 0.0;
	lastT_ = 0;
	silentSince_ = -1;
}

void AvgSmoother::feed(float avgDb, qint64 tMs)
{
	if (std::isnan(avgDb))
		return;
	if (have_ && tMs < lastT_)
		return; // time went backwards: ignore this feed entirely, lastT_ and the value stay
	const bool silent = isMinusInf(avgDb);
	const double x = silent ? 0.0 : std::pow(10.0, static_cast<double>(avgDb) / 20.0);
	if (!have_) {
		have_ = true;
		lin_ = x;
	} else {
		const qint64 dt = tMs - lastT_;
		const double alpha = 1.0 - std::exp(-static_cast<double>(dt) / static_cast<double>(kAvgTauMs));
		lin_ += alpha * (x - lin_);
	}
	lastT_ = tMs;
	if (silent) {
		if (silentSince_ < 0)
			silentSince_ = tMs;
		if (tMs - silentSince_ >= kSilenceSnapMs)
			lin_ = 0.0;
	} else {
		silentSince_ = -1;
	}
}

float AvgSmoother::value() const
{
	if (!have_ || lin_ <= 0.0)
		return minusInf();
	return static_cast<float>(20.0 * std::log10(lin_));
}

// ---- text

QString formatDb(float db)
{
	if (std::isnan(db) || isMinusInf(db))
		return QStringLiteral("-inf");
	if (std::isinf(db))
		db = 0.0f; // +inf cannot come from a meter; show the ceiling rather than a word
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(db));
	if (std::strcmp(buf, "-0.0") == 0)
		return QStringLiteral("0.0");
	return QString::fromLatin1(buf);
}

Gap gapDb(float micAvgDb, float otherAvgDb)
{
	Gap g;
	if (std::isnan(micAvgDb) || std::isnan(otherAvgDb) || std::isinf(micAvgDb) || std::isinf(otherAvgDb))
		return g;
	g.valid = true;
	g.db = static_cast<double>(micAvgDb) - static_cast<double>(otherAvgDb);
	return g;
}

QString formatLufs(bool ready, double lufs)
{
	if (!ready)
		return QStringLiteral("--");
	return formatDb(static_cast<float>(lufs));
}

QString gapText(const QString &micName, const QString &otherName, const Gap &gap, float otherAvgDb, const TrFn &tr,
		bool lu)
{
	if (!gap.valid) {
		if (std::isnan(otherAvgDb) || isMinusInf(otherAvgDb))
			return tr("InFlight.Levels.Silent", "%1 is silent").arg(otherName);
		return tr("InFlight.Levels.Silent", "%1 is silent").arg(micName);
	}
	if (std::fabs(gap.db) < 0.05)
		return tr("InFlight.Levels.GapLevel", "%1 is level with %2").arg(micName, otherName);
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.1f", std::fabs(gap.db));
	const QString n = QString::fromLatin1(buf);
	if (lu) {
		if (gap.db > 0)
			return tr("InFlight.Levels.GapOverLu", "%1 is %2 LU over %3").arg(micName, n, otherName);
		return tr("InFlight.Levels.GapUnderLu", "%1 is %2 LU under %3").arg(micName, n, otherName);
	}
	if (gap.db > 0)
		return tr("InFlight.Levels.GapOver", "%1 is %2 dB over %3").arg(micName, n, otherName);
	return tr("InFlight.Levels.GapUnder", "%1 is %2 dB under %3").arg(micName, n, otherName);
}

} // namespace inflight
