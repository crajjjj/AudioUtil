#include "Config.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <vector>

namespace Config
{
	namespace
	{
		// Base config, then overlay files. The base loads first (holds the SFW
		// defaults + machine-wide tuning); each config\*.toml then merges on top in
		// sorted filename order, so consumer mods ship their own file instead of
		// clobbering a single shared one. Numeric prefixes (50_, 99_) let a mod
		// deliberately win the global scalars. See MergeFile for merge semantics.
		constexpr auto CONFIG_DIR = "Data\\SKSE\\Plugins\\AudioUtil"sv;
		constexpr auto BASE_FILE = "AudioUtil.toml"sv;
		constexpr auto OVERLAY_SUBDIR = "config"sv;

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

	// Merge one parsed TOML file into an accumulating Settings. Called once per
	// config file in load order (base first, then config\*.toml sorted), so the
	// merge rules are:
	//   - scalar globals ([general]/[ppa]/[lipsync]/[gag] toggles) are
	//     last-writer-wins: value_or(settings->x) carries the prior file's value
	//     forward, and a later file only overrides keys it actually specifies.
	//   - maps ([npc_overrides], [voicetype_map], [sfx], aliases, ...) union,
	//     last-writer-wins per key.
	//   - [[slot]] is keyed by id: a later file with the same id replaces the
	//     whole slot (no per-category deep merge) and logs a warning.
	//   - [gag].keywords and [race_map] accumulate; race_map is sorted once by
	//     Load after all files are merged.
	// Global scalar sections ([general], [ppa], [lipsync], and the [gag] scalar
	// toggles) are OWNED BY THE BASE AudioUtil.toml only. A config\*.toml overlay
	// that sets them is ignored with a warning — this keeps a content mod (or the
	// user's own tuning) from silently stomping engine-wide globals. Overlays
	// remain free to contribute all the ADDITIVE data below (slots, sfx,
	// resolution/category maps, and [gag].keywords).
	void MergeFile(Settings* settings, const toml::table& root, bool a_isBase)
	{
		if (const auto* general = root["general"].as_table()) {
			if (!a_isBase) {
				logger::warn("[general] in an overlay is ignored — globals come only from the base AudioUtil.toml");
			} else {
				settings->sfxSlot = (*general)["sfx_slot"].value_or(settings->sfxSlot);
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
		}

		if (const auto* ppa = root["ppa"].as_table()) {
			if (!a_isBase) {
				logger::warn("[ppa] in an overlay is ignored — globals come only from the base AudioUtil.toml");
			} else {
				settings->ppaEnabled = (*ppa)["enable"].value_or(settings->ppaEnabled);
				settings->ppaEventRateMs = (*ppa)["event_rate_ms"].value_or(settings->ppaEventRateMs);
			}
		}

		if (const auto* lipsync = root["lipsync"].as_table()) {
			if (!a_isBase) {
				logger::warn("[lipsync] in an overlay is ignored — globals come only from the base AudioUtil.toml");
			} else {
				settings->lipsyncEnabled = (*lipsync)["enable"].value_or(settings->lipsyncEnabled);
				settings->lipsyncGain = (*lipsync)["gain"].value_or(settings->lipsyncGain);
				settings->lipsyncAttackMs = (*lipsync)["attack_ms"].value_or(settings->lipsyncAttackMs);
				settings->lipsyncReleaseMs = (*lipsync)["release_ms"].value_or(settings->lipsyncReleaseMs);
				settings->lipsyncMinLevel = (*lipsync)["min_level"].value_or(settings->lipsyncMinLevel);
				settings->lipsyncBlockInDialogue = (*lipsync)["block_in_dialogue"].value_or(settings->lipsyncBlockInDialogue);
				if (const auto* blocked = (*lipsync)["block_categories"].as_array()) {
					for (const auto& entry : *blocked) {
						if (const auto cat = entry.value<std::string>()) {
							settings->lipsyncBlockCategories.insert(Normalize(*cat));
						}
					}
				}
			}
		}

		if (const auto* gag = root["gag"].as_table()) {
			// [gag].keywords is additive (any file may add gag markers), but the
			// enable/default_category scalars are base-only like the rest.
			if (a_isBase) {
				settings->gagEnabled = (*gag)["enable"].value_or(settings->gagEnabled);
				settings->gagDefaultCategory = Normalize((*gag)["default_category"].value_or(""s));
			} else if (gag->contains("enable") || gag->contains("default_category")) {
				logger::warn("[gag] enable/default_category in an overlay are ignored (base-only); its keywords still merge");
			}
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
				// sex: 'F' female, 'A' all/any (sex-neutral: creatures, sfx pools), else
				// 'M' male (the default). First letter only, case-insensitive.
				const auto sex = (*table)["sex"].value_or(""s);
				slot.sex = 'M';
				if (!sex.empty()) {
					const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(sex[0])));
					slot.sex = c == 'f' ? 'F' : c == 'a' ? 'A' : 'M';
				}
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
				// whole-slot last-wins: a later file redefining an existing id replaces
				// it outright (no per-category deep merge). Match on normalized id.
				const auto idNorm = Normalize(slot.id);
				const auto existing = std::find_if(settings->slots.begin(), settings->slots.end(),
					[&](const Slot& a_s) { return Normalize(a_s.id) == idNorm; });
				if (existing != settings->slots.end()) {
					logger::warn("Slot '{}' redefined by a later config file — replacing the earlier definition", slot.id);
					*existing = std::move(slot);
				} else {
					settings->slots.push_back(std::move(slot));
				}
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

		// [race_map] hints accumulate across files; Load sorts the merged list
		// longest-hint-first once all files are in (see the sort there). Sorting
		// per-file would mis-order hints contributed by different files.
		if (const auto* raceMap = root["race_map"].as_table()) {
			for (auto&& [key, value] : *raceMap) {
				if (auto slots = ReadSlotList(value); !slots.empty()) {
					settings->raceMap.emplace_back(Normalize(key.str()), std::move(slots));
				}
			}
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

	}

	// Ordered list of config files to merge: base first, then config\*.toml in
	// sorted filename order. Missing base/overlay dir is not an error (an install
	// may ship only overlays, or only the base). Returns paths as strings for
	// toml::parse_file.
	struct ConfigFile
	{
		std::string path;
		bool        isBase;  // the base AudioUtil.toml owns globals; overlays are additive-only
	};

	std::vector<ConfigFile> CollectConfigFiles()
	{
		namespace fs = std::filesystem;
		std::vector<ConfigFile> files;

		const auto base = fs::path(CONFIG_DIR) / std::string(BASE_FILE);
		std::error_code ec;
		if (fs::exists(base, ec)) {
			files.push_back({ base.string(), true });
		}

		const auto overlayDir = fs::path(CONFIG_DIR) / std::string(OVERLAY_SUBDIR);
		std::vector<std::string> overlays;
		// Walk with the non-throwing ec overloads throughout: a missing/unreadable
		// dir leaves `it == end` (clean skip), and a mid-iteration filesystem error
		// stops the walk via the loop condition instead of throwing out of Load
		// (which runs inside an SKSE message handler — an escaped exception crashes).
		auto       it = fs::directory_iterator(overlayDir, ec);
		const auto end = fs::directory_iterator();
		for (; !ec && it != end; it.increment(ec)) {
			std::error_code entryEc;  // separate: a per-entry query must not end the walk
			if (!it->is_regular_file(entryEc) || entryEc) {
				continue;
			}
			auto path = it->path();
			auto ext = path.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (ext == ".toml") {
				overlays.push_back(path.string());
			}
		}
		// directory_iterator order is unspecified; sort so precedence is deterministic
		std::sort(overlays.begin(), overlays.end());
		for (auto& o : overlays) {
			files.push_back({ std::move(o), false });
		}
		return files;
	}

	bool Load()
	{
		std::scoped_lock lock{ g_loadLock };

		const auto files = CollectConfigFiles();
		if (files.empty()) {
			logger::error("No config files found under {} — keeping previous settings", CONFIG_DIR);
			return false;
		}

		auto settings = std::make_shared<Settings>();
		int merged = 0;
		for (const auto& file : files) {
			toml::table root;
			try {
				root = toml::parse_file(file.path);
			} catch (const toml::parse_error& e) {
				logger::error("Failed to parse {}: {} (line {}) — skipping this file",
					file.path, e.description(), e.source().begin.line);
				continue;
			} catch (const std::exception& e) {
				logger::error("Failed to read {}: {} — skipping this file", file.path, e.what());
				continue;
			}
			MergeFile(settings.get(), root, file.isBase);
			++merged;
			logger::info("Merged config file: {}{}", file.path, file.isBase ? " (base)" : " (overlay)");
		}

		// every file failed to parse — don't clobber working settings with an empty set
		if (merged == 0) {
			logger::error("All {} config file(s) failed to parse — keeping previous settings", files.size());
			return false;
		}

		// [race_map] hints are substring-matched, so precedence must not depend on
		// toml table iteration or file order: longest hint first makes the most
		// specific entry win ("darkelf" and "highelf" both beat "elf")
		std::stable_sort(settings->raceMap.begin(), settings->raceMap.end(),
			[](const auto& a_lhs, const auto& a_rhs) {
				return a_lhs.first.size() > a_rhs.first.size();
			});

		StoreSettings(std::shared_ptr<const Settings>(std::move(settings)));

		const auto& loaded = *Get();
		logger::info("Config loaded from {} file(s): {} slots, {} voicetype mappings, {} sfx entries, flags=0x{:X}, priority={}",
			merged, loaded.slots.size(), loaded.voicetypeMap.size(), loaded.sfxTable.size(),
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
