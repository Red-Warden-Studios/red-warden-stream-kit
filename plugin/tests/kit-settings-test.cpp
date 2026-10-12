// Tests for kit/kit-settings.cpp: defaults, parsing, the merge-preserving save and the file round trip.
// No framework: exits non-zero on failure.
#include "kit-settings.hpp"
#include "update-manifest.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

static int failures = 0;
static int total = 0;
#define CHECK(cond, msg)                                                      \
	do {                                                                  \
		total++;                                                      \
		if (!(cond)) {                                                \
			std::printf("FAIL: %s  (line %d)\n", msg, __LINE__); \
			failures++;                                           \
		} else {                                                      \
			std::printf("ok:   %s\n", msg);                       \
		}                                                             \
	} while (0)

using kitsettings::Settings;

static QJsonObject obj(const QByteArray &json)
{
	return QJsonDocument::fromJson(json).object();
}

static void testDefaults()
{
	const Settings d;
	CHECK(d.multistream && d.preflight && d.inflight, "default: all three tools on");
	CHECK(d.autoSwitchOnLive, "default: auto-switch on live is on");
	CHECK(!d.showAll && !d.multistreamCollapsed && !d.firstRunDone, "default: show-all off, Multistream expanded, first run not done");
	CHECK(kitsettings::parse(QByteArray()) == d, "empty input reads as the defaults");
	CHECK(kitsettings::parse("not json") == d, "malformed input reads as the defaults");
	CHECK(kitsettings::parse("[1,2]") == d, "a JSON array reads as the defaults");
	CHECK(kitsettings::parse("{}") == d, "an empty object reads as the defaults");
	CHECK(kitsettings::parse("{\"check_updates\":false}") == d, "the update preference alone leaves the Kit settings at the defaults");
}

static void testParse()
{
	const Settings s = kitsettings::parse(
		"{\"tools\":{\"multistream\":false,\"preflight\":true,\"inflight\":false},"
		"\"auto_switch_on_live\":false,\"show_all\":true,\"multistream_collapsed\":true,\"first_run_done\":true}");
	CHECK(!s.multistream && s.preflight && !s.inflight, "tools read back individually");
	CHECK(!s.autoSwitchOnLive && s.showAll && s.multistreamCollapsed && s.firstRunDone, "the four flags read back");
	const Settings w = kitsettings::parse("{\"tools\":{\"multistream\":\"no\",\"inflight\":0},\"show_all\":1,\"first_run_done\":null}");
	CHECK(w == Settings(), "values that are not booleans read as the defaults");
	const Settings t = kitsettings::parse("{\"tools\":5,\"show_all\":true}");
	CHECK(t.multistream && t.preflight && t.inflight && t.showAll, "a non-object tools value reads as all-on");
	const Settings p = kitsettings::parse("{\"tools\":{\"preflight\":false}}");
	CHECK(p.multistream && !p.preflight && p.inflight, "a partial tools object keeps the other tools on");
}

static void testMerge()
{
	Settings s;
	s.preflight = false;
	s.showAll = true;
	s.firstRunDone = true;
	const QByteArray existing =
		"{\"check_updates\":false,\"future_key\":{\"a\":1},\"tools\":{\"multistream\":true,\"postflight\":true},\"show_all\":false}";
	const QByteArray out = kitsettings::merge(existing, s);
	const QJsonObject o = obj(out);
	CHECK(o.value("check_updates").isBool() && !o.value("check_updates").toBool(), "merge keeps check_updates (false)");
	CHECK(o.value("future_key").toObject().value("a").toInt() == 1, "merge keeps an unknown top-level key");
	CHECK(o.value("tools").toObject().value("postflight").toBool(), "merge keeps an unknown key inside tools");
	CHECK(!o.value("tools").toObject().value("preflight").toBool(true), "merge writes the changed tool");
	CHECK(o.value("tools").toObject().value("multistream").toBool(false) && o.value("tools").toObject().value("inflight").toBool(false),
	      "merge writes the unchanged tools too (explicit true)");
	CHECK(o.value("show_all").toBool() && o.value("first_run_done").toBool(), "merge writes the changed flags");
	CHECK(o.value("auto_switch_on_live").toBool(false) && !o.value("multistream_collapsed").toBool(true), "merge writes the unchanged flags too");
	CHECK(kitsettings::parse(out) == s, "what merge writes parses back to the same settings");
	const QByteArray fromGarbage = kitsettings::merge("{{{ nope", s);
	CHECK(kitsettings::parse(fromGarbage) == s, "merge over a malformed file still writes valid settings");
	CHECK(kitsettings::parse(kitsettings::merge(QByteArray(), s)) == s, "merge over an empty file works");
}

static void testFiles()
{
	QTemporaryDir dir;
	CHECK(dir.isValid(), "temp dir");
	const QString root = dir.path();
	CHECK(kitsettings::load(root) == Settings(), "load with no file: the defaults");
	CHECK(kitsettings::load(QString()) == Settings(), "load with an empty root: the defaults");
	CHECK(!kitsettings::save(QString(), Settings()), "save with an empty root fails");

	// The update preference is written first by another owner; the Kit settings must not disturb it.
	CHECK(updatecheck::saveSharedCheckUpdates(root, false), "the update preference is saved");
	Settings s;
	s.inflight = false;
	s.multistreamCollapsed = true;
	CHECK(kitsettings::save(root, s), "save");
	CHECK(kitsettings::load(root) == s, "load reads back what save wrote");
	CHECK(!updatecheck::loadSharedCheckUpdates(root), "the update preference survived the Kit save (false)");

	// ...and the other way round: the update preference save keeps the Kit settings.
	CHECK(updatecheck::saveSharedCheckUpdates(root, true), "the update preference is saved again");
	CHECK(kitsettings::load(root) == s, "the Kit settings survived the update preference save");
	CHECK(updatecheck::loadSharedCheckUpdates(root), "the update preference reads true now");

	// A file the Kit created first (no check_updates key) still lets the legacy migration run.
	QTemporaryDir dir2;
	Settings first;
	first.firstRunDone = true;
	CHECK(kitsettings::save(dir2.path(), first), "a Kit-only file is created");
	CHECK(updatecheck::loadSharedCheckUpdates(dir2.path()), "no check_updates key and no legacy files: updates default to on");
	CHECK(kitsettings::load(dir2.path()) == first, "the migration write kept the Kit settings");
}

static void testFirstRunMarker()
{
	// A write that works: the marker is on disk and read back.
	QTemporaryDir dir;
	Settings s;
	CHECK(kitsettings::saveFirstRunDone(dir.path(), s), "saveFirstRunDone: a normal write is verified by the read-back");
	CHECK(s.firstRunDone && kitsettings::load(dir.path()).firstRunDone, "the marker is true in memory and on disk");

	// A write that cannot work: the config root is a file, so the folder cannot be made. False, but the in-memory
	// flag is still set (a later successful save of any setting then writes it).
	QTemporaryDir bad;
	const QString blocker = QDir(bad.path()).filePath(QStringLiteral("plugin_config"));
	QFile f(blocker);
	CHECK(f.open(QIODevice::WriteOnly) && f.write("x") == 1, "a file stands where the config folder should be");
	f.close();
	Settings t;
	CHECK(!kitsettings::save(blocker, t), "save into a root that is a file fails");
	CHECK(!kitsettings::saveFirstRunDone(blocker, t), "saveFirstRunDone reports the failed write (not verified)");
	CHECK(t.firstRunDone, "...and the in-memory marker stays true");
	CHECK(!kitsettings::saveFirstRunDone(QString(), t), "an empty config root fails too");

	// The decision.
	CHECK(kitsettings::shouldForceDock(false, false), "no marker and no saved place: force the dock (a true first run)");
	CHECK(!kitsettings::shouldForceDock(false, true), "no marker but OBS has a saved place: leave the layout alone");
	CHECK(!kitsettings::shouldForceDock(true, false), "marker set: never force");
	CHECK(!kitsettings::shouldForceDock(true, true), "marker set and a saved place: never force");
}

static QByteArray fakeDockState(const QStringList &names)
{
	// What QMainWindow::saveState looks like around the names: a header, then per dock its object name as a
	// QDataStream QString, with other bytes between them.
	QByteArray raw;
	QDataStream out(&raw, QIODevice::WriteOnly);
	out << quint32(0x12345678) << qint32(2);
	for (const QString &n : names) {
		out << quint8(1) << n << qint32(-1) << qint32(300);
	}
	return raw.toBase64();
}

static void testDockState()
{
	const QString id = QStringLiteral("rws-stream-kit");
	CHECK(kitsettings::dockStateHasDock(fakeDockState({QStringLiteral("sourcesDock"), id}), id), "a saved state that holds the dock is found");
	CHECK(kitsettings::dockStateHasDock(fakeDockState({id}), id), "...also when it is the only dock");
	CHECK(!kitsettings::dockStateHasDock(fakeDockState({QStringLiteral("sourcesDock"), QStringLiteral("mixerDock")}), id), "a saved state without the dock is not");
	CHECK(!kitsettings::dockStateHasDock(fakeDockState({QStringLiteral("rws-stream-kit-two")}), id), "a longer name that starts with the id is not a match");
	CHECK(!kitsettings::dockStateHasDock(fakeDockState({QStringLiteral("x-rws-stream-kit")}), id), "a longer name that ends with the id is not a match");
	CHECK(!kitsettings::dockStateHasDock(QByteArray(), id), "empty state: no");
	CHECK(!kitsettings::dockStateHasDock(QByteArray("!!!not base64!!!"), id), "garbage state: no");
	CHECK(!kitsettings::dockStateHasDock(fakeDockState({id}), QString()), "an empty object name never matches");
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	testDefaults();
	testParse();
	testMerge();
	testFiles();
	testFirstRunMarker();
	testDockState();
	std::printf("%s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", total, failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
