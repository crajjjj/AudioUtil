#pragma once

#include <string>
#include <string_view>

// Placeholder-slot pool that makes a first-session fuz decode audible WITHOUT a
// restart. Skyrim only loads loose files present at launch, so a cache wav written
// mid-session can't be read back by the audio loader until the next start (worse
// under MO2's USVFS). But the engine RE-READS a file's bytes on each
// BuildSoundDataFromFile (verified by the `boverwrite` experiment), so we ship a
// small pool of placeholder wavs that DO exist at launch and, at play time, copy
// the decoded PCM into a free slot and build from the slot's (already-indexed)
// path. The engine reads our fresh bytes and plays them with full 3D / volume /
// pause. The slot is held until the instance stops, then returned to the pool.
namespace FuzSlots
{
	// Prepare a_count placeholder wavs (Sound\AudioUtilFuzCache\_au_slotNN.wav),
	// creating any missing, and enable the pool. a_count <= 0 disables it. Call at
	// kDataLoaded. (Shipping the slot wavs avoids a one-launch bootstrap gap; this
	// self-heals if the cache folder was deleted, effective from the next launch.)
	void Configure(int a_count);

	bool Enabled();

	// true if a filename or data-relative path names one of our slot placeholders.
	// The single owner of the _au_slot prefix — callers (FuzCache::EnforceCacheCap
	// eviction, the BCacheFile test probe) use this rather than matching the name.
	bool IsSlotName(std::string_view a_nameOrPath);

	// Acquire a free slot and copy a_srcCacheDataRelPath's bytes into it. On success
	// returns the slot index (>= 0) and sets a_outSlotDataRelPath to the slot's
	// data-relative path (present in the launch resource index, so it plays first
	// try). Returns -1 if the pool is disabled/full or the copy fails (caller then
	// plays the source path directly — correct when it is already indexed).
	int AcquireWithCopy(const std::string& a_srcCacheDataRelPath, std::string& a_outSlotDataRelPath);

	// Return a slot to the pool. Safe for -1 / out-of-range.
	void Release(int a_index);
}
