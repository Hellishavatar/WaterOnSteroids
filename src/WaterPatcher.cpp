#include "WaterPatcher.h"
#include "LodWaterSplit.h"
#include "WaterShader.h"
#include "WaterWatch.h"
#include <RE/I/ImageSpaceData.h>
#include <RE/I/ImageSpaceEffect.h>
#include <RE/I/ImageSpaceEffectParam.h>
#include <RE/I/ImageSpaceManager.h>
#include <algorithm>

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
