/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// The Kit settings dialog (mockup Settings.dc.html): tabs Tools / Updates / About.
//  - Tools: a switch per tool (takes effect the next time OBS starts; switching one OFF is refused while a
//    stream session is active) and the "Switch to In-Flight when I go live" option.
//  - Updates: the shared "tell me when an update is available" preference (the same key every tool used to
//    show; plugin_config/rws-stream-kit/settings.json "check_updates").
//  - About: version and links.
// The dialog only edits a copy; KitDock saves what comes back.

#include "kit-settings.hpp"

#include <QDialog>

#include <functional>

class QCheckBox;
class QLabel;
class QTabWidget;

class KitSettingsDialog : public QDialog {
	Q_OBJECT
public:
	// toolOffBlocked is asked each time a tool is switched off, and again on Save: true while OBS is streaming,
	// any Multistream output is active, or a Kit session is active (a tool must not be switched off then).
	KitSettingsDialog(const kitsettings::Settings &settings, bool checkUpdates, std::function<bool()> toolOffBlocked,
			  QWidget *parent = nullptr);

	kitsettings::Settings settings() const;
	bool checkUpdates() const;

	// Test hooks: one grab per tab.
	int tabCount() const;
	void showTab(int index);
	QString tabKey(int index) const;

private:
	QWidget *buildToolsTab();
	QWidget *buildUpdatesTab();
	QWidget *buildAboutTab();
	void toolToggled(QCheckBox *box, bool on);
	void trySave();

	kitsettings::Settings initial_;
	std::function<bool()> toolOffBlocked_;
	QTabWidget *tabs_ = nullptr;
	QCheckBox *ms_ = nullptr;
	QCheckBox *pf_ = nullptr;
	QCheckBox *inflight_ = nullptr;
	QCheckBox *autoSwitch_ = nullptr;
	QCheckBox *updates_ = nullptr;
	QLabel *toolsNote_ = nullptr;
	bool checkUpdatesInitial_ = true;
};
