#pragma once

#include "PCH.h"

#include <optional>

class LodWaterSplit
{
public:
	struct Piece
	{
		RE::TESWaterForm* type{ nullptr };
		float             localZ{ 0.0f };
		bool              exact{ false };
	};

	static void Install();

	[[nodiscard]] static std::optional<Piece> PieceFor(const RE::BSGeometry* a_geometry);
};
