// Tests for update-manifest.cpp: version comparison and manifest parsing.
// No framework: exits non-zero on failure.
#include "update-manifest.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <string>

using namespace updatecheck;

static int failures = 0;
#define CHECK(cond, msg)                                                      \
	do {                                                                  \
		if (!(cond)) {                                                \
			std::printf("FAIL: %s  (line %d)\n", msg, __LINE__); \
			failures++;                                           \
		} else {                                                      \
			std::printf("ok:   %s\n", msg);                        \
		}                                                             \
	} while (0)

static void writeFile(const QString &path, const QByteArray &data)
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	if (f.open(QIODevice::WriteOnly))
		f.write(data);
}
static QByteArray readAll(const QString &path)
{
	QFile f(path);
	return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int main()
{
	// --- compareVersions ---
	CHECK(compareVersions("1.0.1", "1.0.0") == 1, "patch bump is newer");
	CHECK(compareVersions("1.0.0", "1.0.0") == 0, "equal versions");
	CHECK(compareVersions("1.0.10", "1.0.9") == 1, "numeric, not text: 1.0.10 > 1.0.9");
	CHECK(compareVersions("1.1", "1.0.9") == 1, "missing parts count as 0");
	CHECK(compareVersions("1.0", "1.0.0") == 0, "1.0 == 1.0.0");
	CHECK(compareVersions("0.9.9", "1.0.0") == -1, "older is older");
	CHECK(compareVersions("v1.2.0", "1.1.9") == 1, "leading v ignored");
	CHECK(compareVersions("32.0.1-beta1", "32.0.1") == 0, "suffix after the numbers ignored");
	CHECK(compareVersions("31.1.1", "32.0.0") == -1, "OBS version compare");
	CHECK(compareVersions("garbage", "1.0.0") == -1, "unparseable counts as 0.0.0");

	// --- parseManifest ---
	Manifest m;
	CHECK(parseManifest(R"({"latest":"1.0.1","url":"https://redwardenstudios.com/division/bastion/",
		"notes":"Fixes a reconnect bug.\nSecond line","min_obs":"31.0.0"})",
			    &m),
	      "full manifest parses");
	CHECK(m.latest == "1.0.1", "latest read");
	CHECK(m.url == "https://redwardenstudios.com/division/bastion/", "our https url kept");
	CHECK(m.notes == "Fixes a reconnect bug.", "notes cut to one line");
	CHECK(m.minObs == "31.0.0", "min_obs read");

	CHECK(parseManifest(R"({"latest":"1.0.1","url":"https://evil.example.com/redwardenstudios.com/"})", &m),
	      "foreign url still parses");
	CHECK(m.url == defaultPage(), "foreign url replaced by our page");
	CHECK(parseManifest(R"({"latest":"1.0.1","url":"http://redwardenstudios.com/x"})", &m) &&
		      m.url == defaultPage(),
	      "plain http replaced");
	CHECK(parseManifest(R"({"latest":"1.0.1","url":"https://redwardenstudios.com.evil.com/"})", &m) &&
		      m.url == defaultPage(),
	      "look-alike host replaced");
	CHECK(parseManifest(R"({"latest":"1.0.1","url":"https://user@redwardenstudios.com/"})", &m) &&
		      m.url == defaultPage(),
	      "userinfo url replaced");
	CHECK(parseManifest(R"({"latest":"1.0.1"})", &m) && m.url == defaultPage() && m.minObs.isEmpty(),
	      "url and min_obs optional");

	CHECK(!parseManifest("not json", &m), "malformed JSON rejected");
	CHECK(!parseManifest("[1,2]", &m), "non-object rejected");
	CHECK(!parseManifest(R"({"url":"https://redwardenstudios.com/"})", &m), "missing latest rejected");
	CHECK(!parseManifest(R"({"latest":"soon"})", &m), "non-version latest rejected");
	CHECK(parseManifest(R"({"latest":"1.0.1","min_obs":"whenever"})", &m) && m.minObs.isEmpty(),
	      "bad min_obs ignored, not trusted");

	// --- Red Warden Stream Kit: one shared manifest, one version, one scheduler (Multistream's) ---
	CHECK(std::string(kKitVersion) == "1.1.0", "kit version is 1.1.0 (the one DLL's version)");
	CHECK(kitManifestUrl() == "https://redwardenstudios.com/updates/stream-kit.json", "kit manifest URL is stream-kit.json");
	CHECK(defaultPage() == "https://redwardenstudios.com/division/bastion/stream-kit/", "default page is the Stream Kit page");
	CHECK(parseManifest(R"({"latest":"1.0.1","url":"https://evil.example.com/"})", &m) &&
		      m.url == "https://redwardenstudios.com/division/bastion/stream-kit/",
	      "foreign url falls back to the Stream Kit page");
	CHECK(parseManifest(R"({"latest":"1.0.0","url":"https://redwardenstudios.com/division/bastion/stream-kit/"})", &m) &&
		      m.url == "https://redwardenstudios.com/division/bastion/stream-kit/",
	      "Stream Kit url kept");
	CHECK(compareVersions("1.1.0", kKitVersion) == 0, "latest == kit version: no notice");
	CHECK(compareVersions("1.1.1", kKitVersion) > 0, "latest newer than kit version: notice");
	CHECK(compareVersions("1.0.9", kKitVersion) < 0, "latest older than kit version: no notice");
	// --- Shared "check_updates" preference (plugin_config/rws-stream-kit/settings.json) ---
	CHECK(readCheckUpdatesKey(R"({"check_updates":false})") == 0, "key false -> 0");
	CHECK(readCheckUpdatesKey(R"({"check_updates":true})") == 1, "key true -> 1");
	CHECK(readCheckUpdatesKey(R"({"other":1})") == -1, "absent key -> -1");
	CHECK(readCheckUpdatesKey(R"({"check_updates":"no"})") == -1, "non-bool key -> -1");
	CHECK(readCheckUpdatesKey("garbage") == -1, "malformed JSON -> -1");
	CHECK(mergeLegacyCheckUpdates(-1, -1) == true, "migration: nothing set -> on");
	CHECK(mergeLegacyCheckUpdates(1, 1) == true, "migration: both true -> on");
	CHECK(mergeLegacyCheckUpdates(0, 1) == false, "migration: Multistream explicit false wins");
	CHECK(mergeLegacyCheckUpdates(1, 0) == false, "migration: Pre-Flight explicit false wins");
	CHECK(mergeLegacyCheckUpdates(0, -1) == false, "migration: one false, other unset -> off");
	CHECK(mergeLegacyCheckUpdates(-1, 0) == false, "migration: one unset, other false -> off");
	{
		QTemporaryDir tmp;
		CHECK(tmp.isValid(), "temp dir for the shared-file tests");
		const QString root = tmp.path();
		CHECK(kitSettingsPath(root).endsWith("rws-stream-kit/settings.json"), "shared file lives in rws-stream-kit/settings.json");
		CHECK(loadSharedCheckUpdates(root) == true, "first run, no legacy values: on");
		CHECK(readCheckUpdatesKey(readAll(kitSettingsPath(root))) == 1, "first run writes the shared file");
	}
	{
		QTemporaryDir tmp;
		const QString root = tmp.path();
		writeFile(root + "/rws-preflight/settings.json", R"({"check_updates":false,"setup_done":true})");
		writeFile(root + "/rws-multistream/settings.json", R"({"check_updates":true})");
		CHECK(loadSharedCheckUpdates(root) == false, "first run: Pre-Flight's explicit false migrates");
		writeFile(root + "/rws-preflight/settings.json", R"({"check_updates":true})");
		CHECK(loadSharedCheckUpdates(root) == false, "later runs read the shared file, not the legacy files");
		CHECK(saveSharedCheckUpdates(root, true), "save true");
		CHECK(loadSharedCheckUpdates(root) == true, "toggle in one plugin is read back by the other");
		CHECK(saveSharedCheckUpdates(root, false) && loadSharedCheckUpdates(root) == false, "save false round-trips");
	}
	{
		QTemporaryDir tmp;
		const QString root = tmp.path();
		writeFile(root + "/rws-multistream/settings.json", R"({"check_updates":false})");
		CHECK(loadSharedCheckUpdates(root) == false, "first run: Multistream's explicit false migrates");
		writeFile(kitSettingsPath(root), R"({"keep":"me","check_updates":true})");
		CHECK(saveSharedCheckUpdates(root, false), "save keeps working on an existing file");
		CHECK(readAll(kitSettingsPath(root)).contains("\"keep\""), "save keeps unknown keys");
		CHECK(loadSharedCheckUpdates(QString()) == true, "no config root: on, nothing written");
		CHECK(!saveSharedCheckUpdates(QString(), false), "no config root: save refuses");
	}
	std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
