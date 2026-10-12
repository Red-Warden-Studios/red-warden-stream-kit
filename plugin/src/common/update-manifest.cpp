/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "update-manifest.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>

namespace updatecheck {

namespace {
// Leading "1.2.3" of a version string; empty if it doesn't start with a digit.
QList<int> parts(const QString &v)
{
	static const QRegularExpression re(QStringLiteral("^\\s*v?(\\d+(?:\\.\\d+){0,3})"));
	const auto m = re.match(v);
	QList<int> out;
	if (!m.hasMatch())
		return out;
	for (const QString &p : m.captured(1).split(QLatin1Char('.')))
		out.append(p.toInt());
	return out;
}
} // namespace

QString defaultPage()
{
	return QStringLiteral("https://redwardenstudios.com/division/bastion/stream-kit/");
}

QString kitManifestUrl()
{
	return QStringLiteral("https://") + QString::fromLatin1(kKitHost) + QString::fromLatin1(kKitManifestPath);
}

QString kitSettingsPath(const QString &pluginConfigRoot)
{
	return QDir(pluginConfigRoot).filePath(QStringLiteral("rws-stream-kit/settings.json"));
}

int readCheckUpdatesKey(const QByteArray &json)
{
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return -1;
	const QJsonValue v = doc.object().value(QStringLiteral("check_updates"));
	return v.isBool() ? (v.toBool() ? 1 : 0) : -1;
}

bool mergeLegacyCheckUpdates(int multistreamState, int preflightState)
{
	return !(multistreamState == 0 || preflightState == 0);
}

namespace {
QByteArray slurp(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return f.read(256 * 1024);
}
} // namespace

bool saveSharedCheckUpdates(const QString &pluginConfigRoot, bool on)
{
	if (pluginConfigRoot.isEmpty())
		return false;
	const QString path = kitSettingsPath(pluginConfigRoot);
	QDir().mkpath(QFileInfo(path).absolutePath());
	QJsonObject o;
	const QJsonDocument existing = QJsonDocument::fromJson(slurp(path));
	if (existing.isObject())
		o = existing.object(); // keep unknown keys
	o.insert(QStringLiteral("check_updates"), on);
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	f.write(QJsonDocument(o).toJson());
	return f.commit();
}

bool loadSharedCheckUpdates(const QString &pluginConfigRoot)
{
	if (pluginConfigRoot.isEmpty())
		return true;
	const int shared = readCheckUpdatesKey(slurp(kitSettingsPath(pluginConfigRoot)));
	if (shared >= 0)
		return shared == 1;
	const QDir root(pluginConfigRoot);
	const int ms = readCheckUpdatesKey(slurp(root.filePath(QStringLiteral("rws-multistream/settings.json"))));
	const int pf = readCheckUpdatesKey(slurp(root.filePath(QStringLiteral("rws-preflight/settings.json"))));
	const bool merged = mergeLegacyCheckUpdates(ms, pf);
	saveSharedCheckUpdates(pluginConfigRoot, merged);
	return merged;
}
int compareVersions(const QString &a, const QString &b)
{
	const QList<int> x = parts(a), y = parts(b);
	const int n = (int)std::max(x.size(), y.size());
	for (int i = 0; i < n; i++) {
		const int xi = i < x.size() ? x[i] : 0;
		const int yi = i < y.size() ? y[i] : 0;
		if (xi != yi)
			return xi < yi ? -1 : 1;
	}
	return 0;
}

bool parseManifest(const QByteArray &json, Manifest *out)
{
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return false;
	const QJsonObject o = doc.object();
	Manifest m;
	m.latest = o.value(QStringLiteral("latest")).toString().trimmed();
	if (parts(m.latest).isEmpty())
		return false;

	const QString url = o.value(QStringLiteral("url")).toString().trimmed();
	const QUrl u(url);
	const bool ours = u.isValid() && u.scheme() == QLatin1String("https") &&
			  (u.host() == QLatin1String("redwardenstudios.com") ||
			   u.host() == QLatin1String("www.redwardenstudios.com")) &&
			  u.userInfo().isEmpty() && u.port(443) == 443;
	m.url = ours ? url : defaultPage();

	m.notes = o.value(QStringLiteral("notes")).toString().section(QLatin1Char('\n'), 0, 0).trimmed().left(200);
	const QString minObs = o.value(QStringLiteral("min_obs")).toString().trimmed();
	m.minObs = parts(minObs).isEmpty() ? QString() : minObs;
	if (out)
		*out = m;
	return true;
}

} // namespace updatecheck
