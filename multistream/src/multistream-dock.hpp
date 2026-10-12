/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "config.hpp"
#include "dock-ui.hpp"
#include "importers.hpp"
#include "obs-audio.hpp"
#include "output-runner.hpp"
#include "platform-limits.hpp"
#include "retry-governor.hpp"
#include "session-report.hpp"
#include "update-check.hpp"

#include <QAbstractButton>
#include <QDialog>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <obs-hotkey.h>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>
#include <map>
#include <set>
#include <memory>
#include <vector>

class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

class MultistreamDock : public QWidget {
	Q_OBJECT

public:
	explicit MultistreamDock(QWidget *parent = nullptr);
	// (OBS themes can change at runtime)

	~MultistreamDock() override;

	// Called from the OBS frontend event callback (UI thread).
	void onStreamingStarted();
	void onStreamingStopping();
	void onProfileChanging();
	void onProfileChanged();
	void shutdown();

	// ---- host (Stream Kit dock) interface ----
	// One line for the host's section header: "Ready on 2 of 3", "Live on 2 of 2", "Connecting 1 of 2" ...
	QString summary() const { return summaryText; }
	// True while any destination's output is starting, live, reconnecting or stopping (the Kit refuses to switch
	// a tool off then).
	bool anyOutputActive() const
	{
		for (const auto &[id, r] : runners)
			if (r && r->busy())
				return true;
		return false;
	}

	// ---- external control: hotkeys, obs-websocket, Stream Deck ----------
	struct DestinationInfo {
		QString id, name, platform, state;
		bool enabled = false;
		double kbps = 0.0;
	};
	std::vector<DestinationInfo> snapshot();
	// The same list for Pre-Flight and In-Flight, which run in this process: read on the UI thread (their 1 Hz timers),
	// copied out, nothing held. std::nullopt = "not available" once this dock is shutting down (the Kit also never
	// hands the bridge out when Multistream is switched off, so the tools then see "Multistream not found"). This replaces
	// the obs-websocket GetDestinations round trip they used to make from worker threads; that request stays for external clients.
	std::optional<std::vector<DestinationInfo>> destinationsNow();

	// ---- in-process view for the Kit's Network light (In-Flight) ----
	// What In-Flight needs to judge each destination's outgoing stream separately. Read on the UI thread, in the same
	// process, by In-Flight's 1 Hz tick (the runners and their obs_output pointers are UI-thread objects, see
	// output-runner.hpp, so there is no lock to take and no ref to hold: the numbers are copied out at once and
	// nothing keeps a pointer). Not for any other thread.
	struct OutputSnapshot {
		enum class State { Off, Live, Reconnecting, Other };
		QString id, name;
		State state = State::Off;
		uint64_t dropped = 0; // obs_output_get_frames_dropped of the current output (0 when there is none)
		uint64_t total = 0;   // obs_output_get_total_frames of the same output
		uint64_t serial = 0;  // OutputRunner::outputSerial(): changes when the output is replaced or restarted
	};
	std::vector<OutputSnapshot> outputSnapshots();
	// The Network light's verdict per destination: the 30 s drop percentage of each destination it currently judges
	// bad (amber or red because of dropped frames). Those rows get a small "x.x% DROPPED" badge next to LIVE. The
	// full set every time: a destination not in the map has no badge.
	void setDropFigures(const std::map<QString, double> &pctById);
	// Test hook: one log line with the geometry numbers of the destination list (see FitScrollArea::diagnose).
	void testLogListGeometry(const char *tag);
	// The destination's own switch (the row's toggle, a hotkey and obs-websocket all end up here): off stops
	// that output only and keeps the setting; on starts it at once when OBS is streaming (through the connection budget).
	void setOn(const QString &id, bool on);
	// mode: 0 = off, 1 = on, 2 = toggle. Matches an id, or a name (case-
	// insensitive). Returns false if nothing matched.
	bool setEnabledByKey(const QString &idOrName, int mode, DestinationInfo *out = nullptr);
	void setAll(bool on);
	// Told about every state change; obs-websocket turns these into events.
	std::function<void(const DestinationInfo &)> stateListener;

signals:
	// The summary line changed (emitted from refreshSummary).
	void summaryChanged(const QString &summary);

protected:
	void changeEvent(QEvent *e) override;

private:
	int snapshotCount = 0;
	QString snapshotDir;
	void reload();
	void save();
	void rebuild();
	void refreshCard(const QString &id);
	ui::RowView rowView(const Destination &d);
	void applyTheme();
	void takeSnapshot(); // RWS_MULTISTREAM_SNAPSHOT: render the dock to PNGs for review
	void refreshAll();
	void refreshSummary();

	// (The update notice and its scheduler live in the Kit dock now: one scheduler, independent of which
	// tools are switched on.)
	QString summaryText;

	Destination *find(const QString &id);
	OutputRunner *runner(const QString &id);
	void startWhenMainReady();

	// Persistent reconnect, always through the connection budget.
	void scheduleRetry(const QString &id, int64_t delayMs, bool heldByBudget);
	void cancelRetry(const QString &id);
	void onRunnerChanged(const QString &id);

	DestinationInfo info(const Destination &d);
	void notify(const QString &id);

	void syncHotkeys();      // (re)register one toggle hotkey per destination
	void captureHotkeys();   // copy current bindings into cfg before saving
	void unregisterHotkeys();
	static void onHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed);
	// skipMainWait: start a shared-encoder destination even though OBS's own
	// stream has not produced a frame yet (used once the wait has timed out).
	void startOne(const QString &id, bool skipMainWait = false);
	void stopOne(const QString &id);
	void stopAll(bool immediate);

	void addDestination();
	void editDestination(const QString &id);

	MultistreamConfig cfg;
	std::map<QString, std::unique_ptr<OutputRunner>> runners;
	std::map<QString, ui::DestinationRow *> rows;

	QLabel *headline = nullptr;
	QLabel *subline = nullptr;
	ui::BeaconStrip *strip = nullptr;
	QPushButton *addButton = nullptr;
	QVBoxLayout *list = nullptr;
	ui::Theme theme;
	QTimer statsTimer;
	// Upload capacity: measured (Cloudflare speed test) or entered by hand.
	GlobalSettings globals;
	QLabel *note = nullptr;           // upload estimate and warnings, under the strip
	QToolButton *menuButton = nullptr;
	std::unique_ptr<std::thread> uploadThread;
	std::atomic<bool> uploadCancel{false};
	bool uploadTesting = false;
	double uploadProgressSeconds = 0.0;
	double uploadProgressMbps = 0.0;
	void startUploadTest();
	void finishUploadTest(bool ok, double mbps, const QString &error);
	void enterUploadSpeed();

	// Import from obs-multi-rtmp / Aitum Multistream (current OBS profile).
	std::vector<ImportCandidate> findImportCandidates();
	void addImported(const std::vector<ImportCandidate> &chosen);
	std::set<QString> existingServerKeys();
	void importDestinations();
	int importableCount = 0; // shown as a nudge while the dock is empty

	// End-of-stream report: what happened on each destination.
	SessionReport report;
	QLabel *recap = nullptr;   // "Last stream: ..." line under the note
	QString reportsDir;        // plugin_config/rws-multistream/reports
	int64_t lastTrafficMs = 0;
	QString reportStamp;       // file name part: when the stream started
	SessionReport::Phase phaseFor(const Destination &d);
	void updateReport(const QString &id);
	void finishReport();

	// What each platform accepts (from OBS's services.json), and advice when a
	// destination sends more.
	std::map<QString, PlatformLimits> limits;
	std::vector<LimitIssue> limitIssues(const Destination &d);
	void showReport();
	double plannedUploadKbps(); // what the enabled platforms will need, with overhead

	// OBS's own stream, measured the same way as destinations, so the header
	// can show the TOTAL upload: the number people know from their internet plan.
	void pollMainStream();
	uint64_t mainLastBytes = 0;
	qint64 mainLastMs = 0;
	double mainKbps = 0.0;
	bool closing = false;
	// Every modal dialog this dock has open, outermost first (an edit dialog, and a message box opened over it by
	// a handler that runs while it is up, can both be open). shutdown() rejects them innermost first; nothing
	// else's dialogs are touched. The Kit dock relies on this: it no longer walks the application's modal stack.
	std::vector<QPointer<QDialog>> modals_;
	int execModal(QDialog *dlg);
	// A message box as a heap, tracked modal (a static QMessageBox::question/warning/information would be an
	// untracked exec loop that a shutdown could not end). Returns the clicked standard button, or NoButton when it
	// was closed without one (also when shutdown ended it).
	QMessageBox::StandardButton messageBox(QMessageBox::Icon icon, const QString &title, const QString &text,
					       QMessageBox::StandardButtons buttons = QMessageBox::Ok);
	ObsStreamAudio obsAudio;

	// The connection budget itself is connectionBudget(id) (connection-budget.hpp),
	// shared with the Twitch bandwidth test.
	struct RetryState {
		int autoFailures = 0;    // consecutive automatic retries without success
		QTimer *timer = nullptr; // pending retry, if any
		int64_t retryAtMs = 0;
		bool heldByBudget = false; // waiting because the budget is spent
		OutputRunner::State lastState = OutputRunner::State::Idle;
		// Quick reconnect after a live connection dropped (replaces libobs' own
		// reconnect, which bypassed the budget). Cleared on Live or a user stop.
		bool recovering = false;
		int dropAttempts = 0; // quick reconnects made since the drop
	};
	std::map<QString, RetryState> retries;
	std::map<QString, double> dropFigures_; // see setDropFigures

	std::map<obs_hotkey_id, QString> hotkeyDest; // toggle hotkey -> destination
	obs_hotkey_id allOnHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id allOffHotkey = OBS_INVALID_HOTKEY_ID;

	// Shared-encoder destinations wait here until OBS's own stream is
	// actually sending frames; attaching earlier can stall the output.
	std::set<QString> waitingForMain;
	QTimer mainReadyTimer;
	qint64 mainWaitStartedMs = 0;
};
