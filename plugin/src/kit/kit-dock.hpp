/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// The ONE "RW Stream Kit" dock. It holds the three tools' panels (created
// by their modules, handed over by plugin-main.cpp), the stage strip, the Multistream section pinned on
// top, the "Stream ended" card, the Kit-level update notice and the Kit settings. The stage logic is
// kitstage::Machine (pure, unit-tested); this class is the Qt/OBS shell around it. UI thread only.
//
// Panels: each tool's panel is a child of this dock once it is built. A tool that is switched off has no
// panel (nullptr) and its module's load() was never called: nothing of it exists in this process.
//
// Shutdown order: plugin-main's kitFrontendEvent runs every tool's EXIT shutdown (workers, timers, vendor
// requests, dialogs; Multistream first) and then this dock's shutdown(), which only stops what the dock itself
// owns (the 1 Hz timer, the update check). No step runs the event loop. The panels die with the dock, after
// their EXIT handling, when Qt deletes OBS's main window (before libobs shuts down).

#include "inflight-eval.hpp"
#include "kit-settings.hpp"
#include "kit-settings-dialog.hpp"
#include "kit-stage.hpp"
#include "update-check.hpp"

#include <obs-frontend-api.h>

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <vector>

class InflightDock;
class MultistreamDock;
class PreflightDock;
class QCheckBox;
class QDockWidget;
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class KitSectionHeader;
class LevelsPanel;

class KitDock : public QWidget {
	Q_OBJECT
public:
	static constexpr const char *kDockId = "rws-stream-kit";
	static constexpr const char *kTitle = "RW Stream Kit";

	struct Panels {
		MultistreamDock *multistream = nullptr;
		PreflightDock *preflight = nullptr;
		InflightDock *inflight = nullptr;
	};

	KitDock(const Panels &panels, const kitsettings::Settings &settings, QWidget *parent = nullptr);
	~KitDock() override;

	// Routed from the Kit's own frontend event callback (UI thread).
	void handleFrontendEvent(obs_frontend_event event);
	// OBS_FRONTEND_EVENT_EXIT: stop the 1 Hz timer and the update check. Idempotent (closing_): the destructor calls
	// it again. Runs no event loop.
	void shutdown();
	// Called by plugin-main right after the dock is registered. On the very first run OBS resets its dock layout
	// right afterwards (it shows and re-floats every extra dock once), so the dock is muted (Qt::WA_DontShowOnScreen:
	// rendered offscreen, never mapped) until dockFirstRun() (FINISHED_LOADING) clears that and docks it right.
	void prepareFirstRun();

	// ---- read back by the tests ----
	const kitstage::Machine &machine() const { return machine_; }
	const kitsettings::Settings &settings() const { return settings_; }

protected:
	void changeEvent(QEvent *e) override;
	bool eventFilter(QObject *o, QEvent *e) override;

private slots:
	void onLiveReport(inflight::LiveState live, quint64 sessionId);
	void onAttentionChanged(bool needsAttention);
	void onTaskbarAlert();
	void onTick();

private:
	// What the view shows, to notice when it has to be refreshed.
	struct ViewKey {
		kitstage::Stage stage = kitstage::Stage::PreFlight;
		bool peeking = false, active = false, ended = false, resetAllowed = true, showAll = false;
		bool operator==(const ViewKey &o) const
		{
			return stage == o.stage && peeking == o.peeking && active == o.active && ended == o.ended &&
			       resetAllowed == o.resetAllowed && showAll == o.showAll;
		}
		bool operator!=(const ViewKey &o) const { return !(*this == o); }
	};
	ViewKey viewKey() const;

	void buildHeader(QVBoxLayout *root);
	void buildBody(QVBoxLayout *root);
	QFrame *buildEndedCard(QWidget *parent);
	void applyTheme();
	void refreshView();
	void refreshLive();
	void refreshStrip();
	void refreshSections();
	void setMultistreamCollapsed(bool collapsed, bool save);
	void setShowAll(bool on, bool save);
	void doReset();
	void doDone();
	void openSettings();
	// A tool must not be switched off now: OBS is streaming, any Multistream output is active, or a Kit session is.
	bool toolOffBlocked() const;
	bool levelsSectionShown() const;
	void logLevels(const char *tag);
	void openManual();
	void openManualAt(const QString &anchor); // the manual page at #anchor (empty = the manual's front page)
	void showUpdate(const updatecheck::Manifest &m);
	bool saveSettings();
	// Levels gate: the Levels meters run only while the Levels section is shown, expanded AND at least partly inside the
	// scroll viewport of this dock. Recomputed on page / expand changes, on scrolling, resizing and layout changes, and by
	// the 1 Hz tick as a self-check.
	bool levelsInViewport() const;
	void updateLevelsGate();
	void scheduleLevelsGate();
	// First-run marker (see kit-settings.hpp): verified persist, and "does OBS already have a place for the dock?".
	bool persistFirstRunDone();
	bool savedLayoutHasDock() const;
	QDockWidget *hostDock() const;
	void dockFirstRun();
	void setTitleMark(bool on);
	static qint64 nowMs();
	static QString clockText(qint64 ms);

	// ---- test hooks (RWS_KIT_TEST + RWS_KIT_TEST_SHOT_DIR; offscreen grabs only, nothing is shown) ----
	void installTestHooks();
	bool logLevelsStep(const char *tag, bool scrollToLevels); // false = a pump ran OBS's exit
	void runShots();
	bool saveShot(const QString &name, QWidget *target = nullptr); // target: a child to photograph instead of the whole dock. false = a pump ran OBS's exit; the caller must stop
	void pollShowcase();                // the opt-in showcase run: command files from the harness (see kit-dock.cpp)
	void runLevelsLabelShots();         // LUFS labels at 360 / 300 px, Reference on / off, "?" help link (parentless demo panels)
	// Pump events for a test hook and report whether this dock is still alive and not shutting down (a pump can run
	// OBS's exit). The hook must stop at once on false.
	bool testPump();

	// The tools' panels, by QPointer: they are children of this dock's sections and Qt deletes them with it. Every
	// handler re-reads them, and a deleted one reads as null instead of dangling. (The Levels panel is not held
	// here at all: it is read through InflightDock::levelsPanel(), which owns it logically; this dock only hosts it.)
	struct PanelRefs {
		QPointer<MultistreamDock> multistream;
		QPointer<PreflightDock> preflight;
		QPointer<InflightDock> inflight;
	};
	PanelRefs panels_;
	kitsettings::Settings settings_;
	kitstage::Machine machine_;
	ViewKey shown_;
	bool closing_ = false;
	bool testPreview_ = false; // a screenshot is previewing a state the machine is not in
	bool inTheme_ = false;     // applyTheme is running (setStyleSheet sends PaletteChange to this very widget)
	QString lastCss_;

	// header
	QWidget *header_ = nullptr;
	QWidget *strip_ = nullptr;
	struct StageButton {
		kitstage::Stage stage;
		QPushButton *button;
	};
	std::vector<StageButton> stageButtons_;
	std::vector<QLabel *> stageArrows_;
	QToolButton *helpBtn_ = nullptr;
	QToolButton *gearBtn_ = nullptr;
	QCheckBox *showAllBox_ = nullptr;
	QLabel *liveLabel_ = nullptr;
	QPushButton *resetBtn_ = nullptr;
	QLabel *updateBanner_ = nullptr;
	QPointer<KitSettingsDialog> settingsDlg_; // the open Kit settings dialog (heap; rejected by shutdown)
	int bannerTicks_ = 0;

	// body
	QScrollArea *scroll_ = nullptr;
	QWidget *content_ = nullptr;
	struct Section {
		QWidget *wrap = nullptr;
		KitSectionHeader *head = nullptr;
		QWidget *body = nullptr;
		bool collapsed = false; // Show-all only (not saved)
	};
	Section msSection_, pfSection_, ifSection_;
	QWidget *levelsWrap_ = nullptr; // the Kit-owned Levels section (holds In-Flight's LevelsPanel)
	bool showcaseBusy_ = false;      // pollShowcase is working on a command (it pumps events)
	bool testLevelsHidden_ = false; // test hook: pretend the Levels section is not shown
	bool levelsGatePending_ = false; // a deferred updateLevelsGate() is queued (layout / resize events come in bursts)
	bool skipFirstRun_ = false;      // first_run_done was lost but OBS has a saved place for the dock: never force it
	bool firstRunPersisted_ = false; // first_run_done is verified on disk
	QFrame *endedCard_ = nullptr;
	QWidget *endedHolder_ = nullptr; // the margin around the card (hidden with it)
	QLabel *endedTime_ = nullptr;
	QLabel *endedLog_ = nullptr;

	// live bookkeeping (steady ms)
	QTimer tick_;
	qint64 sessionStartMs_ = 0;
	qint64 lastLiveMs_ = 0;
	uint32_t startedSeen_ = 0;
	bool wasActive_ = false;
	bool wasEnded_ = false;
	bool titleMarked_ = false;
	QString msSummary_;

	// updates (one scheduler for the whole Kit)
	UpdateChecker updates_;
	updatecheck::Manifest pendingUpdate_;
};
