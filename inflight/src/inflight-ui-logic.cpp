/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-ui-logic.hpp"

#include <cmath>

namespace inflight {

namespace {
QString pct(double v)
{
	return QString::number(v, 'f', 1);
}

QString secs(double v)
{
	return durationText(static_cast<qint64>(std::llround(v * 1000.0)));
}

int rank(LightState s)
{
	switch (s) {
	case LightState::Red:
		return 4;
	case LightState::Amber:
		return 3;
	case LightState::Unknown:
		return 2;
	case LightState::Green:
		return 1;
	case LightState::Grey:
		return 0;
	}
	return 0;
}
} // namespace

// One case per Reason and no default: a new Reason is a compile warning here and a failing unit test.
ReasonText reasonText(Reason r, double value, const TrFn &tr)
{
	switch (r) {
	case Reason::NotSetUp:
		return {tr("InFlight.Reason.NotSetUp", "Not set up yet"),
			tr("InFlight.Fix.NotSetUp", "Choose it in In-Flight settings (gear button).")};
	case Reason::Disabled:
		return {tr("InFlight.Reason.Disabled", "Turned off"),
			tr("InFlight.Fix.Disabled", "Turn this light on in In-Flight settings (gear button).")};
	case Reason::OffAir:
		return {tr("InFlight.Reason.OffAir", "Not live"),
			tr("InFlight.Fix.OffAir", "This light only watches while you are live.")};
	case Reason::MainStreamOff:
		return {tr("InFlight.Reason.MainStreamOff", "OBS is not streaming"),
			tr("InFlight.Fix.MainStreamOff", "This light only judges OBS's own stream.")};
	case Reason::LiveUnknown:
		return {tr("InFlight.Reason.LiveUnknown", "Can't tell if you are live"),
			tr("InFlight.Fix.LiveUnknown", "In-Flight keeps checking; nothing to do yet.")};
	case Reason::Ok:
		return {tr("InFlight.Reason.Ok", "All good"), tr("InFlight.Fix.Ok", "Nothing to do.")};
	case Reason::MicMuted:
		return {tr("InFlight.Reason.MicMuted", "Your mic has been muted for %1").arg(secs(value)),
			tr("InFlight.Fix.MicMuted", "Unmute it in the Audio Mixer.")};
	case Reason::MicMissing:
		return {tr("InFlight.Reason.MicMissing", "Your mic source was not found"),
			tr("InFlight.Fix.MicMissing", "Pick your mic again in In-Flight settings (gear button).")};
	case Reason::MicNotAudio:
		return {tr("InFlight.Reason.MicNotAudio", "The chosen mic is not an audio source"),
			tr("InFlight.Fix.MicNotAudio", "Pick your mic again in In-Flight settings (gear button).")};
	case Reason::MicInactive:
		return {tr("InFlight.Reason.MicInactive", "Your mic is not active"),
			tr("InFlight.Fix.MicInactive", "Check that the mic is plugged in and switched on in OBS.")};
	case Reason::MicNoCallback:
		return {tr("InFlight.Reason.MicNoCallback", "No sound data from your mic for %1").arg(secs(value)),
			tr("InFlight.Fix.MicNoCallback", "Check that the device is still connected; OBS may have lost it.")};
	case Reason::MicNotHeard:
		return {tr("InFlight.Reason.MicNotHeard", "Your mic has been very quiet for %1").arg(secs(value)),
			tr("InFlight.Fix.MicNotHeard", "Check the mic's meter, gain and that the right device is picked.")};
	case Reason::DesktopMuted:
		return {tr("InFlight.Reason.DesktopMuted", "Desktop audio is muted"),
			tr("InFlight.Fix.DesktopMuted", "Unmute it in the Audio Mixer.")};
	case Reason::DesktopMissing:
		return {tr("InFlight.Reason.DesktopMissing", "Your desktop audio source was not found"),
			tr("InFlight.Fix.DesktopMissing", "Pick it again in In-Flight settings (gear button).")};
	case Reason::DesktopNotAudio:
		return {tr("InFlight.Reason.DesktopNotAudio", "The chosen desktop source is not an audio source"),
			tr("InFlight.Fix.DesktopNotAudio", "Pick it again in In-Flight settings (gear button).")};
	case Reason::DesktopNoCallback:
		return {tr("InFlight.Reason.DesktopNoCallback", "No sound data from desktop audio for %1")
				.arg(secs(value)),
			tr("InFlight.Fix.DesktopNoCallback", "Check that the audio device is still connected.")};
	case Reason::DesktopDigitalSilence:
		return {tr("InFlight.Reason.DesktopDigitalSilence", "Desktop audio has been completely silent for %1")
				.arg(secs(value)),
			tr("InFlight.Fix.DesktopDigitalSilence",
			   "If something should be playing, check that the audio device is still connected.")};
	case Reason::SceneOverLimit:
		return {tr("InFlight.Reason.SceneOverLimit", "A holding scene is %1 over its time limit")
				.arg(secs(value)),
			tr("InFlight.Fix.SceneOverLimit", "Switch to your next scene when you are ready.")};
	case Reason::RecordingNotStarted:
		return {tr("InFlight.Reason.RecordingNotStarted", "The recording has not started (%1 since you went live)")
				.arg(secs(value)),
			tr("InFlight.Fix.RecordingNotStarted", "Start the recording in OBS.")};
	case Reason::RecordingStopped:
		return {tr("InFlight.Reason.RecordingStopped", "The recording stopped"),
			tr("InFlight.Fix.RecordingStopped", "Start it again in OBS.")};
	case Reason::RecordingPaused:
		return {tr("InFlight.Reason.RecordingPaused", "The recording is paused"),
			tr("InFlight.Fix.RecordingPaused", "Resume it in OBS when you are ready.")};
	case Reason::NetworkDrops:
		return {tr("InFlight.Reason.NetworkDrops", "%1% of frames dropped over the last 30 s").arg(pct(value)),
			tr("InFlight.Fix.NetworkDrops", "Check your internet connection; a lower bitrate may help.")};
	case Reason::NetworkReconnecting:
		return {tr("InFlight.Reason.NetworkReconnecting", "Reconnecting to the stream server"),
			tr("InFlight.Fix.NetworkReconnecting", "Check your internet connection; OBS is retrying.")};
	case Reason::NetworkWindowFilling:
		return {tr("InFlight.Reason.NetworkWindowFilling", "Still measuring the connection"),
			tr("InFlight.Fix.NetworkWindowFilling", "It needs about 30 seconds of streaming to judge.")};
	case Reason::PcRenderLag:
		return {tr("InFlight.Reason.PcRenderLag", "OBS rendered %1% of frames late over the last 30 s").arg(pct(value)),
			tr("InFlight.Fix.PcRenderLag", "Close heavy programs, or lower the canvas size or frame rate.")};
	case Reason::PcEncoderLag:
		return {tr("InFlight.Reason.PcEncoderLag", "The encoder skipped %1% of frames over the last 30 s").arg(pct(value)),
			tr("InFlight.Fix.PcEncoderLag", "Lower the output size or frame rate, or use a lighter encoder.")};
	case Reason::PcWindowFilling:
		return {tr("InFlight.Reason.PcWindowFilling", "Still measuring your PC"),
			tr("InFlight.Fix.PcWindowFilling", "It needs about 30 seconds of streaming to judge.")};
	case Reason::StreamDropped:
		return {tr("InFlight.Reason.StreamDropped", "Stream dropped (code %1)").arg(qRound(value)),
			tr("InFlight.Fix.StreamDropped",
			   "Start the stream again. If it keeps happening, check your connection and stream key.")};
	}
	return {QString(), QString()};
}

ReasonText reasonTextFor(const LightResult &r, const TrFn &tr)
{
	if (r.net.valid && !r.net.outputId.isEmpty()) {
		const QString name = r.net.outputName;
		if (r.reason == Reason::NetworkDrops)
			return {tr("InFlight.Reason.DestDrops", "%1 is dropping %2% of its frames (last 30 s).").arg(name, pct(r.value)),
				tr("InFlight.Fix.DestDrops",
				   "Check that destination's connection or lower its bitrate. Turning it off keeps the rest of your stream going.")};
		if (r.reason == Reason::NetworkReconnecting)
			return {tr("InFlight.Reason.DestReconnecting", "%1 is reconnecting.").arg(name),
				tr("InFlight.Fix.DestReconnecting",
				   "Multistream is trying to get it back. Turn it off if you do not need it right now.")};
	}
	// Still measuring: say WHICH outputs have no verdict yet.
	if (r.reason == Reason::NetworkWindowFilling && (r.net.mainFilling || !r.net.fillingDestinations.isEmpty())) {
		QStringList who;
		if (r.net.mainFilling)
			who.push_back(tr("InFlight.Net.MainName", "Main stream"));
		who += r.net.fillingDestinations;
		return {tr("InFlight.Reason.NetworkFillingWho", "Still measuring %1").arg(who.join(QStringLiteral(", "))),
			tr("InFlight.Fix.NetworkWindowFilling", "It needs about 30 seconds of streaming to judge.")};
	}
	return reasonText(r.reason, r.value, tr);
}

QString networkSessionLine(const LightResult &r, const TrFn &tr)
{
	if (!r.net.valid)
		return QString();
	const QString mainName = tr("InFlight.Net.MainName", "Main stream");
	const QString name = r.net.outputId.isEmpty() ? mainName : r.net.outputName;
	// "... are fine" only makes sense next to other outputs; the session figure does not need them.
	QStringList fine;
	if (r.net.multi) {
		if (r.net.mainFine)
			fine.push_back(mainName);
		for (const QString &d : r.net.fineDestinations)
			fine.push_back(d);
	}

	QString first;
	if (r.net.sessionPct >= 0.0)
		first = tr("InFlight.Net.SessionOn", "This session on %1: %2% dropped").arg(name, pct(r.net.sessionPct));
	QString second;
	if (fine.size() == 1) {
		second = tr("InFlight.Net.OneFine", "%1 is fine").arg(fine[0]);
	} else if (fine.size() >= 2) {
		const QString last = fine.takeLast();
		second = tr("InFlight.Net.ManyFine", "%1 and %2 are fine").arg(fine.join(QStringLiteral(", ")), last);
	}
	if (first.isEmpty())
		return second;
	if (second.isEmpty())
		return first;
	return first + QStringLiteral(" ") + QChar(0x00B7) + QStringLiteral(" ") + second;
}

QString turnOffText(const QString &outputName, const TrFn &tr)
{
	return tr("InFlight.Net.TurnOff", "Turn off %1").arg(outputName);
}

// Value-free short noun phrases, for log lines whose incident was never seen open in a snapshot.
QString reasonLabel(Reason r, const TrFn &tr)
{
	switch (r) {
	case Reason::NotSetUp:
		return tr("InFlight.Label.NotSetUp", "Not set up");
	case Reason::Disabled:
		return tr("InFlight.Label.Disabled", "Turned off");
	case Reason::OffAir:
		return tr("InFlight.Label.OffAir", "Not live");
	case Reason::MainStreamOff:
		return tr("InFlight.Label.MainStreamOff", "Not streaming");
	case Reason::LiveUnknown:
		return tr("InFlight.Label.LiveUnknown", "Live state unknown");
	case Reason::Ok:
		return tr("InFlight.Label.Ok", "All good");
	case Reason::MicMuted:
		return tr("InFlight.Label.MicMuted", "Mic muted");
	case Reason::MicMissing:
		return tr("InFlight.Label.MicMissing", "Mic missing");
	case Reason::MicNotAudio:
		return tr("InFlight.Label.MicNotAudio", "Mic is not an audio source");
	case Reason::MicInactive:
		return tr("InFlight.Label.MicInactive", "Mic not active");
	case Reason::MicNoCallback:
		return tr("InFlight.Label.MicNoCallback", "No sound data from mic");
	case Reason::MicNotHeard:
		return tr("InFlight.Label.MicNotHeard", "Mic very quiet");
	case Reason::DesktopMuted:
		return tr("InFlight.Label.DesktopMuted", "Desktop audio muted");
	case Reason::DesktopMissing:
		return tr("InFlight.Label.DesktopMissing", "Desktop audio missing");
	case Reason::DesktopNotAudio:
		return tr("InFlight.Label.DesktopNotAudio", "Desktop source is not audio");
	case Reason::DesktopNoCallback:
		return tr("InFlight.Label.DesktopNoCallback", "No sound data from desktop audio");
	case Reason::DesktopDigitalSilence:
		return tr("InFlight.Label.DesktopDigitalSilence", "Desktop audio silent");
	case Reason::SceneOverLimit:
		return tr("InFlight.Label.SceneOverLimit", "Holding scene too long");
	case Reason::RecordingNotStarted:
		return tr("InFlight.Label.RecordingNotStarted", "Recording not started");
	case Reason::RecordingStopped:
		return tr("InFlight.Label.RecordingStopped", "Recording stopped");
	case Reason::RecordingPaused:
		return tr("InFlight.Label.RecordingPaused", "Recording paused");
	case Reason::NetworkDrops:
		return tr("InFlight.Label.NetworkDrops", "Frames dropped");
	case Reason::NetworkReconnecting:
		return tr("InFlight.Label.NetworkReconnecting", "Reconnecting");
	case Reason::NetworkWindowFilling:
		return tr("InFlight.Label.NetworkWindowFilling", "Measuring connection");
	case Reason::PcRenderLag:
		return tr("InFlight.Label.PcRenderLag", "Rendering lag");
	case Reason::PcEncoderLag:
		return tr("InFlight.Label.PcEncoderLag", "Encoder lag");
	case Reason::PcWindowFilling:
		return tr("InFlight.Label.PcWindowFilling", "Measuring PC");
	case Reason::StreamDropped:
		return tr("InFlight.Label.StreamDropped", "Stream dropped");
	}
	return QString();
}

QString durationText(qint64 ms)
{
	if (ms < 0)
		ms = 0;
	const qint64 s = ms / 1000;
	if (s < 60)
		return QStringLiteral("%1 s").arg(s);
	if (s < 3600)
		return QStringLiteral("%1 min").arg(s / 60);
	const qint64 h = s / 3600;
	const qint64 m = (s % 3600) / 60;
	return QStringLiteral("%1 h %2 min").arg(h).arg(m, 2, 10, QLatin1Char('0'));
}

LightState headerState(const Snapshot &s)
{
	LightState best = LightState::Grey;
	for (const LightResult &l : s.lights) {
		LightState st = l.state;
		if (st == LightState::Unknown &&
		    (l.reason == Reason::NetworkWindowFilling || l.reason == Reason::PcWindowFilling))
			st = LightState::Green;
		if (rank(st) > rank(best))
			best = st;
	}
	return best;
}

QString headerText(LiveState live, const Snapshot &s, const TrFn &tr)
{
	if (s.at(Light::Stream).state == LightState::Red)
		return tr("InFlight.Status.Dropped", "Stream dropped");
	switch (live) {
	case LiveState::Live:
		return tr("InFlight.Status.Watching", "Watching");
	case LiveState::Unknown:
		return tr("InFlight.Status.Checking", "Checking...");
	case LiveState::Off:
		return tr("InFlight.Status.Standing", "Standing by");
	}
	return QString();
}

bool needsAttention(const Snapshot &s, qint64 nowMs)
{
	for (const LightResult &l : s.lights) {
		if (l.state == LightState::Red && l.incidentId != 0 && !l.acknowledged &&
		    !(l.snoozedUntilMs > nowMs))
			return true;
	}
	return false;
}

QString lightName(Light l, const TrFn &tr)
{
	switch (l) {
	case Light::Mic:
		return tr("InFlight.Light.Mic", "Mic");
	case Light::Desktop:
		return tr("InFlight.Light.Desktop", "Desktop audio");
	case Light::Scene:
		return tr("InFlight.Light.Scene", "Scene");
	case Light::Recording:
		return tr("InFlight.Light.Recording", "Recording");
	case Light::Network:
		return tr("InFlight.Light.Network", "Network");
	case Light::Pc:
		return tr("InFlight.Light.Pc", "PC keeping up");
	case Light::Stream:
		return tr("InFlight.Light.Stream", "Stream");
	}
	return QString();
}

QString lightShortName(Light l, const TrFn &tr)
{
	switch (l) {
	case Light::Mic:
		return tr("InFlight.LightShort.Mic", "Mic");
	case Light::Desktop:
		return tr("InFlight.LightShort.Desktop", "Desktop");
	case Light::Scene:
		return tr("InFlight.LightShort.Scene", "Scene");
	case Light::Recording:
		return tr("InFlight.LightShort.Recording", "Rec");
	case Light::Network:
		return tr("InFlight.LightShort.Network", "Net");
	case Light::Pc:
		return tr("InFlight.LightShort.Pc", "PC");
	case Light::Stream:
		return tr("InFlight.LightShort.Stream", "Stream");
	}
	return QString();
}

QString stateName(LightState s, const TrFn &tr)
{
	switch (s) {
	case LightState::Grey:
		return tr("InFlight.State.Grey", "Off");
	case LightState::Green:
		return tr("InFlight.State.Green", "OK");
	case LightState::Amber:
		return tr("InFlight.State.Amber", "Check soon");
	case LightState::Red:
		return tr("InFlight.State.Red", "Failing");
	case LightState::Unknown:
		return tr("InFlight.State.Unknown", "Can't tell");
	}
	return QString();
}

QString eventKindText(IncidentEventKind k, const TrFn &tr)
{
	switch (k) {
	case IncidentEventKind::Started:
		return tr("InFlight.Event.Started", "started");
	case IncidentEventKind::Escalated:
		return tr("InFlight.Event.Escalated", "got worse");
	case IncidentEventKind::Ended:
		return tr("InFlight.Event.Ended", "ended");
	case IncidentEventKind::Acknowledged:
		return tr("InFlight.Event.Acknowledged", "acknowledged");
	}
	return QString();
}

} // namespace inflight
