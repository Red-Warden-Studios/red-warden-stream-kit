/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "mic-test.hpp"
#include "multistream-probe.hpp"
#include "obs-probe.hpp"
#include "preflight-eval.hpp"
#include "preflight-settings.hpp"

#include <obs-frontend-api.h>

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

// Everything between OBS and the evaluator. UI thread only. Owns the settings, the OBS probe, the
// mic test, the Multistream probe (a direct in-process question on this thread, no worker) and the 1 s timer.
class PreflightController : public QObject {
	Q_OBJECT
public:
	explicit PreflightController(QObject *parent = nullptr);
	~PreflightController() override;

	const preflight::Settings &settings() const { return settings_; }
	// Saves, expires the mic pass if the mic changed, pokes the Multistream probe, re-evaluates.
	void setSettings(const preflight::Settings &s);

	QVector<preflight::CheckResult> results() const { return results_; }

	// The Kit's Reset ("start fresh for the next stream"): clears the ticked custom items and the mic-test
	// result (a running test is cancelled), restarts the frame windows and re-runs every check right away,
	// as at startup.
	void resetForNewStream();

	// Where the Multistream check reads the destinations from (the Kit wires it when Multistream is loaded; never set =
	// "Multistream wasn't found"). Called on the UI thread, at most every 5 s. See MultistreamProbe.
	void setMultistreamSource(preflight::MultistreamProbe::Source src);

	void startMicTest();
	bool micTestRunning() const { return mic_.running(); }
	// Loudest / Typical of the last finished mic test (cleared by Reset, a mic change, a new test).
	preflight::MicNumbers micNumbers() const { return mic_.lastNumbers(); }
	void setItemTicked(int index, bool ticked);

	preflight::GoLiveDecision goLiveDecision() const;
	void startStreaming(); // obs_frontend_streaming_start(); the caller has already asked the user if needed

	// OBS's own Start Streaming went ahead while checks were red (for the dock's banner). Cleared
	// when the stream stops.
	bool startedWithReds() const { return startedWithReds_; }
	QStringList startedWithRedsList() const { return startedReds_; }

	void handleFrontendEvent(obs_frontend_event event);
	void shutdown(); // idempotent; call at OBS_FRONTEND_EVENT_EXIT before Qt objects die
	// Idempotent. Ends the mic test (volmeter + source reference) and keeps it from starting again. Called at
	// OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN, which OBS emits BEFORE ClearSceneData(); shutdown() calls it too.
	void releaseSources();

	static QString stateName(preflight::State s);
	static QString checkName(preflight::CheckId id);

signals:
	void resultsChanged(QVector<preflight::CheckResult> results);
	void micTestRunningChanged(bool running);
	void micNumbersChanged();

private:
	void tick();
	void logStates(const QVector<preflight::CheckResult> &res);
	void syncTicks();

	preflight::Settings settings_;
	preflight::ObsProbe obs_;
	preflight::MicTest mic_;
	preflight::MultistreamProbe probe_;
	QTimer timer_;
	QVector<bool> ticks_;
	QVector<preflight::CheckResult> results_;
	QVector<int> lastStates_;
	bool startedWithReds_ = false;
	QStringList startedReds_;
	bool testHooks_ = false;
	bool shutdown_ = false;
	bool sourcesReleased_ = false; // releaseSources() ran: no new mic test
};
