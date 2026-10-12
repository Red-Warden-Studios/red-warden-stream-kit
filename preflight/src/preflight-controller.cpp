/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "preflight-controller.hpp"
#include "kit-cost.hpp"

#include <obs.h>
#include <plugin-support.h>
#include <util/platform.h>

using namespace preflight;

namespace {
qint64 nowMs()
{
	return static_cast<qint64>(os_gettime_ns() / 1000000ULL);
}

bool sameResults(const QVector<CheckResult> &a, const QVector<CheckResult> &b)
{
	if (a.size() != b.size())
		return false;
	for (int i = 0; i < a.size(); ++i) {
		if (a[i].id != b[i].id || a[i].state != b[i].state || a[i].title != b[i].title ||
		    a[i].message != b[i].message)
			return false;
	}
	return true;
}
} // namespace

PreflightController::PreflightController(QObject *parent) : QObject(parent), mic_(this), timer_(this)
{
	testHooks_ = qEnvironmentVariableIsSet("RWS_PREFLIGHT_TEST");
	settings_ = Settings::load();
	syncTicks();

	connect(&mic_, &MicTest::numbersChanged, this, &PreflightController::micNumbersChanged);
	connect(&mic_, &MicTest::runningChanged, this, [this](bool r) {
		emit micTestRunningChanged(r);
		if (!r && !shutdown_)
			tick();
	});

	timer_.setInterval(1000);
	connect(&timer_, &QTimer::timeout, this, &PreflightController::tick);
	timer_.start();
	probe_.start();
}

PreflightController::~PreflightController()
{
	shutdown();
}

void PreflightController::releaseSources()
{
	sourcesReleased_ = true;
	mic_.cancel(); // detaches and destroys the volmeter, drops the source ref
}

void PreflightController::shutdown()
{
	if (shutdown_)
		return;
	shutdown_ = true;
	timer_.stop();
	releaseSources();
	probe_.stop(); // nothing to wait for: the probe is a direct call on this thread; this just drops the source
	obs_.resetFrameWindows();
	obs_.shutdown(); // stops the disk worker (short bounded wait)
}

QString PreflightController::stateName(State s)
{
	switch (s) {
	case State::Green:
		return QStringLiteral("Green");
	case State::Amber:
		return QStringLiteral("Amber");
	case State::Red:
		return QStringLiteral("Red");
	case State::Unknown:
		return QStringLiteral("Unknown");
	case State::Hidden:
		return QStringLiteral("Hidden");
	}
	return QStringLiteral("?");
}

QString PreflightController::checkName(CheckId id)
{
	switch (id) {
	case CheckId::Setup:
		return QStringLiteral("Setup");
	case CheckId::MicMuted:
		return QStringLiteral("MicMuted");
	case CheckId::MicTest:
		return QStringLiteral("MicTest");
	case CheckId::Desktop:
		return QStringLiteral("Desktop");
	case CheckId::Scene:
		return QStringLiteral("Scene");
	case CheckId::MainStream:
		return QStringLiteral("MainStream");
	case CheckId::Multistream:
		return QStringLiteral("Multistream");
	case CheckId::Recording:
		return QStringLiteral("Recording");
	case CheckId::Performance:
		return QStringLiteral("Performance");
	case CheckId::Custom:
		return QStringLiteral("Custom");
	}
	return QStringLiteral("?");
}

void PreflightController::syncTicks()
{
	ticks_.resize(settings_.customItems.size()); // new slots are false, existing ticks kept
}

void PreflightController::setSettings(const Settings &s)
{
	if (shutdown_)
		return;
	const bool micChanged = s.micUuid != settings_.micUuid;
	const bool itemsChanged = s.customItems != settings_.customItems;
	settings_ = s;
	if (!settings_.save())
		obs_log(LOG_WARNING, "could not save settings");
	if (micChanged) {
		mic_.cancel(); // also forgets any earlier pass
	}
	if (itemsChanged) {
		ticks_.clear(); // list changed: start the ticks over rather than guess which moved where
		syncTicks();
	}
	probe_.poke();
	tick();
}

void PreflightController::resetForNewStream()
{
	if (shutdown_)
		return;
	ticks_.fill(false);
	mic_.cancel(); // also forgets an earlier pass
	obs_.resetNetworkWindow();
	obs_.resetFrameWindows();
	probe_.poke();
	tick();
}

void PreflightController::setMultistreamSource(MultistreamProbe::Source src)
{
	if (shutdown_)
		return;
	// No tick() here: this runs while the Kit is still loading (scenes and sources are not there yet), so a tick now would
	// judge the mic and scenes too early. The next 1 s tick asks Multistream straight away (setSource marks the probe due).
	probe_.setSource(std::move(src));
}

void PreflightController::startMicTest()
{
	if (shutdown_ || sourcesReleased_ || settings_.micUuid.isEmpty())
		return;
	mic_.start(settings_.micUuid);
}

void PreflightController::setItemTicked(int index, bool ticked)
{
	if (index < 0 || index >= ticks_.size() || ticks_[index] == ticked)
		return;
	ticks_[index] = ticked;
	tick();
}

GoLiveDecision PreflightController::goLiveDecision() const
{
	return decideGoLive(results_);
}

void PreflightController::startStreaming()
{
	obs_frontend_streaming_start();
}

void PreflightController::handleFrontendEvent(obs_frontend_event event)
{
	if (shutdown_)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING: {
		tick(); // fresh results, not up to a second old
		const GoLiveDecision d = goLiveDecision();
		startedWithReds_ = !d.reds.isEmpty();
		startedReds_ = d.reds;
		break;
	}
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		obs_.resetNetworkWindow();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		ticks_.fill(false);
		mic_.expire();
		obs_.resetNetworkWindow();
		startedWithReds_ = false;
		startedReds_.clear();
		tick();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		mic_.cancel();
		if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
			tick();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		// A running test listened to the old profile's audio setup; its result would mean nothing now.
		// (An earlier finished result is kept: the mic source itself did not change.)
		if (mic_.running())
			mic_.cancel();
		obs_.resetFrameWindows();
		obs_.resetRecordingCache();
		tick();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		probe_.poke(); // everything has loaded; ask Multistream again now
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		shutdown();
		break;
	default:
		break;
	}
}

void PreflightController::tick()
{
	kitcost::Scope costScope(kitcost::PreflightTick); // test-only timing, see kit-cost.hpp
	if (shutdown_)
		return;
	ObsProbe::Runtime rt;
	rt.nowMs = nowMs();
	probe_.poll(rt.nowMs); // direct, in-process: Multistream's dock on this thread
	const MultistreamResult ms = probe_.result();
	rt.multistreamStatus = ms.status;
	rt.multistreamNames = ms.names;
	rt.micTest = mic_.lastResult();
	rt.ticks = ticks_;

	const Inputs in = obs_.gather(settings_, rt);
	const QVector<CheckResult> res = evaluate(in);
	logStates(res);
	if (!sameResults(res, results_)) {
		results_ = res;
		emit resultsChanged(results_);
	}
}

void PreflightController::logStates(const QVector<CheckResult> &res)
{
	if (!testHooks_)
		return;
	for (const CheckResult &r : res) {
		const int idx = static_cast<int>(r.id);
		if (idx >= lastStates_.size())
			lastStates_.resize(idx + 1, -1);
		if (lastStates_[idx] == static_cast<int>(r.state))
			continue;
		lastStates_[idx] = static_cast<int>(r.state);
		obs_log(LOG_INFO, "state %s=%s", checkName(r.id).toUtf8().constData(),
			stateName(r.state).toUtf8().constData());
		// Test hook only: the message text of a few checks, so the harness can assert on wording and
		// destination names. Never MainStream (its text is near the stream key); never a path.
		if (r.id == CheckId::MicMuted || r.id == CheckId::Multistream)
			obs_log(LOG_INFO, "message %s: %s", checkName(r.id).toUtf8().constData(),
				r.message.toUtf8().constData());
	}
}
