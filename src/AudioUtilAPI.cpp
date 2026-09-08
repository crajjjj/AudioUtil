#include "API/AudioUtilAPI.h"

#include "MouthClaim.h"
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
	return 10200;  // 1.2.0 - claim listing
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

// ------------------------------------------------------------------------ Mouth claims

void AudioUtil_ClaimMouth(RE::Actor* actor, float seconds, const char* owner)
{
	MouthClaim::Claim(actor, seconds, owner ? owner : "");
}

void AudioUtil_ReleaseMouth(RE::Actor* actor, const char* owner)
{
	MouthClaim::Release(actor, owner ? owner : "");
}

bool AudioUtil_IsMouthClaimed(RE::Actor* actor)
{
	return MouthClaim::IsClaimed(actor);
}

bool AudioUtil_IsMouthBusy(RE::Actor* actor)
{
	return MouthClaim::IsBusy(actor);
}

std::uint32_t AudioUtil_GetMouthClaimOwner(RE::Actor* actor, char* buffer, std::uint32_t size)
{
	if (!buffer || size == 0) {
		return 0;
	}
	const auto owner = MouthClaim::Owner(actor);
	const auto written = static_cast<std::uint32_t>(
		owner.size() < size - 1 ? owner.size() : size - 1);
	std::memcpy(buffer, owner.data(), written);
	buffer[written] = '\0';
	return written;
}

float AudioUtil_GetMouthClaimTimeLeft(RE::Actor* actor)
{
	return MouthClaim::TimeLeft(actor);
}

std::uint32_t AudioUtil_GetClaimedActors(RE::Actor** out, std::uint32_t max)
{
	const auto ids = MouthClaim::ClaimedActorIDs();
	std::uint32_t written = 0;
	for (const auto formID : ids) {
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
		if (!actor) {
			continue;  // unloaded/deleted since the claim landed
		}
		if (out && written < max) {
			out[written] = actor;
		}
		++written;
	}
	// out == nullptr is the "how many?" call; otherwise never report more than
	// the caller's buffer actually holds
	return (out && written > max) ? max : written;
}

}  // extern "C"
