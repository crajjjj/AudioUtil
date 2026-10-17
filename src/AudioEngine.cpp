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
	}

	const RE::BGSSoundOutput* OutputModelFor(RE::Actor* a_follow)
	{
		if (!a_follow || a_follow->IsPlayerRef() || !a_follow->Get3D() || !OutputModelEnabled()) {
			return nullptr;
		}
		return DefaultObject<RE::BGSSoundOutput>(RE::DEFAULT_OBJECT::kDialogueOutputModel3D);
	}

	bool EngineRollsOff(RE::Actor* a_follow)
	{
		const auto* model = OutputModelFor(a_follow);
		return model && model->DoGetAttenuatesWithDistance();
	}

	float VoiceVolumeFactor()
	{
		if (!VoiceSliderEnabled()) {
			return 1.0f;
		}
		// The master slider is left out: it is what every sound gets, this one
		// included, and must not be applied twice.
		const auto* master = DefaultObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kMasterSoundCategory);
		const auto* category = DefaultObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kDialogueVoiceCategory);
		float       factor = 1.0f;
		for (int depth = 0; category && category != master && depth < 8; ++depth) {
			factor *= category->GetCategoryVolume() * StaticMult(*category);
			category = category->parentCategory;
		}
		return std::clamp(factor, 0.0f, 1.0f);
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

	std::string DescribeEngineSound(const RE::BSSoundHandle& a_handle)
	{
		auto* manager = RE::BSAudioManager::GetSingleton();
		if (!manager || !a_handle.IsValid()) {
			return "engine: no such sound";
		}
		const auto it = manager->activeSounds.find(a_handle.soundID);
		if (it == manager->activeSounds.end() || !it->second) {
			return std::format("engine: sound {} is not active", a_handle.soundID);
		}
		const auto* sound = it->second;
		const auto* model = sound->outputModel ? skyrim_cast<RE::BGSSoundOutput*>(sound->outputModel) : nullptr;
		const auto* category = sound->category ? skyrim_cast<RE::BGSSoundCategory*>(sound->category) : nullptr;
		return std::format("engine: sound {}, output model {}{}, category {}{}, static att {}, system att {}, "
						   "volume {:.2f}, listener distance {:.0f}",
			a_handle.soundID, FormName(model), sound->outputModel && !model ? " (not a form)" : "",
			FormName(category), sound->category && !category ? " (not a form)" : "",
			sound->staticAttenuation, sound->systemAttenuation, sound->volume, sound->listenerDistance);
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
		const auto* master = DefaultObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kMasterSoundCategory);
		const auto* category = DefaultObject<RE::BGSSoundCategory>(RE::DEFAULT_OBJECT::kDialogueVoiceCategory);
		out += "; voice categories:";
		for (int depth = 0; category && depth < 8; ++depth) {
			out += std::format(" {} vol {:.2f} (field {:.2f}) x static {:.2f}{}", FormName(category),
				category->GetCategoryVolume(), category->volumeMult, StaticMult(*category),
				category == master ? " [master, not applied]" : "");
			if (category == master) {
				break;
			}
			category = category->parentCategory;
		}
		out += std::format("; factor {:.2f}", VoiceVolumeFactor());
		return out;
	}

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
		if (a_slotOut) {
			*a_slotOut = slot;
		}

		handle.SetVolume(std::clamp(a_volume, 0.0f, 1.0f));

		if (a_follow) {
			if (auto* node = a_follow->Get3D()) {
				// A sound built from a bare file has no output model of its own,
				// and with none it played centered and at full level wherever it
				// followed. A real NPC line goes through this one.
				if (const auto* model = OutputModelFor(a_follow)) {
					handle.SetOutputModel(model);
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
