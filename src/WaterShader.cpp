#include "WaterShader.h"
#include "CellWaterMap.h"
#include "LodWaterSplit.h"
#include "Settings.h"
#include "WaterWatch.h"

#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSVisit.h>
#include <RE/B/BSTriShape.h>
#include <RE/B/BSWaterShaderMaterial.h>
#include <RE/B/BSWaterShaderProperty.h>
#include <RE/G/GridCellArray.h>
#include <RE/N/NiSkinPartition.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TES.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESWaterSystem.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace
{
    static_assert(sizeof(RE::BSWaterShaderMaterial) == 0x180, "Unexpected BSWaterShaderMaterial size for memcpy scratch buffer");
    static_assert(alignof(RE::BSWaterShaderMaterial) <= alignof(std::max_align_t), "Unexpected BSWaterShaderMaterial alignment");

    thread_local const RE::BSShaderMaterial* currentSource = nullptr;
    thread_local float currentSolid = 0.0f;
    thread_local std::uint32_t loadedTechnique = 0;
    thread_local bool distantWaterUpgraded = false;
    thread_local bool riverFlowStripped = false;

    float HalfToFloat(std::uint16_t a_half)
    {
        const std::uint32_t sign = static_cast<std::uint32_t>(a_half & 0x8000u) << 16;
        std::uint32_t       exponent = (a_half >> 10) & 0x1Fu;
        std::uint32_t       mantissa = a_half & 0x3FFu;
        if (exponent == 0) {
            if (mantissa == 0) {
                return std::bit_cast<float>(sign);
            }
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3FFu;
            return std::bit_cast<float>(sign | (exponent << 23) | (mantissa << 13));
        }
        if (exponent == 0x1F) {
            return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
        }
        return std::bit_cast<float>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
    }

    struct LocalHeights
    {
        const void*   rawVertexData{ nullptr };
        std::uint32_t count{ 0 };
        RE::NiBound   bound{};
        bool          valid{ false };
        bool          fromBound{ false };
        float         minZ{ 0.0f };
        float         maxZ{ 0.0f };
    };

    thread_local std::unordered_map<const void*, LocalHeights> heightCache;

    bool DecodeLocalHeights(const std::uint8_t* a_raw, std::uint32_t a_count, std::uint32_t a_stride, bool a_fullPrecision, LocalHeights& a_out)
    {
        const auto& bound = a_out.bound;
        const float limit = bound.radius * 1.01f + 1.0f;
        const float limitSq = limit * limit;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const bool full = (attempt == 0) == a_fullPrecision;
            if (a_count == 0 || a_stride < (full ? 12u : 6u)) {
                continue;
            }
            bool  inside = true;
            float halfWidth = 0.0f;
            float halfDepth = 0.0f;
            float minZ = 0.0f;
            float maxZ = 0.0f;
            for (std::uint32_t i = 0; i < a_count && inside; ++i) {
                const std::uint8_t* vertex = a_raw + static_cast<std::size_t>(i) * a_stride;
                float               p[3];
                if (full) {
                    std::memcpy(p, vertex, sizeof(p));
                } else {
                    std::uint16_t half[3];
                    std::memcpy(half, vertex, sizeof(half));
                    for (int k = 0; k < 3; ++k) {
                        p[k] = HalfToFloat(half[k]);
                    }
                }
                const float dx = p[0] - bound.center.x;
                const float dy = p[1] - bound.center.y;
                const float dz = p[2] - bound.center.z;
                inside = dx * dx + dy * dy + dz * dz <= limitSq;
                halfWidth = (std::max)(halfWidth, std::abs(dx));
                halfDepth = (std::max)(halfDepth, std::abs(dy));
                minZ = i == 0 ? p[2] : (std::min)(minZ, p[2]);
                maxZ = i == 0 ? p[2] : (std::max)(maxZ, p[2]);
            }
            if (!inside) {
                continue;
            }
            a_out.valid = true;
            const float tolerance = (std::max)(2.0f, bound.radius * 0.01f);
            if (std::abs((minZ + maxZ) * 0.5f - bound.center.z) <= tolerance) {
                a_out.minZ = minZ;
                a_out.maxZ = maxZ;
            } else {
                const float halfHeightSq = bound.radius * bound.radius - halfWidth * halfWidth - halfDepth * halfDepth;
                const float halfHeight = std::sqrt((std::max)(halfHeightSq, 0.0f));
                a_out.fromBound = true;
                a_out.minZ = bound.center.z - halfHeight;
                a_out.maxZ = bound.center.z + halfHeight;
            }
            return true;
        }
        return false;
    }

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

    constexpr float kSolidFogScale = 0.1f;
    constexpr float kSolidNearHeight = 512.0f;
    constexpr float kSolidFullHeight = 2048.0f;

    float NearbySolidAmount(const RE::BSWaterShaderProperty* a_property)
    {
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera || !camera->cameraRoot || !a_property || std::abs(a_property->plane.normal.z - 1.0f) > 1e-3f) {
            return 0.0f;
        }
        const float above = camera->cameraRoot->world.translate.z - a_property->plane.constant;
        const float t = std::clamp((above - kSolidNearHeight) / (kSolidFullHeight - kSolidNearHeight), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    RE::BSWaterShaderMaterial* BuildSolidMaterial(
        const RE::BSWaterShaderMaterial* a_source,
        const SettingsSnapshot& a_settings,
        bool a_overrideActive,
        float a_amount)
    {
        alignas(RE::BSWaterShaderMaterial) static thread_local std::byte scratch[sizeof(RE::BSWaterShaderMaterial)];
        std::memcpy(scratch, a_overrideActive ? BuildOverrideMaterial(a_source, a_settings) : a_source, sizeof(RE::BSWaterShaderMaterial));
        auto* waterMat = reinterpret_cast<RE::BSWaterShaderMaterial*>(scratch);

        const float keep = 1.0f - a_amount;
        const float fogScale = std::pow(kSolidFogScale, a_amount);
        waterMat->depthProperties.reflections *= keep;
        waterMat->depthProperties.refraction *= keep;
        waterMat->depthProperties.normals *= keep;
        waterMat->depthProperties.specularLighting *= keep;
        waterMat->aboveWaterFogDistFar = (std::max)(waterMat->aboveWaterFogDistFar * fogScale, 1.0f);
        waterMat->unk15C = (std::max)(waterMat->unk15C * fogScale, 1.0f);
        return waterMat;
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

std::optional<WaterShader::MeshHeights> WaterShader::MeasureMeshHeights(RE::BSGeometry* a_geometry)
{
    if (!a_geometry) {
        return std::nullopt;
    }
    auto& runtime = a_geometry->GetGeometryRuntimeData();
    auto* data = runtime.rendererData;
    const auto& world = a_geometry->world;
    const auto& rot = world.rotate.entry;
    if (!data || std::abs(rot[2][0]) > 1e-4f || std::abs(rot[2][1]) > 1e-4f) {
        return std::nullopt;
    }
    if (const auto known = LodWaterSplit::KnownLocalHeight(a_geometry, data)) {
        const float height = world.translate.z + world.scale * rot[2][2] * *known;
        auto*       shape = a_geometry->AsTriShape();
        return MeshHeights{ height, height, shape ? shape->GetTrishapeRuntimeData().vertexCount : 0u, false };
    }
    if (!data->rawVertexData) {
        return std::nullopt;
    }

    const auto& bound = a_geometry->GetModelData().modelBound;
    if (heightCache.size() > 4096) {
        heightCache.clear();
    }
    auto& cached = heightCache[a_geometry];
    if (cached.rawVertexData != data->rawVertexData || cached.bound.radius != bound.radius ||
        cached.bound.center.x != bound.center.x || cached.bound.center.y != bound.center.y || cached.bound.center.z != bound.center.z) {
        auto* shape = a_geometry->AsTriShape();
        if (!shape) {
            cached = LocalHeights{ data->rawVertexData, 0, bound };
            return std::nullopt;
        }
        const std::uint32_t count = shape->GetTrishapeRuntimeData().vertexCount;
        cached = LocalHeights{ data->rawVertexData, count, bound };
        for (const auto* desc : { &runtime.vertexDesc, &data->vertexDesc }) {
            const auto          bits = std::bit_cast<std::uint64_t>(*desc);
            const std::uint32_t stride = static_cast<std::uint32_t>(bits & 0xF) * 4;
            if (DecodeLocalHeights(data->rawVertexData, count, stride, desc->HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC), cached)) {
                break;
            }
        }
    }
    if (!cached.valid) {
        return std::nullopt;
    }

    const float scale = world.scale * rot[2][2];
    const float a = world.translate.z + scale * cached.minZ;
    const float b = world.translate.z + scale * cached.maxZ;
    return MeshHeights{ (std::min)(a, b), (std::max)(a, b), cached.count, cached.fromBound };
}

std::optional<RE::NiPlane> WaterShader::DistantWaterPlane(RE::BSGeometry* a_geometry, const RE::NiPlane& a_stored)
{
    if (!a_geometry) {
        return std::nullopt;
    }
    const auto  heights = MeasureMeshHeights(a_geometry);
    const float height = heights ? heights->maxZ : a_geometry->worldBound.center.z;
    if (std::abs(a_stored.normal.x) < 1e-4f && std::abs(a_stored.normal.y) < 1e-4f && std::abs(a_stored.normal.z - 1.0f) < 1e-4f &&
        std::abs(a_stored.constant - height) < 1.0f) {
        return std::nullopt;
    }
    return RE::NiPlane{ { 0.0f, 0.0f, 1.0f }, height };
}

RE::NiPointer<RE::TESWaterObject> WaterShader::FindMatchingCloseWater(RE::BSGeometry* a_distant)
{
    auto* waterSystem = RE::TESWaterSystem::GetSingleton();
    if (!a_distant || !waterSystem) {
        return nullptr;
    }

    struct CachedCloseWaterMatch
    {
        RE::NiPoint3                          center{};
        std::uint32_t                         waterObjCount{ 0 };
        std::chrono::steady_clock::time_point time{};
        RE::NiPointer<RE::TESWaterObject>     match;
        bool                                  flat{ false };
    };

    static thread_local std::unordered_map<const RE::BSGeometry*, CachedCloseWaterMatch> s_matchCache;
    static thread_local std::uint32_t                                                     s_lastWaterCount = 0;
    static thread_local std::chrono::steady_clock::time_point                             s_waterCountChanged{};
    constexpr auto        kMatchCacheTtl = std::chrono::milliseconds(250);
    constexpr auto        kWaterChangeSettle = std::chrono::milliseconds(100);
    constexpr std::size_t kMatchCacheLimit = 1024;

    const auto&         centre = a_distant->worldBound.center;
    const std::uint32_t currentCount = waterSystem->waterObjects.size();
    const auto          now = std::chrono::steady_clock::now();

    if (currentCount != s_lastWaterCount) {
        s_lastWaterCount = currentCount;
        s_waterCountChanged = now;
    }
    const bool waterSettling = now - s_waterCountChanged < kWaterChangeSettle;

    if (auto it = s_matchCache.find(a_distant); it != s_matchCache.end()) {
        const auto& entry = it->second;
        if (entry.center.x == centre.x && entry.center.y == centre.y && entry.center.z == centre.z &&
            now - entry.time < kMatchCacheTtl &&
            (!entry.flat || entry.waterObjCount == currentCount || waterSettling)) {
            return entry.match;
        }
    }

    auto store = [&](RE::NiPointer<RE::TESWaterObject> a_match, bool a_flat) {
        if (s_matchCache.size() >= kMatchCacheLimit) {
            std::erase_if(s_matchCache, [&](const auto& a_entry) { return now - a_entry.second.time >= kMatchCacheTtl; });
            if (s_matchCache.size() >= kMatchCacheLimit) {
                s_matchCache.clear();
            }
        }
        s_matchCache[a_distant] = CachedCloseWaterMatch{ centre, currentCount, now, a_match, a_flat };
    };

    const auto heights = MeasureMeshHeights(a_distant);
    if (!heights) {
        store(nullptr, false);
        return nullptr;
    }

    const bool  flat = heights->maxZ - heights->minZ <= 2.0f;
    const float height = heights->maxZ;
    auto*       type = CellWaterMap::TypeFor(a_distant, heights->minZ, heights->maxZ);
    float       bestDistanceSq = (std::numeric_limits<float>::max)();
    float       bestTypeDistanceSq = (std::numeric_limits<float>::max)();
    RE::NiPointer<RE::TESWaterObject> best;
    RE::NiPointer<RE::TESWaterObject> bestType;

    {
        RE::BSSpinLockGuard guard(waterSystem->lock);
        for (const auto& water : waterSystem->waterObjects) {
            if (!water || !water->shape) {
                continue;
            }
            auto* shape = water->shape.get();
            const auto& plane = water->plane;
            if (shape == a_distant || IsLodWaterGeometry(shape) || std::abs(plane.normal.z - 1.0f) > 1e-3f) {
                continue;
            }
            const bool sameType = type && water->waterType == type;
            const bool sameHeight = flat && std::abs(plane.constant - height) <= 2.0f;
            if (!sameType && !sameHeight) {
                continue;
            }
            auto* property = shape->GetGeometryRuntimeData().shaderProperty.get();
            if (!property || !property->material || property->material->GetType() != RE::BSShaderMaterial::Type::kWater) {
                continue;
            }
            const float dx = shape->worldBound.center.x - centre.x;
            const float dy = shape->worldBound.center.y - centre.y;
            const float distanceSq = dx * dx + dy * dy;
            if (sameType && distanceSq < bestTypeDistanceSq) {
                bestTypeDistanceSq = distanceSq;
                bestType = water;
            }
            if (sameHeight && distanceSq < bestDistanceSq) {
                bestDistanceSq = distanceSq;
                best = water;
            }
        }
    }

    if (bestType) {
        best = bestType;
    }
    store(best, true);
    return best;
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
    currentSolid = 0.0f;

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
    RE::NiPointer<RE::TESWaterObject> matchedWater;
    RE::NiPointer<RE::BSTriShape>     matchedShape;
    RE::BSShaderMaterial*             matchedMaterial = nullptr;
    RE::BSWaterShaderProperty*        waterShaderProp = nullptr;

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
            float solid = 0.0f;
            if (matching && settings.solidDistantWater) {
                solid = distant ? (distantWaterUpgraded ? 1.0f : 0.0f) : NearbySolidAmount(waterShaderProp);
            }
            if (distant) {
                matchedWater = FindMatchingCloseWater(a_pass->geometry);
                matchedShape = matchedWater ? matchedWater->shape : nullptr;
                auto* property = matchedShape ? matchedShape->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
                if (property && property->material && property->material->GetType() == RE::BSShaderMaterial::Type::kWater) {
                    source = property->material;
                    matchedMaterial = property->material;
                }
            }

            if ((overrideActive || matching) && (source != currentSource || std::abs(solid - currentSolid) > 1e-3f)) {
                currentSource = source;
                currentSolid = solid;
                const auto* water = static_cast<const RE::BSWaterShaderMaterial*>(source);
                if (solid > 0.0f) {
                    originalSetupMaterial(a_shader, BuildSolidMaterial(water, settings, overrideActive, solid));
                } else {
                    originalSetupMaterial(a_shader, overrideActive ? BuildOverrideMaterial(water, settings) : source);
                }
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
    if (distantWaterUpgraded && waterShaderProp && matchedMaterial && waterShaderProp->material != matchedMaterial) {
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
