#include "WaterPatcher.h"
#include "LodWaterSplit.h"
#include "RenderPassFinder.h"
#include "WaterShader.h"
#include "WaterWatch.h"
#include <RE/I/ImageSpaceData.h>
#include <RE/I/ImageSpaceEffect.h>
#include <RE/I/ImageSpaceEffectParam.h>
#include <RE/I/ImageSpaceManager.h>
#include <RE/T/TESWaterSystem.h>
#include <algorithm>
#include <cstring>

namespace
{
	constexpr float kClearDofStrength = 0.75f;
	constexpr float kClearDofDistance = 50.0f;
	constexpr float kClearDofRange = 1000.0f;

	struct Hook_DepthOfFieldUpdateParams
	{
		static bool thunk(RE::ImageSpaceEffect* a_effect, RE::ImageSpaceEffectParam* a_param)
		{
			const auto& settings = Settings::GetFastSnapshot();
			auto*       manager = RE::ImageSpaceManager::GetSingleton();
			if (!settings.enabled || !settings.clearUnderwater || !manager) {
				return func(a_effect, a_param);
			}

			auto&      underwater = manager->GetRuntimeData().underwaterBaseData;
			auto*const authored = underwater;
			if (!authored) {
				return func(a_effect, a_param);
			}

			static thread_local RE::ImageSpaceBaseData clear;
			clear = *authored;
			auto& dof = clear.depthOfField;
			dof.strength = (std::min)(dof.strength, kClearDofStrength);
			dof.distance = (std::max)(dof.distance, kClearDofDistance);
			dof.range = (std::max)(dof.range, kClearDofRange);

			underwater = &clear;
			const bool result = func(a_effect, a_param);
			underwater = authored;
			return result;
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	constexpr std::uint32_t kWaterPassFirst = 0x5C000071;
	constexpr std::uint32_t kWaterPassLast = 0x5C006071;
	constexpr std::int32_t  kWaterWadingGroup = 0xA;
	constexpr std::uint32_t kFrameFlagFullWaterShading = 0x80;

	struct Hook_UpdatePerFrameConstants
	{
		static void thunk(void* a_renderer, std::uint32_t a_flags)
		{
			lastRenderer = a_renderer;
			lastFlags = a_flags;
			func(a_renderer, a_flags);
		}

		static inline REL::Relocation<decltype(thunk)> func;
		static inline void*         lastRenderer{ nullptr };
		static inline std::uint32_t lastFlags{ 0 };
	};

	struct Hook_RenderPassRange
	{
		static void thunk(void* a_accumulator, std::uint32_t a_firstPass, std::uint32_t a_lastPass, std::uint32_t a_flags, std::int32_t a_group)
		{
			bool        force = false;
			void* const renderer = Hook_UpdatePerFrameConstants::lastRenderer;
			const bool  isWaterPass = (a_firstPass == kWaterPassFirst && a_lastPass == kWaterPassLast) ||
			                          a_group == kWaterWadingGroup;
			if (isWaterPass && renderer &&
				!(Hook_UpdatePerFrameConstants::lastFlags & kFrameFlagFullWaterShading)) {
				const auto& settings = Settings::GetFastSnapshot();
				const auto* waterSystem = RE::TESWaterSystem::GetSingleton();
				force = settings.enabled && settings.realisticDistantWater && (!WaterShader::IsWorldMapOpen() || settings.worldMapRealisticDistantWater) && !(waterSystem && waterSystem->playerUnderwater);
			}

			if (force) {
				Hook_UpdatePerFrameConstants::func(renderer, Hook_UpdatePerFrameConstants::lastFlags | kFrameFlagFullWaterShading);
			}
			func(a_accumulator, a_firstPass, a_lastPass, a_flags, a_group);
			if (force) {
				Hook_UpdatePerFrameConstants::func(renderer, Hook_UpdatePerFrameConstants::lastFlags);
			}
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	std::optional<RenderPassFinder::Result> FindRenderPassFunctions()
	{
		const auto text = REL::Module::get().segment(REL::Segment::textx);
		struct Code
		{
			std::uintptr_t begin;
			std::uintptr_t end;
		} code{ text.address(), text.address() + text.size() };

		const RenderPassFinder::Memory memory{
			&code,
			[](void* a_code, std::uintptr_t a_address, void* a_out, std::size_t a_size) {
				const auto& c = *static_cast<const Code*>(a_code);
				if (a_address < c.begin || a_size > c.end - a_address) {
					return false;
				}
				std::memcpy(a_out, reinterpret_cast<const void*>(a_address), a_size);
				return true;
			},
			[](void* a_code, std::uintptr_t a_address, std::uintptr_t& a_begin, std::uintptr_t& a_end) {
				const auto& c = *static_cast<const Code*>(a_code);
				if (a_address < c.begin || a_address >= c.end) {
					return false;
				}
				DWORD64    imageBase = 0;
				const auto entry = RtlLookupFunctionEntry(a_address, &imageBase, nullptr);
				if (!entry) {
					return false;
				}
				a_begin = static_cast<std::uintptr_t>(imageBase + entry->BeginAddress);
				a_end = static_cast<std::uintptr_t>(imageBase + entry->EndAddress);
				return true;
			}
		};

		REL::Relocation<std::uintptr_t> accumulatorVtbl{ RE::VTABLE_BSShaderAccumulator[0] };
		const auto postResolveDepth = reinterpret_cast<const std::uintptr_t*>(accumulatorVtbl.address())[0x2B];
		REL::Relocation<std::uintptr_t> stateCameraUpdate{ RELOCATION_ID(75694, 77503) };

		const char* failure = "";
		const auto  found = RenderPassFinder::Find(memory, postResolveDepth, stateCameraUpdate.address(), &failure);
		const auto  base = REL::Module::get().base();
		if (found) {
			logger::info("Realistic Distant Water: found the pass-range draw at +0x{:X} and the per-frame constants update at +0x{:X} (accumulator pass +0x{:X}).",
				found->passRangeDraw - base, found->perFrameUpdate - base, found->accumulatorPass - base);
		} else {
			logger::error("Realistic Distant Water: could not find the pass-range draw and the per-frame constants update in the game's code ({}). Distant water stays vanilla.", failure);
		}
		return found;
	}
}

void WaterPatcher::Initialize()
{
	if (initialized.exchange(true)) {
		return;
	}

	logger::info("Initializing...");
	WaterShader::GetSingleton().Initialize();

	REL::Relocation<std::uintptr_t> dofVtbl{ RE::VTABLE_ImageSpaceEffectDepthOfField[0] };
	Hook_DepthOfFieldUpdateParams::func = dofVtbl.write_vfunc(7, Hook_DepthOfFieldUpdateParams::thunk);
	logger::info("Clear Underwater: hooked ImageSpaceEffectDepthOfField::UpdateParams (underwater lens blur is adjusted per frame; image space records untouched).");

	if (const auto renderPass = FindRenderPassFunctions()) {
		if (Detour::Function<Hook_UpdatePerFrameConstants>(renderPass->perFrameUpdate) &&
			Detour::Function<Hook_RenderPassRange>(renderPass->passRangeDraw)) {
			logger::info("Realistic Distant Water: hooked the per-frame constants and the pass-range draw (water keeps its full shading past 8192 units).");
		} else {
			logger::error("Realistic Distant Water: failed to hook the per-frame constants or the pass-range draw (another mod may have patched them). Distant water stays vanilla.");
		}
	}

	LodWaterSplit::Install();

	RefreshWaterSettings();
}

void WaterPatcher::RefreshWaterSettings(bool a_silent)
{
	WaterWatch::Scope watchScope("RefreshWaterSettings");

	if (!a_silent) {
		const auto& settings = Settings::GetSingleton();
		logger::info("Water settings applied at draw time (records untouched): Enabled={}, WaterDepth={}, Transparency={:.0f}%, Refl={:.2f}, Refr={:.2f}, Norm={:.2f}, Spec={:.2f}, CustomColors={}, ClearUnderwater={}, FullShaderDistantWater={}, DisableRiverFlow={}, RealisticDistantWater={}, RemoveSunGlints={}",
			settings.enabled, settings.fixDepthProperties, settings.waterTransparency,
			settings.depthReflections, settings.depthRefraction, settings.depthNormals, settings.depthSpecularLighting,
			settings.hasCustomColors, settings.clearUnderwater, settings.fullShaderDistantWater, settings.disableRiverFlow, settings.realisticDistantWater, settings.removeSunGlints);
	}
}
