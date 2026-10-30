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
#include <chrono>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <mutex>
#include <numbers>
#include <vector>

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

	constexpr float kSplashRippleScale = 0.01f;
	constexpr float kSplashDabSpacing = 8.0f;
	constexpr float kSplashSizeHeavy = 50.0f;
	constexpr auto  kRippleStep = std::chrono::milliseconds(50);
	constexpr std::size_t kMaxPendingSplashes = 32;

	struct PendingSplash
	{
		RE::NiPoint3                          pos;
		float                                 radius{ 0.0f };
		std::chrono::steady_clock::time_point discTime{};
		bool                                  discDone{ false };
	};

	std::mutex                 splashLock;
	std::vector<PendingSplash> pendingSplashes;
	std::atomic<bool>          hasPendingSplashes{ false };
	std::uintptr_t             actorSplashReturn{ 0 };

	std::uintptr_t FindActorSplashReturn()
	{
		const auto splash = REL::RelocationID(31297, 32081).address();
		const auto actorWater = REL::RelocationID(36357, 37348).address();

		DWORD64    imageBase = 0;
		const auto entry = RtlLookupFunctionEntry(actorWater, &imageBase, nullptr);
		if (!entry || imageBase + entry->BeginAddress != actorWater) {
			return 0;
		}
		const auto end = static_cast<std::uintptr_t>(imageBase + entry->EndAddress);

		std::uintptr_t found = 0;
		for (auto address = actorWater; address + 5 <= end; ++address) {
			if (*reinterpret_cast<const std::uint8_t*>(address) != 0xE8) {
				continue;
			}
			std::int32_t displacement;
			std::memcpy(&displacement, reinterpret_cast<const void*>(address + 1), sizeof(displacement));
			if (address + 5 + static_cast<std::intptr_t>(displacement) != splash) {
				continue;
			}
			if (found) {
				return 0;
			}
			found = address + 5;
		}
		return found;
	}

	struct Hook_WaterSplash
	{
		static void thunk(float a_size, RE::NiPoint3* a_pos)
		{
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			func(a_size, a_pos);

			const auto& settings = Settings::GetFastSnapshot();
			if (caller != actorSplashReturn || !a_pos || !settings.enabled || !settings.splashRipples || !(a_size > 0.0f)) {
				return;
			}
			const float weight = std::clamp(a_size / kSplashSizeHeavy, 0.1f, 1.5f);
			const float radius = 8.0f + 28.0f * weight;

			std::scoped_lock lock(splashLock);
			if (pendingSplashes.size() < kMaxPendingSplashes) {
				pendingSplashes.push_back({ *a_pos, radius });
				hasPendingSplashes.store(true, std::memory_order_release);
			}
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	void AddSplashDab(RE::TESWaterSystem* a_system, const RE::NiPoint3& a_center, float a_x, float a_y)
	{
		a_system->AddRipple(RE::NiPoint3{ a_center.x + a_x, a_center.y + a_y, a_center.z }, kSplashRippleScale);
	}

	void AddSplashDisc(RE::TESWaterSystem* a_system, const RE::NiPoint3& a_center, float a_radius)
	{
		const int steps = static_cast<int>(a_radius / kSplashDabSpacing);
		for (int iy = -steps; iy <= steps; ++iy) {
			for (int ix = -steps; ix <= steps; ++ix) {
				const float x = static_cast<float>(ix) * kSplashDabSpacing;
				const float y = static_cast<float>(iy) * kSplashDabSpacing;
				if (x * x + y * y <= a_radius * a_radius) {
					AddSplashDab(a_system, a_center, x, y);
				}
			}
		}
	}

	void AddSplashRing(RE::TESWaterSystem* a_system, const RE::NiPoint3& a_center, float a_radius)
	{
		const int count = std::clamp(static_cast<int>(std::ceil(2.0f * std::numbers::pi_v<float> * a_radius / kSplashDabSpacing)), 8, 96);
		for (int i = 0; i < count; ++i) {
			const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(count);
			AddSplashDab(a_system, a_center, a_radius * std::cos(angle), a_radius * std::sin(angle));
		}
	}
}

void WaterPatcher::OnFrame()
{
	if (!hasPendingSplashes.load(std::memory_order_acquire)) {
		return;
	}

	std::vector<PendingSplash> due;
	{
		std::scoped_lock lock(splashLock);
		if (pendingSplashes.empty()) {
			hasPendingSplashes.store(false, std::memory_order_relaxed);
			return;
		}
		due.swap(pendingSplashes);
		hasPendingSplashes.store(false, std::memory_order_relaxed);
	}

	auto* system = RE::TESWaterSystem::GetSingleton();
	if (!system || !RE::PlayerCharacter::GetSingleton()) {
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	std::vector<PendingSplash> later;
	for (auto& splash : due) {
		if (!splash.discDone) {
			AddSplashDisc(system, splash.pos, splash.radius * 0.5f);
			splash.discDone = true;
			splash.discTime = now;
			later.push_back(splash);
		} else if (now - splash.discTime >= kRippleStep) {
			AddSplashRing(system, splash.pos, splash.radius);
		} else {
			later.push_back(splash);
		}
	}

	if (!later.empty()) {
		std::scoped_lock lock(splashLock);
		for (auto& splash : later) {
			if (pendingSplashes.size() < kMaxPendingSplashes) {
				pendingSplashes.push_back(splash);
			}
		}
		if (!pendingSplashes.empty()) {
			hasPendingSplashes.store(true, std::memory_order_release);
		}
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

	actorSplashReturn = FindActorSplashReturn();
	if (!actorSplashReturn) {
		logger::error("Splash Ripples: could not find the actor water-entry splash call in the game's code. Splashes keep the vanilla ripple.");
	} else if (Detour::Function<Hook_WaterSplash>(REL::RelocationID(31297, 32081).address())) {
		logger::info("Splash Ripples: hooked the water splash; actors jumping or falling into water (call returning to +0x{:X}) add ripple rings through TESWaterSystem::AddRipple.",
			actorSplashReturn - REL::Module::get().base());
	} else {
		logger::error("Splash Ripples: failed to hook the water splash (another mod may have patched it). Splashes keep the vanilla ripple.");
	}

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
		logger::info("Water settings applied at draw time (records untouched): Enabled={}, WaterDepth={}, Transparency={:.0f}%, Refl={:.2f}, Refr={:.2f}, Norm={:.2f}, Spec={:.2f}, CustomColors={}, ClearUnderwater={}, FullShaderDistantWater={}, DisableRiverFlow={}, SplashRipples={}, RealisticDistantWater={}, RemoveSunGlints={}",
			settings.enabled, settings.fixDepthProperties, settings.waterTransparency,
			settings.depthReflections, settings.depthRefraction, settings.depthNormals, settings.depthSpecularLighting,
			settings.hasCustomColors, settings.clearUnderwater, settings.fullShaderDistantWater, settings.disableRiverFlow, settings.splashRipples, settings.realisticDistantWater, settings.removeSunGlints);
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
