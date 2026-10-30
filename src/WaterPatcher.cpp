#include "WaterPatcher.h"
#include "LodWaterSplit.h"
#include "WaterShader.h"
#include "WaterWatch.h"
#include <RE/B/BSWaterShaderMaterial.h>
#include <RE/B/BSWaterShaderProperty.h>
#include <RE/T/TESWaterSystem.h>
#include <RE/T/TESWaterObject.h>
#include <RE/T/TES.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESDataHandler.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/E/ExtraCellWaterType.h>
#include <RE/I/ImageSpaceData.h>
#include <RE/I/ImageSpaceEffect.h>
#include <RE/I/ImageSpaceEffectParam.h>
#include <RE/I/ImageSpaceManager.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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

static std::string GetWaterName(RE::TESWaterForm* a_water)
{
	if (!a_water) return "<Null Water>";

	std::string baseName = Util::GetEditorID(a_water);
	if (baseName.empty()) {
		baseName = std::format("Water_{:08X}", a_water->GetFormID());
	}

	auto winningFile = a_water->GetFile(-1);
	if (winningFile && winningFile->fileName && winningFile->fileName[0] != '\0') {
		return std::format("{} [{}]", baseName, winningFile->fileName);
	}

	return baseName;
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

bool WaterPatcher::CaptureActiveWaterColors()
{
	WaterWatch::Scope watchScope("CaptureActiveWaterColors");
	auto& settings = Settings::GetSingleton();
	if (!settings.enabled) {
		return false;
	}

	RE::TESWaterForm* targetWater = nullptr;
	if (auto waterSystem = RE::TESWaterSystem::GetSingleton()) {
		targetWater = waterSystem->currentWaterType;
	}

	if (!targetWater) {
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			if (auto cell = player->GetParentCell()) {
				if (auto extraWater = cell->extraList.GetByType<RE::ExtraCellWaterType>()) {
					targetWater = extraWater->water;
				}
				if (!targetWater) {
					for (auto world = cell->GetRuntimeData().worldSpace; world != nullptr;
						world = world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseWaterData) ? world->parentWorld : nullptr) {
						if (world->worldWater) {
							targetWater = world->worldWater;
							break;
						}
					}
				}
			}
		}
	}

	if (!targetWater) {
		if (auto dataHandler = RE::TESDataHandler::GetSingleton()) {
			targetWater = dataHandler->LookupForm<RE::TESWaterForm>(0x18, "Skyrim.esm");
		}
	}

	if (!targetWater) {
		logger::warn("CaptureActiveWaterColors: Failed to identify any active water form.");
		return false;
	}

	settings.shallowColor[0] = targetWater->data.shallowWaterColor.red / 255.0f;
	settings.shallowColor[1] = targetWater->data.shallowWaterColor.green / 255.0f;
	settings.shallowColor[2] = targetWater->data.shallowWaterColor.blue / 255.0f;

	settings.deepColor[0] = targetWater->data.deepWaterColor.red / 255.0f;
	settings.deepColor[1] = targetWater->data.deepWaterColor.green / 255.0f;
	settings.deepColor[2] = targetWater->data.deepWaterColor.blue / 255.0f;

	settings.reflectionColor[0] = targetWater->data.reflectionWaterColor.red / 255.0f;
	settings.reflectionColor[1] = targetWater->data.reflectionWaterColor.green / 255.0f;
	settings.reflectionColor[2] = targetWater->data.reflectionWaterColor.blue / 255.0f;

	auto winningFile = targetWater->GetFile(-1);
	std::string modName = winningFile ? std::string(winningFile->GetFilename()) : "Skyrim.esm";
	logger::info("Sampled water colors from [0x{:08X}] '{}' (Source: '{}'): Shallow=({:.2f},{:.2f},{:.2f}), Deep=({:.2f},{:.2f},{:.2f}), Refl=({:.2f},{:.2f},{:.2f})",
		targetWater->GetFormID(), GetWaterName(targetWater), modName,
		settings.shallowColor[0], settings.shallowColor[1], settings.shallowColor[2],
		settings.deepColor[0], settings.deepColor[1], settings.deepColor[2],
		settings.reflectionColor[0], settings.reflectionColor[1], settings.reflectionColor[2]);

	settings.UpdateSnapshot();

	return true;
}
