#include "Hook.h"
#include "Settings.h"

#ifdef SMF_ENABLED
#include "SKSEMenuFramework.h"
namespace ImGui = ImGuiMCP;
#endif

#if defined(MANUAL_SKSE_PLUGIN_METADATA) && defined(SKYRIM_SUPPORT_AE)
SKSEPluginVersion = []() {
	SKSE::PluginVersionData v;
	v.PluginVersion({ 0, 1, 0, 0 });
	v.PluginName("Skyrim Camera Disocclusion");
	v.AuthorName("anon");
	v.UsesAddressLibrary();
	v.UsesUpdatedStructs();
	v.CompatibleVersions({ SKSE::RUNTIME_SSE_1_6_1170 });

	return v;
}();
#endif

namespace
{
#ifdef SMF_ENABLED
	namespace Menu
	{
		bool IsRuntimeAvailable()
		{
			return SKSEMenuFramework::IsInstalled();
		}

		void __stdcall RenderGeneral()
		{
			ImGui::Text("Skyrim Camera Disocclusion");
			ImGui::Text("General toggles and camera tuning");

			bool modEnabled = Settings::IsModEnabled();
			if (ImGui::Checkbox("Enable disocclusion feature", &modEnabled)) {
				Settings::SetModEnabled(modEnabled);
				Hooks::OnFeatureToggleChanged(modEnabled);
				Settings::Save();
			}

			bool dumpLogEnabled = Settings::IsDumpLogEnabled();
			if (ImGui::Checkbox("Enable local dump log", &dumpLogEnabled)) {
				Settings::SetDumpLogEnabled(dumpLogEnabled);
				Settings::Save();
			}

			ImGui::Separator();
			ImGui::SliderFloat("Player height offset", &Settings::PlayerHeightOffset, 0.0f, 500.0f, "%.1f");
			ImGui::SliderFloat("Fan radius H", &Settings::FanRadiusH, 0.0f, 200.0f, "%.1f");
			ImGui::SliderFloat("Fan radius V", &Settings::FanRadiusV, 0.0f, 200.0f, "%.1f");
			ImGui::SliderFloat("Strip linger seconds", &Settings::StripLingerSeconds, 0.0f, 5.0f, "%.2f");
			ImGui::SliderFloat("Hit linger seconds", &Settings::HitLingerSeconds, 0.0f, 30.0f, "%.2f");

			if (ImGui::Button("Reload from INI")) {
				Settings::Load();
				Hooks::OnFeatureToggleChanged(Settings::IsModEnabled());
			}

			if (ImGui::Button("Save current values to INI")) {
				Settings::Save();
			}
		}

		bool Register()
		{
			if (!IsRuntimeAvailable()) {
				return false;
			}

			SKSEMenuFramework::SetSection("SkyrimCameraDisocclusion");
			SKSEMenuFramework::AddSectionItem("General", RenderGeneral);
			return true;
		}
	}
#endif
}


// Local VS Code build adaptation: use the CommonLibSSE-NG entry symbol.
SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	logs::info("Hello World!");
	Settings::Load();
	Hooks::OnFeatureToggleChanged(Settings::IsModEnabled());
	SKSE::AllocTrampoline(1 << 7);
	Hooks::Install();

#ifdef SMF_ENABLED
	if (Menu::Register()) {
		Settings::LogEvent("[SMF] menu integration enabled", true);
	} else {
		Settings::LogEvent("[SMF] menu integration disabled: runtime not found", true);
	}
#endif

	// Delay renderer-dependent hook setup until game data is ready.
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::OnDataLoaded();
		}
	});

	return true;
}
