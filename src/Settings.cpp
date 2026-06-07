#include "Settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <Windows.h>

namespace
{
	constexpr const char* kIniPath = "Data\\SKSE\\Plugins\\skyrimcameradisocclusion.ini";
	constexpr const char* kDumpLogPathPrimary = "Data\\SKSE\\Plugins\\skyrimcameradisocclusion_dump.log";
	constexpr const char* kDumpLogPathFallback = "skyrimcameradisocclusion_dump.log";
	std::mutex g_dumpLogMutex;

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

	bool ParseBool(const std::string& a_raw, bool a_default)
	{
		std::string v;
		v.reserve(a_raw.size());
		for (const char c : a_raw) {
			v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}

		if (v == "1" || v == "true" || v == "on" || v == "yes") {
			return true;
		}
		if (v == "0" || v == "false" || v == "off" || v == "no") {
			return false;
		}
		return a_default;
	}

	bool LookupBool(const std::unordered_map<std::string, std::string>& a_map,
	                const std::string& a_sectionDotKey,
	                bool a_default)
	{
		const auto it = a_map.find(a_sectionDotKey);
		if (it == a_map.end()) {
			return a_default;
		}
		return ParseBool(it->second, a_default);
	}

	std::string LookupString(const std::unordered_map<std::string, std::string>& a_map,
	                         const std::string& a_sectionDotKey,
	                         const std::string& a_default)
	{
		const auto it = a_map.find(a_sectionDotKey);
		if (it == a_map.end()) {
			return a_default;
		}
		const auto trimmed = Trim(it->second);
		return trimmed.empty() ? a_default : trimmed;
	}

	void AppendDumpLine(const std::string& a_message)
	{
		std::lock_guard lock(g_dumpLogMutex);

		SYSTEMTIME t{};
		GetLocalTime(&t);

		auto writeLine = [&](const char* a_path) {
			std::FILE* file = nullptr;
			if (fopen_s(&file, a_path, "a") != 0 || !file) {
				return false;
			}

			std::fprintf(
				file,
				"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\n",
				t.wYear,
				t.wMonth,
				t.wDay,
				t.wHour,
				t.wMinute,
				t.wSecond,
				t.wMilliseconds,
				a_message.c_str());
			std::fclose(file);
			return true;
		};

		if (!writeLine(kDumpLogPathPrimary)) {
			writeLine(kDumpLogPathFallback);
		}
	}

	void ResetDumpLogForSession()
	{
		auto truncate = [](const char* a_path) {
			std::FILE* file = nullptr;
			if (fopen_s(&file, a_path, "w") != 0 || !file) {
				return false;
			}
			std::fclose(file);
			return true;
		};

		if (!truncate(kDumpLogPathPrimary)) {
			truncate(kDumpLogPathFallback);
		}
	}

	void SaveBool(const char* a_section, const char* a_key, bool a_value)
	{
		WritePrivateProfileStringA(a_section, a_key, a_value ? "1" : "0", kIniPath);
	}

	void SaveFloat(const char* a_section, const char* a_key, float a_value)
	{
		char buffer[64]{};
		std::snprintf(buffer, sizeof(buffer), "%.4f", a_value);
		WritePrivateProfileStringA(a_section, a_key, buffer, kIniPath);
	}

	void SaveString(const char* a_section, const char* a_key, const std::string& a_value)
	{
		WritePrivateProfileStringA(a_section, a_key, a_value.c_str(), kIniPath);
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
		ModEnabled.store(LookupBool(ini, "General.bEnable", true), std::memory_order_release);
		DumpLogEnabled.store(LookupBool(ini, "General.bDumpLog", true), std::memory_order_release);
		EnabledCameraLayerSpec = LookupString(ini, "General.EnabledCameraLayerSpec", "skyrim camera disocclusion.esp|800");
		DisabledCameraLayerSpec = LookupString(ini, "General.DisabledCameraLayerSpec", "Skyrim.esm|088788");

		if (IsDumpLogEnabled()) {
			ResetDumpLogForSession();
		}

		logs::info("settings: enabled={} dumpLog={} heightOffset={:.1f} fan=({:.1f},{:.1f}) lingers=(strip={:.2f}, hit={:.2f})",
			IsModEnabled(), IsDumpLogEnabled(),
			PlayerHeightOffset, FanRadiusH, FanRadiusV,
			StripLingerSeconds, HitLingerSeconds);
		logs::info("settings: layerSpec enabled='{}' disabled='{}'", EnabledCameraLayerSpec, DisabledCameraLayerSpec);

		LogEvent("[settings] loaded", true);
	}

	void Save()
	{
		SaveBool("General", "bEnable", IsModEnabled());
		SaveBool("General", "bDumpLog", IsDumpLogEnabled());
		SaveString("General", "EnabledCameraLayerSpec", EnabledCameraLayerSpec);
		SaveString("General", "DisabledCameraLayerSpec", DisabledCameraLayerSpec);

		SaveFloat("Camera", "PlayerHeightOffset", PlayerHeightOffset);
		SaveFloat("RayFan", "FanRadiusH", FanRadiusH);
		SaveFloat("RayFan", "FanRadiusV", FanRadiusV);
		SaveFloat("Strip", "StripLingerSeconds", StripLingerSeconds);
		SaveFloat("Strip", "HitLingerSeconds", HitLingerSeconds);

		LogEvent("[settings] saved", true);
	}

	bool IsModEnabled()
	{
		return ModEnabled.load(std::memory_order_acquire);
	}

	bool IsDumpLogEnabled()
	{
		return DumpLogEnabled.load(std::memory_order_acquire);
	}

	void SetModEnabled(bool a_enabled)
	{
		ModEnabled.store(a_enabled, std::memory_order_release);
		LogEvent(a_enabled ? "[toggle] mod enabled" : "[toggle] mod disabled", true);
	}

	void SetDumpLogEnabled(bool a_enabled)
	{
		DumpLogEnabled.store(a_enabled, std::memory_order_release);
		if (a_enabled) {
			ResetDumpLogForSession();
		}
		LogEvent(a_enabled ? "[toggle] dump log enabled" : "[toggle] dump log disabled", true);
	}

	void LogEvent(const std::string& a_message, bool a_forceDump)
	{
		logs::info("{}", a_message);
		if (a_forceDump || IsDumpLogEnabled()) {
			AppendDumpLine(a_message);
		}
	}
}
