/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kit-stage.hpp"

namespace kitstage {

Machine::Machine(Tools tools, Options options) : tools_(tools), options_(options)
{
	stage_ = firstVisible();
}

bool Machine::enabled(Stage s) const
{
	switch (s) {
	case Stage::PreFlight:
		return tools_.preflight;
	case Stage::InFlight:
		return tools_.inflight;
	case Stage::PostFlight:
		return tools_.postflight;
	}
	return false;
}

Stage Machine::firstVisible() const
{
	if (tools_.preflight)
		return Stage::PreFlight;
	if (tools_.inflight)
		return Stage::InFlight;
	if (tools_.postflight)
		return Stage::PostFlight;
	return Stage::PreFlight; // nothing enabled: defined fallback
}

std::vector<Stage> Machine::visibleStages() const
{
	std::vector<Stage> v;
	if (tools_.preflight)
		v.push_back(Stage::PreFlight);
	if (tools_.inflight)
		v.push_back(Stage::InFlight);
	if (tools_.postflight)
		v.push_back(Stage::PostFlight);
	return v;
}

Stage Machine::stage() const
{
	return peekSet_ ? peek_ : stage_;
}

bool Machine::peeking() const
{
	return peekSet_ && peek_ != stage_;
}

bool Machine::sessionActive() const
{
	return active_;
}

bool Machine::ended() const
{
	return ended_;
}

bool Machine::resetAllowed() const
{
	return !active_;
}

bool Machine::offTimerRunning() const
{
	return active_ && offRunning_;
}

uint32_t Machine::sessionsStarted() const
{
	return started_;
}

void Machine::setTools(Tools tools)
{
	tools_ = tools;
	if (!enabled(stage_))
		stage_ = firstVisible();
	if (peekSet_ && !enabled(peek_))
		peekSet_ = false;
}

void Machine::setOptions(Options options)
{
	options_ = options;
}

// A clock that goes backwards must not fire the off timer (or underflow the elapsed time): the
// confirmation restarts from the new time.
void Machine::seeClock(int64_t nowMs)
{
	if (haveClock_ && nowMs < lastNow_ && offRunning_)
		offSince_ = nowMs;
	lastNow_ = nowMs;
	haveClock_ = true;
}

void Machine::checkOffTimer(int64_t nowMs)
{
	if (!active_ || !offRunning_ || nowMs < offSince_)
		return;
	// unsigned difference: exact even across the whole int64 range, no signed overflow
	const uint64_t elapsed = static_cast<uint64_t>(nowMs) - static_cast<uint64_t>(offSince_);
	const uint64_t need = options_.offConfirmMs > 0 ? static_cast<uint64_t>(options_.offConfirmMs) : 0;
	if (elapsed >= need)
		endSession();
}

void Machine::endSession()
{
	active_ = false;
	offRunning_ = false;
	ended_ = true;
	if (!tools_.inflight)
		return; // In-Flight off: tracked, but the stage is not ours to move
	peekSet_ = false;
	if (tools_.postflight)
		stage_ = Stage::PostFlight;
}

void Machine::onLive(Live live, uint64_t sessionId, int64_t nowMs)
{
	seeClock(nowMs);
	switch (live) {
	case Live::Live:
		offRunning_ = false;
		if (!active_) {
			active_ = true;
			ended_ = false;
			sessionId_ = sessionId;
			started_++;
			if (options_.autoSwitchOnLive && tools_.inflight) {
				stage_ = Stage::InFlight;
				peekSet_ = false;
			}
		} else {
			sessionId_ = sessionId; // same live session continuing
		}
		break;
	case Live::Unknown:
		// Off is no longer confirmed continuously; nothing starts or ends.
		offRunning_ = false;
		break;
	case Live::Off:
		if (active_ && !offRunning_) {
			offRunning_ = true;
			offSince_ = nowMs;
		}
		checkOffTimer(nowMs);
		break;
	}
}

void Machine::onTick(int64_t nowMs)
{
	seeClock(nowMs);
	checkOffTimer(nowMs);
}

bool Machine::done(int64_t nowMs)
{
	seeClock(nowMs);
	checkOffTimer(nowMs);
	if (stage_ != Stage::PreFlight || active_ || !tools_.inflight)
		return false;
	stage_ = Stage::InFlight;
	peekSet_ = false;
	return true;
}

void Machine::peek(Stage s)
{
	if (!enabled(s))
		return;
	peekSet_ = true;
	peek_ = s;
}

bool Machine::reset(int64_t nowMs)
{
	seeClock(nowMs);
	checkOffTimer(nowMs);
	if (active_)
		return false;
	stage_ = firstVisible();
	ended_ = false;
	peekSet_ = false;
	offRunning_ = false;
	return true;
}

} // namespace kitstage
