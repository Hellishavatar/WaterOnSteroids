#include "CellWaterMap.h"

#include <RE/T/TES.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESFile.h>
#include <RE/T/TESWaterForm.h>
#include <RE/T/TESWorldSpace.h>

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr std::uint32_t kCompressed = 0x40000;
	constexpr std::uint8_t  kHasWater = 0x02;
	constexpr std::size_t   kLoggedCells = 100;

	constexpr std::uint32_t Tag(const char (&a_tag)[5])
	{
		return static_cast<std::uint32_t>(a_tag[0]) | static_cast<std::uint32_t>(a_tag[1]) << 8 |
		       static_cast<std::uint32_t>(a_tag[2]) << 16 | static_cast<std::uint32_t>(a_tag[3]) << 24;
	}

	struct Cell
	{
		float             height{ std::numeric_limits<float>::quiet_NaN() };
		RE::TESWaterForm* type{ nullptr };
		bool              water{ true };
	};

	struct Map
	{
		RE::TESWorldSpace*                       world{ nullptr };
		float                                    defaultHeight{ 0.0f };
		RE::TESWaterForm*                        defaultType{ nullptr };
		std::unordered_map<std::uint64_t, Cell> cells;
	};

	using Maps = std::unordered_map<RE::TESWorldSpace*, std::shared_ptr<Map>>;

	std::uint64_t Key(std::int32_t a_x, std::int32_t a_y)
	{
		return static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x)) << 32 | static_cast<std::uint32_t>(a_y);
	}

	RE::FormID Resolve(const RE::TESFile* a_file, std::uint32_t a_raw)
	{
		const std::uint32_t index = a_raw >> 24;
		const RE::TESFile*  owner = index < a_file->masterCount ? a_file->masterPtrs[index] : a_file;
		if (!owner || owner->compileIndex == 0xFF) {
			return 0;
		}
		if (owner->IsLight()) {
			return 0xFE000000 | static_cast<RE::FormID>(owner->smallFileCompileIndex) << 12 | (a_raw & 0xFFF);
		}
		return static_cast<RE::FormID>(owner->compileIndex) << 24 | (a_raw & 0xFFFFFF);
	}

	std::shared_ptr<Map> DefaultMap(RE::TESWorldSpace* a_world)
	{
		auto map = std::make_shared<Map>();
		map->world = a_world;
		map->defaultHeight = a_world->defaultWaterHeight;
		for (auto* world = a_world; world && !map->defaultType; world = world->parentWorld) {
			map->defaultType = world->worldWater;
			if (!world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseWaterData)) {
				break;
			}
		}
		return map;
	}

	std::mutex                 mapsLock;
	Maps                       maps;
	std::atomic<bool>          ready{ false };

	struct Header
	{
		std::uint32_t type;
		std::uint32_t size;
		std::uint32_t flags;
		std::uint32_t id;
		std::uint32_t extra[2];
	};
	static_assert(sizeof(Header) == 24);

	class PluginReader
	{
	public:
		PluginReader(const RE::TESFile* a_file, Maps& a_maps) :
			file(a_file), maps(a_maps)
		{}

		bool Read()
		{
			const auto path = std::filesystem::path(file->path) / file->fileName;
			in.open(path, std::ios::binary);
			if (!in) {
				return false;
			}
			in.seekg(0, std::ios::end);
			const auto end = static_cast<std::uint64_t>(in.tellg());
			Header tes4{};
			if (!ReadHeader(0, tes4) || tes4.type != Tag("TES4")) {
				return false;
			}
			std::uint64_t pos = sizeof(Header) + tes4.size;
			while (pos + sizeof(Header) <= end) {
				Header group{};
				if (!ReadHeader(pos, group) || group.type != Tag("GRUP") || group.size < sizeof(Header)) {
					return false;
				}
				if (group.flags == Tag("WRLD")) {
					ReadWorlds(pos + sizeof(Header), pos + group.size);
				}
				pos += group.size;
			}
			return true;
		}

		std::uint32_t cellsRead{ 0 };

	private:
		bool ReadHeader(std::uint64_t a_pos, Header& a_out)
		{
			in.seekg(static_cast<std::streamoff>(a_pos));
			return static_cast<bool>(in.read(reinterpret_cast<char*>(&a_out), sizeof(a_out)));
		}

		void ReadWorlds(std::uint64_t a_pos, std::uint64_t a_end)
		{
			while (a_pos + sizeof(Header) <= a_end) {
				Header h{};
				if (!ReadHeader(a_pos, h)) {
					return;
				}
				if (h.type != Tag("GRUP")) {
					a_pos += sizeof(Header) + h.size;
					continue;
				}
				if (h.size < sizeof(Header)) {
					return;
				}
				if (static_cast<std::int32_t>(h.id) == 1) {
					if (auto* world = RE::TESForm::LookupByID<RE::TESWorldSpace>(Resolve(file, h.flags))) {
						auto& map = maps[world];
						if (!map) {
							map = DefaultMap(world);
						}
						ReadCells(a_pos + sizeof(Header), a_pos + h.size, *map);
					}
				}
				a_pos += h.size;
			}
		}

		void ReadCells(std::uint64_t a_pos, std::uint64_t a_end, Map& a_map)
		{
			while (a_pos + sizeof(Header) <= a_end) {
				Header h{};
				if (!ReadHeader(a_pos, h)) {
					return;
				}
				if (h.type == Tag("GRUP")) {
					if (h.size < sizeof(Header)) {
						return;
					}
					const auto groupType = static_cast<std::int32_t>(h.id);
					if (groupType == 4 || groupType == 5) {
						ReadCells(a_pos + sizeof(Header), a_pos + h.size, a_map);
					}
					a_pos += h.size;
					continue;
				}
				if (h.type == Tag("CELL")) {
					ReadCell(a_pos + sizeof(Header), h, a_map);
				}
				a_pos += sizeof(Header) + h.size;
			}
		}

		void ReadCell(std::uint64_t a_pos, const Header& a_header, Map& a_map)
		{
			std::vector<std::uint8_t> data(a_header.size);
			in.seekg(static_cast<std::streamoff>(a_pos));
			if (!in.read(reinterpret_cast<char*>(data.data()), data.size())) {
				return;
			}
			if (a_header.flags & kCompressed) {
				if (data.size() < 4) {
					return;
				}
				std::uint32_t size = 0;
				std::memcpy(&size, data.data(), 4);
				std::vector<std::uint8_t> out(size);
				uLongf                    outSize = size;
				if (uncompress(out.data(), &outSize, data.data() + 4, static_cast<uLong>(data.size() - 4)) != Z_OK) {
					return;
				}
				out.resize(outSize);
				data = std::move(out);
			}

			bool          hasCoords = false;
			std::int32_t  x = 0;
			std::int32_t  y = 0;
			Cell          cell;
			std::uint32_t rawType = 0;
			std::size_t   p = 0;
			std::uint32_t bigSize = 0;
			while (p + 6 <= data.size()) {
				std::uint32_t tag = 0;
				std::uint16_t size16 = 0;
				std::memcpy(&tag, data.data() + p, 4);
				std::memcpy(&size16, data.data() + p + 4, 2);
				const std::size_t size = bigSize ? bigSize : size16;
				bigSize = 0;
				const std::uint8_t* field = data.data() + p + 6;
				if (p + 6 + size > data.size()) {
					break;
				}
				if (tag == Tag("XXXX") && size >= 4) {
					std::memcpy(&bigSize, field, 4);
				} else if (tag == Tag("XCLC") && size >= 8) {
					std::memcpy(&x, field, 4);
					std::memcpy(&y, field + 4, 4);
					hasCoords = true;
				} else if (tag == Tag("XCLW") && size >= 4) {
					float height = 0.0f;
					std::memcpy(&height, field, 4);
					if (height > -1.0e6f && height < 1.0e6f) {
						cell.height = height;
					}
				} else if (tag == Tag("XCWT") && size >= 4) {
					std::memcpy(&rawType, field, 4);
				} else if (tag == Tag("DATA") && size >= 1) {
					cell.water = (field[0] & kHasWater) != 0;
				}
				p += 6 + size;
			}
			if (!hasCoords) {
				return;
			}
			if (rawType) {
				const auto id = Resolve(file, rawType);
				cell.type = id ? RE::TESForm::LookupByID<RE::TESWaterForm>(id) : nullptr;
			}
			a_map.cells[Key(x, y)] = cell;
			++cellsRead;
		}

		const RE::TESFile* file;
		Maps&              maps;
		std::ifstream      in;
	};

	void ScanAll()
	{
		const auto    start = std::chrono::steady_clock::now();
		Maps          built;
		std::uint32_t plugins = 0;
		std::uint32_t failed = 0;
		std::uint32_t records = 0;
		std::uint32_t outOfOrder = 0;
		std::int32_t  lastIndex = -1;
		if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
			for (auto* file : dataHandler->files) {
				if (!file || file->compileIndex == 0xFF) {
					continue;
				}
				if (!file->IsLight()) {
					outOfOrder += file->compileIndex < lastIndex;
					lastIndex = file->compileIndex;
				}
				PluginReader reader(file, built);
				if (reader.Read()) {
					++plugins;
					records += reader.cellsRead;
				} else {
					++failed;
					logger::warn("Cell water map: could not read {}.", file->GetFilename());
				}
			}
		}

		std::size_t cells = 0;
		for (const auto& [world, map] : built) {
			cells += map->cells.size();
			if (map->cells.size() < kLoggedCells) {
				continue;
			}
			std::size_t ownTypes = 0;
			for (const auto& [key, cell] : map->cells) {
				ownTypes += cell.type != nullptr;
			}
			logger::info("Cell water map: {:08X} '{}': {} cells, {} with their own water type, default {:08X} at {:.1f}.",
				world->GetFormID(), world->GetFormEditorID(), map->cells.size(), ownTypes,
				map->defaultType ? map->defaultType->GetFormID() : 0, map->defaultHeight);
		}
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		logger::info("Cell water map: {} worldspaces, {} cells from {} cell records in {} plugins ({} unreadable, {} out of load order), {} ms.",
			built.size(), cells, records, plugins, failed, outOfOrder, ms);

		{
			std::scoped_lock lock(mapsLock);
			maps = std::move(built);
		}
		ready = true;
	}

	std::shared_ptr<const Map> MapFor(RE::TESWorldSpace* a_world)
	{
		if (!ready) {
			return nullptr;
		}
		std::scoped_lock lock(mapsLock);
		auto&            map = maps[a_world];
		if (!map) {
			map = DefaultMap(a_world);
		}
		return map;
	}
}

void CellWaterMap::Scan()
{
	if (!ready) {
		ScanAll();
	}
}

RE::TESWorldSpace* CellWaterMap::LodWorld()
{
	auto* tes = RE::TES::GetSingleton();
	auto* world = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	while (world && world->parentWorld && world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData)) {
		world = world->parentWorld;
	}
	return world;
}

CellWaterMap::Cells::Cells(RE::TESWorldSpace* a_world) :
	map(a_world ? MapFor(a_world) : nullptr)
{}

CellWaterMap::CellWater CellWaterMap::Cells::At(std::int32_t a_x, std::int32_t a_y) const
{
	const auto* cells = static_cast<const Map*>(map.get());
	const auto  it = cells->cells.find(Key(a_x, a_y));
	const Cell  cell = it != cells->cells.end() ? it->second : Cell{};
	return CellWater{ std::isnan(cell.height) ? cells->defaultHeight : cell.height, cell.type ? cell.type : cells->defaultType, cell.water };
}

RE::TESWaterForm* CellWaterMap::LodWaterType(RE::TESWorldSpace* a_world)
{
	for (auto* world = a_world; world; world = world->parentWorld) {
		if (world->lodWater) {
			return world->lodWater;
		}
		if (!world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData)) {
			break;
		}
	}
	const auto map = a_world ? MapFor(a_world) : nullptr;
	return map ? map->defaultType : nullptr;
}

std::optional<std::pair<float, RE::TESWaterForm*>> CellWaterMap::DefaultWater()
{
	auto* world = LodWorld();
	if (!world) {
		return std::nullopt;
	}
	const auto map = MapFor(world);
	if (!map || !map->defaultType) {
		return std::nullopt;
	}
	return std::make_pair(map->defaultHeight, map->defaultType);
}
