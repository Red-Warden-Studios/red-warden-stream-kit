/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

// Where each tool keeps its files. Before the merge every tool was its own OBS module, so
// obs_module_config_path("x") meant <OBS config>/plugin_config/<tool module name>/x. The Stream Kit is
// one module (rws-stream-kit), so that call would now point every tool at the same folder and
// orphan the settings, reports and hotkeys users already have. These helpers keep the old folders:
//
//   pluginConfigRoot()          <OBS config>/plugin_config            (no trailing slash)
//   toolDir("rws-multistream")  <OBS config>/plugin_config/rws-multistream/   (trailing slash, as OBS
//                                                                              returned for "")
//   toolFile(name, "x.json")    <OBS config>/plugin_config/<name>/x.json
//
// No Qt and no OBS in this file or kit-paths.cpp, so the logic is unit-tested (tests/kit-paths-test.cpp).
// The single OBS call is injected: kit-paths-obs.cpp (DLL only) installs obs_module_config_path.
// Every function is null-safe: with no provider, or a provider that returns null, the answer is an
// empty string, and callers treat that as "no config folder" exactly as they treated a null path.

#include <string>

namespace kitpaths {

// The pre-merge module names, i.e. the folder names under plugin_config/. Never change these: they
// are where users' existing files are.
constexpr const char *kMultistream = "rws-multistream";
constexpr const char *kPreflight = "rws-preflight";
constexpr const char *kInflight = "rws-inflight";

// obs_module_config_path-compatible: returns a bmem-allocated string (or null) that the caller frees.
using ConfigPathFn = char *(*)(const char *file);
// Frees what the provider returned (bfree for OBS).
using FreeFn = void (*)(void *);

// nullptr clears the provider. Called once at module load; tests call it with fakes.
void setProvider(ConfigPathFn fn, FreeFn freeFn);

// ---- pure string logic (also used directly by the tests) ----
// "<root>/<module>/" or "<root>/<module>" -> "<root>". Empty in, or no separator, gives "".
std::string rootFromModuleDir(const std::string &moduleDir);
// Joins without doubling separators: join("a/b", "c") == "a/b/c"; join("a/b/", "c") == "a/b/c".
std::string join(const std::string &dir, const std::string &name);

// ---- via the provider ----
// Wires the real obs_module_config_path in. Defined in kit-paths-obs.cpp (DLL only).
void installObsProvider();

std::string pluginConfigRoot();
std::string toolDir(const char *name);
std::string toolFile(const char *name, const char *file);

} // namespace kitpaths
