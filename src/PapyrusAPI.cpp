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
		constexpr auto SCRIPT_NAME = "AudioUtil";
		constexpr auto PPA_SCRIPT_NAME = "AudioUtilPPA";
		constexpr std::int32_t API_VERSION = 1;

		using VM = RE::BSScript::IVirtualMachine;

		// ---------- slot resolution ----------

		bool SameSlotID(const std::string& a_id, const std::string& a_other)
		{
			return a_id.size() == a_other.size() &&
			       std::equal(a_id.begin(), a_id.end(), a_other.begin(),
					   [](char a, char b) { return std::tolower(a) == std::tolower(b); });
		}

		// slots named by pc_female_slot / pc_male_slot belong to the player alone
		bool IsPCReserved(const Config::Settings& a_settings, const Config::Slot& a_slot)
		{
			return (!a_settings.pcFemaleSlot.empty() && SameSlotID(a_slot.id, a_settings.pcFemaleSlot)) ||
			       (!a_settings.pcMaleSlot.empty() && SameSlotID(a_slot.id, a_settings.pcMaleSlot));
		}

		// load-order-stable per-NPC id for spreading actors across candidate slots
		std::uint32_t StableLocalID(RE::TESNPC* a_base)
		{
			const auto formID = a_base->GetFormID();
			return (formID >> 24) == 0xFE ? (formID & 0xFFF) : (formID & 0xFFFFFF);
		}

		// pick one usable slot from a candidate list, deterministically by actor:
		// the same NPC always gets the same slot; different NPCs spread across the
		// list. Unusable candidates (missing, PC-reserved) are filtered first so
		// the pick is always among real options.
		const Config::Slot* PickFromSlotList(const Config::Settings& a_settings,
			const Config::SlotList& a_candidates, RE::TESNPC* a_base,
			const std::function<bool(const Config::Slot*)>& a_usable)
		{
			std::vector<const Config::Slot*> usable;
			usable.reserve(a_candidates.size());
			for (const auto& id : a_candidates) {
				if (const auto* slot = Config::FindSlot(a_settings, id); a_usable(slot)) {
					usable.push_back(slot);
				}
			}
			if (usable.empty()) {
				return nullptr;
			}
			if (usable.size() == 1) {
				return usable.front();
			}
			// mix the local id so consecutive editor ids don't all alternate in step
			const std::uint32_t mixed = StableLocalID(a_base) * 2654435761u;
			return usable[mixed % usable.size()];
		}

		// resolution order: PC reservation (player only) -> npc_overrides ->
		// voicetype remap/map -> race match -> default by sex. Non-player actors
		// never resolve to a PC-reserved slot; if the default is reserved they get
		// the first free slot of their sex instead.
		const Config::Slot* ResolveSlotForActor(const Config::Settings& a_settings, RE::Actor* a_actor)
		{
			if (!a_actor) {
				return nullptr;
			}
			auto* base = a_actor->GetActorBase();
			if (!base) {
				return nullptr;
			}

			const bool isPC = a_actor->IsPlayerRef();
			const bool female = base->GetSex() == RE::SEX::kFemale;

			// 0. the player speaks with their reserved slot when one is configured
			if (isPC) {
				const auto& pcID = female ? a_settings.pcFemaleSlot : a_settings.pcMaleSlot;
				if (!pcID.empty()) {
					if (const auto* slot = Config::FindSlot(a_settings, pcID)) {
						return slot;
					}
				}
			}

			const auto usable = [&](const Config::Slot* a_slot) {
				return a_slot && (isPC || !IsPCReserved(a_settings, *a_slot));
			};

			// 1. per-NPC override (an explicit pin wins, even to a reserved slot)
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

			// 2. voicetype remap -> map
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
					if (const auto* slot = PickFromSlotList(a_settings, it->second, base, usable)) {
						return slot;
					}
				}
			}

			// 3. race match: first [race_map] hint (most specific first) found in
			// the actor's race editor id ("nord" matches NordRace / NordRaceVampire);
			// only same-sex slots qualify
			if (!a_settings.raceMap.empty()) {
				std::string raceID;
				if (const auto* race = a_actor->GetRace()) {
					raceID = Config::Normalize(race->GetFormEditorID());
				}
				if (!raceID.empty()) {
					const auto sexUsable = [&](const Config::Slot* a_slot) {
						return a_slot && a_slot->sex == (female ? 'F' : 'M') && usable(a_slot);
					};
					for (const auto& [hint, slotIDs] : a_settings.raceMap) {
						if (raceID.find(hint) == std::string::npos) {
							continue;
						}
						if (const auto* slot = PickFromSlotList(a_settings, slotIDs, base, sexUsable)) {
							return slot;
						}
					}
				}
			}

			// 4. default by sex; if reserved (or missing), first free slot of the sex
			const auto* fallback = Config::FindSlot(a_settings,
				female ? a_settings.defaultFemaleSlot : a_settings.defaultMaleSlot);
			if (usable(fallback)) {
				return fallback;
			}
			for (const auto& slot : a_settings.slots) {
				if (slot.sex == (female ? 'F' : 'M') && usable(&slot)) {
					return &slot;
				}
			}
			return nullptr;
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

		// bound under the separate AudioUtilPPA script: the bridge is an optional
		// integration and its surface stays out of the core AudioUtil class

		bool IsConnected(RE::StaticFunctionTag*)
		{
			return PPABridge::Connected();
		}

		void SetEventRate(RE::StaticFunctionTag*, std::int32_t a_ms)
		{
			PPABridge::SetEventRateMs(a_ms > 0 ? static_cast<std::uint32_t>(a_ms) : 2000u);
		}

		std::int32_t GetContext(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? static_cast<std::int32_t>(snapshot->context) : 0;
		}

		float GetDepth(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? snapshot->depth : 0.0f;
		}

		float GetVaginalOpening(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? snapshot->vaginalOpening : 0.0f;
		}

		float GetAnalOpening(RE::StaticFunctionTag*, RE::Actor* a_receiver)
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
		REGISTERFUNC(DebugPlayFile, SCRIPT_NAME);
		REGISTERFUNC(IsConnected, PPA_SCRIPT_NAME);
		REGISTERFUNC(SetEventRate, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetContext, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetDepth, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetVaginalOpening, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetAnalOpening, PPA_SCRIPT_NAME);
		return true;
	}
}
