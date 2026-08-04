#include "API/AudioUtilAPI.h"

#include "PapyrusAPI.h"

// Native C++ exports for other SKSE plugins - a mirror of the AudioUtil Papyrus
// PlayFile natives. Thin, null-safe forwarders onto the same PapyrusAPI::PlayFileByPath
// the Papyrus bindings use, so C++ and Papyrus callers get identical behaviour
// (captions, channels, groups, lipsync guards). Exported via exports.def; see
// include/API/AudioUtilAPI.h for semantics and the consumer contract.

extern "C" {

// ------------------------------------------------------------------------------- Meta

std::uint32_t AudioUtil_GetVersion()
{
	// Packed from the DLL's own plugin declaration (xmake.lua PROJECT_VERSION), so it
	// tracks the release automatically. major*1e7 + minor*1e5 + patch (900009 = 0.9.9).
	const auto v = SKSE::PluginDeclaration::GetSingleton()->GetVersion();
	return static_cast<std::uint32_t>(v.major()) * 10000000u +
	       static_cast<std::uint32_t>(v.minor()) * 100000u +
	       static_cast<std::uint32_t>(v.patch());
}

std::uint32_t AudioUtil_GetInterfaceVersion()
{
	return 10000;  // 1.0.0
}

// ---------------------------------------------------------------------------- Playback

std::int32_t AudioUtil_PlayFile(const char* dataRelPath, RE::Actor* follow,
	float volume, const char* group, const char* channel)
{
	if (!dataRelPath || !*dataRelPath) {
		return 0;
	}
	return PapyrusAPI::PlayFileByPath(dataRelPath, follow, volume,
		group ? group : "", channel ? channel : "", false);
}

std::int32_t AudioUtil_PlayFileWithLipSync(const char* dataRelPath, RE::Actor* follow,
	float volume, const char* group, const char* channel)
{
	if (!dataRelPath || !*dataRelPath) {
		return 0;
	}
	return PapyrusAPI::PlayFileByPath(dataRelPath, follow, volume,
		group ? group : "", channel ? channel : "", true);
}

}  // extern "C"
