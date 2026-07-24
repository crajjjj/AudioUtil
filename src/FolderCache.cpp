#include "FolderCache.h"

#include "AudioEngine.h"
#include "Config.h"

namespace FolderCache
{
	void AuditExplicitFiles(const Config::Settings& a_settings);  // defined after Rebuild
	void AuditWiring(const Config::Settings& a_settings);         // defined after Rebuild

	namespace
	{
		struct Folder
		{
			std::vector<std::string> files;  // data-relative paths
			std::vector<std::size_t> deck;   // shuffled indices, consumed from the back
			std::size_t              lastPlayed = SIZE_MAX;
		};

		std::unordered_map<std::string, Folder> g_folders;  // key -> folder
		std::unordered_map<std::string, std::string> g_resolveCache;
		std::unordered_map<std::string, bool>        g_missLogged;
		std::mutex   g_lock;
		std::mt19937 g_rng{ std::random_device{}() };

		bool IsAudioFile(const std::filesystem::path& a_path)
		{
			const auto ext = a_path.extension().string();
			return _stricmp(ext.c_str(), ".wav") == 0 || _stricmp(ext.c_str(), ".xwm") == 0;
		}

		std::string DataRelative(const std::filesystem::path& a_abs, const std::filesystem::path& a_dataRoot)
		{
			// lexical only: std::filesystem::relative() canonicalizes through the
			// filesystem, and under MO2's USVFS that resolves Data\Sound\... to the
			// real mod folder (E:\...\mods\<mod>\Sound\...), yielding "..\..\mods\..."
			// which the game's resource loader cannot open. Entry paths are composed
			// lexically from a_dataRoot, so a lexical relative is exact.
			auto rel = a_abs.lexically_relative(a_dataRoot).string();
			std::replace(rel.begin(), rel.end(), '/', '\\');
			return rel;
		}

		// scan one directory (non-recursive) into g_folders under a_key. Caller holds g_lock.
		bool ScanDir(const std::string& a_key, const std::filesystem::path& a_dir,
			const std::filesystem::path& a_dataRoot)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(a_dir, ec)) {
				return false;
			}
			Folder folder;
			for (const auto& entry : std::filesystem::directory_iterator(a_dir, ec)) {
				if (entry.is_regular_file(ec) && IsAudioFile(entry.path())) {
					folder.files.push_back(DataRelative(entry.path(), a_dataRoot));
				}
			}
			if (folder.files.empty()) {
				return false;
			}
			std::sort(folder.files.begin(), folder.files.end());
			g_folders[a_key] = std::move(folder);
			return true;
		}

		std::filesystem::path DataRoot()
		{
			return std::filesystem::current_path() / "Data";
		}

		// caller holds g_lock
		bool HasKey(const std::string& a_key)
		{
			return g_folders.contains(a_key);
		}
	}

	void Rebuild()
	{
		const auto settings = Config::Get();
		const auto dataRoot = DataRoot();

		std::scoped_lock lock{ g_lock };
		g_folders.clear();
		g_resolveCache.clear();
		g_missLogged.clear();

		std::size_t voiceFolders = 0;
		for (const auto& slot : settings->slots) {
			// how many category folders THIS slot contributed (explicit + scanned).
			// Used below to flag a pack slot whose `path` exists but scanned empty.
			std::size_t slotFolders = 0;
			// explicit [slot.categories] first: trusted as-is (no filesystem check,
			// so they can point at BSA-packed audio the engine resolves at play time)
			for (const auto& [category, files] : slot.categories) {
				if (files.empty()) {
					continue;
				}
				const auto key = Config::Normalize(slot.id) + "/" + category;
				if (HasKey(key)) {
					logger::warn("Slot {}: duplicate explicit category '{}' ignored", slot.id, category);
					continue;
				}
				Folder folder;
				folder.files.reserve(files.size());
				for (auto file : files) {
					std::replace(file.begin(), file.end(), '/', '\\');
					folder.files.push_back(std::move(file));
				}
				g_folders[key] = std::move(folder);
				++voiceFolders;
				++slotFolders;
			}

			// folder-string categories: scanned like the [sfx] table ('Sound\...' =
			// full Data-relative path, otherwise relative to the slot's path).
			// Loose files only - BSA-packed audio needs the file-list form above.
			for (const auto& [category, folder] : slot.categoryDirs) {
				const auto key = Config::Normalize(slot.id) + "/" + category;
				if (HasKey(key)) {
					logger::warn("Slot {}: duplicate explicit category '{}' ignored", slot.id, category);
					continue;
				}
				const bool absolute = folder.size() >= 6 && _strnicmp(folder.c_str(), "sound\\", 6) == 0;
				std::filesystem::path dir;
				if (absolute) {
					dir = dataRoot / folder;
				} else if (!slot.root.empty()) {
					dir = dataRoot / slot.root / folder;
				} else {
					logger::warn("Slot {} category '{}': relative folder '{}' needs the slot to have a path",
						slot.id, category, folder);
					continue;
				}
				if (ScanDir(key, dir, dataRoot)) {
					++voiceFolders;
					++slotFolders;
				} else {
					logger::warn("Slot {} category '{}': no audio files in {}", slot.id, category, dir.string());
				}
			}

			if (slot.root.empty()) {
				continue;
			}
			const auto slotDir = dataRoot / slot.root;
			std::error_code ec;
			if (!std::filesystem::is_directory(slotDir, ec)) {
				logger::warn("Slot {}: folder not found: {}", slot.id, slotDir.string());
				continue;
			}
			for (const auto& entry : std::filesystem::directory_iterator(slotDir, ec)) {
				if (!entry.is_directory(ec)) {
					continue;
				}
				const auto category = Config::Normalize(entry.path().filename().string());
				const auto key = Config::Normalize(slot.id) + "/" + category;
				if (HasKey(key)) {
					logger::warn("Slot {}: duplicate normalized category '{}' — folder {} ignored",
						slot.id, category, entry.path().string());
					continue;
				}
				if (ScanDir(key, entry.path(), dataRoot)) {
					++voiceFolders;
					++slotFolders;
				}
			}

			// A slot that declares a scannable `path` is meant to hold a pack. The
			// scan above is otherwise UNAUDITED (unlike explicit file lists, see
			// AuditExplicitFiles) — so a folder that exists but yields no category
			// subfolders is invisible in the log and silently falls back to the
			// slot's `fallback` (e.g. F1 -> stock F0 moans). Surface it: a pack
			// scanned to zero almost always means an extra nesting level, folder
			// names that don't match the category scheme, or BSA-packed audio a
			// loose-file scan can't see.
			if (slotFolders == 0) {
				logger::warn("Slot {}: 0 category folders under {} — folder exists but has no "
					"<Category> subfolders. Check for an extra nesting level, non-standard "
					"folder names, or BSA-packed audio (folder scans see loose files only).",
					slot.id, slotDir.string());
			} else {
				logger::info("Slot {}: {} category folders scanned under {}",
					slot.id, slotFolders, slotDir.string());
			}
		}

		std::size_t sfxFolders = 0;
		for (const auto& [name, folder] : settings->sfxTable) {
			// [sfx] values are full Data-relative paths, same as slot paths
			std::filesystem::path dir = dataRoot / folder;
			const auto key = "sfx/" + name;
			if (ScanDir(key, dir, dataRoot)) {
				++sfxFolders;
			} else {
				logger::warn("SFX '{}': no audio files in {}", name, dir.string());
			}
		}

		logger::info("FolderCache: {} voice category folders, {} sfx folders", voiceFolders, sfxFolders);

		AuditExplicitFiles(*settings);
		AuditWiring(*settings);
	}

	// walk every explicit [slot.categories] file list (the hand-written, BSA-capable
	// paths - creature slots, F1/F2 orgasm lists) and log any that do not resolve in
	// the current load order, so a typo'd path is visible in one game load instead of
	// only surfacing as silence when that category is requested. Folder scans and
	// folder-string categories aren't audited: their files exist by virtue of being
	// found on disk.
	void AuditExplicitFiles(const Config::Settings& a_settings)
	{
		std::size_t checked = 0;
		std::size_t missing = 0;
		for (const auto& slot : a_settings.slots) {
			for (const auto& [category, files] : slot.categories) {
				for (const auto& file : files) {
					++checked;
					if (!AudioEngine::ResourceExists(file)) {
						++missing;
						logger::warn("Missing audio: slot {} category '{}' -> '{}' (path resolves to nothing)",
							slot.id, category, file);
					}
				}
			}
		}
		if (missing == 0) {
			logger::info("Audit: all {} explicit slot files resolve", checked);
		} else {
			logger::warn("Audit: {} of {} explicit slot files are MISSING (see warnings above)",
				missing, checked);
		}
	}

	// A slot is "wired" only if some config route points an actor at it: a
	// default / pc / sfx assignment, an [npc_overrides] / [voicetype_map] /
	// [race_map] target, or another slot's fallback / gag_slot. A slot that holds
	// audio yet is the target of none of these resolves for no actor through the
	// config — the follower-pack mistake (drop a pack into F4, forget to map a
	// voicetype). This is invisible otherwise: the content scans in fine, the log
	// looks clean, the slot just never resolves. (A script CAN still play it
	// directly via PlayVoiceFromSlot, so this is a warning, not an error.) Empty
	// unrouted slots are NOT flagged — F2/F3 ship as reserved placeholders until a
	// pack is installed — so we key off slots that actually resolved content.
	// Caller holds g_lock (reads g_folders).
	void AuditWiring(const Config::Settings& a_settings)
	{
		// slots that ended up with >=1 resolved category (explicit or scanned):
		// g_folders keys are "normslot/category" (and "sfx/..." for the flat table).
		std::unordered_set<std::string> withContent;
		for (const auto& [key, folder] : g_folders) {
			if (const auto slash = key.find('/'); slash != std::string::npos) {
				withContent.insert(key.substr(0, slash));
			}
		}

		// every slot id some routing path can reach (normalized; add() re-normalizes,
		// which is idempotent, so it's safe on already-normalized fallback/gag ids).
		std::unordered_set<std::string> wired;
		const auto add = [&wired](std::string_view a_id) {
			if (!a_id.empty()) {
				wired.insert(Config::Normalize(a_id));
			}
		};
		add(a_settings.defaultFemaleSlot);
		add(a_settings.defaultMaleSlot);
		add(a_settings.pcFemaleSlot);
		add(a_settings.pcMaleSlot);
		add(a_settings.sfxSlot);
		for (const auto& [key, target] : a_settings.npcOverrides) {
			add(target);
		}
		for (const auto& [voicetype, ids] : a_settings.voicetypeMap) {
			for (const auto& id : ids) {
				add(id);
			}
		}
		for (const auto& [hint, ids] : a_settings.raceMap) {
			for (const auto& id : ids) {
				add(id);
			}
		}
		for (const auto& slot : a_settings.slots) {
			add(slot.fallbackSlot);
			add(slot.gagSlot);
		}
		// note: [voicetype_remap] values are voicetype NAMES, not slot ids — they
		// redirect to another voicetype that must itself map, so they're not routes.

		std::size_t orphaned = 0;
		for (const auto& slot : a_settings.slots) {
			const auto norm = Config::Normalize(slot.id);
			if (withContent.contains(norm) && !wired.contains(norm)) {
				++orphaned;
				logger::warn("Slot {}: has audio but no config route points to it — no actor "
					"resolves to this slot (a script may still target it via PlayVoiceFromSlot). "
					"To route actors, add it to [voicetype_map] / [npc_overrides] / [race_map], set "
					"it as a default/pc/sfx slot, or reference it as another slot's fallback / gag_slot.",
					slot.id);
			}
		}
		if (orphaned == 0) {
			logger::info("Wiring: every slot with audio is routable");
		}
	}

	std::string ResolveVoiceKey(const Config::Settings& a_settings,
		const Config::Slot& a_slot, std::string_view a_category)
	{
		const auto slotNorm = Config::Normalize(a_slot.id);
		const auto catNorm = Config::Normalize(a_category);
		const auto cacheKey = slotNorm + "/" + catNorm;

		std::scoped_lock lock{ g_lock };

		if (const auto it = g_resolveCache.find(cacheKey); it != g_resolveCache.end()) {
			return it->second;
		}

		// alias -> male_only_remap -> category-fallback resolution within one slot
		const auto resolveInSlot = [&](const Config::Slot& a_inSlot) -> std::string {
			const auto inSlotNorm = Config::Normalize(a_inSlot.id);
			// Female slots use the female category layer; male AND 'all' (sex-neutral:
			// creature / sfx) slots share the male layer, since presets author their
			// creature/neutral category fallbacks there (e.g. Breathing -> Orgasm).
			// male_only_remap below stays 'M'-only, so 'all' slots skip it.
			const auto& aliases = a_inSlot.sex == 'F' ? a_settings.femaleAliases : a_settings.maleAliases;
			const auto& fallbacks = a_inSlot.sex == 'F' ? a_settings.femaleFallbacks : a_settings.maleFallbacks;

			const auto tryCandidates = [&](std::string_view a_cat) -> std::string {
				std::vector<std::string> candidates;
				candidates.emplace_back(a_cat);
				if (const auto it = aliases.find(std::string(a_cat)); it != aliases.end()) {
					candidates.push_back(it->second);
				}
				if (a_inSlot.sex == 'M') {
					if (const auto it = a_settings.maleOnlyRemap.find(std::string(a_cat));
						it != a_settings.maleOnlyRemap.end()) {
						candidates.push_back(it->second);
					}
				}
				for (const auto& candidate : candidates) {
					const auto key = inSlotNorm + "/" + candidate;
					if (HasKey(key)) {
						return key;
					}
				}
				return {};
			};

			std::string result = tryCandidates(catNorm);
			if (result.empty()) {
				if (const auto it = fallbacks.find(catNorm); it != fallbacks.end()) {
					result = tryCandidates(it->second);
				}
			}
			return result;
		};

		// walk the per-slot fallback chain: a scanned pack slot backfills any
		// category it lacks from its fallback slot (hop cap breaks cycles)
		std::string resolved;
		const Config::Slot* slot = &a_slot;
		for (int hop = 0; slot != nullptr && hop < 4; ++hop) {
			resolved = resolveInSlot(*slot);
			if (!resolved.empty() || slot->fallbackSlot.empty()) {
				break;
			}
			slot = Config::FindSlot(a_settings, slot->fallbackSlot);
		}

		g_resolveCache[cacheKey] = resolved;
		if (resolved.empty() && !g_missLogged[cacheKey]) {
			g_missLogged[cacheKey] = true;
			logger::warn("No audio for slot {} category '{}' (no folder, alias, fallback, or fallback slot)",
				a_slot.id, a_category);
		}
		return resolved;
	}

	std::string ResolveDirKey(std::string_view a_dataRelativeFolder)
	{
		std::string cleaned{ a_dataRelativeFolder };
		std::replace(cleaned.begin(), cleaned.end(), '/', '\\');
		const auto key = "dir/" + Config::Normalize(cleaned);

		std::scoped_lock lock{ g_lock };
		if (!HasKey(key)) {
			const auto dataRoot = DataRoot();
			if (!ScanDir(key, dataRoot / cleaned, dataRoot)) {
				if (!g_missLogged[key]) {
					g_missLogged[key] = true;
					logger::warn("PlayFolder: no audio files in {}", cleaned);
				}
				return {};
			}
		}
		return key;
	}

	std::string PickNext(const std::string& a_folderKey)
	{
		std::scoped_lock lock{ g_lock };
		const auto it = g_folders.find(a_folderKey);
		if (it == g_folders.end() || it->second.files.empty()) {
			return {};
		}
		auto& folder = it->second;

		if (folder.files.size() == 1) {
			return folder.files[0];
		}

		if (folder.deck.empty()) {
			folder.deck.resize(folder.files.size());
			for (std::size_t i = 0; i < folder.deck.size(); ++i) {
				folder.deck[i] = i;
			}
			std::shuffle(folder.deck.begin(), folder.deck.end(), g_rng);
			// avoid an immediate repeat across refills. Compare the file PATH, not
			// the deck index, so a clip listed more than once (deliberate weighting)
			// still never plays back-to-back. Swap in the first entry that's a
			// genuinely different clip; if every entry is the same file there's
			// nothing else to play and a repeat is unavoidable.
			if (folder.lastPlayed != SIZE_MAX &&
				folder.files[folder.deck.back()] == folder.files[folder.lastPlayed]) {
				for (auto d = folder.deck.begin(); d + 1 != folder.deck.end(); ++d) {
					if (folder.files[*d] != folder.files[folder.lastPlayed]) {
						std::swap(*d, folder.deck.back());
						break;
					}
				}
			}
		}

		const auto index = folder.deck.back();
		folder.deck.pop_back();
		folder.lastPlayed = index;
		return folder.files[index];
	}

	int FileCount(const std::string& a_folderKey)
	{
		std::scoped_lock lock{ g_lock };
		const auto it = g_folders.find(a_folderKey);
		return it != g_folders.end() ? static_cast<int>(it->second.files.size()) : 0;
	}
}
