#include "Hook.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/P/PlayerCamera.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/B/bhkWorld.h"
#include "RE/B/bhkRigidBody.h"
#include "RE/B/bhkCollisionFilter.h"
#include "RE/B/bhkPickData.h"
#include "RE/H/hkpWorldRayCastInput.h"
#include "RE/H/hkpWorldRayCastOutput.h"
#include "RE/H/hkpRigidBody.h"
#include "RE/T/TESHavokUtilities.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/N/NiPoint3.h"
#include "RE/N/NiAVObject.h"
#include "RE/N/NiNode.h"
#include "RE/B/BSShader.h"
#include "RE/B/BSRenderPass.h"
#include "RE/B/BSPortalGraph.h"
#include "RE/B/BSMultiBoundNode.h"
#include "RE/B/BSMultiBoundRoom.h"
#include "RE/B/BSOcclusionPlane.h"
#include "RE/B/BSOcclusionBox.h"
#include "RE/B/BGSCollisionLayer.h"
#include "RE/S/Sky.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESFile.h"
#include "RE/A/ActorState.h"
#include "Settings.h"
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "RE/C/CFilter.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <Windows.h>

namespace Hooks
{
	struct HitEntry
	{
		// Bumps the NiRefObject refcount so the engine can't free the mesh while we
		// have it in the map. The raw key pointer is never dereferenced ??only used
		// for hashing in Update and as a comparison target in the render-pass thunk.
		RE::NiPointer<RE::NiAVObject> keepalive;
		float                         secondsSinceSeen;
		float                         distFromCamera;
	};
	// Owned by HookPlayerCharacter::Update only. We assume Update is called serially
	// on a single thread (Skyrim's main actor-update loop is single-threaded
	// per frame), so this map is mutated without locking. No other code reads from
	// it directly ??they read the atomic snapshots below.
	static std::unordered_map<RE::NiAVObject*, HitEntry> g_hitObjects;

	// Atomic snapshots published by Update each frame, consumed by the BSLightingShader
	// render-pass thunk and the ClearRTV hook. We don't know which thread(s) call
	// those ??could be the same thread as Update, could be a separate render-submit
	// thread ??so all access goes through atomic load/store with acquire/release
	// ordering. The shared_ptr publish pattern means consumers see a consistent
	// snapshot even if Update rebuilds mid-load; old snapshots free themselves once
	// no consumer is holding them.
	using HitSet = std::unordered_set<RE::NiAVObject*>;
	static std::atomic<std::shared_ptr<const HitSet>> g_publishedHits;
	static std::atomic<float>                         g_clipDistance{ 0.0f };

	// Published once per StripGraph call on interior cells: the set of rooms in the
	// player's portal graph, plus a keepalive on the graph itself so the engine
	// can't free it under us. The QPointWithin thunk loads this lock-free and
	// returns true for any of these rooms while see-through is engaged, making
	// the engine's visibility walk seed from every room in the cell.
	struct ForceVisRecord
	{
		RE::NiPointer<RE::BSPortalGraph>          graphKeepalive;
		std::unordered_set<RE::BSMultiBoundRoom*> rooms;
	};
	static std::atomic<std::shared_ptr<const ForceVisRecord>> g_forceVisRecord;

	// Feature gates. Default-true; kept as toggles so each feature can be disabled
	// at compile time without ripping out the implementation.
	//   kEnableStripper   ??install OcclusionStripper event sinks + run the
	//                       per-tick refresh that drives the position-displace
	//                       outdoor strip (writes shape translations to a
	//                       far-away point so they occlude nothing, restores
	//                       on toggle-off).
	//   kInstallQPoint    ??install BSMultiBoundRoom::QPointWithin vtable hook.
	//                       While see-through is engaged in an interior, the hook
	//                       returns true for every room in the player's cell graph,
	//                       making the engine's visibility walk seed from every
	//                       room and the whole cell render regardless of where
	//                       the camera floats.
	//   kPublishIndoor    ??populate g_forceVisRecord during interior StripGraph
	//                       so the QPointWithin thunk can identify rooms.
	//   kFindPlayerRoom   ??walk the cell graph in Update each tick to cache the
	//                       player's BSMultiBoundRoom (used by the QPointWithin
	//                       redirect to pick the seed room for visibility walking).
	constexpr bool kEnableStripper  = true;
	constexpr bool kInstallQPoint   = true;
	constexpr bool kPublishIndoor   = true;
	constexpr bool kFindPlayerRoom  = true;

	// The room the engine's own QPointWithin says contains the player, refreshed
	// every frame in HookPlayerCharacter::Update. Used as a binary "is the player
	// in some interior room?" signal by the QPointWithin thunk ??when non-null and
	// see-through is engaged, the thunk returns true for every room in the player's
	// cell graph, making the engine's visibility walk seed from every room and the
	// whole cell render regardless of where the camera floats.
	static std::atomic<RE::BSMultiBoundRoom*> g_playerRoom{ nullptr };
	constexpr std::uint32_t kDefaultCameraLayerIndex = 39;
	static std::atomic<RE::COL_LAYER> g_enabledCameraLayer{ static_cast<RE::COL_LAYER>(kDefaultCameraLayerIndex) };
	static std::atomic<RE::COL_LAYER> g_disabledCameraLayer{ static_cast<RE::COL_LAYER>(kDefaultCameraLayerIndex) };
	// CNAM-equivalent runtime masks for the selected collision layer profile.
	// The toggle now switches these profiles; core disocclusion logic stays active.
	static std::atomic<std::uint64_t> g_enabledCameraLayerMask{ 0 };
	static std::atomic<std::uint64_t> g_disabledCameraLayerMask{ 0 };
	static std::atomic<bool> g_useEnabledCameraProfile{ false };
	static std::atomic<std::uint32_t> g_updateThreadId{ 0 };
	static std::atomic<bool> g_hasDeferredProfileApply{ false };
	static std::atomic<bool> g_deferredUseEnabledProfile{ false };
	static std::atomic<bool> g_deferredForceApply{ false };
	// Debounce profile oscillation around condition boundaries (weapon/combat/lock transitions).
	// Trade-off: a profile flip can be delayed by up to this window to reduce rapid back-and-forth applies.
	constexpr std::uint32_t kProfileSwitchDebounceMs = 150;
	static std::atomic<bool> g_debounceTargetUseEnabledProfile{ false };
	static std::atomic<std::uint32_t> g_debounceTargetSinceTick{ 0 };
	static std::atomic<bool> g_debounceActive{ false };

	namespace
	{
		void ApplyCameraCollisionLayer(RE::COL_LAYER a_layer, std::uint64_t a_mask, const char* a_reason);

		std::uint32_t GetNowTickMs()
		{
			return static_cast<std::uint32_t>(::GetTickCount64() & 0xFFFFFFFFULL);
		}

		bool HasElapsedMs(std::uint32_t a_startTick, std::uint32_t a_intervalMs)
		{
			const std::uint32_t now = GetNowTickMs();
			return static_cast<std::uint32_t>(now - a_startTick) >= a_intervalMs;
		}

		void BeginProfileDebounce(bool a_targetUseEnabledProfile, const char* a_reason)
		{
			g_debounceTargetUseEnabledProfile.store(a_targetUseEnabledProfile, std::memory_order_release);
			g_debounceTargetSinceTick.store(GetNowTickMs(), std::memory_order_release);
			g_debounceActive.store(true, std::memory_order_release);
			Settings::LogEvent(std::string("[debounce] start (") + a_reason + ") target=" +
				(a_targetUseEnabledProfile ? "enabled" : "disabled") +
				" windowMs=" + std::to_string(kProfileSwitchDebounceMs), true);
		}

		bool ShouldApplyProfileWithDebounce(bool a_desiredUseEnabledProfile, bool a_forceApply, const char* a_reason)
		{
			if (a_forceApply) {
				// Explicit operations (menu toggles, startup sync) should feel immediate and deterministic.
				// We bypass debounce here so user intent is applied right away.
				g_debounceActive.store(false, std::memory_order_release);
				return true;
			}

			const bool currentUseEnabledProfile = g_useEnabledCameraProfile.load(std::memory_order_acquire);
			if (a_desiredUseEnabledProfile == currentUseEnabledProfile) {
				g_debounceActive.store(false, std::memory_order_release);
				return false;
			}

			const bool debounceActive = g_debounceActive.load(std::memory_order_acquire);
			if (!debounceActive) {
				BeginProfileDebounce(a_desiredUseEnabledProfile, a_reason);
				return false;
			}

			const bool pendingTarget = g_debounceTargetUseEnabledProfile.load(std::memory_order_acquire);
			if (pendingTarget != a_desiredUseEnabledProfile) {
				// Direction changed during debounce window; restart the timer for the new target.
				BeginProfileDebounce(a_desiredUseEnabledProfile, a_reason);
				return false;
			}

			const std::uint32_t sinceTick = g_debounceTargetSinceTick.load(std::memory_order_acquire);
			if (!HasElapsedMs(sinceTick, kProfileSwitchDebounceMs)) {
				return false;
			}

			g_debounceActive.store(false, std::memory_order_release);
			Settings::LogEvent(std::string("[debounce] commit (") + a_reason + ") target=" +
				(a_desiredUseEnabledProfile ? "enabled" : "disabled"), true);
			return true;
		}

		void MarkCurrentThreadAsUpdateThread()
		{
			const auto tid = static_cast<std::uint32_t>(::GetCurrentThreadId());
			std::uint32_t expected = 0;
			if (g_updateThreadId.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
				Settings::LogEvent("[thread] update thread captured: tid=" + std::to_string(tid), true);
			}
		}

		bool IsUpdateThread()
		{
			const auto updateTid = g_updateThreadId.load(std::memory_order_acquire);
			if (updateTid == 0) {
				return false;
			}
			const auto currentTid = static_cast<std::uint32_t>(::GetCurrentThreadId());
			return updateTid == currentTid;
		}

		struct ConditionState
		{
			bool masterEnabled{ false };
			bool useWeaponCondition{ false };
			bool useCombatCondition{ false };
			bool useTargetLockCondition{ false };
			bool weaponDrawn{ false };
			bool inCombat{ false };
			bool targetLocked{ false };
			bool selectedEnabledProfile{ false };
		};

		bool operator==(const ConditionState& a_lhs, const ConditionState& a_rhs)
		{
			return a_lhs.masterEnabled == a_rhs.masterEnabled &&
			       a_lhs.useWeaponCondition == a_rhs.useWeaponCondition &&
			       a_lhs.useCombatCondition == a_rhs.useCombatCondition &&
			       a_lhs.useTargetLockCondition == a_rhs.useTargetLockCondition &&
			       a_lhs.weaponDrawn == a_rhs.weaponDrawn &&
			       a_lhs.inCombat == a_rhs.inCombat &&
			       a_lhs.targetLocked == a_rhs.targetLocked &&
			       a_lhs.selectedEnabledProfile == a_rhs.selectedEnabledProfile;
		}

		bool operator!=(const ConditionState& a_lhs, const ConditionState& a_rhs)
		{
			return !(a_lhs == a_rhs);
		}

		void LogConditionState(const ConditionState& a_state, const char* a_reason)
		{
			Settings::LogEvent(std::string("[condition] ") + a_reason +
				" master=" + (a_state.masterEnabled ? "1" : "0") +
				" toggles(wpn=" + (a_state.useWeaponCondition ? "1" : "0") +
				",combat=" + (a_state.useCombatCondition ? "1" : "0") +
				",lock=" + (a_state.useTargetLockCondition ? "1" : "0") +
				") states(wpn=" + (a_state.weaponDrawn ? "1" : "0") +
				",combat=" + (a_state.inCombat ? "1" : "0") +
				",lock=" + (a_state.targetLocked ? "1" : "0") +
				") profile=" + (a_state.selectedEnabledProfile ? "enabled" : "disabled"),
				true);
		}

		bool QueryTargetLockState(RE::Actor* a_player)
		{
			// Use graph variable only. Calling TDM API during startup can crash in some load orders.
			// TDM updates this variable on the player animation graph while lock-on is active.
			static bool s_loggedMissingGraphVar = false;
			bool graphVarLocked = false;
			if (a_player && a_player->GetGraphVariableBool("TDM_TargetLock", graphVarLocked)) {
				return graphVarLocked;
			}
			if (!s_loggedMissingGraphVar) {
				Settings::LogEvent("[condition] TDM target-lock graph var unavailable, defaulting to unlocked", true);
				s_loggedMissingGraphVar = true;
			}
			return false;
		}

		ConditionState EvaluateConditions(RE::Actor* a_player)
		{
			ConditionState state;
			state.masterEnabled = Settings::IsModEnabled();
			state.useWeaponCondition = Settings::IsUseWeaponDrawCondition();
			state.useCombatCondition = Settings::IsUseCombatCondition();
			state.useTargetLockCondition = Settings::IsUseTargetLockCondition();

			if (a_player) {
				state.weaponDrawn = a_player->IsWeaponDrawn();
				state.inCombat = a_player->IsInCombat();
				state.targetLocked = QueryTargetLockState(a_player);
			}

			// Master OFF is an explicit fallback mode for compatibility:
			// always pick Disabled profile (typically Skyrim.esm|088788).
			if (!state.masterEnabled) {
				state.selectedEnabledProfile = false;
				return state;
			}

			int enabledConditionCount = 0;
			bool passAllEnabledConditions = true;
			if (state.useWeaponCondition) {
				++enabledConditionCount;
				passAllEnabledConditions = passAllEnabledConditions && state.weaponDrawn;
			}
			if (state.useCombatCondition) {
				++enabledConditionCount;
				passAllEnabledConditions = passAllEnabledConditions && state.inCombat;
			}
			if (state.useTargetLockCondition) {
				++enabledConditionCount;
				passAllEnabledConditions = passAllEnabledConditions && state.targetLocked;
			}

			// No condition enabled: keep legacy behavior of using Enabled profile when master is ON.
			// Condition toggles are AND-combined: all enabled conditions must be true.
			state.selectedEnabledProfile = enabledConditionCount == 0 ? true : passAllEnabledConditions;
			return state;
		}

		void ApplySelectedProfile(bool a_useEnabledProfile, const char* a_reason)
		{
			// Profile switch writes both layer index and CNAM-equivalent runtime bitmask.
			// This is required because Enabled/Disabled specs may share layerIdx but differ by mask.
			const auto targetLayer = a_useEnabledProfile
				? g_enabledCameraLayer.load(std::memory_order_acquire)
				: g_disabledCameraLayer.load(std::memory_order_acquire);
			const auto targetMask = a_useEnabledProfile
				? g_enabledCameraLayerMask.load(std::memory_order_acquire)
				: g_disabledCameraLayerMask.load(std::memory_order_acquire);
			ApplyCameraCollisionLayer(targetLayer, targetMask, a_reason);
			g_useEnabledCameraProfile.store(a_useEnabledProfile, std::memory_order_release);
		}

		void EvaluateAndApplyProfile(RE::Actor* a_player, const char* a_reason, bool a_forceApply)
		{
			static ConditionState s_lastLogged{};
			static bool s_hasLoggedState = false;

			const auto state = EvaluateConditions(a_player);
			if (!s_hasLoggedState || state != s_lastLogged) {
				LogConditionState(state, a_reason);
				s_lastLogged = state;
				s_hasLoggedState = true;
			}

			if (ShouldApplyProfileWithDebounce(state.selectedEnabledProfile, a_forceApply, a_reason)) {
				if (!IsUpdateThread()) {
					// Camera/Havok writes are deferred to update thread for safety.
					// Non-update threads only publish desired state atomically.
					g_deferredUseEnabledProfile.store(state.selectedEnabledProfile, std::memory_order_release);
					g_deferredForceApply.store(a_forceApply, std::memory_order_release);
					g_hasDeferredProfileApply.store(true, std::memory_order_release);
					Settings::LogEvent(std::string("[thread] deferred profile apply from non-update thread (") + a_reason + ")", true);
					return;
				}

				ApplySelectedProfile(state.selectedEnabledProfile, a_reason);
			}
		}

		void DrainDeferredProfileApply()
		{
			if (!g_hasDeferredProfileApply.exchange(false, std::memory_order_acq_rel)) {
				return;
			}

			const bool useEnabledProfile = g_deferredUseEnabledProfile.load(std::memory_order_acquire);
			const bool forceApply = g_deferredForceApply.exchange(false, std::memory_order_acq_rel);
			const bool currentlyEnabled = g_useEnabledCameraProfile.load(std::memory_order_acquire);
			if (forceApply || useEnabledProfile != currentlyEnabled) {
				// Deferred commit executes on update thread.
				ApplySelectedProfile(useEnabledProfile, "deferred-apply");
			}

			g_debounceActive.store(false, std::memory_order_release);
		}

		std::string Hex8(std::uint32_t a_value)
		{
			char buffer[16]{};
			std::snprintf(buffer, sizeof(buffer), "%08X", a_value);
			return buffer;
		}

		std::string Hex16(std::uint64_t a_value)
		{
			char buffer[24]{};
			std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(a_value));
			return buffer;
		}

		std::string TrimCopy(std::string a_s)
		{
			const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
			a_s.erase(a_s.begin(), std::find_if(a_s.begin(), a_s.end(), notSpace));
			a_s.erase(std::find_if(a_s.rbegin(), a_s.rend(), notSpace).base(), a_s.end());
			return a_s;
		}

		bool TryParseHex(std::string a_text, std::uint32_t& a_out)
		{
			a_text = TrimCopy(std::move(a_text));
			if (a_text.empty()) {
				return false;
			}
			if (a_text.rfind("0x", 0) == 0 || a_text.rfind("0X", 0) == 0) {
				a_text.erase(0, 2);
			}
			char* end = nullptr;
			const unsigned long parsed = std::strtoul(a_text.c_str(), &end, 16);
			if (end == a_text.c_str() || (end && *end != '\0') || parsed > 0xFFFFFFFFUL) {
				return false;
			}
			a_out = static_cast<std::uint32_t>(parsed);
			return true;
		}

		bool ResolveLayerSpec(const std::string& a_spec,
		                      RE::COL_LAYER& a_outLayer,
		                      std::uint32_t& a_outResolvedFormID,
		                      std::uint64_t& a_outMask,
		                      std::string& a_outMessage)
		{
			// Spec format: "PluginName|FormID" (hex). Resolve to BGSCollisionLayer,
			// then derive both collisionIdx and a 64-bit collides-with mask.
			const auto split = a_spec.find('|');
			if (split == std::string::npos) {
				a_outMessage = "bad spec format (expected ModName|FormID): " + a_spec;
				return false;
			}

			const std::string modName = TrimCopy(a_spec.substr(0, split));
			std::uint32_t localID = 0;
			if (modName.empty() || !TryParseHex(a_spec.substr(split + 1), localID)) {
				a_outMessage = "bad spec token(s): " + a_spec;
				return false;
			}

			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) {
				a_outMessage = "TESDataHandler unavailable";
				return false;
			}

			auto* layerForm = dataHandler->LookupForm<RE::BGSCollisionLayer>(localID, modName);

			if (!layerForm) {
				// Accept either local IDs or already-expanded IDs (FE/full plugin forms)
				// so user config can follow common modding notation.
				if (const auto* modFile = dataHandler->LookupModByName(modName)) {
					std::uint32_t normalizedLocalID = localID;
					bool hasNormalizedID = false;

					if (modFile->IsLight()) {
						if ((localID >> 12) == modFile->GetPartialIndex()) {
							normalizedLocalID = localID & 0x00000FFF;
							hasNormalizedID = true;
						}
					} else {
						if ((localID >> 24) == modFile->GetCompileIndex()) {
							normalizedLocalID = localID & 0x00FFFFFF;
							hasNormalizedID = true;
						}
					}

					if (hasNormalizedID && normalizedLocalID != localID) {
						layerForm = dataHandler->LookupForm<RE::BGSCollisionLayer>(normalizedLocalID, modName);
					}

					if (!layerForm) {
						const std::uint32_t localOnlyID = modFile->IsLight()
							? (localID & 0x00000FFF)
							: (localID & 0x00FFFFFF);
						if (localOnlyID != 0 && localOnlyID != localID) {
							layerForm = dataHandler->LookupForm<RE::BGSCollisionLayer>(localOnlyID, modName);
						}
					}
				}
			}

			if (!layerForm) {
				a_outMessage = "layer form not found: " + a_spec;
				return false;
			}

			const std::uint32_t idx = layerForm->collisionIdx;
			if (idx > 0x7F) {
				a_outMessage = "collisionIdx out of range (must be <=127): " + std::to_string(idx);
				return false;
			}

			std::uint64_t mask = 0;
			// Runtime filter table is 64-bit; only indices [0,63] are represented here.
			for (const auto* collidesWithLayer : layerForm->collidesWith) {
				if (!collidesWithLayer) {
					continue;
				}
				const std::uint32_t otherIdx = collidesWithLayer->collisionIdx;
				if (otherIdx < 64) {
					mask |= (std::uint64_t{ 1 } << otherIdx);
				}
			}

			a_outLayer = static_cast<RE::COL_LAYER>(idx);
			a_outResolvedFormID = layerForm->GetFormID();
			a_outMask = mask;
			a_outMessage = "ok";
			return true;
		}

		void RefreshConfiguredCameraLayers()
		{
			// Re-read INI specs and publish resolved profile data used by Update()
			// and ApplyCameraCollisionLayer() when toggling in menu.
			//
			// Upstream workflow note:
			// - Disabled profile can stay vanilla (e.g. Skyrim.esm|088788) unchanged.
			// - Mod profile is expected to be a new BGSCollisionLayer from mod plugin
			//   (e.g. skyrim camera disocclusion.esp|800).
			// Runtime then switches between these two profiles by conditions.
			RE::COL_LAYER enabledLayer = static_cast<RE::COL_LAYER>(kDefaultCameraLayerIndex);
			RE::COL_LAYER disabledLayer = static_cast<RE::COL_LAYER>(kDefaultCameraLayerIndex);
			std::uint32_t enabledResolvedForm = 0;
			std::uint32_t disabledResolvedForm = 0;
			std::uint64_t enabledMask = 0;
			std::uint64_t disabledMask = 0;
			std::string msg;

			if (ResolveLayerSpec(Settings::EnabledCameraLayerSpec, enabledLayer, enabledResolvedForm, enabledMask, msg)) {
				g_enabledCameraLayer.store(enabledLayer, std::memory_order_release);
				g_enabledCameraLayerMask.store(enabledMask, std::memory_order_release);
				Settings::LogEvent("[layer] enabled spec resolved: " + Settings::EnabledCameraLayerSpec +
					" -> form=0x" + Hex8(enabledResolvedForm) +
					" layerIdx=" + std::to_string(static_cast<std::uint32_t>(enabledLayer)) +
					" mask=0x" + Hex16(enabledMask), true);
			} else {
				Settings::LogEvent("[layer] enabled spec failed: " + Settings::EnabledCameraLayerSpec + " reason=" + msg, true);
			}

			if (ResolveLayerSpec(Settings::DisabledCameraLayerSpec, disabledLayer, disabledResolvedForm, disabledMask, msg)) {
				g_disabledCameraLayer.store(disabledLayer, std::memory_order_release);
				g_disabledCameraLayerMask.store(disabledMask, std::memory_order_release);
				Settings::LogEvent("[layer] disabled spec resolved: " + Settings::DisabledCameraLayerSpec +
					" -> form=0x" + Hex8(disabledResolvedForm) +
					" layerIdx=" + std::to_string(static_cast<std::uint32_t>(disabledLayer)) +
					" mask=0x" + Hex16(disabledMask), true);
			} else {
				Settings::LogEvent("[layer] disabled spec failed: " + Settings::DisabledCameraLayerSpec + " reason=" + msg, true);
			}

			if (enabledLayer == disabledLayer && enabledMask == disabledMask) {
				Settings::LogEvent("[layer] warning: enabled/disabled layerIdx are identical (" +
					std::to_string(static_cast<std::uint32_t>(enabledLayer)) +
					"). Toggle will not change camera collision behavior.", true);
			} else if (enabledLayer == disabledLayer && enabledMask != disabledMask) {
				Settings::LogEvent("[layer] info: enabled/disabled share same layerIdx but have different masks; runtime mask switching will be used.", true);
			}
		}

		void ApplyCameraCollisionLayer(RE::COL_LAYER a_layer, std::uint64_t a_mask, const char* a_reason)
		{
			// Apply profile to both camera objects and global bhkCollisionFilter table.
			// Updating layerBitfields[idx] is what makes same-layerIdx profiles diverge.
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				Settings::LogEvent(std::string("[layer] apply skipped (") + a_reason + "): PlayerCamera unavailable", true);
				return;
			}

			bool appliedToRoot = false;
			bool appliedToRigidBody = false;
			bool appliedToFilter = false;

			if (auto* root = camera->cameraRoot.get()) {
				root->SetCollisionLayer(a_layer);
				appliedToRoot = true;
			}

			if (auto* body = camera->rigidBody.get()) {
				if (auto* rigid = body->GetRigidBody()) {
					if (auto* collidable = rigid->GetCollidableRW()) {
						collidable->broadPhaseHandle.collisionFilterInfo.SetCollisionLayer(a_layer);
						appliedToRigidBody = true;
					}
				}
			}

			if (auto* collisionFilter = RE::bhkCollisionFilter::GetSingleton()) {
				const auto idx = static_cast<std::uint32_t>(a_layer);
				if (idx < 64) {
					collisionFilter->layerBitfields[idx] = a_mask;
					appliedToFilter = true;
				}
			}

			Settings::LogEvent(std::string("[layer] apply (") + a_reason + "): layerIdx=" +
				std::to_string(static_cast<std::uint32_t>(a_layer)) +
				" mask=0x" + Hex16(a_mask) +
				" root=" + (appliedToRoot ? "1" : "0") +
				" rigidBody=" + (appliedToRigidBody ? "1" : "0") +
				" filter=" + (appliedToFilter ? "1" : "0"), true);
		}
	}

	static void ResetRuntimeStateToVanilla()
	{
		g_hitObjects.clear();
		g_publishedHits.store(nullptr, std::memory_order_release);
		g_clipDistance.store(0.0f, std::memory_order_release);
		g_forceVisRecord.store(nullptr, std::memory_order_release);
		g_playerRoom.store(nullptr, std::memory_order_release);
		OcclusionStripper::RestoreAll();
		OcclusionStripper::SetStripped(false);

		if (auto* sky = RE::Sky::GetSingleton()) {
			if (auto* skyRoot = sky->root.get()) {
				skyRoot->SetAppCulled(false);
			}
		}
	}

	// HookQPointWithin is defined further down (it depends on g_forceVisRecord/g_playerRoom
	// declared above). Update needs to dispatch through its `func` trampoline to ask the
	// engine's real QPointWithin which room the player is in, so forward-declare a tiny
	// helper here and define it after the struct.
	namespace { bool CallOrigQPointWithin(RE::BSMultiBoundRoom* a_room, RE::NiPoint3& a_point); }

	static void UpdateCameraData()
	{
		using func_t = decltype(&UpdateCameraData);
		static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };
		func();
	}

	static RE::BSGraphics::RendererData* GetRendererDataCompat()
	{
		return RE::BSGraphics::Renderer::GetRendererData();
	}

	static REX::W32::ID3D11DeviceContext* GetRendererContextCompat()
	{
		auto* rendererData = GetRendererDataCompat();
		return rendererData ? rendererData->context : nullptr;
	}

	static RE::NiPoint3 GetCameraPositionCompat(const RE::PlayerCamera* a_camera)
	{
#ifdef COMMONLIBSSE_LEGACY_API
		return a_camera ? a_camera->pos : RE::NiPoint3{};
#else
		return a_camera ? a_camera->GetRuntimeData2().pos : RE::NiPoint3{};
#endif
	}

	static RE::LOADED_CELL_DATA* GetLoadedCellDataCompat(RE::TESObjectCELL* a_cell)
	{
		if (!a_cell) {
			return nullptr;
		}
#ifdef COMMONLIBSSE_LEGACY_API
		return a_cell->loadedData;
#else
		return a_cell->GetRuntimeData().loadedData;
#endif
	}

	static RE::BSGraphics::ViewData* GetShadowViewDataCompat()
	{
#ifdef COMMONLIBSSE_LEGACY_API
		auto* state = RE::BSGraphics::State::GetSingleton();
		if (!state || state->cameraDataCache.empty()) {
			return nullptr;
		}
		auto* raw = state->cameraDataCache.front();
		return raw ? reinterpret_cast<RE::BSGraphics::ViewData*>(raw) : nullptr;
#else
		auto* shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		if (!shadowState) {
			return nullptr;
		}
		auto& data = shadowState->GetRuntimeData();
		// data.cameraData is wrapped in EYE_POSITION<ViewData>; for flat runtime eye[0]
		// is layout-compatible with ViewData.
		return reinterpret_cast<RE::BSGraphics::ViewData*>(&data.cameraData);
#endif
	}

	using ViewMatrix = RE::BSGraphics::ViewData::Matrix;

	static float& Matrix34(ViewMatrix& a_m)
	{
		return a_m.m[2][3];
	}

	static const float& Matrix34(const ViewMatrix& a_m)
	{
		return a_m.m[2][3];
	}

	static float& Matrix43(ViewMatrix& a_m)
	{
		return a_m.m[3][2];
	}

	static const float& Matrix43(const ViewMatrix& a_m)
	{
		return a_m.m[3][2];
	}

	static ViewMatrix MultiplyViewMatrix(const ViewMatrix& a_lhs, const ViewMatrix& a_rhs)
	{
		ViewMatrix out{};
		for (std::uint32_t r = 0; r < 4; ++r) {
			for (std::uint32_t c = 0; c < 4; ++c) {
				float sum = 0.0f;
				for (std::uint32_t k = 0; k < 4; ++k) {
					sum += a_lhs.m[r][k] * a_rhs.m[k][c];
				}
				out.m[r][c] = sum;
			}
		}
		return out;
	}

	// Hook ID3D11DeviceContext::ClearRenderTargetView (vtbl index 50). The engine's
	// back-buffer clear passes &RendererData2::clearColor as the color pointer; we
	// detect that exact pointer and substitute black while clipping. Other clears
	// (shadow maps, post-fx targets, ENB/ReShade) pass different pointers and are
	// untouched, and we always call the original ??so chained hooks still see the call.
	using ClearRTV_t = void(STDMETHODCALLTYPE*)(REX::W32::ID3D11DeviceContext*, REX::W32::ID3D11RenderTargetView*, const float[4]);
	static ClearRTV_t s_origClearRTV = nullptr;

	static void STDMETHODCALLTYPE HookedClearRTV(REX::W32::ID3D11DeviceContext* a_ctx, REX::W32::ID3D11RenderTargetView* a_rtv, const float a_color[4])
	{
		if (g_clipDistance.load(std::memory_order_relaxed) > 0.0f && a_color) {
			if (auto* rendererData = GetRendererDataCompat()) {
				if (a_color == &rendererData->clearColor[0]) {
					const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
					s_origClearRTV(a_ctx, a_rtv, black);
					return;
				}
			}
		}
		s_origClearRTV(a_ctx, a_rtv, a_color);
	}

	static void InstallClearRTVHook()
	{
		static bool s_installed = false;
		if (s_installed) {
			return;
		}
		auto* ctx = GetRendererContextCompat();
		if (!ctx) {
			return;
		}
		void** vtbl = *reinterpret_cast<void***>(ctx);
		constexpr std::size_t kClearRTVIndex = 50; // ID3D11DeviceContext::ClearRenderTargetView
		DWORD oldProtect = 0;
		if (!VirtualProtect(&vtbl[kClearRTVIndex], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			return;
		}
		s_origClearRTV = reinterpret_cast<ClearRTV_t>(vtbl[kClearRTVIndex]);
		vtbl[kClearRTVIndex] = reinterpret_cast<void*>(&HookedClearRTV);
		VirtualProtect(&vtbl[kClearRTVIndex], sizeof(void*), oldProtect, &oldProtect);
		s_installed = true;
		logs::info("ClearRenderTargetView hook installed");
	}

	void HookBSLightingShader::BSBatchRenderer_RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
	{
		bool        shouldClip = false;
		const float clipDist   = g_clipDistance.load(std::memory_order_relaxed);
		auto        hits       = g_publishedHits.load(std::memory_order_acquire);
		if (a_pass && a_pass->geometry && clipDist > 0.0f && hits && !hits->empty()) {
			RE::NiAVObject* cur = a_pass->geometry;
			while (cur) {
				if (hits->count(cur) != 0) {
					shouldClip = true;
					break;
				}
				cur = cur->parent;
			}
		}

		if (shouldClip) {
			auto* cameraData = GetShadowViewDataCompat();
			if (cameraData && std::abs(Matrix34(cameraData->projMat) - 1.0f) < 0.01f && std::abs(Matrix43(cameraData->projMat)) < 50.0f) {
				const float origNear = -Matrix43(cameraData->projMat);
				if (clipDist > origNear) {
					auto origProj = cameraData->projMat;
					auto origViewProj = cameraData->viewProjMat;
					auto origProjUnj = cameraData->projMatrixUnjittered;
					auto origViewProjUnj = cameraData->viewProjMatrixUnjittered;

					auto* context = GetRendererContextCompat();
					if (!context) {
						func(a_pass, a_technique, a_alphaTest, a_renderFlags);
						return;
					}

					std::uint32_t numViewports = 1;
					REX::W32::D3D11_VIEWPORT viewport;
					context->RSGetViewports(&numViewports, &viewport);

					const float origMinDepth = viewport.minDepth;
					const float minDepthShift = 1.0f - (origNear / clipDist);
					viewport.minDepth = origMinDepth + minDepthShift * (viewport.maxDepth - origMinDepth);
					context->RSSetViewports(1, &viewport);

					Matrix43(cameraData->projMat) = -clipDist;
					cameraData->viewProjMat = MultiplyViewMatrix(cameraData->viewMat, cameraData->projMat);
					Matrix43(cameraData->projMatrixUnjittered) = -clipDist;
					cameraData->viewProjMatrixUnjittered = MultiplyViewMatrix(cameraData->viewMat, cameraData->projMatrixUnjittered);

					UpdateCameraData();
					func(a_pass, a_technique, a_alphaTest, a_renderFlags);

					context->RSGetViewports(&numViewports, &viewport);
					viewport.minDepth = origMinDepth;
					context->RSSetViewports(1, &viewport);

					cameraData->projMat = origProj;
					cameraData->viewProjMat = origViewProj;
					cameraData->projMatrixUnjittered = origProjUnj;
					cameraData->viewProjMatrixUnjittered = origViewProjUnj;
					UpdateCameraData();
					return;
				}
			}
		}
		func(a_pass, a_technique, a_alphaTest, a_renderFlags);
	}

	static bool IsFinite(const RE::NiPoint3& a_pos)
	{
		return std::isfinite(a_pos.x) && std::isfinite(a_pos.y) && std::isfinite(a_pos.z);
	}

	void HookPlayerCharacter::Update(RE::Actor* a_this, float a_delta)
	{
		_Update(a_this, a_delta);
		if (!a_this || !a_this->IsPlayerRef()) {
			return;
		}

		MarkCurrentThreadAsUpdateThread();
		DrainDeferredProfileApply();

		EvaluateAndApplyProfile(a_this, "player-update", false);

		auto camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		RE::NiPoint3 cameraPos = GetCameraPositionCompat(camera);
		RE::NiPoint3 playerPos = a_this->GetPosition();
		playerPos.z += Settings::PlayerHeightOffset;

		auto* parentCell = a_this->GetParentCell();
		auto* bhkWorld = parentCell ? parentCell->GetbhkWorld() : nullptr;
		if (!bhkWorld || !IsFinite(cameraPos) || !IsFinite(playerPos)) {
			return;
		}

		// Hide the sky scene-subtree only when see-through is actively engaged
		// AND the player is in an interior. Sets kHidden on Sky::root so the
		// renderer skips the entire sky subtree (sun, moons, stars, clouds, aurora,
		// atmosphere) without touching Sky::mode, weather, or any downstream
		// lighting state. Default interior play (no see-through, e.g. just
		// looking out a Dragonsreach skylight) keeps sky visible.
		if (auto* sky = RE::Sky::GetSingleton()) {
			if (auto* skyRoot = sky->root.get()) {
				const bool seeThroughActive = g_clipDistance.load(std::memory_order_acquire) > 0.0f;
				const bool shouldHide       = seeThroughActive && parentCell->IsInteriorCell();
				skyRoot->SetAppCulled(shouldHide);
			}
		}

		// Identify the room the player is in (interior cells only). Walk the cell's
		// portal graph and call the engine's real QPointWithin via the trampoline ??		// never the thunk, which would recurse on this logic. The QPointWithin
		// thunk uses the cached pointer to gate its override (only fires when
		// non-null = player is in some room of an interior with a portal graph).
		// kFindPlayerRoom needs kInstallQPoint ??the helper dispatches through the
		// QPointWithin trampoline, which is only populated by Install().
		static_assert(!kFindPlayerRoom || kInstallQPoint, "kFindPlayerRoom requires kInstallQPoint");
		RE::BSMultiBoundRoom* foundRoom = nullptr;
		if (kFindPlayerRoom && parentCell->IsInteriorCell()) {
			if (auto* loaded = GetLoadedCellDataCompat(parentCell)) {
				if (auto* graph = loaded->portalGraph.get()) {
					for (auto& roomPtr : graph->rooms) {
						if (auto* room = roomPtr.get()) {
							RE::NiPoint3 pp = playerPos;  // by-ref; engine may mutate
							if (CallOrigQPointWithin(room, pp)) {
								foundRoom = room;
								break;
							}
						}
					}
				}
			}
		}
		g_playerRoom.store(foundRoom, std::memory_order_release);

		float scale = RE::bhkWorld::GetWorldScale();
		RE::CFilter colFilter;
		a_this->GetCollisionFilterInfo(colFilter);
		const RE::COL_LAYER desiredLayer = g_useEnabledCameraProfile.load(std::memory_order_acquire)
			? g_enabledCameraLayer.load(std::memory_order_acquire)
			: g_disabledCameraLayer.load(std::memory_order_acquire);
		// Casting layer follows the currently selected runtime camera profile.
		colFilter.SetCollisionLayer(desiredLayer);
		// Age existing entries; new hits will reset to 0 below.
		for (auto& [obj, entry] : g_hitObjects) {
			entry.secondsSinceSeen += a_delta;
		}

		const RE::NiPoint3 ray = playerPos - cameraPos;
		const float totalDist = ray.Length();
		if (totalDist < 1.0f) {
			OcclusionStripper::SetStripped(false);
			return;
		}
		const RE::NiPoint3 dir = ray * (1.0f / totalDist);

		// Build an orthonormal basis (u, v) perpendicular to dir for fanning rays.
		RE::NiPoint3 u = dir.Cross(RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
		if (const float uLen = u.Length(); uLen < 1e-3f) {
			u = { 1.0f, 0.0f, 0.0f };
		} else {
			u = u * (1.0f / uLen);
		}
		const RE::NiPoint3 vBase = dir.Cross(u);

		// Rotate the sample angles by the golden angle each frame so successive frames
		// fill in different points around the disc. Combined with Settings::HitLingerSeconds of
		// hysteresis this gives a dense, non-repeating sample without more rays/frame.
		static std::uint32_t s_fanFrame = 0;
		constexpr float      kGoldenAngle = 2.39996323f;  // radians, ~137.5?
		const float          phase        = s_fanFrame++ * kGoldenAngle;

		auto* selfRoot = a_this->Get3D();
		bool  hadHitThisFrame = false;
		constexpr int   kMaxHits     = 8;
		constexpr float kStepEpsilon = 2.0f;

		// Bias the fan into an ellipse: wider horizontally than vertically. Most
		// occluders that matter for a chase cam are walls/columns to the sides; the
		// floor/ceiling above and below rarely need clipping, so don't waste rays there.
		auto ellipse = [&](float a_baseAngle) {
			const float a = a_baseAngle + phase;
			return u * (std::cos(a) * Settings::FanRadiusH) + vBase * (std::sin(a) * Settings::FanRadiusV);
		};
		constexpr float kPi = 3.14159265f;
		const RE::NiPoint3 offsets[9] = {
			{ 0.0f, 0.0f, 0.0f },
			ellipse(0.0f),
			ellipse(kPi * 0.25f),
			ellipse(kPi * 0.5f),
			ellipse(kPi * 0.75f),
			ellipse(kPi),
			ellipse(kPi * 1.25f),
			ellipse(kPi * 1.5f),
			ellipse(kPi * 1.75f),
		};

		for (const auto& offset : offsets) {
			const RE::NiPoint3 rayFromBase = cameraPos + offset;
			const RE::NiPoint3 rayToBase   = playerPos + offset;
			float start = 0.0f;

			for (int i = 0; i < kMaxHits; ++i) {
				const RE::NiPoint3 from = rayFromBase + dir * start;
				RE::bhkPickData pick;
				pick.rayInput.from       = { from.x * scale, from.y * scale, from.z * scale, 0 };
				pick.rayInput.to         = { rayToBase.x * scale, rayToBase.y * scale, rayToBase.z * scale, 0 };
				pick.rayInput.filterInfo = colFilter;

				if (!bhkWorld->PickObject(pick) || !pick.rayOutput.HasHit()) {
					break;
				}

				const float segLen            = totalDist - start;
				const float hitDistFromCamera = start + segLen * pick.rayOutput.hitFraction;

				// Climb to the REFR's 3D root so all sub-meshes (scaffolding, trim,
				// separate draw calls under the same building) share one ancestor.
				auto* refr = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
				RE::NiAVObject* root = refr ? refr->Get3D() : nullptr;
				if (!root) {
					root = RE::TESHavokUtilities::FindCollidableObject(*pick.rayOutput.rootCollidable);
				}
				// Reject floor-ish surfaces by face-normal regardless of collision layer.
				// Skyrim authors roads/paths/floors as plain kStatic, so the layer alone
				// can't tell them from walls ??but a near-vertical hit normal (within ~45?
				// of world up) can. Havok normals are +Z-up, same as the engine.
				const bool isFloorish = std::abs(pick.rayOutput.normal.Dot3({ 0.0f, 0.0f, 1.0f, 0.0f })) > 0.7f;

				if (!isFloorish && root && root != selfRoot && refr != a_this) {
					// Read the layer from the hit collidable directly. NiAVObject::GetCollisionLayer
					// only inspects the root's own collision object; doors and other compound
					// meshes carry collision on a child node and would otherwise report kUnidentified.
					const auto colLayer = pick.rayOutput.rootCollidable->GetCollisionLayer();
					if (colLayer == RE::COL_LAYER::kStatic || colLayer == RE::COL_LAYER::kAnimStatic) {
						auto [it, inserted] = g_hitObjects.try_emplace(root, HitEntry{ RE::NiPointer<RE::NiAVObject>(root), 0.0f, hitDistFromCamera });
						auto& entry = it->second;
						entry.secondsSinceSeen = 0.0f;
						// Only grow distance within the linger window ??fan rays hitting the
						// same wall at slightly closer points must not pull the clip plane back.
						if (!inserted && hitDistFromCamera > entry.distFromCamera) {
							entry.distFromCamera = hitDistFromCamera;
						}
						hadHitThisFrame = true;
					}
				}

				start = hitDistFromCamera + kStepEpsilon;
				if (start >= totalDist) {
					break;
				}
			}
		}

		// Drop entries that haven't been re-hit recently.
		std::erase_if(g_hitObjects, [](const auto& kv) {
			return kv.second.secondsSinceSeen > Settings::HitLingerSeconds;
		});

		// Build the publish snapshot and find the furthest hit in one pass. The
		// release on the publishedHits store pairs with the acquire load in the
		// consumer hook so the set's contents are visible. clipDistance is released
		// after, so any observer seeing a non-zero clip is guaranteed to see the
		// matching set.
		auto  snap     = std::make_shared<HitSet>();
		float furthest = 0.0f;
		snap->reserve(g_hitObjects.size());
		for (const auto& [obj, entry] : g_hitObjects) {
			snap->insert(obj);
			if (entry.distFromCamera > furthest) {
				furthest = entry.distFromCamera;
			}
		}
		g_publishedHits.store(std::shared_ptr<const HitSet>(std::move(snap)), std::memory_order_release);
		g_clipDistance.store(g_hitObjects.empty() ? 0.0f : furthest + 15.0f, std::memory_order_release);

		// Strip linger is short (absorbs ray-miss frames) but well below the visual
		// linger, so occluders snap back on once you actually stop looking through
		// walls. Visual shift still rides the longer HitLingerSeconds for smoothness.
		static float s_secondsSinceHit = 1e6f;  // huge so first frame doesn't strip
		if (hadHitThisFrame) {
			s_secondsSinceHit = 0.0f;
		} else {
			s_secondsSinceHit += a_delta;
		}
		OcclusionStripper::SetStripped(s_secondsSinceHit < Settings::StripLingerSeconds);
	}

	//I know RawNode and RawList are bad. but I'm pretty sure that the interface in commonlib is wrong.
	//The 3rd member is supposed to be a pointer to an occlusionshape, not a stack allocated one. And
	// the fact that this shit works is representative of this.
	namespace
	{
		// Mirrors the runtime layout of NiTPointerList<BSOcclusionShape*> ??a
		// next/prev/data triple per node, with the list header carrying head/tail
		// and the allocator's size field. We READ these in the collection step
		// (no mutation), so the engine's own cleanup paths walk them unchanged.
		struct RawNode
		{
			RawNode*              next;
			RawNode*              prev;
			RE::BSOcclusionShape* shape;
		};
		static_assert(sizeof(RawNode) == 0x18);

		struct RawList
		{
			RawNode*      head;
			RawNode*      tail;
			std::uint32_t size;
			std::uint32_t pad;
		};
		static_assert(sizeof(RawList) == 0x18);

		// Lateral margin around the camera??layer segment. Shapes whose AABB
		// (inflated by this radius) is hit by the segment get added to the
		// skip-set. Smaller = fewer shapes skipped = better perf in dense areas
		// but more risk of see-through getting blocked by an off-axis occluder.
		constexpr float kStripRadius = 512.f;

		// Conservative world-space AABB for an occlusion shape. Plane is a flat
		// box (z extent = 0). Rotation is folded in via |M|?e (tighter than a
		// pure sphere, looser than a true OBB).
		bool ShapeWorldAABB(const RE::BSOcclusionShape* a_shape, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
		{
			if (!a_shape) {
				return false;
			}
			RE::NiPoint3 e{ 0.0f, 0.0f, 0.0f };
			if (a_shape->IsOcclusionBox()) {
				const auto* box = static_cast<const RE::BSOcclusionBox*>(a_shape);
				e = { box->size.x, box->size.y, box->size.z };
			} else if (a_shape->IsOcclusionPlane()) {
				const auto* plane = static_cast<const RE::BSOcclusionPlane*>(a_shape);
				e = { plane->size.x, plane->size.y, 0.0f };
			} else {
				return false;
			}
			const auto& r = a_shape->rotation;
			const RE::NiPoint3 worldHalf{
				std::abs(r.entry[0][0]) * e.x + std::abs(r.entry[0][1]) * e.y + std::abs(r.entry[0][2]) * e.z,
				std::abs(r.entry[1][0]) * e.x + std::abs(r.entry[1][1]) * e.y + std::abs(r.entry[1][2]) * e.z,
				std::abs(r.entry[2][0]) * e.x + std::abs(r.entry[2][1]) * e.y + std::abs(r.entry[2][2]) * e.z,
			};
			a_min = a_shape->translation - worldHalf;
			a_max = a_shape->translation + worldHalf;
			return true;
		}

		// Standard slab segment-vs-AABB intersection.
		bool SegmentIntersectsAABB(const RE::NiPoint3& a_A, const RE::NiPoint3& a_B,
		                           const RE::NiPoint3& a_min, const RE::NiPoint3& a_max)
		{
			const RE::NiPoint3 D = a_B - a_A;
			float tmin = 0.0f, tmax = 1.0f;
			for (int i = 0; i < 3; ++i) {
				const float a  = (&a_A.x)[i];
				const float d  = (&D.x)[i];
				const float lo = (&a_min.x)[i];
				const float hi = (&a_max.x)[i];
				if (std::abs(d) < 1e-6f) {
					if (a < lo || a > hi) {
						return false;
					}
					continue;
				}
				float t1 = (lo - a) / d;
				float t2 = (hi - a) / d;
				if (t1 > t2) {
					std::swap(t1, t2);
				}
				tmin = std::max(tmin, t1);
				tmax = std::min(tmax, t2);
				if (tmin > tmax) {
					return false;
				}
			}
			return true;
		}

		// ???? Position-displace strip machinery ????????????????????????????????????????????????????????????????????
		// Save each shape's original translation on first encounter, then write
		// a far-away position so it occludes nothing visible. Restore writes the
		// saved translation back. List structure stays untouched ??the engine's
		// extra-data cleanup during cell unload walks unmodified pointers.
		// Keepalive holds the shape alive even if its graph drops before restore.
		//
		// Race note: shape->translation is a 12-byte NiPoint3 written here without
		// locking. If the engine reads positions concurrently from another thread
		// during our write, the read could tear (mix of old + new components). We
		// accept this ??worst case is one frame of partial-displaced positions,
		// which manifests as imperceptible flicker. The displace/restore both run
		// from SetStripped, called only from Update, which we assume is serial.
		struct PositionSnapshot
		{
			RE::NiPointer<RE::BSOcclusionShape> keepalive;
			RE::NiPoint3                        saved;
		};

		std::unordered_map<RE::BSOcclusionShape*, PositionSnapshot>& Positions()
		{
			static std::unordered_map<RE::BSOcclusionShape*, PositionSnapshot> s_map;
			return s_map;
		}

		bool& StrippedFlag()
		{
			static bool s_stripped = false;
			return s_stripped;
		}

		// "Out of frustum / nowhere near anything" ??large but finite so we don't
		// trip any NaN/Inf guards in engine code. Anything beyond ~1e7 is well
		// outside any worldspace cell coordinate the engine ever uses.
		constexpr RE::NiPoint3 kFarawayPosition{ 1.0e8f, 1.0e8f, 1.0e8f };

		void DisplaceList(RawList* a_list,
		                  const RE::NiPoint3& a_segA, const RE::NiPoint3& a_segB,
		                  float a_radius)
		{
			if (!a_list) {
				return;
			}
			auto& map = Positions();
			for (auto* node = a_list->head; node; node = node->next) {
				auto* shape = node->shape;
				if (!shape) {
					continue;
				}
				if (map.contains(shape)) {
					continue;  // already displaced this frame; don't re-snapshot the displaced position
				}
				RE::NiPoint3 mn, mx;
				if (!ShapeWorldAABB(shape, mn, mx)) {
					continue;
				}
				const RE::NiPoint3 r{ a_radius, a_radius, a_radius };
				if (!SegmentIntersectsAABB(a_segA, a_segB, mn - r, mx + r)) {
					continue;
				}
				map.emplace(shape, PositionSnapshot{
					RE::NiPointer<RE::BSOcclusionShape>(shape),
					shape->translation
				});
				shape->translation = kFarawayPosition;
			}
		}

		void RestoreAllPositions()
		{
			auto& map = Positions();
			for (auto& [rawPtr, snap] : map) {
				if (auto* live = snap.keepalive.get()) {
					live->translation = snap.saved;
				}
			}
			map.clear();
		}

	}


	// Hook BSMultiBoundRoom::QPointWithin (vfunc 0x3F). The engine calls it to test
	// whether a point lies inside a room ??drives both camera-room association
	// (which seeds the portal-graph visibility walk) and object-room association.
	//
	// While see-through is engaged AND the player is in some interior with a portal
	// graph, we return true for every room in that cell's graph. The engine
	// accumulates visibility across every room that returns true, so the entire
	// cell renders regardless of where the camera floats. Outside see-through
	// (clipDistance == 0) we don't override anything, so default play is untouched.
	struct HookQPointWithin
	{
		static bool thunk(RE::BSMultiBoundNode* a_this, RE::NiPoint3& a_point)
		{
			const bool seeThroughActive = g_clipDistance.load(std::memory_order_acquire) > 0.0f;
			const bool havePlayerRoom   = g_playerRoom.load(std::memory_order_acquire) != nullptr;
			if (seeThroughActive && havePlayerRoom) {
				auto       rec  = g_forceVisRecord.load(std::memory_order_acquire);
				auto*      self = static_cast<RE::BSMultiBoundRoom*>(a_this);
				if (rec && rec->rooms.count(self) > 0) {
					return true;
				}
			}
			return func(a_this, a_point);
		}
		static inline REL::Relocation<decltype(thunk)> func;

		static void Install()
		{
			if (!kInstallQPoint) {
				logs::info("[install] BSMultiBoundRoom::QPointWithin hook SKIPPED (kInstallQPoint=false)");
				return;
			}
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSMultiBoundRoom[0] };
			func = vtbl.write_vfunc(0x3F, thunk);
			logs::info("[install] BSMultiBoundRoom::QPointWithin hook installed at vtbl[0x3F]");
		}
	};

	namespace
	{
		bool CallOrigQPointWithin(RE::BSMultiBoundRoom* a_room, RE::NiPoint3& a_point)
		{
			return HookQPointWithin::func(a_room, a_point);
		}
	}

	OcclusionStripper* OcclusionStripper::GetSingleton()
	{
		static OcclusionStripper inst;
		return &inst;
	}

	void OcclusionStripper::StripGraph(RE::BSPortalGraph* a_graph)
	{
		// Outdoors: position-displace shapes inside the camera??layer capsule so they
		// stop occluding visible geometry. The graph's list structure is read but never
		// mutated, so the engine's extra-data cleanup during cell unload walks
		// unmodified pointers ??no race, no crash.
		//
		// Indoors: publish (graph + rooms + bounds) into g_forceVisRecord so the
		// QPointWithin thunk can redirect camera-position queries to the player's
		// room. Separate mechanism from the displace strip.
		if (!a_graph) {
			return;
		}
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return;
		}
		auto* cam = RE::PlayerCamera::GetSingleton();
		const RE::NiPoint3 segB = pc->GetPosition();
		const RE::NiPoint3 segA = cam ? GetCameraPositionCompat(cam) : segB;
		const bool         isInterior = pc->GetParentCell() && pc->GetParentCell()->IsInteriorCell();

		// Outdoor only: position-displace strip. Write each in-capsule shape's
		// translation far away so it stops occluding visible geometry. List
		// structure unchanged ??cell-unload extra-data cleanup is unaffected.
		if (!isInterior) {
			// reinterpret_cast hell
			DisplaceList(reinterpret_cast<RawList*>(&a_graph->occlusionShapes),
			             segA, segB, kStripRadius);
		}

		if (isInterior && kPublishIndoor) {
			auto rec            = std::make_shared<ForceVisRecord>();
			rec->graphKeepalive = RE::NiPointer<RE::BSPortalGraph>(a_graph);
			for (auto& roomPtr : a_graph->rooms) {
				if (auto* room = roomPtr.get()) {
					rec->rooms.insert(room);
				}
			}
			g_forceVisRecord.store(std::shared_ptr<const ForceVisRecord>(std::move(rec)),
			                       std::memory_order_release);
		}
	}

	void OcclusionStripper::StripCell(RE::TESObjectCELL* a_cell)
	{
		if (!a_cell) {
			return;
		}
		if (auto* loaded = GetLoadedCellDataCompat(a_cell)) {
			if (auto* graph = loaded->portalGraph.get()) {
				StripGraph(graph);
			}
		}
	}

	void OcclusionStripper::RestoreAll()
	{
		// Position-displace restore: write each saved translation back.
		RestoreAllPositions();
	}

	bool OcclusionStripper::IsStripped()
	{
		return StrippedFlag();
	}

	void OcclusionStripper::SetStripped(bool a_stripped)
	{
		if (!kEnableStripper) {
			return;
		}

		// Position-displace lifecycle: displace once on toggle-on, refresh every
		// 0.25s while active so shapes that come into the capsule as the player
		// moves get caught. Restore-then-displace keeps the saved-position map
		// in sync (re-displacing without restoring would snapshot a displaced
		// position as the "saved" value, then restore-to-far-away forever).
		constexpr float kRefreshInterval = 0.25f;
		static float*   g_DeltaTime      = (float*)RELOCATION_ID(523661, 410200).address();
		static float    s_secondsSinceRefresh = 0.0f;

		if (a_stripped == StrippedFlag()) {
			if (a_stripped) {
				s_secondsSinceRefresh += *g_DeltaTime;
				if (s_secondsSinceRefresh >= kRefreshInterval) {
					RestoreAllPositions();
					StripAll();
					s_secondsSinceRefresh = 0.0f;
				}
			}
			return;
		}
		if (a_stripped) {
			StripAll();
			StrippedFlag() = true;
			s_secondsSinceRefresh = 0.0f;
		} else {
			RestoreAll();
			StrippedFlag() = false;
		}
	}

	void OcclusionStripper::StripAll()
	{
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return;
		}
		if (auto* cell = pc->GetParentCell()) {
			StripCell(cell);
		}
		if (auto* ws = pc->GetWorldspace()) {
			if (auto* graph = ws->portalGraph.get()) {
				StripGraph(graph);
			}
		}
	}

	RE::BSEventNotifyControl OcclusionStripper::ProcessEvent(
		const RE::TESCellFullyLoadedEvent*,
		RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
	{
		// On any cell-load: restore positions if currently stripped, then re-strip
		// so the new cell's shapes get picked up and stale entries are dropped.
		// Position-displace doesn't crash on cell transition because list
		// structure is untouched, so we don't need this to fire pre-transition.
		if (StrippedFlag()) {
			RestoreAllPositions();
			StrippedFlag() = false;
			StripAll();
			StrippedFlag() = true;
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl OcclusionStripper::ProcessEvent(
		const RE::TESLoadGameEvent*,
		RE::BSTEventSource<RE::TESLoadGameEvent>*)
	{
		g_forceVisRecord.store(nullptr, std::memory_order_release);
		g_publishedHits.store(nullptr, std::memory_order_release);
		g_clipDistance.store(0.0f, std::memory_order_release);
		RestoreAllPositions();
		StrippedFlag() = false;
		return RE::BSEventNotifyControl::kContinue;
	}

	void OcclusionStripper::Install()
	{
		if (!kEnableStripper) {
			logs::info("occlusion stripper DISABLED (kEnableStripper=false)");
			return;
		}
		if (auto* src = RE::ScriptEventSourceHolder::GetSingleton()) {
			src->AddEventSink<RE::TESCellFullyLoadedEvent>(GetSingleton());
			src->AddEventSink<RE::TESLoadGameEvent>(GetSingleton());
			logs::info("occlusion stripper sinks registered");
		}
	}

	void Install()
	{
		HookPlayerCharacter::Install();
		HookBSLightingShader::Install();
		HookQPointWithin::Install();
		OcclusionStripper::Install();
	}

	void OnDataLoaded()
	{
		InstallClearRTVHook();
		RefreshConfiguredCameraLayers();

		auto* player = RE::PlayerCharacter::GetSingleton();
		EvaluateAndApplyProfile(player, "data-loaded", true);
	}

	void OnFeatureToggleChanged(bool a_enabled)
	{
		// Master toggle and condition toggles only affect profile selection.
		RefreshConfiguredCameraLayers();

		auto* player = RE::PlayerCharacter::GetSingleton();
		EvaluateAndApplyProfile(player, a_enabled ? "toggle-master-on" : "toggle-master-off", true);
	}
}
