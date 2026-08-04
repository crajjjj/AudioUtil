#pragma once

#include <cstdint>

namespace RE
{
	class Actor;
}

// ======================================================================================
// AudioUtil - native C++ inter-plugin API
// ======================================================================================
//
// A C++ mirror of the AudioUtil Papyrus `PlayFile` natives (the canonical modder
// interface), for SKSE plugins that want to play loose audio files directly in C++ -
// same semantics as the script API, no Papyrus round-trip, no SNDR/SOUN forms.
//
// Consumers do NOT link against AudioUtil - resolve the exports at runtime:
//
//     auto h = GetModuleHandleA("AudioUtil.dll");                 // null if not installed
//     if (h) {
//         auto playFile = reinterpret_cast<int32_t (*)(const char*, RE::Actor*, float,
//                             const char*, const char*)>(
//                             GetProcAddress(h, "AudioUtil_PlayFile"));
//         if (playFile) {
//             int32_t id = playFile("Sound\\FX\\MyMod\\whoosh.wav", actor, 1.0f, "", "");
//         }
//     }
//
// -------------------------------------------------------------------------------------
// SEMANTICS (identical to the Papyrus natives - same code path)
// -------------------------------------------------------------------------------------
// - `dataRelPath` is Data-relative ("Sound\\FX\\MyMod\\whoosh.wav"; forward slashes are
//   normalized). Playable formats: wav, xwm, fuz - loose or BSA-packed (the engine's
//   resource loader resolves both; a .fuz plays via its decoded PCM cache).
// - `follow` 3D-positions the sound at that actor (it tracks the actor while playing);
//   nullptr plays flat/2D at full volume.
// - `volume` is 0.0-1.0; effective volume = volume x group volume x duck factor.
// - `group` is a named volume group (SetGroupVolume/DuckGroup from the Papyrus side
//   affect it); "" = no special grouping.
// - `channel` is an exclusivity lane: playing on an occupied channel stops the previous
//   instance; "" = no channel.
// - Returns an instance handle: >0 success, 0 = nothing played. Handles share the id
//   space of the Papyrus API, so a handle returned here works with AudioUtil.StopHandle /
//   IsHandlePlaying / SetHandleVolume etc. from script. This C surface has no handle
//   ops of its own (yet) - feature-detect additions via AudioUtil_GetInterfaceVersion.
// - A same-named `.toml` caption sidecar next to the wav shows its text as a game
//   subtitle attributed to `follow` (loose files only), exactly like the Papyrus call.
// - The WithLipSync variant additionally drives `follow`'s mouth like a voice line:
//   authored `.lip`/fuz phoneme curves when available, else the amplitude envelope
//   (needs loose PCM wav or fuz). The global [lipsync] toggle and the gag / tongue /
//   player-dialogue guards all apply. The plain variant NEVER moves the mouth.
//
// -------------------------------------------------------------------------------------
// THREADING / LIFECYCLE
// -------------------------------------------------------------------------------------
// These are the exact code paths of the Papyrus natives, which run on Papyrus VM
// threads - call from the game thread, an SKSE task, or a VM thread. They are not
// validated from arbitrary background threads. Do not call before kDataLoaded (the
// audio engine and AudioUtil's config are not up yet).
//
// ABI: strings cross as null-terminated `const char*` (null tolerated = ""); actors as
// `RE::Actor*`; everything else is POD. All functions are null-safe.
//
// -------------------------------------------------------------------------------------
// WHAT THIS HEADER IS FOR (consumers)
// -------------------------------------------------------------------------------------
// It is a REFERENCE, not a link-time dependency: it documents the semantics and gives
// you the exact signatures to cast GetProcAddress results to. Copy it into your project
// and include it freely - the declarations carry no dllimport/dllexport, so including it
// can never make your plugin link against (or load-time depend on) AudioUtil.
//
// Do NOT call the AudioUtil_* names directly - they are declarations of functions that
// live in AudioUtil's DLL, so a direct call is an unresolved external at link time.
// Always go through a function pointer obtained from GetProcAddress, as shown above.
// (Inside AudioUtil itself the exports come from exports.def.)
// ======================================================================================

extern "C" {

// ------------------------------------------------------------------------------- Meta
// Packed AudioUtil DLL version (e.g. 900009 for 0.9.9, 10000000 for 1.0.0):
// major*10000000 + minor*100000 + patch. Tracks the mod release version.
std::uint32_t AudioUtil_GetVersion();
// Version of THIS C++ interface, packed MMmmpp (10000 == 1.0.0). Independent of the mod
// version; bumped only when exports are added. New functions are appended, never
// reordered/removed, so a check here is enough to feature-detect the C API surface.
std::uint32_t AudioUtil_GetInterfaceVersion();

// ---------------------------------------------------------------------------- Playback
// Play a loose (or BSA-packed) audio file by Data-relative path. Never drives the
// mouth. Returns the instance handle (>0) or 0 on failure.
std::int32_t AudioUtil_PlayFile(const char* dataRelPath, RE::Actor* follow,
	float volume, const char* group, const char* channel);
// Spoken-line variant: same as AudioUtil_PlayFile plus voice-call lipsync on `follow`
// (subject to the global lipsync toggle and the gag/tongue/dialogue guards).
std::int32_t AudioUtil_PlayFileWithLipSync(const char* dataRelPath, RE::Actor* follow,
	float volume, const char* group, const char* channel);

}  // extern "C"
