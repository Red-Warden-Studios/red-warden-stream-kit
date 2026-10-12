/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// Reads the choices Pre-Flight saved, as pure logic (Qt Core only, no OBS), so it can be
// unit-tested. The caller reads the file and hands over the bytes.

#include "inflight-settings.hpp"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace inflight {

struct ImportResult {
	bool parsed = false; // the bytes were a JSON object
	bool micFound = false;
	bool desktopFound = false;
	bool recordFound = false;
	QString micUuid;
	QString desktopUuid;
	bool recordIntent = false;

	bool anythingFound() const { return micFound || desktopFound || recordFound; }
};

// Never throws; bad, partial or empty input just finds less. An empty UUID string, or a value of
// the wrong type, counts as not found.
ImportResult importFromPreflightJson(const QByteArray &json);

// Copies what was found into `s`. Fields that were not found are left alone. Setup is marked done only
// when a microphone was found, because the lights cannot work without one.
void applyImport(const ImportResult &r, Settings &s);

// Where Pre-Flight keeps its settings, given this plugin's own config folder (the folder returned
// by the module config path for ""). Both live side by side under the plugin_config folder.
// Empty when the folder is empty. The caller checks that the file exists.
QString preflightSettingsPath(const QString &ownConfigDir);

} // namespace inflight
