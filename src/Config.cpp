#include "Config.h"

#include <toml++/toml.hpp>

#include <charconv>
#include <format>

namespace Config
{
	namespace
	{
		constexpr auto CONFIG_PATH = "Data\\SKSE\\Plugins\\AudioUtil\\AudioUtil.toml"sv;

		std::shared_ptr<const Settings> g_settings = std::make_shared<Settings>();
		std::mutex                      g_settingsLock;
		std::mutex                      g_loadLock;

		void StoreSettings(std::shared_ptr<const Settings> a_settings)
		{
			std::scoped_lock lock{ g_settingsLock };
			g_settings = std::move(a_settings);
		}

		// canonical npc-override key: normalized plugin name + "|" + lowercase hex local id
		// without leading zeros. "MyFollower.esp|000D62" -> "myfolloweresp|d62"
		std::optional<std::string> CanonicalFormKey(std::string_view a_plugin, std::uint32_t a_localID)
		{
			if (a_plugin.empty() || a_localID == 0) {
				return std::nullopt;
			}
			return MakeNpcKey(a_plugin, a_localID);
		}

		std::optional<std::string> ParseFormKey(std::string_view a_key)
		{
			const auto sep = a_key.find('|');
			if (sep == std::string_view::npos) {
				return std::nullopt;
			}
			auto idText = a_key.substr(sep + 1);
			if (idText.starts_with("0x") || idText.starts_with("0X")) {
				idText.remove_prefix(2);
			}
			std::uint32_t localID = 0;
			const auto [ptr, ec] = std::from_chars(idText.data(), idText.data() + idText.size(), localID, 16);
			if (ec != std::errc{} || ptr != idText.data() + idText.size()) {
				return std::nullopt;
			}
			return CanonicalFormKey(a_key.substr(0, sep), localID);
		}

		void ReadStringMap(const toml::table* a_table, StringMap& a_out, bool a_normalizeValues)
		{
			if (!a_table) {
				return;
			}
			for (auto&& [key, value] : *a_table) {
				if (const auto str = value.value<std::string>()) {
					a_out[Normalize(key.str())] = a_normalizeValues ? Normalize(*str) : *str;
				}
			}
		}

		// a map value that is either a single slot id or an array of candidates
		SlotList ReadSlotList(const toml::node& a_value)
		{
			SlotList out;
			if (const auto single = a_value.value<std::string>()) {
				out.push_back(*single);
			} else if (const auto* arr = a_value.as_array()) {
				for (const auto& entry : *arr) {
					if (const auto item = entry.value<std::string>()) {
						out.push_back(*item);
					}
				}
			}
			return out;
		}
	}

	std::string MakeNpcKey(std::string_view a_plugin, std::uint32_t a_localID)
	{
		return std::format("{}|{:x}", Normalize(a_plugin), a_localID);
	}

	std::string Normalize(std::string_view a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		for (const unsigned char ch : a_text) {
			if (std::isalnum(ch)) {
				out.push_back(static_cast<char>(std::tolower(ch)));
			}
		}
		return out;
	}

	bool Load()
	{
		std::scoped_lock lock{ g_loadLock };

		toml::table root;
		try {
			root = toml::parse_file(CONFIG_PATH);
		} catch (const toml::parse_error& e) {
			logger::error("Failed to parse {}: {} (line {})", CONFIG_PATH,
				e.description(), e.source().begin.line);
			return false;
		} catch (const std::exception& e) {
			logger::error("Failed to read {}: {}", CONFIG_PATH, e.what());
			return false;
		}

		auto settings = std::make_shared<Settings>();

		if (const auto* general = root["general"].as_table()) {
			settings->voiceRoot = (*general)["voice_root"].value_or(settings->voiceRoot);
			settings->sfxRoot = (*general)["sfx_root"].value_or(settings->sfxRoot);
			settings->soundFlags = (*general)["sound_flags"].value_or(settings->soundFlags);
			settings->soundPriority = (*general)["sound_priority"].value_or(settings->soundPriority);
			settings->defaultFemaleSlot = (*general)["default_female_slot"].value_or(settings->defaultFemaleSlot);
			settings->defaultMaleSlot = (*general)["default_male_slot"].value_or(settings->defaultMaleSlot);
			settings->pcFemaleSlot = (*general)["pc_female_slot"].value_or(settings->pcFemaleSlot);
			settings->pcMaleSlot = (*general)["pc_male_slot"].value_or(settings->pcMaleSlot);
			settings->voice3D = (*general)["voice_3d"].value_or(settings->voice3D);
			settings->voiceNoInterrupt = (*general)["voice_no_interrupt"].value_or(settings->voiceNoInterrupt);

			if (const auto level = (*general)["log_level"].value<std::string>()) {
				const auto lvl = spdlog::level::from_str(*level);
				spdlog::default_logger()->set_level(lvl);
				spdlog::flush_on(lvl);
			}
		}

		if (const auto* ppa = root["ppa"].as_table()) {
			settings->ppaEnabled = (*ppa)["enable"].value_or(settings->ppaEnabled);
			settings->ppaEventRateMs = (*ppa)["event_rate_ms"].value_or(settings->ppaEventRateMs);
		}

		if (const auto* lipsync = root["lipsync"].as_table()) {
			settings->lipsyncEnabled = (*lipsync)["enable"].value_or(settings->lipsyncEnabled);
			settings->lipsyncGain = (*lipsync)["gain"].value_or(settings->lipsyncGain);
			settings->lipsyncAttackMs = (*lipsync)["attack_ms"].value_or(settings->lipsyncAttackMs);
			settings->lipsyncReleaseMs = (*lipsync)["release_ms"].value_or(settings->lipsyncReleaseMs);
			settings->lipsyncMinLevel = (*lipsync)["min_level"].value_or(settings->lipsyncMinLevel);
		}

		if (const auto* gag = root["gag"].as_table()) {
			settings->gagEnabled = (*gag)["enable"].value_or(settings->gagEnabled);
			settings->gagDefaultCategory = Normalize((*gag)["default_category"].value_or(""s));
			if (const auto* keywords = (*gag)["keywords"].as_array()) {
				for (const auto& entry : *keywords) {
					const auto text = entry.value<std::string>();
					if (!text) {
						continue;
					}
					// "Plugin.esp|7EB8" -> {plugin, localID}; the id is hex, 0x optional
					const auto sep = text->find('|');
					if (sep == std::string::npos) {
						logger::warn("[gag] keyword '{}' missing '|' — expected 'Plugin.esp|FormID'", *text);
						continue;
					}
					std::string_view idText = std::string_view(*text).substr(sep + 1);
					if (idText.starts_with("0x") || idText.starts_with("0X")) {
						idText.remove_prefix(2);
					}
					GagKeyword kw;
					kw.plugin = text->substr(0, sep);
					const auto [ptr, ec] =
						std::from_chars(idText.data(), idText.data() + idText.size(), kw.localID, 16);
					if (kw.plugin.empty() || ec != std::errc{} || ptr != idText.data() + idText.size()) {
						logger::warn("[gag] keyword '{}' has an invalid form id", *text);
						continue;
					}
					settings->gagKeywords.push_back(std::move(kw));
				}
			}
		}

		if (const auto* slots = root["slot"].as_array()) {
			for (const auto& entry : *slots) {
				const auto* table = entry.as_table();
				if (!table) {
					continue;
				}
				Slot slot;
				slot.id = (*table)["id"].value_or(""s);
				slot.root = (*table)["path"].value_or(""s);
				slot.fallbackSlot = Normalize((*table)["fallback"].value_or(""s));
				slot.gagSlot = Normalize((*table)["gag_slot"].value_or(""s));
				const auto sex = (*table)["sex"].value_or(""s);
				slot.sex = !sex.empty() && (sex[0] == 'f' || sex[0] == 'F') ? 'F' : 'M';
				if (const auto* categories = (*table)["categories"].as_table()) {
					for (auto&& [catName, catValue] : *categories) {
						if (const auto* files = catValue.as_array()) {
							auto& list = slot.categories[Normalize(catName.str())];
							for (const auto& file : *files) {
								if (const auto path = file.value<std::string>()) {
									list.push_back(*path);
								}
							}
						} else if (const auto dir = catValue.value<std::string>()) {
							// string value = one folder to scan (vs array = file list)
							slot.categoryDirs[Normalize(catName.str())] = *dir;
						}
					}
				}
				if (slot.id.empty() || (slot.root.empty() && slot.categories.empty() && slot.categoryDirs.empty())) {
					logger::warn("Skipping [[slot]] entry with missing id, or neither path nor categories");
					continue;
				}
				settings->slots.push_back(std::move(slot));
			}
		}

		if (const auto* remap = root["voicetype_remap"].as_table()) {
			settings->voicetypeRemapEnabled = (*remap)["enable"].value_or(true);
			for (auto&& [key, value] : *remap) {
				if (key.str() == "enable"sv) {
					continue;
				}
				if (const auto str = value.value<std::string>()) {
					settings->voicetypeRemap[Normalize(key.str())] = Normalize(*str);
				}
			}
		}

		if (const auto* voiceMap = root["voicetype_map"].as_table()) {
			for (auto&& [key, value] : *voiceMap) {
				if (auto slots = ReadSlotList(value); !slots.empty()) {
					settings->voicetypeMap[Normalize(key.str())] = std::move(slots);
				}
			}
		}

		// [race_map] hints are substring-matched, so precedence must not depend on
		// toml table iteration (alphabetical): longest hint first makes the most
		// specific entry win ("darkelf" and "highelf" both beat "elf")
		if (const auto* raceMap = root["race_map"].as_table()) {
			for (auto&& [key, value] : *raceMap) {
				if (auto slots = ReadSlotList(value); !slots.empty()) {
					settings->raceMap.emplace_back(Normalize(key.str()), std::move(slots));
				}
			}
			std::stable_sort(settings->raceMap.begin(), settings->raceMap.end(),
				[](const auto& a_lhs, const auto& a_rhs) {
					return a_lhs.first.size() > a_rhs.first.size();
				});
		}

		if (const auto* overrides = root["npc_overrides"].as_table()) {
			for (auto&& [key, value] : *overrides) {
				const auto slotID = value.value<std::string>();
				const auto canonical = ParseFormKey(key.str());
				if (!slotID || !canonical) {
					logger::warn("Invalid [npc_overrides] entry '{}' — expected 'Plugin.esp|FormID' = \"SlotId\"",
						key.str());
					continue;
				}
				settings->npcOverrides[*canonical] = *slotID;
			}
		}
		ReadStringMap(root["category_aliases"]["female"].as_table(), settings->femaleAliases, true);
		ReadStringMap(root["category_aliases"]["male"].as_table(), settings->maleAliases, true);
		ReadStringMap(root["male_only_remap"].as_table(), settings->maleOnlyRemap, true);
		ReadStringMap(root["category_fallbacks"]["female"].as_table(), settings->femaleFallbacks, true);
		ReadStringMap(root["category_fallbacks"]["male"].as_table(), settings->maleFallbacks, true);
		ReadStringMap(root["sfx"].as_table(), settings->sfxTable, false);

		if (const auto* groups = root["groups"].as_table()) {
			for (auto&& [key, value] : *groups) {
				if (const auto vol = value.value<double>()) {
					settings->groupVolumes[std::string(key.str())] = static_cast<float>(*vol);
				}
			}
		}

		StoreSettings(std::shared_ptr<const Settings>(std::move(settings)));

		const auto& loaded = *Get();
		logger::info("Config loaded: {} slots, {} voicetype mappings, {} sfx entries, flags=0x{:X}, priority={}",
			loaded.slots.size(), loaded.voicetypeMap.size(), loaded.sfxTable.size(),
			loaded.soundFlags, loaded.soundPriority);
		return true;
	}

	std::shared_ptr<const Settings> Get()
	{
		std::scoped_lock lock{ g_settingsLock };
		return g_settings;
	}

	const Slot* FindSlot(const Settings& a_settings, std::string_view a_id)
	{
		// Match on the normalized id so slot lookups are case- AND space-insensitive
		// (the documented contract). Callers pass ids from mixed sources — some raw
		// (npc_overrides, voicetype_map, pc slots), some already normalized (a slot's
		// fallback) — so normalize both sides here rather than trusting the caller.
		const auto idNorm = Normalize(a_id);
		for (const auto& slot : a_settings.slots) {
			if (Normalize(slot.id) == idNorm) {
				return &slot;
			}
		}
		return nullptr;
	}
}
