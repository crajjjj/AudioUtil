#pragma once

#include "Config.h"

namespace FolderCache
{
	// scan every configured slot root + sfx folder. Call at kDataLoaded and after ReloadConfig.
	void Rebuild();

	// resolve slot+category through aliases (and male_only_remap / fallbacks) to a scanned folder.
	// returns empty string on miss (logged once per key).
	std::string ResolveVoiceKey(const Config::Settings& a_settings,
		const Config::Slot& a_slot, std::string_view a_category);

	// register+scan an arbitrary data-relative folder on first use (for PlayFolder / sfx table)
	std::string ResolveDirKey(std::string_view a_dataRelativeFolder);

	// shuffle-bag pick; returns a data-relative wav path or empty
	std::string PickNext(const std::string& a_folderKey);

	int FileCount(const std::string& a_folderKey);
}
