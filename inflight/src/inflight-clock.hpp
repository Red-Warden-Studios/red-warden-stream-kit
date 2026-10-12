/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The one clock the probe uses: the tick, the signal callbacks and the Multistream worker all stamp
// times with this, so the evaluator sees a single steady timeline. Needs libobs; the pure logic
// files do not include this.
#pragma once

#include <util/platform.h>

#include <QtGlobal>

namespace inflight {

inline qint64 nowMs()
{
	return static_cast<qint64>(os_gettime_ns() / 1000000ULL);
}

} // namespace inflight
