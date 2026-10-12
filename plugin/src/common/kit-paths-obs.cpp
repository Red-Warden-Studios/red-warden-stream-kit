/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

// The OBS side of kit-paths: plugs obs_module_config_path into the injectable provider. Compiled into
// the DLL only (the unit test never links this file).

#include "kit-paths.hpp"

#include <obs-module.h>
#include <util/bmem.h>

namespace kitpaths {

namespace {
char *obsConfigPath(const char *file)
{
	return obs_module_config_path(file);
}
void obsFree(void *p)
{
	bfree(p);
}
} // namespace

void installObsProvider()
{
	setProvider(obsConfigPath, obsFree);
}

} // namespace kitpaths
