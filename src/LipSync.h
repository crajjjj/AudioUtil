#pragma once

namespace LipSync
{
	// begin driving the actor's mouth from the wav's amplitude envelope, in sync
	// with the playing instance. No-op if disabled, the file isn't readable PCM
	// wav (e.g. xwm/BSA-packed), or the actor is invalid — failures only log.
	void Start(RE::Actor* a_actor, const std::string& a_dataRelPath,
		RE::BSSoundHandle a_handle, std::int32_t a_instanceId);

	// early-stop notification (StopHandle / channel replace / StopGroup / StopAll)
	void OnInstanceStopped(std::int32_t a_instanceId);

	void StopFor(RE::Actor* a_actor);
	bool IsActiveFor(RE::Actor* a_actor);

	// runtime master switch; ApplyConfig re-reads the toml value
	void SetEnabled(bool a_enabled);
	bool Enabled();
	void SetGain(float a_gain);

	// pull [lipsync] settings from Config (call after Load/Reload)
	void ApplyConfig();

	// drop all state (preload / new game)
	void Reset();
}
