#pragma once

namespace AudioEngine
{
	// what PlayPath did to a sound beyond building it, for InstanceManager::Register
	struct Played
	{
		// FuzSlots placeholder backing the audio (-1 = none); the caller MUST
		// release it (via InstanceManager) when the instance stops
		int fuzSlot = -1;
		// the output model set on the sound: the game's "Dialogue Output Model 3D"
		// for a followed NPC while [general] dialogue_output_model is on, else null
		// (left as built). The player's own lines never get one.
		const RE::BGSSoundOutput* outputModel = nullptr;
	};

	// build+play a loose audio file. Returns an invalid handle on failure (logged).
	// a_follow may be null (2D / UI-style playback).
	// a_translateFuz: swap a .fuz path for its FuzCache-extracted payload (the
	// normal behavior). DebugPlayFile passes false so a raw .fuz can be fed
	// straight to the engine for diagnostics.
	// a_played: when non-null, receives what was done to the sound; AND when the
	// play is a fuz-derived cache wav, the audio is routed through a FuzSlots
	// placeholder so a first-session decode is audible without a restart
	// (a_played->fuzSlot >= 0).
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority,
		bool a_translateFuz = true, Played* a_played = nullptr);

	// overload using config defaults for flags/priority
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow, float a_volume,
		Played* a_played = nullptr);

	// The game's Voice volume as real dialogue gets it: GetCategoryVolume() of
	// the "Dialogue Voice Category" default object, which already multiplies in
	// every parent's slider and static multiplier. The Master slider is not a
	// category (BSAudioManager::masterVolume, applied to every sound). 1.0 while
	// [general] voice_slider is off.
	float VoiceVolumeFactor();

	// runtime A/B overrides of [general] dialogue_output_model / voice_slider
	// (autest outmodel / voiceslider): -1 = follow the config, 0 = off, 1 = on.
	// ApplyConfig (load, ReloadConfig) drops them back to the config.
	void        SetOutputModelMode(int a_mode);
	bool        OutputModelEnabled();
	void        SetVoiceSliderMode(int a_mode);
	bool        VoiceSliderEnabled();
	std::string OutputModelStatus();  // "on (config)", "off (autest override)", ...
	std::string VoiceSliderStatus();
	void        ApplyConfig();

	// the default objects VoiceVolumeFactor and the output model come from, as numbers
	std::string DescribeDialogueDefaults();

	// does a data-relative path resolve to a real resource in the current load
	// order? Goes through the engine's archive system, so it resolves loose files
	// AND BSA-packed content (the same resolver PlayPath uses). Confirms the path
	// resolves, not that the audio is valid PCM.
	bool ResourceExists(const std::string& a_dataRelPath);
}
