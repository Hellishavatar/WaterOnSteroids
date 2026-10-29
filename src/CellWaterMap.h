#pragma once

#include "PCH.h"

#include <optional>
#include <utility>

namespace CellWaterMap
{
	void Scan();

	[[nodiscard]] RE::TESWorldSpace* LodWorld();

	[[nodiscard]] RE::TESWaterForm* TypeFor(const RE::BSGeometry* a_geometry, float a_minZ, float a_maxZ);

	[[nodiscard]] std::optional<std::pair<float, RE::TESWaterForm*>> DefaultWater();

	[[nodiscard]] std::vector<RE::TESWaterForm*> TypesAt(std::int32_t a_minX, std::int32_t a_minY, std::int32_t a_maxX, std::int32_t a_maxY, float a_height);
}
