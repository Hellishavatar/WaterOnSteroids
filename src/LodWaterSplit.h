#pragma once

#include "PCH.h"

#include <optional>

class LodWaterSplit
{
public:
	static void Install();

	[[nodiscard]] static std::optional<float> KnownLocalHeight(const RE::BSGeometry* a_geometry, const RE::BSGraphics::TriShape* a_rendererData);
};
