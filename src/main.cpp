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
#include "MouthClaim.h"
#include "TongueState.h"
#include "PPABridge.h"
#include "PapyrusAPI.h"
#include "VoiceLog.h"

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

	// Decode-ahead the config's .fuz set on a detached background thread, so first
	// plays skip the inline decode. Stops at [general] fuz_cache_max_mb (CacheFull):
	// a large fuz pack decodes to gigabytes of PCM, and past the cap the next
	// launch's eviction would only delete what this wrote. The lines it does not
	// reach decode on first play. Without fuz_slots, a cache wav written mid-session
	// cannot be read by the engine until a relaunch (the loose-file index is frozen
	// at game start, and MO2's USVFS never surfaces a mid-session file), so the
	// first launch after new fuz is added warms silently.
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
			const auto fuz = std::ranges::count_if(files, [](const std::string& a_file) {
				return FuzCache::IsFuzPath(a_file);
			});
			std::size_t visited = 0;
			std::size_t cached = 0;
			bool        full = false;
			for (const auto& f : files) {
				if (!FuzCache::IsFuzPath(f)) {
					continue;
				}
				if (FuzCache::CacheFull()) {
					full = true;
					break;
				}
				++visited;
				if (!FuzCache::Resolve(f).empty()) {
					++cached;
				}
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			const auto settings = Config::Get();
			log::info("Fuz prewarm: {} of {} fuz cached (of {} audio files) in {} ms.{}{}", cached, fuz,
				files.size(), ms,
				full ? std::format(" Stopped at the fuz_cache_max_mb cap ({} MB) after {} fuz: the rest "
								   "decode on first play.",
						   settings->fuzCacheMaxMB, visited) :
					   "",
				settings->fuzSlots <= 0 ? " fuz_slots is 0: newly decoded lines play after the next game restart." : "");
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
			VoiceLog::ApplyConfig();
			FuzSlots::Configure(Config::Get()->fuzSlots);
			FuzCache::EnforceCacheCap();
			PPABridge::TryConnect();
			PrewarmFuzCache();
			break;
		case MessagingInterface::kPreLoadGame:
		case MessagingInterface::kNewGame:
			InstanceManager::StopAll();
			LipSync::Reset();
			MouthClaim::Reset();
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
