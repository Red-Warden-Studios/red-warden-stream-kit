
/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Answers "can anybody see this widget right now?" for a widget that lives in a QDockWidget. The verdict is
// computed from the live widget state every time, never trusted from a remembered signal value:
//   visible = the containing QDockWidget isVisible()
//             AND it is not behind a tab (tabbed dock that is not the current tab of its tab bar)
//             AND the window holding it is not minimised
//             AND the host says its page is shown (setHostShown, default true).
// The host input exists for the Stream Kit dock: In-Flight lives on a page of the Kit's one dock, so the
// QDockWidget can be visible while the In-Flight page is hidden (another stage, or a collapsed Show-all
// section). Only the host knows; it sets the flag on every page change.
// A floating dock is never "behind a tab". QDockWidget::visibilityChanged is NOT emitted for a dock that was
// already shown/floating before we connected (or restored by OBS in a way that never reaches us), so the
// signal, Show/Hide/WindowStateChange events and rebind() are only TRIGGERS for a recheck; the owner also
// calls rebind() on a slow timer (LevelsPanel::recheckRequested, 1 Hz while the panel is open).
// Detecting the current tab: Qt keeps the docks of one area in a QTabBar that is a child of the QMainWindow,
// with the dock's pointer as quintptr in the tab data (QDockAreaLayoutInfo::updateTabBar, Qt 6; confirmed
// against qdockarealayout.cpp, Qt 6.8: currentTabId() is a quintptr). We match on that only. If a tabified dock
// has no tab carrying its pointer, we use the last QDockWidget::visibilityChanged value received (false when
// none yet); we never guess from the tab title and never default to "visible".
// Every change of the state is logged once at LOG_INFO. UI thread only.
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

class QDockWidget;
class QMainWindow;
class QWidget;

class DockVisibilityGate : public QObject {
	Q_OBJECT
public:
	struct State {
		bool dock = false;       // a QDockWidget contains the content
		bool floating = false;   // that dock is floating (its own window)
		bool tabbed = false;     // docked and sharing an area with another dock (tabified)
		bool currentTab = true;  // when tabbed: it is the current tab (true when not tabbed)
		bool minimized = false;  // the window holding it is minimised
		bool hostShown = true;   // the host's page holding the content is the shown one
		bool visible = false;    // the verdict
		bool operator==(const State &o) const
		{
			return dock == o.dock && floating == o.floating && tabbed == o.tabbed &&
			       currentTab == o.currentTab && minimized == o.minimized && hostShown == o.hostShown &&
			       visible == o.visible;
		}
		bool operator!=(const State &o) const { return !(*this == o); }
	};

	explicit DockVisibilityGate(QWidget *content, QObject *parent = nullptr);

	// Look for the containing QDockWidget and the top-level window again (after the dock is registered,
	// floated or re-docked) and re-evaluate from the live widget state. Cheap; safe to call any time, and
	// the slow 1 Hz self-correction.
	void rebind();
	// The host (the Kit dock) says whether the page holding the content is the shown one. Re-evaluates at once.
	void setHostShown(bool shown);
	bool hostShown() const { return hostShown_; }
	bool visible() const { return last_; }
	// What the gate has bound to, for the test and the log.
	bool hasDock() const { return !dock_.isNull(); }
	bool windowMinimized() const;
	// The live verdict with its parts (does not emit or log).
	State state() const;
	// How the current tab was identified at the last state(): "data", "signal" (no tab matched: last visibilityChanged), "none" (not tabbed / unmatched).
	QString tabMatch() const { return tabMatch_; }

signals:
	void changed(bool visible);

protected:
	bool eventFilter(QObject *o, QEvent *e) override;

private:
	bool isCurrentTab(QMainWindow *mw, bool *matched) const;
	void recheck();
	void bindWindow();

	QPointer<QWidget> content_;
	QPointer<QDockWidget> dock_;
	QPointer<QWidget> window_;
	State lastState_;
	bool haveVisSignal_ = false;  // a QDockWidget::visibilityChanged has been received from the bound dock
	bool lastVisSignal_ = false;  // ...and this is its last value (the fallback when no tab matches the dock)
	bool haveState_ = false;
	bool hostShown_ = true;
	bool last_ = false;
	mutable QString tabMatch_ = QStringLiteral("none");
};
