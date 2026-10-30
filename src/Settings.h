#pragma once

#include "PCH.h"
#include <shared_mutex>

struct SettingsSnapshot
{
	bool enabled{ true };
	float windowAlpha{ 0.70f };
	uint32_t toggleKey{ VK_F3 };

	bool clearUnderwater{ true };
	bool fullShaderDistantWater{ true };
	bool disableRiverFlow{ true };
	bool realisticDistantWater{ true };
	bool removeSunGlints{ true };

	bool fixDepthProperties{ true };
	float waterTransparency{ 10.0f };
	float depthReflections{ 1.0f };
	float depthRefraction{ 0.70f };
	float depthNormals{ 0.91f };
	float depthSpecularLighting{ 1.00f };

	bool hasCustomColors{ false };
	int colorMode{ 0 };
	std::array<float, 3> shallowColor{ 0.0f, 0.0f, 0.0f };
	std::array<float, 3> deepColor{ 0.0f, 0.118812f, 0.101167f };
	std::array<float, 3> reflectionColor{ 0.0151946f, 0.0990099f, 0.0716247f };
	std::array<float, 3> shallowColorScale{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> deepColorScale{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> reflectionColorScale{ 1.0f, 1.0f, 1.0f };

	[[nodiscard]] bool IsRiverFlowDisabled() const noexcept { return disableRiverFlow || realisticDistantWater; }
};

class Settings
{
public:
	static constexpr float kMaxScale = 3.0f;

	static Settings& GetSingleton()
	{
		static Settings instance;
		return instance;
	}

	SettingsSnapshot GetSnapshot() const
	{
		std::shared_lock lock(m_snapshotLock);
		return m_snapshot;
	}

	void UpdateSnapshot()
	{
		std::unique_lock lock(m_snapshotLock);
		m_snapshot.enabled = enabled;
		m_snapshot.windowAlpha = windowAlpha;
		m_snapshot.toggleKey = toggleKey;
		m_snapshot.clearUnderwater = clearUnderwater;
		m_snapshot.fullShaderDistantWater = fullShaderDistantWater;
		m_snapshot.disableRiverFlow = disableRiverFlow;
		m_snapshot.realisticDistantWater = realisticDistantWater;
		m_snapshot.removeSunGlints = removeSunGlints;
		m_snapshot.fixDepthProperties = fixDepthProperties;
		m_snapshot.waterTransparency = waterTransparency;
		m_snapshot.depthReflections = depthReflections;
		m_snapshot.depthRefraction = depthRefraction;
		m_snapshot.depthNormals = depthNormals;
		m_snapshot.depthSpecularLighting = depthSpecularLighting;
		m_snapshot.hasCustomColors = hasCustomColors;
		m_snapshot.colorMode = colorMode;
		m_snapshot.shallowColor = shallowColor;
		m_snapshot.deepColor = deepColor;
		m_snapshot.reflectionColor = reflectionColor;
		m_snapshot.shallowColorScale = shallowColorScale;
		m_snapshot.deepColorScale = deepColorScale;
		m_snapshot.reflectionColorScale = reflectionColorScale;
		m_version.fetch_add(1, std::memory_order_release);
	}

	static const SettingsSnapshot& GetFastSnapshot()
	{
		static thread_local uint32_t s_cachedVersion = 0;
		static thread_local SettingsSnapshot s_cachedSnapshot{};

		auto& inst = GetSingleton();
		uint32_t curVer = inst.m_version.load(std::memory_order_relaxed);
		if (s_cachedVersion != curVer) {
			s_cachedSnapshot = inst.GetSnapshot();
			s_cachedVersion = curVer;
		}
		return s_cachedSnapshot;
	}

	static uint32_t ParseKeyCode(const std::string& a_str)
	{
		try {
			uint32_t val = std::stoul(a_str, nullptr, 16);
			if (val > 0 && val <= 0xFE) {
				return val;
			}
		} catch (...) {}
		return VK_F3;
	}

	static std::string GetKeyName(uint32_t a_vk)
	{
		LONG scanCode = MapVirtualKeyA(a_vk, MAPVK_VK_TO_VSC);
		if (scanCode != 0) {
			char nameBuf[64]{ 0 };
			if (GetKeyNameTextA(scanCode << 16, nameBuf, sizeof(nameBuf)) > 0) {
				return std::string(nameBuf);
			}
		}
		char hexBuf[16];
		snprintf(hexBuf, sizeof(hexBuf), "0x%02X", a_vk);
		return std::string(hexBuf);
	}

	void Load()
	{
		const auto path = std::filesystem::path("Data/SKSE/Plugins/WaterOnSteroids.ini");
		if (!std::filesystem::exists(path)) {
			Save();
			return;
		}

		std::ifstream file(path);
		if (!file.is_open()) {
			return;
		}

		std::string currentSection;
		std::string line;
		while (std::getline(file, line)) {
			auto start = line.find_first_not_of(" \t\r\n");
			if (start == std::string::npos || line[start] == ';' || line[start] == '#') {
				continue;
			}

			if (line[start] == '[') {
				auto endBracket = line.find(']', start);
				if (endBracket != std::string::npos) {
					currentSection = line.substr(start + 1, endBracket - start - 1);
					auto s = currentSection.find_first_not_of(" \t");
					auto e = currentSection.find_last_not_of(" \t");
					if (s != std::string::npos) {
						currentSection = currentSection.substr(s, e - s + 1);
					}
				}
				continue;
			}

			auto eq = line.find('=', start);
			if (eq == std::string::npos) {
				continue;
			}

			auto keyEnd = line.find_last_not_of(" \t", eq - 1);
			if (keyEnd == std::string::npos || keyEnd < start) {
				continue;
			}
			std::string key = line.substr(start, keyEnd - start + 1);

			auto valStart = line.find_first_not_of(" \t", eq + 1);
			if (valStart == std::string::npos) {
				continue;
			}
			auto valEnd = line.find_first_of(" \t\r\n;#", valStart);
			std::string val = (valEnd == std::string::npos) ? line.substr(valStart) : line.substr(valStart, valEnd - valStart);

			if (key == "bEnabled") {
				enabled = (val == "true" || val == "1");
			} else if (key == "fWindowAlpha") {
				try { windowAlpha = std::stof(val); } catch (...) {}
			} else if (key == "uToggleKey") {
				toggleKey = ParseKeyCode(val);
			}
			else if (key == "bFixDepthProperties") {
				fixDepthProperties = (val == "true" || val == "1");
			} else if (key == "fWaterTransparency") {
				try { waterTransparency = std::stof(val); } catch (...) {}
			} else if (key == "fReflections") {
				try { depthReflections = std::stof(val); } catch (...) {}
			} else if (key == "fRefraction") {
				try { depthRefraction = std::stof(val); } catch (...) {}
			} else if (key == "fNormals") {
				try { depthNormals = std::stof(val); } catch (...) {}
			} else if (key == "fSpecularLighting") {
				try { depthSpecularLighting = std::stof(val); } catch (...) {}
			}
			else if (key == "bHasCustomColors") {
				hasCustomColors = (val == "true" || val == "1");
			} else if (key == "iColorMode") {
				try { colorMode = std::stoi(val); } catch (...) {}
			} else if (key == "fShallowColorR") {
				try { shallowColor[0] = std::stof(val); } catch (...) {}
			} else if (key == "fShallowColorG") {
				try { shallowColor[1] = std::stof(val); } catch (...) {}
			} else if (key == "fShallowColorB") {
				try { shallowColor[2] = std::stof(val); } catch (...) {}
			} else if (key == "fDeepColorR") {
				try { deepColor[0] = std::stof(val); } catch (...) {}
			} else if (key == "fDeepColorG") {
				try { deepColor[1] = std::stof(val); } catch (...) {}
			} else if (key == "fDeepColorB") {
				try { deepColor[2] = std::stof(val); } catch (...) {}
			} else if (key == "fReflectionColorR") {
				try { reflectionColor[0] = std::stof(val); } catch (...) {}
			} else if (key == "fReflectionColorG") {
				try { reflectionColor[1] = std::stof(val); } catch (...) {}
			} else if (key == "fReflectionColorB") {
				try { reflectionColor[2] = std::stof(val); } catch (...) {}
			} else if (key == "fShallowScaleR") {
				try { shallowColorScale[0] = std::stof(val); } catch (...) {}
			} else if (key == "fShallowScaleG") {
				try { shallowColorScale[1] = std::stof(val); } catch (...) {}
			} else if (key == "fShallowScaleB") {
				try { shallowColorScale[2] = std::stof(val); } catch (...) {}
			} else if (key == "fDeepScaleR") {
				try { deepColorScale[0] = std::stof(val); } catch (...) {}
			} else if (key == "fDeepScaleG") {
				try { deepColorScale[1] = std::stof(val); } catch (...) {}
			} else if (key == "fDeepScaleB") {
				try { deepColorScale[2] = std::stof(val); } catch (...) {}
			} else if (key == "fReflectionScaleR") {
				try { reflectionColorScale[0] = std::stof(val); } catch (...) {}
			} else if (key == "fReflectionScaleG") {
				try { reflectionColorScale[1] = std::stof(val); } catch (...) {}
			} else if (key == "fReflectionScaleB") {
				try { reflectionColorScale[2] = std::stof(val); } catch (...) {}
			}
			else if (key == "bClearUnderwater") {
				clearUnderwater = (val == "true" || val == "1");
			} else if (key == "bFullShaderDistantWater") {
				fullShaderDistantWater = (val == "true" || val == "1");
			} else if (key == "bDisableRiverFlow") {
				disableRiverFlow = (val == "true" || val == "1");
			} else if (key == "bRealisticDistantWater") {
				realisticDistantWater = (val == "true" || val == "1");
			} else if (key == "bRemoveSunGlints") {
				removeSunGlints = (val == "true" || val == "1");
			}
			else if (key == "bLiveWaterLog") {
				liveWaterLog = (val == "true" || val == "1");
			}
		}

		depthReflections = std::clamp(depthReflections, 0.0f, 1.0f);
		depthRefraction = std::clamp(depthRefraction, 0.0f, 1.0f);
		depthSpecularLighting = std::clamp(depthSpecularLighting, 0.0f, 1.0f);
		depthNormals = std::clamp(depthNormals, 0.0f, 1.0f);
		waterTransparency = std::clamp(waterTransparency, 0.0f, 100.0f);

		colorMode = std::clamp(colorMode, 0, 1);
		for (int i = 0; i < 3; ++i) {
			shallowColor[i] = std::clamp(shallowColor[i], 0.0f, 1.0f);
			deepColor[i] = std::clamp(deepColor[i], 0.0f, 1.0f);
			reflectionColor[i] = std::clamp(reflectionColor[i], 0.0f, 1.0f);
			shallowColorScale[i] = std::clamp(shallowColorScale[i], 0.0f, kMaxScale);
			deepColorScale[i] = std::clamp(deepColorScale[i], 0.0f, kMaxScale);
			reflectionColorScale[i] = std::clamp(reflectionColorScale[i], 0.0f, kMaxScale);
		}

		windowAlpha = std::clamp(windowAlpha, 0.0f, 1.0f);

		logger::info("Settings loaded: Enabled={}, WindowAlpha={:.2f}, ToggleKey={} (0x{:02X}), FixDepthProps={}, WaterTransparency={:.0f}%, Reflections={:.2f}, Refraction={:.2f}, Normals={:.2f}, Specular={:.2f}, CustomColors={}, ColorMode={}",
			enabled, windowAlpha, GetKeyName(toggleKey), toggleKey, fixDepthProperties, waterTransparency, depthReflections, depthRefraction, depthNormals, depthSpecularLighting, hasCustomColors, colorMode);

		UpdateSnapshot();
	}

	void Save() const
	{
		const auto path = std::filesystem::path("Data/SKSE/Plugins/WaterOnSteroids.ini");
		std::filesystem::create_directories(path.parent_path());

		std::ofstream file(path);
		if (!file.is_open()) {
			return;
		}

		bool afterValue = false;
		auto line = [&](std::string_view a_text) {
			if (afterValue && a_text.starts_with('#')) {
				file << '\n';
			}
			afterValue = false;
			file << a_text << '\n';
		};
		auto flag = [&](std::string_view a_key, bool a_value) {
			file << a_key << " = " << (a_value ? "true" : "false") << '\n';
			afterValue = true;
		};
		auto value = [&](std::string_view a_key, auto a_value) {
			file << a_key << " = " << a_value << '\n';
			afterValue = true;
		};
		auto rgb = [&](std::string_view a_key, const std::array<float, 3>& a_rgb) {
			value(std::format("{}R", a_key), a_rgb[0]);
			value(std::format("{}G", a_key), a_rgb[1]);
			value(std::format("{}B", a_key), a_rgb[2]);
		};

		line("# WaterOnSteroids settings. The in-game Save button rewrites this file.");
		line("# Manual edits are read when the game starts.");
		line("");

		line("[General]");
		line("# Turns the whole plugin on or off.");
		flag("bEnabled", enabled);
		line("# Menu background opacity, 0.0 to 1.0.");
		value("fWindowAlpha", windowAlpha);
		line("# Key that opens the menu (Windows key code, 0x72 = F3).");
		char keyHex[16];
		snprintf(keyHex, sizeof(keyHex), "0x%02X", toggleKey);
		value("uToggleKey", keyHex);
		line("");

		line("[Features]");
		line("# Clear Underwater: reduces the underwater blur.");
		flag("bClearUnderwater", clearUnderwater);
		line("# Full Shader for Distant Water: distant water uses the same shader and colors as nearby water.");
		flag("bFullShaderDistantWater", fullShaderDistantWater);
		line("# Disable River Flow: removes river flow to hide the seam with distant water.");
		line("# Always on while bRealisticDistantWater is true.");
		flag("bDisableRiverFlow", disableRiverFlow);
		line("# Realistic Distant Water: keeps water color and depth at any distance. Also turns on bDisableRiverFlow.");
		flag("bRealisticDistantWater", realisticDistantWater);
		line("# Remove Sun Glints: removes the flashing sun specks on calm water.");
		flag("bRemoveSunGlints", removeSunGlints);
		line("");

		line("[WaterDepth]");
		line("# Water Depth: uses the values below instead of the game's (false = game values).");
		line("# Each value: 0.0 = no fade, 1.0 = full fade toward the shore.");
		flag("bFixDepthProperties", fixDepthProperties);
		line("# Reflection fade near the shore.");
		value("fReflections", depthReflections);
		line("# Refraction fade near the shore.");
		value("fRefraction", depthRefraction);
		line("# Wave fade near the shore.");
		value("fNormals", depthNormals);
		line("# Sun glare fade near the shore.");
		value("fSpecularLighting", depthSpecularLighting);
		line("");

		line("[WaterTransparency]");
		line("# Water clarity in percent: 0 = unchanged, 100 = fully clear.");
		value("fWaterTransparency", waterTransparency);
		line("");

		line("[WaterColors]");
		line("# Uses the colors below (false = the game's colors).");
		flag("bHasCustomColors", hasCustomColors);
		line("# 0 = Direct Override (uses f*Color), 1 = Tint / Multiplier (uses f*Scale).");
		value("iColorMode", colorMode);
		line("# Shallow water color, 0.0 to 1.0 per channel.");
		rgb("fShallowColor", shallowColor);
		line("# Deep water color, 0.0 to 1.0 per channel.");
		rgb("fDeepColor", deepColor);
		line("# Reflection color, 0.0 to 1.0 per channel.");
		rgb("fReflectionColor", reflectionColor);
		line("# Shallow color multiplier, 0.0 to 3.0 per channel (1.0 = unchanged).");
		rgb("fShallowScale", shallowColorScale);
		line("# Deep color multiplier, 0.0 to 3.0 per channel (1.0 = unchanged).");
		rgb("fDeepScale", deepColorScale);
		line("# Reflection color multiplier, 0.0 to 3.0 per channel (1.0 = unchanged).");
		rgb("fReflectionScale", reflectionColorScale);
		line("");

		line("[Logging]");
		line("# Starts the live water log when the game starts.");
		flag("bLiveWaterLog", liveWaterLog);
	}

	bool enabled{ true };
	float windowAlpha{ 0.70f };
	uint32_t toggleKey{ VK_F3 };

	bool clearUnderwater{ true };
	bool fullShaderDistantWater{ true };
	bool disableRiverFlow{ true };
	bool realisticDistantWater{ true };
	bool removeSunGlints{ true };

	bool fixDepthProperties{ true };
	float waterTransparency{ 10.0f };
	float depthReflections{ 1.0f };
	float depthRefraction{ 0.70f };
	float depthNormals{ 0.91f };
	float depthSpecularLighting{ 1.00f };

	bool hasCustomColors{ false };
	int colorMode{ 0 };
	std::array<float, 3> shallowColor{ 0.0f, 0.0f, 0.0f };
	std::array<float, 3> deepColor{ 0.0f, 0.118812f, 0.101167f };
	std::array<float, 3> reflectionColor{ 0.0151946f, 0.0990099f, 0.0716247f };
	std::array<float, 3> shallowColorScale{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> deepColorScale{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> reflectionColorScale{ 1.0f, 1.0f, 1.0f };

	bool liveWaterLog{ false };

private:
	Settings()
	{
		UpdateSnapshot();
	}
	Settings(const Settings&) = delete;
	Settings& operator=(const Settings&) = delete;

	mutable std::shared_mutex m_snapshotLock;
	SettingsSnapshot m_snapshot;
	std::atomic<uint32_t> m_version{ 1 };
};
