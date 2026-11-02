#pragma once

#include <string>
#include <string_view>
#include <vector>

// The .fuz voice container: "FUZE" magic, u32 version, u32 lip size, the LIP
// block, then the RIFF audio (form "XWMA" = xWMA, "WAVE" = PCM wav).
//
// Playback needs nothing from here. The engine's generic sound open
// (BSXAudio2GameSound::SyncOpen / FinishAsyncOpen) skips the FUZE header and
// lip block itself and decodes the xWMA inside, the way every vanilla dialogue
// line plays, so AudioEngine::PlayPath builds a .fuz path as-is, loose or
// BSA-packed. (Confirmed in game 2026-11-02 with `autest fuzraw`. Before
// 0.9.32 the DLL decoded every fuz to a PCM wav on disk and routed it through
// a pool of pre-indexed placeholder wavs; the silence that motivated it was a
// mid-session-written cache file missing from the launch-only loose-file index,
// never the fuz itself.)
//
// What AudioUtil still reads out of a fuz, in memory and never written to disk:
// the LIP block for phoneme lipsync, and the audio decoded to PCM for the
// amplitude envelope of a fuz without a usable lip.
namespace Fuz
{
	// true if the path names a .fuz file (case-insensitive extension check)
	bool IsFuzPath(std::string_view a_path);

	// The embedded LIP block, read through the engine's resource loader (loose
	// or BSA-packed fuz). Empty if the fuz is unreadable, malformed, or carries
	// no lip.
	std::vector<std::uint8_t> ReadLipBlock(const std::string& a_fuzDataRelPath);

	// The fuz's audio as an in-memory RIFF/WAVE blob: a WAVE payload as-is, an
	// xWMA payload decoded to 16-bit PCM through the OS WMA decoder DMO. Empty
	// (warned) if the fuz is unreadable or the decode fails. Uncached: the
	// caller keeps what it derives (LipSync keeps the envelope, per path).
	std::vector<std::uint8_t> DecodeToWav(const std::string& a_fuzDataRelPath);
}
