#pragma once

#include "PCH.h"
#include "Settings.h"
#include <atomic>
#include <optional>

class WaterShader
{
public:
	static WaterShader& GetSingleton()
	{
		static WaterShader instance;
		return instance;
	}

	bool Initialize();

	static bool IsMaterialOverrideActive(const SettingsSnapshot& a_settings);

	struct MeshHeights
	{
		float         minZ{ 0.0f };
		float         maxZ{ 0.0f };
		std::uint32_t vertexCount{ 0 };
		bool          fromBound{ false };
	};
	[[nodiscard]] static std::optional<MeshHeights> MeasureMeshHeights(RE::BSGeometry* a_geometry);

	[[nodiscard]] static std::optional<RE::NiPlane> DistantWaterPlane(RE::BSGeometry* a_geometry, const RE::NiPlane& a_stored);

	[[nodiscard]] static RE::NiPointer<RE::TESWaterObject> FindMatchingCloseWater(RE::BSGeometry* a_distant);

	using SetupTechnique_t = bool (*)(RE::BSShader*, std::uint32_t);
	static bool Hooked_SetupTechnique(RE::BSShader* a_shader, std::uint32_t a_technique);

	using SetupMaterial_t = void (*)(RE::BSShader*, const RE::BSShaderMaterial*);
	static void Hooked_SetupMaterial(RE::BSShader* a_shader, const RE::BSShaderMaterial* a_material);

	using SetupGeometry_t = void (*)(RE::BSShader*, RE::BSRenderPass*, std::uint32_t);
	static void Hooked_SetupGeometry(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_flags);

private:
	WaterShader() = default;
	WaterShader(const WaterShader&) = delete;
	WaterShader& operator=(const WaterShader&) = delete;

	std::atomic<bool> hookInstalled{ false };

	static inline SetupTechnique_t originalSetupTechnique{ nullptr };
	static inline SetupMaterial_t originalSetupMaterial{ nullptr };
	static inline SetupGeometry_t originalSetupGeometry{ nullptr };
};
