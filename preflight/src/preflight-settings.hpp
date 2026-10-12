/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QString>
#include <QStringList>

namespace preflight {

// Everything the user chose in setup. Stored as settings.json in the module config folder.
// Sources and scenes are remembered by UUID so renaming them never breaks a choice.
struct Settings {
	bool setupDone = false;
	bool useMainStream = true;
	bool useMultistream = false;
	QString micUuid;
	QString desktopUuid;
	QString startSceneUuid;
	bool recordIntent = false;

	// Per-check enabled flags (match preflight::Inputs::Enabled).
	bool checkMic = true;
	bool checkDesktop = true;
	bool checkScene = true;
	bool checkMainStream = true;
	bool checkMultistream = true;
	bool checkRecording = true;
	bool checkPerformance = true;
	bool checkCustom = true;

	QStringList customItems;
	bool updateChecks = true; // the update notice off switch (key "check_updates")

	// Defaults when the file is missing or unreadable. UI thread; needs the module loaded
	// (kitpaths, see common/kit-paths.hpp).
	static Settings load();
	// Merges into the existing file, so keys this build does not know about survive.
	bool save() const;
};

} // namespace preflight
