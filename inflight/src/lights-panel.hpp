/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The watchdog UI: header dot + text, a row of seven lights, a details panel for the clicked light,
// and an in-memory session log. Self-contained and UI-thread only: it owns no OBS objects and calls no
// libobs; text goes through the injected tr function. Fed by setSnapshot().
#pragma once

#include "inflight-eval.hpp"
#include "inflight-ui-logic.hpp"

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <array>

class QFrame;
class QLabel;
class QListWidget;
class QPushButton;
class QToolButton;
class HeaderMark;
class LightButton;

class LightsPanel : public QWidget {
	Q_OBJECT
public:
	explicit LightsPanel(inflight::TrFn tr, QWidget *parent = nullptr);
	~LightsPanel() override;

	// nowMs is the controller's steady clock at the time of the snapshot; the panel never reads a clock
	// for durations. Wall-clock text (log lines, "Since") uses an offset captured with each snapshot.
	void setSnapshot(const inflight::Snapshot &s, inflight::LiveState live, qint64 nowMs);

	// Opens the details for a light (or closes them when already open for that light).
	void toggleDetails(inflight::Light l);
	// Opens the details for a light and keeps them open if they already are (the toggle above would close them).
	void showDetails(inflight::Light l);
	// Test hook support: the Network details' "Turn off <destination>" button. Visible only while the worst output
	// is a Multistream destination. testClickTurnOff() presses it (a real click on the real button) and says if it was there.
	bool turnOffVisible() const;
	QString turnOffLabel() const;
	bool testClickTurnOff();
	// Test hook support: the state the panel is showing for a light right now (what the harness asserts instead of
	// guessing it from pixel colour).
	inflight::LightState shownState(inflight::Light l) const { return last_[static_cast<size_t>(l)].state; }
	void setLogExpanded(bool expanded);
	// Stops the flash timer and restores steady marks; later snapshots are ignored. Idempotent.
	void shutdown();
	int logCount() const { return logCount_; }
	// Empties the session log (the Kit's Reset). Open-incident wording is kept: it is not part of the log.
	void clearLog();
	// Places another section (the Levels panel) under the lights and their details, above the session log.
	void insertSection(QWidget *w);
	static constexpr int kMaxLog = 200;

signals:
	void acknowledgeRequested(inflight::Light l);
	void snoozeRequested(inflight::Light l);
	// The Network details' primary button: stop sending to this Multistream destination (same path as its switch).
	void turnOffRequested(const QString &outputId, const QString &outputName);
	// The gear button in the header was clicked.
	void settingsRequested();
	// Some light's flashNow was true in the snapshot just delivered.
	void newRed();

private:
	void refreshLight(int i, const inflight::LightResult &r, qint64 nowMs);
	void refreshDetails(qint64 nowMs);
	void appendEvents(const inflight::Snapshot &s, qint64 nowMs);
	void addLogLine(const QString &line);
	void startFlash(int i);
	void flashTick();
	QString whatFor(const inflight::LightResult &r) const;
	QString wallTime(qint64 steadyMs) const;

	inflight::TrFn tr_;
	HeaderMark *mark_ = nullptr;
	QLabel *headerLabel_ = nullptr;
	std::array<LightButton *, inflight::kLightCount> buttons_{};

	QFrame *details_ = nullptr;
	QLabel *dName_ = nullptr;
	QLabel *dState_ = nullptr;
	QLabel *dWhat_ = nullptr;
	QLabel *dSince_ = nullptr;
	QLabel *dFix_ = nullptr;
	QLabel *dNet_ = nullptr; // Network only: this session's figure for the worst output and which others are fine
	QPushButton *turnOffBtn_ = nullptr;
	QPushButton *ackBtn_ = nullptr;
	QPushButton *snoozeBtn_ = nullptr;

	QToolButton *logToggle_ = nullptr;
	QListWidget *log_ = nullptr;
	int logCount_ = 0;

	std::array<inflight::LightResult, inflight::kLightCount> last_{};
	std::array<int, inflight::kLightCount> flashLeft_{};
	QTimer flashTimer_;
	int selected_ = -1;
	qint64 lastNow_ = 0;
	qint64 wallOffset_ = 0; // wall-clock ms minus steady ms, captured with each snapshot
	bool closed_ = false;   // set by shutdown(): nothing fires or updates after it
	bool haveSnapshot_ = false;
	// The last "what" text seen while an incident was open, for the log lines of its later events.
	QHash<quint64, QString> openWhat_;
};
