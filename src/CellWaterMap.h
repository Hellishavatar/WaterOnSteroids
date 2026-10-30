#pragma once

#include "PCH.h"

#include <memory>
#include <optional>
#include <utility>

namespace CellWaterMap
{
	struct CellWater
	{
		float             height{ 0.0f };
		RE::TESWaterForm* type{ nullptr };
		bool              water{ false };
	};

	void Scan();

	[[nodiscard]] RE::TESWorldSpace* LodWorld();

	class Cells
	{
	public:
		explicit Cells(RE::TESWorldSpace* a_world);

		[[nodiscard]] explicit operator bool() const { return map != nullptr; }

		[[nodiscard]] CellWater At(std::int32_t a_x, std::int32_t a_y) const;

	private:
		std::shared_ptr<const void> map;
	};

	[[nodiscard]] RE::TESWaterForm* LodWaterType(RE::TESWorldSpace* a_world);

	[[nodiscard]] std::optional<std::pair<float, RE::TESWaterForm*>> DefaultWater();
}
