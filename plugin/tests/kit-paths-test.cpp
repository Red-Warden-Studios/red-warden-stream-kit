// Tests for common/kit-paths: the helper that keeps every tool in its pre-merge plugin_config folder
// now that the three tools share one OBS module. No OBS, no Qt: the one OBS call is a fake provider.
#include "kit-paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

// What obs_module_config_path returns for the Kit module ("rws-stream-kit"): a malloc'ed string.
static const char *g_moduleDir = "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-stream-kit/";
static int g_freed = 0;
static int g_calls = 0;
static std::string g_lastFile;

static char *fakeConfigPath(const char *file)
{
	g_calls++;
	g_lastFile = file ? file : "<null>";
	if (!g_moduleDir)
		return nullptr;
	const std::string s = std::string(g_moduleDir) + (file ? file : "");
	char *out = static_cast<char *>(std::malloc(s.size() + 1));
	std::memcpy(out, s.c_str(), s.size() + 1);
	return out;
}
static void fakeFree(void *p)
{
	g_freed++;
	std::free(p);
}

int main()
{
	using namespace kitpaths;

	// --- pure string logic ---
	CHECK(rootFromModuleDir("C:/a/plugin_config/rws-stream-kit/") == "C:/a/plugin_config", "root: trailing slash");
	CHECK(rootFromModuleDir("C:/a/plugin_config/rws-stream-kit") == "C:/a/plugin_config", "root: no trailing slash");
	CHECK(rootFromModuleDir("C:\\a\\plugin_config\\rws-stream-kit\\") == "C:\\a\\plugin_config", "root: backslashes");
	CHECK(rootFromModuleDir("C:/a/plugin_config/rws-stream-kit//") == "C:/a/plugin_config", "root: doubled trailing slash");
	CHECK(rootFromModuleDir("").empty(), "root: empty in, empty out");
	CHECK(rootFromModuleDir("rws-stream-kit").empty(), "root: no separator, no parent");
	CHECK(rootFromModuleDir("/").empty(), "root: just a slash");
	CHECK(join("a/b", "c") == "a/b/c", "join: adds one slash");
	CHECK(join("a/b/", "c") == "a/b/c", "join: no doubled slash");
	CHECK(join("", "c") == "c" && join("a", "") == "a", "join: empty sides");

	// --- no provider: everything is empty, nothing crashes ---
	setProvider(nullptr, nullptr);
	CHECK(pluginConfigRoot().empty(), "no provider: root empty");
	CHECK(toolDir(kMultistream).empty(), "no provider: toolDir empty");
	CHECK(toolFile(kPreflight, "settings.json").empty(), "no provider: toolFile empty");

	// --- a provider that returns null (OBS could not name the folder) ---
	g_moduleDir = nullptr;
	setProvider(fakeConfigPath, fakeFree);
	CHECK(pluginConfigRoot().empty(), "null from OBS: root empty");
	CHECK(toolFile(kInflight, "settings.json").empty(), "null from OBS: toolFile empty");
	CHECK(g_freed == 0, "null from OBS: nothing to free");

	// --- the real shape ---
	g_moduleDir = "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-stream-kit/";
	g_freed = 0;
	g_calls = 0;
	CHECK(pluginConfigRoot() == "C:/Users/x/AppData/Roaming/obs-studio/plugin_config", "root is plugin_config");
	CHECK(g_lastFile.empty(), "root asks OBS for the module folder (empty file name)");
	CHECK(g_freed == g_calls && g_calls == 1, "the OBS string is freed (once per call)");
	CHECK(toolDir(kMultistream) == "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-multistream/",
	      "Multistream folder is its old one, with the trailing slash OBS used to return");
	CHECK(toolDir(kPreflight) == "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-preflight/", "Pre-Flight folder");
	CHECK(toolDir(kInflight) == "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-inflight/", "In-Flight folder");
	CHECK(toolFile(kMultistream, "settings.json") ==
		      "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-multistream/settings.json",
	      "Multistream settings.json");
	CHECK(toolFile(kMultistream, "hotkeys.json") ==
		      "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-multistream/hotkeys.json",
	      "Multistream hotkeys.json");
	CHECK(toolFile(kMultistream, "reports") == "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-multistream/reports",
	      "Multistream reports (no trailing slash, as before)");
	CHECK(toolFile(kPreflight, "") == toolDir(kPreflight), "empty file name is the folder itself");
	CHECK(toolFile(kPreflight, nullptr) == toolDir(kPreflight), "null file name is the folder itself");
	CHECK(toolDir(nullptr).empty() && toolDir("").empty(), "null or empty tool name: empty");
	CHECK(g_freed == g_calls, "every OBS string freed");

	// In-Flight's import path is built from its own folder: .../rws-inflight/ + ../rws-preflight/settings.json
	// resolves to Pre-Flight's file; with the helper it is simply Pre-Flight's toolFile.
	CHECK(toolFile(kPreflight, "settings.json") ==
		      "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-preflight/settings.json",
	      "Pre-Flight settings.json (the file In-Flight imports)");

	// The shared Kit settings are the module's own folder: plugin_config/rws-stream-kit/settings.json
	CHECK(join(pluginConfigRoot(), "rws-stream-kit/settings.json") ==
		      "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/rws-stream-kit/settings.json",
	      "shared Kit settings location unchanged");

	// A module folder given with backslashes (not what OBS returns, but be safe)
	g_moduleDir = "C:\\obs\\plugin_config\\rws-stream-kit\\";
	CHECK(pluginConfigRoot() == "C:\\obs\\plugin_config", "backslash module folder");
	CHECK(toolFile(kInflight, "settings.json") == "C:\\obs\\plugin_config/rws-inflight/settings.json",
	      "backslash root joins with a forward slash (Windows accepts both)");

	setProvider(nullptr, nullptr);
	std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
