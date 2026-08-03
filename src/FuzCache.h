#pragma once

namespace FuzCache
{
	// Resolve a .fuz voice container (loose OR BSA-packed - read through the
	// engine's resource loader) to a playable audio path: the xWMA/wav payload
	// is extracted once into Data\Sound\AudioUtilFuzCache\ and that
	// data-relative path is returned ("" if the fuz can't be read or parsed).
	// Results (including failures) are cached per path for the session; the
	// extracted files persist on disk across sessions and are reused, so a
	// given fuz is unpacked exactly once per install. The cache folder can be
	// deleted freely - it is rebuilt on demand.
	std::string Resolve(const std::string& a_fuzDataRelPath);

	// true if the path names a .fuz file (case-insensitive extension check)
	bool IsFuzPath(std::string_view a_path);

	// Enforce [general] fuz_cache_max_mb on the cache folder: while the total
	// size exceeds the cap, the oldest files (by write time) are deleted. Also
	// drops the in-session resolve map so evicted paths re-extract on demand.
	// No-op when the cap is 0 (unlimited) or the folder doesn't exist.
	// Called on kDataLoaded and ReloadConfig.
	void EnforceCacheCap();
}
