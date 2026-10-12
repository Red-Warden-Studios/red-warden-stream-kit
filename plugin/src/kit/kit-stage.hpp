/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// The stage machine behind the one-dock UI. Pure: no OBS, no Qt, no I/O,
// no clock of its own (every call is handed nowMs), and nothing is persisted: a new Machine always
// starts on the first visible stage. Unit-tested by tests/kit-stage-test.cpp.
//
// Choices where the spec is silent:
//  - In-Flight disabled: live input is still TRACKED (sessionActive / ended / resetAllowed stay
//    truthful) but never touches the stage or a peek, on start or on end. The dock just sits on
//    Pre-Flight, as the spec says.
//  - The end-of-session move to Post-Flight happens whether or not autoSwitchOnLive is on (that option
//    only governs entering In-Flight on Live). With Post-Flight disabled the stage stays where it is.
//  - A Live report that continues an active session (any sessionId) is not a start: it only cancels
//    a running off timer; it does not change the stage or a peek.
//  - Off reports are idempotent: the off timer runs from the FIRST Off of a streak.
//  - No tools enabled at all: visibleStages() is empty, stage() is PreFlight, peeks are ignored, live
//    input is still tracked, and the stage never changes until a tool is enabled again.

#include <cstdint>
#include <vector>

namespace kitstage {

enum class Stage { PreFlight, InFlight, PostFlight };
// In-Flight's tri-state live result. "Reconnecting" arrives as Live or Unknown, never as Off.
enum class Live { Unknown, Off, Live };

struct Tools {
	bool preflight = true;
	bool inflight = true;
	bool postflight = false; // arrives with Kit 1.2.0
};

struct Options {
	bool autoSwitchOnLive = true;
	int64_t offConfirmMs = 10000; // Off must be reported continuously this long to end a session
};

class Machine {
public:
	explicit Machine(Tools tools, Options options = {});

	Stage stage() const;          // what the dock shows (a manual peek wins over the underlying stage)
	bool peeking() const;         // a peek that differs from the underlying stage is active
	bool sessionActive() const;   // a live session started and has not been confirmed ended
	bool ended() const;           // a session ended and Reset has not been pressed ("Stream ended" card)
	bool resetAllowed() const;    // false while a session is active (Live, Unknown, or off-confirming)
	bool offTimerRunning() const; // Off is being confirmed (a session is active and Off was reported)
	uint32_t sessionsStarted() const; // count of real session starts (a continuing Live is not one)
	std::vector<Stage> visibleStages() const; // in order Pre, In, Post; only enabled tools

	void setTools(Tools tools);
	void setOptions(Options options);

	void onLive(Live live, uint64_t sessionId, int64_t nowMs);
	void onTick(int64_t nowMs);
	bool done(int64_t nowMs);   // Pre-Flight "Done" -> In-Flight; false (no change) when refused
	void peek(Stage stage);     // manual strip click; ignored for a stage that is not visible
	bool reset(int64_t nowMs);  // false (no change) when !resetAllowed()

private:
	bool enabled(Stage s) const;
	Stage firstVisible() const;
	void seeClock(int64_t nowMs);
	void checkOffTimer(int64_t nowMs);
	void endSession();

	Tools tools_;
	Options options_;
	Stage stage_ = Stage::PreFlight;
	bool peekSet_ = false;
	Stage peek_ = Stage::PreFlight;
	bool active_ = false;
	bool ended_ = false;
	bool offRunning_ = false;
	int64_t offSince_ = 0;
	bool haveClock_ = false;
	int64_t lastNow_ = 0;
	uint64_t sessionId_ = 0;
	uint32_t started_ = 0;
};

} // namespace kitstage
