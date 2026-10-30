#include "WaterShader.h"
#include "CellWaterMap.h"
#include "LodWaterSplit.h"
#include "Settings.h"
#include "TypeMaterial.h"
#include "WaterWatch.h"

#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSWaterShaderMaterial.h>
#include <RE/B/BSWaterShaderProperty.h>
#include <RE/T/TES.h>
#include <RE/T/TESWaterSystem.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
    static_assert(sizeof(RE::BSWaterShaderMaterial) == 0x180, "Unexpected BSWaterShaderMaterial size for memcpy scratch buffer");
    static_assert(alignof(RE::BSWaterShaderMaterial) <= alignof(std::max_align_t), "Unexpected BSWaterShaderMaterial alignment");

    thread_local const RE::BSShaderMaterial* currentSource = nullptr;
    thread_local std::uint32_t loadedTechnique = 0;
    thread_local bool distantWaterUpgraded = false;
    thread_local bool riverFlowStripped = false;

    bool IsLodWaterGeometry(const RE::BSGeometry* a_geometry)
    {
        if (!a_geometry) {
            return false;
        }
        auto tes = RE::TES::GetSingleton();
        if (tes && tes->objLODWaterRoot) {
            auto* lodRoot = tes->objLODWaterRoot;
            int depth = 0;
            for (auto* parent = a_geometry->parent; parent != nullptr && depth < 4; parent = parent->parent, ++depth) {
                if (parent == lodRoot) {
                    return true;
                }
            }
        }
        return false;
    }

    constexpr std::uint32_t kWaterTechniqueBase = 0x5C000071;
    constexpr std::uint32_t kWaterTechniqueEnd = 0x5C006071;
    constexpr std::uint32_t kTechniqueShift = 11;
    constexpr std::uint32_t kTechniqueMask = 0xF;
    constexpr std::uint32_t kTechniqueMain = 0;
    constexpr std::uint32_t kTechniqueLod = 9;
    constexpr std::uint32_t kFlagVertexColor = 1u << 0;
    constexpr std::uint32_t kFlagReflections = 1u << 2;
    constexpr std::uint32_t kFlagRefractions = 1u << 3;
    constexpr std::uint32_t kFlagDepth = 1u << 4;
    constexpr std::uint32_t kFlagCubemap = 1u << 8;
    constexpr std::uint32_t kFlagFlowmap = 1u << 9;

    std::uint32_t UpgradeLodWaterTechnique(std::uint32_t a_technique)
    {
        if (a_technique < kWaterTechniqueBase || a_technique >= kWaterTechniqueEnd) {
            return a_technique;
        }
        const std::uint32_t descriptor = a_technique - kWaterTechniqueBase;
        if (((descriptor >> kTechniqueShift) & kTechniqueMask) != kTechniqueLod) {
            return a_technique;
        }
        const std::uint32_t kept = descriptor & (kFlagVertexColor | kFlagReflections | kFlagCubemap);
        return kWaterTechniqueBase + (kept | kFlagRefractions | kFlagDepth);
    }

    std::uint32_t StripRiverFlowTechnique(std::uint32_t a_technique)
    {
        if (a_technique < kWaterTechniqueBase || a_technique >= kWaterTechniqueEnd) {
            return a_technique;
        }
        const std::uint32_t descriptor = a_technique - kWaterTechniqueBase;
        if (((descriptor >> kTechniqueShift) & kTechniqueMask) != kTechniqueMain || (descriptor & kFlagFlowmap) == 0) {
            return a_technique;
        }
        return kWaterTechniqueBase + (descriptor & ~kFlagFlowmap);
    }

    void ApplyWaterColorOverrides(
        RE::BSWaterShaderMaterial* a_target,
        const RE::BSWaterShaderMaterial* a_source,
        const SettingsSnapshot& a_settings)
    {
        if (!a_settings.hasCustomColors || !a_target || !a_source) {
            return;
        }

        const auto origShallow = a_source->shallowWaterColor;
        const auto origDeep = a_source->deepWaterColor;
        const auto origRefl = a_source->reflectionColor;

        if (a_settings.colorMode == 0) {
            a_target->shallowWaterColor.red = std::clamp(a_settings.shallowColor[0], 0.0f, 3.0f);
            a_target->shallowWaterColor.green = std::clamp(a_settings.shallowColor[1], 0.0f, 3.0f);
            a_target->shallowWaterColor.blue = std::clamp(a_settings.shallowColor[2], 0.0f, 3.0f);

            a_target->deepWaterColor.red = std::clamp(a_settings.deepColor[0], 0.0f, 3.0f);
            a_target->deepWaterColor.green = std::clamp(a_settings.deepColor[1], 0.0f, 3.0f);
            a_target->deepWaterColor.blue = std::clamp(a_settings.deepColor[2], 0.0f, 3.0f);

            a_target->reflectionColor.red = std::clamp(a_settings.reflectionColor[0], 0.0f, 3.0f);
            a_target->reflectionColor.green = std::clamp(a_settings.reflectionColor[1], 0.0f, 3.0f);
            a_target->reflectionColor.blue = std::clamp(a_settings.reflectionColor[2], 0.0f, 3.0f);
        } else {
            a_target->shallowWaterColor.red = std::clamp(origShallow.red * a_settings.shallowColorScale[0], 0.0f, 3.0f);
            a_target->shallowWaterColor.green = std::clamp(origShallow.green * a_settings.shallowColorScale[1], 0.0f, 3.0f);
            a_target->shallowWaterColor.blue = std::clamp(origShallow.blue * a_settings.shallowColorScale[2], 0.0f, 3.0f);

            a_target->deepWaterColor.red = std::clamp(origDeep.red * a_settings.deepColorScale[0], 0.0f, 3.0f);
            a_target->deepWaterColor.green = std::clamp(origDeep.green * a_settings.deepColorScale[1], 0.0f, 3.0f);
            a_target->deepWaterColor.blue = std::clamp(origDeep.blue * a_settings.deepColorScale[2], 0.0f, 3.0f);

            a_target->reflectionColor.red = std::clamp(origRefl.red * a_settings.reflectionColorScale[0], 0.0f, 3.0f);
            a_target->reflectionColor.green = std::clamp(origRefl.green * a_settings.reflectionColorScale[1], 0.0f, 3.0f);
            a_target->reflectionColor.blue = std::clamp(origRefl.blue * a_settings.reflectionColorScale[2], 0.0f, 3.0f);
        }
    }

    constexpr float kAuthoredTransparency = 0.0f;

    bool HasTransparencyOverride(const SettingsSnapshot& a_settings)
    {
        return std::abs(a_settings.waterTransparency - kAuthoredTransparency) > 0.01f;
    }

    void ApplyTransparency(RE::BSWaterShaderMaterial* a_mat, float a_percent)
    {
        constexpr float kMinDistance = 1.0f;
        constexpr float kMaxDistance = 1.0e6f;

        const float t = 0.5f + std::clamp(a_percent, 0.0f, 100.0f) / 200.0f;
        const float authoredFar = (std::max)(a_mat->aboveWaterFogDistFar, kMinDistance);
        const float authoredNear = authoredFar - a_mat->unk15C;
        const float ratio = t / (1.0f - t);
        const float scale = t >= 1.0f ? kMaxDistance / authoredFar : ratio * ratio;

        const float farPlane = std::clamp(authoredFar * scale, kMinDistance, kMaxDistance);
        float       nearPlane = authoredNear * scale;
        nearPlane = std::lerp(nearPlane, (std::max)(nearPlane, 0.0f), (t - 0.5f) * 2.0f);
        nearPlane = (std::min)(nearPlane, farPlane - kMinDistance);

        a_mat->aboveWaterFogDistFar = farPlane;
        a_mat->unk15C = (std::max)(farPlane - nearPlane, kMinDistance);
    }

    RE::BSWaterShaderMaterial* BuildOverrideMaterial(
        const RE::BSWaterShaderMaterial* a_source,
        const SettingsSnapshot& a_settings)
    {
        alignas(RE::BSWaterShaderMaterial) static thread_local std::byte scratch[sizeof(RE::BSWaterShaderMaterial)];
        std::memcpy(scratch, a_source, sizeof(RE::BSWaterShaderMaterial));
        auto* waterMat = reinterpret_cast<RE::BSWaterShaderMaterial*>(scratch);

        if (a_settings.fixDepthProperties) {
            waterMat->depthProperties.reflections = a_settings.depthReflections;
            waterMat->depthProperties.refraction = a_settings.depthRefraction;
            waterMat->depthProperties.normals = a_settings.depthNormals;
            waterMat->depthProperties.specularLighting = a_settings.depthSpecularLighting;
        }

        if (HasTransparencyOverride(a_settings)) {
            ApplyTransparency(waterMat, a_settings.waterTransparency);
        }

        ApplyWaterColorOverrides(waterMat, a_source, a_settings);

        if (a_settings.removeSunGlints) {
            waterMat->sunSparkleMagnitude = 0.0f;
        }

        return waterMat;
    }

    constexpr float kSeaLevelTolerance = 2.0f;

    RE::TESWaterForm* SeaWaterAt(const RE::TESWaterForm* a_type, float a_height)
    {
        const auto sea = CellWaterMap::DefaultWater();
        if (!sea || sea->second == a_type || std::abs(a_height - sea->first) > kSeaLevelTolerance) {
            return nullptr;
        }
        return sea->second;
    }
}

bool WaterShader::IsMaterialOverrideActive(const SettingsSnapshot& a_settings)
{
    return a_settings.enabled && (
        a_settings.hasCustomColors ||
        a_settings.fixDepthProperties ||
        a_settings.removeSunGlints ||
        HasTransparencyOverride(a_settings));
}

float WaterShader::PieceHeight(const RE::BSGeometry* a_geometry, const LodWaterSplit::Piece& a_piece)
{
    const auto& world = a_geometry->world;
    return world.translate.z + world.scale * world.rotate.entry[2][2] * a_piece.localZ;
}

std::optional<RE::NiPlane> WaterShader::DistantWaterPlane(RE::BSGeometry* a_geometry, const RE::NiPlane& a_stored)
{
    if (!a_geometry) {
        return std::nullopt;
    }
    const auto  piece = LodWaterSplit::PieceFor(a_geometry);
    const float height = piece ? PieceHeight(a_geometry, *piece) : a_geometry->worldBound.center.z;
    if (std::abs(a_stored.normal.x) < 1e-4f && std::abs(a_stored.normal.y) < 1e-4f && std::abs(a_stored.normal.z - 1.0f) < 1e-4f &&
        std::abs(a_stored.constant - height) < 1.0f) {
        return std::nullopt;
    }
    return RE::NiPlane{ { 0.0f, 0.0f, 1.0f }, height };
}

bool WaterShader::Hooked_SetupTechnique(RE::BSShader* a_shader, std::uint32_t a_technique)
{
    if (!originalSetupTechnique) {
        return false;
    }

    std::uint32_t technique = a_technique;

    const auto& settings = Settings::GetFastSnapshot();
    if (settings.enabled && settings.fullShaderDistantWater) {
        const auto* waterSystem = RE::TESWaterSystem::GetSingleton();
        if (!waterSystem || !waterSystem->playerUnderwater) {
            technique = UpgradeLodWaterTechnique(a_technique);
        }
    }

    if (technique != a_technique) {
        static std::atomic<bool> logged{ false };
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            logger::info("WaterShader: drawing distant water technique 0x{:08X} with full water technique 0x{:08X}.", a_technique, technique);
        }
    }
    distantWaterUpgraded = technique != a_technique;

    riverFlowStripped = false;
    if (settings.enabled && settings.IsRiverFlowDisabled()) {
        const std::uint32_t still = StripRiverFlowTechnique(technique);
        riverFlowStripped = still != technique;
        if (riverFlowStripped) {
            static std::atomic<bool> logged{ false };
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                logger::info("WaterShader: drawing river water technique 0x{:08X} without its flow map as 0x{:08X}.", technique, still);
            }
        }
        technique = still;
    }

    loadedTechnique = technique;
    return originalSetupTechnique(a_shader, technique);
}

void WaterShader::Hooked_SetupMaterial(RE::BSShader* a_shader, const RE::BSShaderMaterial* a_material)
{
    if (!originalSetupMaterial) {
        return;
    }

    if (WaterWatch::IsEnabled()) {
        WaterWatch::NoteSetupMaterial(a_material);
    }

    currentSource = nullptr;

    const auto& settings = Settings::GetFastSnapshot();
    if (a_material && a_material->GetType() == RE::BSShaderMaterial::Type::kWater) {
        currentSource = a_material;
        if (IsMaterialOverrideActive(settings)) {
            originalSetupMaterial(a_shader, BuildOverrideMaterial(static_cast<const RE::BSWaterShaderMaterial*>(a_material), settings));
        } else {
            originalSetupMaterial(a_shader, a_material);
        }
        return;
    }

    originalSetupMaterial(a_shader, a_material);
}

void WaterShader::Hooked_SetupGeometry(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
{
    if (!originalSetupGeometry) {
        return;
    }

    bool rebuilt = false;
    RE::BSShaderMaterial*             matchedMaterial = nullptr;
    RE::BSWaterShaderProperty*        waterShaderProp = nullptr;
    bool                              riverAtSeaLevel = false;

    if (a_pass && a_pass->shaderProperty && a_pass->shaderProperty->material &&
        a_pass->shaderProperty->material->GetType() == RE::BSShaderMaterial::Type::kWater) {
        waterShaderProp = static_cast<RE::BSWaterShaderProperty*>(a_pass->shaderProperty);
        const auto* passMat = waterShaderProp->material;

        if (originalSetupMaterial && a_pass->geometry) {
            const auto& settings = Settings::GetFastSnapshot();
            const bool  overrideActive = IsMaterialOverrideActive(settings);
            const bool  matching = settings.enabled && settings.fullShaderDistantWater;

            const RE::BSShaderMaterial* source = passMat;
            const bool distant = matching && IsLodWaterGeometry(a_pass->geometry);
            if (distant) {
                if (const auto piece = LodWaterSplit::PieceFor(a_pass->geometry)) {
                    auto* type = piece->type;
                    if (settings.IsRiverFlowDisabled() && type->flags.any(RE::TESWaterForm::Flag::kEnableFlowmap)) {
                        if (auto* sea = SeaWaterAt(type, PieceHeight(a_pass->geometry, *piece))) {
                            type = sea;
                        }
                    }
                    if (auto* own = TypeMaterial::For(type)) {
                        source = own;
                        matchedMaterial = own;
                    }
                }
            } else if (matching && riverFlowStripped && std::abs(waterShaderProp->plane.normal.z - 1.0f) < 1e-3f) {
                if (auto* sea = SeaWaterAt(nullptr, waterShaderProp->plane.constant)) {
                    if (auto* own = TypeMaterial::For(sea)) {
                        source = own;
                        matchedMaterial = own;
                        riverAtSeaLevel = true;
                    }
                }
            }

            if ((overrideActive || matching) && source != currentSource) {
                currentSource = source;
                const auto* water = static_cast<const RE::BSWaterShaderMaterial*>(source);
                originalSetupMaterial(a_shader, overrideActive ? BuildOverrideMaterial(water, settings) : source);
                rebuilt = true;
            }
        }
    }

    if (WaterWatch::IsEnabled() && a_pass && a_pass->geometry) {
        WaterWatch::NoteDraw(a_pass, loadedTechnique ? loadedTechnique : a_pass->passEnum, IsLodWaterGeometry(a_pass->geometry), rebuilt);
    }

    RE::BSWaterShaderProperty* planeProperty = nullptr;
    RE::NiPlane                storedPlane{};
    if (distantWaterUpgraded && waterShaderProp && a_pass->geometry) {
        if (const auto plane = DistantWaterPlane(a_pass->geometry, waterShaderProp->plane)) {
            planeProperty = waterShaderProp;
            storedPlane = waterShaderProp->plane;
            waterShaderProp->plane = *plane;
        }
    }

    RE::BSWaterShaderProperty* flowProperty = nullptr;
    if (riverFlowStripped && waterShaderProp &&
        waterShaderProp->waterFlags.any(RE::BSWaterShaderProperty::WaterFlag::kEnableFlowmap)) {
        flowProperty = waterShaderProp;
        waterShaderProp->waterFlags.reset(RE::BSWaterShaderProperty::WaterFlag::kEnableFlowmap);
    }

    RE::BSWaterShaderProperty* textureProperty = nullptr;
    RE::BSShaderMaterial*      storedMaterial = nullptr;
    if ((distantWaterUpgraded || riverAtSeaLevel) && waterShaderProp && matchedMaterial && waterShaderProp->material != matchedMaterial) {
        textureProperty = waterShaderProp;
        storedMaterial = waterShaderProp->material;
        waterShaderProp->material = matchedMaterial;
    }

    static_assert(sizeof(RE::NiPointer<RE::NiSourceTexture>) == sizeof(RE::NiSourceTexture*));
    RE::NiPointer<RE::NiSourceTexture>* lentSlots[2]{};
    if (auto* lender = flowProperty ? flowProperty : textureProperty) {
        auto* water = static_cast<RE::BSWaterShaderMaterial*>(lender->material);
        RE::NiSourceTexture* first = water->normalTexture1.get();
        if (first) {
            int lent = 0;
            for (auto* slot : { &water->normalTexture2, &water->normalTexture3 }) {
                if (!*slot) {
                    std::memcpy(slot, &first, sizeof(first));
                    lentSlots[lent++] = slot;
                }
            }
        }
    }

    originalSetupGeometry(a_shader, a_pass, a_flags);

    for (auto* slot : lentSlots) {
        if (slot) {
            RE::NiSourceTexture* none = nullptr;
            std::memcpy(slot, &none, sizeof(none));
        }
    }
    if (textureProperty) {
        textureProperty->material = storedMaterial;
    }
    if (flowProperty) {
        flowProperty->waterFlags.set(RE::BSWaterShaderProperty::WaterFlag::kEnableFlowmap);
    }
    if (planeProperty) {
        planeProperty->plane = storedPlane;
    }
}

bool WaterShader::Initialize()
{
    bool expected = false;
    if (hookInstalled.compare_exchange_strong(expected, true)) {
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSWaterShader[0] };
        originalSetupTechnique = reinterpret_cast<SetupTechnique_t>(vtbl.write_vfunc(2, &Hooked_SetupTechnique));
        originalSetupMaterial = reinterpret_cast<SetupMaterial_t>(vtbl.write_vfunc(4, &Hooked_SetupMaterial));
        originalSetupGeometry = reinterpret_cast<SetupGeometry_t>(vtbl.write_vfunc(6, &Hooked_SetupGeometry));
        logger::info("WaterShader: hooked BSWaterShader::SetupTechnique vtable[2] (original: 0x{:X}), SetupMaterial vtable[4] (original: 0x{:X}) and SetupGeometry vtable[6] (original: 0x{:X}).",
            reinterpret_cast<std::uintptr_t>(originalSetupTechnique), reinterpret_cast<std::uintptr_t>(originalSetupMaterial), reinterpret_cast<std::uintptr_t>(originalSetupGeometry));
    }

    return hookInstalled.load();
}
