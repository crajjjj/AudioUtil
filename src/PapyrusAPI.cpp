#include "PapyrusAPI.h"

#include <fstream>

#include "AudioEngine.h"
#include "CaptionManager.h"
#include "Config.h"
#include "FolderCache.h"
#include "FuzCache.h"
#include "FuzSlots.h"
#include "GagState.h"
#include "TongueState.h"
#include "InstanceManager.h"
#include "LipCapture.h"
#include "LipSync.h"
#include "PPABridge.h"
#include "Tags.h"
#include "TomlStore.h"

namespace PapyrusAPI
{
	namespace
	{
		constexpr auto SCRIPT_NAME = "AudioUtil";
		constexpr auto PPA_SCRIPT_NAME = "AudioUtilPPA";
		constexpr auto TOML_SCRIPT_NAME = "TomlUtil";
		constexpr auto TEST_SCRIPT_NAME = "AudioUtilTest";  // debug/calibration natives only
		constexpr std::int32_t API_VERSION = 6;  // v6: tag-scored playback (PlayVoiceTagged, PlayVoiceFromSlotTagged)

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

		// a creature = an actor whose race carries no ActorTypeNPC keyword (the
		// engine's own humanoid marker). In vanilla ONLY the playable humanoid races
		// carry it — the ten player races plus their vampire/child variants, Elder,
		// Dremora, Afflicted (verified against Skyrim.esm). Draugr, falmer, werewolves
		// and every beast do NOT, so they all read as creatures here. That is the
		// point: this gates the blind by-sex fallback ONLY, so a race_map'd draugr
		// still resolves at its own step and never notices.
		//
		// The keyword is read straight off the race / actor base. Do NOT reach for
		// RE::TESObjectREFR::IsHumanoid() here (0.9.11-0.9.12 did, and it CTD'd):
		// it funnels into CommonLib's HasKeywordWithType, which does
		//     auto keyword = *dobj->GetObject<BGSKeyword>(keywordType);
		// - an unguarded deref of a T** that GetObject returns as nullptr whenever
		// the kKeywordNPC default-object slot is unset or holds a non-keyword form.
		// That is an EXCEPTION_ACCESS_VIOLATION (`mov rdx, [rax]`, rax = 0) on every
		// single PlayVoice call on an affected install. The keyword walk below
		// answers the same question and never touches the default-object table.
		bool IsCreature(RE::Actor* a_actor)
		{
			constexpr auto ACTOR_TYPE_NPC = "ActorTypeNPC"sv;
			if (const auto* race = a_actor->GetRace()) {
				if (race->HasKeywordString(ACTOR_TYPE_NPC)) {
					return false;
				}
			}
			// second look at the actor base: a mod-added humanoid can carry the
			// keyword on the NPC record rather than on its race
			if (const auto* base = a_actor->GetActorBase()) {
				if (base->HasKeywordString(ACTOR_TYPE_NPC)) {
					return false;
				}
			}
			return true;
		}

		// TESForm::GetFormEditorID() is the GAME's virtual (CommonLib only declares
		// the override) — it hands back the record's raw BSFixedString pointer, which
		// is NULL when the record carries no EDID. CommonLib's null->"" fallback lives
		// only in its own BSFixedString wrapper, so it does not apply here. Feeding
		// that straight to Normalize() builds a std::string_view over nullptr —
		// strlen(0), the same access violation as the IsHumanoid() case above. Every
		// editor-id read goes through this.
		std::string_view SafeEditorID(const char* a_editorID)
		{
			return a_editorID ? std::string_view{ a_editorID } : std::string_view{};
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
				voicetype = Config::Normalize(SafeEditorID(vt->GetFormEditorID()));
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
					raceID = Config::Normalize(SafeEditorID(race->GetFormEditorID()));
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
			//
			// The mirror of that guard: a CREATURE no explicit route claimed takes
			// default_creature_slot and stops, never the by-sex default below — every
			// slot that can reach is a human voice pack, so an unmapped frostbite
			// spider or fox would otherwise speak human lines. Unset = silent.
			if (IsCreature(a_actor)) {
				if (const auto* creatureSlot =
						Config::FindSlot(a_settings, a_settings.defaultCreatureSlot);
					usable(creatureSlot)) {
					return creatureSlot;
				}
				// GetName() bottoms out in a game virtual that returns null for a
				// nameless form — and unnamed actors are exactly what this branch
				// logs. Formatting a null const char* is UB in fmt, so never hand
				// it over raw.
				const char* name = a_actor->GetName();
				logger::debug("no slot for unrouted creature '{}' — add a [race_map] hint "
							  "or set [general] default_creature_slot to voice it",
					name ? name : "<unnamed>");
				return nullptr;
			}

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
			const Config::Slot& a_slot, std::string_view a_category, RE::Actor* a_actor,
			Tags::Mask a_facts = 0)
		{
			const Config::Slot* slot = &a_slot;
			if (a_settings.gagEnabled && !a_slot.gagSlot.empty() && GagState::IsGagged(a_actor)) {
				if (const auto* gagSlot = Config::FindSlot(a_settings, a_slot.gagSlot)) {
					slot = gagSlot;
				}
			}
			auto key = FolderCache::ResolveVoiceKey(a_settings, *slot, a_category, a_facts);
			if (key.empty() && slot != &a_slot && !a_settings.gagDefaultCategory.empty()) {
				key = FolderCache::ResolveVoiceKey(a_settings, *slot, a_settings.gagDefaultCategory, a_facts);
			}
			return key;
		}

		// resolve an sfx name to a folder key: first as a category of the dedicated
		// sfx slot (id defaults to "SFX0"), so sfx pools get the full [[slot]]
		// toolset — explicit file lists (BSA-capable), folder refs, or a scanned
		// path — then the legacy flat [sfx] table. Direct key lookups (no
		// alias/fallback), so a name only in the [sfx] table doesn't trip the
		// voice resolver's miss warning.
		// the sfx slot is a real [[slot]], so its categories are tag-scanned and
		// may hold only tagged pools. Gating on the tag-blind file count would
		// hand back a key PickNext(key, facts) can't satisfy — handle 0, no log,
		// no fallback. Gate on what the pick will actually see, and say so once
		// when a category exists but is tagged past the request.
		std::string ResolveSfxKey(const Config::Settings& a_settings, std::string_view a_name,
			Tags::Mask a_facts = 0)
		{
			const auto catNorm = Config::Normalize(a_name);
			const auto usable = [&](const std::string& a_key) {
				if (FolderCache::HasPlayableFiles(a_key, a_facts)) {
					return true;
				}
				if (FolderCache::FileCount(a_key) > 0) {
					static std::unordered_set<std::string> warned;
					static std::mutex                      warnedLock;
					std::scoped_lock lock{ warnedLock };
					if (warned.insert(a_key + "|" + std::format("{:x}", a_facts)).second) {
						logger::warn("SFX '{}': every pool is tagged beyond this request's facts [{}] — "
						             "nothing qualifies to play",
							a_key, Tags::Describe(a_facts));
					}
				}
				return false;
			};
			if (!a_settings.sfxSlot.empty()) {
				const auto slotKey = Config::Normalize(a_settings.sfxSlot) + "/" + catNorm;
				if (usable(slotKey)) {
					return slotKey;
				}
			}
			const auto sfxKey = "sfx/" + catNorm;
			return usable(sfxKey) ? sfxKey : std::string{};
		}

		// ---------- shared play helper ----------

		// a_mouth: actor whose lips follow the clip's amplitude (voice calls pass
		// the speaker; sfx/folder/file playback passes nullptr).
		// a_speaker: actor a caption sidecar is attributed to — the raw speaker,
		// independent of the voice_3d follow gating and the lipsync block (a
		// mouth-still line still shows its caption)
		std::int32_t PlayFromKey(const std::string& a_folderKey, RE::Actor* a_follow,
			float a_volume, const std::string& a_group, const std::string& a_channel,
			RE::Actor* a_mouth = nullptr, bool a_noInterrupt = false,
			RE::Actor* a_speaker = nullptr, Tags::Mask a_facts = 0,
			bool a_blockCaption = false)
		{
			if (a_folderKey.empty()) {
				return 0;
			}
			const auto file = FolderCache::PickNext(a_folderKey, a_facts);
			if (file.empty()) {
				return 0;
			}
			InstanceManager::SweepNow();  // free slots of lines that already stopped
			int slot = -1;
			auto handle = AudioEngine::PlayPath(file, a_follow, a_volume, &slot);
			if (!handle.IsValid()) {
				return 0;  // PlayPath released any acquired slot on failure
			}
			const auto id = InstanceManager::Register(handle, a_volume, a_group, file, a_follow, slot);
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
			if (!a_blockCaption) {
				CaptionManager::Start(a_speaker ? a_speaker : a_follow, file, handle, id);
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
			Tags::Configure(*Config::Get());
			FolderCache::Rebuild();
			GagState::Resolve(*Config::Get());
			TongueState::Resolve(*Config::Get());
			InstanceManager::ApplyConfigGroupVolumes();
			LipSync::ApplyConfig();
			CaptionManager::ApplyConfig();
			FuzCache::EnforceCacheCap();
			PPABridge::SetEventRateMs(Config::Get()->ppaEventRateMs);
			return ok;
		}

		// shared body of PlayVoice / PlayVoiceTagged: identical except for the
		// tag fact set threaded into resolution + pool selection (0 =
		// untagged-only = the pre-tags behavior, so there is ONE code path, not
		// a per-variation branch)
		std::int32_t PlayVoiceImpl(RE::Actor* a_actor, const char* a_category,
			Tags::Mask a_facts, float a_volume, const char* a_group,
			const char* a_channel, bool a_blockLipSync, bool a_blockCaption = false)
		{
			const auto settings = Config::Get();
			const auto* slot = ResolveSlotForActor(*settings, a_actor);
			if (!slot) {
				logger::warn("PlayVoice: no slot resolvable for actor");
				return 0;
			}
			auto key = ResolveGaggedKey(*settings, *slot, a_category, a_actor, a_facts);
			if (key.empty()) {
				// last resort: non-voice scene sounds (PullOutGape, Smack, ...) live in
				// the sfx slot / [sfx] table. The facts ride along — the sfx slot is a
				// real [[slot]], so its categories can carry tag pools too, and
				// PlayFromKey picks with these same facts.
				key = ResolveSfxKey(*settings, a_category, a_facts);
			}
			// no-interrupt early-out: cheap pre-check to avoid building a sound we'd
			// drop (the atomic claim in PlayFromKey closes the check/claim race)
			if (settings->voiceNoInterrupt && a_channel[0] != '\0' &&
				InstanceManager::IsChannelBusy(a_channel)) {
				return 0;
			}
			// 3D-follow only when voice3D is on; either way the mouth actor drives lipsync
			RE::Actor* follow = settings->voice3D ? a_actor : nullptr;
			// per-call opt-out OR a category configured to never lipsync (oral sfx, climax)
			const bool blockLip = a_blockLipSync ||
				settings->lipsyncBlockCategories.contains(Config::Normalize(a_category));
			return PlayFromKey(key, follow, a_volume, a_group, a_channel,
				blockLip ? nullptr : a_actor, settings->voiceNoInterrupt, a_actor, a_facts,
				a_blockCaption);
		}

		std::int32_t PlayVoice(RE::StaticFunctionTag*, RE::Actor* a_actor,
			RE::BSFixedString a_category, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel, bool a_blockLipSync)
		{
			return PlayVoiceImpl(a_actor, a_category.c_str(), 0, a_volume,
				a_group.c_str(), a_channel.c_str(), a_blockLipSync);
		}

		// tagged variant: same as PlayVoice plus a fact string ("rcv victim intense") —
		// the scene truths this line may be matched against. Tagged pools whose
		// constraints the facts cover compete tone-first; no coverage = the
		// untagged pool; no untagged pool either = normal category fallbacks.
		std::int32_t PlayVoiceTagged(RE::StaticFunctionTag*, RE::Actor* a_actor,
			RE::BSFixedString a_category, RE::BSFixedString a_tags, float a_volume,
			RE::BSFixedString a_group, RE::BSFixedString a_channel, bool a_blockLipSync,
			bool a_blockCaption)
		{
			return PlayVoiceImpl(a_actor, a_category.c_str(), Tags::ParseFacts(a_tags.c_str()),
				a_volume, a_group.c_str(), a_channel.c_str(), a_blockLipSync, a_blockCaption);
		}

		std::int32_t PlayVoiceFromSlotImpl(const char* a_slot, const char* a_category,
			Tags::Mask a_facts, RE::Actor* a_follow, float a_volume,
			const char* a_group, const char* a_channel, bool a_blockLipSync,
			bool a_blockCaption = false)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot);
			if (!slot) {
				logger::warn("PlayVoiceFromSlot: unknown slot '{}'", a_slot);
				return 0;
			}
			const auto key = ResolveGaggedKey(*settings, *slot, a_category, a_follow, a_facts);
			if (settings->voiceNoInterrupt && a_channel[0] != '\0' &&
				InstanceManager::IsChannelBusy(a_channel)) {
				return 0;
			}
			RE::Actor* follow = settings->voice3D ? a_follow : nullptr;
			const bool blockLip = a_blockLipSync ||
				settings->lipsyncBlockCategories.contains(Config::Normalize(a_category));
			return PlayFromKey(key, follow, a_volume, a_group, a_channel,
				blockLip ? nullptr : a_follow, settings->voiceNoInterrupt, a_follow, a_facts,
				a_blockCaption);
		}

		std::int32_t PlayVoiceFromSlot(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category, RE::Actor* a_follow, float a_volume,
			RE::BSFixedString a_group, RE::BSFixedString a_channel, bool a_blockLipSync)
		{
			return PlayVoiceFromSlotImpl(a_slot.c_str(), a_category.c_str(), 0, a_follow,
				a_volume, a_group.c_str(), a_channel.c_str(), a_blockLipSync);
		}

		std::int32_t PlayVoiceFromSlotTagged(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category, RE::BSFixedString a_tags, RE::Actor* a_follow,
			float a_volume, RE::BSFixedString a_group, RE::BSFixedString a_channel,
			bool a_blockLipSync, bool a_blockCaption)
		{
			return PlayVoiceFromSlotImpl(a_slot.c_str(), a_category.c_str(),
				Tags::ParseFacts(a_tags.c_str()), a_follow, a_volume,
				a_group.c_str(), a_channel.c_str(), a_blockLipSync, a_blockCaption);
		}

		std::int32_t PlaySFX(RE::StaticFunctionTag*, RE::BSFixedString a_name,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			const auto key = ResolveSfxKey(*Config::Get(), a_name.c_str());
			return PlayFromKey(key, a_follow, a_volume, a_group.c_str(), a_channel.c_str());
		}

		// original semantics, untouched: never drives the mouth (PlayFile also
		// serves non-vocal one-shots, and existing consumers rely on that)
		std::int32_t PlayFile(RE::StaticFunctionTag*, RE::BSFixedString a_path,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			return PlayFileByPath(a_path.c_str(), a_follow, a_volume,
				a_group.c_str(), a_channel.c_str(), false);
		}

		// spoken-line variant: same as PlayFile plus voice-call lipsync (global
		// [lipsync] toggle + gag/tongue/dialogue guards apply; PCM wav or fuz)
		std::int32_t PlayFileWithLipSync(RE::StaticFunctionTag*, RE::BSFixedString a_path,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			return PlayFileByPath(a_path.c_str(), a_follow, a_volume,
				a_group.c_str(), a_channel.c_str(), true);
		}

		std::int32_t PlayFolder(RE::StaticFunctionTag*, RE::BSFixedString a_folder,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			const auto key = FolderCache::ResolveDirKey(a_folder.c_str());
			return PlayFromKey(key, a_follow, a_volume, a_group.c_str(), a_channel.c_str());
		}

		// spoken-line variant of PlayFolder: the shuffle-bag pick drives
		// a_follow's mouth like a voice call (same toggle + guards)
		std::int32_t PlayFolderWithLipSync(RE::StaticFunctionTag*, RE::BSFixedString a_folder,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel)
		{
			const auto key = FolderCache::ResolveDirKey(a_folder.c_str());
			return PlayFromKey(key, a_follow, a_volume, a_group.c_str(), a_channel.c_str(),
				a_follow);
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

		// data-relative path of the file this handle played — the exact shuffle-bag
		// pick, so a script can read back which clip a Play* call actually chose.
		// "" for a dead/unknown handle. Valid until the instance is swept (handle
		// stops playing) — read it right after the Play* call returns.
		RE::BSFixedString GetHandlePath(RE::StaticFunctionTag*, std::int32_t a_handle)
		{
			return a_handle > 0 ? RE::BSFixedString(InstanceManager::InstancePath(a_handle)) : "";
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

		// Optional per-slot schema label: "B" for a pack using the alternate
		// folder/category layout, "A" otherwise (default, and for unknown slots).
		// AudioUtil does not interpret it - consumers gate their own routing on it.
		RE::BSFixedString GetSlotVariation(RE::StaticFunctionTag*, RE::BSFixedString a_slot)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot.c_str());
			return (slot && slot->variation == "B") ? "B" : "A";
		}

		std::int32_t GetCategoryFileCount(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot.c_str());
			if (!slot) {
				return 0;
			}
			// kAllFacts: introspection keeps its legacy meaning — "does this
			// category hold ANY content" — even when all of it is tagged
			const auto key = FolderCache::ResolveVoiceKey(*settings, *slot, a_category.c_str(), Tags::kAllFacts);
			return key.empty() ? 0 : FolderCache::FileCount(key);
		}

		bool CategoryExists(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category)
		{
			return GetCategoryFileCount(nullptr, a_slot, a_category) > 0;
		}

		// Which slot in the fallback chain actually supplies the audio for
		// slot/category: the queried slot itself (the pack voices it), one of its
		// fallback slots (backfill), or "" when nothing resolves. Same resolution
		// PlayVoiceFromSlot uses — lets a consumer audit whether a category would
		// really play from the pack or lean on stock backfill.
		RE::BSFixedString GetResolvingSlot(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot.c_str());
			if (!slot) {
				return "";
			}
			const auto key = FolderCache::ResolveVoiceKey(*settings, *slot, a_category.c_str(), Tags::kAllFacts);
			if (key.empty()) {
				return "";
			}
			// voice keys are "<normalized slot id>/<category>"; walk the same
			// fallback chain ResolveVoiceKey walked (same hop cap) to map the
			// prefix back to a display slot id
			const auto prefix = key.substr(0, key.find('/'));
			const Config::Slot* hopSlot = slot;
			for (int hop = 0; hopSlot != nullptr && hop < 4; ++hop) {
				if (Config::Normalize(hopSlot->id) == prefix) {
					return hopSlot->id.c_str();
				}
				hopSlot = Config::FindSlot(*settings, hopSlot->fallbackSlot);
			}
			return slot->id.c_str();
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

		// Opt a playing instance into lipsync after the fact: drive akActor's
		// mouth from the clip's loudness, exactly like a PlayVoice line. For
		// PlayFile/PlayFolder callers whose file IS a spoken line (their play
		// calls never lipsync on their own - they also serve non-vocal
		// one-shots). Works for loose PCM wav and fuz (decoded cache). No-op
		// for a dead/unknown handle or unreadable audio.
		void StartLipSync(RE::StaticFunctionTag*, RE::Actor* a_actor, std::int32_t a_handle)
		{
			if (!a_actor || a_handle <= 0) {
				return;
			}
			const auto handle = InstanceManager::InstanceHandle(a_handle);
			if (!handle.IsValid()) {
				return;
			}
			LipSync::Start(a_actor, InstanceManager::InstancePath(a_handle), handle, a_handle);
		}

		void StopLipSync(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			logger::debug("StopLipSync called for {:08X}", a_actor ? a_actor->GetFormID() : 0);
			if (a_actor) {
				LipSync::StopFor(a_actor);
			}
		}

		void SetLipSyncEnabled(RE::StaticFunctionTag*, bool a_enabled)
		{
			logger::debug("SetLipSyncEnabled({}) called", a_enabled);
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

		// ---------- natives: captions ----------

		// caption text of the file this handle played, resolved for the current
		// [captions] language ("" if the wav has no sidecar / no usable key).
		// Works regardless of enable/hud, so a consumer can render captions
		// itself with hud = false.
		RE::BSFixedString GetHandleCaption(RE::StaticFunctionTag*, std::int32_t a_handle)
		{
			if (a_handle <= 0) {
				return "";
			}
			return RE::BSFixedString(
				CaptionManager::TextForFile(InstanceManager::InstancePath(a_handle)));
		}

		void SetCaptionsEnabled(RE::StaticFunctionTag*, bool a_enabled)
		{
			CaptionManager::SetEnabled(a_enabled);
		}

		bool AreCaptionsEnabled(RE::StaticFunctionTag*)
		{
			return CaptionManager::Enabled();
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
			// raw path on purpose (no fuz->payload translation): lets diagnostics
			// feed a .fuz straight to the engine to probe its native handling
			auto handle = AudioEngine::PlayPath(a_path.c_str(), a_follow, 1.0f,
				static_cast<std::uint32_t>(a_flags), static_cast<std::uint32_t>(a_priority), false);
			if (!handle.IsValid()) {
				return 0;
			}
			return InstanceManager::Register(handle, 1.0f, "", a_path.c_str(), a_follow);
		}

		// lip-calibration capture (see LipCapture.h) — registered on the TEST
		// script class so the public AudioUtil API surface stays untouched
		bool StartLipCapture(RE::StaticFunctionTag*)
		{
			return LipCapture::Start();
		}

		RE::BSFixedString StopLipCapture(RE::StaticFunctionTag*)
		{
			return LipCapture::Stop();
		}

		bool IsLipCapturing(RE::StaticFunctionTag*)
		{
			return LipCapture::IsActive();
		}

		// Decode-only prewarm: unpack every .fuz in a folder into the fuz cache
		// (Data\Sound\AudioUtilFuzCache\) WITHOUT playing anything. Needed under
		// MO2: a cache wav written mid-session is invisible to the engine's
		// loose-file resource index (frozen at launch), so a first-time fuz plays
		// silent until the next game start. Run this once over a fuz folder, then
		// restart — every line is then in the launch-time index and plays first
		// try. Returns the number of fuz successfully decoded/cached.
		std::int32_t PrewarmFolder(RE::StaticFunctionTag*, RE::BSFixedString a_folder)
		{
			const auto start = std::chrono::steady_clock::now();
			const auto files = FolderCache::ListFolder(a_folder.c_str());
			std::int32_t decoded = 0;
			std::int32_t fuz = 0;
			for (const auto& f : files) {
				if (!FuzCache::IsFuzPath(f)) {
					continue;
				}
				++fuz;
				if (!FuzCache::Resolve(f).empty()) {
					++decoded;
				}
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();
			logger::info("PrewarmFolder '{}': {} of {} fuz decoded/cached ({} files scanned) in "
						 "{} ms ({} ms/fuz) — restart so the new cache wavs enter the resource index",
				a_folder.c_str(), decoded, fuz, files.size(), ms, fuz ? ms / fuz : 0);
			return decoded;
		}

		// B-experiment probe: overwrite dst's on-disk bytes with src's (both
		// data-relative wavs). Tests whether the engine RE-READS a file on each
		// BuildSoundDataFromFile, or caches decoded audio by resource id. Use a dst
		// that existed at THIS session's launch (a prior-session cache wav), so it's
		// in the resource index. Procedure:
		//   autest play  <dst>                 -> hear dst's line (baseline)
		//   autest boverwrite <dst> <src>      -> dst's bytes become src's line
		//   autest play  <dst>                 -> SAME path again:
		//     hear SRC's line  => engine re-reads    => placeholder-overwrite player VIABLE
		//     hear DST's line  => engine caches by id => not viable
		bool BOverwriteFile(RE::StaticFunctionTag*, RE::BSFixedString a_dst, RE::BSFixedString a_src)
		{
			const auto bytes = FuzCache::ReadResourceBytes(a_src.c_str());
			if (bytes.empty()) {
				logger::warn("boverwrite: could not read src '{}'", a_src.c_str());
				return false;
			}
			std::string dst = a_dst.c_str();
			std::replace(dst.begin(), dst.end(), '/', '\\');
			const auto path = std::filesystem::current_path() / "Data" / dst;
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out) {
				logger::warn("boverwrite: could not open dst '{}' for write", dst);
				return false;
			}
			out.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			out.close();
			logger::info("boverwrite: wrote {} bytes of '{}' over '{}'. Now `autest play \"{}\"` — "
						 "src's line = engine re-reads (B viable); dst's line = cached (B dead).",
				bytes.size(), a_src.c_str(), dst, dst);
			return static_cast<bool>(out);
		}

		// Nth .wav (sorted) in the fuz cache folder, as a data-relative path, or ""
		// — lets the short no-arg B-experiment commands name files without the
		// caller typing a long hash path (Skyrim's console caps input length).
		RE::BSFixedString BCacheFile(RE::StaticFunctionTag*, std::int32_t a_index)
		{
			namespace fs = std::filesystem;
			const auto dir = fs::current_path() / "Data" / "Sound" / "AudioUtilFuzCache";
			std::error_code ec;
			std::vector<std::string> wavs;
			if (fs::is_directory(dir, ec)) {
				for (const auto& e : fs::directory_iterator(dir, ec)) {
					if (e.is_regular_file(ec) &&
						_stricmp(e.path().extension().string().c_str(), ".wav") == 0 &&
						!FuzSlots::IsSlotName(e.path().filename().string())) {  // not the placeholders
						wavs.push_back(e.path().filename().string());
					}
				}
			}
			std::sort(wavs.begin(), wavs.end());
			if (a_index < 0 || a_index >= static_cast<std::int32_t>(wavs.size())) {
				return RE::BSFixedString{ "" };
			}
			return RE::BSFixedString{ ("Sound\\AudioUtilFuzCache\\" + wavs[a_index]).c_str() };
		}

		// runtime toggle for .lip-driven phoneme lipsync (A/B against envelope)
		void SetLipFilesMode(RE::StaticFunctionTag*, bool a_enabled)
		{
			LipSync::SetLipFilesEnabled(a_enabled);
		}

		bool GetLipFilesMode(RE::StaticFunctionTag*)
		{
			return LipSync::LipFilesEnabled();
		}

		// runtime toggle for envelope->pseudo-phoneme synthesis (lines without lip data)
		void SetPseudoLipMode(RE::StaticFunctionTag*, bool a_enabled)
		{
			LipSync::SetPseudoLipEnabled(a_enabled);
		}

		bool GetPseudoLipMode(RE::StaticFunctionTag*)
		{
			return LipSync::PseudoLipEnabled();
		}

		// mouth timing lead calibration (ms, applies to live entries too)
		void SetLipLeadMs(RE::StaticFunctionTag*, std::int32_t a_ms)
		{
			LipSync::SetLeadMs(a_ms);
		}

		std::int32_t GetLipLeadMs(RE::StaticFunctionTag*)
		{
			return LipSync::LeadMs();
		}

		// ---------- natives: TomlUtil (generic consumer-config surface) ----------
		// Registered under its own script class so any mod can read TOML files
		// through AudioUtil's DLL without touching the audio API.

		namespace Toml
		{
			constexpr std::int32_t TOML_API_VERSION = 2;  // v2: added SetInt/SetFloat/SetString/SetBool

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

			// comment-preserving writers (see TomlStore/TomlEdit): the value is
			// spliced into the file text in place, validated, persisted, and the
			// cache refreshed - false = nothing was written

			bool SetInt(RE::StaticFunctionTag*, RE::BSFixedString a_file,
				RE::BSFixedString a_key, std::int32_t a_value)
			{
				return TomlStore::SetInt(a_file.c_str(), a_key.c_str(), a_value);
			}

			bool SetFloat(RE::StaticFunctionTag*, RE::BSFixedString a_file,
				RE::BSFixedString a_key, float a_value)
			{
				// route through the float's shortest decimal form: a raw
				// float->double promotion would serialize 0.1 as 0.10000000149...
				const auto value = std::strtod(std::format("{}", a_value).c_str(), nullptr);
				return TomlStore::SetFloat(a_file.c_str(), a_key.c_str(), value);
			}

			bool SetString(RE::StaticFunctionTag*, RE::BSFixedString a_file,
				RE::BSFixedString a_key, RE::BSFixedString a_value)
			{
				return TomlStore::SetString(a_file.c_str(), a_key.c_str(), a_value.c_str());
			}

			bool SetBool(RE::StaticFunctionTag*, RE::BSFixedString a_file,
				RE::BSFixedString a_key, bool a_value)
			{
				return TomlStore::SetBool(a_file.c_str(), a_key.c_str(), a_value);
			}
		}
	}

	// shared body of PlayFile / PlayFileWithLipSync (the two natives differ only
	// in whether the file is treated as a spoken line); public so the C exports
	// in AudioUtilAPI.cpp run the exact same code path as the Papyrus natives
	std::int32_t PlayFileByPath(const char* a_dataRelPath, RE::Actor* a_follow,
		float a_volume, const char* a_group, const char* a_channel, bool a_lipSync)
	{
		std::string path = a_dataRelPath;
		std::replace(path.begin(), path.end(), '/', '\\');
		InstanceManager::SweepNow();  // free slots of lines that already stopped
		int slot = -1;
		auto handle = AudioEngine::PlayPath(path, a_follow, a_volume, &slot);
		if (!handle.IsValid()) {
			return 0;  // PlayPath released any acquired slot on failure
		}
		const auto id = InstanceManager::Register(handle, a_volume, a_group, path, a_follow, slot);
		if (*a_channel) {
			InstanceManager::PlayOnChannel(a_channel, id);
		}
		// explicit files caption too when a sidecar exists (needs an actor
		// to attribute the subtitle to)
		CaptionManager::Start(a_follow, path, handle, id);
		if (a_lipSync && a_follow) {
			LipSync::Start(a_follow, path, handle, id);
		}
		return id;
	}

	bool RegisterFuncs(VM* a_vm)
	{
		REGISTERFUNC(GetAPIVersion, SCRIPT_NAME);
		REGISTERFUNC(ReloadConfig, SCRIPT_NAME);
		REGISTERFUNC(PlayVoice, SCRIPT_NAME);
		REGISTERFUNC(PlayVoiceTagged, SCRIPT_NAME);
		REGISTERFUNC(PlayVoiceFromSlot, SCRIPT_NAME);
		REGISTERFUNC(PlayVoiceFromSlotTagged, SCRIPT_NAME);
		REGISTERFUNC(PlaySFX, SCRIPT_NAME);
		REGISTERFUNC(PlayFile, SCRIPT_NAME);
		REGISTERFUNC(PlayFileWithLipSync, SCRIPT_NAME);
		REGISTERFUNC(PlayFolder, SCRIPT_NAME);
		REGISTERFUNC(PlayFolderWithLipSync, SCRIPT_NAME);
		REGISTERFUNC(IsHandlePlaying, SCRIPT_NAME);
		REGISTERFUNC(StopHandle, SCRIPT_NAME);
		REGISTERFUNC(GetHandleDuration, SCRIPT_NAME);
		REGISTERFUNC(GetHandlePath, SCRIPT_NAME);
		REGISTERFUNC(SetHandleVolume, SCRIPT_NAME);
		REGISTERFUNC(SetGroupVolume, SCRIPT_NAME);
		REGISTERFUNC(DuckGroup, SCRIPT_NAME);
		REGISTERFUNC(UnduckGroup, SCRIPT_NAME);
		REGISTERFUNC(StopGroup, SCRIPT_NAME);
		REGISTERFUNC(StopAllAudio, SCRIPT_NAME);
		REGISTERFUNC(StopChannel, SCRIPT_NAME);
		REGISTERFUNC(IsLipSyncActive, SCRIPT_NAME);
		REGISTERFUNC(StartLipSync, SCRIPT_NAME);
		REGISTERFUNC(StopLipSync, SCRIPT_NAME);
		REGISTERFUNC(SetLipSyncEnabled, SCRIPT_NAME);
		REGISTERFUNC(IsLipSyncEnabled, SCRIPT_NAME);
		REGISTERFUNC(SetLipSyncGain, SCRIPT_NAME);
		REGISTERFUNC(GetHandleCaption, SCRIPT_NAME);
		REGISTERFUNC(SetCaptionsEnabled, SCRIPT_NAME);
		REGISTERFUNC(AreCaptionsEnabled, SCRIPT_NAME);
		REGISTERFUNC(GetSlotForActor, SCRIPT_NAME);
		REGISTERFUNC(GetSlotVariation, SCRIPT_NAME);
		REGISTERFUNC(GetCategoryFileCount, SCRIPT_NAME);
		REGISTERFUNC(CategoryExists, SCRIPT_NAME);
		REGISTERFUNC(GetResolvingSlot, SCRIPT_NAME);
		REGISTERFUNC(FileExists, SCRIPT_NAME);
		REGISTERFUNC(DebugPlayFile, SCRIPT_NAME);
		REGISTERFUNC(StartLipCapture, TEST_SCRIPT_NAME);
		REGISTERFUNC(StopLipCapture, TEST_SCRIPT_NAME);
		REGISTERFUNC(IsLipCapturing, TEST_SCRIPT_NAME);
		REGISTERFUNC(PrewarmFolder, TEST_SCRIPT_NAME);
		REGISTERFUNC(BOverwriteFile, TEST_SCRIPT_NAME);
		REGISTERFUNC(BCacheFile, TEST_SCRIPT_NAME);
		REGISTERFUNC(SetLipFilesMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetLipFilesMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(SetPseudoLipMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetPseudoLipMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(SetLipLeadMs, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetLipLeadMs, TEST_SCRIPT_NAME);
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
		a_vm->RegisterFunction("SetInt"sv, TOML_SCRIPT_NAME, Toml::SetInt, true);
		a_vm->RegisterFunction("SetFloat"sv, TOML_SCRIPT_NAME, Toml::SetFloat, true);
		a_vm->RegisterFunction("SetString"sv, TOML_SCRIPT_NAME, Toml::SetString, true);
		a_vm->RegisterFunction("SetBool"sv, TOML_SCRIPT_NAME, Toml::SetBool, true);
		return true;
	}
}
