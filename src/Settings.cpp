#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>

namespace
{
	constexpr const char* kIniPath = "Data\\SKSE\\Plugins\\skyrimcameradisocclusion.ini";

	std::string Trim(std::string a_s)
	{
		const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
		a_s.erase(a_s.begin(), std::find_if(a_s.begin(), a_s.end(), notSpace));
		a_s.erase(std::find_if(a_s.rbegin(), a_s.rend(), notSpace).base(), a_s.end());
		return a_s;
	}

	std::unordered_map<std::string, std::string> ParseIni(const char* a_path)
	{
		std::unordered_map<std::string, std::string> out;
		std::ifstream in(a_path);
		if (!in.is_open()) {
			return out;
		}
		std::string section;
		std::string line;
		while (std::getline(in, line)) {
			line = Trim(line);
			if (line.empty() || line.front() == ';' || line.front() == '#') {
				continue;
			}
			if (line.front() == '[' && line.back() == ']') {
				section = Trim(line.substr(1, line.size() - 2));
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			out[section + "." + Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
		}
		return out;
	}

	float Lookup(const std::unordered_map<std::string, std::string>& a_map,
	             const std::string& a_sectionDotKey,
	             float a_default, float a_min, float a_max)
	{
		const auto it = a_map.find(a_sectionDotKey);
		if (it == a_map.end()) {
			return a_default;
		}
		char* end = nullptr;
		const float v = std::strtof(it->second.c_str(), &end);
		if (end == it->second.c_str()) {
			return a_default;
		}
		return std::clamp(v, a_min, a_max);
	}
}

namespace Settings
{
	void Load()
	{
		const auto ini = ParseIni(kIniPath);

		PlayerHeightOffset = Lookup(ini, "Camera.PlayerHeightOffset", 150.0f,    0.0f,  500.0f);
		FanRadiusH         = Lookup(ini, "RayFan.FanRadiusH",          20.0f,    0.0f,  200.0f);
		FanRadiusV         = Lookup(ini, "RayFan.FanRadiusV",           6.0f,    0.0f,  200.0f);
		StripLingerSeconds = Lookup(ini, "Strip.StripLingerSeconds",    0.3f,    0.0f,    5.0f);
		HitLingerSeconds   = Lookup(ini, "Strip.HitLingerSeconds",      2.0f,    0.0f,   30.0f);

		logs::info("settings: heightOffset={:.1f} fan=({:.1f},{:.1f}) lingers=(strip={:.2f}, hit={:.2f})",
			PlayerHeightOffset, FanRadiusH, FanRadiusV,
			StripLingerSeconds, HitLingerSeconds);
	}
}
