/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace inflight {

// A scene that is expected to be on air only for a while (starting soon, be right back, ending).
struct HoldingScene {
	QString sceneUuid;
	int limitSeconds = 300;
	QString label;
};

// Everything the user chose in setup, plus the thresholds. Stored as settings.json in the module
// config folder. Sources and scenes are remembered by UUID so renaming them never breaks a choice.
struct Settings {
	bool setupDone = false;
	QString micUuid;
	QString desktopUuid;
	QVector<HoldingScene> holdingScenes;
	bool recordIntent = false;

	// Which lights are on.
	bool lightMic = true;
	bool lightDesktop = true;
	bool lightScene = true;
	bool lightRecording = true;
	bool lightNetwork = true;
	bool lightPc = true;
	bool lightStream = true;

	// Thresholds (seconds unless the name says otherwise).
	int micMutedAmberS = 20;
	int micMutedRedS = 60;
	bool heardYouEnabled = false;
	int heardYouSilenceS = 90;
	int heardYouDbfs = -50;
	int desktopSilenceS = 120;
	int sceneRedExtraS = 300;
	int recordStartGraceS = 60;
	double dropAmberPct = 0.5;
	double dropRedPct = 2.0;

	bool chime = false;
	bool updateChecks = true; // the shared update notice off switch (key "check_updates")
	// The user has dismissed (x) or followed (Change) the "Using your microphone and scenes from Pre-Flight." note
	// (key "preflight_note_seen").
	bool preflightNoteSeen = false;

	// Levels panel: whether it is open (key "levels_open") and the extra sources the user added, by UUID
	// (key "levels_sources"; the mic and desktop are not listed here). On disk the array holds one
	// object per source, {"uuid": "..."}: libobs' obs_data arrays cannot hold bare strings.
	bool levelsOpen = false;
	QStringList levelsSources;
	// Levels panel: the meter ("levels_mode": "db" default, or "lufs"), the LUFS-mode true-peak switch
	// ("levels_true_peak", off by default) and the optional reference tick ("levels_lufs_ref_on" off by default,
	// "levels_lufs_ref" in LUFS, default -16, clamped to -60..0 by the panel). No target is ever applied.
	QString levelsMode = QStringLiteral("db");
	bool levelsTruePeak = false;
	bool levelsLufsRefOn = false;
	double levelsLufsRef = -16.0;

	// Defaults when the file is missing or unreadable. UI thread; needs the module loaded
	// (kitpaths, see common/kit-paths.hpp).
	static Settings load();
	// Merges into the existing file, so keys this build does not know about survive.
	bool save() const;
};

} // namespace inflight
