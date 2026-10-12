/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-settings-check.hpp"

#include <QSet>

#include <algorithm>

namespace inflight {

namespace {
QString say(const TrFn &tr, const char *key, const char *fallback)
{
	return tr ? tr(key, fallback) : QString::fromUtf8(fallback);
}

// Clamps v into [lo, hi]; on a change adds "<name> was changed from X to Y (allowed: lo to hi)."
void clampInt(int &v, int lo, int hi, const char *nameKey, const char *nameFallback, const TrFn &tr,
	      QStringList &out)
{
	const int before = v;
	v = std::min(std::max(v, lo), hi);
	if (v == before)
		return;
	out << say(tr, "InFlight.Settings.Adj.Int", "%1 was changed from %2 to %3 (allowed: %4 to %5).")
			.arg(say(tr, nameKey, nameFallback))
			.arg(before)
			.arg(v)
			.arg(lo)
			.arg(hi);
}

QString num1(double v)
{
	return QString::number(v, 'f', 1);
}

void clampDouble(double &v, double lo, double hi, const char *nameKey, const char *nameFallback,
		 const TrFn &tr, QStringList &out)
{
	const double before = v;
	v = std::min(std::max(v, lo), hi);
	if (v == before)
		return;
	out << say(tr, "InFlight.Settings.Adj.Int", "%1 was changed from %2 to %3 (allowed: %4 to %5).")
			.arg(say(tr, nameKey, nameFallback))
			.arg(num1(before))
			.arg(num1(v))
			.arg(num1(lo))
			.arg(num1(hi));
}
} // namespace

QStringList normalizeSettings(Settings &s, const TrFn &tr)
{
	QStringList out;

	// Mic muted: amber 5..600 s, red at least 5 s above amber, at most 1800 s.
	clampInt(s.micMutedAmberS, 5, 600, "InFlight.Settings.Name.MicAmber", "Mic muted: warn after (seconds)",
		 tr, out);
	{
		const int before = s.micMutedRedS;
		s.micMutedRedS = std::min(std::max(s.micMutedRedS, s.micMutedAmberS + 5), 1800);
		if (s.micMutedRedS != before)
			out << say(tr, "InFlight.Settings.Adj.MicRed",
				   "Mic muted: failing after was changed from %1 s to %2 s (it must be at least 5 s "
				   "after the warning and at most 1800 s).")
					.arg(before)
					.arg(s.micMutedRedS);
	}
	clampInt(s.heardYouSilenceS, 30, 1800, "InFlight.Settings.Name.HeardSilence",
		 "Mic very quiet: seconds", tr, out);
	clampInt(s.heardYouDbfs, -80, -20, "InFlight.Settings.Name.HeardDbfs", "Mic very quiet: level (dBFS)",
		 tr, out);
	clampInt(s.desktopSilenceS, 30, 1800, "InFlight.Settings.Name.DesktopSilence",
		 "Desktop audio silent: seconds", tr, out);
	clampInt(s.sceneRedExtraS, 60, 3600, "InFlight.Settings.Name.SceneExtra",
		 "Holding scene: extra seconds before failing", tr, out);
	clampInt(s.recordStartGraceS, 10, 600, "InFlight.Settings.Name.RecordGrace",
		 "Recording must start within (seconds)", tr, out);

	// Drops: amber 0.1..10 %, red at least 0.1 above amber, at most 50 %.
	clampDouble(s.dropAmberPct, 0.1, 10.0, "InFlight.Settings.Name.DropAmber", "Dropped frames: warn at (%)",
		    tr, out);
	{
		const double before = s.dropRedPct;
		const double minRed = s.dropAmberPct + 0.1;
		if (s.dropRedPct < minRed - 1e-9) // epsilon: 10.0 + 0.1 must not reject 10.1
			s.dropRedPct = minRed;
		if (s.dropRedPct > 50.0)
			s.dropRedPct = 50.0;
		if (s.dropRedPct != before)
			out << say(tr, "InFlight.Settings.Adj.DropRed",
				   "Dropped frames: failing at was changed from %1% to %2% (it must be at least "
				   "0.1% above the warning and at most 50%).")
					.arg(num1(before))
					.arg(num1(s.dropRedPct));
	}

	// Holding scenes: no empty UUID, no repeats (first kept), limit 60..7200 s, label up to 40 chars.
	QVector<HoldingScene> kept;
	QSet<QString> seen;
	for (HoldingScene h : s.holdingScenes) {
		if (h.sceneUuid.isEmpty()) {
			out << say(tr, "InFlight.Settings.Adj.NoScene",
				   "A holding scene row with no scene chosen was removed.");
			continue;
		}
		if (seen.contains(h.sceneUuid)) {
			out << say(tr, "InFlight.Settings.Adj.DupScene",
				   "A scene listed more than once was removed (the first one is kept).");
			continue;
		}
		seen.insert(h.sceneUuid);
		const int before = h.limitSeconds;
		h.limitSeconds = std::min(std::max(h.limitSeconds, 60), 7200);
		if (h.limitSeconds != before)
			out << say(tr, "InFlight.Settings.Adj.SceneLimit",
				   "A holding scene's time limit was changed from %1 s to %2 s (allowed: 60 s to "
				   "7200 s).")
					.arg(before)
					.arg(h.limitSeconds);
		h.label = h.label.trimmed();
		if (h.label.size() > 40) {
			h.label = h.label.left(40).trimmed();
			out << say(tr, "InFlight.Settings.Adj.Label",
				   "A holding scene's label was shortened to 40 characters.");
		}
		kept.push_back(h);
	}
	s.holdingScenes = kept;
	return out;
}

} // namespace inflight
