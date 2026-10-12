/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The collapsible Levels panel (dB mode): per watched source a post-fader peak (held) and average
// (smoothed) in dBFS, a slim level bar, and a gap line under the rows. UI thread only. It owns the
// volmeters (LevelsMeters) and two timers, and runs all of it ONLY while the panel is open AND the dock is
// visible AND nothing has suspended it (scene collection / profile change, shutdown):
//   - a FAST timer (kFastMs = 50 ms, 20 Hz) takes the meters' window, feeds PeakHold / AvgSmoother and
//     drives the BARS: the fill is the instantaneous peak of the last 50 ms window, a thin tick is the 1.5 s
//     held peak, a marker is the smoothed average;
//   - a TEXT timer (kTextMs = 200 ms, 5 Hz) drives the NUMBERS (Peak / Avg text, gap lines) so they stay
//     readable. A row's first reading is shown on the very fast tick that has it, not on the next text beat.
// The start is immediate: opening the panel, the dock's Show and every gate rebind recompute the gate and
// attach/start in the same call. A separate 1 Hz timer runs whenever the panel is open: it asks the owner to
// recheck the visibility gate (recheckRequested), the safety net behind those triggers, and
// resolves the rows' source names, so a row whose source exists but is not metered shows its real name
// and "--", and "(missing source)" only means OBS has no source with that uuid.
#pragma once

#include "inflight-ui-logic.hpp"
#include "levels-math.hpp"

#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <functional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QToolButton;
class QVBoxLayout;

namespace inflight {
class LevelsMeters;
class LevelsLufs;
}

class LevelsPanel : public QWidget {
	Q_OBJECT
public:
	explicit LevelsPanel(inflight::TrFn tr, QWidget *parent = nullptr);
	~LevelsPanel() override;

	static constexpr int kFastMs = 50;  // 20 Hz: meters taken, bars drawn
	static constexpr int kTextMs = 200; // 5 Hz: numbers and gap lines

	// The sources shown: the mic and desktop from settings (either may be empty) plus the user's extras.
	// UUIDs only; names come from the live source. Duplicates and empty entries are dropped.
	void setSources(const QString &micUuid, const QString &desktopUuid, const QStringList &extras);
	// Open/collapse. `notify` = emit openChanged (a user action); programmatic restores pass false.
	void setOpen(bool open, bool notify = true);
	bool isOpen() const { return open_; }
	// The dock reports its visibility (show/hide events: tabbed away, closed, minimised).
	void setDockVisible(bool visible);
	// Scene collection / profile change in progress: detach and stop; false = resume if still wanted.
	void setSuspended(bool suspended);
	// Detach and stop for good (dock shutdown / EXIT). Idempotent. When it returns, no audio capture callback is
	// registered, the loudness worker is joined and every volmeter is detached: no libobs thread can reach this widget.
	void shutdown();

	// True while the meters are attached and the timer runs (the gate is satisfied).
	bool running() const { return running_; }
	QStringList extras() const { return extras_; }
	// The meter the panel shows. dB (default): volmeters, peak and average. LUFS: audio capture callbacks and the
	// loudness worker (levels-lufs.hpp), momentary and short-term, an optional true peak and an optional
	// reference tick; only one of the two backends ever runs. `notify` = emit the change signal (a user action).
	void setMode(inflight::LevelsMode mode, bool notify = true);
	inflight::LevelsMode mode() const { return mode_; }
	// True peak (LUFS mode only, off by default: oversampling costs real CPU).
	void setTruePeak(bool on, bool notify = true);
	bool truePeak() const { return truePeak_; }
	// The optional reference tick on the LUFS bars (off by default; no target is ever suggested). The value is
	// clamped to -60..0.
	void setLufsRef(bool on, double lufs, bool notify = true);

	// "?" beside the dB | LUFS pair: opens the manual's Levels section. The host (the Kit dock) installs the opener
	// (it knows where the manual folder is); the argument is the manual anchor ("levels"). Without an opener the web
	// manual opens. setHelpLogOnly: tests only, log the URL instead of opening it.
	void setHelpOpener(std::function<void(const QString &anchor)> opener) { helpOpener_ = std::move(opener); }
	void setHelpLogOnly(bool on) { helpLogOnly_ = on; }
	void openHelp();
	// Test hooks: click the "?" button; log what the first row shows ("levels-state <tag>: ...").
	void testClickHelp();
	void logTestState(const char *tag) const;

	// Test/diagnostic: the name text currently shown for a row ("" if no such row) and its peak text.
	QString rowLabel(const QString &uuid) const;
	QString rowPeakText(const QString &uuid) const;
	// Test/diagnostic: how many volmeters are attached right now, and whether both timers are running.
	int attachedMeters() const;
	bool timerActive() const { return fast_.isActive() && text_.isActive(); }

	// Test hooks. Override: counts the dock as visible (only the dock sets it, only under
	// RWS_INFLIGHT_TEST_LEVELS). armTestLog: once, 3 s after the meters first run, log one line per source
	// ("levels: <name> peak=<x> avg=<y>") and the gap lines ("levels: gap <text>"); and once, 10 s after, the
	// fast-tick cost ("levels: fast tick mean=<us> max=<us> n=<count>"). Under RWS_INFLIGHT_TEST the panel also
	// logs, per source and per start, "levels: first reading after <ms> ms (<name>)" (from the moment the
	// meters were started to the fast tick that had the row's first fresh reading) and times every fast tick.
	void setGateOverride(bool on);
	void armTestLog();
	void logTickStats();
	// Demo (offscreen screenshots): no meters, no timer. demoSet() feeds one source's numbers; a source
	// never fed reads as missing.
	void setDemoMode(bool demo);
	void demoSet(const QString &uuid, const QString &name, float peakDb, float avgDb);
	// Demo: a source that is attached but sends no reading ("unavailable"): shows "--".
	void demoSetNoData(const QString &uuid, const QString &name);
	// Demo, LUFS mode: one source's numbers (a window not ready reads "--"; tpDbtp -inf is silence, NaN = true
	// peak off for this row).
	void demoSetLufs(const QString &uuid, const QString &name, bool mReady, double momentary, bool sReady, double shortTerm,
			 double tpDbtp);
	// Test hook (RWS_INFLIGHT_TEST_LUFS): once, 5 s after the LUFS backend first runs, log one line per source
	// ("lufs: <name> M=<x> S=<y> TP=<z>"), the worker cost ("lufs: worker mean=<us> max=<us> per 10 ms") and
	// the ring overflow count ("lufs: ring overflow=<n>").
	void armLufsTestLog();

signals:
	void modeChanged(bool lufs);
	void truePeakChanged(bool on);
	void lufsRefChanged(bool on, double lufs);
	// Emitted at once when the panel is opened, and then at 1 Hz while it is open (independent of the meter
	// timers and of whether the meters run): the owner re-evaluates the dock visibility gate from live widget
	// state and pushes the verdict back with setDockVisible(). UI thread.
	void recheckRequested();
	void openChanged(bool open);
	void extrasChanged(const QStringList &extras);

private:
	struct Row;

	void rebuildRows();
	void refillAdd();
	void startBackend();           // the volmeters (dB) or the capture + worker (LUFS), per mode_
	void stopBackend();
	void resetRowsForBackend();    // drop every row's numbers (mode switch, start, stop)
	void applyModeUi();            // mode buttons, options row, per-row widgets
	QString refLabelText() const;  // the reference value as drawn above the line
	void lufsFastTick();
	void lufsTextTick();
	void lufsLogNow();
	void startLufsVolumeTest();
	void updateRunning();
	void fastTick();
	void textTick();
	void refreshRow(Row &r, qint64 tMs);     // text and bar
	void refreshRowText(Row &r, qint64 tMs); // name, Peak / Avg numbers, accessible name
	void refreshBar(Row &r, qint64 tMs);
	void refreshGaps();
	void testLogNow();
	void slowTick();
	void updateSlow();
	void resolveRow(Row &r);
	void classify(Row &r, bool attached);
	QStringList rowUuids() const;
	QString trk(const char *key, const char *fallback) const { return tr_(key, fallback); }

	inflight::TrFn tr_;
	QToolButton *toggle_ = nullptr;
	QWidget *body_ = nullptr;
	QVBoxLayout *rowsLayout_ = nullptr;
	QVBoxLayout *gapsLayout_ = nullptr;
	QLabel *none_ = nullptr;
	QComboBox *add_ = nullptr;

	QString mic_;
	QString desktop_;
	QStringList extras_;
	QVector<Row *> rows_;

	inflight::LevelsMode mode_ = inflight::LevelsMode::Db;
	bool truePeak_ = false;
	bool refOn_ = false;
	double ref_ = inflight::kLufsRefDefault;
	QWidget *modeBar_ = nullptr;     // the dB | LUFS pair in the header (shown while open)
	QToolButton *dbBtn_ = nullptr;
	QToolButton *lufsBtn_ = nullptr;
	QToolButton *helpBtn_ = nullptr;  // "?" next to the pair
	std::function<void(const QString &)> helpOpener_;
	bool helpLogOnly_ = false;
	int numWDb_ = 0;                  // width of the Peak / Avg labels
	int numWLufs_ = 0;                // width of the Now / 3s avg labels
	QButtonGroup *modeGroup_ = nullptr;
	QWidget *lufsOpts_ = nullptr;    // True peak + reference row (LUFS mode only)
	QCheckBox *tpBox_ = nullptr;
	QCheckBox *refBox_ = nullptr;
	QDoubleSpinBox *refSpin_ = nullptr;
	inflight::LevelsLufs *lufs_ = nullptr;     // created on first LUFS run, deleted in the destructor
	QTimer lufsLog_;                           // test hook: one-shot, 5 s after the LUFS backend starts
	bool lufsLogArmed_ = false;
	bool lufsLogDone_ = false;
	inflight::LevelsMeters *meters_ = nullptr; // created on first run, deleted in the destructor
	QTimer fast_; // 20 Hz while running: meters taken, bars drawn
	QTimer text_; // 5 Hz while running: numbers and gap lines
	QTimer slow_; // 1 Hz while open: gate recheck + name resolution
	QTimer testLog_;  // test hook: one-shot, 3 s after the meters start; stopped by shutdown()
	QTimer statsLog_; // test hook: one-shot, 10 s after; stopped by shutdown()
	QStringList addShown_; // the uuids behind the combo's current items
	int tickCount_ = 0;     // text ticks since the meters started (the add list refreshes every 5th)
	bool wantedDirty_ = false; // the next fast tick must (re)attach meters for the current rows
	qint64 startMs_ = 0;       // when the meters were last started (test timing only)

	// Test hooks (RWS_INFLIGHT_TEST only): timing of the fast tick and of the first reading.
	bool testHooks_ = false;
	qint64 statN_ = 0;
	qint64 statSumNs_ = 0;
	qint64 statMaxNs_ = 0;
	bool statsLogged_ = false;

	bool open_ = false;
	bool dockVisible_ = false;
	bool override_ = false;
	bool suspended_ = false;
	bool closed_ = false;
	bool shutDown_ = false; // shutdown() has run: the capture is gone for good and it never runs again
	bool demo_ = false;
	bool running_ = false;
	bool testLogArmed_ = false;
	bool testLogDone_ = false;
};
