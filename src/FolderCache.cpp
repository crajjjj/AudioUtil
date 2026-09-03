#include "FolderCache.h"

#include "AudioEngine.h"
#include "Config.h"
#include "Tags.h"

#include <toml++/toml.hpp>

#include <bit>
#include <format>
#include <fstream>
#include <unordered_set>

namespace FolderCache
{
	void AuditExplicitFiles(const Config::Settings& a_settings);  // defined after Rebuild
	void AuditWiring(const Config::Settings& a_settings);         // defined after Rebuild
	void LogRoster(const Config::Settings& a_settings);          // defined after Rebuild

	namespace
	{
		// one shuffle-bag of files sharing an effective tag set. pools[0] of every
		// Folder is the UNTAGGED pool (tags == 0) — the always-valid floor a
		// legacy (facts = 0) call draws from; tagged pools only qualify when the
		// request's facts cover their constraints (see Tags::Qualifies).
		struct Pool
		{
			Tags::Mask               tags{ 0 };
			std::vector<std::string> files;  // data-relative paths
			std::vector<std::size_t> deck;   // shuffled indices, consumed from the back
			std::size_t              lastPlayed = SIZE_MAX;
		};

		struct Folder
		{
			std::vector<Pool> pools;  // invariant: pools[0] exists and has tags == 0

			Folder() { pools.emplace_back(); }

			Pool& PoolFor(Tags::Mask a_tags)
			{
				for (auto& pool : pools) {
					if (pool.tags == a_tags) {
						return pool;
					}
				}
				auto& pool = pools.emplace_back();
				pool.tags = a_tags;
				return pool;
			}

			std::size_t TotalFiles() const
			{
				std::size_t n = 0;
				for (const auto& pool : pools) {
					n += pool.files.size();
				}
				return n;
			}

			bool HasQualifying(Tags::Mask a_facts) const
			{
				for (const auto& pool : pools) {
					if (!pool.files.empty() && Tags::Qualifies(pool.tags, a_facts)) {
						return true;
					}
				}
				return false;
			}
		};

		std::unordered_map<std::string, Folder> g_folders;  // key -> folder
		std::unordered_map<std::string, std::string> g_resolveCache;
		std::unordered_map<std::string, bool>        g_missLogged;
		std::mutex   g_lock;
		std::mt19937 g_rng{ std::random_device{}() };

		// path -> narrow string via the ANSI codepage. path::string() THROWS
		// std::system_error on a character the codepage cannot express (one
		// exotic TTS filename in a 42k-file voicepack CTD'd the whole scan) --
		// and the engine's narrow-path resource loader could never open such a
		// file anyway, so the right treatment is skip-and-count, never throw.
		bool NarrowPathImpl(const std::filesystem::path& a_path, std::string& a_out)
		{
			try {
				a_out = a_path.string();
				return true;
			} catch (const std::exception&) {
				return false;
			}
		}

		bool IsAudioFile(const std::filesystem::path& a_path)
		{
			// .fuz voice containers count too: AudioEngine::PlayPath transparently
			// plays their extracted xWMA/wav payload (see FuzCache)
			std::string ext;
			if (!NarrowPathImpl(a_path.extension(), ext)) {
				return false;
			}
			return _stricmp(ext.c_str(), ".wav") == 0 || _stricmp(ext.c_str(), ".xwm") == 0 ||
			       _stricmp(ext.c_str(), ".fuz") == 0;
		}

		std::string DataRelative(const std::filesystem::path& a_abs, const std::filesystem::path& a_dataRoot)
		{
			// lexical only: std::filesystem::relative() canonicalizes through the
			// filesystem, and under MO2's USVFS that resolves Data\Sound\... to the
			// real mod folder (E:\...\mods\<mod>\Sound\...), yielding "..\..\mods\..."
			// which the game's resource loader cannot open. Entry paths are composed
			// lexically from a_dataRoot, so a lexical relative is exact.
			std::string rel;
			if (!NarrowPathImpl(a_abs.lexically_relative(a_dataRoot), rel)) {
				return {};  // unmappable name: caller skips (engine couldn't open it)
			}
			std::replace(rel.begin(), rel.end(), '/', '\\');
			return rel;
		}

		// finalize a scanned Folder into g_folders: sort each pool's files, order
		// tagged pools deterministically (by mask) so equal-score ties resolve the
		// same on every machine, drop tagless empties. Caller holds g_lock.
		bool CommitFolder(const std::string& a_key, Folder&& a_folder, std::size_t a_unmappable)
		{
			if (a_unmappable > 0) {
				logger::warn("Scan '{}': skipped {} file(s) whose names the system "
				             "codepage cannot express (unplayable by the engine)",
					a_key, a_unmappable);
			}
			if (a_folder.TotalFiles() == 0) {
				return false;
			}
			for (auto& pool : a_folder.pools) {
				std::sort(pool.files.begin(), pool.files.end());
			}
			std::sort(a_folder.pools.begin() + 1, a_folder.pools.end(),
				[](const Pool& a, const Pool& b) { return a.tags < b.tags; });
			g_folders[a_key] = std::move(a_folder);
			return true;
		}

		// scan one directory (non-recursive, no tag semantics) into g_folders
		// under a_key — for [sfx] folders and PlayFolder dir keys. Caller holds g_lock.
		bool ScanDir(const std::string& a_key, const std::filesystem::path& a_dir,
			const std::filesystem::path& a_dataRoot)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(a_dir, ec)) {
				return false;
			}
			Folder folder;
			std::size_t unmappable = 0;
			for (const auto& entry : std::filesystem::directory_iterator(a_dir, ec)) {
				if (entry.is_regular_file(ec) && IsAudioFile(entry.path())) {
					auto rel = DataRelative(entry.path(), a_dataRoot);
					if (rel.empty()) {
						++unmappable;
						continue;
					}
					folder.pools[0].files.push_back(std::move(rel));
				}
			}
			return CommitFolder(a_key, std::move(folder), unmappable);
		}

		// ---------- tag carriers (tag subfolders / [bracketed] filenames / _tags.toml) ----------

		// "line_07 [victim intense].wav" -> "victim intense"; empty when the stem
		// carries no trailing [...] group
		std::string BracketTags(const std::string& a_filename)
		{
			const auto dot = a_filename.rfind('.');
			auto stem = a_filename.substr(0, dot == std::string::npos ? a_filename.size() : dot);
			while (!stem.empty() && stem.back() == ' ') {
				stem.pop_back();
			}
			if (stem.size() < 2 || stem.back() != ']') {
				return {};
			}
			const auto open = stem.rfind('[');
			if (open == std::string::npos) {
				return {};
			}
			return stem.substr(open + 1, stem.size() - open - 2);
		}

		// optional per-folder _tags.toml manifest: filename -> token string
		// (tags an existing pack without renaming files + their sidecars)
		using ManifestMap = std::unordered_map<std::string, std::string>;  // normalized filename -> tags
		ManifestMap LoadManifest(const std::filesystem::path& a_dir)
		{
			ManifestMap out;
			std::error_code ec;
			const auto file = a_dir / "_tags.toml";
			if (!std::filesystem::is_regular_file(file, ec)) {
				return out;
			}
			try {
				std::ifstream stream(file);  // stream, not parse_file: path may exceed the ANSI codepage
				if (!stream) {
					logger::warn("_tags.toml unreadable in {}", a_dir.string());
					return out;
				}
				const auto root = toml::parse(stream);
				for (auto&& [name, value] : root) {
					if (const auto text = value.value<std::string>()) {
						out[Config::Normalize(name.str())] = *text;
					} else {
						logger::warn("_tags.toml in {}: '{}' is not a string — ignored",
							a_dir.string(), name.str());
					}
				}
			} catch (const std::exception& e) {
				logger::warn("_tags.toml parse error in {}: {}", a_dir.string(), e.what());
			}
			return out;
		}

		// scan a voice CATEGORY directory with tag semantics: loose
		// files land in the pool for their effective tag set (subfolder tags ∪
		// filename [brackets] ∪ _tags.toml manifest); one level of tag subfolders
		// is honored, deeper nesting and invalid tag sets are warned + ignored.
		// A file whose tags don't parse (typo) or self-contradict (two tokens of
		// one axis) is EXCLUDED — a bad tag must degrade to silence-with-a-log,
		// never to playing in the wrong context. Caller holds g_lock.
		bool ScanCategoryDir(const std::string& a_key, const std::filesystem::path& a_dir,
			const std::filesystem::path& a_dataRoot)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(a_dir, ec)) {
				return false;
			}
			Folder folder;
			std::size_t unmappable = 0;
			// no [tags] vocabulary loaded = the tag layer is dormant: ignore
			// subfolders and tag carriers entirely (exactly the pre-tags scan),
			// so stray subdirs / bracketed names in a pack cause zero warnings
			const bool tagged = Tags::IsConfigured();

			const auto addFile = [&](const std::filesystem::directory_entry& a_entry,
									 Tags::Mask a_folderTags, const ManifestMap& a_manifest) {
				std::error_code fec;
				if (!a_entry.is_regular_file(fec) || !IsAudioFile(a_entry.path())) {
					return;
				}
				auto rel = DataRelative(a_entry.path(), a_dataRoot);
				std::string name;
				if (rel.empty() || !NarrowPathImpl(a_entry.path().filename(), name)) {
					++unmappable;
					return;
				}
				Tags::Mask  tags = a_folderTags;
				std::string err;
				if (!tagged) {
					folder.pools[0].files.push_back(std::move(rel));
					return;
				}
				// a trailing [group] is a tag carrier only when it actually names
				// vocabulary. One mod's [tags] block enables the carrier for every
				// pack on the install, and packs bracket filenames for their own
				// reasons ("moan_04 [loud].wav") — treating those as a broken tag
				// set would silently mute unrelated content. A group with SOME
				// known tokens is a real (mistyped) tag set and still excludes.
				if (const auto bracket = BracketTags(name);
					!bracket.empty() && Tags::ContainsKnownToken(bracket)) {
					Tags::Mask m = 0;
					if (!Tags::ParseConstraints(bracket, m, err)) {
						logger::warn("Scan '{}': '{}' excluded — {}", a_key, name, err);
						return;
					}
					tags |= m;
				}
				if (const auto it = a_manifest.find(Config::Normalize(name)); it != a_manifest.end()) {
					Tags::Mask m = 0;
					if (!Tags::ParseConstraints(it->second, m, err)) {
						logger::warn("Scan '{}': '{}' excluded — _tags.toml entry: {}", a_key, name, err);
						return;
					}
					tags |= m;
				}
				if (Tags::HasAxisConflict(tags)) {
					logger::warn("Scan '{}': '{}' excluded — contradictory axis tokens across its "
					             "folder/filename/manifest tags ({})",
						a_key, name, Tags::Describe(tags));
					return;
				}
				folder.PoolFor(tags).files.push_back(std::move(rel));
			};

			const auto topManifest = tagged ? LoadManifest(a_dir) : ManifestMap{};
			for (const auto& entry : std::filesystem::directory_iterator(a_dir, ec)) {
				if (entry.is_directory(ec)) {
					if (!tagged) {
						continue;
					}
					std::string sub;
					if (!NarrowPathImpl(entry.path().filename(), sub)) {
						logger::warn("Scan '{}': skipped a subfolder whose name the system "
						             "codepage cannot express", a_key);
						continue;
					}
					Tags::Mask  folderTags = 0;
					std::string err;
					if (!Tags::ParseConstraints(sub, folderTags, err) || folderTags == 0) {
						// same rule as the [bracket] carrier: a name holding no
						// vocabulary at all isn't a mistyped tag folder, it's an
						// unrelated pack's subdirectory — ignore it as the pre-tags
						// scan did, without a warning nobody can act on
						if (Tags::ContainsKnownToken(sub)) {
							logger::warn("Scan '{}': subfolder '{}' is not a valid tag set{}{} — ignored "
							             "(tag folders are named from the [tags] vocabulary, e.g. 'victim intense')",
								a_key, sub, err.empty() ? "" : ": ", err);
						}
						continue;
					}
					const auto  subManifest = LoadManifest(entry.path());
					bool        nestedWarned = false;
					std::error_code sec;
					for (const auto& subEntry : std::filesystem::directory_iterator(entry.path(), sec)) {
						if (subEntry.is_directory(sec)) {
							if (!nestedWarned) {
								nestedWarned = true;
								logger::warn("Scan '{}': nested folder(s) under tag folder '{}' ignored "
								             "(tag folders are one level deep, not a tree)", a_key, sub);
							}
							continue;
						}
						addFile(subEntry, folderTags, subManifest);
					}
				} else {
					addFile(entry, 0, topManifest);
				}
			}
			return CommitFolder(a_key, std::move(folder), unmappable);
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

		// caller holds g_lock: the key exists AND holds >=1 nonempty pool the
		// facts cover — the existence test the tag-aware resolver routes on
		bool KeyQualifies(const std::string& a_key, Tags::Mask a_facts)
		{
			const auto it = g_folders.find(a_key);
			return it != g_folders.end() && it->second.HasQualifying(a_facts);
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
				// explicit lists are untagged (pool 0): they're the BSA-capable hand-
				// written form, and a BSA path has no scannable tag carriers anyway
				Folder folder;
				folder.pools[0].files.reserve(files.size());
				for (auto file : files) {
					std::replace(file.begin(), file.end(), '/', '\\');
					folder.pools[0].files.push_back(std::move(file));
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
				if (ScanCategoryDir(key, dir, dataRoot)) {
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
				std::string catName;
				if (!NarrowPathImpl(entry.path().filename(), catName)) {
					logger::warn("Slot {}: skipped a category folder whose name the "
					             "system codepage cannot express", slot.id);
					continue;
				}
				const auto category = Config::Normalize(catName);
				const auto key = Config::Normalize(slot.id) + "/" + category;
				if (HasKey(key)) {
					logger::warn("Slot {}: duplicate normalized category '{}' — folder {} ignored",
						slot.id, category, catName);
					continue;
				}
				if (ScanCategoryDir(key, entry.path(), dataRoot)) {
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
		LogRoster(*settings);
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
	// directly via PlayVoiceFromSlot, so this is a warning, not an error — a slot
	// deliberately driven that way sets script_only=true to opt out entirely.) Empty
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
		add(a_settings.defaultCreatureSlot);
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
			// script_only slots are reached directly via PlayVoiceFromSlot by design,
			// so "no route points to them" is expected, not a mistake — skip them.
			if (slot.scriptOnly) {
				continue;
			}
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

	// One consolidated dump of the fully MERGED configuration: every slot and
	// every resolution table as they ended up AFTER the base config and all
	// overlays were unioned (additively, last-writer-wins per key). This is the
	// one thing the rest of the log can't show — a slot, route, alias, or sfx
	// entry can be contributed by any of several files, and only the combined
	// result decides what actually resolves, so the effective state is otherwise
	// impossible to read back without replaying every file by hand. Emitted once
	// per load (and on `au reload`), after the audits. Reads g_folders for the
	// resolved per-category file counts, so the caller must hold g_lock.
	void LogRoster(const Config::Settings& a_settings)
	{
		const auto joinSlots = [](const Config::SlotList& a_ids) {
			std::string out;
			for (const auto& id : a_ids) {
				if (!out.empty()) {
					out += ',';
				}
				out += id;
			}
			return out;
		};

		// sorted key/value dump for the additive string maps (route + category layers)
		const auto dumpMap = [](std::string_view a_label, const Config::StringMap& a_map) {
			if (a_map.empty()) {
				return;
			}
			logger::info("{} ({}):", a_label, a_map.size());
			std::vector<std::string> keys;
			keys.reserve(a_map.size());
			for (const auto& [k, v] : a_map) {
				keys.push_back(k);
			}
			std::sort(keys.begin(), keys.end());
			for (const auto& k : keys) {
				logger::info("  {} -> {}", k, a_map.at(k));
			}
		};

		logger::info("================= AudioUtil registration roster (merged config) =================");

		// ---- slots: a summary line per slot, then its resolved categories wrapped
		// a few per indented line (a slot can have 60+ categories - one per line
		// would bury the maps below, one giant line is unreadable). categories come
		// from g_folders (explicit lists + scanned folders merged), so the counts
		// are what will actually PLAY, not just what the TOML declared.
		std::size_t maxIdLen = 0;
		for (const auto& slot : a_settings.slots) {
			maxIdLen = std::max(maxIdLen, slot.id.size());
		}
		logger::info("Slots ({}):", a_settings.slots.size());
		for (const auto& slot : a_settings.slots) {
			const auto prefix = Config::Normalize(slot.id) + "/";
			std::vector<std::string> cats;      // "moan(12)"
			std::vector<std::string> tagLines;  // per-category tagged-pool detail
			std::size_t totalFiles = 0;
			for (const auto& [key, folder] : g_folders) {
				if (key.starts_with(prefix)) {
					cats.push_back(key.substr(prefix.size()) + "(" +
						std::to_string(folder.TotalFiles()) + ")");
					totalFiles += folder.TotalFiles();
					if (folder.pools.size() > 1) {
						// tagged pools: show each tag set + its file count so an
						// author can verify what the scan understood of their tagging
						std::string line = "      tags " + key.substr(prefix.size()) + ":";
						for (const auto& pool : folder.pools) {
							if (!pool.files.empty() && pool.tags != 0) {
								line += " [" + Tags::Describe(pool.tags) + "](" +
									std::to_string(pool.files.size()) + ")";
							}
						}
						tagLines.push_back(std::move(line));
					}
				}
			}
			std::sort(cats.begin(), cats.end());
			std::sort(tagLines.begin(), tagLines.end());

			// flags: explicit-only marker (no scan root), then variation / fallback /
			// gag redirect - only the ones actually set, space-joined
			std::string attrs;
			const auto addAttr = [&attrs](const std::string& a_s) {
				if (!attrs.empty()) {
					attrs += ' ';
				}
				attrs += a_s;
			};
			if (slot.root.empty()) {
				addAttr("explicit");
			}
			if (slot.scriptOnly) {
				addAttr("script-only");
			}
			if (!slot.variation.empty() && slot.variation != "A") {
				addAttr("var=" + slot.variation);
			}
			if (!slot.fallbackSlot.empty()) {
				addAttr("fb=" + slot.fallbackSlot);
			}
			if (!slot.gagSlot.empty()) {
				addAttr("gag=" + slot.gagSlot);
			}

			std::string idPad = slot.id;
			idPad.resize(maxIdLen, ' ');  // align the [sex] column across all slots
			std::string line = "  " + idPad + " [" + std::string(1, slot.sex) + "]";
			if (!attrs.empty()) {
				line += " " + attrs;
			}
			if (cats.empty()) {
				line += "  EMPTY (no resolved audio)";
			} else {
				line += "  " + std::to_string(cats.size()) + (cats.size() == 1 ? " cat  " : " cats  ") +
					std::to_string(totalFiles) + " files";
			}
			logger::info("{}", line);

			// categories wrapped 6 per line, indented under the slot
			const std::size_t perLine = 6;
			for (std::size_t i = 0; i < cats.size(); i += perLine) {
				std::string wrap = "      ";
				for (std::size_t j = i; j < cats.size() && j < i + perLine; ++j) {
					if (j > i) {
						wrap += ' ';
					}
					wrap += cats[j];
				}
				logger::info("{}", wrap);
			}
			for (const auto& tagLine : tagLines) {
				logger::info("{}", tagLine);
			}
		}

		// ---- blind default / PC-reservation / sfx slot assignments ----
		logger::info("defaults: female={} male={} creature={}  pc.female={} pc.male={}  sfx={}",
			a_settings.defaultFemaleSlot, a_settings.defaultMaleSlot,
			a_settings.defaultCreatureSlot.empty() ? "-" : a_settings.defaultCreatureSlot,
			a_settings.pcFemaleSlot.empty() ? "-" : a_settings.pcFemaleSlot,
			a_settings.pcMaleSlot.empty() ? "-" : a_settings.pcMaleSlot,
			a_settings.sfxSlot.empty() ? "-" : a_settings.sfxSlot);

		// ---- actor -> slot routing tables (all additive) ----
		logger::info("voicetype_map ({}):", a_settings.voicetypeMap.size());
		{
			std::vector<std::string> keys;
			keys.reserve(a_settings.voicetypeMap.size());
			for (const auto& [vt, ids] : a_settings.voicetypeMap) {
				keys.push_back(vt);
			}
			std::sort(keys.begin(), keys.end());
			for (const auto& vt : keys) {
				logger::info("  {} -> [{}]", vt, joinSlots(a_settings.voicetypeMap.at(vt)));
			}
		}

		if (!a_settings.voicetypeRemapEnabled) {
			logger::info("voicetype_remap is DISABLED ([voicetype_remap] enable=false)");
		}
		dumpMap("voicetype_remap", a_settings.voicetypeRemap);
		dumpMap("npc_overrides", a_settings.npcOverrides);

		logger::info("race_map ({}, longest-hint-first):", a_settings.raceMap.size());
		for (const auto& [hint, ids] : a_settings.raceMap) {
			logger::info("  {} -> [{}]", hint, joinSlots(ids));
		}

		// ---- category layer: aliases / fallbacks / male-only remap ----
		dumpMap("aliases[F]", a_settings.femaleAliases);
		dumpMap("aliases[M/A]", a_settings.maleAliases);
		dumpMap("male_only_remap", a_settings.maleOnlyRemap);
		dumpMap("fallbacks[F]", a_settings.femaleFallbacks);
		dumpMap("fallbacks[M/A]", a_settings.maleFallbacks);

		// ---- sfx table + group volumes + gag markers ----
		dumpMap("sfx table", a_settings.sfxTable);

		if (!a_settings.groupVolumes.empty()) {
			std::string line = "group volumes:";
			std::vector<std::string> keys;
			keys.reserve(a_settings.groupVolumes.size());
			for (const auto& [g, v] : a_settings.groupVolumes) {
				keys.push_back(g);
			}
			std::sort(keys.begin(), keys.end());
			for (const auto& g : keys) {
				line += " " + g + "=" + std::format("{:.2f}", a_settings.groupVolumes.at(g));
			}
			logger::info("{}", line);
		}

		if (!a_settings.lipsyncBlockCategories.empty()) {
			std::vector<std::string> cats(a_settings.lipsyncBlockCategories.begin(),
				a_settings.lipsyncBlockCategories.end());
			std::sort(cats.begin(), cats.end());
			std::string line = "lipsync block_categories:";
			for (const auto& c : cats) {
				line += " " + c;
			}
			logger::info("{}", line);
		}

		// gag keyword/item counts are the CONFIGURED refs; GagState logs how many
		// actually resolved to live forms in the current load order.
		logger::info("gag: {}  default_category={}  keywords={}  items={}",
			a_settings.gagEnabled ? "enabled" : "disabled",
			a_settings.gagDefaultCategory.empty() ? "-" : a_settings.gagDefaultCategory,
			a_settings.gagKeywords.size(), a_settings.gagItems.size());

		logger::info("================================================================================");
	}

	std::string ResolveVoiceKey(const Config::Settings& a_settings,
		const Config::Slot& a_slot, std::string_view a_category, Tags::Mask a_facts)
	{
		const auto slotNorm = Config::Normalize(a_slot.id);
		const auto catNorm = Config::Normalize(a_category);
		// facts are part of the resolution identity: a category whose only pools
		// are tagged "exists" for a request that covers them and falls through to
		// fallbacks for one that doesn't — so each fact set caches separately
		const auto cacheKey = slotNorm + "/" + catNorm +
			(a_facts ? "|" + std::format("{:x}", a_facts) : "");

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
					if (KeyQualifies(key, a_facts)) {
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
		// the miss log is keyed WITHOUT the facts: a category that's simply absent
		// misses for every fact set a consumer sends, and one warn per combination
		// is log spam over an unbounded key set. Once per slot+category, as before.
		const auto missKey = slotNorm + "/" + catNorm;
		if (resolved.empty() && !g_missLogged[missKey]) {
			g_missLogged[missKey] = true;
			// kAllFacts is introspection asking "is there ANY content here" — it's
			// not a fact set a caller sent, so don't echo the whole vocabulary back
			if (a_facts && a_facts != Tags::kAllFacts) {
				logger::warn("No audio for slot {} category '{}' facts [{}] (no qualifying pool, alias, fallback, or fallback slot)",
					a_slot.id, a_category, Tags::Describe(a_facts));
			} else {
				logger::warn("No audio for slot {} category '{}' (no folder, alias, fallback, or fallback slot)",
					a_slot.id, a_category);
			}
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

	std::vector<std::string> ListFolder(std::string_view a_dataRelativeFolder)
	{
		const auto key = ResolveDirKey(a_dataRelativeFolder);  // scans on first use
		if (key.empty()) {
			return {};
		}
		std::scoped_lock lock{ g_lock };
		const auto it = g_folders.find(key);
		if (it == g_folders.end()) {
			return {};
		}
		std::vector<std::string> out;
		for (const auto& pool : it->second.pools) {
			out.insert(out.end(), pool.files.begin(), pool.files.end());
		}
		return out;
	}

	std::vector<std::string> AllAudioFiles()
	{
		std::unordered_set<std::string> seen;
		std::scoped_lock lock{ g_lock };
		for (const auto& [key, folder] : g_folders) {
			for (const auto& pool : folder.pools) {
				for (const auto& f : pool.files) {
					seen.insert(f);
				}
			}
		}
		return { seen.begin(), seen.end() };
	}

	std::string PickNext(const std::string& a_folderKey, Tags::Mask a_facts)
	{
		std::scoped_lock lock{ g_lock };
		const auto it = g_folders.find(a_folderKey);
		if (it == g_folders.end()) {
			return {};
		}

		// pool selection: among nonempty pools whose constraints the facts cover,
		// the highest weight-sum wins; ties go to the more specific set (more
		// tokens), then the lowest mask (pools are mask-sorted at scan, so this is
		// deterministic per install). facts = 0 -> only the untagged pool
		// qualifies = the pre-tags behavior, one code path for both.
		// no score sentinel: the FIRST qualifying pool seeds the comparison, so a
		// vocabulary with a zero or negative axis weight still picks a pool rather
		// than reporting "playable" (HasQualifying) and then returning nothing.
		Pool* best = nullptr;
		int   bestScore = 0;
		int   bestBits = 0;
		for (auto& pool : it->second.pools) {
			if (pool.files.empty() || !Tags::Qualifies(pool.tags, a_facts)) {
				continue;
			}
			const int score = Tags::Score(pool.tags);
			const int bits = std::popcount(pool.tags);
			if (!best || score > bestScore || (score == bestScore && bits > bestBits)) {
				best = &pool;
				bestScore = score;
				bestBits = bits;
			}
		}
		if (!best) {
			return {};
		}
		auto& pool = *best;

		if (pool.files.size() == 1) {
			return pool.files[0];
		}

		if (pool.deck.empty()) {
			pool.deck.resize(pool.files.size());
			for (std::size_t i = 0; i < pool.deck.size(); ++i) {
				pool.deck[i] = i;
			}
			std::shuffle(pool.deck.begin(), pool.deck.end(), g_rng);
			// avoid an immediate repeat across refills. Compare the file PATH, not
			// the deck index, so a clip listed more than once (deliberate weighting)
			// still never plays back-to-back. Swap in the first entry that's a
			// genuinely different clip; if every entry is the same file there's
			// nothing else to play and a repeat is unavoidable.
			if (pool.lastPlayed != SIZE_MAX &&
				pool.files[pool.deck.back()] == pool.files[pool.lastPlayed]) {
				for (auto d = pool.deck.begin(); d + 1 != pool.deck.end(); ++d) {
					if (pool.files[*d] != pool.files[pool.lastPlayed]) {
						std::swap(*d, pool.deck.back());
						break;
					}
				}
			}
		}

		const auto index = pool.deck.back();
		pool.deck.pop_back();
		pool.lastPlayed = index;
		return pool.files[index];
	}

	int FileCount(const std::string& a_folderKey)
	{
		std::scoped_lock lock{ g_lock };
		const auto it = g_folders.find(a_folderKey);
		return it != g_folders.end() ? static_cast<int>(it->second.TotalFiles()) : 0;
	}

	bool HasPlayableFiles(const std::string& a_folderKey, Tags::Mask a_facts)
	{
		std::scoped_lock lock{ g_lock };
		return KeyQualifies(a_folderKey, a_facts);
	}
}
