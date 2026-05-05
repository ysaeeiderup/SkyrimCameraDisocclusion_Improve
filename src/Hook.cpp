#include "Hook.h"
#include "Settings.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "RE/C/CFilter.h"
#include <cmath>
#include <Windows.h>
#include <MinHook.h>

namespace Hooks
{
	struct HitEntry
	{
		RE::NiPointer<RE::NiAVObject> keepalive;
		float                         secondsSinceSeen;
		float                         distFromCamera;
	};
	static std::unordered_map<RE::NiAVObject*, HitEntry> g_hitObjects;

	using HitSet = std::unordered_set<RE::NiAVObject*>;
	static std::atomic<std::shared_ptr<const HitSet>> g_publishedHits;
	static std::atomic<float>                         g_clipDistance{ 0.0f };

	static void UpdateCameraData()
	{
		using func_t = decltype(&UpdateCameraData);
		static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };
		func();
	}

	using ClearRTV_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const float[4]);
	static ClearRTV_t s_origClearRTV = nullptr;

	static void STDMETHODCALLTYPE HookedClearRTV(ID3D11DeviceContext* a_ctx, ID3D11RenderTargetView* a_rtv, const float a_color[4])
	{
		if (g_clipDistance.load(std::memory_order_relaxed) > 0.0f && a_rtv) {
			if (auto* renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				auto& mainTarget = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
				if (a_rtv == mainTarget.RTV) {
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
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			return;
		}
		auto* ctx = renderer->GetRuntimeData().context;
		if (!ctx) {
			return;
		}
		void** vtbl = *reinterpret_cast<void***>(ctx);
		constexpr std::size_t kClearRTVIndex = 50;
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
			auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			if (shadowState) {
				auto& data = shadowState->GetRuntimeData();
				auto& cameraData = *reinterpret_cast<RE::BSGraphics::ViewData*>(&data.cameraData);

				if (std::abs(cameraData.projMat._34 - 1.0f) < 0.01f && std::abs(cameraData.projMat._43) < 50.0f) {
					float origNear = -cameraData.projMat._43;
					if (clipDist > origNear) {
						auto origProj = cameraData.projMat;
						auto origViewProj = cameraData.viewProjMat;
						auto origProjUnj = cameraData.projMatrixUnjittered;
						auto origViewProjUnj = cameraData.viewProjMatrixUnjittered;

						auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
						std::uint32_t numViewports = 1;
						REX::W32::D3D11_VIEWPORT viewport;
						context->RSGetViewports(&numViewports, &viewport);

						const float origMinDepth = viewport.minDepth;
						const float minDepthShift = 1.0f - (origNear / clipDist);
						viewport.minDepth = origMinDepth + minDepthShift * (viewport.maxDepth - origMinDepth);
						context->RSSetViewports(1, &viewport);

						cameraData.projMat._43 = -clipDist;
						cameraData.viewProjMat = cameraData.viewMat * cameraData.projMat;
						cameraData.projMatrixUnjittered._43 = -clipDist;
						cameraData.viewProjMatrixUnjittered = cameraData.viewMat * cameraData.projMatrixUnjittered;

						UpdateCameraData();
						func(a_pass, a_technique, a_alphaTest, a_renderFlags);

						// Re-read current D3D viewport so we don't clobber any field
						// func() may have updated mid-pass; restore only minDepth.
						context->RSGetViewports(&numViewports, &viewport);
						viewport.minDepth = origMinDepth;
						context->RSSetViewports(1, &viewport);

						cameraData.projMat = origProj;
						cameraData.viewProjMat = origViewProj;
						cameraData.projMatrixUnjittered = origProjUnj;
						cameraData.viewProjMatrixUnjittered = origViewProjUnj;
						UpdateCameraData();
						return;
					}
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
		if (!a_this || !a_this->IsPlayerRef() || !a_this->Is3DLoaded()) {
			return;
		}

		auto camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		RE::NiPoint3 cameraPos = camera->GetRuntimeData2().pos;
		RE::NiPoint3 playerPos = a_this->GetPosition();
		playerPos.z += Settings::PlayerHeightOffset;

		auto* parentCell = a_this->GetParentCell();
		auto* bhkWorld = parentCell ? parentCell->GetbhkWorld() : nullptr;
		if (!bhkWorld || !IsFinite(cameraPos) || !IsFinite(playerPos)) {
			return;
		}

		if (auto* sky = RE::Sky::GetSingleton()) {
			if (auto* skyRoot = sky->root.get()) {
				const bool seeThroughActive = g_clipDistance.load(std::memory_order_acquire) > 0.0f;
				const bool shouldHide       = seeThroughActive && parentCell->IsInteriorCell();
				skyRoot->SetAppCulled(shouldHide);
			}
		}

		float scale = RE::bhkWorld::GetWorldScale();
		RE::CFilter colFilter;
		a_this->GetCollisionFilterInfo(colFilter);
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

		RE::NiPoint3 u = dir.Cross(RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
		if (const float uLen = u.Length(); uLen < 1e-3f) {
			u = { 1.0f, 0.0f, 0.0f };
		} else {
			u = u * (1.0f / uLen);
		}
		const RE::NiPoint3 vBase = dir.Cross(u);

		static std::uint32_t s_fanFrame = 0;
		constexpr float      kGoldenAngle = 2.39996323f;
		const float          phase        = s_fanFrame++ * kGoldenAngle;

		auto* selfRoot = a_this->Get3D();
		bool  hadHitThisFrame = false;
		constexpr int   kMaxHits     = 8;
		constexpr float kStepEpsilon = 2.0f;

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

		{
		RE::BSReadLockGuard worldLock(bhkWorld->worldLock);
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

				auto* refr = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
				RE::NiAVObject* root = refr ? refr->Get3D() : nullptr;
				if (!root) {
					root = RE::TESHavokUtilities::FindCollidableObject(*pick.rayOutput.rootCollidable);
				}
				const bool isFloorish = std::abs(pick.rayOutput.normal.Dot3({ 0.0f, 0.0f, 1.0f, 0.0f })) > 0.7f;

				const bool isLightFixture = refr && refr->GetBaseObject() &&
				                            refr->GetBaseObject()->As<RE::TESObjectLIGH>();

				if (!isFloorish && !isLightFixture && root && root != selfRoot && refr != a_this) {
					const auto colLayer = pick.rayOutput.rootCollidable->GetCollisionLayer();
					if (colLayer == RE::COL_LAYER::kStatic || colLayer == RE::COL_LAYER::kAnimStatic) {
						auto [it, inserted] = g_hitObjects.try_emplace(root, HitEntry{ RE::NiPointer<RE::NiAVObject>(root), 0.0f, hitDistFromCamera });
						auto& entry = it->second;
						entry.secondsSinceSeen = 0.0f;
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
		}

		std::erase_if(g_hitObjects, [](const auto& kv) {
			return kv.second.secondsSinceSeen > Settings::HitLingerSeconds;
		});

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

		static float s_secondsSinceHit = 1e6f;
		if (hadHitThisFrame) {
			s_secondsSinceHit = 0.0f;
		} else {
			s_secondsSinceHit += a_delta;
		}
		OcclusionStripper::SetStripped(s_secondsSinceHit < Settings::StripLingerSeconds);
	}

	namespace
	{
		// this is a grotesque reinterpret cast because the pointerlist in commonlib i dont think is correct
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

		constexpr float kStripRadius = 512.f;

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
				tmin = max(tmin, t1);
				tmax = min(tmax, t2);
				if (tmin > tmax) {
					return false;
				}
			}
			return true;
		}

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
					continue;
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


	struct HookQPointWithin
	{
		static bool thunk(RE::BSMultiBoundNode* a_this, RE::NiPoint3& a_point)
		{
			// obviously dangerous
			const bool result = func(a_this, a_point);
			if (g_clipDistance.load(std::memory_order_acquire) > 0.0f) {
				return true;
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;

		static void Install()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSMultiBoundRoom[0] };
			func = vtbl.write_vfunc(0x3F, thunk);
			logs::info("[install] BSMultiBoundRoom::QPointWithin hook installed at vtbl[0x3F]");
		}
	};

	OcclusionStripper* OcclusionStripper::GetSingleton()
	{
		static OcclusionStripper inst;
		return &inst;
	}

	namespace
	{
		void DisplaceForFrame()
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc) {
				return;
			}
			auto* cam = RE::PlayerCamera::GetSingleton();
			const RE::NiPoint3 segB = pc->GetPosition();
			const RE::NiPoint3 segA = cam ? cam->GetRuntimeData2().pos : segB;

			auto* cell = pc->GetParentCell();
			if (cell && !cell->IsInteriorCell()) {
				if (auto* loaded = cell->GetRuntimeData().loadedData) {
					if (auto* g = loaded->portalGraph.get()) {
						DisplaceList(reinterpret_cast<RawList*>(&g->occlusionShapes),
						             segA, segB, kStripRadius);
					}
				}
			}
			if (auto* ws = pc->GetWorldspace()) {
				if (auto* g = ws->portalGraph.get()) {
					DisplaceList(reinterpret_cast<RawList*>(&g->occlusionShapes),
					             segA, segB, kStripRadius);
				}
			}
		}
	}

	void OcclusionStripper::SetStripped(bool a_stripped)
	{
		constexpr float kRefreshInterval = 0.25f;
		static float*   g_DeltaTime      = (float*)RELOCATION_ID(523661, 410200).address();
		static float    s_secondsSinceRefresh = 0.0f;

		if (a_stripped == StrippedFlag()) {
			if (a_stripped) {
				s_secondsSinceRefresh += *g_DeltaTime;
				if (s_secondsSinceRefresh >= kRefreshInterval) {
					RestoreAllPositions();
					DisplaceForFrame();
					s_secondsSinceRefresh = 0.0f;
				}
			}
			return;
		}
		if (a_stripped) {
			DisplaceForFrame();
			StrippedFlag() = true;
			s_secondsSinceRefresh = 0.0f;
		} else {
			RestoreAllPositions();
			StrippedFlag() = false;
		}
	}

	RE::BSEventNotifyControl OcclusionStripper::ProcessEvent(
		const RE::TESCellFullyLoadedEvent*,
		RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
	{
		if (StrippedFlag()) {
			RestoreAllPositions();
			DisplaceForFrame();
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	RE::BSEventNotifyControl OcclusionStripper::ProcessEvent(
		const RE::TESLoadGameEvent*,
		RE::BSTEventSource<RE::TESLoadGameEvent>*)
	{
		g_publishedHits.store(nullptr, std::memory_order_release);
		g_clipDistance.store(0.0f, std::memory_order_release);
		RestoreAllPositions();
		StrippedFlag() = false;
		return RE::BSEventNotifyControl::kContinue;
	}

	void OcclusionStripper::Install()
	{
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
	}
}
