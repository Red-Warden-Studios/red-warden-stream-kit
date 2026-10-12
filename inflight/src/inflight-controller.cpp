/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-controller.hpp"
#include "inflight-clock.hpp"
#include "kit-cost.hpp"
#include "inflight-probe-logic.hpp"
#include "inflight-ui-logic.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QtGlobal>

#include <cmath>
#include <utility>

using namespace inflight;

namespace {
const char *liveName(LiveState s)
{
	switch (s) {
	case LiveState::Off:
		return "off";
	case LiveState::Live:
		return "live";
	case LiveState::Unknown:
		return "unknown";
	}
	return "?";
}

const char *msName(MsStatus s)
{
	switch (s) {
	case MsStatus::Pending:
		return "pending";
	case MsStatus::NotFound:
		return "notfound";
	case MsStatus::Ok:
		return "ok";
	case MsStatus::Error:
		return "error";
	}
	return "?";
}

// Short names of the Network light's reasons for the test line.
const char *netReasonName(Reason r)
{
	switch (r) {
	case Reason::NetworkDrops:
		return "drops";
	case Reason::NetworkReconnecting:
		return "reconnecting";
	case Reason::NetworkWindowFilling:
		return "filling";
	case Reason::Ok:
		return "ok";
	case Reason::MainStreamOff:
		return "mainoff";
	default:
		return "other";
	}
}

const char *incidentKindName(IncidentEventKind k)
{
	switch (k) {
	case IncidentEventKind::Started:
		return "started";
	case IncidentEventKind::Escalated:
		return "escalated";
	case IncidentEventKind::Ended:
		return "ended";
	case IncidentEventKind::Acknowledged:
		return "acknowledged";
	}
	return "?";
}

// Light letters in the probe test line: g = grey, G = green, A = amber, R = red, U = unknown.
char lightLetter(LightState s)
{
	switch (s) {
	case LightState::Grey:
		return 'g';
	case LightState::Green:
		return 'G';
	case LightState::Amber:
		return 'A';
	case LightState::Red:
		return 'R';
	case LightState::Unknown:
		return 'U';
	}
	return '?';
}
} // namespace

InflightController::InflightController(QObject *parent) : QObject(parent)
{
	timer_.setInterval(1000);
	timer_.setTimerType(Qt::CoarseTimer);
	connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
}

InflightController::~InflightController()
{
	shutdown();
}

void InflightController::start(const Settings &s)
{
	if (started_ || stopped_)
		return;
	started_ = true;
	settings_ = s;
	testHooks_ = qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST");
	forceLive_ = testHooks_ && qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST_FORCE_LIVE");
	evaluator_.setThresholds(thresholdsFromSettings(settings_));
	ms_.start();
	sig_.sync();
	timer_.start();
	obs_log(LOG_INFO, "probe started");
}

void InflightController::setSettings(const Settings &s)
{
	settings_ = s;
	evaluator_.setThresholds(thresholdsFromSettings(settings_));
}

void InflightController::setOutputBridge(OutputBridge bridge)
{
	bridge_ = std::move(bridge);
}

void InflightController::setMultistreamSource(MultistreamLive::Source src)
{
	ms_.setSource(std::move(src));
}

void InflightController::handleFrontendEvent(obs_frontend_event event)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		shutdown();
		return;
	}
	if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		releaseSources();
		return;
	}
	if (!started_ || stopped_)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		if (!suspended_)
			sig_.sync();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (!suspended_) {
			sig_.sync();
			sig_.pushStarted(nowMs());
		}
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		probe_.pushRecordingEvent(RecordingEventKind::Started, nowMs());
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		probe_.pushRecordingEvent(RecordingEventKind::Stopped, nowMs());
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		probe_.pushRecordingEvent(RecordingEventKind::Paused, nowMs());
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		probe_.pushRecordingEvent(RecordingEventKind::Unpaused, nowMs());
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		tick(); // so A -> B -> A inside one second is not missed
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		suspended_ = true;
		audio_.detachAll();
		sig_.unbind();
		evaluator_.resetBaselines();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		suspended_ = false; // the next tick binds the output again
		break;
	default:
		break;
	}
}

// The audio watch holds a volmeter and a source reference per watched source. OBS clears the scenes and drops its
// own source references in ClearSceneData(), BEFORE it emits EXIT, so the watch lets go earlier, at SCRIPTING_SHUTDOWN.
// The flag keeps a timer tick that runs in between from attaching again.
void InflightController::releaseSources()
{
	sourcesReleased_ = true;
	audio_.detachAll();
}

void InflightController::shutdown()
{
	if (stopped_)
		return;
	stopped_ = true;
	timer_.stop();
	releaseSources();
	sig_.unbind();
	ms_.stop();
}

void InflightController::tick()
{
	kitcost::Scope costScope(kitcost::InflightTick); // test-only timing, see kit-cost.hpp
	if (suspended_ || stopped_ || !started_ || inTick_)
		return;
	inTick_ = true; // the signal handler below must not re-enter us

	const qint64 now = nowMs();
	sig_.sync();
	const bool mainActive = obs_frontend_streaming_active();
	const MsReply ms = ms_.poll(now); // in-process, UI thread: asks Multistream's dock directly every 2 s
	LiveState live = combineLive(mainActive, ms, now);
	if (forceLive_) {
		live = LiveState::Live;
		if (!forceLiveLogged_) {
			forceLiveLogged_ = true;
			obs_log(LOG_INFO, "TEST force-live");
		}
	}
	if (!sourcesReleased_)
		audio_.sync(settings_, live, now); // before gather: gather reads the levels
	Inputs in = probe_.gather(settings_, now, live, sig_, audio_);
	// Multistream's destinations, read here on the UI thread from its own outputs (no websocket, nothing held).
	if (bridge_.read)
		in.outputs = bridge_.read();
	const Snapshot snap = evaluator_.update(in, now);
	lastNetwork_ = snap.at(Light::Network);

	if (!haveLastLive_ || live != lastLive_) {
		if (haveLastLive_)
			obs_log(LOG_INFO, "live state: %s -> %s", liveName(lastLive_), liveName(live));
		else
			obs_log(LOG_INFO, "live state: %s", liveName(live));
		lastLive_ = live;
		haveLastLive_ = true;
	}

	if (testHooks_) {
		QString lights;
		for (const LightResult &l : snap.lights)
			lights += QLatin1Char(lightLetter(l.state));
		const QString line = QStringLiteral("probe: live=%1 main=%2 ms=%3 out=%4 lights=%5")
					     .arg(QString::fromLatin1(liveName(live)))
					     .arg(mainActive ? 1 : 0)
					     .arg(QString::fromLatin1(msName(ms.status)))
					     .arg(sig_.output() ? 1 : 0)
					     .arg(lights);
		if (line != lastProbeLine_) {
			lastProbeLine_ = line;
			obs_log(LOG_INFO, "%s", line.toUtf8().constData());
		}
	}

	// Tell Multistream which destinations the Network light currently judges bad because of dropped frames
	// (its row shows the figure). Only on a change.
	if (bridge_.showDrops) {
		std::map<QString, double> drops;
		for (const NetOutputView &v : snap.netOutputs)
			if (!v.id.isEmpty() && v.reason == Reason::NetworkDrops && (v.state == LightState::Amber || v.state == LightState::Red))
				drops[v.id] = std::round(v.pct * 10.0) / 10.0;
		if (drops != lastDrops_) {
			lastDrops_ = drops;
			bridge_.showDrops(drops);
		}
	}

	if (testHooks_) {
		// One line per change, for the harness: the Network light's verdict, the output it names, its 30 s and
		// session figures, who else is fine, and every judged output (G/A/R/U + 30 s percentage).
		const LightResult &n = snap.at(Light::Network);
		QString views;
		for (const NetOutputView &v : snap.netOutputs)
			views += QStringLiteral("%1%2:%3:%4")
					 .arg(views.isEmpty() ? QString() : QStringLiteral(","))
					 .arg(v.id.isEmpty() ? QStringLiteral("main") : v.id)
					 .arg(QLatin1Char(lightLetter(v.state)))
					 .arg(v.pct, 0, 'f', 1);
		QString fine = n.net.mainFine ? QStringLiteral("main") : QString();
		for (const QString &d : n.net.fineDestinations)
			fine += (fine.isEmpty() ? QString() : QStringLiteral(",")) + d;
		const QString line = QStringLiteral("net: light=%1 reason=%2 out=%3 pct30=%4 session=%5 fine=%6 views=%7")
					     .arg(QLatin1Char(lightLetter(n.state)))
					     .arg(QString::fromLatin1(netReasonName(n.reason)))
					     .arg(n.net.valid ? (n.net.outputId.isEmpty() ? QStringLiteral("main") : n.net.outputName)
							      : QStringLiteral("-"))
					     .arg(n.value, 0, 'f', 1)
					     .arg(n.net.sessionPct, 0, 'f', 1)
					     .arg(fine.isEmpty() ? QStringLiteral("-") : fine)
					     .arg(views.isEmpty() ? QStringLiteral("-") : views);
		if (line != lastNetLine_) {
			lastNetLine_ = line;
			obs_log(LOG_INFO, "%s", line.toUtf8().constData());
		}
		for (const IncidentEvent &e : snap.events)
			if (e.light == Light::Network)
				obs_log(LOG_INFO, "net: incident %s %s%s", incidentKindName(e.kind),
					e.outputName.isEmpty() ? "main" : e.outputName.toUtf8().constData(),
					e.turnedOff ? " (turned off)" : "");
	}

	emit snapshotReady(snap, live, now);
	inTick_ = false;
}

void InflightController::turnOffOutput(const QString &outputId, const QString &outputName)
{
	if (stopped_ || !started_ || !bridge_.turnOff || outputId.isEmpty())
		return;
	// The button was shown for the snapshot of a moment ago. Look again at the latest Network result and at the
	// destinations as they are now: a held incident can outlive the destination's being the worst output.
	const inflight::TurnOffCheck chk = inflight::checkTurnOff(lastNetwork_, outputId, bridge_.read ? bridge_.read() : std::vector<inflight::OutputSample>());
	if (chk != inflight::TurnOffCheck::Ok) {
		obs_log(LOG_INFO, "network: not turning off %s: %s", outputName.toUtf8().constData(), inflight::turnOffCheckName(chk));
		return;
	}
	obs_log(LOG_INFO, "network: turning off %s (from the Network details)", outputName.toUtf8().constData());
	bridge_.turnOff(outputId);
	tick(); // the destination reads as off now: show the recomputed light at once
}

void InflightController::acknowledge(Light l)
{
	if (stopped_ || !started_)
		return;
	evaluator_.acknowledge(l, nowMs());
	tick(); // the evaluator emits the event on the next update; show the result now
}

void InflightController::forgetIncidentMarks()
{
	if (stopped_ || !started_)
		return;
	evaluator_.forgetIncidentMarks(nowMs());
	tick();
}

void InflightController::snooze(Light l)
{
	if (stopped_ || !started_)
		return;
	evaluator_.snooze(l, nowMs());
	tick();
}
