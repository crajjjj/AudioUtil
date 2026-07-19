#include "AudioEngine.h"

#include "Config.h"

namespace AudioEngine
{
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority)
	{
		RE::BSSoundHandle handle;

		auto* manager = RE::BSAudioManager::GetSingleton();
		if (!manager) {
			logger::error("BSAudioManager unavailable");
			return handle;
		}

		RE::BSResource::ID id;
		id.GenerateFromPath(a_dataRelPath.c_str());
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
}
