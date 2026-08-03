#include "AudioEngine.h"

#include "Config.h"
#include "FuzCache.h"

namespace AudioEngine
{
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority,
		bool a_translateFuz)
	{
		RE::BSSoundHandle handle;

		auto* manager = RE::BSAudioManager::GetSingleton();
		if (!manager) {
			logger::error("BSAudioManager unavailable");
			return handle;
		}

		// .fuz (voice container) can't be fed to the generic decoder - swap in
		// the cached xWMA/wav payload FuzCache extracts. Only this build step
		// uses the translated path: callers keep registering/lipsyncing/
		// captioning against the ORIGINAL path (a .fuz caption sidecar is
		// foo.toml next to foo.fuz).
		const std::string* playPath = &a_dataRelPath;
		std::string        extracted;
		if (a_translateFuz && FuzCache::IsFuzPath(a_dataRelPath)) {
			extracted = FuzCache::Resolve(a_dataRelPath);
			if (extracted.empty()) {
				return handle;  // Resolve already logged why
			}
			playPath = &extracted;
		}

		RE::BSResource::ID id;
		id.GenerateFromPath(playPath->c_str());
		manager->BuildSoundDataFromFile(handle, id, a_flags, a_priority);

		if (!handle.IsValid()) {
			logger::warn("BuildSoundDataFromFile failed for '{}' (flags=0x{:X}, priority={})",
				a_dataRelPath, a_flags, a_priority);
			return handle;
		}

		handle.SetVolume(std::clamp(a_volume, 0.0f, 1.0f));

		if (a_follow) {
			if (auto* node = a_follow->Get3D()) {
				handle.SetObjectToFollow(node);
			}
		}

		if (!handle.Play()) {
			logger::warn("BSSoundHandle::Play failed for '{}'", a_dataRelPath);
		}
		logger::debug("Playing '{}' (soundID={}, vol={:.2f})", a_dataRelPath, handle.soundID, a_volume);
		return handle;
	}

	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow, float a_volume)
	{
		const auto settings = Config::Get();
		return PlayPath(a_dataRelPath, a_follow, a_volume, settings->soundFlags, settings->soundPriority);
	}

	bool ResourceExists(const std::string& a_dataRelPath)
	{
		if (a_dataRelPath.empty()) {
			return false;
		}
		// opening a resource stream succeeds only if the path resolves from a
		// loose file or a mounted archive - the engine's own lookup
		RE::BSResourceNiBinaryStream stream{ a_dataRelPath.c_str() };
		return stream.good();
	}
}
