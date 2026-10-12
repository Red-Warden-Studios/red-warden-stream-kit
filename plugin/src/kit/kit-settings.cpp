/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kit-settings.hpp"

#include "update-manifest.hpp"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace kitsettings {

namespace {
bool boolOr(const QJsonObject &o, const char *key, bool dflt)
{
	const QJsonValue v = o.value(QLatin1String(key));
	return v.isBool() ? v.toBool() : dflt;
}

QByteArray slurp(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return f.read(256 * 1024);
}
} // namespace

Settings parse(const QByteArray &json)
{
	Settings s;
	const QJsonDocument doc = QJsonDocument::fromJson(json);
	if (!doc.isObject())
		return s;
	const QJsonObject o = doc.object();
	const QJsonValue t = o.value(QStringLiteral("tools"));
	if (t.isObject()) {
		const QJsonObject tools = t.toObject();
		s.multistream = boolOr(tools, "multistream", s.multistream);
		s.preflight = boolOr(tools, "preflight", s.preflight);
		s.inflight = boolOr(tools, "inflight", s.inflight);
	}
	s.autoSwitchOnLive = boolOr(o, "auto_switch_on_live", s.autoSwitchOnLive);
	s.showAll = boolOr(o, "show_all", s.showAll);
	s.multistreamCollapsed = boolOr(o, "multistream_collapsed", s.multistreamCollapsed);
	s.firstRunDone = boolOr(o, "first_run_done", s.firstRunDone);
	return s;
}

QByteArray merge(const QByteArray &existing, const Settings &s)
{
	QJsonObject o;
	const QJsonDocument doc = QJsonDocument::fromJson(existing);
	if (doc.isObject())
		o = doc.object(); // keep unknown keys (check_updates among them)
	QJsonObject tools;
	if (o.value(QStringLiteral("tools")).isObject())
		tools = o.value(QStringLiteral("tools")).toObject(); // keep unknown tool keys
	tools.insert(QStringLiteral("multistream"), s.multistream);
	tools.insert(QStringLiteral("preflight"), s.preflight);
	tools.insert(QStringLiteral("inflight"), s.inflight);
	o.insert(QStringLiteral("tools"), tools);
	o.insert(QStringLiteral("auto_switch_on_live"), s.autoSwitchOnLive);
	o.insert(QStringLiteral("show_all"), s.showAll);
	o.insert(QStringLiteral("multistream_collapsed"), s.multistreamCollapsed);
	o.insert(QStringLiteral("first_run_done"), s.firstRunDone);
	return QJsonDocument(o).toJson();
}

Settings load(const QString &pluginConfigRoot)
{
	if (pluginConfigRoot.isEmpty())
		return Settings();
	return parse(slurp(updatecheck::kitSettingsPath(pluginConfigRoot)));
}

bool save(const QString &pluginConfigRoot, const Settings &s)
{
	if (pluginConfigRoot.isEmpty())
		return false;
	const QString path = updatecheck::kitSettingsPath(pluginConfigRoot);
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
		return false;
	f.write(merge(slurp(path), s));
	return f.commit();
}

bool saveFirstRunDone(const QString &pluginConfigRoot, Settings &s)
{
	s.firstRunDone = true;
	return save(pluginConfigRoot, s) && load(pluginConfigRoot).firstRunDone;
}

bool dockStateHasDock(const QByteArray &base64State, const QString &objectName)
{
	if (base64State.isEmpty() || objectName.isEmpty())
		return false;
	const QByteArray state = QByteArray::fromBase64(base64State);
	QByteArray needle;
	{
		QDataStream out(&needle, QIODevice::WriteOnly);
		out << objectName; // exactly how QMainWindow::saveState writes a dock's object name
	}
	return state.indexOf(needle) >= 0;
}

bool shouldForceDock(bool firstRunDone, bool savedLayoutHasDock)
{
	return !firstRunDone && !savedLayoutHasDock;
}

} // namespace kitsettings
