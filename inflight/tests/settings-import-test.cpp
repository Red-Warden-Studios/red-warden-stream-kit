// Tests for inflight-import.cpp: reading another plugin's saved choices without ever failing.
// No framework: exits non-zero on failure.
#include "inflight-import.hpp"

#include <cstdio>

using namespace inflight;

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

int main()
{
	// --- full valid file ---
	{
		const ImportResult r = importFromPreflightJson(R"({
			"setup_done": true, "use_main_stream": true, "use_multistream": false,
			"mic_uuid": "aaaaaaaa-0000-0000-0000-000000000001",
			"desktop_uuid": "aaaaaaaa-0000-0000-0000-000000000002",
			"start_scene_uuid": "bbbbbbbb-0000-0000-0000-000000000003",
			"record_intent": true,
			"check_updates": true, "custom_items": [{"text": "Water"}]
		})");
		CHECK(r.parsed, "full file parses");
		CHECK(r.micFound && r.micUuid == "aaaaaaaa-0000-0000-0000-000000000001", "mic uuid read");
		CHECK(r.desktopFound && r.desktopUuid == "aaaaaaaa-0000-0000-0000-000000000002", "desktop uuid read");
		CHECK(r.recordFound && r.recordIntent, "record intent read");
		CHECK(r.anythingFound(), "something found");

		Settings s;
		applyImport(r, s);
		CHECK(s.setupDone, "apply marks setup done");
		CHECK(s.micUuid == r.micUuid && s.desktopUuid == r.desktopUuid, "apply copies the sources");
		CHECK(s.holdingScenes.isEmpty() && s.recordIntent, "apply copies record intent and adds no holding scene even though start_scene_uuid was present");
		CHECK(s.micMutedAmberS == 20 && s.dropRedPct == 2.0, "apply leaves thresholds at their defaults");
	}
	// --- record_intent false is a real answer ---
	{
		const ImportResult r = importFromPreflightJson(R"({"record_intent": false})");
		CHECK(r.recordFound && !r.recordIntent, "record intent false is found");
		Settings s;
		s.recordIntent = true;
		applyImport(r, s);
		CHECK(!s.recordIntent, "apply copies record intent false");
		CHECK(!s.setupDone, "record intent alone does not finish setup");
	}
	// --- missing keys ---
	{
		const ImportResult r = importFromPreflightJson(R"({"mic_uuid": "aaaaaaaa-0000-0000-0000-000000000001"})");
		CHECK(r.parsed && r.micFound, "partial file: mic found");
		CHECK(!r.desktopFound && !r.recordFound, "partial file: the rest not found");
		Settings s;
		s.desktopUuid = "keep-me";
		applyImport(r, s);
		CHECK(s.desktopUuid == "keep-me", "apply leaves fields that were not found");
		CHECK(s.setupDone, "a mic alone finishes setup");
	}
	// --- no mic: apply what was found, setup stays open ---
	{
		const ImportResult r = importFromPreflightJson(R"({
			"desktop_uuid": "aaaaaaaa-0000-0000-0000-000000000002", "record_intent": true
		})");
		CHECK(r.parsed && !r.micFound && r.desktopFound && r.recordFound, "no mic: the rest is found");
		Settings s;
		applyImport(r, s);
		CHECK(s.desktopUuid == r.desktopUuid && s.recordIntent, "no mic: what was found is applied");
		CHECK(!s.setupDone, "no mic: setup stays not done");
		CHECK(s.micUuid.isEmpty(), "no mic: mic stays empty");
	}
	{
		const ImportResult r = importFromPreflightJson("{}");
		CHECK(r.parsed && !r.anythingFound(), "empty object: parsed, nothing found");
	}
	// --- empty file, invalid JSON, wrong shape ---
	{
		const ImportResult r = importFromPreflightJson(QByteArray());
		CHECK(!r.parsed && !r.anythingFound(), "empty file: nothing found");
	}
	{
		const ImportResult r = importFromPreflightJson("{ this is not json");
		CHECK(!r.parsed && !r.anythingFound(), "invalid JSON: nothing found");
	}
	{
		const ImportResult r = importFromPreflightJson("[1, 2, 3]");
		CHECK(!r.parsed && !r.anythingFound(), "JSON array: nothing found");
	}
	{
		const ImportResult r = importFromPreflightJson("\"just a string\"");
		CHECK(!r.parsed && !r.anythingFound(), "JSON string: nothing found");
	}
	// --- wrong types ---
	{
		const ImportResult r = importFromPreflightJson(R"({
			"mic_uuid": 42, "desktop_uuid": true, "start_scene_uuid": ["x"], "record_intent": "yes"
		})");
		CHECK(r.parsed, "wrong types: file still parses");
		CHECK(!r.micFound && !r.desktopFound && !r.recordFound, "wrong types: nothing found");
		CHECK(r.micUuid.isEmpty() && r.desktopUuid.isEmpty(), "wrong types: no values taken");
	}
	{
		const ImportResult r = importFromPreflightJson(R"({"mic_uuid": null, "record_intent": null})");
		CHECK(!r.micFound && !r.recordFound, "null values: not found");
	}
	// --- extra unknown keys ---
	{
		const ImportResult r = importFromPreflightJson(R"({
			"future_key": {"a": [1, 2, 3]}, "another": "value",
			"mic_uuid": "aaaaaaaa-0000-0000-0000-000000000001"
		})");
		CHECK(r.parsed && r.micFound, "unknown keys are ignored");
		CHECK(!r.desktopFound, "unknown keys add nothing");
	}
	// --- empty UUID strings ---
	{
		const ImportResult r = importFromPreflightJson(R"({
			"mic_uuid": "", "desktop_uuid": "   ", "start_scene_uuid": "", "record_intent": true
		})");
		CHECK(r.parsed, "empty uuids: file parses");
		CHECK(!r.micFound && !r.desktopFound, "empty or blank uuid strings are not found");
		CHECK(r.recordFound && r.anythingFound(), "other keys are still read");
	}
	// --- settings file path ---
	{
		const QString p = preflightSettingsPath("C:/cfg/plugin_config/rws-inflight/");
		CHECK(p == "C:/cfg/plugin_config/rws-preflight/settings.json", "path beside the own config folder (trailing slash)");
		const QString q = preflightSettingsPath("C:/cfg/plugin_config/rws-inflight");
		CHECK(q == "C:/cfg/plugin_config/rws-preflight/settings.json", "path beside the own config folder");
		CHECK(preflightSettingsPath(QString()).isEmpty(), "empty config folder gives an empty path");
	}
	std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
