#pragma once

namespace LipSync
{
	// Warm the caches Start reads for a .fuz (its lip block, or else its audio
	// decoded in memory for the envelope). Call it BEFORE the sound starts: the
	// mouth clock starts when Start's entry first sees the sound playing, so a
	// first-play decode inside Start would leave the mouth trailing the voice by
	// the decode time. No-op for other files (a loose wav parses fast) and while
	// lipsync is off.
	void Prepare(const std::string& a_dataRelPath);

	// begin driving the actor's mouth from the line's lip curves (a .lip beside
	// the file, or a fuz's embedded block) or its amplitude envelope, in sync
	// with the playing instance. No-op if disabled, the audio isn't readable
	// (xwm, BSA-packed wav), or the actor is invalid; failures only log.
	// a_dialogueGuard = false skips the in-dialogue-with-the-player block (at
	// start and on every handover re-check); gag, tongue and claims still apply.
	void Start(RE::Actor* a_actor, const std::string& a_dataRelPath,
		RE::BSSoundHandle a_handle, std::int32_t a_instanceId, bool a_dialogueGuard = true);

	// early-stop notification (StopHandle / channel replace / StopGroup / StopAll)
	void OnInstanceStopped(std::int32_t a_instanceId);

	void StopFor(RE::Actor* a_actor);
	bool IsActiveFor(RE::Actor* a_actor);

	// runtime master switch; ApplyConfig re-reads the toml value
	void SetEnabled(bool a_enabled);
	bool Enabled();
	void SetGain(float a_gain);

	// runtime toggle for .lip-driven phoneme mode ([lipsync] use_lip_files;
	// console: `autest lipfiles`). Off = amplitude envelope for everything.
	// Affects new lines only - already-playing entries keep their mode.
	void SetLipFilesEnabled(bool a_enabled);
	bool LipFilesEnabled();

	// runtime toggle for envelope->pseudo-phoneme synthesis on lines WITHOUT
	// lip data ([lipsync] pseudo_phonemes; console: `autest pseudolip`).
	// Affects new lines only.
	void SetPseudoLipEnabled(bool a_enabled);
	bool PseudoLipEnabled();

	// mouth timing lead in ms ([lipsync] lead_ms; console: `autest liplead`).
	// Positive = curves sampled ahead of the playback clock (mouth earlier);
	// applies to lip, pseudo and envelope modes, live entries included.
	void SetLeadMs(std::int32_t a_ms);
	std::int32_t LeadMs();

	// pull [lipsync] settings from Config (call after Load/Reload)
	void ApplyConfig();

	// drop all state (preload / new game)
	void Reset();
}
