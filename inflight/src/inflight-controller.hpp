/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "audio-watch.hpp"
#include "inflight-eval.hpp"
#include "inflight-probe.hpp"
#include "inflight-settings.hpp"
#include "multistream-live.hpp"
#include "stream-signals.hpp"

#include <obs-frontend-api.h>

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <map>
#include <vector>

// Owns the evaluator and the probe pieces and runs the 1 Hz tick on the UI thread. Everything here
// is UI thread only except what the signal callbacks do on their own threads.
class InflightController : public QObject {
	Q_OBJECT
public:
	explicit InflightController(QObject *parent = nullptr);
	~InflightController() override;

	// At OBS_FRONTEND_EVENT_FINISHED_LOADING. Idempotent.
	void start(const inflight::Settings &s);
	// After the settings change (an import): new thresholds, same session.
	void setSettings(const inflight::Settings &s);
	// The settings the controller is judging with (read back by the apply-path test hook).
	const inflight::Settings &currentSettings() const { return settings_; }
	void handleFrontendEvent(obs_frontend_event event);
	// Idempotent. Stops the timer, drops the output ref and lets go of the Multistream source.
	void shutdown();
	// Idempotent. Detaches the audio watch (volmeters + their source refs) and keeps it detached. Called at
	// OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN, which OBS emits BEFORE ClearSceneData(); shutdown() calls it too.
	void releaseSources();

	// From the lights UI (UI thread): act on the light's open incident now.
	void acknowledge(inflight::Light l);
	void snooze(inflight::Light l);
	// The Network light's link to Multistream, all in this process and all on the UI thread (the Kit wires it
	// when Multistream is loaded; without it the main stream is judged alone, as before):
	//   read     - the destinations as they are right now (copied out, nothing held);
	//   turnOff  - stops sending to one destination, the same path as its own switch;
	//   showDrops- told, once per change, the 30 s drop percentage of every destination judged bad (Multistream's row badge).
	struct OutputBridge {
		std::function<std::vector<inflight::OutputSample>()> read;
		std::function<void(const QString &outputId)> turnOff;
		std::function<void(const std::map<QString, double> &pctById)> showDrops;
	};
	void setOutputBridge(OutputBridge bridge);
	// Where "is any Multistream destination live?" comes from (the Kit wires it when Multistream is loaded; see
	// MultistreamLive). Called on the UI thread at the 2 s poll. Never set = Multistream not found (Off).
	void setMultistreamSource(inflight::MultistreamLive::Source src);
	// From the Network details' "Turn off <destination>" button (UI thread).
	void turnOffOutput(const QString &outputId, const QString &outputName);
	// The Kit's Reset: forget acknowledgements, snoozes and a latched stream drop (Evaluator::forgetIncidentMarks).
	void forgetIncidentMarks();

signals:
	// Emitted from tick(), on the UI thread. nowMs is the steady clock the snapshot was taken at.
	void snapshotReady(const inflight::Snapshot &snap, inflight::LiveState live, qint64 nowMs);

private:
	void tick();

	inflight::Settings settings_;
	inflight::Evaluator evaluator_;
	inflight::Probe probe_;
	inflight::StreamSignals sig_;
	inflight::MultistreamLive ms_;
	inflight::AudioWatch audio_;
	QTimer timer_;
	bool started_ = false;
	bool stopped_ = false;
	bool suspended_ = false;
	bool sourcesReleased_ = false; // releaseSources() ran: the audio watch must not attach again
	bool inTick_ = false;

	// Change tracking for the logs.
	bool haveLastLive_ = false;
	inflight::LiveState lastLive_ = inflight::LiveState::Off;
	bool testHooks_ = false;
	bool forceLive_ = false; // test only: RWS_INFLIGHT_TEST and RWS_INFLIGHT_TEST_FORCE_LIVE both set
	bool forceLiveLogged_ = false;
	QString lastProbeLine_;
	OutputBridge bridge_;
	std::map<QString, double> lastDrops_; // what showDrops was last told
	QString lastNetLine_;
	inflight::LightResult lastNetwork_; // the Network light of the latest update: the click's last look reads it
};
