#include "Hook.h"
#include "Settings.h"

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


// Local VS Code build adaptation: use the CommonLibSSE-NG entry symbol.
SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	logs::info("Hello World!");
	Settings::Load();
	SKSE::AllocTrampoline(1 << 7);
	Hooks::Install();

	// Delay renderer-dependent hook setup until game data is ready.
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::OnDataLoaded();
		}
	});

	return true;
}
