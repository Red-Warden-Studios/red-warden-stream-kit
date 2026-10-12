/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The pure half of the probe: reading Multistream's destination states and combining them with OBS's own stream
// state into a LiveState. Qt Core only, no libobs, no clock: every time is an argument.
#pragma once

#include "inflight-eval.hpp"

#include <QStringList>
#include <QtGlobal>

namespace inflight {

enum class MsStatus { Pending, NotFound, Ok, Error };

struct MsReply {
	MsStatus status = MsStatus::Pending;
	bool anyLive = false;
	qint64 atMs = 0; // when the reply arrived, on the shared clock
};

// A Multistream answer older than this no longer counts. (Multistream is polled in-process on the UI thread now, so an
// answer is always fresh when it is used; the rule stays as the safety net it always was.)
constexpr qint64 kMsFreshMs = 10000;

// True when any destination's state string is "live" or "reconnecting" (case-insensitive). "enabled" does not mean
// live, and every other state (connecting, ready, off, error, waiting, paused, stopping, empty) is not live.
bool anyDestinationLive(const QStringList &states);

// Main stream active -> Live. Otherwise: no Multistream -> Off; no usable answer -> Unknown;
// a fresh Ok answer -> Live or Off by anyLive; a stale (over kMsFreshMs) or future-dated answer -> Unknown.
LiveState combineLive(bool mainStreamActive, const MsReply &ms, qint64 nowMs);

// libobs's OBS_OUTPUT_SUCCESS is 0 (defined here so this file needs no libobs).
constexpr int kOutputSuccess = 0;
StreamEventKind stopKindForCode(int code);

} // namespace inflight
