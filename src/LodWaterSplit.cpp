#include "LodWaterSplit.h"
#include "CellWaterMap.h"
#include "Settings.h"

#include <RE/B/BGSTerrainChunk.h>
#include <RE/B/BGSTerrainManager.h>
#include <RE/B/BGSTerrainNode.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSSubIndexTriShape.h>
#include <RE/B/BSTriShape.h>
#include <RE/N/NiAlphaProperty.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiSkinPartition.h>
#include <RE/T/TESWaterForm.h>
#include <RE/T/TESWaterSystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	static_assert(sizeof(RE::SegmentData) == 0x14, "Unexpected sub-index segment size");
	static_assert(offsetof(RE::BSSubIndexTriShape::SUB_INDEX_TRISHAPE_RUNTIME_DATA, unk170) == 0x10, "Unexpected sub-index dirty flag offset");
	static_assert(offsetof(RE::BSSubIndexTriShape::SUB_INDEX_TRISHAPE_RUNTIME_DATA, nonSegmented) == 0x11, "Unexpected sub-index flag offset");

	constexpr float         kSameHeight = 2.0f;
	constexpr float         kCellSize = 4096.0f;
	constexpr float         kCellEdge = 1.0f;
	constexpr std::uint32_t kDetailedBlocks = 20;
	constexpr std::uint32_t kSummaryEvery = 500;

	std::atomic<std::uint32_t> shapesBuilt{ 0 };
	std::atomic<std::uint32_t> shapesSplit{ 0 };
	std::atomic<std::uint32_t> piecesMade{ 0 };
	std::atomic<std::uint32_t> exactTriangles{ 0 };
	std::atomic<std::uint32_t> keptTriangles{ 0 };
	std::atomic<std::uint32_t> shapesUnread{ 0 };
	std::atomic<std::uint32_t> buildFailures{ 0 };
	std::atomic<std::uint32_t> piecesUnregistered{ 0 };
	std::atomic<std::uint32_t> blocksDetailed{ 0 };
	std::atomic<std::uint64_t> buildMicroseconds{ 0 };

	struct Entry
	{
		const RE::BSGraphics::TriShape* rendererData{ nullptr };
		LodWaterSplit::Piece            piece{};
	};

	std::mutex                                       registryLock;
	std::unordered_map<const RE::BSGeometry*, Entry> registry;

	void Register(RE::BSGeometry* a_shape, const LodWaterSplit::Piece& a_piece)
	{
		std::scoped_lock lock(registryLock);
		registry[a_shape] = { a_shape->GetGeometryRuntimeData().rendererData, a_piece };
	}

	void Unregister(const RE::BSGeometry* a_shape)
	{
		std::scoped_lock lock(registryLock);
		registry.erase(a_shape);
	}

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
		float       minX = 0.0f;
		float       maxX = 0.0f;
		float       minY = 0.0f;
		float       maxY = 0.0f;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			float p[3];
			ReadPosition(a_raw, a_stride, a_full, i, p);
			const float dx = p[0] - a_bound.center.x;
			const float dy = p[1] - a_bound.center.y;
			const float dz = p[2] - a_bound.center.z;
			if (!(dx * dx + dy * dy + dz * dz <= limit * limit)) {
				return false;
			}
			minX = i == 0 ? p[0] : (std::min)(minX, p[0]);
			maxX = i == 0 ? p[0] : (std::max)(maxX, p[0]);
			minY = i == 0 ? p[1] : (std::min)(minY, p[1]);
			maxY = i == 0 ? p[1] : (std::max)(maxY, p[1]);
		}
		return (std::max)(maxX - minX, maxY - minY) >= a_bound.radius * 0.5f;
	}

	struct Frame
	{
		float originX{ 0.0f };
		float originY{ 0.0f };
		float scale{ 1.0f };
		float heightZ{ 0.0f };
		bool  usable{ false };

		[[nodiscard]] float WorldX(float a_local) const { return originX + scale * a_local; }
		[[nodiscard]] float WorldY(float a_local) const { return originY + scale * a_local; }
		[[nodiscard]] float WorldZ(float a_local) const { return heightZ + scale * a_local; }
		[[nodiscard]] float LocalZ(float a_world) const { return (a_world - heightZ) / scale; }
	};

	Frame FrameFor(const RE::BSTriShape* a_shape, std::int32_t a_cellX, std::int32_t a_cellY)
	{
		const auto& local = a_shape->local;
		const auto& rot = local.rotate.entry;
		Frame       frame;
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

	using Triangle = std::array<RE::NiPoint3, 3>;

	struct Mesh
	{
		std::vector<Triangle> triangles;
		bool                  full{ false };
	};

	bool ReadMesh(RE::BSTriShape* a_shape, Mesh& a_out)
	{
		auto& runtime = a_shape->GetGeometryRuntimeData();
		auto* data = runtime.rendererData;
		if (!data || !data->rawVertexData || !data->rawIndexData || runtime.skinInstance) {
			return false;
		}
		const auto          tri = a_shape->GetTrishapeRuntimeData();
		const std::uint32_t vertexCount = tri.vertexCount;
		const auto          bits = std::bit_cast<std::uint64_t>(data->vertexDesc);
		const std::uint32_t stride = static_cast<std::uint32_t>(bits & 0xF) * 4;
		const auto&         bound = a_shape->GetModelData().modelBound;
		bool                full = data->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
		if (!PositionsFitBound(data->rawVertexData, vertexCount, stride, full, bound)) {
			full = !full;
			if (!PositionsFitBound(data->rawVertexData, vertexCount, stride, full, bound)) {
				return false;
			}
		}
		a_out.full = full;
		a_out.triangles.resize(tri.triangleCount);
		for (std::uint32_t t = 0; t < tri.triangleCount; ++t) {
			for (int k = 0; k < 3; ++k) {
				const std::uint16_t index = data->rawIndexData[t * 3 + k];
				if (index >= vertexCount) {
					return false;
				}
				float p[3];
				ReadPosition(data->rawVertexData, stride, full, index, p);
				a_out.triangles[t][k] = { p[0], p[1], p[2] };
			}
		}
		return true;
	}

	struct Key
	{
		RE::TESWaterForm* type{ nullptr };
		float             localZ{ 0.0f };
		bool              exact{ false };

		bool operator==(const Key& a_other) const { return type == a_other.type && localZ == a_other.localZ && exact == a_other.exact; }
	};

	Key Classify(const Triangle& a_triangle, const Frame& a_frame, const CellWaterMap::Cells& a_cells, RE::TESWaterForm* a_lodType)
	{
		const float lo = (std::min)({ a_triangle[0].z, a_triangle[1].z, a_triangle[2].z });
		const float hi = (std::max)({ a_triangle[0].z, a_triangle[1].z, a_triangle[2].z });
		const Key   kept{ a_lodType, hi, false };
		if (!a_frame.usable || (hi - lo) * a_frame.scale > kSameHeight) {
			return kept;
		}
		const float centreX = a_frame.WorldX((a_triangle[0].x + a_triangle[1].x + a_triangle[2].x) / 3.0f);
		const float centreY = a_frame.WorldY((a_triangle[0].y + a_triangle[1].y + a_triangle[2].y) / 3.0f);
		const auto  cellX = static_cast<std::int32_t>(std::floor(centreX / kCellSize));
		const auto  cellY = static_cast<std::int32_t>(std::floor(centreY / kCellSize));
		const float minX = static_cast<float>(cellX) * kCellSize - kCellEdge;
		const float minY = static_cast<float>(cellY) * kCellSize - kCellEdge;
		for (const auto& p : a_triangle) {
			const float x = a_frame.WorldX(p.x);
			const float y = a_frame.WorldY(p.y);
			if (x < minX || y < minY || x > minX + kCellSize + 2.0f * kCellEdge || y > minY + kCellSize + 2.0f * kCellEdge) {
				return kept;
			}
		}
		const auto cell = a_cells.At(cellX, cellY);
		if (!cell.water || !cell.type || std::abs(a_frame.WorldZ(hi) - cell.height) > kSameHeight) {
			return kept;
		}
		return { cell.type, a_frame.LocalZ(cell.height), true };
	}

	struct Group
	{
		Key                        key;
		std::vector<std::uint32_t> triangles;
	};

	std::size_t GroupIndex(std::vector<Group>& a_groups, const Key& a_key)
	{
		for (std::size_t i = 0; i < a_groups.size(); ++i) {
			if (a_groups[i].key == a_key) {
				return i;
			}
		}
		a_groups.push_back({ a_key, {} });
		return a_groups.size() - 1;
	}

	void MergeKept(std::vector<Group>& a_groups)
	{
		std::size_t first = a_groups.size();
		for (std::size_t i = 0; i < a_groups.size(); ++i) {
			if (a_groups[i].key.exact) {
				continue;
			}
			if (first == a_groups.size()) {
				first = i;
				continue;
			}
			auto& target = a_groups[first];
			target.key.localZ = (std::max)(target.key.localZ, a_groups[i].key.localZ);
			target.triangles.insert(target.triangles.end(), a_groups[i].triangles.begin(), a_groups[i].triangles.end());
			a_groups[i].triangles.clear();
		}
		std::erase_if(a_groups, [](const Group& a_group) { return a_group.triangles.empty(); });
	}

	LodWaterSplit::Piece PieceOf(const Key& a_key)
	{
		return { a_key.type, a_key.localZ, a_key.exact };
	}

	void Count(const std::vector<Group>& a_groups)
	{
		for (const auto& group : a_groups) {
			(group.key.exact ? exactTriangles : keptTriangles) += static_cast<std::uint32_t>(group.triangles.size());
		}
	}

	struct Builder
	{
		std::vector<RE::NiPoint3>  positions;
		std::vector<std::uint16_t> indices;

		bool Add(const Triangle& a_triangle)
		{
			if (positions.size() + 3 > (std::numeric_limits<std::uint16_t>::max)()) {
				return false;
			}
			for (const auto& p : a_triangle) {
				indices.push_back(static_cast<std::uint16_t>(positions.size()));
				positions.push_back(p);
			}
			return true;
		}
	};

	std::string Describe(const std::vector<Group>& a_groups)
	{
		std::string text;
		for (std::size_t i = 0; i < a_groups.size(); ++i) {
			const auto& key = a_groups[i].key;
			text += std::format("{}{:08X} at local {}{} ({} tris)", i ? ", " : "", key.type ? key.type->GetFormID() : 0, key.localZ,
				key.exact ? "" : " kept as LOD water", a_groups[i].triangles.size());
		}
		return text;
	}

	bool BuildShape(RE::NiNode* a_parent, RE::BSTriShape* a_shape, std::int32_t a_cellX, std::int32_t a_cellY, const CellWaterMap::Cells& a_cells, RE::TESWaterForm* a_lodType, bool a_canReplace, std::string& a_log)
	{
		Mesh mesh;
		if (!ReadMesh(a_shape, mesh)) {
			++shapesUnread;
			return false;
		}
		const auto         frame = FrameFor(a_shape, a_cellX, a_cellY);
		std::vector<Group> groups;
		for (std::uint32_t t = 0; t < mesh.triangles.size(); ++t) {
			groups[GroupIndex(groups, Classify(mesh.triangles[t], frame, a_cells, a_lodType))].triangles.push_back(t);
		}
		MergeKept(groups);
		if (groups.empty()) {
			return false;
		}
		if (!a_canReplace && groups.size() > 1) {
			Key kept{ a_lodType, groups.front().key.localZ, false };
			for (const auto& group : groups) {
				for (const auto t : group.triangles) {
					for (const auto& p : mesh.triangles[t]) {
						kept.localZ = (std::max)(kept.localZ, p.z);
					}
				}
			}
			Group all{ kept, {} };
			for (const auto& group : groups) {
				all.triangles.insert(all.triangles.end(), group.triangles.begin(), group.triangles.end());
			}
			groups = { all };
		}
		Count(groups);
		++shapesBuilt;
		if (groups.size() == 1) {
			Register(a_shape, PieceOf(groups.front().key));
			a_log += std::format(" [{}]", Describe(groups));
			return true;
		}

		auto& runtime = a_shape->GetGeometryRuntimeData();
		std::vector<RE::NiPointer<RE::BSTriShape>> made;
		for (const auto& group : groups) {
			Builder builder;
			for (const auto t : group.triangles) {
				if (!builder.Add(mesh.triangles[t])) {
					++buildFailures;
					return false;
				}
			}
			RE::NiPointer<RE::BSTriShape> shape{ CreateShape(static_cast<std::uint32_t>(group.triangles.size()), static_cast<std::uint32_t>(builder.positions.size()), builder.positions.data(), builder.indices.data()) };
			if (!shape || !shape->GetGeometryRuntimeData().rendererData) {
				if (buildFailures++ < 10) {
					logger::warn("Distant water: the game's shape factory returned nothing for a {}-triangle piece of shape {}; the shape is left as it is.", group.triangles.size(), fmt::ptr(a_shape));
				}
				return false;
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
					if (buildFailures++ < 10) {
						logger::warn("Distant water: could not copy the water shader property of shape {}; the shape is left as it is.", fmt::ptr(a_shape));
					}
					return false;
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
			Register(shape, PieceOf(groups[i].key));
		}
		a_parent->DetachChild(a_shape);
		a_parent->AttachChild(made.front().get(), true);
		for (std::size_t i = 1; i < made.size(); ++i) {
			a_parent->AttachChild(made[i].get(), false);
		}
		++shapesSplit;
		piecesMade += static_cast<std::uint32_t>(made.size());
		a_log += std::format(" [{}]", Describe(groups));
		return true;
	}

	bool BuildSubIndexShape(RE::NiNode* a_parent, RE::BSSubIndexTriShape* a_shape, std::int32_t a_cellX, std::int32_t a_cellY, const CellWaterMap::Cells& a_cells, RE::TESWaterForm* a_lodType, std::string& a_log)
	{
		auto& segments = a_shape->GetSubIndexedTrishapeRuntimeData();
		if (!segments.segmentData || segments.nonSegmented) {
			return BuildShape(a_parent, a_shape, a_cellX, a_cellY, a_cells, a_lodType, false, a_log);
		}
		Mesh mesh;
		if (!ReadMesh(a_shape, mesh)) {
			++shapesUnread;
			return false;
		}
		const std::uint32_t triangleCount = static_cast<std::uint32_t>(mesh.triangles.size());
		const std::uint32_t segmentCount = segments.numSegments;
		std::uint32_t       covered = 0;
		for (std::uint32_t i = 0; i < segmentCount; ++i) {
			const auto& segment = segments.segmentData[i];
			if (segment.index % 3 != 0 || segment.index / 3 + segment.unkTriCount > triangleCount) {
				++shapesUnread;
				return false;
			}
			covered += segment.unkTriCount;
		}
		if (covered != triangleCount) {
			++shapesUnread;
			return false;
		}

		const auto         frame = FrameFor(a_shape, a_cellX, a_cellY);
		std::vector<Group> groups;
		std::vector<std::size_t> segmentGroup(segmentCount, 0);
		for (std::uint32_t i = 0; i < segmentCount; ++i) {
			const auto& segment = segments.segmentData[i];
			if (!segment.unkTriCount) {
				continue;
			}
			const std::uint32_t first = segment.index / 3;
			Key                 key = Classify(mesh.triangles[first], frame, a_cells, a_lodType);
			for (std::uint32_t t = first + 1; t < first + segment.unkTriCount && key.exact; ++t) {
				if (!(Classify(mesh.triangles[t], frame, a_cells, a_lodType) == key)) {
					key = { a_lodType, key.localZ, false };
				}
			}
			if (!key.exact) {
				for (std::uint32_t t = first; t < first + segment.unkTriCount; ++t) {
					for (const auto& p : mesh.triangles[t]) {
						key.localZ = (std::max)(key.localZ, p.z);
					}
				}
			}
			segmentGroup[i] = GroupIndex(groups, key);
			for (std::uint32_t t = first; t < first + segment.unkTriCount; ++t) {
				groups[segmentGroup[i]].triangles.push_back(t);
			}
		}
		std::vector<std::size_t> merged(groups.size());
		{
			std::size_t keptGroup = groups.size();
			std::size_t next = 0;
			for (std::size_t g = 0; g < groups.size(); ++g) {
				if (groups[g].key.exact || keptGroup == groups.size()) {
					merged[g] = next++;
					if (!groups[g].key.exact) {
						keptGroup = g;
					}
				} else {
					merged[g] = merged[keptGroup];
				}
			}
			for (auto& index : segmentGroup) {
				index = groups.empty() ? 0 : merged[index];
			}
		}
		MergeKept(groups);
		if (groups.empty()) {
			return false;
		}
		Count(groups);
		++shapesBuilt;
		if (groups.size() == 1) {
			Register(a_shape, PieceOf(groups.front().key));
			a_log += std::format(" [{}]", Describe(groups));
			return true;
		}

		std::vector<RE::NiPointer<RE::BSSubIndexTriShape>> made;
		for (std::size_t g = 0; g < groups.size(); ++g) {
			RE::NiPointer<RE::NiObject>           copy{ a_shape->Clone() };
			RE::NiPointer<RE::BSSubIndexTriShape> piece{ copy ? netimmerse_cast<RE::BSSubIndexTriShape*>(copy.get()) : nullptr };
			auto*                                 pieceSegments = piece ? &piece->GetSubIndexedTrishapeRuntimeData() : nullptr;
			if (!piece || !pieceSegments->segmentData || pieceSegments->numSegments != segmentCount) {
				if (buildFailures++ < 10) {
					logger::warn("Distant water: could not copy sub-index water shape {}; the shape is left as it is.", fmt::ptr(a_shape));
				}
				return false;
			}
			Builder       builder;
			std::uint32_t start = 0;
			for (std::uint32_t i = 0; i < segmentCount; ++i) {
				const auto&         source = segments.segmentData[i];
				auto&               target = pieceSegments->segmentData[i];
				const std::uint32_t count = source.unkTriCount && segmentGroup[i] == g ? source.unkTriCount : 0;
				for (std::uint32_t t = source.index / 3; t < source.index / 3 + count; ++t) {
					if (!builder.Add(mesh.triangles[t])) {
						++buildFailures;
						return false;
					}
				}
				target.index = count ? start * 3 : 0;
				target.unkTriCount = count;
				target.unkFlags = count > 0;
				target.numTris = count;
				target.flags = 0;
				start += count;
			}
			pieceSegments->unk170 = true;

			RE::NiPointer<RE::BSTriShape> meshShape{ CreateShape(start, static_cast<std::uint32_t>(builder.positions.size()), builder.positions.data(), builder.indices.data()) };
			if (!meshShape || !meshShape->GetGeometryRuntimeData().rendererData) {
				if (buildFailures++ < 10) {
					logger::warn("Distant water: the game's shape factory returned nothing for a {}-triangle piece of sub-index shape {}; the shape is left as it is.", start, fmt::ptr(a_shape));
				}
				return false;
			}
			auto& pieceRuntime = piece->GetGeometryRuntimeData();
			auto& meshRuntime = meshShape->GetGeometryRuntimeData();
			std::swap(pieceRuntime.rendererData, meshRuntime.rendererData);
			std::swap(pieceRuntime.vertexDesc, meshRuntime.vertexDesc);
			auto& pieceTri = piece->GetTrishapeRuntimeData();
			auto& meshTri = meshShape->GetTrishapeRuntimeData();
			std::swap(pieceTri.triangleCount, meshTri.triangleCount);
			std::swap(pieceTri.vertexCount, meshTri.vertexCount);
			piece->GetModelData().modelBound = meshShape->GetModelData().modelBound;
			piece->worldBound.center = piece->world * piece->GetModelData().modelBound.center;
			piece->worldBound.radius = piece->GetModelData().modelBound.radius * piece->world.scale;
			made.push_back(piece);
		}

		for (std::size_t g = 0; g < made.size(); ++g) {
			Register(made[g].get(), PieceOf(groups[g].key));
		}
		a_parent->DetachChild(a_shape);
		a_parent->AttachChild(made.front().get(), true);
		for (std::size_t g = 1; g < made.size(); ++g) {
			a_parent->AttachChild(made[g].get(), false);
		}
		++shapesSplit;
		piecesMade += static_cast<std::uint32_t>(made.size());
		a_log += std::format(" [{}]", Describe(groups));
		return true;
	}

	void BuildChunkWater(RE::NiNode* a_water, std::int32_t a_cellX, std::int32_t a_cellY, RE::TESWorldSpace* a_world)
	{
		const auto start = std::chrono::steady_clock::now();
		auto*      lodType = CellWaterMap::LodWaterType(a_world);
		const CellWaterMap::Cells cells(a_world);
		if (!lodType || !cells) {
			return;
		}
		std::vector<RE::NiPointer<RE::BSTriShape>> shapes;
		for (auto& child : a_water->GetChildren()) {
			auto* geometry = child ? child->AsGeometry() : nullptr;
			auto* shape = geometry ? geometry->AsTriShape() : nullptr;
			const char* type = shape ? shape->GetRTTI()->GetName() : nullptr;
			if (type && (std::strcmp(type, "BSTriShape") == 0 || std::strcmp(type, "BSSubIndexTriShape") == 0)) {
				shapes.emplace_back(shape);
			}
		}
		std::string log;
		const auto  before = shapesBuilt.load();
		for (auto& shape : shapes) {
			if (std::strcmp(shape->GetRTTI()->GetName(), "BSSubIndexTriShape") == 0) {
				BuildSubIndexShape(a_water, static_cast<RE::BSSubIndexTriShape*>(shape.get()), a_cellX, a_cellY, cells, lodType, log);
			} else {
				BuildShape(a_water, shape.get(), a_cellX, a_cellY, cells, lodType, true, log);
			}
		}
		if (!log.empty() && blocksDetailed++ < kDetailedBlocks) {
			logger::info("Distant water: block {},{} of {:08X}:{}", a_cellX, a_cellY, a_world->GetFormID(), log);
		}
		buildMicroseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		const auto built = shapesBuilt.load();
		if (built / kSummaryEvery != before / kSummaryEvery) {
			logger::info("Distant water: {} LOD water shapes built from cell records in {} ms ({} split into {} pieces); {} triangles at their cell's height and type, {} kept as LOD water; {} shapes unreadable, {} failed; {} pieces of unloaded blocks removed from the water system.",
				built, buildMicroseconds.load() / 1000, shapesSplit.load(), piecesMade.load(), exactTriangles.load(), keptTriangles.load(), shapesUnread.load(), buildFailures.load(), piecesUnregistered.load());
		}
	}

	struct Hook_TerrainChunkCtor
	{
		static RE::BGSTerrainChunk* thunk(RE::BGSTerrainChunk* a_this, RE::BGSTerrainNode* a_node, RE::BSStream& a_stream, std::int32_t a_cellX, std::int32_t a_cellY)
		{
			auto* chunk = func(a_this, a_node, a_stream, a_cellX, a_cellY);
			const auto& settings = Settings::GetFastSnapshot();
			auto*       world = a_node && a_node->manager ? a_node->manager->worldSpace : nullptr;
			if (chunk && chunk->waterNode && !chunk->prepared && world && settings.enabled && settings.fullShaderDistantWater) {
				BuildChunkWater(chunk->waterNode.get(), a_cellX, a_cellY, world);
			}
			return chunk;
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Hook_TerrainChunkDetach
	{
		static void thunk(RE::BGSTerrainChunk* a_this)
		{
			if (a_this && a_this->waterAttached && a_this->waterNode) {
				auto& children = a_this->waterNode->GetChildren();
				if (children.size() > 1) {
					auto* waterSystem = RE::TESWaterSystem::GetSingleton();
					auto  lodRoot = RE::BGSTerrainManager::GetWaterNode();
					for (std::uint16_t i = 1; waterSystem && i < children.free_idx(); ++i) {
						if (auto& child = children[i]) {
							waterSystem->RemoveLODWater(child.get(), lodRoot.get());
							++piecesUnregistered;
						}
					}
				}
			}
			func(a_this);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Hook_TerrainChunkDtor
	{
		static void thunk(RE::BGSTerrainChunk* a_this)
		{
			if (a_this && a_this->waterNode) {
				for (auto& child : a_this->waterNode->GetChildren()) {
					if (auto* geometry = child ? child->AsGeometry() : nullptr) {
						Unregister(geometry);
					}
				}
			}
			func(a_this);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void LodWaterSplit::Install()
{
	if (Detour::Function<Hook_TerrainChunkCtor>(REL::RelocationID(30932, 31735).address())) {
		logger::info("Distant water: hooked BGSTerrainChunk::Ctor; each terrain block's LOD water takes the height and type of the cell it lies in, from the cell records, when the block loads.");
	} else {
		logger::error("Distant water: failed to hook BGSTerrainChunk::Ctor; distant water is drawn as vanilla LOD water.");
		return;
	}
	if (Detour::Function<Hook_TerrainChunkDetach>(REL::RelocationID(30936, 31739).address())) {
		logger::info("Distant water: hooked BGSTerrainChunk::Detach; every water piece of an unloading terrain block is removed from the water system (the game removes only the first).");
	} else {
		logger::error("Distant water: failed to hook BGSTerrainChunk::Detach; split pieces stay in the water system after their block unloads.");
	}
	if (!Detour::Function<Hook_TerrainChunkDtor>(REL::RelocationID(30933, 31736).address())) {
		logger::error("Distant water: failed to hook BGSTerrainChunk::Dtor; the piece list is not cleaned when blocks are destroyed.");
	}
}

std::optional<LodWaterSplit::Piece> LodWaterSplit::PieceFor(const RE::BSGeometry* a_geometry)
{
	if (!a_geometry) {
		return std::nullopt;
	}
	const auto* data = const_cast<RE::BSGeometry*>(a_geometry)->GetGeometryRuntimeData().rendererData;
	std::scoped_lock lock(registryLock);
	const auto       it = registry.find(a_geometry);
	if (it == registry.end() || it->second.rendererData != data) {
		return std::nullopt;
	}
	return it->second.piece;
}
