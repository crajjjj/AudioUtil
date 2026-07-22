#pragma once

namespace AudioEngine
{
	// build+play a loose audio file. Returns an invalid handle on failure (logged).
	// a_follow may be null (2D / UI-style playback).
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority);

	// overload using config defaults for flags/priority
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow, float a_volume);

	// does a data-relative path resolve to a real resource in the current load
	// order? Goes through the engine's archive system, so it resolves loose files
	// AND BSA-packed content (the same resolver PlayPath uses). Confirms the path
	// resolves, not that the audio is valid PCM.
	bool ResourceExists(const std::string& a_dataRelPath);
}
