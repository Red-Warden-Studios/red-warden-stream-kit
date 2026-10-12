/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The wording and the header rule of the lights UI, as pure functions (Qt Core only, no widgets, no
// libobs) so they can be unit-tested. Text goes through an injected tr function, which the dock wires
// to obs_module_text and the test wires to the identity (fallback) lookup.
#pragma once

#include "inflight-eval.hpp"

#include <QString>
#include <QtGlobal>

#include <functional>

namespace inflight {

using TrFn = std::function<QString(const char *key, const char *fallback)>;

struct ReasonText {
	QString what; // what is wrong (or fine), one short line
	QString fix;  // one actionable sentence
};

// `value` is the LightResult's value: seconds for timed reasons, drop percent for NetworkDrops and
// PcRenderLag / PcEncoderLag, the stop code for StreamDropped.
ReasonText reasonText(Reason r, double value, const TrFn &tr);

// reasonText for a whole light result: a Network result that names a Multistream destination says WHICH one
// ("Twitch is dropping 3.1% of its frames (last 30 s)."); everything else is exactly reasonText(r.reason, r.value).
ReasonText reasonTextFor(const LightResult &r, const TrFn &tr);

// The Network details' extra line: the named output's session total ("This session on Twitch: 1.4% dropped"),
// and, only when several outputs are being watched, which others are fine ("... · Main stream and YouTube are
// fine"). The session part is there whenever the result names an output, also when the main stream is the only one.
// Empty when the result names no output.
QString networkSessionLine(const LightResult &r, const TrFn &tr);


// The label of the Network details' button when the worst output is a destination: "Turn off Twitch".
QString turnOffText(const QString &outputName, const TrFn &tr);

// A short noun phrase for a Reason with no value in it ("Mic muted", "Frames dropped"), for log lines
// that have no value to show.
QString reasonLabel(Reason r, const TrFn &tr);

// "45 s", "3 min", "1 h 05 min". Negative counts as 0.
QString durationText(qint64 ms);

// The worst light for the header dot: Red > Amber > Unknown > Green > Grey. A light that is Unknown only
// because its frame window is filling does not count as Unknown. Snoozed lights still count.
LightState headerState(const Snapshot &s);

QString headerText(LiveState live, const Snapshot &s, const TrFn &tr);

// True while some light is Red with an open incident that is neither acknowledged nor snoozed (at
// nowMs). The dock keeps its title mark on exactly while this holds.
bool needsAttention(const Snapshot &s, qint64 nowMs);

// Full display name ("Desktop audio") and the short label under the light ("Desktop").
QString lightName(Light l, const TrFn &tr);
QString lightShortName(Light l, const TrFn &tr);
// "Green", "Amber", "Red", "Unknown", "Off" (for tooltips and the details panel).
QString stateName(LightState s, const TrFn &tr);
// "started", "got worse", "ended", "acknowledged".
QString eventKindText(IncidentEventKind k, const TrFn &tr);

} // namespace inflight
