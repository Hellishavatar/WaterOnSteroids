#include "PCH.h"
#include "Version.h"
#include "Settings.h"
#include "WaterPatcher.h"
#include "Menu.h"
#include "WaterWatch.h"
#include "CellWaterMap.h"
#include "ShaderPatch.h"

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

void MessageHandler(SKSE::MessagingInterface::Message* a_message)
{
	if (!a_message) return;

	switch (a_message->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		WaterPatcher::GetSingleton().Initialize();
		Menu::InstallHooks();
		CellWaterMap::Scan();
		ShaderPatch::ReportStatus();
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
	SKSE::Init(a_skse);
	ShaderPatch::Install();
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
