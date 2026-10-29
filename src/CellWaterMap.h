#pragma once

#include "PCH.h"

namespace CellWaterMap
{
	void Scan();

	[[nodiscard]] RE::TESWorldSpace* LodWorld();

	[[nodiscard]] RE::TESWaterForm* TypeFor(const RE::BSGeometry* a_geometry, float a_minZ, float a_maxZ);
}
