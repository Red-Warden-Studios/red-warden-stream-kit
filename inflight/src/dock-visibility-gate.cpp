
/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "dock-visibility-gate.hpp"

#include <obs.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QEvent>
#include <QList>
#include <QMainWindow>
#include <QTabBar>
#include <QVariant>
#include <QWidget>

DockVisibilityGate::DockVisibilityGate(QWidget *content, QObject *parent) : QObject(parent), content_(content)
{
	if (content_)
		content_->installEventFilter(this);
	rebind();
}

bool DockVisibilityGate::windowMinimized() const
{
	if (window_ && window_->isMinimized())
		return true;
	// A floating dock is its own window; minimising the OBS main window hides it too.
	if (dock_) {
		if (QWidget *p = dock_->parentWidget()) {
			if (QWidget *w = p->window())
				return w->isMinimized();
		}
	}
	return false;
}

// Is `dock_` the current tab of the tab bar that holds it? matched = a tab for it was found.
bool DockVisibilityGate::isCurrentTab(QMainWindow *mw, bool *matched) const
{
	*matched = false;
	tabMatch_ = QStringLiteral("none");
	const quintptr id = reinterpret_cast<quintptr>(dock_.data());
	const QList<QTabBar *> bars = mw->findChildren<QTabBar *>();
	// Pass 1: the tab data (quintptr of the dock widget, Qt 6).
	for (QTabBar *tb : bars) {
		if (!tb->isVisibleTo(mw))
			continue; // a pooled, unused tab bar
		for (int i = 0; i < tb->count(); ++i) {
			const QVariant d = tb->tabData(i);
			if (d.isValid() && d.canConvert<quintptr>() && d.value<quintptr>() == id) {
				*matched = true;
				tabMatch_ = QStringLiteral("data");
				return tb->currentIndex() == i;
			}
		}
	}
	// No tab carries this dock's pointer (Qt 6.8 stores it as tab data, so this should not happen for a tabified
	// dock). There is deliberately NO guess from the tab title (a title is not an identity). Fall back to the
	// last QDockWidget::visibilityChanged value Qt told us, which Qt computes from the real tab state; with no
	// signal yet the answer is "not visible" (never "visible" by default: a wrong "visible" would meter a
	// hidden dock). The next visibilityChanged (tab switches always emit it) corrects it.
	tabMatch_ = QStringLiteral("signal");
	return haveVisSignal_ && lastVisSignal_;
}

DockVisibilityGate::State DockVisibilityGate::state() const
{
	State s;
	if (!content_)
		return s;
	bool v;
	if (dock_) {
		s.dock = true;
		s.floating = dock_->isFloating();
		QMainWindow *mw = qobject_cast<QMainWindow *>(dock_->parentWidget());
		s.tabbed = !s.floating && mw && !mw->tabifiedDockWidgets(dock_).isEmpty();
		s.currentTab = true;
		if (s.tabbed) {
			bool matched = false;
			s.currentTab = isCurrentTab(mw, &matched);
		} else {
			tabMatch_ = QStringLiteral("none");
		}
		v = dock_->isVisible() && s.currentTab;
	} else {
		s.currentTab = true;
		v = content_->isVisible(); // the fallback, no QDockWidget around it
	}
	s.minimized = windowMinimized();
	s.hostShown = hostShown_;
	s.visible = v && !s.minimized && hostShown_;
	return s;
}

void DockVisibilityGate::recheck()
{
	const State s = state();
	if (!haveState_ || s != lastState_) {
		haveState_ = true;
		lastState_ = s;
		obs_log(LOG_INFO, "levels: gate visible=%d (dock=%d floating=%d tabbed=%d currentTab=%d minimized=%d%s)",
			s.visible ? 1 : 0, s.dock ? 1 : 0, s.floating ? 1 : 0, s.tabbed ? 1 : 0, s.currentTab ? 1 : 0,
			s.minimized ? 1 : 0, s.hostShown ? "" : " hostShown=0");
	}
	if (s.visible == last_)
		return;
	last_ = s.visible;
	emit changed(s.visible);
}

void DockVisibilityGate::setHostShown(bool shown)
{
	if (hostShown_ == shown)
		return;
	hostShown_ = shown;
	recheck();
}

void DockVisibilityGate::bindWindow()
{
	QWidget *w = content_ ? content_->window() : nullptr;
	if (w == window_)
		return;
	if (window_ && window_ != dock_.data() && window_ != content_.data())
		window_->removeEventFilter(this);
	window_ = w;
	if (window_)
		window_->installEventFilter(this);
}

void DockVisibilityGate::rebind()
{
	if (!content_)
		return;
	QDockWidget *found = nullptr;
	for (QWidget *w = content_->parentWidget(); w && !found; w = w->parentWidget())
		found = qobject_cast<QDockWidget *>(w);
	if (found != dock_) {
		if (dock_) {
			dock_->disconnect(this);
			dock_->removeEventFilter(this);
		}
		dock_ = found;
		if (dock_) {
			// Triggers only: each one re-reads the live state.
			haveVisSignal_ = false; // a new dock: nothing heard from it yet
			connect(dock_, &QDockWidget::visibilityChanged, this, [this](bool v) {
				haveVisSignal_ = true;
				lastVisSignal_ = v; // only used when no tab matches the dock (isCurrentTab)
				bindWindow(); // the dock may have floated into its own window, or come back
				recheck();
			});
			connect(dock_, &QDockWidget::topLevelChanged, this, [this](bool) {
				bindWindow();
				recheck();
			});
			connect(dock_, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea) {
				bindWindow();
				recheck();
			});
		}
	}
	bindWindow();
	if (dock_)
		dock_->installEventFilter(this); // Show / Hide / WindowStateChange of the dock itself
	recheck();
}

bool DockVisibilityGate::eventFilter(QObject *o, QEvent *e)
{
	const bool ours = o == content_.data() || o == dock_.data() || o == window_.data();
	if (!ours)
		return false;
	switch (e->type()) {
	case QEvent::ParentChange:
		if (o == content_.data())
			rebind();
		break;
	case QEvent::WindowStateChange:
	case QEvent::Show:
	case QEvent::Hide:
		recheck(); // the live state decides; a Show/Hide of the content alone also covers the no-dock fallback
		break;
	default:
		break;
	}
	return false;
}
