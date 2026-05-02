#include "Hook.h"
#include "Settings.h"

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	logs::info("Hello World!");
	Settings::Load();
	SKSE::AllocTrampoline(1 << 7);
	Hooks::Install();

	// Renderer hooks deferred until the engine has finished loading data —
	// kDataLoaded is the first reliable point at which the D3D11 device and
	// context exist for vtable patching.
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::OnDataLoaded();
		}
	});

	return true;
}
