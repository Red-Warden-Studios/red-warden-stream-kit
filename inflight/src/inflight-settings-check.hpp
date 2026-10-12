/*
Red Warden In-Flight
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Pure validation of the settings (Qt Core only, no widgets, no libobs), so it can be unit-tested.
#pragma once

#include "inflight-settings.hpp"
#include "inflight-ui-logic.hpp"

#include <QStringList>

namespace inflight {

// Clamps every number to a sane range, fixes the amber/red ordering, drops holding scenes with an
// empty UUID or a repeated UUID (first one kept), clamps their limits and cuts labels to 40
// characters. Returns one human sentence per change it made; empty means nothing was changed.
// (Surrounding spaces on a label are removed silently.) `tr` is optional: without it the English
// fallback text is used.
QStringList normalizeSettings(Settings &s, const TrFn &tr = TrFn());

} // namespace inflight
