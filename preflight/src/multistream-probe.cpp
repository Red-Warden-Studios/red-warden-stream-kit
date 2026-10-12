/*
Red Warden Pre-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multistream-probe.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QtGlobal>

namespace preflight {
namespace {
constexpr qint64 kPollEveryMs = 5000;

const char *statusName(MultistreamStatus s)
{
	switch (s) {
	case MultistreamStatus::Pending:
		return "pending";
	case MultistreamStatus::NotFound:
		return "not found";
	case MultistreamStatus::Ok:
		return "ok";
	case MultistreamStatus::Error:
		return "error";
	}
	return "?";
}
} // namespace

void MultistreamProbe::setSource(Source src)
{
	src_ = std::move(src);
	poked_ = true; // ask again at the next poll
}

void MultistreamProbe::start()
{
	started_ = true;
	// Test hook only (needs RWS_PREFLIGHT_TEST too): ask on every poll, so exit is exercised with the probe at its busiest.
	hammer_ = qEnvironmentVariableIsSet("RWS_PREFLIGHT_TEST") && qEnvironmentVariableIsSet("RWS_PREFLIGHT_TEST_HAMMER");
}

void MultistreamProbe::poke()
{
	poked_ = true;
}

void MultistreamProbe::stop()
{
	started_ = false;
	src_ = nullptr; // nothing may be asked (or held) after EXIT
}

void MultistreamProbe::poll(qint64 nowMs)
{
	if (!started_)
		return;
	const bool due = poked_ || hammer_ || last_.status == MultistreamStatus::Pending || nowMs < lastAskMs_ ||
			 nowMs - lastAskMs_ >= kPollEveryMs;
	if (!due)
		return;
	poked_ = false;
	lastAskMs_ = nowMs;

	MultistreamResult r;
	const std::optional<std::vector<MultistreamDestination>> dests = src_ ? src_() : std::nullopt;
	if (!dests) {
		r.status = MultistreamStatus::NotFound; // no Multistream (off, absent, closing): the verdict a missing vendor gave
	} else {
		r.status = MultistreamStatus::Ok;
		for (const MultistreamDestination &d : *dests) {
			if (!d.enabled)
				continue;
			r.names << (d.name.isEmpty() ? d.id : d.name);
		}
	}
	last_ = r;

	// State changes only, never per poll.
	if (!logged_ || loggedResult_.status != r.status || loggedResult_.names != r.names) {
		obs_log(LOG_INFO, "multistream probe: %s (%d enabled)", statusName(r.status), (int)r.names.size());
		loggedResult_ = r;
		logged_ = true;
	}
	if (!loggedHook_ && qEnvironmentVariableIsSet("RWS_PREFLIGHT_TEST")) {
		loggedHook_ = true;
		obs_log(LOG_INFO, "multistream-probe direct-bridge (UI thread, no worker)");
	}
}

} // namespace preflight
