#pragma once

namespace AudioEngine
{
	// build+play a loose audio file. Returns an invalid handle on failure (logged).
	// a_follow may be null (2D / UI-style playback).
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority);

	// overload using config defaults for flags/priority
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow, float a_volume);
}
