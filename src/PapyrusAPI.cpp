#include "PapyrusAPI.h"

#include "AudioEngine.h"
#include "Config.h"
#include "FolderCache.h"
#include "InstanceManager.h"
#include "PPABridge.h"

namespace PapyrusAPI
{
	namespace
	{
		constexpr auto SCRIPT_NAME = "HentairimAudio";
		constexpr std::int32_t API_VERSION = 1;

		using VM = RE::BSScript::IVirtualMachine;

		// ---------- slot resolution ----------

		const Config::Slot* ResolveSlotForActor(const Config::Settings& a_settings, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return nullptr;
			}
			auto* base = a_actor->GetActorBase();
			if (!base) {
				return nullptr;
			}

			// 1. per-NPC override
			if (!a_settings.npcOverrides.empty()) {
				if (const auto* file = base->GetFile(0)) {
					const auto formID = base->GetFormID();
					const std::uint32_t localID =
						(formID >> 24) == 0xFE ? (formID & 0xFFF) : (formID & 0xFFFFFF);
					const auto key = Config::MakeNpcKey(file->GetFilename(), localID);
					if (const auto it = a_settings.npcOverrides.find(key);
						it != a_settings.npcOverrides.end()) {
						if (const auto* slot = Config::FindSlot(a_settings, it->second)) {
							return slot;
						}
					}
				}
			}

			// 2-4. voicetype remap -> map -> default by sex
			std::string voicetype;
			if (const auto* vt = base->GetVoiceType()) {
				voicetype = Config::Normalize(vt->GetFormEditorID());
			}
			if (!voicetype.empty()) {
				if (a_settings.voicetypeRemapEnabled) {
					if (const auto it = a_settings.voicetypeRemap.find(voicetype);
						it != a_settings.voicetypeRemap.end()) {
						voicetype = it->second;
					}
				}
				if (const auto it = a_settings.voicetypeMap.find(voicetype);
					it != a_settings.voicetypeMap.end()) {
					if (const auto* slot = Config::FindSlot(a_settings, it->second)) {
						return slot;
					}
				}
			}

			const bool female = base->GetSex() == RE::SEX::kFemale;
			return Config::FindSlot(a_settings,
				female ? a_settings.defaultFemaleSlot : a_settings.defaultMaleSlot);
		}

		// ---------- shared play helper ----------

		std::int32_t PlayFromKey(const std::string& a_folderKey, RE::Actor* a_follow,
			float a_volume, const std::string& a_group, const std::string& a_channel)
		{
			if (a_folderKey.empty()) {
				return 0;
			}
			const auto file = FolderCache::PickNext(a_folderKey);
			if (file.empty()) {
				return 0;
			}
			auto handle = AudioEngine::PlayPath(file, a_follow, a_volume);
			if (!handle.IsValid()) {
				return 0;
			}
			const auto id = InstanceManager::Register(handle, a_volume, a_group);
			if (!a_channel.empty()) {
				InstanceManager::PlayOnChannel(a_channel, id);
			}
			return id;
		}

		// ---------- natives: core ----------

		std::int32_t GetAPIVersion(RE::StaticFunctionTag*)
		{
			return API_VERSION;
		}

		bool ReloadConfig(RE::StaticFunctionTag*)
		{
			const bool ok = Config::Load();
			FolderCache::Rebuild();
			InstanceManager::ApplyConfigGroupVolumes();
			PPABridge::SetEventRateMs(Config::Get()->ppaEventRateMs);
			return ok;
		}

		std::int32_t PlayVoice(RE::StaticFunctionTag*, RE::Actor* a_actor,
			RE::BSFixedString a_category, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			const auto settings = Config::Get();
			const auto* slot = ResolveSlotForActor(*settings, a_actor);
			if (!slot) {
				logger::warn("PlayVoice: no slot resolvable for actor");
				return 0;
			}
			auto key = FolderCache::ResolveVoiceKey(*settings, *slot, a_category.c_str());
			if (key.empty()) {
				// last resort: non-voice scene sounds (PullOutGape, Smack, ...) live in the sfx table
				const auto sfxKey = "sfx/" + Config::Normalize(a_category.c_str());
				if (FolderCache::FileCount(sfxKey) > 0) {
					key = sfxKey;
				}
			}
			return PlayFromKey(key, a_actor, a_volume, a_group.c_str(), a_channel.c_str());
		}

		std::int32_t PlayVoiceFromSlot(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category, RE::Actor* a_follow, float a_volume,
			RE::BSFixedString a_group, RE::BSFixedString a_channel)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot.c_str());
			if (!slot) {
				logger::warn("PlayVoiceFromSlot: unknown slot '{}'", a_slot.c_str());
				return 0;
			}
			const auto key = FolderCache::ResolveVoiceKey(*settings, *slot, a_category.c_str());
			return PlayFromKey(key, a_follow, a_volume, a_group.c_str(), a_channel.c_str());
		}

		std::int32_t PlaySFX(RE::StaticFunctionTag*, RE::BSFixedString a_name,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			const auto key = "sfx/" + Config::Normalize(a_name.c_str());
			return PlayFromKey(key, a_follow, a_volume, a_group.c_str(), a_channel.c_str());
		}

		std::int32_t PlayFile(RE::StaticFunctionTag*, RE::BSFixedString a_path,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			std::string path = a_path.c_str();
			std::replace(path.begin(), path.end(), '/', '\\');
			auto handle = AudioEngine::PlayPath(path, a_follow, a_volume);
			if (!handle.IsValid()) {
				return 0;
			}
			const auto id = InstanceManager::Register(handle, a_volume, a_group.c_str());
			if (a_channel.length() > 0) {
				InstanceManager::PlayOnChannel(a_channel.c_str(), id);
			}
			return id;
		}

		std::int32_t PlayFolder(RE::StaticFunctionTag*, RE::BSFixedString a_folder,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			const auto key = FolderCache::ResolveDirKey(a_folder.c_str());
			return PlayFromKey(key, a_follow, a_volume, a_group.c_str(), a_channel.c_str());
		}

		// ---------- natives: handles ----------

		bool IsHandlePlaying(RE::StaticFunctionTag*, std::int32_t a_handle)
		{
			return a_handle > 0 && InstanceManager::IsPlaying(a_handle);
		}

		bool StopHandle(RE::StaticFunctionTag*, std::int32_t a_handle)
		{
			return a_handle > 0 && InstanceManager::Stop(a_handle);
		}

		float GetHandleDuration(RE::StaticFunctionTag*, std::int32_t a_handle)
		{
			return a_handle > 0 ? InstanceManager::DurationSec(a_handle) : 0.0f;
		}

		void SetHandleVolume(RE::StaticFunctionTag*, std::int32_t a_handle, float a_volume)
		{
			if (a_handle > 0) {
				InstanceManager::SetInstanceVolume(a_handle, a_volume);
			}
		}

		// ---------- natives: groups & channels ----------

		void SetGroupVolume(RE::StaticFunctionTag*, RE::BSFixedString a_group, float a_volume)
		{
			InstanceManager::SetGroupVolume(a_group.c_str(), a_volume);
		}

		void DuckGroup(RE::StaticFunctionTag*, RE::BSFixedString a_group, float a_factor)
		{
			InstanceManager::DuckGroup(a_group.c_str(), a_factor);
		}

		void UnduckGroup(RE::StaticFunctionTag*, RE::BSFixedString a_group)
		{
			InstanceManager::UnduckGroup(a_group.c_str());
		}

		void StopGroup(RE::StaticFunctionTag*, RE::BSFixedString a_group)
		{
			InstanceManager::StopGroup(a_group.c_str());
		}

		void StopAllAudio(RE::StaticFunctionTag*)
		{
			InstanceManager::StopAll();
		}

		void StopChannel(RE::StaticFunctionTag*, RE::BSFixedString a_channel)
		{
			InstanceManager::StopChannel(a_channel.c_str());
		}

		// ---------- natives: introspection ----------

		RE::BSFixedString GetSlotForActor(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			const auto settings = Config::Get();
			const auto* slot = ResolveSlotForActor(*settings, a_actor);
			return slot ? slot->id.c_str() : "";
		}

		std::int32_t GetCategoryFileCount(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot.c_str());
			if (!slot) {
				return 0;
			}
			const auto key = FolderCache::ResolveVoiceKey(*settings, *slot, a_category.c_str());
			return key.empty() ? 0 : FolderCache::FileCount(key);
		}

		bool CategoryExists(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category)
		{
			return GetCategoryFileCount(nullptr, a_slot, a_category) > 0;
		}

		// ---------- natives: PPA ----------

		bool IsPPAConnected(RE::StaticFunctionTag*)
		{
			return PPABridge::Connected();
		}

		void SetPPAEventRate(RE::StaticFunctionTag*, std::int32_t a_ms)
		{
			PPABridge::SetEventRateMs(a_ms > 0 ? static_cast<std::uint32_t>(a_ms) : 500u);
		}

		std::int32_t GetPPAContext(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? static_cast<std::int32_t>(snapshot->context) : 0;
		}

		float GetPPADepth(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? snapshot->depth : 0.0f;
		}

		float GetPPAVaginalOpening(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? snapshot->vaginalOpening : 0.0f;
		}

		float GetPPAAnalOpening(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? snapshot->anusOpening : 0.0f;
		}

		// ---------- natives: debug ----------

		std::int32_t DebugPlayFile(RE::StaticFunctionTag*, RE::BSFixedString a_path,
			RE::Actor* a_follow, std::int32_t a_flags, std::int32_t a_priority)
		{
			auto handle = AudioEngine::PlayPath(a_path.c_str(), a_follow, 1.0f,
				static_cast<std::uint32_t>(a_flags), static_cast<std::uint32_t>(a_priority));
			if (!handle.IsValid()) {
				return 0;
			}
			return InstanceManager::Register(handle, 1.0f, "");
		}
	}

	bool RegisterFuncs(VM* a_vm)
	{
		REGISTERFUNC(GetAPIVersion, SCRIPT_NAME);
		REGISTERFUNC(ReloadConfig, SCRIPT_NAME);
		REGISTERFUNC(PlayVoice, SCRIPT_NAME);
		REGISTERFUNC(PlayVoiceFromSlot, SCRIPT_NAME);
		REGISTERFUNC(PlaySFX, SCRIPT_NAME);
		REGISTERFUNC(PlayFile, SCRIPT_NAME);
		REGISTERFUNC(PlayFolder, SCRIPT_NAME);
		REGISTERFUNC(IsHandlePlaying, SCRIPT_NAME);
		REGISTERFUNC(StopHandle, SCRIPT_NAME);
		REGISTERFUNC(GetHandleDuration, SCRIPT_NAME);
		REGISTERFUNC(SetHandleVolume, SCRIPT_NAME);
		REGISTERFUNC(SetGroupVolume, SCRIPT_NAME);
		REGISTERFUNC(DuckGroup, SCRIPT_NAME);
		REGISTERFUNC(UnduckGroup, SCRIPT_NAME);
		REGISTERFUNC(StopGroup, SCRIPT_NAME);
		REGISTERFUNC(StopAllAudio, SCRIPT_NAME);
		REGISTERFUNC(StopChannel, SCRIPT_NAME);
		REGISTERFUNC(GetSlotForActor, SCRIPT_NAME);
		REGISTERFUNC(GetCategoryFileCount, SCRIPT_NAME);
		REGISTERFUNC(CategoryExists, SCRIPT_NAME);
		REGISTERFUNC(IsPPAConnected, SCRIPT_NAME);
		REGISTERFUNC(SetPPAEventRate, SCRIPT_NAME);
		REGISTERFUNC(GetPPAContext, SCRIPT_NAME);
		REGISTERFUNC(GetPPADepth, SCRIPT_NAME);
		REGISTERFUNC(GetPPAVaginalOpening, SCRIPT_NAME);
		REGISTERFUNC(GetPPAAnalOpening, SCRIPT_NAME);
		REGISTERFUNC(DebugPlayFile, SCRIPT_NAME);
		return true;
	}
}
