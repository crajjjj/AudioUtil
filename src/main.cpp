#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#include <chrono>
#include <thread>

#include "CaptionManager.h"
#include "Config.h"
#include "FolderCache.h"
#include "Tags.h"
#include "FuzCache.h"
#include "FuzSlots.h"
#include "GagState.h"
#include "InstanceManager.h"
#include "LipSync.h"
#include "TongueState.h"
#include "PPABridge.h"
#include "PapyrusAPI.h"

using namespace SKSE;
using namespace SKSE::log;

namespace
{
	void InitializeLogging()
	{
		auto path = log_directory();
		if (!path) {
			stl::report_and_fail("Unable to lookup SKSE logs directory.");
		}
		*path /= PluginDeclaration::GetSingleton()->GetName();
		*path += L".log";

		std::shared_ptr<spdlog::logger> logger;
		if (IsDebuggerPresent()) {
			logger = std::make_shared<spdlog::logger>(
				"Global", std::make_shared<spdlog::sinks::msvc_sink_mt>());
		} else {
			logger = std::make_shared<spdlog::logger>(
				"Global", std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true));
		}
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);

		spdlog::set_default_logger(std::move(logger));
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%s:%#] %v");
	}

	void InitializePapyrus()
	{
		auto* papyrus = GetPapyrusInterface();
		if (!papyrus || !papyrus->Register(PapyrusAPI::RegisterFuncs)) {
			stl::report_and_fail("Failed to register Papyrus bindings.");
		}
		log::info("Papyrus functions bound.");
	}

	// Decode-ahead the whole config's .fuz set on a detached background thread, so
	// their cache wavs are on disk BEFORE the next launch indexes loose files.
	// (A cache wav written mid-session can't be read back by the engine's resource
	// loader until a relaunch — the loose-file index is frozen at game start, and
	// MO2's USVFS never surfaces a mid-session file at all. So the first launch
	// after new fuz is added warms silently; every launch after plays first try.)
	// Idempotent + cheap once cached: FuzCache::Resolve short-circuits on a disk
	// hit. Gated on [general] prewarm_fuz. Detached like LipSync's ticker.
	void PrewarmFuzCache()
	{
		if (!Config::Get()->prewarmFuz) {
			return;
		}
		std::thread([] {
			const auto start = std::chrono::steady_clock::now();
			const auto files = FolderCache::AllAudioFiles();
			std::size_t fuz = 0;
			std::size_t decoded = 0;
			for (const auto& f : files) {
				if (!FuzCache::IsFuzPath(f)) {
					continue;
				}
				++fuz;
				if (!FuzCache::Resolve(f).empty()) {
					++decoded;
				}
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			log::info("Fuz prewarm: {} of {} fuz cached (of {} audio files) in {} ms. Newly "
					  "decoded lines become playable after the next game restart.",
				decoded, fuz, files.size(), ms);
		}).detach();
	}

	void OnMessage(MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case MessagingInterface::kDataLoaded:
			Config::Load();
			Tags::Configure(*Config::Get());  // before Rebuild: the scan parses folder/file tags
			FolderCache::Rebuild();
			GagState::Resolve(*Config::Get());
			TongueState::Resolve(*Config::Get());
			InstanceManager::ApplyConfigGroupVolumes();
			LipSync::ApplyConfig();
			CaptionManager::ApplyConfig();
			FuzSlots::Configure(Config::Get()->fuzSlots);
			FuzCache::EnforceCacheCap();
			PPABridge::TryConnect();
			PrewarmFuzCache();
			break;
		case MessagingInterface::kPreLoadGame:
		case MessagingInterface::kNewGame:
			InstanceManager::StopAll();
			LipSync::Reset();
			CaptionManager::Reset();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const LoadInterface* skse)
{
	InitializeLogging();

	const auto* plugin = PluginDeclaration::GetSingleton();
	log::info("{} v{} is loading...", plugin->GetName(), plugin->GetVersion());
	log::info("Runtime version: {}", REL::Module::get().version().string());

	Init(skse);
	InitializePapyrus();

	if (const auto* messaging = GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	}

	log::info("{} loaded.", plugin->GetName());
	return true;
}
