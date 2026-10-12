/*
Red Warden Multistream
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "config.hpp"
#include "kit-paths.hpp"

#include <string>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QUuid>

static QString profileFile(const char *name)
{
	char *profile = obs_frontend_get_current_profile_path();
	if (!profile)
		return {};
	QString path = QString::fromUtf8(profile) + "/" + name;
	bfree(profile);
	return path;
}

QString configPath()
{
	return profileFile("rws-multistream.json");
}

// 0.1.x shipped under the working name "Beacon".
static QString legacyConfigPath()
{
	return profileFile("rws-beacon.json");
}

QString newDestinationId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static QString str(obs_data_t *d, const char *k)
{
	return QString::fromUtf8(obs_data_get_string(d, k));
}

MultistreamConfig loadConfig()
{
	MultistreamConfig cfg;
	const QString path = configPath();
	if (path.isEmpty())
		return cfg;

	QString readPath = path;
	if (!os_file_exists(path.toUtf8().constData()) && os_file_exists(legacyConfigPath().toUtf8().constData()))
		readPath = legacyConfigPath(); // migrated on the next save
	obs_data_t *root = obs_data_create_from_json_file_safe(readPath.toUtf8().constData(), "bak");
	if (!root)
		return cfg;

	const int version = (int)obs_data_get_int(root, "version");
	obs_data_array_t *arr = obs_data_get_array(root, "destinations");
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *it = obs_data_array_item(arr, i);
		Destination d;
		d.id = str(it, "id");
		d.name = str(it, "name");
		d.platform = str(it, "platform");
		d.server = str(it, "server");
		d.enabled = obs_data_get_bool(it, "enabled");

		obs_data_t *e = obs_data_get_obj(it, "encoder");
		if (e) {
			d.enc.shared = obs_data_has_user_value(e, "shared") ? obs_data_get_bool(e, "shared") : true;
			d.enc.encoderId = str(e, "encoder_id");
			if (obs_data_has_user_value(e, "video_bitrate"))
				d.enc.videoBitrate = (int)obs_data_get_int(e, "video_bitrate");
			if (obs_data_has_user_value(e, "audio_bitrate"))
				d.enc.audioBitrate = (int)obs_data_get_int(e, "audio_bitrate");
			if (obs_data_has_user_value(e, "audio_track"))
				d.enc.audioTrack = (int)obs_data_get_int(e, "audio_track");
			// v1 (0.1.0) saved hardcoded defaults of 160 kbps / Track 1 even when
			// the user never touched them. Treat that exact pair as "match OBS".
			if (version < 2 && d.enc.audioBitrate == 160 && d.enc.audioTrack == 0) {
				d.enc.audioBitrate = 0;
				d.enc.audioTrack = -1;
			}
			d.enc.width = (int)obs_data_get_int(e, "width");
			d.enc.height = (int)obs_data_get_int(e, "height");
			obs_data_release(e);
		}
		if (obs_data_array_t *hk = obs_data_get_array(it, "hotkey")) {
			obs_data_t *wrap = obs_data_create();
			obs_data_set_array(wrap, "b", hk);
			d.hotkeyJson = QString::fromUtf8(obs_data_get_json(wrap));
			obs_data_release(wrap);
			obs_data_array_release(hk);
		}
		if (d.id.isEmpty())
			d.id = newDestinationId();
		cfg.destinations.push_back(d);
		obs_data_release(it);
	}
	obs_data_array_release(arr);
	obs_data_release(root);
	return cfg;
}

bool saveConfig(const MultistreamConfig &cfg)
{
	const QString path = configPath();
	if (path.isEmpty())
		return false;

	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "version", 2);
	obs_data_array_t *arr = obs_data_array_create();
	for (const auto &d : cfg.destinations) {
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "id", d.id.toUtf8().constData());
		obs_data_set_string(it, "name", d.name.toUtf8().constData());
		obs_data_set_string(it, "platform", d.platform.toUtf8().constData());
		obs_data_set_string(it, "server", d.server.toUtf8().constData());
		obs_data_set_bool(it, "enabled", d.enabled);

		obs_data_t *e = obs_data_create();
		obs_data_set_bool(e, "shared", d.enc.shared);
		obs_data_set_string(e, "encoder_id", d.enc.encoderId.toUtf8().constData());
		obs_data_set_int(e, "video_bitrate", d.enc.videoBitrate);
		obs_data_set_int(e, "audio_bitrate", d.enc.audioBitrate);
		obs_data_set_int(e, "audio_track", d.enc.audioTrack);
		obs_data_set_int(e, "width", d.enc.width);
		obs_data_set_int(e, "height", d.enc.height);
		obs_data_set_obj(it, "encoder", e);
		obs_data_release(e);

		if (!d.hotkeyJson.isEmpty()) {
			obs_data_t *wrap = obs_data_create_from_json(d.hotkeyJson.toUtf8().constData());
			if (wrap) {
				obs_data_array_t *hk = obs_data_get_array(wrap, "b");
				if (hk) {
					obs_data_set_array(it, "hotkey", hk);
					obs_data_array_release(hk);
				}
				obs_data_release(wrap);
			}
		}

		obs_data_array_push_back(arr, it);
		obs_data_release(it);
	}
	obs_data_set_array(root, "destinations", arr);
	obs_data_array_release(arr);

	const bool ok = obs_data_save_json_safe(root, path.toUtf8().constData(), "tmp", "bak");
	obs_data_release(root);
	if (!ok)
		obs_log(LOG_WARNING, "could not save destinations to the profile folder");
	return ok;
}

GlobalSettings loadGlobalSettings()
{
	GlobalSettings g;
	const std::string pathStr = kitpaths::toolFile(kitpaths::kMultistream, "settings.json");
	if (pathStr.empty())
		return g;
	const char *path = pathStr.c_str();
	if (obs_data_t *root = obs_data_create_from_json_file_safe(path, "bak")) {
		g.uploadMbps = obs_data_get_double(root, "upload_mbps");
		g.uploadSource = str(root, "upload_source");
		g.uploadWhen = str(root, "upload_when");
		if (obs_data_has_user_value(root, "check_updates"))
			g.checkUpdates = obs_data_get_bool(root, "check_updates");
		obs_data_release(root);
	}
	return g;
}

void saveGlobalSettings(const GlobalSettings &g)
{
	const std::string dirStr = kitpaths::toolDir(kitpaths::kMultistream);
	if (!dirStr.empty())
		os_mkdirs(dirStr.c_str());
	const std::string pathStr = kitpaths::toolFile(kitpaths::kMultistream, "settings.json");
	if (pathStr.empty())
		return;
	const char *path = pathStr.c_str();
	obs_data_t *root = obs_data_create();
	obs_data_set_double(root, "upload_mbps", g.uploadMbps);
	obs_data_set_string(root, "upload_source", g.uploadSource.toUtf8().constData());
	obs_data_set_string(root, "upload_when", g.uploadWhen.toUtf8().constData());
	obs_data_set_bool(root, "check_updates", g.checkUpdates);
	obs_data_save_json_safe(root, path, "tmp", "bak");
	obs_data_release(root);
}
