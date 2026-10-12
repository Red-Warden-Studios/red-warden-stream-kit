/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "inflight-settings.hpp"

#include "kit-paths.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

namespace inflight {
namespace {
QString getStr(obs_data_t *d, const char *key)
{
	const char *v = obs_data_get_string(d, key);
	return v ? QString::fromUtf8(v) : QString();
}

void setStr(obs_data_t *d, const char *key, const QString &v)
{
	obs_data_set_string(d, key, v.toUtf8().constData());
}

// If a file exists but cannot be read as JSON, move it to <path>.bad (replacing an older .bad).
void setAsideIfDamaged(const char *path)
{
	if (!os_file_exists(path))
		return;
	if (obs_data_t *ok = obs_data_create_from_json_file(path)) {
		obs_data_release(ok);
		return;
	}
	const QByteArray bad = QByteArray(path) + ".bad";
	os_unlink(bad.constData());
	if (os_rename(path, bad.constData()) == 0)
		obs_log(LOG_WARNING, "a settings file could not be read; it was kept as %s", bad.constData());
	else
		obs_log(LOG_WARNING, "a settings file could not be read and could not be set aside: %s", path);
}
} // namespace

Settings Settings::load()
{
	Settings s;
	const std::string pathStr = kitpaths::toolFile(kitpaths::kInflight, "settings.json");
	if (pathStr.empty())
		return s;
	const char *path = pathStr.c_str();
	if (obs_data_t *d = obs_data_create_from_json_file_safe(path, "bak")) {
		obs_data_set_default_bool(d, "setup_done", s.setupDone);
		obs_data_set_default_bool(d, "record_intent", s.recordIntent);
		obs_data_set_default_bool(d, "light_mic", s.lightMic);
		obs_data_set_default_bool(d, "light_desktop", s.lightDesktop);
		obs_data_set_default_bool(d, "light_scene", s.lightScene);
		obs_data_set_default_bool(d, "light_recording", s.lightRecording);
		obs_data_set_default_bool(d, "light_network", s.lightNetwork);
		obs_data_set_default_bool(d, "light_pc", s.lightPc);
		obs_data_set_default_bool(d, "light_stream", s.lightStream);
		obs_data_set_default_int(d, "mic_muted_amber_s", s.micMutedAmberS);
		obs_data_set_default_int(d, "mic_muted_red_s", s.micMutedRedS);
		obs_data_set_default_bool(d, "heard_you_enabled", s.heardYouEnabled);
		obs_data_set_default_int(d, "heard_you_silence_s", s.heardYouSilenceS);
		obs_data_set_default_int(d, "heard_you_dbfs", s.heardYouDbfs);
		obs_data_set_default_int(d, "desktop_silence_s", s.desktopSilenceS);
		obs_data_set_default_int(d, "scene_red_extra_s", s.sceneRedExtraS);
		obs_data_set_default_int(d, "record_start_grace_s", s.recordStartGraceS);
		obs_data_set_default_double(d, "drop_amber_pct", s.dropAmberPct);
		obs_data_set_default_double(d, "drop_red_pct", s.dropRedPct);
		obs_data_set_default_bool(d, "chime", s.chime);
		obs_data_set_default_bool(d, "check_updates", s.updateChecks);
		obs_data_set_default_bool(d, "preflight_note_seen", s.preflightNoteSeen);
		obs_data_set_default_bool(d, "levels_open", s.levelsOpen);
		obs_data_set_default_string(d, "levels_mode", "db");
		obs_data_set_default_bool(d, "levels_true_peak", s.levelsTruePeak);
		obs_data_set_default_bool(d, "levels_lufs_ref_on", s.levelsLufsRefOn);
		obs_data_set_default_double(d, "levels_lufs_ref", s.levelsLufsRef);

		s.setupDone = obs_data_get_bool(d, "setup_done");
		s.micUuid = getStr(d, "mic_uuid");
		s.desktopUuid = getStr(d, "desktop_uuid");
		s.recordIntent = obs_data_get_bool(d, "record_intent");
		s.lightMic = obs_data_get_bool(d, "light_mic");
		s.lightDesktop = obs_data_get_bool(d, "light_desktop");
		s.lightScene = obs_data_get_bool(d, "light_scene");
		s.lightRecording = obs_data_get_bool(d, "light_recording");
		s.lightNetwork = obs_data_get_bool(d, "light_network");
		s.lightPc = obs_data_get_bool(d, "light_pc");
		s.lightStream = obs_data_get_bool(d, "light_stream");
		s.micMutedAmberS = (int)obs_data_get_int(d, "mic_muted_amber_s");
		s.micMutedRedS = (int)obs_data_get_int(d, "mic_muted_red_s");
		s.heardYouEnabled = obs_data_get_bool(d, "heard_you_enabled");
		s.heardYouSilenceS = (int)obs_data_get_int(d, "heard_you_silence_s");
		s.heardYouDbfs = (int)obs_data_get_int(d, "heard_you_dbfs");
		s.desktopSilenceS = (int)obs_data_get_int(d, "desktop_silence_s");
		s.sceneRedExtraS = (int)obs_data_get_int(d, "scene_red_extra_s");
		s.recordStartGraceS = (int)obs_data_get_int(d, "record_start_grace_s");
		s.dropAmberPct = obs_data_get_double(d, "drop_amber_pct");
		s.dropRedPct = obs_data_get_double(d, "drop_red_pct");
		s.chime = obs_data_get_bool(d, "chime");
		s.updateChecks = obs_data_get_bool(d, "check_updates");
		s.preflightNoteSeen = obs_data_get_bool(d, "preflight_note_seen");
		s.levelsOpen = obs_data_get_bool(d, "levels_open");
		s.levelsMode = getStr(d, "levels_mode");
		if (s.levelsMode != QStringLiteral("lufs"))
			s.levelsMode = QStringLiteral("db"); // unknown text falls back to the default meter
		s.levelsTruePeak = obs_data_get_bool(d, "levels_true_peak");
		s.levelsLufsRefOn = obs_data_get_bool(d, "levels_lufs_ref_on");
		s.levelsLufsRef = obs_data_get_double(d, "levels_lufs_ref");
		if (!(s.levelsLufsRef >= -60.0 && s.levelsLufsRef <= 0.0))
			s.levelsLufsRef = -16.0; // out of range or NaN: the default (the panel never draws it unless switched on)
		if (obs_data_array_t *lv = obs_data_get_array(d, "levels_sources")) {
			const size_t n = obs_data_array_count(lv);
			for (size_t i = 0; i < n; ++i) {
				if (obs_data_t *item = obs_data_array_item(lv, i)) {
					const QString u = getStr(item, "uuid");
					if (!u.isEmpty() && !s.levelsSources.contains(u))
						s.levelsSources.push_back(u);
					obs_data_release(item);
				}
			}
			obs_data_array_release(lv);
		}

		if (obs_data_array_t *arr = obs_data_get_array(d, "holding_scenes")) {
			const size_t n = obs_data_array_count(arr);
			for (size_t i = 0; i < n; ++i) {
				if (obs_data_t *item = obs_data_array_item(arr, i)) {
					HoldingScene h;
					h.sceneUuid = getStr(item, "scene_uuid");
					h.limitSeconds = (int)obs_data_get_int(item, "limit_seconds");
					h.label = getStr(item, "label");
					if (!h.sceneUuid.isEmpty())
						s.holdingScenes.push_back(h);
					obs_data_release(item);
				}
			}
			obs_data_array_release(arr);
		}
		obs_data_release(d);
	}
	return s;
}

bool Settings::save() const
{
	const std::string pathStr = kitpaths::toolFile(kitpaths::kInflight, "settings.json");
	if (pathStr.empty())
		return false;
	const char *path = pathStr.c_str();
	// A damaged file is kept aside, never silently replaced.
	setAsideIfDamaged(path);
	const QByteArray backupPath = QByteArray(path) + ".bak";
	setAsideIfDamaged(backupPath.constData());
	// Start from what is on disk so unknown keys are kept.
	obs_data_t *d = obs_data_create_from_json_file_safe(path, "bak");
	if (!d)
		d = obs_data_create();

	obs_data_set_bool(d, "setup_done", setupDone);
	setStr(d, "mic_uuid", micUuid);
	setStr(d, "desktop_uuid", desktopUuid);
	obs_data_set_bool(d, "record_intent", recordIntent);
	obs_data_set_bool(d, "light_mic", lightMic);
	obs_data_set_bool(d, "light_desktop", lightDesktop);
	obs_data_set_bool(d, "light_scene", lightScene);
	obs_data_set_bool(d, "light_recording", lightRecording);
	obs_data_set_bool(d, "light_network", lightNetwork);
	obs_data_set_bool(d, "light_pc", lightPc);
	obs_data_set_bool(d, "light_stream", lightStream);
	obs_data_set_int(d, "mic_muted_amber_s", micMutedAmberS);
	obs_data_set_int(d, "mic_muted_red_s", micMutedRedS);
	obs_data_set_bool(d, "heard_you_enabled", heardYouEnabled);
	obs_data_set_int(d, "heard_you_silence_s", heardYouSilenceS);
	obs_data_set_int(d, "heard_you_dbfs", heardYouDbfs);
	obs_data_set_int(d, "desktop_silence_s", desktopSilenceS);
	obs_data_set_int(d, "scene_red_extra_s", sceneRedExtraS);
	obs_data_set_int(d, "record_start_grace_s", recordStartGraceS);
	obs_data_set_double(d, "drop_amber_pct", dropAmberPct);
	obs_data_set_double(d, "drop_red_pct", dropRedPct);
	obs_data_set_bool(d, "chime", chime);
	obs_data_set_bool(d, "check_updates", updateChecks);
	obs_data_set_bool(d, "preflight_note_seen", preflightNoteSeen);
	obs_data_set_bool(d, "levels_open", levelsOpen);
	setStr(d, "levels_mode", levelsMode == QStringLiteral("lufs") ? QStringLiteral("lufs") : QStringLiteral("db"));
	obs_data_set_bool(d, "levels_true_peak", levelsTruePeak);
	obs_data_set_bool(d, "levels_lufs_ref_on", levelsLufsRefOn);
	obs_data_set_double(d, "levels_lufs_ref", levelsLufsRef);

	// The extra Levels sources, merged like the scenes below: an entry kept by uuid keeps its unknown keys.
	{
		obs_data_array_t *oldLv = obs_data_get_array(d, "levels_sources");
		obs_data_array_t *newLv = obs_data_array_create();
		for (const QString &u : levelsSources) {
			obs_data_t *item = nullptr;
			if (oldLv) {
				const size_t n = obs_data_array_count(oldLv);
				for (size_t i = 0; i < n && !item; ++i) {
					obs_data_t *it = obs_data_array_item(oldLv, i);
					if (it && getStr(it, "uuid") == u)
						item = it; // keep this reference
					else if (it)
						obs_data_release(it);
				}
			}
			if (!item)
				item = obs_data_create();
			setStr(item, "uuid", u);
			obs_data_array_push_back(newLv, item);
			obs_data_release(item);
		}
		obs_data_set_array(d, "levels_sources", newLv);
		obs_data_array_release(newLv);
		if (oldLv)
			obs_data_array_release(oldLv);
	}

	// Each scene is merged into the existing entry with the same scene_uuid, so keys this build does
	// not know about survive. Scenes no longer listed are dropped; new ones are appended.
	obs_data_array_t *old = obs_data_get_array(d, "holding_scenes");
	obs_data_array_t *arr = obs_data_array_create();
	for (const HoldingScene &h : holdingScenes) {
		obs_data_t *item = nullptr;
		if (old) {
			const size_t n = obs_data_array_count(old);
			for (size_t i = 0; i < n && !item; ++i) {
				obs_data_t *it = obs_data_array_item(old, i);
				if (it && getStr(it, "scene_uuid") == h.sceneUuid)
					item = it; // keep this reference
				else if (it)
					obs_data_release(it);
			}
		}
		if (!item)
			item = obs_data_create();
		setStr(item, "scene_uuid", h.sceneUuid);
		obs_data_set_int(item, "limit_seconds", h.limitSeconds);
		setStr(item, "label", h.label);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(d, "holding_scenes", arr);
	obs_data_array_release(arr);
	if (old)
		obs_data_array_release(old);

	const std::string dirStr = kitpaths::toolDir(kitpaths::kInflight);
	if (!dirStr.empty())
		os_mkdirs(dirStr.c_str());
	const bool ok = obs_data_save_json_safe(d, path, "tmp", "bak");
	obs_data_release(d);
	return ok;
}

} // namespace inflight
