#pragma once

#include "PCH.h"
#include "Settings.h"
#include <atomic>

class WaterPatcher
{
public:
	static WaterPatcher& GetSingleton()
	{
		static WaterPatcher instance;
		return instance;
	}

	void Initialize();
	void RefreshWaterSettings(bool a_silent = false);

private:
	WaterPatcher() = default;
	WaterPatcher(const WaterPatcher&) = delete;
	WaterPatcher& operator=(const WaterPatcher&) = delete;

	std::atomic<bool> initialized{ false };
};
