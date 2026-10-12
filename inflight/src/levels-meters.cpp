/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "levels-meters.hpp"
#include "inflight-clock.hpp"
#include "kit-cost.hpp"

#include <plugin-support.h>

namespace inflight {
static_assert(MAX_AUDIO_CHANNELS == LevelAccumulator::kMaxChannels, "channel slot count must match libobs");

LevelsMeters::~LevelsMeters()
{
	detachAll();
}

// Audio thread. Post-fader peak and magnitude, max over the meter's current channel slots. No
// allocation, no lock, no logging: two small loops and a few atomics (same callback as AudioWatch).
void LevelsMeters::onVolmeter(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			      const float peak[MAX_AUDIO_CHANNELS], const float[])
{
	kitcost::Scope costScope(kitcost::VolmeterLevels); // atomics only
	auto *acc = static_cast<LevelsHandoff *>(param);
	const int n = acc->channels();
	acc->onCallback(LevelAccumulator::maxDb(peak, n), LevelAccumulator::maxDb(magnitude, n), nowMs());
}

void LevelsMeters::setWanted(const QStringList &uuids)
{
	// Detach what is no longer wanted, and what OBS has removed.
	const QStringList have = meters_.keys();
	for (const QString &u : have) {
		const std::shared_ptr<Meter> &m = meters_[u];
		if (!uuids.contains(u))
			detach(u, "no longer wanted");
		else if (m->src && obs_source_removed(m->src))
			detach(u, "source removed");
	}
	for (const QString &u : uuids) {
		if (u.isEmpty() || meters_.contains(u))
			continue;
		auto fail = [&](const char *why) {
			if (!failedLogged_.contains(u)) { // once per uuid; retried every call, quietly
				failedLogged_.push_back(u);
				obs_log(LOG_INFO, "levels: attach failed (%s)", why);
			}
		};
		obs_source_t *src = obs_get_source_by_uuid(u.toUtf8().constData()); // new reference
		if (!src) {
			fail("source not found");
			continue;
		}
		if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) || obs_source_removed(src)) {
			obs_source_release(src);
			fail("not an audio source");
			continue;
		}
		obs_volmeter_t *vol = obs_volmeter_create(OBS_FADER_LOG); // same mapping as OBS's mixer
		if (!vol) {
			obs_source_release(src);
			fail("volmeter create failed");
			continue;
		}
		auto m = std::make_shared<Meter>();
		m->acc = std::make_unique<LevelsHandoff>();
		obs_volmeter_add_callback(vol, &LevelsMeters::onVolmeter, m->acc.get());
		if (!obs_volmeter_attach_source(vol, src)) {
			obs_volmeter_remove_callback(vol, &LevelsMeters::onVolmeter, m->acc.get());
			obs_volmeter_destroy(vol);
			obs_source_release(src);
			fail("volmeter attach failed");
			continue;
		}
		m->uuid = u;
		m->src = src;
		m->vol = vol;
		m->acc->setChannels(obs_volmeter_get_nr_channels(vol));
		meters_.insert(u, m);
		failedLogged_.removeAll(u);
		obs_log(LOG_INFO, "levels: meter attached (%d now)", static_cast<int>(meters_.size()));
	}
	// The slot table: slot i = the i-th wanted uuid (the caller's row order).
	slots_.clear();
	slots_.reserve(uuids.size());
	for (const QString &u : uuids)
		slots_.push_back(meters_.value(u));
}

// remove_callback FIRST: it takes the lock the volmeter holds while dispatching, so no callback runs
// after it returns. Only then is it safe to free the accumulator the callback pointed at.
void LevelsMeters::detach(const QString &uuid, const char *reason)
{
	auto it = meters_.find(uuid);
	if (it == meters_.end())
		return;
	std::shared_ptr<Meter> m = it.value();
	meters_.erase(it);
	for (std::shared_ptr<Meter> &s : slots_) {
		if (s == m)
			s.reset(); // a slot never points at a torn-down meter
	}
	if (m->vol) {
		obs_volmeter_remove_callback(m->vol, &LevelsMeters::onVolmeter, m->acc.get());
		obs_volmeter_detach_source(m->vol);
		obs_volmeter_destroy(m->vol);
		m->vol = nullptr;
	}
	if (m->src) {
		obs_source_release(m->src);
		m->src = nullptr;
	}
	m->acc.reset();
	obs_log(LOG_INFO, "levels: meter detached (%s, %d left)", reason, static_cast<int>(meters_.size()));
}

void LevelsMeters::detachAll()
{
	const QStringList have = meters_.keys();
	for (const QString &u : have)
		detach(u, "detach all");
	slots_.clear();
	failedLogged_.clear();
}

LevelsMeters::SlotReading LevelsMeters::takeSlot(int slot, qint64 tMs)
{
	SlotReading r;
	if (slot < 0 || slot >= slots_.size() || !slots_[slot])
		return r;
	Meter &m = *slots_[slot];
	if (!m.acc || !m.vol)
		return r;
	r.attached = true;
	// obs_volmeter_get_nr_channels takes no lock (same call AudioWatch makes each tick); the audio
	// thread sees the result through an atomic.
	m.acc->setChannels(obs_volmeter_get_nr_channels(m.vol));
	const LevelsHandoff::Reading a = m.acc->take();
	r.fresh = a.fresh;
	r.available = a.lastCallbackMs != 0 && tMs - a.lastCallbackMs <= kStaleMs;
	if (a.fresh) {
		r.peakDb = a.peakDb;
		r.avgDb = a.avgDb;
	}
	return r;
}

QString LevelsMeters::sourceName(int slot) const
{
	if (slot < 0 || slot >= slots_.size() || !slots_[slot] || !slots_[slot]->src)
		return QString();
	const char *nm = obs_source_get_name(slots_[slot]->src);
	return nm ? QString::fromUtf8(nm) : QString();
}

} // namespace inflight
