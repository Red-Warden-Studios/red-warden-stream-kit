/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "multistream-live.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QtGlobal>

namespace inflight {
namespace {
constexpr qint64 kPollEveryMs = 2000;
constexpr qint64 kPollEveryNotFoundMs = 10000;

const char *statusName(MsStatus s)
{
	switch (s) {
	case MsStatus::Pending:
		return "pending";
	case MsStatus::NotFound:
		return "not found";
	case MsStatus::Ok:
		return "ok";
	case MsStatus::Error:
		return "error";
	}
	return "?";
}
} // namespace

void MultistreamLive::setSource(Source src)
{
	src_ = std::move(src);
	asked_ = false; // ask again at the next poll instead of waiting out the cadence
}

void MultistreamLive::start()
{
	started_ = true;
}

void MultistreamLive::stop()
{
	started_ = false;
	src_ = nullptr; // nothing may be asked (or held) after EXIT
}

MsReply MultistreamLive::poll(qint64 nowMs)
{
	if (!started_)
		return last_;
	const qint64 every = last_.status == MsStatus::NotFound ? kPollEveryNotFoundMs : kPollEveryMs;
	if (asked_ && nowMs >= lastAskMs_ && nowMs - lastAskMs_ < every)
		return last_;
	asked_ = true;
	lastAskMs_ = nowMs;

	MsReply r;
	r.atMs = nowMs; // stamped when the answer was read: always fresh, so combineLive's 10 s rule cannot trip
	const std::optional<QStringList> states = src_ ? src_() : std::nullopt;
	if (!states) {
		r.status = MsStatus::NotFound; // no Multistream (off, absent, closing): the same verdict the missing vendor gave
	} else {
		r.status = MsStatus::Ok;
		r.anyLive = anyDestinationLive(*states);
	}
	last_ = r;

	// Status changes only, never per poll.
	if (!logged_ || loggedStatus_ != r.status || loggedLive_ != r.anyLive) {
		obs_log(LOG_INFO, "multistream live probe: %s (any live: %s)", statusName(r.status), r.anyLive ? "yes" : "no");
		loggedStatus_ = r.status;
		loggedLive_ = r.anyLive;
		logged_ = true;
	}
	if (!loggedHook_ && qEnvironmentVariableIsSet("RWS_INFLIGHT_TEST")) {
		loggedHook_ = true;
		obs_log(LOG_INFO, "multistream-live direct-bridge (UI thread, no worker)");
	}
	return last_;
}

} // namespace inflight
