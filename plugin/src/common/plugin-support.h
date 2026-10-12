/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

void obs_log(int log_level, const char *format, ...);
// Same as obs_log, but the line starts with "[<prefix>] " instead of "[<PLUGIN_NAME>] ".
void rws_log(const char *prefix, int log_level, const char *format, ...);
extern void blogva(int log_level, const char *format, va_list args);

#ifdef __cplusplus
}
#endif

// One DLL carries three tools. Each tool's sources are compiled with RWS_LOG_PREFIX set (see
// plugin/CMakeLists.txt) so their obs_log lines keep "[rws-multistream]" / "[rws-preflight]" /
// "[rws-inflight]", which the test harnesses parse. Code without the define (plugin-main.cpp,
// common/) logs as "[rws-stream-kit]".
#ifdef RWS_LOG_PREFIX
#define obs_log(level, ...) rws_log(RWS_LOG_PREFIX, (level), __VA_ARGS__)
#endif
