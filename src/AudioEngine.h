#pragma once

namespace AudioEngine
{
	// build+play a loose audio file. Returns an invalid handle on failure (logged).
	// a_follow may be null (2D / UI-style playback).
	// a_translateFuz: swap a .fuz path for its FuzCache-extracted payload (the
	// normal behavior). DebugPlayFile passes false so a raw .fuz can be fed
	// straight to the engine for diagnostics.
	// a_slotOut: when non-null AND the play is a fuz-derived cache wav, the audio is
	// routed through a FuzSlots placeholder so a first-session decode is audible
	// without a restart; *a_slotOut receives the slot index (>=0) the caller MUST
	// release (via InstanceManager) when the instance stops, or -1 if unrouted.
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority,
		bool a_translateFuz = true, int* a_slotOut = nullptr);

	// overload using config defaults for flags/priority
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow, float a_volume,
		int* a_slotOut = nullptr);

	// The engine output model PlayPath gives a sound that follows a_follow:
	// the game's own "Dialogue Output Model 3D" for an NPC with loaded 3D while
	// [general] dialogue_output_model is on, else null (the sound is left as
	// built). The player's own lines are never given one.
	const RE::BGSSoundOutput* OutputModelFor(RE::Actor* a_follow);

	// whether that output model rolls the sound off with distance itself, so
	// AudioUtil's own voice_attenuation must stay out of it
	bool EngineRollsOff(RE::Actor* a_follow);

	// the game's Voice volume as real dialogue gets it: the "Dialogue Voice
	// Category" default object and its parents, short of the master category.
	// 1.0 while [general] voice_slider is off.
	float VoiceVolumeFactor();

	// runtime A/B overrides of the two switches above (autest outmodel /
	// voiceslider): -1 = follow the config, 0 = off, 1 = on
	void SetOutputModelMode(int a_mode);
	bool OutputModelEnabled();
	void SetVoiceSliderMode(int a_mode);
	bool VoiceSliderEnabled();

	// what the engine holds for a playing sound: output model, category,
	// attenuation. Debug only (autest sndinfo): it reads the audio thread's
	// sound table from the game thread.
	std::string DescribeEngineSound(const RE::BSSoundHandle& a_handle);

	// the default objects VoiceVolumeFactor and OutputModelFor read, as numbers
	std::string DescribeDialogueDefaults();

	// does a data-relative path resolve to a real resource in the current load
	// order? Goes through the engine's archive system, so it resolves loose files
	// AND BSA-packed content (the same resolver PlayPath uses). Confirms the path
	// resolves, not that the audio is valid PCM.
	bool ResourceExists(const std::string& a_dataRelPath);
}
