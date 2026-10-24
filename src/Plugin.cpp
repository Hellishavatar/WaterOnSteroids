#include "PCH.h"
#include "Version.h"
#include "Settings.h"
#include "WaterPatcher.h"
#include "Menu.h"
#include "WaterWatch.h"

#include <spdlog/sinks/basic_file_sink.h>

void InitializeLog()
{
	auto path = logger::log_directory();
	if (!path) {
		return;
	}

	*path /= std::format("{}.log", Plugin::NAME);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
	auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));

	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
}

bool IsCommunityShadersInstalled()
{
	if (GetModuleHandleW(L"CommunityShaders.dll")) {
		return true;
	}
	std::error_code error;
	return std::filesystem::exists("Data/SKSE/Plugins/CommunityShaders.dll", error);
}

void MessageHandler(SKSE::MessagingInterface::Message* a_message)
{
	if (!a_message) return;

	switch (a_message->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		WaterPatcher::GetSingleton().Initialize();
		Menu::InstallHooks();
		break;
	case SKSE::MessagingInterface::kPostLoadGame:
	case SKSE::MessagingInterface::kNewGame:
		WaterPatcher::GetSingleton().RefreshWaterSettings();
		Menu::ForceUnlockControls();
		break;
	default:
		break;
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	if (const auto runtime = a_skse->RuntimeVersion(); runtime > REL::Version(1, 7, 104, 0)) {
		logger::error("This build supports Skyrim SE and AE up to 1.7.104 only; refusing to load in Skyrim {}.", runtime.string("."));
		return false;
	}
	if (IsCommunityShadersInstalled()) {
		logger::error("Community Shaders is installed. Water On Steroids does not support it and did not load.");
		return false;
	}
	SKSE::Init(a_skse);
	if (REL::Module::GetRuntime() == REL::Module::Runtime::VR) {
		logger::error("This build supports Skyrim SE and AE only; refusing to load in Skyrim VR.");
		return false;
	}
	logger::info("Loaded v{}.{}.{} | {} {}",
		Plugin::VERSION.major(),
		Plugin::VERSION.minor(),
		Plugin::VERSION.patch(),
		REL::Module::IsAE() ? "AE" : "SE",
		REL::Module::get().version().string("."));

	Settings::GetSingleton().Load();
	if (Settings::GetSingleton().liveWaterLog) {
		WaterWatch::SetEnabled(true);
	}

	auto messaging = SKSE::GetMessagingInterface();
	if (messaging) {
		messaging->RegisterListener(MessageHandler);
	}

	return true;
}
