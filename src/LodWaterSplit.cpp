#include "LodWaterSplit.h"
#include "CellWaterMap.h"
#include "Settings.h"

#include <RE/B/BGSTerrainChunk.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSSubIndexTriShape.h>
#include <RE/B/BSTriShape.h>
#include <RE/N/NiAlphaProperty.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiSkinPartition.h>
#include <RE/T/TESWaterForm.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
	static_assert(sizeof(RE::SegmentData) == 0x14, "Unexpected sub-index segment size");
	static_assert(offsetof(RE::BSSubIndexTriShape::SUB_INDEX_TRISHAPE_RUNTIME_DATA, unk170) == 0x10, "Unexpected sub-index dirty flag offset");
	static_assert(offsetof(RE::BSSubIndexTriShape::SUB_INDEX_TRISHAPE_RUNTIME_DATA, nonSegmented) == 0x11, "Unexpected sub-index flag offset");

	constexpr float         kSameHeight = 2.0f;
	constexpr float         kCellSize = 4096.0f;
	constexpr float         kMinClipArea = 1.0f;
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
		RE::TESWaterForm*               type{ nullptr };
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

	using Polygon = std::vector<RE::NiPoint3>;

	Polygon ClipPolygon(const Polygon& a_in, bool a_alongX, float a_bound, bool a_keepBelow)
	{
		Polygon out;
		const std::size_t count = a_in.size();
		for (std::size_t i = 0; i < count; ++i) {
			const auto& current = a_in[i];
			const auto& next = a_in[(i + 1) % count];
			const float a = a_alongX ? current.x : current.y;
			const float b = a_alongX ? next.x : next.y;
			const bool  currentIn = a_keepBelow ? a <= a_bound : a >= a_bound;
			const bool  nextIn = a_keepBelow ? b <= a_bound : b >= a_bound;
			if (currentIn) {
				out.push_back(current);
			}
			if (currentIn != nextIn) {
				const float t = (a_bound - a) / (b - a);
				out.push_back({ current.x + (next.x - current.x) * t, current.y + (next.y - current.y) * t, current.z + (next.z - current.z) * t });
			}
		}
		return out;
	}

	float PolygonArea(const Polygon& a_polygon)
	{
		float twice = 0.0f;
		for (std::size_t i = 0; i < a_polygon.size(); ++i) {
			const auto& a = a_polygon[i];
			const auto& b = a_polygon[(i + 1) % a_polygon.size()];
			twice += a.x * b.y - b.x * a.y;
		}
		return std::abs(twice) * 0.5f;
	}

	struct PieceBuilder
	{
		float                                                 localZ{ 0.0f };
		RE::TESWaterForm*                                     type{ nullptr };
		std::vector<RE::NiPoint3>                             positions;
		std::vector<std::uint16_t>                            indices;
		std::map<std::array<std::uint32_t, 3>, std::uint16_t> lookup;
		float                                                 topZ{ -(std::numeric_limits<float>::max)() };
		bool                                                  overflow{ false };

		void AddVertex(const RE::NiPoint3& a_point)
		{
			const std::array<std::uint32_t, 3> key{ std::bit_cast<std::uint32_t>(a_point.x), std::bit_cast<std::uint32_t>(a_point.y), std::bit_cast<std::uint32_t>(a_point.z) };
			if (const auto it = lookup.find(key); it != lookup.end()) {
				indices.push_back(it->second);
				return;
			}
			if (positions.size() >= (std::numeric_limits<std::uint16_t>::max)()) {
				overflow = true;
				indices.push_back(0);
				return;
			}
			const auto index = static_cast<std::uint16_t>(positions.size());
			lookup.emplace(key, index);
			positions.push_back(a_point);
			topZ = (std::max)(topZ, a_point.z);
			indices.push_back(index);
		}

		void AddTriangle(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, const RE::NiPoint3& a_c)
		{
			AddVertex(a_a);
			AddVertex(a_b);
			AddVertex(a_c);
		}

		[[nodiscard]] std::size_t TriangleCount() const { return indices.size() / 3; }
	};

	struct CellFrame
	{
		float        originX{ 0.0f };
		float        originY{ 0.0f };
		float        scale{ 1.0f };
		float        heightZ{ 0.0f };
		bool         usable{ false };

		[[nodiscard]] float WorldX(float a_local) const { return originX + scale * a_local; }
		[[nodiscard]] float WorldY(float a_local) const { return originY + scale * a_local; }
		[[nodiscard]] float LocalX(float a_world) const { return (a_world - originX) / scale; }
		[[nodiscard]] float LocalY(float a_world) const { return (a_world - originY) / scale; }
		[[nodiscard]] std::int32_t CellX(float a_local) const { return static_cast<std::int32_t>(std::floor(WorldX(a_local) / kCellSize)); }
		[[nodiscard]] std::int32_t CellY(float a_local) const { return static_cast<std::int32_t>(std::floor(WorldY(a_local) / kCellSize)); }
	};

	CellFrame FrameFor(const RE::BSTriShape* a_shape, std::int32_t a_cellX, std::int32_t a_cellY)
	{
		const auto& local = a_shape->local;
		const auto& rot = local.rotate.entry;
		CellFrame   frame;
		frame.scale = local.scale;
		frame.originX = static_cast<float>(a_cellX) * kCellSize + local.translate.x;
		frame.originY = static_cast<float>(a_cellY) * kCellSize + local.translate.y;
		frame.heightZ = local.translate.z;
		frame.usable = frame.scale > 0.0f &&
		               std::abs(rot[0][0] - 1.0f) < 1e-4f && std::abs(rot[1][1] - 1.0f) < 1e-4f && std::abs(rot[2][2] - 1.0f) < 1e-4f &&
		               std::abs(rot[0][1]) < 1e-4f && std::abs(rot[0][2]) < 1e-4f && std::abs(rot[1][0]) < 1e-4f &&
		               std::abs(rot[1][2]) < 1e-4f && std::abs(rot[2][0]) < 1e-4f && std::abs(rot[2][1]) < 1e-4f;
		return frame;
	}

	void BuildTypedPieces(const Piece& a_piece, const std::vector<std::array<RE::NiPoint3, 3>>& a_triangles, const CellFrame& a_frame, std::vector<PieceBuilder>& a_out)
	{
		auto single = [&](RE::TESWaterForm* a_type) -> PieceBuilder& {
			for (auto& builder : a_out) {
				if (builder.localZ == a_piece.localZ && builder.type == a_type) {
					return builder;
				}
			}
			auto& builder = a_out.emplace_back();
			builder.localZ = a_piece.localZ;
			builder.type = a_type;
			return builder;
		};

		std::int32_t minX = (std::numeric_limits<std::int32_t>::max)();
		std::int32_t minY = (std::numeric_limits<std::int32_t>::max)();
		std::int32_t maxX = (std::numeric_limits<std::int32_t>::min)();
		std::int32_t maxY = (std::numeric_limits<std::int32_t>::min)();
		if (a_frame.usable) {
			for (const auto t : a_piece.sourceTriangles) {
				for (const auto& p : a_triangles[t]) {
					minX = (std::min)(minX, a_frame.CellX(p.x));
					maxX = (std::max)(maxX, a_frame.CellX(p.x));
					minY = (std::min)(minY, a_frame.CellY(p.y));
					maxY = (std::max)(maxY, a_frame.CellY(p.y));
				}
			}
		}
		const auto types = a_frame.usable ? CellWaterMap::TypesAt(minX, minY, maxX, maxY, a_frame.heightZ + a_frame.scale * a_piece.localZ) : std::vector<RE::TESWaterForm*>{};
		const auto typeAt = [&](std::int32_t a_x, std::int32_t a_y) -> RE::TESWaterForm* {
			return types[static_cast<std::size_t>(a_y - minY) * (maxX - minX + 1) + (a_x - minX)];
		};

		const std::size_t first = a_out.size();
		for (const auto t : a_piece.sourceTriangles) {
			const auto& triangle = a_triangles[t];
			if (types.empty()) {
				single(nullptr).AddTriangle(triangle[0], triangle[1], triangle[2]);
				continue;
			}
			std::int32_t tMinX = (std::numeric_limits<std::int32_t>::max)();
			std::int32_t tMinY = (std::numeric_limits<std::int32_t>::max)();
			std::int32_t tMaxX = (std::numeric_limits<std::int32_t>::min)();
			std::int32_t tMaxY = (std::numeric_limits<std::int32_t>::min)();
			for (const auto& p : triangle) {
				tMinX = (std::min)(tMinX, a_frame.CellX(p.x));
				tMaxX = (std::max)(tMaxX, a_frame.CellX(p.x));
				tMinY = (std::min)(tMinY, a_frame.CellY(p.y));
				tMaxY = (std::max)(tMaxY, a_frame.CellY(p.y));
			}
			RE::TESWaterForm* shared = typeAt(tMinX, tMinY);
			bool              uniform = true;
			for (std::int32_t y = tMinY; y <= tMaxY && uniform; ++y) {
				for (std::int32_t x = tMinX; x <= tMaxX && uniform; ++x) {
					uniform = typeAt(x, y) == shared;
				}
			}
			if (uniform) {
				single(shared).AddTriangle(triangle[0], triangle[1], triangle[2]);
				continue;
			}
			for (std::int32_t y = tMinY; y <= tMaxY; ++y) {
				for (std::int32_t x = tMinX; x <= tMaxX; ++x) {
					Polygon polygon{ triangle[0], triangle[1], triangle[2] };
					polygon = ClipPolygon(polygon, true, a_frame.LocalX(static_cast<float>(x) * kCellSize), false);
					polygon = ClipPolygon(polygon, true, a_frame.LocalX(static_cast<float>(x + 1) * kCellSize), true);
					polygon = ClipPolygon(polygon, false, a_frame.LocalY(static_cast<float>(y) * kCellSize), false);
					polygon = ClipPolygon(polygon, false, a_frame.LocalY(static_cast<float>(y + 1) * kCellSize), true);
					if (polygon.size() < 3 || PolygonArea(polygon) < kMinClipArea) {
						continue;
					}
					auto& builder = single(typeAt(x, y));
					for (std::size_t k = 1; k + 1 < polygon.size(); ++k) {
						builder.AddTriangle(polygon[0], polygon[k], polygon[k + 1]);
					}
				}
			}
		}

		std::size_t largest = a_out.size();
		for (std::size_t i = first; i < a_out.size(); ++i) {
			if (a_out[i].type && (largest == a_out.size() || a_out[i].TriangleCount() > a_out[largest].TriangleCount())) {
				largest = i;
			}
		}
		if (largest == a_out.size()) {
			return;
		}
		for (std::size_t i = first; i < a_out.size(); ++i) {
			if (a_out[i].type || a_out[i].TriangleCount() == 0) {
				continue;
			}
			auto& target = a_out[largest];
			auto& unknown = a_out[i];
			for (std::size_t k = 0; k + 2 < unknown.indices.size(); k += 3) {
				target.AddTriangle(unknown.positions[unknown.indices[k]], unknown.positions[unknown.indices[k + 1]], unknown.positions[unknown.indices[k + 2]]);
			}
			unknown.indices.clear();
		}
		std::erase_if(a_out, [](const PieceBuilder& a_builder) { return a_builder.TriangleCount() == 0; });
	}

	enum class SplitResult
	{
		kFlat,
		kSplit,
		kFailed
	};

	SplitResult SplitShape(RE::NiNode* a_parent, RE::BSTriShape* a_shape, std::int32_t a_cellX, std::int32_t a_cellY)
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
		std::vector<std::array<RE::NiPoint3, 3>>   triangles(triangleCount);
		for (std::uint32_t t = 0; t < triangleCount; ++t) {
			float z[3];
			for (int k = 0; k < 3; ++k) {
				const std::uint16_t index = data->rawIndexData[t * 3 + k];
				if (index >= vertexCount) {
					return SplitResult::kFlat;
				}
				float p[3];
				ReadPosition(raw, stride, full, index, p);
				triangles[t][k] = { p[0], p[1], p[2] };
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
		const auto                frame = FrameFor(a_shape, a_cellX, a_cellY);
		std::vector<PieceBuilder> builders;
		for (const auto& piece : pieces) {
			BuildTypedPieces(piece, triangles, frame, builders);
		}
		if (builders.size() < 2) {
			return SplitResult::kFlat;
		}
		for (const auto& builder : builders) {
			if (builder.overflow) {
				return SplitResult::kFlat;
			}
		}

		std::vector<RE::NiPointer<RE::BSTriShape>> made;
		for (const auto& builder : builders) {
			RE::NiPointer<RE::BSTriShape> shape{ CreateShape(static_cast<std::uint32_t>(builder.TriangleCount()), static_cast<std::uint32_t>(builder.positions.size()), builder.positions.data(), builder.indices.data()) };
			if (!shape) {
				if (splitFailures < 10) {
					logger::warn("LOD water split: the game's shape factory returned nothing for a {}-triangle piece of shape {}; the shape is left as it is.",
						builder.TriangleCount(), fmt::ptr(a_shape));
				}
				return SplitResult::kFailed;
			}
			made.push_back(shape);
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
				knownHeights[shape] = { pieceRuntime.rendererData, builders[i].topZ, builders[i].type };
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
			for (std::size_t i = 0; i < builders.size(); ++i) {
				heights += std::format("{}{} type {:08X} ({} tris)", i ? ", " : "", builders[i].localZ, builders[i].type ? builders[i].type->GetFormID() : 0, builders[i].TriangleCount());
			}
			logger::info("LOD water split: shape {} ({} triangles, block {},{}) split into {} pieces at local heights {}.", fmt::ptr(a_shape), triangleCount, a_cellX, a_cellY, made.size(), heights);
		}
		if (count % kSummaryEvery == 0) {
			logger::info("LOD water split: {} shapes with several water heights or types split into {} pieces so far ({} could not be split).", count, piecesMade.load(), splitFailures.load());
		}
		return SplitResult::kSplit;
	}

	void RecordPiece(RE::BSGeometry* a_shape, float a_localZ, RE::TESWaterForm* a_type)
	{
		std::scoped_lock lock(knownLock);
		if (knownHeights.size() > 8192) {
			knownHeights.clear();
		}
		knownHeights[a_shape] = { a_shape->GetGeometryRuntimeData().rendererData, a_localZ, a_type };
	}

	SplitResult SplitSubIndexShape(RE::NiNode* a_parent, RE::BSSubIndexTriShape* a_shape, std::int32_t a_cellX, std::int32_t a_cellY)
	{
		auto& runtime = a_shape->GetGeometryRuntimeData();
		auto* data = runtime.rendererData;
		auto& segments = a_shape->GetSubIndexedTrishapeRuntimeData();
		if (!data || !data->rawVertexData || !data->rawIndexData || runtime.skinInstance || !segments.segmentData || segments.nonSegmented) {
			return SplitResult::kFlat;
		}

		const auto          tri = a_shape->GetTrishapeRuntimeData();
		const std::uint32_t vertexCount = tri.vertexCount;
		const std::uint32_t triangleCount = tri.triangleCount;
		const std::uint32_t segmentCount = segments.numSegments;
		const auto          side = static_cast<std::uint32_t>(std::lround(std::sqrt(static_cast<double>(segmentCount))));
		if (segmentCount == 0 || side * side != segmentCount) {
			return SplitResult::kFlat;
		}

		std::uint32_t covered = 0;
		for (std::uint32_t i = 0; i < segmentCount; ++i) {
			const auto& segment = segments.segmentData[i];
			if (segment.index % 3 != 0 || segment.index / 3 + segment.unkTriCount > triangleCount) {
				return SplitResult::kFlat;
			}
			covered += segment.unkTriCount;
		}
		if (covered != triangleCount) {
			return SplitResult::kFlat;
		}

		const auto          bits = std::bit_cast<std::uint64_t>(data->vertexDesc);
		const std::uint32_t stride = static_cast<std::uint32_t>(bits & 0xF) * 4;
		const auto*         raw = data->rawVertexData;
		bool                full = data->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
		if (!PositionsFitBound(raw, vertexCount, stride, full, a_shape->GetModelData().modelBound)) {
			full = !full;
			if (!PositionsFitBound(raw, vertexCount, stride, full, a_shape->GetModelData().modelBound)) {
				return SplitResult::kFlat;
			}
		}

		float lo = (std::numeric_limits<float>::max)();
		float hi = -(std::numeric_limits<float>::max)();
		for (std::uint32_t i = 0; i < triangleCount * 3; ++i) {
			const std::uint16_t index = data->rawIndexData[i];
			if (index >= vertexCount) {
				return SplitResult::kFlat;
			}
			float point[3];
			ReadPosition(raw, stride, full, index, point);
			lo = (std::min)(lo, point[2]);
			hi = (std::max)(hi, point[2]);
		}
		if (hi - lo > kSameHeight) {
			return SplitResult::kFlat;
		}

		const auto frame = FrameFor(a_shape, a_cellX, a_cellY);
		if (!frame.usable) {
			return SplitResult::kFlat;
		}
		const auto last = static_cast<std::int32_t>(side) - 1;
		const auto types = CellWaterMap::TypesAt(a_cellX, a_cellY, a_cellX + last, a_cellY + last, frame.heightZ + frame.scale * hi);
		if (types.empty()) {
			return SplitResult::kFlat;
		}

		std::vector<RE::TESWaterForm*> segmentTypes(segmentCount, nullptr);
		std::map<RE::TESWaterForm*, std::uint32_t> triangles;
		for (std::uint32_t i = 0; i < segmentCount; ++i) {
			const std::uint32_t x = i / side;
			const std::uint32_t y = i % side;
			segmentTypes[i] = types[static_cast<std::size_t>(y) * side + x];
			if (segments.segmentData[i].unkTriCount) {
				triangles[segmentTypes[i]] += segments.segmentData[i].unkTriCount;
			}
		}
		RE::TESWaterForm* largest = nullptr;
		std::uint32_t     largestCount = 0;
		for (const auto& [type, count] : triangles) {
			if (type && count > largestCount) {
				largest = type;
				largestCount = count;
			}
		}
		if (!largest) {
			return SplitResult::kFlat;
		}
		for (auto& type : segmentTypes) {
			if (!type) {
				type = largest;
			}
		}
		std::vector<RE::TESWaterForm*> groups;
		for (std::uint32_t i = 0; i < segmentCount; ++i) {
			if (segments.segmentData[i].unkTriCount && std::find(groups.begin(), groups.end(), segmentTypes[i]) == groups.end()) {
				groups.push_back(segmentTypes[i]);
			}
		}
		if (groups.size() < 2) {
			return SplitResult::kFlat;
		}

		std::vector<RE::NiPointer<RE::BSSubIndexTriShape>> made;
		std::vector<std::uint32_t>                         madeTriangles;
		for (auto* group : groups) {
			RE::NiPointer<RE::NiObject>         copy{ a_shape->Clone() };
			RE::NiPointer<RE::BSSubIndexTriShape> piece{ copy ? netimmerse_cast<RE::BSSubIndexTriShape*>(copy.get()) : nullptr };
			auto*                               pieceSegments = piece ? &piece->GetSubIndexedTrishapeRuntimeData() : nullptr;
			if (!piece || !pieceSegments->segmentData || pieceSegments->numSegments != segmentCount) {
				if (splitFailures < 10) {
					logger::warn("LOD water split: could not copy sub-index water shape {}; the shape is left as it is.", fmt::ptr(a_shape));
				}
				return SplitResult::kFailed;
			}

			std::vector<RE::NiPoint3>  positions;
			std::vector<std::uint16_t> indices;
			std::vector<std::int32_t>  remap(vertexCount, -1);
			std::uint32_t              start = 0;
			for (std::uint32_t i = 0; i < segmentCount; ++i) {
				const auto& source = segments.segmentData[i];
				auto&       target = pieceSegments->segmentData[i];
				const std::uint32_t count = segmentTypes[i] == group ? source.unkTriCount : 0;
				for (std::uint32_t k = 0; k < count * 3; ++k) {
					const std::uint16_t index = data->rawIndexData[source.index + k];
					if (remap[index] < 0) {
						float point[3];
						ReadPosition(raw, stride, full, index, point);
						remap[index] = static_cast<std::int32_t>(positions.size());
						positions.push_back({ point[0], point[1], point[2] });
					}
					indices.push_back(static_cast<std::uint16_t>(remap[index]));
				}
				target.index = count ? start * 3 : 0;
				target.unkTriCount = count;
				target.unkFlags = count > 0;
				target.numTris = count;
				target.flags = 0;
				start += count;
			}
			pieceSegments->unk170 = true;

			RE::NiPointer<RE::BSTriShape> mesh{ CreateShape(start, static_cast<std::uint32_t>(positions.size()), positions.data(), indices.data()) };
			if (!mesh || !mesh->GetGeometryRuntimeData().rendererData) {
				if (splitFailures < 10) {
					logger::warn("LOD water split: the game's shape factory returned nothing for a {}-triangle piece of sub-index shape {}; the shape is left as it is.",
						start, fmt::ptr(a_shape));
				}
				return SplitResult::kFailed;
			}
			auto& pieceRuntime = piece->GetGeometryRuntimeData();
			auto& meshRuntime = mesh->GetGeometryRuntimeData();
			std::swap(pieceRuntime.rendererData, meshRuntime.rendererData);
			std::swap(pieceRuntime.vertexDesc, meshRuntime.vertexDesc);
			auto& pieceTri = piece->GetTrishapeRuntimeData();
			auto& meshTri = mesh->GetTrishapeRuntimeData();
			std::swap(pieceTri.triangleCount, meshTri.triangleCount);
			std::swap(pieceTri.vertexCount, meshTri.vertexCount);
			piece->GetModelData().modelBound = mesh->GetModelData().modelBound;
			piece->worldBound.center = piece->world * piece->GetModelData().modelBound.center;
			piece->worldBound.radius = piece->GetModelData().modelBound.radius * piece->world.scale;

			made.push_back(piece);
			madeTriangles.push_back(start);
		}

		for (std::size_t i = 0; i < made.size(); ++i) {
			RecordPiece(made[i].get(), hi, groups[i]);
		}
		a_parent->DetachChild(a_shape);
		a_parent->AttachChild(made.front().get(), true);
		for (std::size_t i = 1; i < made.size(); ++i) {
			a_parent->AttachChild(made[i].get(), false);
		}

		const std::uint32_t count = ++shapesSplit;
		piecesMade += static_cast<std::uint32_t>(made.size());
		if (count <= kDetailedSplits) {
			std::string list;
			for (std::size_t i = 0; i < made.size(); ++i) {
				list += std::format("{}type {:08X} ({} tris)", i ? ", " : "", groups[i] ? groups[i]->GetFormID() : 0, madeTriangles[i]);
			}
			logger::info("LOD water split: sub-index shape {} ({} triangles in {} cell segments, block {},{}) split by water type into {}.",
				fmt::ptr(a_shape), triangleCount, segmentCount, a_cellX, a_cellY, list);
		}
		return SplitResult::kSplit;
	}

	void SplitChunkWater(RE::NiNode* a_water, std::int32_t a_cellX, std::int32_t a_cellY)
	{
		std::vector<RE::NiPointer<RE::BSTriShape>> shapes;
		for (auto& child : a_water->GetChildren()) {
			auto* geometry = child ? child->AsGeometry() : nullptr;
			auto* shape = geometry ? geometry->AsTriShape() : nullptr;
			const char* type = shape ? shape->GetRTTI()->GetName() : nullptr;
			if (type && (std::strcmp(type, "BSTriShape") == 0 || std::strcmp(type, "BSSubIndexTriShape") == 0)) {
				shapes.emplace_back(shape);
			}
		}
		for (auto& shape : shapes) {
			auto*      subIndex = std::strcmp(shape->GetRTTI()->GetName(), "BSSubIndexTriShape") == 0 ? static_cast<RE::BSSubIndexTriShape*>(shape.get()) : nullptr;
			const auto result = subIndex ? SplitSubIndexShape(a_water, subIndex, a_cellX, a_cellY) : SplitShape(a_water, shape.get(), a_cellX, a_cellY);
			if (result == SplitResult::kFailed) {
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
				SplitChunkWater(chunk->waterNode.get(), a_cellX, a_cellY);
			}
			return chunk;
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void LodWaterSplit::Install()
{
	if (Detour::Function<Hook_TerrainChunkCtor>(REL::RelocationID(30932, 31735).address())) {
		logger::info("LOD water split: hooked BGSTerrainChunk::Ctor; distant water shapes holding several water heights or water types are split into one piece per height and type, at cell edges, when a terrain block loads.");
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

RE::TESWaterForm* LodWaterSplit::KnownType(const RE::BSGeometry* a_geometry)
{
	if (!a_geometry) {
		return nullptr;
	}
	const auto* data = const_cast<RE::BSGeometry*>(a_geometry)->GetGeometryRuntimeData().rendererData;
	std::scoped_lock lock(knownLock);
	const auto it = knownHeights.find(a_geometry);
	if (it == knownHeights.end() || it->second.rendererData != data) {
		return nullptr;
	}
	return it->second.type;
}
