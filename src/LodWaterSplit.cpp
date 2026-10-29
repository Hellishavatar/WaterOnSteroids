#include "LodWaterSplit.h"
#include "Settings.h"

#include <RE/B/BGSTerrainChunk.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSTriShape.h>
#include <RE/N/NiAlphaProperty.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiSkinPartition.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr float         kSameHeight = 2.0f;
	constexpr std::uint32_t kDetailedSplits = 20;
	constexpr std::uint32_t kSummaryEvery = 200;

	std::atomic<std::uint32_t> shapesSplit{ 0 };
	std::atomic<std::uint32_t> piecesMade{ 0 };
	std::atomic<std::uint32_t> splitFailures{ 0 };
	std::atomic<bool>          rawCopyReported{ false };

	struct KnownHeight
	{
		const RE::BSGraphics::TriShape* rendererData{ nullptr };
		float                           localZ{ 0.0f };
	};

	std::mutex                                         knownLock;
	std::unordered_map<const RE::BSGeometry*, KnownHeight> knownHeights;

	using Factory_t = RE::BSTriShape* (*)(void* a_manager, std::uint32_t a_triangleCount, std::uint32_t a_vertexCount, const RE::NiPoint3* a_positions,
		const void* a_optional1, const void* a_optional2, const std::uint16_t* a_indices);

	RE::BSTriShape* CreateShape(std::uint32_t a_triangleCount, std::uint32_t a_vertexCount, const RE::NiPoint3* a_positions, const std::uint16_t* a_indices)
	{
		static REL::Relocation<void**> manager{ RELOCATION_ID(523950, 410530) };
		void* object = *manager.get();
		if (!object) {
			return nullptr;
		}
		const auto factory = reinterpret_cast<Factory_t>((*static_cast<void***>(object))[1]);
		return factory(object, a_triangleCount, a_vertexCount, a_positions, nullptr, nullptr, a_indices);
	}

	void ReadPosition(const std::uint8_t* a_raw, std::uint32_t a_stride, bool a_full, std::uint32_t a_index, float (&a_out)[3])
	{
		const std::uint8_t* vertex = a_raw + static_cast<std::size_t>(a_index) * a_stride;
		if (a_full) {
			std::memcpy(a_out, vertex, sizeof(a_out));
			return;
		}
		std::uint16_t half[3];
		std::memcpy(half, vertex, sizeof(half));
		for (int k = 0; k < 3; ++k) {
			const std::uint32_t sign = static_cast<std::uint32_t>(half[k] & 0x8000u) << 16;
			const std::uint32_t exponent = (half[k] >> 10) & 0x1Fu;
			const std::uint32_t mantissa = half[k] & 0x3FFu;
			if (exponent == 0 || exponent == 0x1F) {
				a_out[k] = exponent == 0 ? 0.0f : std::bit_cast<float>(sign | 0x7F800000u);
			} else {
				a_out[k] = std::bit_cast<float>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
			}
		}
	}

	bool PositionsFitBound(const std::uint8_t* a_raw, std::uint32_t a_count, std::uint32_t a_stride, bool a_full, const RE::NiBound& a_bound)
	{
		if (a_count == 0 || a_stride < (a_full ? 12u : 6u)) {
			return false;
		}
		const float limit = a_bound.radius * 1.01f + 1.0f;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			float p[3];
			ReadPosition(a_raw, a_stride, a_full, i, p);
			const float dx = p[0] - a_bound.center.x;
			const float dy = p[1] - a_bound.center.y;
			const float dz = p[2] - a_bound.center.z;
			if (!(dx * dx + dy * dy + dz * dz <= limit * limit)) {
				return false;
			}
		}
		return true;
	}

	struct Piece
	{
		float                    localZ{ 0.0f };
		std::vector<std::uint16_t> sourceTriangles;
	};

	enum class SplitResult
	{
		kFlat,
		kSplit,
		kFailed
	};

	SplitResult SplitShape(RE::NiNode* a_parent, RE::BSTriShape* a_shape)
	{
		auto& runtime = a_shape->GetGeometryRuntimeData();
		auto* data = runtime.rendererData;
		if (!data || !data->rawVertexData || !data->rawIndexData || runtime.skinInstance) {
			return SplitResult::kFlat;
		}

		const auto          tri = a_shape->GetTrishapeRuntimeData();
		const std::uint32_t vertexCount = tri.vertexCount;
		const std::uint32_t triangleCount = tri.triangleCount;
		const auto          bits = std::bit_cast<std::uint64_t>(data->vertexDesc);
		const std::uint32_t stride = static_cast<std::uint32_t>(bits & 0xF) * 4;
		const auto*         raw = data->rawVertexData;
		const auto&         bound = a_shape->GetModelData().modelBound;

		bool full = data->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
		if (!PositionsFitBound(raw, vertexCount, stride, full, bound)) {
			full = !full;
			if (!PositionsFitBound(raw, vertexCount, stride, full, bound)) {
				return SplitResult::kFlat;
			}
		}

		std::map<long, std::vector<std::uint16_t>> byHeight;
		for (std::uint32_t t = 0; t < triangleCount; ++t) {
			float z[3];
			for (int k = 0; k < 3; ++k) {
				const std::uint16_t index = data->rawIndexData[t * 3 + k];
				if (index >= vertexCount) {
					return SplitResult::kFlat;
				}
				float p[3];
				ReadPosition(raw, stride, full, index, p);
				z[k] = p[2];
			}
			const float lo = (std::min)({ z[0], z[1], z[2] });
			const float hi = (std::max)({ z[0], z[1], z[2] });
			if (hi - lo > kSameHeight) {
				return SplitResult::kFlat;
			}
			byHeight[std::lround(lo)].push_back(static_cast<std::uint16_t>(t));
		}

		std::vector<Piece> pieces;
		for (auto& [height, triangles] : byHeight) {
			if (!pieces.empty() && static_cast<float>(height) - pieces.back().localZ <= kSameHeight) {
				auto& last = pieces.back().sourceTriangles;
				last.insert(last.end(), triangles.begin(), triangles.end());
			} else {
				pieces.push_back({ static_cast<float>(height), std::move(triangles) });
			}
		}
		if (pieces.size() < 2) {
			return SplitResult::kFlat;
		}

		std::vector<RE::NiPointer<RE::BSTriShape>> made;
		std::vector<float>                         madeHeights;
		for (const auto& piece : pieces) {
			std::vector<std::int32_t>  remap(vertexCount, -1);
			std::vector<RE::NiPoint3>  positions;
			std::vector<std::uint16_t> indices;
			float                      topZ = -(std::numeric_limits<float>::max)();
			for (const auto t : piece.sourceTriangles) {
				for (int k = 0; k < 3; ++k) {
					const std::uint16_t source = data->rawIndexData[t * 3 + k];
					if (remap[source] < 0) {
						float p[3];
						ReadPosition(raw, stride, full, source, p);
						remap[source] = static_cast<std::int32_t>(positions.size());
						positions.push_back({ p[0], p[1], p[2] });
						topZ = (std::max)(topZ, p[2]);
					}
					indices.push_back(static_cast<std::uint16_t>(remap[source]));
				}
			}

			RE::NiPointer<RE::BSTriShape> shape{ CreateShape(static_cast<std::uint32_t>(indices.size() / 3), static_cast<std::uint32_t>(positions.size()), positions.data(), indices.data()) };
			if (!shape) {
				if (splitFailures < 10) {
					logger::warn("LOD water split: the game's shape factory returned nothing for a {}-triangle piece of shape {}; the shape is left as it is.",
						indices.size() / 3, fmt::ptr(a_shape));
				}
				return SplitResult::kFailed;
			}
			made.push_back(shape);
			madeHeights.push_back(topZ);
		}

		std::vector<RE::NiPointer<RE::BSShaderProperty>> properties{ runtime.shaderProperty };
		for (std::size_t i = 1; i < made.size(); ++i) {
			RE::BSShaderProperty* copy = nullptr;
			if (runtime.shaderProperty) {
				auto* clone = runtime.shaderProperty->Clone();
				copy = clone ? netimmerse_cast<RE::BSShaderProperty*>(clone) : nullptr;
				if (!copy) {
					if (splitFailures < 10) {
						logger::warn("LOD water split: could not copy the water shader property of shape {}; the shape is left as it is.", fmt::ptr(a_shape));
					}
					return SplitResult::kFailed;
				}
			}
			properties.emplace_back(copy);
		}

		for (std::size_t i = 0; i < made.size(); ++i) {
			auto* shape = made[i].get();
			shape->name = a_shape->name;
			shape->local = a_shape->local;
			shape->world = a_shape->world;
			shape->previousWorld = a_shape->previousWorld;
			shape->GetFlags() = a_shape->GetFlags();
			const auto& pieceBound = shape->GetModelData().modelBound;
			shape->worldBound.center = shape->world * pieceBound.center;
			shape->worldBound.radius = pieceBound.radius * shape->world.scale;
			auto& pieceRuntime = shape->GetGeometryRuntimeData();
			pieceRuntime.shaderProperty = properties[i];
			pieceRuntime.alphaProperty = runtime.alphaProperty;
			{
				std::scoped_lock lock(knownLock);
				if (knownHeights.size() > 8192) {
					knownHeights.clear();
				}
				knownHeights[shape] = { pieceRuntime.rendererData, madeHeights[i] };
			}
		}
		a_parent->DetachChild(a_shape);
		a_parent->AttachChild(made.front().get(), true);
		for (std::size_t i = 1; i < made.size(); ++i) {
			a_parent->AttachChild(made[i].get(), false);
		}

		const std::uint32_t count = ++shapesSplit;
		piecesMade += static_cast<std::uint32_t>(made.size());
		if (!rawCopyReported.exchange(true)) {
			auto* pieceData = made.front()->GetGeometryRuntimeData().rendererData;
			logger::info("LOD water split: new pieces {} a raw vertex copy (vertex desc 0x{:X}); the source shape {} when split.",
				pieceData && pieceData->rawVertexData ? "keep" : "do NOT keep",
				pieceData ? std::bit_cast<std::uint64_t>(pieceData->vertexDesc) : 0,
				runtime.shaderProperty ? "already had a shader property" : "had no shader property yet");
		}
		if (count <= kDetailedSplits) {
			std::string heights;
			for (std::size_t i = 0; i < pieces.size(); ++i) {
				heights += std::format("{}{} ({} tris)", i ? ", " : "", pieces[i].localZ, pieces[i].sourceTriangles.size());
			}
			logger::info("LOD water split: shape {} ({} triangles) split into {} pieces at local heights {}.", fmt::ptr(a_shape), triangleCount, made.size(), heights);
		}
		if (count % kSummaryEvery == 0) {
			logger::info("LOD water split: {} mixed-height shapes split into {} pieces so far ({} could not be split).", count, piecesMade.load(), splitFailures.load());
		}
		return SplitResult::kSplit;
	}

	void SplitChunkWater(RE::NiNode* a_water)
	{
		std::vector<RE::NiPointer<RE::BSTriShape>> shapes;
		for (auto& child : a_water->GetChildren()) {
			auto* geometry = child ? child->AsGeometry() : nullptr;
			auto* shape = geometry ? geometry->AsTriShape() : nullptr;
			if (shape && std::strcmp(shape->GetRTTI()->GetName(), "BSTriShape") == 0) {
				shapes.emplace_back(shape);
			}
		}
		for (auto& shape : shapes) {
			if (SplitShape(a_water, shape.get()) == SplitResult::kFailed) {
				++splitFailures;
			}
		}
	}

	struct Hook_TerrainChunkCtor
	{
		static RE::BGSTerrainChunk* thunk(RE::BGSTerrainChunk* a_this, RE::BGSTerrainNode* a_node, RE::BSStream& a_stream, std::int32_t a_cellX, std::int32_t a_cellY)
		{
			auto* chunk = func(a_this, a_node, a_stream, a_cellX, a_cellY);
			const auto& settings = Settings::GetFastSnapshot();
			if (chunk && chunk->waterNode && !chunk->prepared && settings.enabled && settings.fullShaderDistantWater) {
				SplitChunkWater(chunk->waterNode.get());
			}
			return chunk;
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void LodWaterSplit::Install()
{
	if (Detour::Function<Hook_TerrainChunkCtor>(REL::RelocationID(30932, 31735).address())) {
		logger::info("LOD water split: hooked BGSTerrainChunk::Ctor; distant water shapes holding several water heights are split into one piece per height when a terrain block loads.");
	} else {
		logger::error("LOD water split: failed to hook BGSTerrainChunk::Ctor; distant water is not split.");
	}
}

std::optional<float> LodWaterSplit::KnownLocalHeight(const RE::BSGeometry* a_geometry, const RE::BSGraphics::TriShape* a_rendererData)
{
	std::scoped_lock lock(knownLock);
	const auto it = knownHeights.find(a_geometry);
	if (it == knownHeights.end() || it->second.rendererData != a_rendererData) {
		return std::nullopt;
	}
	return it->second.localZ;
}
