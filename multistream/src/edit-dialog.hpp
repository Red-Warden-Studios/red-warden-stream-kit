/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "config.hpp"
#include "platform-limits.hpp"

#include <map>

#include <QColor>
#include <QDialog>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QWidget;

class EditDialog : public QDialog {
	Q_OBJECT

public:
	// isNew: adding rather than editing. hasSavedKey: a key is already in
	// Credential Manager (the dialog never reads it back into the UI).
	EditDialog(const Destination &d, bool isNew, bool hasSavedKey, QWidget *parent = nullptr);

	Destination destination() const;
	// Empty unless the user typed a new key.
	QString newKey() const;
	bool removeRequested() const { return removeFlag; }

	// The dialog is a modal of its dock (MultistreamDock::execModal rejects it at OBS exit). A dialog this one has
	// open on top of itself (the Twitch bandwidth test, the Remove question) is tracked in nested_ and ended first,
	// or it would stay up as an orphan modal over a closing OBS.
	void reject() override;

private:
	int execNested(QDialog *dlg); // heap dialog, modal over this one, tracked in nested_; -1 when it was destroyed
	QPointer<QDialog> nested_;
	void onPlatformChanged();
	void onEncoderModeChanged();
	void validate();
	void updateLimits(); // advice when this destination sends more than its platform takes

	Destination base;
	bool removeFlag = false;

	QLineEdit *nameEdit;
	QComboBox *platformCombo;
	QLabel *hintLabel;
	QLineEdit *serverEdit;
	QLineEdit *keyEdit;
	QCheckBox *showKey;
	QRadioButton *sharedRadio;
	QRadioButton *ownRadio;
	QWidget *ownBox;
	QComboBox *encoderCombo;
	QSpinBox *videoBitrate;
	QComboBox *audioBitrate;
	QComboBox *trackCombo;
	QComboBox *resCombo;
	QLabel *errorLabel;
	QPushButton *bandwidthButton;
	QLabel *limitsLabel;
	std::map<QString, PlatformLimits> limits;
	int obsAudioKbps = 160;
	QColor emberColor, noteColor;
	QWidget *okButton;
};
