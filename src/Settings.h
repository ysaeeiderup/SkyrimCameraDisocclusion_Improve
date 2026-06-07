#pragma once

#include <atomic>
#include <string>

namespace Settings
{
	inline float PlayerHeightOffset = 150.0f;
	inline float FanRadiusH = 20.0f;
	inline float FanRadiusV = 6.0f;
	inline float StripLingerSeconds = 0.3f;
	inline float HitLingerSeconds = 2.0f;
	inline std::atomic_bool ModEnabled{ true };
	inline std::atomic_bool DumpLogEnabled{ true };
	inline std::atomic_bool UseWeaponDrawCondition{ false };
	inline std::atomic_bool UseCombatCondition{ false };
	inline std::atomic_bool UseTargetLockCondition{ false };
	inline std::string EnabledCameraLayerSpec = "skyrim camera disocclusion.esp|800";
	inline std::string DisabledCameraLayerSpec = "Skyrim.esm|088788";

	void Load();
	void Save();

	bool IsModEnabled();
	bool IsDumpLogEnabled();
	bool IsUseWeaponDrawCondition();
	bool IsUseCombatCondition();
	bool IsUseTargetLockCondition();
	void SetModEnabled(bool a_enabled);
	void SetDumpLogEnabled(bool a_enabled);
	void SetUseWeaponDrawCondition(bool a_enabled);
	void SetUseCombatCondition(bool a_enabled);
	void SetUseTargetLockCondition(bool a_enabled);

	void LogEvent(const std::string& a_message, bool a_forceDump = false);
}
