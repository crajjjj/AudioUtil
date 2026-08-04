#include "AudioEngine.h"

#include "Config.h"
#include "FuzCache.h"
#include "FuzSlots.h"

namespace AudioEngine
{
	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority,
		bool a_translateFuz, int* a_slotOut)
	{
		RE::BSSoundHandle handle;
		if (a_slotOut) {
			*a_slotOut = -1;
		}

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

		// Route the fuz-derived cache wav through a pre-indexed placeholder slot so
		// a first-session decode is audible now (the cache wav itself isn't in the
		// launch resource index yet). Only when the caller can release the slot on
		// stop (a_slotOut) and the pool is on; otherwise play the cache path direct
		// (correct once it's been cached by a prior session). The slot is a .wav, so
		// only route a decoded-PCM cache path — a decode-failure .xwm fallback keeps
		// its own extension so the engine picks the right codec. See FuzSlots.
		int         slot = -1;
		std::string slotPath;
		if (a_slotOut && playPath == &extracted && FuzSlots::Enabled() &&
			extracted.ends_with(".wav")) {
			slot = FuzSlots::AcquireWithCopy(extracted, slotPath);
			if (slot >= 0) {
				playPath = &slotPath;
			}
		}

		RE::BSResource::ID id;
		id.GenerateFromPath(playPath->c_str());
		manager->BuildSoundDataFromFile(handle, id, a_flags, a_priority);

		// A slot not in the launch index (e.g. one this session self-healed into
		// existence) fails to build — fall back to the direct cache path rather than
		// dropping the line. That path plays if a prior session already cached it.
		if (!handle.IsValid() && slot >= 0) {
			FuzSlots::Release(slot);
			slot = -1;
			id.GenerateFromPath(extracted.c_str());
			manager->BuildSoundDataFromFile(handle, id, a_flags, a_priority);
		}

		if (!handle.IsValid()) {
			logger::warn("BuildSoundDataFromFile failed for '{}' (flags=0x{:X}, priority={})",
				a_dataRelPath, a_flags, a_priority);
			return handle;
		}
		if (a_slotOut) {
			*a_slotOut = slot;
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

	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow, float a_volume,
		int* a_slotOut)
	{
		const auto settings = Config::Get();
		return PlayPath(a_dataRelPath, a_follow, a_volume, settings->soundFlags, settings->soundPriority,
			true, a_slotOut);
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
