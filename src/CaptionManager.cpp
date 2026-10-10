#include "CaptionManager.h"

#include "Config.h"

#include <condition_variable>
#include <filesystem>
#include <thread>
#include <toml++/toml.hpp>

namespace CaptionManager
{
	namespace
	{
		// coarse: captions only need to disappear promptly, not animate
		constexpr auto TICK = std::chrono::milliseconds(250);
		// stream startup is asynchronous (see InstanceManager): give a handle
		// this long to report playing before declaring the line dead
		constexpr auto AUDIBLE_TIMEOUT = std::chrono::milliseconds(2500);

		// ---------- sidecar lookup ----------

		// parsed sidecar tables by wav path; nullptr = known miss (a missing
		// sidecar is the COMMON case — most files have no caption — so misses
		// are cached and never logged)
		std::unordered_map<std::string, std::shared_ptr<const toml::table>> g_sidecarCache;
		std::string g_language{ "en" };  // resolved [captions] language key
		std::mutex  g_dataLock;          // guards the two above

		std::string ToLower(std::string_view a_text)
		{
			std::string out{ a_text };
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return out;
		}

		// "auto" -> the game's sLanguage ini value, mapped to the two-letter
		// sidecar keys for the stock localizations; an unknown language falls
		// through as its lowercased full name, so a sidecar can still key by
		// e.g. "turkish". A non-"auto" config value is used verbatim.
		std::string ResolveLanguage(const std::string& a_configured)
		{
			const auto configured = ToLower(a_configured);
			if (configured != "auto") {
				return configured;
			}
			const char* iniValue = nullptr;
			if (auto* collection = RE::INISettingCollection::GetSingleton()) {
				if (auto* setting = collection->GetSetting("sLanguage:General")) {
					iniValue = setting->GetString();
				}
			}
			const auto name = ToLower(iniValue ? iniValue : "english");
			static constexpr std::pair<std::string_view, std::string_view> kCodes[] = {
				{ "english", "en" }, { "french", "fr" }, { "german", "de" },
				{ "italian", "it" }, { "spanish", "es" }, { "russian", "ru" },
				{ "polish", "pl" }, { "czech", "cs" }, { "japanese", "ja" },
				{ "chinese", "zh" }, { "portuguese", "pt" },
			};
			for (const auto& [full, code] : kCodes) {
				if (name == full) {
					return std::string{ code };
				}
			}
			return name;
		}

		// the wav's sidecar table, parsed once and cached (loose files only)
		std::shared_ptr<const toml::table> GetSidecar(const std::string& a_dataRelPath)
		{
			const auto dot = a_dataRelPath.rfind('.');
			if (dot == std::string::npos) {
				return nullptr;
			}
			const auto key = ToLower(a_dataRelPath.substr(0, dot)) + ".toml";
			{
				std::scoped_lock lock{ g_dataLock };
				if (const auto it = g_sidecarCache.find(key); it != g_sidecarCache.end()) {
					return it->second;
				}
			}

			std::shared_ptr<const toml::table> table;
			const auto diskPath = std::filesystem::current_path() / "Data" / key;
			std::error_code ec;
			if (std::filesystem::exists(diskPath, ec)) {
				try {
					table = std::make_shared<const toml::table>(toml::parse_file(diskPath.string()));
				} catch (const std::exception& e) {
					logger::warn("Caption sidecar '{}' failed to parse: {}", key, e.what());
				}
			}

			std::scoped_lock lock{ g_dataLock };
			if (g_sidecarCache.size() > 512) {
				g_sidecarCache.clear();
			}
			g_sidecarCache[key] = table;
			return table;
		}

		// ---------- active entries ----------

		struct Entry
		{
			RE::ObjectRefHandle speaker;
			std::int32_t        instanceId = 0;
			std::string         text;
			RE::BSSoundHandle   handle;
			std::chrono::steady_clock::time_point createdAt;
			bool audible = false;   // the stream was seen playing at least once
			bool stopped = false;   // early-stop notification arrived
			bool done = false;      // the line is over; swept at the end of the pass
			bool injected = false;  // our SubtitleInfo currently sits in the manager
		};

		// Leaked on purpose, same reasoning as LipSync: the detached ticker
		// thread runs until the process dies and may touch this state during
		// static teardown at game exit.
		std::vector<Entry>& g_entries = *new std::vector<Entry>();
		std::mutex&         g_entriesLock = *new std::mutex();

		std::atomic<bool> g_enabled{ true };
		std::atomic<bool> g_hud{ true };

		std::atomic<bool> g_applyPending{ false };
		std::once_flag    g_tickerOnce;
		std::condition_variable& g_cv = *new std::condition_variable();  // leaked, see g_entries

		// game thread only; caller holds the manager's spin lock
		void Inject(RE::SubtitleManager* a_manager, const Entry& a_entry)
		{
			// real player->speaker distance (at inject time), so captions of
			// different actors resolve by the engine's own "closest speaker wins"
			// rule: in a scene that's normally the actor the player is looking at.
			// The player's own lines get 0: the PC wins ties over partners.
			float distance = 0.0f;
			const auto speakerPtr = a_entry.speaker.get();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (speakerPtr && player && speakerPtr.get() != player) {
				distance = player->GetPosition().GetDistance(speakerPtr->GetPosition());
			}

			RE::SubtitleInfo info;
			info.speaker = a_entry.speaker;
			info.pad04 = 0;
			info.subtitle = a_entry.text.c_str();
			info.targetDistance = distance;
			// forceDisplay: always eligible, shows even with general subtitles
			// off in the player's settings (the pack shipping captions is the opt-in)
			info.forceDisplay = true;
			a_manager->subtitles.push_back(info);
		}

		// game thread: ask the HUD to drop the subtitle it is showing. The message
		// is the one powerof3's Floating Subtitles sends for the same purpose,
		// `show` included (whether the handler reads it is not known). False when
		// it could not be queued.
		bool HideHudSubtitle()
		{
			auto* queue = RE::UIMessageQueue::GetSingleton();
			auto* strings = RE::InterfaceStrings::GetSingleton();
			if (!queue || !strings) {
				return false;
			}
			auto* data = static_cast<RE::HUDData*>(queue->CreateUIMessageData(strings->hudData));
			if (!data) {
				return false;
			}
			auto type = RE::HUD_MESSAGE_TYPE::kHideSubtitle;
#ifdef ENABLE_SKYRIM_VR
			type = RE::GetHUDMessageType(type);  // VR shifts the ids past kSetLoadDoorInfo
#endif
			data->type = type;
			data->show = true;
			queue->AddMessage(strings->hudMenu, RE::UI_MESSAGE_TYPE::kUpdate, data);
			return true;
		}

		// caller holds the manager's spin lock. Remove OUR entry: match speaker +
		// exact text, first hit only. False when it was already gone.
		bool EraseInjected(RE::SubtitleManager* a_manager, const Entry& a_entry)
		{
			auto& subtitles = a_manager->subtitles;
			for (auto it = subtitles.begin(); it != subtitles.end(); ++it) {
				if (it->speaker.native_handle() == a_entry.speaker.native_handle() &&
					std::string_view(it->subtitle.c_str()) == std::string_view(a_entry.text)) {
					subtitles.erase(it);
					return true;
				}
			}
			return false;
		}

		// caller holds g_entriesLock: no later live line belongs to this speaker
		bool IsNewestOfSpeaker(std::size_t a_index)
		{
			const auto speaker = g_entries[a_index].speaker.native_handle();
			for (auto i = a_index + 1; i < g_entries.size(); ++i) {
				if (!g_entries[i].done && g_entries[i].speaker.native_handle() == speaker) {
					return false;
				}
			}
			return true;
		}

		// main thread (SKSE task): sweep finished captions and keep each speaker's
		// newest live line, and only that one, in the manager
		void ApplyAll()
		{
			auto*      manager = RE::SubtitleManager::GetSingleton();
			const auto now = std::chrono::steady_clock::now();
			const bool enabled = g_enabled.load();
			const bool hud = g_hud.load();
			bool       clearedHud = false;
			{
				std::scoped_lock lock{ g_entriesLock };
				for (auto& entry : g_entries) {
					if (entry.stopped || !enabled || !entry.speaker.get()) {
						entry.done = true;
					} else if (entry.handle.IsValid() && entry.handle.IsPlaying()) {
						entry.audible = true;
					} else if (entry.audible || now - entry.createdAt > AUDIBLE_TIMEOUT) {
						entry.done = true;  // line finished (or the stream never started)
					}
				}

				if (manager) {
					// One guard for the whole pass: the engine's own subtitle pass
					// must see the array, currentSpeaker and the queued hide change
					// together, or its show could land before our hide.
					RE::BSSpinLockGuard guard{ manager->lock };
					const auto wanted = [&](std::size_t a_index) {
						return hud && !g_entries[a_index].done && IsNewestOfSpeaker(a_index);
					};
					bool pulledCurrent = false;  // we removed a line of the speaker the HUD is on
					for (std::size_t i = 0; i < g_entries.size(); ++i) {
						auto& entry = g_entries[i];
						if (entry.injected && !wanted(i)) {
							entry.injected = false;
							if (EraseInjected(manager, entry) && manager->currentSpeaker == entry.speaker) {
								pulledCurrent = true;
							}
						}
					}
					for (std::size_t i = 0; i < g_entries.size(); ++i) {
						auto& entry = g_entries[i];
						if (!entry.injected && wanted(i)) {
							Inject(manager, entry);
							entry.injected = true;
						}
					}
					// The engine appears to track the HUD subtitle by SPEAKER
					// (currentSpeaker), not by text: its pass shows a line only
					// when the best entry's speaker differs from that one. That
					// is inferred from a field report and the Floating Subtitles
					// hooks, not disassembled. Splicing the array skips the
					// bookkeeping the engine's own add/remove do, so when an
					// actor's next line took the place of the last one the old
					// text stayed up and the new one never showed, until another
					// speaker's subtitle came and went. So blank the HUD and
					// forget the speaker: the engine's next pass shows whatever
					// is left. The speaker is only forgotten once the hide is
					// queued, since without it the engine's own "nothing left,
					// hide" is the one thing that would still clear the text.
					if (pulledCurrent && HideHudSubtitle()) {
						manager->currentSpeaker.reset();
						clearedHud = true;
					}
				}
				std::erase_if(g_entries, [](const Entry& a_entry) { return a_entry.done; });
			}
			if (clearedHud) {
				logger::debug("Captions: took a finished line off the HUD");
			}
		}

		void EnsureTicker()
		{
			std::call_once(g_tickerOnce, []() {
				std::thread([]() {
					for (;;) {
						{
							std::unique_lock lock{ g_entriesLock };
							g_cv.wait(lock, []() { return !g_entries.empty(); });
						}
						if (!g_applyPending.exchange(true)) {
							if (auto* task = SKSE::GetTaskInterface()) {
								task->AddTask([]() {
									ApplyAll();
									g_applyPending.store(false);
								});
							} else {
								g_applyPending.store(false);
							}
						}
						std::this_thread::sleep_for(TICK);
					}
				}).detach();
			});
		}

		void SendCaptionEvent(RE::Actor* a_speaker, const std::string& a_text,
			std::int32_t a_instanceId)
		{
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}
			const auto handle = a_speaker->GetHandle();
			task->AddTask([handle, a_text, a_instanceId]() {
				const auto actorPtr = handle.get();
				if (!actorPtr) {
					return;
				}
				auto* source = SKSE::GetModCallbackEventSource();
				if (!source) {
					return;
				}
				SKSE::ModCallbackEvent modEvent{
					RE::BSFixedString("AudioUtil_Caption"), RE::BSFixedString(a_text),
					static_cast<float>(a_instanceId), actorPtr.get()
				};
				source->SendEvent(&modEvent);
			});
		}
	}

	void Start(RE::Actor* a_speaker, const std::string& a_dataRelPath,
		RE::BSSoundHandle a_handle, std::int32_t a_instanceId)
	{
		if (!g_enabled.load() || !a_speaker || !a_handle.IsValid()) {
			return;
		}
		auto text = TextForFile(a_dataRelPath);
		if (text.empty()) {
			return;
		}

		// consumers get the event even with hud = false (custom caption UI)
		SendCaptionEvent(a_speaker, text, a_instanceId);

		Entry entry;
		entry.speaker = RE::ObjectRefHandle{ a_speaker };
		entry.instanceId = a_instanceId;
		entry.text = std::move(text);
		entry.handle = a_handle;
		entry.createdAt = std::chrono::steady_clock::now();

		EnsureTicker();
		{
			std::scoped_lock lock{ g_entriesLock };
			g_entries.push_back(std::move(entry));
		}
		g_cv.notify_one();
	}

	void OnInstanceStopped(std::int32_t a_instanceId)
	{
		std::scoped_lock lock{ g_entriesLock };
		for (auto& entry : g_entries) {
			if (entry.instanceId == a_instanceId) {
				entry.stopped = true;  // ticker removes the subtitle on its next pass
			}
		}
	}

	std::string TextForFile(const std::string& a_dataRelPath)
	{
		if (a_dataRelPath.empty()) {
			return {};
		}
		const auto table = GetSidecar(a_dataRelPath);
		if (!table) {
			return {};
		}
		std::string language;
		{
			std::scoped_lock lock{ g_dataLock };
			language = g_language;
		}
		if (const auto value = (*table)[language].value<std::string>()) {
			return *value;
		}
		// a pack without this localization still captions in its english text
		if (language != "en") {
			if (const auto value = (*table)["en"].value<std::string>()) {
				return *value;
			}
		}
		return {};
	}

	void SetEnabled(bool a_enabled)
	{
		g_enabled.store(a_enabled);
		// live entries are swept (and their subtitles removed) by the next tick
	}

	bool Enabled()
	{
		return g_enabled.load();
	}

	void ApplyConfig()
	{
		const auto settings = Config::Get();
		{
			std::scoped_lock lock{ g_dataLock };
			g_language = ResolveLanguage(settings->captionsLanguage);
			g_sidecarCache.clear();  // sidecars may have been edited; cheap to redo
		}
		g_hud.store(settings->captionsHud);
		SetEnabled(settings->captionsEnabled);
		logger::info("Captions: enabled={} hud={} language='{}'",
			settings->captionsEnabled, settings->captionsHud, g_language);
	}

	void Reset()
	{
		// collect the texts we injected, then scrub them on the game thread —
		// the subtitle array is engine state and may survive into the next save
		std::vector<Entry> stale;
		{
			std::scoped_lock lock{ g_entriesLock };
			for (auto& entry : g_entries) {
				if (entry.injected) {
					stale.push_back(entry);
				}
			}
			g_entries.clear();
		}
		if (stale.empty()) {
			return;
		}
		if (auto* task = SKSE::GetTaskInterface()) {
			task->AddTask([stale = std::move(stale)]() {
				auto* manager = RE::SubtitleManager::GetSingleton();
				if (!manager) {
					return;
				}
				// no HUD handover here: nothing of ours follows, so the engine's
				// own pass (or the load itself) clears what was on screen
				RE::BSSpinLockGuard guard{ manager->lock };
				for (const auto& entry : stale) {
					EraseInjected(manager, entry);
				}
			});
		}
	}
}
