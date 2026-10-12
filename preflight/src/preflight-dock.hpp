/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "preflight-controller.hpp"
#include "update-check.hpp"

#include <obs-frontend-api.h>

#include <QDialog>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;
class PreflightCheckRow;

// The Pre-Flight dock: setup card, one row per check, the Go Live bar, the "you went live with
// problems" banner and the once-per-start update notice. All UI-thread only.
class PreflightDock : public QWidget {
	Q_OBJECT
public:
	explicit PreflightDock(QWidget *parent = nullptr);
	~PreflightDock() override;

	// Routed from the frontend event callback (UI thread).
	void handleFrontendEvent(obs_frontend_event event);
	// Called at OBS_FRONTEND_EVENT_EXIT: stop the update worker, the controller and its timers.
	void shutdown();

	// ---- host (Stream Kit dock) interface ----
	// The Kit's Reset: clear check results (ticks, mic-test result) and run the checks again.
	void resetForNewStream();
	// Shows the "Done" button next to Settings (only when a host can act on it).
	void setDoneVisible(bool visible);
	// Where the Multistream check reads the destinations from (the Kit wires it when Multistream is loaded; never set =
	// "Multistream wasn't found"). See MultistreamProbe.
	void setMultistreamSource(preflight::MultistreamProbe::Source src);
	// Host only: the "Levels" button in the button row (shown only when the host has Levels to open, i.e.
	// In-Flight is on). The host opens/closes its Levels section when it is clicked.
	void setLevelsAvailable(bool available);
	bool levelsButtonVisible() const;

signals:
	// "Done" pressed: the host moves on to In-Flight.
	void doneRequested();
	void levelsRequested();
	// The Loudest / Typical line under the Mic test row was shown (true) or cleared (false). Test hooks of the host
	// use it to photograph the row.
	void micNumbersShown(bool shown);

private:
	void showUpdate(const updatecheck::Manifest &m);
	void applyResults(const QVector<preflight::CheckResult> &results);
	void syncCustomItems();
	void updateMicButton();
	void applyMicNumbers();
	void updateLive();
	void openSettings();
	void goLive();
	void installTestHooks();
	QWidget *mainWindow() const;

	QPointer<PreflightController> controller; // child QObject; a QPointer so shutdown never touches a deleted one
	QLabel *updateNote = nullptr;
	QFrame *banner = nullptr;
	QLabel *bannerLabel = nullptr;
	QFrame *setupCard = nullptr;
	QWidget *rowsHost = nullptr;
	QVBoxLayout *rowsLayout = nullptr;
	QPushButton *goBtn = nullptr;
	QPushButton *gearBtn = nullptr;
	QPushButton *doneBtn = nullptr;
	QPushButton *levelsBtn = nullptr;
	QPointer<QDialog> modal_; // the open settings dialog / go-live box, if any (rejected by shutdown)
	QMap<int, PreflightCheckRow *> rows;
	QVector<int> order;
	QVector<bool> ticks;
	QStringList customCache;
	UpdateChecker updates;
	updatecheck::Manifest pendingUpdate;
	bool checkUpdates = true;
	bool closing = false;
	bool shutDown_ = false; // shutdown() has run (EXIT or the destructor): a no-op from then on
	bool micRunning = false;
	bool live = false;
	bool startedFromDock = false;
};
