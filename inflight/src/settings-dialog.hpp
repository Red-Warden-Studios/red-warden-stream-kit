/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "inflight-settings.hpp"

#include <QDialog>
#include <QString>
#include <QStringList>

class QVBoxLayout;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QSpinBox;
class QTabWidget;

// The In-Flight settings dialog. It edits a COPY of the settings; the caller applies result() on
// accept (and sets setupDone). UI thread only. OK checks the values (normalizeSettings): if it had to
// change anything, the dialog shows what it changed and stays open once; the second OK accepts.
class InflightSettingsDialog : public QDialog {
	Q_OBJECT
public:
	explicit InflightSettingsDialog(const inflight::Settings &current, QWidget *parent = nullptr);
	~InflightSettingsDialog() override;

	// What the widgets say now, normalized. setupDone is left as it was in `current`.
	inflight::Settings result() const;

	// Test hook support (offscreen grabs of each tab).
	int tabCount() const;
	QString tabKey(int i) const; // "watch", "scenes", "lights", "advanced"
	void showTab(int i);
	void scrollCurrentTabToEnd(); // only the Advanced tab scrolls

	void accept() override;

private:
	void build();
	void loadFrom(const inflight::Settings &s);
	inflight::Settings collect() const;
	void addSceneRow(const QString &uuid, const QString &label, int limitSeconds);
	void refreshEnabled();
	void showNote(const QString &title, const QStringList &notes);

	inflight::Settings base_;
	QStringList audioNames_, audioUuids_; // enumerated once when the dialog opens
	QStringList sceneNames_, sceneUuids_;
	QTabWidget *tabs_ = nullptr;
	QComboBox *mic_ = nullptr;
	QComboBox *desktop_ = nullptr;
	QCheckBox *record_ = nullptr;
	QSpinBox *recordGrace_ = nullptr;
	QWidget *rowsHost_ = nullptr;
	QVBoxLayout *rowsLay_ = nullptr;
	QLabel *rowsEmpty_ = nullptr;
	QCheckBox *lights_[7] = {};
	QSpinBox *micAmber_ = nullptr;
	QSpinBox *micRed_ = nullptr;
	QCheckBox *heard_ = nullptr;
	QSpinBox *heardSecs_ = nullptr;
	QSpinBox *heardDb_ = nullptr;
	QSpinBox *desktopSilence_ = nullptr;
	QSpinBox *sceneExtra_ = nullptr;
	QDoubleSpinBox *dropAmber_ = nullptr;
	QDoubleSpinBox *dropRed_ = nullptr;
	QCheckBox *chime_ = nullptr;
	QCheckBox *updates_ = nullptr;
	QLabel *note_ = nullptr;
	bool noted_ = false; // the adjustments note has been shown once; the next OK accepts
};
