#pragma once

namespace CaptionManager
{
	// Show the caption for a freshly played file, if it has one: a same-named
	// .toml sidecar next to the wav (line01.wav + line01.toml) holding
	// per-language text (en = "...", ru = "..."). The text for the configured
	// [captions] language is injected as a game subtitle attributed to
	// a_speaker while the instance plays, and an "AudioUtil_Caption" mod event
	// (sender = speaker, strArg = text, numArg = instance id) is sent.
	// No-op when disabled, a_speaker is null, or no sidecar text resolves.
	void Start(RE::Actor* a_speaker, const std::string& a_dataRelPath,
		RE::BSSoundHandle a_handle, std::int32_t a_instanceId);

	// early-stop notification (StopHandle / channel replace / StopGroup / StopAll)
	void OnInstanceStopped(std::int32_t a_instanceId);

	// resolved caption text for a data-relative wav path in the current
	// language ("" if none) — the same lookup Start uses; backs the
	// GetHandleCaption native so a consumer can render captions itself
	std::string TextForFile(const std::string& a_dataRelPath);

	// runtime master switch; ApplyConfig re-reads the toml value
	void SetEnabled(bool a_enabled);
	bool Enabled();

	// pull [captions] settings from Config (call after Load/Reload)
	void ApplyConfig();

	// drop all state and clear anything still on screen (preload / new game)
	void Reset();
}
