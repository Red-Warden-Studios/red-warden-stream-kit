// Checks the merged locale file: it must parse, hold keys, and contain no key twice. The three tools'
// files were merged into one; two tools defining the same key would silently let the later line win.
// Usage: rws-kit-locale-test [path-to-en-US.ini]   (default: the file in this source tree)
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

#ifndef RWS_LOCALE_FILE
#define RWS_LOCALE_FILE "data/locale/en-US.ini"
#endif

int main(int argc, char **argv)
{
	const std::string path = argc > 1 ? argv[1] : RWS_LOCALE_FILE;
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		std::printf("FAIL: cannot read %s\n", path.c_str());
		return 1;
	}
	std::map<std::string, int> firstLine;
	int failures = 0, lineNo = 0, keys = 0;
	std::string line;
	while (std::getline(in, line)) {
		lineNo++;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos || eq == 0) {
			std::printf("FAIL: line %d is not key=value: %s\n", lineNo, line.c_str());
			failures++;
			continue;
		}
		std::string key = line.substr(0, eq);
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
			key.pop_back();
		keys++;
		auto it = firstLine.find(key);
		if (it != firstLine.end()) {
			std::printf("FAIL: duplicate key '%s' (lines %d and %d)\n", key.c_str(), it->second, lineNo);
			failures++;
		} else {
			firstLine[key] = lineNo;
		}
	}
	if (keys == 0) {
		std::printf("FAIL: no keys found in %s\n", path.c_str());
		failures++;
	}
	std::printf("%s: %d keys, %d unique, %d failure%s (%s)\n", failures ? "FAILED" : "ALL PASSED", keys,
		    (int)firstLine.size(), failures, failures == 1 ? "" : "s", path.c_str());
	return failures ? 1 : 0;
}
