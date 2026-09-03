#pragma once

#include "Config.h"
#include "Tags.h"

namespace FolderCache
{
	// scan every configured slot root + sfx folder. Call at kDataLoaded and after ReloadConfig.
	void Rebuild();

	// resolve slot+category through aliases (and male_only_remap / fallbacks) to a scanned
	// folder that has at least one pool QUALIFYING for a_facts (see Tags) — so a category
	// holding only tagged pools none of which qualify falls through to fallbacks, exactly
	// like a missing category. a_facts = 0 (legacy calls) qualifies untagged content only;
	// Tags::kAllFacts qualifies everything (introspection). Returns empty string on miss
	// (logged once per key+facts).
	std::string ResolveVoiceKey(const Config::Settings& a_settings,
		const Config::Slot& a_slot, std::string_view a_category, Tags::Mask a_facts = 0);

	// register+scan an arbitrary data-relative folder on first use (for PlayFolder / sfx
	// table). Flat scan — no tag subfolders.
	std::string ResolveDirKey(std::string_view a_dataRelativeFolder);

	// shuffle-bag pick from the best-scoring pool that qualifies for a_facts
	// (0 = the untagged pool, i.e. legacy behavior); data-relative path or empty
	std::string PickNext(const std::string& a_folderKey, Tags::Mask a_facts = 0);

	// total files across ALL pools of the key (tag-blind, for introspection)
	int FileCount(const std::string& a_folderKey);

	// the key exists AND holds >=1 nonempty pool a_facts covers — i.e. PickNext
	// with the same facts will return a file. Any existence test that gates a
	// PLAY must use this, not FileCount: a category whose pools are all tagged
	// counts files but has nothing a fact-poorer request may play.
	bool HasPlayableFiles(const std::string& a_folderKey, Tags::Mask a_facts = 0);

	// data-relative audio file paths in a folder (non-recursive), scanning it on
	// first use like ResolveDirKey. For cache prewarming. Empty on miss.
	std::vector<std::string> ListFolder(std::string_view a_dataRelativeFolder);

	// every audio file across all scanned folders (deduplicated). Call after
	// Rebuild. Used to prewarm the fuz cache for the whole config at load.
	std::vector<std::string> AllAudioFiles();
}
