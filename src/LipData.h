#pragma once

// Parsed Skyrim .lip animation data. A .lip is a 24-byte header (or a 20-byte
// prerollless one, const14=7) plus a frame-major positional token grid at
// 30 fps, 33 slots per frame:
//   slots 0-15  = the 16 MFG phoneme channels (identity order, Aah..W)
//   slots 16-31 = the 16 MFG modifier channels (BlinkL..head)
//   slot  32    = unused
// The slot map AND the payload byte-format are engine-verified against the
// game's own FUZE/lip loader (SkyrimSE.exe 1.6.1170): the payload is ZERO-RLE
// compressed (a 0x00 byte + u16-LE count = that many zero bytes; other bytes
// literal), decompressing to a dense float32[frames*33] grid of [0,1] weights.
// (The old "dup/tangent/marker" grammar mis-modeled the compressed bytes.)
// Parsing has no game dependencies.
namespace LipData
{
	inline constexpr std::uint32_t CHANNELS = 32;         // 16 phonemes + 16 modifiers
	// interpolation amount for Anim::Sample ([lipsync] frame_interpolation,
	// base-only, 0..1): 0 = hold each 30 fps frame's value (stepped),
	// 1 = full linear interpolation (smooth), between = the value travels that
	// fraction of the way toward the next frame within each frame. Whether the
	// ENGINE lerps or steps its own lip playback is unverified — the A/B knob.
	inline std::atomic<float> g_frameInterp{ 0.3f };
	inline constexpr std::uint32_t PHONEME_CHANNELS = 16;
	inline constexpr float         FPS = 30.0f;

	struct Anim
	{
		std::uint32_t      frames = 0;      // count on the 30 fps grid
		float              durationSec = 0.0f;
		// dense per-channel timelines, values[channel][frame] in [0,1]
		std::vector<float> values[CHANNELS];

		// sampled per g_frameInterp (0 hold .. 1 full lerp); 0 outside the clip
		float Sample(std::uint32_t a_channel, float a_t) const
		{
			if (a_channel >= CHANNELS || frames == 0 || a_t < 0.0f) {
				return 0.0f;
			}
			const auto& series = values[a_channel];
			if (series.empty()) {
				return 0.0f;
			}
			const float pos = a_t * FPS;
			const auto  index = static_cast<std::size_t>(pos);
			if (index + 1 >= series.size()) {
				return index < series.size() ? series[index] : 0.0f;
			}
			const float amount = g_frameInterp.load(std::memory_order_relaxed);
			if (amount <= 0.0f) {
				return series[index];  // hold: the frame's value plays verbatim
			}
			const float frac = (pos - static_cast<float>(index)) * std::min(amount, 1.0f);
			return series[index] + (series[index + 1] - series[index]) * frac;
		}

		// true if any phoneme channel carries signal (a lip worth driving)
		bool HasMouthData() const
		{
			for (std::uint32_t ch = 0; ch < PHONEME_CHANNELS; ++ch) {
				for (const float v : values[ch]) {
					if (v > 0.01f) {
						return true;
					}
				}
			}
			return false;
		}

		// true if any modifier channel (16-31, blink/brow/gaze) carries signal —
		// i.e. a real authored lip. Pseudo-synth lines write only phonemes, so
		// this stays false for them, letting `drive_modifiers` leave the upper
		// face to expression mods on lines that carry no modifier animation.
		bool HasModifierData() const
		{
			for (std::uint32_t ch = PHONEME_CHANNELS; ch < CHANNELS; ++ch) {
				for (const float v : values[ch]) {
					if (v > 0.01f) {
						return true;
					}
				}
			}
			return false;
		}
	};

	// Parse a raw LIP block (the bytes between the FUZE header and the audio,
	// or a standalone .lip file's contents). Handles header variants B (extra
	// byte at offset 14) and C (20-byte prerollless header). Returns nullptr
	// on any structural mismatch.
	std::shared_ptr<const Anim> Parse(const std::uint8_t* a_data, std::size_t a_size);

	// Resolve + parse the lip for a played path, session-cached (misses too):
	//   *.fuz          -> the fuz's embedded LIP block
	//   anything else  -> a same-stem .lip beside it (loose or BSA)
	// Returns nullptr when there is no (usable) lip.
	std::shared_ptr<const Anim> GetFor(const std::string& a_dataRelPath);

	// drop the session cache (config reload)
	void ClearCache();
}
