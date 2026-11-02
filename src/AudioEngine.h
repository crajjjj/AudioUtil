#pragma once

namespace AudioEngine
{
	// what PlayPath did to a sound beyond building it, for InstanceManager::Register
	struct Played
	{
		// the output model set on the sound: the game's "Dialogue Output Model 3D"
		// for a followed NPC while [general] dialogue_output_model is on, else null
		// (left as built). The player's own lines never get one.
		const RE::BGSSoundOutput* outputModel = nullptr;
	};

	// build+play an audio file by data-relative path (wav, xwm or fuz; loose or
	// BSA-packed). Returns an invalid handle on failure (logged). a_follow may be
	// null (2D / UI-style playback). A .fuz is built as-is: the engine's own
	// sound loader skips the FUZE header and plays the xWMA inside (see Fuz).
	// a_played: when non-null, receives what was done to the sound.
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority,
		Played* a_played = nullptr);

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

	// a whole data-relative resource through the engine's loader (loose or
	// BSA-packed; forward slashes normalized). Empty on failure. Uncached:
	// callers keep what they parse.
	std::vector<std::uint8_t> ReadResourceBytes(const std::string& a_dataRelPath);
}
