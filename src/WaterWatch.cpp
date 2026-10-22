#include "WaterWatch.h"
#include "Settings.h"
#include "Version.h"
#include "WaterShader.h"

#include <RE/B/BGSTerrainManager.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSTriShape.h>
#include <RE/B/BSWaterShaderProperty.h>
#include <RE/N/NiSkinPartition.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/Sky.h>
#include <RE/T/TES.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESWaterForm.h>
#include <RE/T/TESWaterObject.h>
#include <RE/T/TESWaterSystem.h>
#include <RE/T/TESWeather.h>
#include <RE/T/TESWorldSpace.h>
#include <RE/U/UI.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>

namespace
{
	constexpr std::size_t kMatSize = sizeof(RE::BSWaterShaderMaterial);
	constexpr std::size_t kFormDataSize = sizeof(RE::WaterShaderData);

	constexpr std::uint64_t kLearnPolls = 90;
	constexpr int kAnimatedHits = 8;
	constexpr std::uint16_t kFrozenFrames = 90;
	constexpr std::uint32_t kFormMuteAfter = 30;
	constexpr std::uint32_t kLinesPerPoll = 400;
	constexpr std::uint32_t kDrawLinesPerSecond = 60;
	constexpr int kLodWalkNodeBudget = 50000;

	struct Field
	{
		std::uint16_t offset;
		std::uint8_t  size;
		char          kind;
		const char*   name;
	};

	constexpr Field kMaterialFields[] = {
		{ 0x00C, 4, 'f', "texCoordOffset[0].x" },
		{ 0x010, 4, 'f', "texCoordOffset[0].y" },
		{ 0x014, 4, 'f', "texCoordOffset[1].x" },
		{ 0x018, 4, 'f', "texCoordOffset[1].y" },
		{ 0x01C, 4, 'f', "texCoordScale[0].x" },
		{ 0x020, 4, 'f', "texCoordScale[0].y" },
		{ 0x024, 4, 'f', "texCoordScale[1].x" },
		{ 0x028, 4, 'f', "texCoordScale[1].y" },
		{ 0x02C, 4, 'u', "hashKey" },
		{ 0x030, 4, 'u', "unk030" },
		{ 0x034, 4, 'u', "unk034" },
		{ 0x038, 8, 'p', "staticReflectionTexture" },
		{ 0x040, 8, 'p', "normalTexture1" },
		{ 0x048, 8, 'p', "normalTexture2" },
		{ 0x050, 8, 'p', "normalTexture3" },
		{ 0x058, 8, 'p', "normalTexture4" },
		{ 0x060, 4, 'f', "shallowWaterColor.r" },
		{ 0x064, 4, 'f', "shallowWaterColor.g" },
		{ 0x068, 4, 'f', "shallowWaterColor.b" },
		{ 0x06C, 4, 'f', "sunSparklePower" },
		{ 0x070, 4, 'f', "deepWaterColor.r" },
		{ 0x074, 4, 'f', "deepWaterColor.g" },
		{ 0x078, 4, 'f', "deepWaterColor.b" },
		{ 0x07C, 4, 'f', "deepWaterColor.a" },
		{ 0x080, 4, 'f', "reflectionColor.r" },
		{ 0x084, 4, 'f', "reflectionColor.g" },
		{ 0x088, 4, 'f', "reflectionColor.b" },
		{ 0x08C, 4, 'f', "reflectionColor.a" },
		{ 0x090, 4, 'f', "sunSpecularPower" },
		{ 0x094, 4, 'f', "reflectionAmount" },
		{ 0x098, 4, 'f', "alpha" },
		{ 0x09C, 4, 'f', "refractionMagnitude" },
		{ 0x0A0, 4, 'u', "unk0A0" },
		{ 0x0A4, 4, 'u', "unk0A4" },
		{ 0x0A8, 4, 'f', "unk0A8" },
		{ 0x0AC, 4, 'u', "unk0AC" },
		{ 0x0B0, 4, 'u', "unk0B0" },
		{ 0x0B4, 4, 'u', "unk0B4" },
		{ 0x0B8, 4, 'u', "unk0B8" },
		{ 0x0BC, 4, 'u', "unk0BC" },
		{ 0x0C0, 4, 'u', "unk0C0" },
		{ 0x0C4, 4, 'u', "unk0C4" },
		{ 0x0C8, 4, 'f', "specularPower" },
		{ 0x0CC, 4, 'u', "unk0CC" },
		{ 0x0D0, 4, 'u', "unk0D0" },
		{ 0x0D4, 4, 'u', "unk0D4" },
		{ 0x0D8, 4, 'u', "unk0D8" },
		{ 0x0DC, 4, 'u', "unk0DC" },
		{ 0x0E0, 4, 'f', "noiseFalloff" },
		{ 0x0E4, 4, 'f', "reflectionMagnitude" },
		{ 0x0E8, 4, 'f', "sunSparkleMagnitude" },
		{ 0x0EC, 4, 'f', "unk0EC" },
		{ 0x0F0, 4, 'f', "depthProperties.reflections" },
		{ 0x0F4, 4, 'f', "depthProperties.refraction" },
		{ 0x0F8, 4, 'f', "depthProperties.normals" },
		{ 0x0FC, 4, 'f', "depthProperties.specularLighting" },
		{ 0x100, 4, 'f', "normalScroll1.x" },
		{ 0x104, 4, 'f', "normalScroll1.y" },
		{ 0x108, 4, 'f', "normalScroll2.x" },
		{ 0x10C, 4, 'f', "normalScroll2.y" },
		{ 0x110, 4, 'f', "normalScroll3.x" },
		{ 0x114, 4, 'f', "normalScroll3.y" },
		{ 0x118, 4, 'u', "unk118" },
		{ 0x11C, 4, 'u', "unk11C" },
		{ 0x120, 4, 'f', "uvScaleA[0]" },
		{ 0x124, 4, 'f', "uvScaleA[1]" },
		{ 0x128, 4, 'f', "uvScaleA[2]" },
		{ 0x12C, 4, 'u', "unk12C" },
		{ 0x130, 4, 'f', "amplitudeA[0]" },
		{ 0x134, 4, 'f', "amplitudeA[1]" },
		{ 0x138, 4, 'f', "amplitudeA[2]" },
		{ 0x13C, 4, 'f', "displacementDampener" },
		{ 0x140, 4, 'f', "plane.normal.x" },
		{ 0x144, 4, 'f', "plane.normal.y" },
		{ 0x148, 4, 'f', "plane.normal.z" },
		{ 0x14C, 4, 'f', "plane.constant" },
		{ 0x150, 4, 'u', "unk150" },
		{ 0x154, 4, 'f', "flowmapScale" },
		{ 0x158, 4, 'f', "aboveWaterFogDistFar" },
		{ 0x15C, 4, 'f', "unk15C" },
		{ 0x160, 4, 'f', "unk160" },
		{ 0x164, 4, 'f', "underwaterFogDistFar" },
		{ 0x168, 4, 'f', "unk168" },
		{ 0x16C, 4, 'f', "underwaterFogAmount" },
		{ 0x170, 4, 'f', "fresnelAmount" },
		{ 0x174, 4, 'u', "unk174" },
		{ 0x178, 4, 'u', "unk178" },
		{ 0x17C, 4, 'u', "unk17C" },
	};
	constexpr std::size_t kMaterialFieldCount = std::size(kMaterialFields);

	constexpr Field kFormFields[] = {
		{ 0x00, 4, 'f', "unk00" },
		{ 0x04, 4, 'f', "unk04" },
		{ 0x08, 4, 'f', "unk08" },
		{ 0x0C, 4, 'f', "unk0C" },
		{ 0x10, 4, 'f', "sunSpecularPower" },
		{ 0x14, 4, 'f', "reflectionAmount" },
		{ 0x18, 4, 'f', "fresnelAmount" },
		{ 0x1C, 4, 'u', "unk1C" },
		{ 0x20, 4, 'f', "aboveWaterFogDistNear" },
		{ 0x24, 4, 'f', "aboveWaterFogDistFar" },
		{ 0x28, 4, 'c', "shallowWaterColor" },
		{ 0x2C, 4, 'c', "deepWaterColor" },
		{ 0x30, 4, 'c', "reflectionWaterColor" },
		{ 0x34, 4, 'u', "unk34" },
		{ 0x38, 4, 'f', "unk38" },
		{ 0x3C, 4, 'f', "unk3C" },
		{ 0x40, 4, 'f', "unk40" },
		{ 0x44, 4, 'f', "unk44" },
		{ 0x48, 4, 'f', "displacementSize" },
		{ 0x4C, 4, 'f', "displacementForce" },
		{ 0x50, 4, 'f', "displacementVelocity" },
		{ 0x54, 4, 'f', "displacementFalloff" },
		{ 0x58, 4, 'f', "displacementDampener" },
		{ 0x5C, 4, 'f', "unk5C" },
		{ 0x60, 4, 'f', "noiseFalloff" },
		{ 0x64, 4, 'f', "noiseWindDirectionA[0]" },
		{ 0x68, 4, 'f', "noiseWindDirectionA[1]" },
		{ 0x6C, 4, 'f', "noiseWindDirectionA[2]" },
		{ 0x70, 4, 'f', "noiseWindSpeedA[0]" },
		{ 0x74, 4, 'f', "noiseWindSpeedA[1]" },
		{ 0x78, 4, 'f', "noiseWindSpeedA[2]" },
		{ 0x7C, 4, 'f', "unk7C" },
		{ 0x80, 4, 'f', "unk80" },
		{ 0x84, 4, 'f', "aboveWaterFogAmount" },
		{ 0x88, 4, 'f', "unk88" },
		{ 0x8C, 4, 'f', "underwaterFogAmount" },
		{ 0x90, 4, 'f', "underwaterFogDistNear" },
		{ 0x94, 4, 'f', "underwaterFogDistFar" },
		{ 0x98, 4, 'f', "refractionMagnitude" },
		{ 0x9C, 4, 'f', "specularPower" },
		{ 0xA0, 4, 'f', "unkA0" },
		{ 0xA4, 4, 'f', "specularRadius" },
		{ 0xA8, 4, 'f', "specularBrightness" },
		{ 0xAC, 4, 'f', "uvScaleA[0]" },
		{ 0xB0, 4, 'f', "uvScaleA[1]" },
		{ 0xB4, 4, 'f', "uvScaleA[2]" },
		{ 0xB8, 4, 'f', "amplitudeA[0]" },
		{ 0xBC, 4, 'f', "amplitudeA[1]" },
		{ 0xC0, 4, 'f', "amplitudeA[2]" },
		{ 0xC4, 4, 'f', "reflectionMagnitude" },
		{ 0xC8, 4, 'f', "sunSparkleMagnitude" },
		{ 0xCC, 4, 'f', "sunSpecularMagnitude" },
		{ 0xD0, 4, 'f', "depthProperties.reflections" },
		{ 0xD4, 4, 'f', "depthProperties.refraction" },
		{ 0xD8, 4, 'f', "depthProperties.normals" },
		{ 0xDC, 4, 'f', "depthProperties.specularLighting" },
		{ 0xE0, 4, 'f', "sunSparklePower" },
		{ 0xE4, 4, 'f', "flowmapScale" },
	};

	static_assert(kMatSize == 0x180);
	static_assert(kFormDataSize == 0xE8);

	struct GeomRec
	{
		std::string   name;
		std::string   parents;
		const void*   property{ nullptr };
		const void*   material{ nullptr };
		RE::FormID    formID{ 0 };
		bool          inWaterSystem{ false };
		bool          underLodRoot{ false };
		bool          culled{ false };
		std::uint32_t waterFlags{ 0 };
		std::uint32_t unk8C{ 0 };
		std::int32_t  cellX{ 0 };
		std::int32_t  cellY{ 0 };
		std::int32_t  flowX{ 0 };
		std::int32_t  flowY{ 0 };
		float         plane[4]{};
		float         translateZ{ 0.0f };
		float         boundCenter[3]{};
		float         boundRadius{ 0.0f };
		std::uint32_t vertexCount{ 0 };
		float         vertexZMin{ 0.0f };
		float         vertexZMax{ 0.0f };
		bool          heightsFromBound{ false };
		std::optional<float> fullShaderPlaneZ;
		bool          matchChecked{ false };
		const void*   matchedShape{ nullptr };
		RE::FormID    matchedForm{ 0 };
		std::uint64_t shaderFlags{ 0 };
		float         alpha{ 0.0f };
		std::uint64_t unkB0{ 0 };
		std::uint8_t  unkB8{ 0 };
		std::int32_t  unkF0{ 0 };
		std::uint32_t unkF4{ 0 };
		std::uint8_t  unkF8{ 0 };
		std::uint8_t  objFlags{ 0 };
		const void*   reflections{ nullptr };
		const void*   displacement{ nullptr };
		const void*   normals{ nullptr };
	};

	struct MatRec
	{
		std::array<std::uint8_t, kMatSize> bytes{};
		std::uint32_t                      liveUsers{ 0 };
		std::uint32_t                      lodUsers{ 0 };
		RE::FormID                         ownerForm{ 0 };
	};

	struct FormRec
	{
		std::array<std::uint8_t, kFormDataSize> data{};
		std::uint8_t                            flags{ 0 };
		bool                                    needUpdate{ false };
		bool                                    resetNoiseTextures{ false };
		const void*                             material{ nullptr };
		const void*                             noise[4]{};
	};

	struct Snapshot
	{
		std::unordered_map<const void*, GeomRec> geoms;
		std::unordered_map<const void*, MatRec>  mats;
		std::unordered_map<RE::FormID, FormRec>  forms;
		const void*                              lodRoot{ nullptr };
		const void*                              engineWaterLod{ nullptr };
		RE::FormID                               cell{ 0 };
		RE::FormID                               worldSpace{ 0 };
		RE::FormID                               currentWaterType{ 0 };
		RE::FormID                               weather{ 0 };
		std::uint32_t                            weatherWaterMult[4]{};
		bool                                     systemEnabled{ false };
		bool                                     playerUnderwater{ false };
		std::uint32_t                            waterObjects{ 0 };
		std::uint32_t                            reflections{ 0 };
		std::uint32_t                            normals{ 0 };
		std::uint32_t                            displacements{ 0 };
		bool                                     lodWalked{ false };
		bool                                     valid{ false };
		RE::NiPoint3                             playerPos{};
		RE::NiPoint3                             cameraPos{};
		float                                    underwaterHeight{ 0.0f };

		void Clear()
		{
			playerPos = cameraPos = {};
			underwaterHeight = 0.0f;
			geoms.clear();
			mats.clear();
			forms.clear();
			lodRoot = engineWaterLod = nullptr;
			cell = worldSpace = currentWaterType = weather = 0;
			std::fill(std::begin(weatherWaterMult), std::end(weatherWaterMult), 0u);
			systemEnabled = playerUnderwater = false;
			waterObjects = reflections = normals = displacements = 0;
			lodWalked = valid = false;
		}
	};

	struct FieldState
	{
		std::uint32_t history{ 0 };
		std::uint16_t idle{ 0 };
		bool          animated{ false };
		bool          frozen{ false };
	};

	struct MatTrack
	{
		std::uint64_t                                firstSeenPoll{ 0 };
		std::array<FieldState, kMaterialFieldCount> fields{};
	};

	struct FormTrack
	{
		std::uint32_t frameChanges{ 0 };
		bool          muted{ false };
	};

	struct DrawRec
	{
		bool          isLod{ false };
		const void*   material{ nullptr };
		std::uint32_t techniques[6]{};
		std::uint8_t  techniqueCount{ 0 };
	};

	enum class Mode
	{
		Frame,
		PreScope,
		Scope
	};

	std::atomic<bool>               g_enabled{ false };
	std::shared_ptr<spdlog::logger> g_log;

	std::mutex                                    g_stateLock;
	Snapshot                                      g_prev;
	Snapshot                                      g_cur;
	std::unordered_map<const void*, MatTrack>     g_matTracks;
	std::unordered_map<RE::FormID, FormTrack>     g_formTracks;
	std::unordered_map<RE::FormID, std::string>   g_formNames;
	std::uint64_t                                 g_pollIndex{ 0 };
	std::uint32_t                                 g_lineBudget{ kLinesPerPoll };
	std::uint32_t                                 g_suppressed{ 0 };
	std::uint32_t                                 g_markCount{ 0 };
	std::string                                   g_pendingMarkLabel;
	std::uint32_t                                 g_pendingMarkFrames{ 0 };
	std::chrono::steady_clock::time_point         g_lastSummary{};
	std::uint64_t                                 g_framesSinceSummary{ 0 };

	std::mutex                                      g_drawLock;
	std::unordered_map<const void*, DrawRec>        g_draws;
	std::unordered_map<const void*, std::uint64_t>  g_matLastDrawFrame;
	std::atomic<std::uint64_t>                      g_frame{ 0 };
	std::atomic<std::uint64_t>                      g_drawCount{ 0 };
	std::atomic<std::uint64_t>                      g_lodDrawCount{ 0 };
	std::atomic<std::uint64_t>                      g_rebuildCount{ 0 };
	std::atomic<std::uint64_t>                      g_setupMaterialCount{ 0 };
	std::atomic<std::int32_t>                       g_drawLineBudget{ static_cast<std::int32_t>(kDrawLinesPerSecond) };
	std::atomic<std::uint32_t>                      g_drawLinesSuppressed{ 0 };

	thread_local int t_scopeDepth = 0;

	void Write(const std::string& a_line)
	{
		if (g_log) {
			g_log->info("{}", a_line);
		}
	}

	template <class... Args>
	void Line(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		if (g_lineBudget == 0) {
			++g_suppressed;
			return;
		}
		--g_lineBudget;
		Write(std::format(a_fmt, std::forward<Args>(a_args)...));
	}

	void DrawLine(const std::string& a_line)
	{
		if (g_drawLineBudget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
			g_drawLinesSuppressed.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		Write(a_line);
	}

	std::string Ptr(const void* a_ptr)
	{
		return a_ptr ? std::format("{}", a_ptr) : std::string("null");
	}

	std::string FormatValue(const Field& a_field, const std::uint8_t* a_base)
	{
		const auto* p = a_base + a_field.offset;
		switch (a_field.kind) {
		case 'f':
			{
				float v;
				std::memcpy(&v, p, sizeof(v));
				return std::format("{:.4f}", v);
			}
		case 'p':
			{
				std::uint64_t v;
				std::memcpy(&v, p, sizeof(v));
				return v ? std::format("0x{:X}", v) : std::string("null");
			}
		case 'c':
			return std::format("rgba({},{},{},{})", p[0], p[1], p[2], p[3]);
		default:
			{
				std::uint32_t u;
				float         v;
				std::memcpy(&u, p, sizeof(u));
				std::memcpy(&v, p, sizeof(v));
				return std::format("0x{:08X}({:g})", u, v);
			}
		}
	}

	bool FieldChanged(const Field& a_field, const std::uint8_t* a_old, const std::uint8_t* a_new)
	{
		return std::memcmp(a_old + a_field.offset, a_new + a_field.offset, a_field.size) != 0;
	}

	std::string DecodeTechnique(std::uint32_t a_id)
	{
		static constexpr std::pair<std::uint32_t, const char*> kSpecial[] = {
			{ 0x5C00006B, "SIMPLE" }, { 0x5C00006C, "SIMPLE|VC" }, { 0x5C00006D, "STENCIL" }, { 0x5C00006E, "STENCIL|VC" },
			{ 0x5C00006F, "STENCIL" }, { 0x5C000070, "STENCIL|VC" }
		};
		for (const auto& [id, name] : kSpecial) {
			if (a_id == id) {
				return std::format("0x{:X}[{}]", a_id, name);
			}
		}
		constexpr std::uint32_t kGeneralBase = 0x5C000071;
		if (a_id < kGeneralBase || a_id - kGeneralBase > 0x7FFF) {
			return std::format("0x{:X}[?]", a_id);
		}
		const std::uint32_t descriptor = a_id - kGeneralBase;

		static constexpr std::pair<std::uint32_t, const char*> kFlags[] = {
			{ 1u << 0, "VC" }, { 1u << 1, "NORMAL_TEXCOORD" }, { 1u << 2, "REFLECTIONS" }, { 1u << 3, "REFRACTIONS" },
			{ 1u << 4, "DEPTH" }, { 1u << 5, "INTERIOR" }, { 1u << 6, "WADING" }, { 1u << 7, "VERTEX_ALPHA_DEPTH" },
			{ 1u << 8, "CUBEMAP" }, { 1u << 9, "FLOWMAP" }, { 1u << 10, "BLEND_NORMALS" }
		};
		const auto technique = (descriptor >> 11) & 0xF;
		std::string out;
		switch (technique) {
		case 0:
			out = "MAIN";
			break;
		case 8:
			out = "UNDERWATER";
			break;
		case 9:
			out = "LOD";
			break;
		case 10:
			out = "STENCIL";
			break;
		case 11:
			out = "SIMPLE";
			break;
		default:
			out = technique < 8 ? std::format("SPECULAR_{}LIGHTS", technique) : std::format("T{}", technique);
			break;
		}
		for (const auto& [bit, name] : kFlags) {
			if (descriptor & bit) {
				out += '|';
				out += name;
			}
		}
		return std::format("0x{:X}[{}]", a_id, out);
	}

	std::string DecodeWaterFlags(std::uint32_t a_flags)
	{
		static constexpr std::pair<std::uint32_t, const char*> kFlags[] = {
			{ 1u << 5, "Underwater" }, { 1u << 6, "Reflections" }, { 1u << 14, "CubemapReflections" },
			{ 1u << 15, "Flowmap" }, { 1u << 16, "BlendNormals" }
		};
		std::string out = std::format("0x{:X}", a_flags);
		std::string names;
		for (const auto& [bit, name] : kFlags) {
			if (a_flags & bit) {
				if (!names.empty()) {
					names += '|';
				}
				names += name;
			}
		}
		return names.empty() ? out : out + "[" + names + "]";
	}

	const std::string& FormName(RE::FormID a_id)
	{
		static const std::string kNone = "none";
		if (a_id == 0) {
			return kNone;
		}
		auto it = g_formNames.find(a_id);
		if (it != g_formNames.end()) {
			return it->second;
		}
		std::string name = std::format("{:08X}", a_id);
		if (auto* form = RE::TESForm::LookupByID(a_id)) {
			const auto edid = Util::GetEditorID(form);
			if (!edid.empty()) {
				name += " '" + edid + "'";
			}
		}
		return g_formNames.emplace(a_id, std::move(name)).first->second;
	}

	std::string ParentChain(const RE::NiAVObject* a_obj)
	{
		std::string out;
		int         depth = 0;
		for (auto* parent = a_obj ? a_obj->parent : nullptr; parent && depth < 4; parent = parent->parent, ++depth) {
			if (!out.empty()) {
				out += " < ";
			}
			const char* name = parent->name.c_str();
			out += (name && name[0]) ? name : "<unnamed>";
		}
		return out.empty() ? std::string("<no parent>") : out;
	}

	bool IsLoading()
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
	}

	bool IsPaused()
	{
		auto* ui = RE::UI::GetSingleton();
		return !ui || ui->GameIsPaused() || IsLoading();
	}

	RE::NiNode* EngineWaterLodNode()
	{
		return RE::BGSTerrainManager::GetWaterNode().get();
	}

	void MeasureSurface(RE::BSGeometry* a_geom, GeomRec& a_rec)
	{
		const auto& world = a_geom->world;
		a_rec.translateZ = world.translate.z;
		a_rec.boundCenter[0] = a_geom->worldBound.center.x;
		a_rec.boundCenter[1] = a_geom->worldBound.center.y;
		a_rec.boundCenter[2] = a_geom->worldBound.center.z;
		a_rec.boundRadius = a_geom->worldBound.radius;

		if (const auto heights = WaterShader::MeasureMeshHeights(a_geom)) {
			a_rec.vertexCount = heights->vertexCount;
			a_rec.vertexZMin = heights->minZ;
			a_rec.vertexZMax = heights->maxZ;
			a_rec.heightsFromBound = heights->fromBound;
		}
	}

	void AddGeometry(Snapshot& a_snap, RE::BSGeometry* a_geom, const RE::TESWaterObject* a_obj, bool a_underLod)
	{
		auto* prop = netimmerse_cast<RE::BSWaterShaderProperty*>(a_geom->GetGeometryRuntimeData().shaderProperty.get());
		if (!prop) {
			return;
		}

		auto [it, inserted] = a_snap.geoms.try_emplace(a_geom);
		auto& g = it->second;
		if (a_underLod) {
			g.underLodRoot = true;
			if (!g.fullShaderPlaneZ) {
				const auto plane = WaterShader::DistantWaterPlane(a_geom, prop->plane);
				g.fullShaderPlaneZ = plane ? plane->constant : prop->plane.constant;
			}
			if (!g.matchChecked) {
				g.matchChecked = true;
				if (const auto match = WaterShader::FindMatchingCloseWater(a_geom)) {
					g.matchedShape = match->shape.get();
					g.matchedForm = match->waterType ? match->waterType->GetFormID() : 0;
				}
			}
		}
		if (a_obj) {
			g.inWaterSystem = true;
			g.formID = a_obj->waterType ? a_obj->waterType->GetFormID() : 0;
			g.objFlags = a_obj->flags;
			g.reflections = a_obj->reflections.get();
			g.displacement = a_obj->displacement.get();
			g.normals = a_obj->normals.get();
		}
		if (!inserted) {
			return;
		}

		const char* name = a_geom->name.c_str();
		g.name = (name && name[0]) ? name : "<unnamed>";
		g.parents = ParentChain(a_geom);
		g.property = prop;
		g.culled = a_geom->GetAppCulled();
		g.waterFlags = prop->waterFlags.underlying();
		g.unk8C = prop->unk8C;
		g.cellX = prop->cellX;
		g.cellY = prop->cellY;
		g.flowX = prop->flowX;
		g.flowY = prop->flowY;
		g.plane[0] = prop->plane.normal.x;
		g.plane[1] = prop->plane.normal.y;
		g.plane[2] = prop->plane.normal.z;
		g.plane[3] = prop->plane.constant;
		MeasureSurface(a_geom, g);
		g.shaderFlags = prop->flags.underlying();
		g.alpha = prop->alpha;
		g.unkB0 = prop->unkB0;
		g.unkB8 = prop->unkB8;
		g.unkF0 = prop->unkF0;
		g.unkF4 = prop->unkF4;
		g.unkF8 = prop->unkF8;

		auto* material = prop->material;
		g.material = material;
		if (material && material->GetType() == RE::BSShaderMaterial::Type::kWater) {
			auto [mit, matInserted] = a_snap.mats.try_emplace(material);
			if (matInserted) {
				std::memcpy(mit->second.bytes.data(), material, kMatSize);
			}
			if (a_obj) {
				++mit->second.liveUsers;
			} else {
				++mit->second.lodUsers;
			}
		}
	}

	void WalkLodRoot(Snapshot& a_snap, RE::NiAVObject* a_root, int& a_budget)
	{
		if (!a_root) {
			return;
		}
		std::vector<RE::NiAVObject*> stack;
		stack.push_back(a_root);
		while (!stack.empty() && a_budget-- > 0) {
			auto* obj = stack.back();
			stack.pop_back();
			if (auto* geom = obj->AsGeometry()) {
				AddGeometry(a_snap, geom, nullptr, true);
				continue;
			}
			if (auto* node = obj->AsNode()) {
				auto&      children = node->GetChildren();
				const auto count = (std::min)(children.free_idx(), children.capacity());
				for (std::uint16_t i = 0; i < count; ++i) {
					if (auto* child = children[i].get()) {
						stack.push_back(child);
					}
				}
			}
		}
	}

	void Collect(Snapshot& a_snap, const Snapshot& a_prev)
	{
		a_snap.Clear();
		a_snap.valid = true;

		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			if (auto* cell = player->GetParentCell()) {
				a_snap.cell = cell->GetFormID();
			}
			a_snap.playerPos = player->GetPosition();
		}
		if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
			a_snap.cameraPos = camera->cameraRoot->world.translate;
		}
		auto* tes = RE::TES::GetSingleton();
		if (tes) {
			if (auto* ws = tes->GetRuntimeData2().worldSpace) {
				a_snap.worldSpace = ws->GetFormID();
			}
			a_snap.lodRoot = tes->objLODWaterRoot;
		}
		auto* engineLod = EngineWaterLodNode();
		a_snap.engineWaterLod = engineLod;

		if (auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather) {
			a_snap.weather = sky->currentWeather->GetFormID();
			for (int t = 0; t < 4; ++t) {
				const auto& c = sky->currentWeather->colorData[RE::TESWeather::ColorTypes::kWaterMultiplier][t];
				a_snap.weatherWaterMult[t] = static_cast<std::uint32_t>(c.red) | (static_cast<std::uint32_t>(c.green) << 8) | (static_cast<std::uint32_t>(c.blue) << 16);
			}
		}

		if (auto* ws = RE::TESWaterSystem::GetSingleton()) {
			RE::BSSpinLockGuard guard(ws->lock);
			a_snap.systemEnabled = ws->enabled;
			a_snap.playerUnderwater = ws->playerUnderwater;
			a_snap.underwaterHeight = ws->underwaterHeight;
			a_snap.currentWaterType = ws->currentWaterType ? ws->currentWaterType->GetFormID() : 0;
			a_snap.waterObjects = ws->waterObjects.size();
			a_snap.reflections = ws->waterReflections.size();
			a_snap.normals = ws->waterNormals.size();
			a_snap.displacements = ws->waterDisplacement.size();
			for (auto& obj : ws->waterObjects) {
				if (obj && obj->shape) {
					AddGeometry(a_snap, obj->shape.get(), obj.get(), false);
				}
			}
		}

		if (!IsLoading()) {
			int budget = kLodWalkNodeBudget;
			if (tes) {
				WalkLodRoot(a_snap, tes->objLODWaterRoot, budget);
			}
			if (engineLod && (!tes || engineLod != tes->objLODWaterRoot)) {
				WalkLodRoot(a_snap, engineLod, budget);
			}
			a_snap.lodWalked = true;
		} else if (a_prev.valid) {
			for (const auto& [key, g] : a_prev.geoms) {
				if (g.underLodRoot && !g.inWaterSystem && !a_snap.geoms.contains(key)) {
					a_snap.geoms.emplace(key, g);
					if (auto mit = a_prev.mats.find(g.material); mit != a_prev.mats.end()) {
						a_snap.mats.try_emplace(g.material, mit->second);
					}
				}
			}
		}

		if (auto* dh = RE::TESDataHandler::GetSingleton()) {
			for (auto* water : dh->GetFormArray<RE::TESWaterForm>()) {
				if (!water) {
					continue;
				}
				auto& f = a_snap.forms[water->GetFormID()];
				std::memcpy(f.data.data(), &water->data, kFormDataSize);
				f.flags = water->flags.underlying();
				f.needUpdate = water->needUpdate;
				f.resetNoiseTextures = water->resetNoiseTextures;
				f.material = water->waterShaderMaterial;
				for (int i = 0; i < 4; ++i) {
					f.noise[i] = water->noiseTextureData[i].get();
				}
				if (water->waterShaderMaterial) {
					if (auto mit = a_snap.mats.find(water->waterShaderMaterial); mit != a_snap.mats.end()) {
						mit->second.ownerForm = water->GetFormID();
					}
				}
			}
		}
	}

	std::string GeomKind(const GeomRec& a_g)
	{
		if (a_g.inWaterSystem && a_g.underLodRoot) {
			return "water-system+lod-root";
		}
		return a_g.inWaterSystem ? "water-system" : "lod-root";
	}

	std::string DescribeGeom(const void* a_key, const GeomRec& a_g)
	{
		return std::format("{} '{}' ({}, form {}, cell {},{})", Ptr(a_key), a_g.name, GeomKind(a_g), FormName(a_g.formID), a_g.cellX, a_g.cellY);
	}

	std::string DescribeSurface(const GeomRec& a_g)
	{
		const float nz = a_g.plane[2];
		std::string planeText;
		float       planeZ = 0.0f;
		const bool  level = std::abs(nz) > 1e-4f;
		if (level) {
			planeZ = a_g.plane[3] / nz;
			planeText = std::format("plane z {:.1f}", planeZ);
			if (std::abs(a_g.plane[0]) > 1e-4f || std::abs(a_g.plane[1]) > 1e-4f || std::abs(nz - 1.0f) > 1e-4f) {
				planeText += std::format(" (normal {:.3f},{:.3f},{:.3f})", a_g.plane[0], a_g.plane[1], nz);
			}
		} else {
			planeText = std::format("plane VERTICAL ({:.3f},{:.3f},{:.3f},{:.1f})", a_g.plane[0], a_g.plane[1], nz, a_g.plane[3]);
		}

		std::string meshText = std::format("mesh translate z {:.1f}, bound centre ({:.0f},{:.0f},{:.1f}) radius {:.0f}",
			a_g.translateZ, a_g.boundCenter[0], a_g.boundCenter[1], a_g.boundCenter[2], a_g.boundRadius);
		std::string verdict;
		if (a_g.vertexCount > 0) {
			meshText += std::format(", {} vertices z {:.1f}..{:.1f}{}", a_g.vertexCount, a_g.vertexZMin, a_g.vertexZMax,
				a_g.heightsFromBound ? " (from the bound: the vertex copy holds no heights)" : "");
			if (a_g.vertexZMax - a_g.vertexZMin >= 1.0f) {
				verdict = "MIXED HEIGHTS";
			} else if (level) {
				const float off = planeZ - a_g.vertexZMin;
				verdict = std::abs(off) < 1.0f ? "plane matches mesh" : std::format("PLANE OFF BY {:.1f}", off);
			}
		} else {
			meshText += ", no vertex copy";
			if (level) {
				verdict = std::format("plane - bound centre = {:.1f}", planeZ - a_g.boundCenter[2]);
			}
		}
		if (a_g.fullShaderPlaneZ) {
			verdict += std::format("{}Full Shader depth plane z {:.1f}", verdict.empty() ? "" : "; ", *a_g.fullShaderPlaneZ);
		}
		if (a_g.matchChecked) {
			verdict += a_g.matchedShape ?
			               std::format("; matches close water {} (form {})", Ptr(a_g.matchedShape), FormName(a_g.matchedForm)) :
			               std::string("; no close water at its height (keeps DefaultWater)");
		}
		return std::format("surface: {} | {}{}{}", planeText, meshText, verdict.empty() ? "" : " => ", verdict);
	}

	std::string DescribeMat(const void* a_key, const MatRec& a_m)
	{
		return std::format("{} (record material of {}, live users {}, lod users {})", Ptr(a_key), FormName(a_m.ownerForm), a_m.liveUsers, a_m.lodUsers);
	}

	void Append(std::string& a_out, std::string_view a_text)
	{
		if (!a_out.empty()) {
			a_out += "; ";
		}
		a_out += a_text;
	}

	template <class T>
	void CompareValue(std::string& a_out, const char* a_name, const T& a_old, const T& a_new)
	{
		if (a_old != a_new) {
			Append(a_out, std::format("{} {} -> {}", a_name, a_old, a_new));
		}
	}

	void ComparePtr(std::string& a_out, const char* a_name, const void* a_old, const void* a_new)
	{
		if (a_old != a_new) {
			Append(a_out, std::format("{} {} -> {}", a_name, Ptr(a_old), Ptr(a_new)));
		}
	}

	std::uint64_t LastDrawFrame(const void* a_material)
	{
		std::scoped_lock lock(g_drawLock);
		auto it = g_matLastDrawFrame.find(a_material);
		return it != g_matLastDrawFrame.end() ? it->second : 0;
	}

	void DiffSystem(const Snapshot& a_old, const Snapshot& a_new, std::string_view a_ctx)
	{
		if (a_old.cell != a_new.cell || a_old.worldSpace != a_new.worldSpace) {
			Line("[{}] ===== CELL CHANGE: cell {} -> {}, worldspace {} -> {} =====", a_ctx,
				FormName(a_old.cell), FormName(a_new.cell), FormName(a_old.worldSpace), FormName(a_new.worldSpace));
		}

		std::string changes;
		ComparePtr(changes, "TES::objLODWaterRoot", a_old.lodRoot, a_new.lodRoot);
		ComparePtr(changes, "engineWaterLodNode", a_old.engineWaterLod, a_new.engineWaterLod);
		if (a_old.currentWaterType != a_new.currentWaterType) {
			Append(changes, std::format("currentWaterType {} -> {}", FormName(a_old.currentWaterType), FormName(a_new.currentWaterType)));
		}
		CompareValue(changes, "waterSystem.enabled", a_old.systemEnabled, a_new.systemEnabled);
		CompareValue(changes, "playerUnderwater", a_old.playerUnderwater, a_new.playerUnderwater);
		CompareValue(changes, "waterObjects", a_old.waterObjects, a_new.waterObjects);
		CompareValue(changes, "waterReflections", a_old.reflections, a_new.reflections);
		CompareValue(changes, "waterNormals", a_old.normals, a_new.normals);
		CompareValue(changes, "waterDisplacement", a_old.displacements, a_new.displacements);
		if (a_old.weather != a_new.weather) {
			Append(changes, std::format("weather {} -> {}", FormName(a_old.weather), FormName(a_new.weather)));
		} else if (!std::equal(std::begin(a_old.weatherWaterMult), std::end(a_old.weatherWaterMult), std::begin(a_new.weatherWaterMult))) {
			Append(changes, std::format("weather waterMultiplier record [{:06X} {:06X} {:06X} {:06X}] -> [{:06X} {:06X} {:06X} {:06X}]",
				a_old.weatherWaterMult[0], a_old.weatherWaterMult[1], a_old.weatherWaterMult[2], a_old.weatherWaterMult[3],
				a_new.weatherWaterMult[0], a_new.weatherWaterMult[1], a_new.weatherWaterMult[2], a_new.weatherWaterMult[3]));
		}
		if (!changes.empty()) {
			Line("[{}] SYSTEM: {}", a_ctx, changes);
		}
	}

	void ReportGeomList(std::string_view a_ctx, const char* a_what, const std::vector<std::pair<const void*, const GeomRec*>>& a_list)
	{
		if (a_list.empty()) {
			return;
		}
		constexpr std::size_t kListed = 8;
		Line("[{}] GEOMETRY {}: {} water geometr{}", a_ctx, a_what, a_list.size(), a_list.size() == 1 ? "y" : "ies");
		for (std::size_t i = 0; i < a_list.size() && i < kListed; ++i) {
			const auto& [key, g] = a_list[i];
			Line("[{}]    {} mat {} flags {} parents: {}", a_ctx, DescribeGeom(key, *g), Ptr(g->material), DecodeWaterFlags(g->waterFlags), g->parents);
		}
		if (a_list.size() > kListed) {
			Line("[{}]    ... and {} more", a_ctx, a_list.size() - kListed);
		}
	}

	void DiffGeoms(const Snapshot& a_old, const Snapshot& a_new, std::string_view a_ctx)
	{
		std::vector<std::pair<const void*, const GeomRec*>> removed;
		std::vector<std::pair<const void*, const GeomRec*>> added;
		for (const auto& [key, g] : a_old.geoms) {
			if (!a_new.geoms.contains(key)) {
				removed.emplace_back(key, &g);
			}
		}
		for (const auto& [key, g] : a_new.geoms) {
			auto it = a_old.geoms.find(key);
			if (it == a_old.geoms.end()) {
				added.emplace_back(key, &g);
				continue;
			}
			const auto& o = it->second;
			std::string changes;
			ComparePtr(changes, "property", o.property, g.property);
			ComparePtr(changes, "material", o.material, g.material);
			if (o.formID != g.formID) {
				Append(changes, std::format("waterType {} -> {}", FormName(o.formID), FormName(g.formID)));
			}
			CompareValue(changes, "inWaterSystem", o.inWaterSystem, g.inWaterSystem);
			CompareValue(changes, "underLodRoot", o.underLodRoot, g.underLodRoot);
			CompareValue(changes, "appCulled", o.culled, g.culled);
			if (o.waterFlags != g.waterFlags) {
				Append(changes, std::format("waterFlags {} -> {}", DecodeWaterFlags(o.waterFlags), DecodeWaterFlags(g.waterFlags)));
			}
			CompareValue(changes, "unk8C", o.unk8C, g.unk8C);
			CompareValue(changes, "cellX", o.cellX, g.cellX);
			CompareValue(changes, "cellY", o.cellY, g.cellY);
			CompareValue(changes, "flowX", o.flowX, g.flowX);
			CompareValue(changes, "flowY", o.flowY, g.flowY);
			if (std::memcmp(o.plane, g.plane, sizeof(o.plane)) != 0) {
				Append(changes, std::format("plane ({:.2f},{:.2f},{:.2f},{:.2f}) -> ({:.2f},{:.2f},{:.2f},{:.2f})",
					o.plane[0], o.plane[1], o.plane[2], o.plane[3], g.plane[0], g.plane[1], g.plane[2], g.plane[3]));
			}
			if (o.shaderFlags != g.shaderFlags) {
				Append(changes, std::format("shaderFlags 0x{:X} -> 0x{:X}", o.shaderFlags, g.shaderFlags));
			}
			CompareValue(changes, "alpha", o.alpha, g.alpha);
			CompareValue(changes, "unkB0", o.unkB0, g.unkB0);
			CompareValue(changes, "unkB8", o.unkB8, g.unkB8);
			CompareValue(changes, "unkF0", o.unkF0, g.unkF0);
			CompareValue(changes, "unkF4", o.unkF4, g.unkF4);
			CompareValue(changes, "unkF8", o.unkF8, g.unkF8);
			CompareValue(changes, "waterObject.flags", o.objFlags, g.objFlags);
			ComparePtr(changes, "waterObject.reflections", o.reflections, g.reflections);
			ComparePtr(changes, "waterObject.displacement", o.displacement, g.displacement);
			ComparePtr(changes, "waterObject.normals", o.normals, g.normals);
			if (!changes.empty()) {
				Line("[{}] GEOMETRY {}: {}", a_ctx, DescribeGeom(key, g), changes);
			}
		}

		ReportGeomList(a_ctx, "removed", removed);
		ReportGeomList(a_ctx, "added", added);

		if (!removed.empty()) {
			std::scoped_lock lock(g_drawLock);
			for (const auto& [key, g] : removed) {
				g_draws.erase(key);
			}
		}
	}

	void DiffMats(const Snapshot& a_old, const Snapshot& a_new, std::string_view a_ctx, Mode a_mode)
	{
		const bool          paused = IsPaused();
		const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
		std::size_t         appeared = 0;

		for (const auto& [key, m] : a_new.mats) {
			auto [tit, trackInserted] = g_matTracks.try_emplace(key);
			auto& track = tit->second;
			if (trackInserted) {
				track.firstSeenPoll = g_pollIndex;
			}

			auto oit = a_old.mats.find(key);
			if (oit == a_old.mats.end()) {
				++appeared;
				continue;
			}

			const bool learning = g_pollIndex - track.firstSeenPoll < kLearnPolls;
			const bool drawnRecently = a_mode == Mode::Frame && frame - LastDrawFrame(key) <= 2;
			std::string changes;
			std::string started;
			std::string frozen;

			for (std::size_t i = 0; i < kMaterialFieldCount; ++i) {
				const auto& field = kMaterialFields[i];
				auto&       st = track.fields[i];
				const bool  changed = FieldChanged(field, oit->second.bytes.data(), m.bytes.data());

				if (a_mode == Mode::Frame) {
					st.history = (st.history << 1) | (changed ? 1u : 0u);
					if (changed) {
						st.idle = 0;
						if (st.frozen) {
							st.frozen = false;
							Append(started, std::format("{} (resumed)", field.name));
						}
						if (!st.animated && std::popcount(st.history & 0xFFFu) >= kAnimatedHits) {
							st.animated = true;
							Append(started, field.name);
						}
					} else if (st.animated && !paused && drawnRecently) {
						if (++st.idle >= kFrozenFrames) {
							st.animated = false;
							st.frozen = true;
							st.idle = 0;
							Append(frozen, std::format("{} = {}", field.name, FormatValue(field, m.bytes.data())));
						}
					}
				}

				if (!changed) {
					continue;
				}
				const bool recentlyBusy = std::popcount(st.history & 0xFFFu) >= 3;
				const bool report = a_mode == Mode::Scope || (!learning && !st.animated && !recentlyBusy);
				if (report) {
					Append(changes, std::format("{} {} -> {}", field.name, FormatValue(field, oit->second.bytes.data()), FormatValue(field, m.bytes.data())));
				}
			}

			if (!changes.empty()) {
				Line("[{}] MATERIAL {}: {}", a_ctx, DescribeMat(key, m), changes);
			}
			if (!started.empty()) {
				Line("[{}] MATERIAL {} animates every frame (not logged further): {}", a_ctx, Ptr(key), started);
			}
			if (!frozen.empty()) {
				Line("[{}] !!! FROZEN: MATERIAL {} is still being drawn but these fields stopped animating for {} frames: {}",
					a_ctx, DescribeMat(key, m), kFrozenFrames, frozen);
			}
		}

		std::size_t gone = 0;
		if (a_old.lodWalked && a_new.lodWalked) {
			for (const auto& [key, m] : a_old.mats) {
				if (!a_new.mats.contains(key)) {
					++gone;
					g_matTracks.erase(key);
				}
			}
		}
		if (appeared || gone) {
			Line("[{}] MATERIALS: {} appeared, {} gone ({} live water materials now)", a_ctx, appeared, gone, a_new.mats.size());
		}
	}

	void DiffForms(const Snapshot& a_old, const Snapshot& a_new, std::string_view a_ctx, Mode a_mode)
	{
		for (const auto& [id, f] : a_new.forms) {
			auto oit = a_old.forms.find(id);
			if (oit == a_old.forms.end()) {
				continue;
			}
			const auto& o = oit->second;
			std::string changes;
			for (const auto& field : kFormFields) {
				if (FieldChanged(field, o.data.data(), f.data.data())) {
					Append(changes, std::format("data.{} {} -> {}", field.name, FormatValue(field, o.data.data()), FormatValue(field, f.data.data())));
				}
			}
			if (o.flags != f.flags) {
				Append(changes, std::format("flags 0x{:02X} -> 0x{:02X}", o.flags, f.flags));
			}
			CompareValue(changes, "needUpdate", o.needUpdate, f.needUpdate);
			CompareValue(changes, "resetNoiseTextures", o.resetNoiseTextures, f.resetNoiseTextures);
			ComparePtr(changes, "waterShaderMaterial", o.material, f.material);
			for (int i = 0; i < 4; ++i) {
				if (o.noise[i] != f.noise[i]) {
					Append(changes, std::format("noiseTextureData[{}] {} -> {}", i, Ptr(o.noise[i]), Ptr(f.noise[i])));
				}
			}
			if (changes.empty()) {
				continue;
			}

			auto& track = g_formTracks[id];
			if (a_mode == Mode::Frame && ++track.frameChanges == kFormMuteAfter) {
				track.muted = true;
				Line("[{}] RECORD {} changes on its own every frame; muting its per-frame changes", a_ctx, FormName(id));
			}
			if (a_mode == Mode::Scope || !track.muted) {
				const auto liveMat = a_new.mats.find(f.material);
				const auto users = liveMat != a_new.mats.end() ? liveMat->second.liveUsers + liveMat->second.lodUsers : 0u;
				Line("[{}] RECORD {} (record material {} drawn by {} live geometr{}): {}", a_ctx, FormName(id), Ptr(f.material), users, users == 1 ? "y" : "ies", changes);
			}
		}
	}

	void LogPluginState(std::string_view a_prefix)
	{
		const auto s = Settings::GetSingleton().GetSnapshot();
		Line("{}plugin state: enabled={} fixDepthProperties={} transparency={:.0f}% refl={:.2f} refr={:.2f} normals={:.2f} spec={:.2f} customColors={} clearUnderwater={} fullShaderDistantWater={} disableRiverFlow={} splashRipples={} realisticDistantWater={} removeSunGlints={} drawOverrideActive={}",
			a_prefix, s.enabled, s.fixDepthProperties, s.waterTransparency, s.depthReflections, s.depthRefraction, s.depthNormals,
			s.depthSpecularLighting, s.hasCustomColors, s.clearUnderwater, s.fullShaderDistantWater, s.disableRiverFlow, s.splashRipples, s.realisticDistantWater, s.removeSunGlints, WaterShader::IsMaterialOverrideActive(s));
	}

	void Poll(std::string_view a_ctx, Mode a_mode)
	{
		g_lineBudget = kLinesPerPoll;
		g_suppressed = 0;
		++g_pollIndex;

		Collect(g_cur, g_prev);
		if (!g_prev.valid) {
			Line("[{}] baseline: cell {}, worldspace {}, {} water-system objects, {} water geometries ({} materials), TES::objLODWaterRoot {} engineWaterLodNode {} ({})",
				a_ctx, FormName(g_cur.cell), FormName(g_cur.worldSpace), g_cur.waterObjects, g_cur.geoms.size(), g_cur.mats.size(),
				Ptr(g_cur.lodRoot), Ptr(g_cur.engineWaterLod), g_cur.lodRoot == g_cur.engineWaterLod ? "same node" : "DIFFERENT nodes");
		} else {
			DiffSystem(g_prev, g_cur, a_ctx);
			DiffGeoms(g_prev, g_cur, a_ctx);
			DiffMats(g_prev, g_cur, a_ctx, a_mode);
			DiffForms(g_prev, g_cur, a_ctx, a_mode);
		}

		if (g_prev.valid && g_prev.cell != g_cur.cell) {
			std::scoped_lock lock(g_drawLock);
			g_draws.clear();
		}

		std::swap(g_prev, g_cur);

		if (g_suppressed) {
			g_lineBudget = 1;
			Line("[{}] ... {} more lines suppressed this frame", a_ctx, g_suppressed);
		}
	}

	void Dump(std::string_view a_label)
	{
		g_lineBudget = 100000;
		const auto& s = g_prev;
		const auto  frame = g_frame.load(std::memory_order_relaxed);

		Line("==================== MARK #{}: {} (frame {}) ====================", ++g_markCount, a_label, frame);
		LogPluginState("   ");
		Line("   cell {}, worldspace {}, currentWaterType {}, weather {}, waterSystem.enabled={}, playerUnderwater={}",
			FormName(s.cell), FormName(s.worldSpace), FormName(s.currentWaterType), FormName(s.weather), s.systemEnabled, s.playerUnderwater);
		Line("   player at ({:.0f},{:.0f},{:.1f}), camera at ({:.0f},{:.0f},{:.1f}), water system's current water height {:.1f}",
			s.playerPos.x, s.playerPos.y, s.playerPos.z, s.cameraPos.x, s.cameraPos.y, s.cameraPos.z, s.underwaterHeight);
		Line("   water-system objects {}, reflections {}, normals {}, displacements {}; TES::objLODWaterRoot {} engineWaterLodNode {} ({})",
			s.waterObjects, s.reflections, s.normals, s.displacements, Ptr(s.lodRoot), Ptr(s.engineWaterLod),
			s.lodRoot == s.engineWaterLod ? "same node" : "DIFFERENT nodes");

		std::vector<std::pair<const void*, const GeomRec*>> geoms;
		geoms.reserve(s.geoms.size());
		for (const auto& [key, g] : s.geoms) {
			geoms.emplace_back(key, &g);
		}
		std::sort(geoms.begin(), geoms.end(), [](const auto& a, const auto& b) {
			if (a.second->inWaterSystem != b.second->inWaterSystem) {
				return a.second->inWaterSystem;
			}
			return a.second->name < b.second->name;
		});

		std::unordered_map<const void*, DrawRec> draws;
		{
			std::scoped_lock lock(g_drawLock);
			draws = g_draws;
		}

		constexpr std::size_t kMaxGeoms = 300;
		Line("   --- {} water geometries (water-system first) ---", geoms.size());
		for (std::size_t i = 0; i < geoms.size() && i < kMaxGeoms; ++i) {
			const auto& [key, g] = geoms[i];
			std::string drawInfo = "not drawn since cell load";
			if (auto dit = draws.find(key); dit != draws.end()) {
				drawInfo = std::format("drawn as {} with", dit->second.isLod ? "LOD-classified" : "normal");
				for (std::uint8_t t = 0; t < dit->second.techniqueCount; ++t) {
					drawInfo += ' ';
					drawInfo += DecodeTechnique(dit->second.techniques[t]);
				}
			}
			Line("   GEOM {} culled={} waterFlags {} flow {},{} mat {} | {} | {} | parents: {}",
				DescribeGeom(key, *g), g->culled, DecodeWaterFlags(g->waterFlags), g->flowX, g->flowY, Ptr(g->material), drawInfo,
				DescribeSurface(*g), g->parents);
		}
		if (geoms.size() > kMaxGeoms) {
			Line("   ... and {} more geometries", geoms.size() - kMaxGeoms);
		}

		Line("   --- {} live water materials ---", s.mats.size());
		for (const auto& [key, m] : s.mats) {
			std::string fields;
			std::string animated;
			std::string frozen;
			auto        tit = g_matTracks.find(key);
			for (std::size_t i = 0; i < kMaterialFieldCount; ++i) {
				const auto& field = kMaterialFields[i];
				Append(fields, std::format("{}={}", field.name, FormatValue(field, m.bytes.data())));
				if (tit != g_matTracks.end()) {
					if (tit->second.fields[i].animated) {
						Append(animated, field.name);
					}
					if (tit->second.fields[i].frozen) {
						Append(frozen, field.name);
					}
				}
			}
			const auto lastDraw = LastDrawFrame(key);
			Line("   MAT {} last drawn {} | animated: [{}] | FROZEN: [{}]", DescribeMat(key, m),
				lastDraw ? std::format("{} frames ago", frame - lastDraw) : std::string("never"), animated, frozen);
			Line("       {}", fields);
		}

		std::unordered_set<RE::FormID> usedForms;
		usedForms.insert(s.currentWaterType);
		for (const auto& [key, g] : s.geoms) {
			usedForms.insert(g.formID);
		}
		usedForms.erase(0);
		Line("   --- {} water records in use ---", usedForms.size());
		for (auto id : usedForms) {
			auto fit = s.forms.find(id);
			if (fit == s.forms.end()) {
				continue;
			}
			const auto& f = fit->second;
			std::string fields;
			for (const auto& field : kFormFields) {
				Append(fields, std::format("{}={}", field.name, FormatValue(field, f.data.data())));
			}
			const bool shared = f.material && s.mats.contains(f.material);
			Line("   RECORD {} flags=0x{:02X} needUpdate={} resetNoiseTextures={} recordMaterial={} ({})",
				FormName(id), f.flags, f.needUpdate, f.resetNoiseTextures, Ptr(f.material),
				shared ? "SAME object live water draws with" : "not drawn directly");
			Line("       {}", fields);
		}
		Line("==================== END MARK #{} ====================", g_markCount);
	}
}

namespace WaterWatch
{
	bool IsEnabled() noexcept
	{
		return g_enabled.load(std::memory_order_relaxed);
	}

	void SetEnabled(bool a_enabled)
	{
		std::scoped_lock lock(g_stateLock);
		if (a_enabled == g_enabled.load()) {
			return;
		}

		if (a_enabled && !g_log) {
			auto path = logger::log_directory();
			if (!path) {
				logger::error("Live water log: SKSE log directory unavailable; not starting.");
				return;
			}
			*path /= std::format("{}_Watch.log", Plugin::NAME);
			try {
				auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
				g_log = std::make_shared<spdlog::logger>("water watch", std::move(sink));
				g_log->set_level(spdlog::level::info);
				g_log->flush_on(spdlog::level::info);
				g_log->set_pattern("[%H:%M:%S.%e] %v");
			} catch (const std::exception& e) {
				logger::error("Live water log: could not open {}: {}", path->string(), e.what());
				return;
			}
			logger::info("Live water log writing to {}", path->string());
		}

		if (a_enabled) {
			g_prev.Clear();
			g_cur.Clear();
			g_matTracks.clear();
			g_formTracks.clear();
			g_pollIndex = 0;
			g_framesSinceSummary = 0;
			g_pendingMarkFrames = 0;
			g_lastSummary = std::chrono::steady_clock::now();
			{
				std::scoped_lock drawLock(g_drawLock);
				g_draws.clear();
				g_matLastDrawFrame.clear();
			}
			g_lineBudget = kLinesPerPoll;
			Write("");
			Write(std::format("######## Live water log started (v{}) ########", Plugin::VERSION.string()));
			Write("Tags: [engine] = changed while no plugin code ran; [engine, before X] = changed since the last frame, flushed right");
			Write("      before plugin action X; [X] = changed by plugin action X itself. Starting from the menu writes a full");
			Write("      snapshot marked LOOKS CORRECT; click the button again while the bug is visible for a second one and to stop.");
			LogPluginState("");
			g_enabled.store(true);
		} else {
			g_enabled.store(false);
			Write("######## Live water log stopped ########");
		}
	}

	void OnFrame()
	{
		if (!IsEnabled()) {
			return;
		}
		std::scoped_lock lock(g_stateLock);
		g_frame.fetch_add(1, std::memory_order_relaxed);
		g_drawLineBudget.store(static_cast<std::int32_t>(kDrawLinesPerSecond), std::memory_order_relaxed);
		++g_framesSinceSummary;

		Poll("engine", Mode::Frame);

		if (g_pendingMarkFrames > 0 && --g_pendingMarkFrames == 0) {
			Dump(g_pendingMarkLabel);
		}

		const auto now = std::chrono::steady_clock::now();
		if (now - g_lastSummary >= std::chrono::seconds(10)) {
			g_lastSummary = now;
			g_lineBudget = 2;
			std::size_t waterSystemGeoms = 0;
			for (const auto& [key, g] : g_prev.geoms) {
				waterSystemGeoms += g.inWaterSystem ? 1 : 0;
			}
			Line("[stats] last {} frames: {} water draws ({} LOD-classified), {} draw-time material rebuilds, {} SetupMaterial calls; "
				 "{} water geometries ({} water-system), {} materials; {} draw lines suppressed",
				g_framesSinceSummary, g_drawCount.exchange(0), g_lodDrawCount.exchange(0), g_rebuildCount.exchange(0),
				g_setupMaterialCount.exchange(0), g_prev.geoms.size(), waterSystemGeoms, g_prev.mats.size(), g_drawLinesSuppressed.exchange(0));
			LogPluginState("[stats] ");
			g_framesSinceSummary = 0;
		}
	}

	void Mark(std::string_view a_label)
	{
		if (!IsEnabled()) {
			return;
		}
		std::scoped_lock lock(g_stateLock);
		Poll(std::format("engine, before mark '{}'", a_label), Mode::PreScope);
		Dump(a_label);
	}

	void MarkAfterFrames(std::string_view a_label, std::uint32_t a_frames)
	{
		if (!IsEnabled()) {
			return;
		}
		if (a_frames == 0) {
			Mark(a_label);
			return;
		}
		std::scoped_lock lock(g_stateLock);
		g_pendingMarkLabel = a_label;
		g_pendingMarkFrames = a_frames;
	}

	void NoteSetupMaterial(const RE::BSShaderMaterial*)
	{
		if (IsEnabled()) {
			g_setupMaterialCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void NoteDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_isLod, bool a_rebuiltMaterial)
	{
		if (!IsEnabled() || !a_pass || !a_pass->geometry) {
			return;
		}
		g_drawCount.fetch_add(1, std::memory_order_relaxed);
		if (a_isLod) {
			g_lodDrawCount.fetch_add(1, std::memory_order_relaxed);
		}
		if (a_rebuiltMaterial) {
			g_rebuildCount.fetch_add(1, std::memory_order_relaxed);
		}

		const auto* geom = a_pass->geometry;
		const void* material = a_pass->shaderProperty ? a_pass->shaderProperty->material : nullptr;
		const auto  technique = a_technique;
		const auto  frame = g_frame.load(std::memory_order_relaxed);
		const auto replaced = [&]() {
			return a_technique != a_pass->passEnum ? std::format(" (replacing {})", DecodeTechnique(a_pass->passEnum)) : std::string{};
		};

		std::string message;
		{
			std::scoped_lock lock(g_drawLock);
			if (material) {
				g_matLastDrawFrame[material] = frame;
			}

			auto [it, inserted] = g_draws.try_emplace(geom);
			auto& d = it->second;
			if (inserted) {
				d.isLod = a_isLod;
				d.material = material;
				d.techniques[0] = technique;
				d.techniqueCount = 1;
				const char* name = geom->name.c_str();
				message = std::format("[draw] first draw of {} '{}' as {}: technique {}{} mat {} parents: {}",
					Ptr(geom), (name && name[0]) ? name : "<unnamed>", a_isLod ? "LOD-classified" : "normal",
					DecodeTechnique(technique), replaced(), Ptr(material), ParentChain(geom));
			} else {
				std::string changes;
				if (d.isLod != a_isLod) {
					Append(changes, std::format("classification {} -> {}", d.isLod ? "LOD" : "normal", a_isLod ? "LOD" : "normal"));
					d.isLod = a_isLod;
				}
				if (d.material != material) {
					Append(changes, std::format("material {} -> {}", Ptr(d.material), Ptr(material)));
					d.material = material;
				}
				const bool known = std::find(d.techniques, d.techniques + d.techniqueCount, technique) != d.techniques + d.techniqueCount;
				if (!known) {
					std::string previous;
					for (std::uint8_t t = 0; t < d.techniqueCount; ++t) {
						previous += ' ';
						previous += DecodeTechnique(d.techniques[t]);
					}
					Append(changes, std::format("NEW technique {}{} (seen before:{})", DecodeTechnique(technique), replaced(), previous));
					if (d.techniqueCount < std::size(d.techniques)) {
						d.techniques[d.techniqueCount++] = technique;
					} else {
						d.techniques[std::size(d.techniques) - 1] = technique;
					}
				}
				if (!changes.empty()) {
					const char* name = geom->name.c_str();
					message = std::format("[draw] {} '{}': {}", Ptr(geom), (name && name[0]) ? name : "<unnamed>", changes);
				}
			}
		}
		if (!message.empty()) {
			DrawLine(message);
		}
	}

	Scope::Scope(const char* a_name) :
		name(a_name)
	{
		if (t_scopeDepth++ == 0 && IsEnabled()) {
			active = true;
			std::scoped_lock lock(g_stateLock);
			Poll(std::format("engine, before {}", name), Mode::PreScope);
			g_lineBudget = (std::max)(g_lineBudget, 2u);
			Line("----> BEGIN {} (frame {})", name, g_frame.load(std::memory_order_relaxed));
			LogPluginState("      ");
		}
	}

	Scope::~Scope()
	{
		--t_scopeDepth;
		if (active && IsEnabled()) {
			std::scoped_lock lock(g_stateLock);
			Poll(name, Mode::Scope);
			g_lineBudget = (std::max)(g_lineBudget, 1u);
			Line("<---- END {}", name);
		}
	}
}
