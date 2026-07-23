#include "PapyrusAPI.h"

#include "AudioEngine.h"
#include "Config.h"
#include "FolderCache.h"
#include "GagState.h"
#include "InstanceManager.h"
#include "LipSync.h"
#include "PPABridge.h"
#include "TomlStore.h"

namespace PapyrusAPI
{
	namespace
	{
		constexpr auto SCRIPT_NAME = "AudioUtil";
		constexpr auto PPA_SCRIPT_NAME = "AudioUtilPPA";
		constexpr auto TOML_SCRIPT_NAME = "TomlUtil";
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
					// a race-mapped slot qualifies if it matches the actor's sex OR is
					// sex-neutral ('A') — this is what lets one creature slot serve a
					// creature regardless of the sex the engine reports for it
					const auto sexUsable = [&](const Config::Slot* a_slot) {
						return a_slot && (a_slot->sex == (female ? 'F' : 'M') || a_slot->sex == 'A') &&
						       usable(a_slot);
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

			// 4. default by sex; if reserved (or missing), first free slot of the sex.
			// This blind scan is F/M only — 'A' (sex-neutral) slots are reached only
			// by explicit routing, so an sfx/creature slot never leaks onto a
			// voiceless human here. (default_*_slot may still name an 'A' slot: that
			// goes through FindSlot above, not this scan.)
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

		// resolve (slot, category) to a folder key, applying gag routing: when the
		// actor is gagged and the slot has a gag_slot, the category resolves from
		// the gag slot instead; if that category is absent there, the muffled
		// gagDefaultCategory plays rather than leaking the clear line.
		std::string ResolveGaggedKey(const Config::Settings& a_settings,
			const Config::Slot& a_slot, std::string_view a_category, RE::Actor* a_actor)
		{
			const Config::Slot* slot = &a_slot;
			if (a_settings.gagEnabled && !a_slot.gagSlot.empty() && GagState::IsGagged(a_actor)) {
				if (const auto* gagSlot = Config::FindSlot(a_settings, a_slot.gagSlot)) {
					slot = gagSlot;
				}
			}
			auto key = FolderCache::ResolveVoiceKey(a_settings, *slot, a_category);
			if (key.empty() && slot != &a_slot && !a_settings.gagDefaultCategory.empty()) {
				key = FolderCache::ResolveVoiceKey(a_settings, *slot, a_settings.gagDefaultCategory);
			}
			return key;
		}

		// resolve an sfx name to a folder key: first as a category of the dedicated
		// sfx slot (id defaults to "SFX0"), so sfx pools get the full [[slot]]
		// toolset — explicit file lists (BSA-capable), folder refs, or a scanned
		// path — then the legacy flat [sfx] table. Direct key lookups (no
		// alias/fallback), so a name only in the [sfx] table doesn't trip the
		// voice resolver's miss warning.
		std::string ResolveSfxKey(const Config::Settings& a_settings, std::string_view a_name)
		{
			const auto catNorm = Config::Normalize(a_name);
			if (!a_settings.sfxSlot.empty()) {
				const auto slotKey = Config::Normalize(a_settings.sfxSlot) + "/" + catNorm;
				if (FolderCache::FileCount(slotKey) > 0) {
					return slotKey;
				}
			}
			const auto sfxKey = "sfx/" + catNorm;
			return FolderCache::FileCount(sfxKey) > 0 ? sfxKey : std::string{};
		}

		// ---------- shared play helper ----------

		// a_mouth: actor whose lips follow the clip's amplitude (voice calls pass
		// the speaker; sfx/folder/file playback passes nullptr)
		std::int32_t PlayFromKey(const std::string& a_folderKey, RE::Actor* a_follow,
			float a_volume, const std::string& a_group, const std::string& a_channel,
			RE::Actor* a_mouth = nullptr, bool a_noInterrupt = false)
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
				// claim atomically: if no-interrupt loses the race for a channel
				// still playing, drop this one (it's within startup grace, silent)
				if (!InstanceManager::PlayOnChannel(a_channel, id, a_noInterrupt)) {
					InstanceManager::Stop(id);
					return 0;
				}
			}
			if (a_mouth) {
				LipSync::Start(a_mouth, file, handle, id);
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
			GagState::Resolve(*Config::Get());
			InstanceManager::ApplyConfigGroupVolumes();
			LipSync::ApplyConfig();
			PPABridge::SetEventRateMs(Config::Get()->ppaEventRateMs);
			return ok;
		}

		std::int32_t PlayVoice(RE::StaticFunctionTag*, RE::Actor* a_actor,
			RE::BSFixedString a_category, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel, bool a_blockLipSync)
		{
			const auto settings = Config::Get();
			const auto* slot = ResolveSlotForActor(*settings, a_actor);
			if (!slot) {
				logger::warn("PlayVoice: no slot resolvable for actor");
				return 0;
			}
			auto key = ResolveGaggedKey(*settings, *slot, a_category.c_str(), a_actor);
			if (key.empty()) {
				// last resort: non-voice scene sounds (PullOutGape, Smack, ...) live in
				// the sfx slot / [sfx] table
				key = ResolveSfxKey(*settings, a_category.c_str());
			}
			// no-interrupt early-out: cheap pre-check to avoid building a sound we'd
			// drop (the atomic claim in PlayFromKey closes the check/claim race)
			if (settings->voiceNoInterrupt && a_channel.length() > 0 &&
				InstanceManager::IsChannelBusy(a_channel.c_str())) {
				return 0;
			}
			// 3D-follow only when voice3D is on; either way the mouth actor drives lipsync
			RE::Actor* follow = settings->voice3D ? a_actor : nullptr;
			// per-call opt-out OR a category configured to never lipsync (oral sfx, climax)
			const bool blockLip = a_blockLipSync ||
				settings->lipsyncBlockCategories.contains(Config::Normalize(a_category.c_str()));
			return PlayFromKey(key, follow, a_volume, a_group.c_str(), a_channel.c_str(),
				blockLip ? nullptr : a_actor, settings->voiceNoInterrupt);
		}

		std::int32_t PlayVoiceFromSlot(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category, RE::Actor* a_follow, float a_volume,
			RE::BSFixedString a_group, RE::BSFixedString a_channel, bool a_blockLipSync)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot.c_str());
			if (!slot) {
				logger::warn("PlayVoiceFromSlot: unknown slot '{}'", a_slot.c_str());
				return 0;
			}
			const auto key = ResolveGaggedKey(*settings, *slot, a_category.c_str(), a_follow);
			if (settings->voiceNoInterrupt && a_channel.length() > 0 &&
				InstanceManager::IsChannelBusy(a_channel.c_str())) {
				return 0;
			}
			RE::Actor* follow = settings->voice3D ? a_follow : nullptr;
			const bool blockLip = a_blockLipSync ||
				settings->lipsyncBlockCategories.contains(Config::Normalize(a_category.c_str()));
			return PlayFromKey(key, follow, a_volume, a_group.c_str(), a_channel.c_str(),
				blockLip ? nullptr : a_follow, settings->voiceNoInterrupt);
		}

		std::int32_t PlaySFX(RE::StaticFunctionTag*, RE::BSFixedString a_name,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			const auto key = ResolveSfxKey(*Config::Get(), a_name.c_str());
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

		// does a data-relative path resolve to a real resource (loose or BSA)
		// in the current load order? Confirms the path resolves, not that the
		// audio is valid PCM. Path separators may be '/' or '\'.
		bool FileExists(RE::StaticFunctionTag*, RE::BSFixedString a_path)
		{
			std::string path = a_path.c_str();
			std::replace(path.begin(), path.end(), '/', '\\');
			return AudioEngine::ResourceExists(path);
		}

		// ---------- natives: lipsync ----------

		bool IsLipSyncActive(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			return a_actor && LipSync::IsActiveFor(a_actor);
		}

		void StopLipSync(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			if (a_actor) {
				LipSync::StopFor(a_actor);
			}
		}

		void SetLipSyncEnabled(RE::StaticFunctionTag*, bool a_enabled)
		{
			LipSync::SetEnabled(a_enabled);
		}

		bool IsLipSyncEnabled(RE::StaticFunctionTag*)
		{
			return LipSync::Enabled();
		}

		void SetLipSyncGain(RE::StaticFunctionTag*, float a_gain)
		{
			LipSync::SetGain(a_gain);
		}

		void SetLipSyncBlocked(RE::StaticFunctionTag*, RE::Actor* a_actor, bool a_blocked,
			RE::BSFixedString a_callerMod)
		{
			if (a_actor) {
				logger::info("SetLipSyncBlocked: {} '{}' ({:08X}) by '{}'",
					a_blocked ? "block" : "unblock", a_actor->GetDisplayFullName(),
					a_actor->GetFormID(), a_callerMod.c_str());
				LipSync::SetBlockedFor(a_actor, a_blocked);
			}
		}

		bool IsLipSyncBlocked(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			return a_actor && LipSync::IsBlockedFor(a_actor);
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

		// ---------- natives: TomlUtil (generic consumer-config surface) ----------
		// Registered under its own script class so any mod can read TOML files
		// through AudioUtil's DLL without touching the audio API.

		namespace Toml
		{
			constexpr std::int32_t TOML_API_VERSION = 1;

			std::int32_t GetAPIVersion(RE::StaticFunctionTag*)
			{
				return TOML_API_VERSION;
			}

			std::int32_t GetInt(RE::StaticFunctionTag*, RE::BSFixedString a_file,
				RE::BSFixedString a_key, std::int32_t a_default)
			{
				const auto value = TomlStore::GetInt(a_file.c_str(), a_key.c_str());
				return value ? static_cast<std::int32_t>(*value) : a_default;
			}

			float GetFloat(RE::StaticFunctionTag*, RE::BSFixedString a_file,
				RE::BSFixedString a_key, float a_default)
			{
				const auto value = TomlStore::GetFloat(a_file.c_str(), a_key.c_str());
				return value ? static_cast<float>(*value) : a_default;
			}

			RE::BSFixedString GetString(RE::StaticFunctionTag*, RE::BSFixedString a_file,
				RE::BSFixedString a_key, RE::BSFixedString a_default)
			{
				const auto value = TomlStore::GetString(a_file.c_str(), a_key.c_str());
				return value ? RE::BSFixedString(*value) : a_default;
			}

			bool GetBool(RE::StaticFunctionTag*, RE::BSFixedString a_file,
				RE::BSFixedString a_key, bool a_default)
			{
				const auto value = TomlStore::GetBool(a_file.c_str(), a_key.c_str());
				return value ? *value : a_default;
			}

			std::vector<RE::BSFixedString> GetStringArray(RE::StaticFunctionTag*,
				RE::BSFixedString a_file, RE::BSFixedString a_key)
			{
				std::vector<RE::BSFixedString> out;
				for (const auto& item : TomlStore::GetStringArray(a_file.c_str(), a_key.c_str())) {
					out.emplace_back(item);
				}
				return out;
			}

			bool HasKey(RE::StaticFunctionTag*, RE::BSFixedString a_file, RE::BSFixedString a_key)
			{
				return TomlStore::HasKey(a_file.c_str(), a_key.c_str());
			}

			bool Reload(RE::StaticFunctionTag*, RE::BSFixedString a_file)
			{
				return TomlStore::Reload(a_file.c_str());
			}
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
		REGISTERFUNC(IsLipSyncActive, SCRIPT_NAME);
		REGISTERFUNC(StopLipSync, SCRIPT_NAME);
		REGISTERFUNC(SetLipSyncEnabled, SCRIPT_NAME);
		REGISTERFUNC(IsLipSyncEnabled, SCRIPT_NAME);
		REGISTERFUNC(SetLipSyncGain, SCRIPT_NAME);
		REGISTERFUNC(SetLipSyncBlocked, SCRIPT_NAME);
		REGISTERFUNC(IsLipSyncBlocked, SCRIPT_NAME);
		REGISTERFUNC(GetSlotForActor, SCRIPT_NAME);
		REGISTERFUNC(GetCategoryFileCount, SCRIPT_NAME);
		REGISTERFUNC(CategoryExists, SCRIPT_NAME);
		REGISTERFUNC(FileExists, SCRIPT_NAME);
		REGISTERFUNC(DebugPlayFile, SCRIPT_NAME);
		REGISTERFUNC(IsConnected, PPA_SCRIPT_NAME);
		REGISTERFUNC(SetEventRate, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetContext, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetDepth, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetVaginalOpening, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetAnalOpening, PPA_SCRIPT_NAME);
		a_vm->RegisterFunction("GetAPIVersion"sv, TOML_SCRIPT_NAME, Toml::GetAPIVersion, true);
		a_vm->RegisterFunction("GetInt"sv, TOML_SCRIPT_NAME, Toml::GetInt, true);
		a_vm->RegisterFunction("GetFloat"sv, TOML_SCRIPT_NAME, Toml::GetFloat, true);
		a_vm->RegisterFunction("GetString"sv, TOML_SCRIPT_NAME, Toml::GetString, true);
		a_vm->RegisterFunction("GetBool"sv, TOML_SCRIPT_NAME, Toml::GetBool, true);
		a_vm->RegisterFunction("GetStringArray"sv, TOML_SCRIPT_NAME, Toml::GetStringArray, true);
		a_vm->RegisterFunction("HasKey"sv, TOML_SCRIPT_NAME, Toml::HasKey, true);
		a_vm->RegisterFunction("Reload"sv, TOML_SCRIPT_NAME, Toml::Reload, true);
		return true;
	}
}
