/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-import.hpp"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace inflight {

namespace {
// A non-empty string value, or an empty string for anything else.
QString uuidKey(const QJsonObject &o, const char *key)
{
	const QJsonValue v = o.value(QString::fromLatin1(key));
	return v.isString() ? v.toString().trimmed() : QString();
}
} // namespace

ImportResult importFromPreflightJson(const QByteArray &json)
{
	ImportResult r;
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return r;
	r.parsed = true;
	const QJsonObject o = doc.object();

	r.micUuid = uuidKey(o, "mic_uuid");
	r.micFound = !r.micUuid.isEmpty();
	r.desktopUuid = uuidKey(o, "desktop_uuid");
	r.desktopFound = !r.desktopUuid.isEmpty();

	const QJsonValue rec = o.value(QStringLiteral("record_intent"));
	if (rec.isBool()) {
		r.recordFound = true;
		r.recordIntent = rec.toBool();
	}
	return r;
}

void applyImport(const ImportResult &r, Settings &s)
{
	if (r.micFound)
		s.micUuid = r.micUuid;
	if (r.desktopFound)
		s.desktopUuid = r.desktopUuid;
	if (r.recordFound)
		s.recordIntent = r.recordIntent;
	if (r.micFound)
		s.setupDone = true;
}

QString preflightSettingsPath(const QString &ownConfigDir)
{
	if (ownConfigDir.isEmpty())
		return QString();
	// Lexical, so it works before this plugin's own folder has been created.
	return QDir::cleanPath(ownConfigDir + QStringLiteral("/../rws-preflight/settings.json"));
}

} // namespace inflight
