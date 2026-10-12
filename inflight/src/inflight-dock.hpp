/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "inflight-controller.hpp"
#include "inflight-settings.hpp"
#include "settings-dialog.hpp"
#include "update-check.hpp"

#include <obs-frontend-api.h>

#include <QPointer>
#include <QString>
#include <QWidget>

class DockVisibilityGate;
class LevelsPanel;
class LightsPanel;
class QFrame;
class QLabel;
class QPushButton;
class QShowEvent;

// The In-Flight dock: the lights panel (header dot + text, lights, details, session log) and a setup
// card shown until the user has chosen. All UI-thread only.
class InflightDock : public QWidget {
	Q_OBJECT
public:
	explicit InflightDock(QWidget *parent = nullptr);
	~InflightDock() override;

	// Routed from the frontend event callback (UI thread).
	void handleFrontendEvent(obs_frontend_event event);
	// Called at OBS_FRONTEND_EVENT_EXIT: stop the update worker and anything else with a thread or timer.
	void shutdown();
	// Called at OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN (before OBS clears its scenes): drops the Levels capture and the
	// audio watch with their source references. Idempotent.
	void releaseSources();

	// ---- host (Stream Kit dock) interface ----
	// The Kit's Reset ("start fresh"): forget acknowledged / snoozed incidents and clear the session log.
	void resetSession();
	// The host says whether the Levels meters may run as far as it is concerned: its Levels section is on screen
	// (Pre-Flight or In-Flight stage, or Show all) AND expanded. It is one of the Levels visibility gate's
	// inputs (the others: the Kit dock visible, not tabbed away, not minimised), recomputed here at once.
	void setShownByHost(bool shown);
	// The Levels panel. It is created and configured here (settings, persistence, gate, meters) but NOT placed
	// in this dock's layout: the host puts it in its own Levels section (a child of the host, under the stage
	// page). Until then it stays hidden.
	// Ownership: In-Flight is the single LOGICAL owner of the Levels panel. It creates and configures it, it alone
	// tears its capture down (shutdown() below, at EXIT, exactly once), and it holds it by QPointer. The Kit's Levels
	// section only HOSTS it: after the reparent the Qt parent is the Kit's wrapper widget, which is also what finally
	// deletes it (its destructor then has nothing left to release). May be null once the host has destroyed it.
	LevelsPanel *levelsPanel() const;
	// Test/diagnostic: "<hostShown> <dock-side verdict> <running>" of the Levels gate, for the host's log line.
	bool levelsHostShown() const;
	bool levelsGateVisible() const;
	bool levelsRunning() const;
	// Entries in the session log (the "Stream ended" card quotes it).
	int sessionLogCount() const;
	// The Network light's link to Multistream (see InflightController::OutputBridge). Set by the Kit when
	// Multistream is loaded; never set = the main stream alone is judged.
	void setOutputBridge(InflightController::OutputBridge bridge);
	// Where the live-state check reads Multistream's destination states from (the Kit wires it with Multistream loaded;
	// never set = Multistream not found). See MultistreamLive.
	void setMultistreamSource(inflight::MultistreamLive::Source src);

signals:
	// Every tick (1 Hz): In-Flight's three-state live result with a session id. The id counts real sessions:
	// it increases when Live follows a confirmed Off, never across Unknown (a reconnect is not a new session).
	void liveReport(inflight::LiveState live, quint64 sessionId);
	// "Something red needs you" changed (the dock title mark). The host owns the dock title.
	void attentionChanged(bool needsAttention);
	// A new red incident: the host raises the one taskbar alert.
	void taskbarAlertRequested();
	// Test hook only (RWS_INFLIGHT_TEST + RWS_INFLIGHT_TEST_NET): the Network light has been red on a Multistream
	// destination for a few ticks with its details open; the host may take its offscreen screenshot now.
	void testNetRed();

protected:
	// Show: recompute the Levels visibility gate at once (the panel starts its meters in the same call).
	void showEvent(QShowEvent *e) override;

private:
	// Re-bind the gate to the live dock state and push the verdict into the Levels panel now (attaches the
	// meters and starts the timers in this call if the panel is open and the dock is visible).
	void syncLevelsGate();
	void applySetupVisibility();
	void copyFromPreflight();
	// Reads Pre-Flight's file and applies it to the settings (saved, given to the controller). False = nothing was
	// applied; *failNote then names why when it can be said to the user. Shared by the button and the automatic copy.
	bool importPreflightChoices(QString *failNote);
	// Not set up and Pre-Flight has a file: copy its choices quietly (dock built, dock shown). Never once setup is done.
	void autoCopyFromPreflight();
	void applyPreflightNote();
	void dismissPreflightNote(bool openSettingsAfter);
	// The settings dialog, on the heap, one at a time. Normal use: exec() (returns nullptr). With
	// testOpenOnly (test hook) it is shown offscreen with open() and returned so the hook can drive it.
	InflightSettingsDialog *openSettings(bool testOpenOnly = false);
	void settingsFinished(InflightSettingsDialog *dlg, int rc);
	bool runSettingsShots();  // test hook: offscreen grabs of the dialog's tabs and the setup card; false = a pump ran OBS's exit, `this` may be gone
	void runApplyTests();     // test hook: the real apply path, driven offscreen (RWS_INFLIGHT_TEST_APPLY)
	QString preflightFile() const;
	void onSnapshot(const inflight::Snapshot &snap, inflight::LiveState live, qint64 nowMs);
	void onNewRed();
	void setTitleMark(bool on);
	void runUiShots(); // test hook: offscreen grabs of two parentless panels, never a visible window

	inflight::Settings settings;
	QFrame *setupCard = nullptr;
	QLabel *setupNote = nullptr;
	QPushButton *copyBtn = nullptr;
	QFrame *preflightNote = nullptr; // "Using your microphone and scenes from Pre-Flight." + Change + x
	bool autoCopied_ = false;        // this session's automatic copy completed the setup
	QString autoCopyStamp_;          // size + mtime of Pre-Flight's file at the last automatic attempt
	QPointer<InflightSettingsDialog> dialog_; // the open settings dialog, if any
	bool dialogOpenedUpdates = true;          // the shared update preference as the dialog opened
	// Child objects are held by QPointer: shutdown() and the handlers can run while Qt deletes things, and a QPointer
	// goes null instead of dangling.
	QPointer<LightsPanel> panel;
	QPointer<LevelsPanel> levels; // created as our child, then reparented into the Kit's Levels section (see levelsPanel())
	QPointer<DockVisibilityGate> gate_; // is the dock visible (tab, close, minimise): drives the Levels meters
	bool levelsGateTest = false;   // RWS_INFLIGHT_TEST and RWS_INFLIGHT_TEST_LEVELS_GATE both set
	void runGateTest();            // test hook: the gate in an offscreen QMainWindow with tabified docks
	bool levelsTest = false;       // RWS_INFLIGHT_TEST and RWS_INFLIGHT_TEST_LEVELS both set
	bool titleMarked = false;
	quint64 sessionId_ = 0;       // see liveReport
	bool lastDefiniteLive_ = false; // the last Live/Off report was Live
	bool uiTest = false;    // RWS_INFLIGHT_TEST and RWS_INFLIGHT_TEST_SHOT_DIR both set
	bool haveSnap = false;  // test hook only: the latest real snapshot
	// Test hook (RWS_INFLIGHT_TEST + RWS_INFLIGHT_TEST_NET): wait for the Network light to name a bad destination,
	// open its details, announce it (testNetRed), then press the real "Turn off <destination>" button once.
	bool netTest = false;
	int netBadTicks = 0;
	bool netClickScheduled = false;
	void runNetTest(const inflight::Snapshot &snap);
	inflight::Snapshot lastSnap;
	inflight::LiveState lastLive = inflight::LiveState::Off;
	qint64 lastNow = 0;
	QPointer<InflightController> controller; // child QObject; owns the evaluator and the probe
	UpdateChecker updates;
	bool checkUpdates = true;
	bool setupDeferred = false;
	bool closing = false;
	bool shutDown_ = false; // shutdown() has run (EXIT or the destructor): it is a no-op from then on
	// Test hooks only: pump the event loop, and say whether this dock is still alive and not shutting down. The
	// caller must stop at once on false (a pump can run OBS's whole exit).
	bool testPump();
};
