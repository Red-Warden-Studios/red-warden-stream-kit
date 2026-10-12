/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// A tiny pure state machine for "a timed run that can be cancelled". No Qt, no OBS.
//
// Every begin() hands out a new token. cancel() invalidates whatever run is current. A finisher
// (a timer callback, a queued event) may publish its result only if complete(token) returns true:
// a finisher that belongs to a cancelled or superseded run always gets false, even if it fires late.
//
// One copy for the whole Stream Kit. Pre-Flight and In-Flight each used to carry their own copy in
// their own namespace; the class now lives in rwskit and both namespaces re-export it, so existing
// code (`preflight::RunGeneration`, `inflight::RunGeneration`) is unchanged and no symbol is defined
// twice (the class is header-only and inline).
#pragma once

#include <cstdint>

namespace rwskit {

class RunGeneration {
public:
	// Start a new run (supersedes any running one). Returns the token the run's finisher must carry.
	std::uint64_t begin()
	{
		running_ = true;
		return ++gen_;
	}
	// Abort the current run, if any. Returns true when a run was actually running.
	bool cancel()
	{
		const bool was = running_;
		running_ = false;
		++gen_; // any finisher still holding the old token is now stale
		return was;
	}
	// The finisher for `token` asks to publish. True exactly once, and only for the live run.
	bool complete(std::uint64_t token)
	{
		if (!running_ || token != gen_)
			return false;
		running_ = false;
		return true;
	}
	bool running() const { return running_; }

private:
	std::uint64_t gen_ = 0;
	bool running_ = false;
};

} // namespace rwskit

namespace preflight {
using rwskit::RunGeneration;
}
namespace inflight {
using rwskit::RunGeneration;
}
