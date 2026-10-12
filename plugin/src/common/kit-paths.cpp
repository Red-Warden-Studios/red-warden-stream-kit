/*
Red Warden Stream Kit
Copyright (C) 2026 Red Warden Studios LLC
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kit-paths.hpp"

namespace kitpaths {

namespace {
ConfigPathFn g_provider = nullptr;
FreeFn g_free = nullptr;

bool isSep(char c)
{
	return c == '/' || c == '\\';
}

// obs_module_config_path("") of the module, as an owned std::string; empty when unavailable.
std::string moduleDir()
{
	if (!g_provider)
		return {};
	char *raw = g_provider("");
	if (!raw)
		return {};
	std::string out(raw);
	if (g_free)
		g_free(raw);
	return out;
}
} // namespace

void setProvider(ConfigPathFn fn, FreeFn freeFn)
{
	g_provider = fn;
	g_free = freeFn;
}

std::string rootFromModuleDir(const std::string &moduleDir)
{
	std::string s = moduleDir;
	while (!s.empty() && isSep(s.back()))
		s.pop_back();
	size_t cut = s.size();
	while (cut > 0 && !isSep(s[cut - 1]))
		cut--; // start of the last component
	if (cut == 0)
		return {}; // no separator: no parent we can name
	s.resize(cut);
	while (!s.empty() && isSep(s.back()))
		s.pop_back();
	return s;
}

std::string join(const std::string &dir, const std::string &name)
{
	if (dir.empty())
		return name;
	if (name.empty())
		return dir;
	return isSep(dir.back()) ? dir + name : dir + "/" + name;
}

std::string pluginConfigRoot()
{
	return rootFromModuleDir(moduleDir());
}

std::string toolDir(const char *name)
{
	const std::string root = pluginConfigRoot();
	if (root.empty() || !name || !*name)
		return {};
	return root + "/" + name + "/";
}

std::string toolFile(const char *name, const char *file)
{
	const std::string dir = toolDir(name);
	if (dir.empty())
		return {};
	return (file && *file) ? dir + file : dir;
}

} // namespace kitpaths
