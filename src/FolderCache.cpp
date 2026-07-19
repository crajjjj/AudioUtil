#include "FolderCache.h"

namespace FolderCache
{
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
			auto rel = std::filesystem::relative(a_abs, a_dataRoot).string();
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
				}
			}
		}

		std::size_t sfxFolders = 0;
		for (const auto& [name, folder] : settings->sfxTable) {
			// values starting with "Sound\" are full Data-relative paths; others are
			// relative to sfx_root
			const bool absolute = folder.size() >= 6 && _strnicmp(folder.c_str(), "sound\\", 6) == 0;
			std::filesystem::path dir = absolute ? dataRoot / folder :
			                                       dataRoot / settings->sfxRoot / folder;
			const auto key = "sfx/" + name;
			if (ScanDir(key, dir, dataRoot)) {
				++sfxFolders;
			} else {
				logger::warn("SFX '{}': no audio files in {}", name, dir.string());
			}
		}

		logger::info("FolderCache: {} voice category folders, {} sfx folders", voiceFolders, sfxFolders);
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

		const auto& aliases = a_slot.sex == 'F' ? a_settings.femaleAliases : a_settings.maleAliases;
		const auto& fallbacks = a_slot.sex == 'F' ? a_settings.femaleFallbacks : a_settings.maleFallbacks;

		const auto tryCandidates = [&](std::string_view a_cat) -> std::string {
			std::vector<std::string> candidates;
			candidates.emplace_back(a_cat);
			if (const auto it = aliases.find(std::string(a_cat)); it != aliases.end()) {
				candidates.push_back(it->second);
			}
			if (a_slot.sex == 'M') {
				if (const auto it = a_settings.maleOnlyRemap.find(std::string(a_cat));
					it != a_settings.maleOnlyRemap.end()) {
					candidates.push_back(it->second);
				}
			}
			for (const auto& candidate : candidates) {
				const auto key = slotNorm + "/" + candidate;
				if (HasKey(key)) {
					return key;
				}
			}
			return {};
		};

		std::string resolved = tryCandidates(catNorm);
		if (resolved.empty()) {
			if (const auto it = fallbacks.find(catNorm); it != fallbacks.end()) {
				resolved = tryCandidates(it->second);
			}
		}

		g_resolveCache[cacheKey] = resolved;
		if (resolved.empty() && !g_missLogged[cacheKey]) {
			g_missLogged[cacheKey] = true;
			logger::warn("No audio for slot {} category '{}' (no folder, alias, or fallback)",
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
			// avoid immediate repeat across refills
			if (folder.deck.back() == folder.lastPlayed) {
				std::swap(folder.deck.back(), folder.deck.front());
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
