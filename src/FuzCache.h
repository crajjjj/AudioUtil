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

	// Read a whole data-relative resource through the engine's loader (loose or
	// BSA-packed). Empty on failure. Uncached - callers cache what they parse.
	std::vector<std::uint8_t> ReadResourceBytes(const std::string& a_dataRelPath);

	// The embedded LIP block of a .fuz (the bytes Resolve() skips). Empty if
	// the fuz is unreadable, malformed, or carries no lip.
	std::vector<std::uint8_t> ReadLipBlock(const std::string& a_fuzDataRelPath);

	// Enforce [general] fuz_cache_max_mb on the cache folder: when the total
	// size exceeds the cap, the oldest files (by write time) are deleted until
	// it is a tenth below the cap, which leaves the prewarm room to work. Also
	// drops the in-session resolve map so evicted paths re-extract on demand.
	// No-op when the cap is 0 (unlimited) or the folder doesn't exist.
	// Called on kDataLoaded and ReloadConfig.
	void EnforceCacheCap();

	// True once the cache folder holds fuz_cache_max_mb (never with a cap of 0):
	// the size EnforceCacheCap last measured plus every file written since. The
	// background prewarm stops here, because decoding past the cap only feeds the
	// next launch's eviction, which the prewarm then refills.
	bool CacheFull();
}
