#include "AudioEngine.h"

#include "Config.h"
#include "FuzCache.h"
#include "FuzSlots.h"

namespace AudioEngine
{
	namespace
	{
		std::atomic<int> g_outputModelMode{ -1 };
		std::atomic<int> g_voiceSliderMode{ -1 };

		bool ModeOr(const std::atomic<int>& a_mode, bool a_config)
		{
			const int mode = a_mode.load();
			return mode < 0 ? a_config : mode != 0;
		}

		std::string ModeStatus(const std::atomic<int>& a_mode, bool a_config)
		{
			const int mode = a_mode.load();
			return std::format("{} ({})", ModeOr(a_mode, a_config) ? "on" : "off",
				mode < 0 ? "config" : "autest override");
		}

		template <class T>
		T* DefaultObject(RE::DEFAULT_OBJECT a_object)
		{
			auto* dobj = RE::BGSDefaultObjectManager::GetSingleton();
			return dobj ? dobj->GetObject<T>(a_object) : nullptr;
		}

		// VNAM, stored as u16 of 1.0 (CommonLib's GetStaticVolumeMultiplier
		// divides as integers and reads 0 for anything below 1.0)
		float StaticMult(const RE::BGSSoundCategory& a_category)
		{
			return static_cast<float>(a_category.staticMult) / 65535.0f;
		}

		std::string FormName(const RE::TESForm* a_form)
		{
			return a_form ? std::format("{:08X}", a_form->GetFormID()) : "none"s;
		}

		const RE::BGSSoundOutput* OutputModelFor(RE::Actor* a_follow)
		{
			if (!a_follow || a_follow->IsPlayerRef() || !OutputModelEnabled()) {
				return nullptr;
			}
			return DefaultObject<RE::BGSSoundOutput>(RE::DEFAULT_OBJECT::kDialogueOutputModel3D);
		}
	}

	float VoiceVolumeFactor()
	{
		if (!VoiceSliderEnabled()) {
			return 1.0f;
		}
		const auto* category = DefaultObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kDialogueVoiceCategory);
		return category ? std::clamp(category->GetCategoryVolume(), 0.0f, 1.0f) : 1.0f;
	}

	void SetOutputModelMode(int a_mode)
	{
		g_outputModelMode.store(a_mode < 0 ? -1 : (a_mode != 0 ? 1 : 0));
	}

	bool OutputModelEnabled()
	{
		return ModeOr(g_outputModelMode, Config::Get()->dialogueOutputModel);
	}

	void SetVoiceSliderMode(int a_mode)
	{
		g_voiceSliderMode.store(a_mode < 0 ? -1 : (a_mode != 0 ? 1 : 0));
	}

	bool VoiceSliderEnabled()
	{
		return ModeOr(g_voiceSliderMode, Config::Get()->voiceSlider);
	}

	std::string OutputModelStatus()
	{
		return ModeStatus(g_outputModelMode, Config::Get()->dialogueOutputModel);
	}

	std::string VoiceSliderStatus()
	{
		return ModeStatus(g_voiceSliderMode, Config::Get()->voiceSlider);
	}

	void ApplyConfig()
	{
		// an `autest` A/B ends with a reload, as `autest lipfiles` does
		g_outputModelMode.store(-1);
		g_voiceSliderMode.store(-1);
	}

	std::string DescribeDialogueDefaults()
	{
		std::string out;
		if (const auto* model = DefaultObject<RE::BGSSoundOutput>(RE::DEFAULT_OBJECT::kDialogueOutputModel3D)) {
			const auto* att = model->attenuation;
			out = std::format("dialogue 3D model {}: {}, rolls off {}, {:.0f} to {:.0f} units", FormName(model),
				model->DoGetUsesHRTF() ? "HRTF" : "speaker output", model->DoGetAttenuatesWithDistance(),
				att ? att->data.minDistance : 0.0f, att ? att->data.maxDistance : 0.0f);
		} else {
			out = "dialogue 3D model: none";
		}
		// each level's own slider and static multiplier; GetCategoryVolume of the
		// first is their product, the Voice factor
		const auto* category = DefaultObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kDialogueVoiceCategory);
		out += "\nvoice categories:";
		for (int depth = 0; category && depth < 8; ++depth) {
			out += std::format(" {} slider {:.2f} x static {:.2f};", FormName(category), category->volumeMult,
				StaticMult(*category));
			category = category->parentCategory;
		}
		out += std::format(" Voice factor {:.2f}, output model {}, voice slider {}", VoiceVolumeFactor(),
			OutputModelStatus(), VoiceSliderStatus());
		return out;
	}

	RE::BSSoundHandle PlayPath(const std::string& a_dataRelPath, RE::Actor* a_follow,
		float a_volume, std::uint32_t a_flags, std::uint32_t a_priority,
		bool a_translateFuz, Played* a_played)
	{
		RE::BSSoundHandle handle;
		if (a_played) {
			*a_played = {};
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
		// stop (a_played) and the pool is on; otherwise play the cache path direct
		// (correct once it's been cached by a prior session). The slot is a .wav, so
		// only route a decoded-PCM cache path — a decode-failure .xwm fallback keeps
		// its own extension so the engine picks the right codec. See FuzSlots.
		int         slot = -1;
		std::string slotPath;
		if (a_played && playPath == &extracted && FuzSlots::Enabled() &&
			extracted.ends_with(".wav")) {
			slot = FuzSlots::AcquireWithCopy(extracted, slotPath);
			if (slot >= 0) {
				playPath = &slotPath;
			}
		}

		RE::BSResource::ID id;
		id.GenerateFromPath(playPath->c_str());
		manager->GetSoundHandleByFile(handle, id, a_flags, a_priority);

		// A slot not in the launch index (e.g. one this session self-healed into
		// existence) fails to build — fall back to the direct cache path rather than
		// dropping the line. That path plays if a prior session already cached it.
		if (!handle.IsValid() && slot >= 0) {
			FuzSlots::Release(slot);
			slot = -1;
			id.GenerateFromPath(extracted.c_str());
			manager->GetSoundHandleByFile(handle, id, a_flags, a_priority);
		}

		if (!handle.IsValid()) {
			logger::warn("GetSoundHandleByFile failed for '{}' (flags=0x{:X}, priority={})",
				a_dataRelPath, a_flags, a_priority);
			return handle;
		}
		if (a_played) {
			a_played->fuzSlot = slot;
		}

		handle.SetVolume(std::clamp(a_volume, 0.0f, 1.0f));

		if (a_follow) {
			if (auto* node = a_follow->Get3D()) {
				// The engine places a sound in the speakers and rolls it off with
				// distance only through its output model. A sound built from a
				// bare file has none: it played centered and at full level
				// wherever it followed. A real NPC line goes through this one.
				if (const auto* model = OutputModelFor(a_follow)) {
					handle.SetOutputModel(model);
					if (a_played) {
						a_played->outputModel = model;
					}
				}
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
		Played* a_played)
	{
		const auto settings = Config::Get();
		return PlayPath(a_dataRelPath, a_follow, a_volume, settings->soundFlags, settings->soundPriority,
			true, a_played);
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
