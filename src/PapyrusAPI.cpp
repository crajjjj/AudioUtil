#include "PapyrusAPI.h"

#include <fstream>

#include "AudioEngine.h"
#include "CaptionManager.h"
#include "Config.h"
#include "FolderCache.h"
#include "GagState.h"
#include "TongueState.h"
#include "InstanceManager.h"
#include "LipCapture.h"
#include "LipSync.h"
#include "MouthClaim.h"
#include "PPABridge.h"
#include "Tags.h"
#include "TomlStore.h"
#include "VoiceLog.h"

namespace PapyrusAPI
{
	namespace
	{
		constexpr auto SCRIPT_NAME = "AudioUtil";
		constexpr auto PPA_SCRIPT_NAME = "AudioUtilPPA";
		constexpr auto TOML_SCRIPT_NAME = "TomlUtil";
		constexpr auto TEST_SCRIPT_NAME = "AudioUtilTest";  // debug/calibration natives only
		constexpr std::int32_t API_VERSION = 11;  // v11: Ex play natives with the dialogueGuard switch (v10: PPA one-call snapshot, v9: penetration site, v8: mouth claims)

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
		constexpr auto ACTOR_TYPE_NPC = "ActorTypeNPC"sv;

		bool IsHumanoidRace(const RE::TESRace* a_race)
		{
			return a_race && a_race->HasKeywordString(ACTOR_TYPE_NPC);
		}

		bool IsCreature(RE::Actor* a_actor)
		{
			if (IsHumanoidRace(a_actor->GetRace())) {
				return false;
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

		// BEAST FORM: a humanoid NPC switched into a creature race (werewolf,
		// werebear, vampire lord). The race on his NPC record cannot tell: the game's
		// race switch (Actor::SwitchRace) calls the record's race setter, which
		// overwrites TESNPC::race with the new race, then copies the record's race
		// into the actor. So in werewolf form both race pointers are the werewolf.
		// TESNPC::originalRace remembers the human: the setter fills it only while
		// it is empty, so it holds the FIRST race of a chain (Nord, then vampire,
		// then vampire lord leaves it at Nord), and clears it on a switch back to
		// exactly that race (read from the 1.7.104 exe: SwitchRace 0x1406ad970,
		// setter 0x1403be3c0). Humanoid is judged like IsCreature does: the race's
		// keyword or the NPC record's.
		// Known limits: originalRace lives on the NPC record, so two transformed
		// copies of one plain base share it, and the first to revert clears it for
		// both. A read during the switch itself can land on either side of it. Each
		// costs a line or two in the voice the actor is leaving.
		bool IsBeastForm(const RE::TESNPC* a_base, const RE::TESRace* a_liveRace)
		{
			if (!a_base->originalRace || !a_liveRace || IsHumanoidRace(a_liveRace)) {
				return false;
			}
			return IsHumanoidRace(a_base->originalRace) || a_base->HasKeywordString(ACTOR_TYPE_NPC);
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

		// resolution order, numbered as in docs/config/resolution.md: 1. PC
		// reservation (player only) -> 2. npc_overrides -> 3. voicetype remap/map ->
		// 4. race match -> 5. default_creature_slot (creatures and humanoids in beast
		// form: they stop here) -> 6. default by sex. A humanoid in BEAST FORM takes
		// 4 before 3 (see there). Non-player actors never resolve to a PC-reserved
		// slot; if the default is reserved they get the first free slot of their
		// sex instead.
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

			// 1. the player speaks with their reserved slot when one is configured. This
			// wins in beast form too: a reservation is the player's voice, always
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

			// 2. per-NPC override (an explicit pin wins, even to a reserved slot)
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

			// 3. voicetype remap -> map, then 4. the race match. A humanoid in BEAST
			// FORM (see IsBeastForm) takes the race match first: his voice type is the
			// one on his human NPC record. SLO VE's bandit captain carried MaleBandit
			// into a werewolf scene, took the M4 male pack, and his "Breathing"
			// ambience fell through that pack's fallbacks to its climax cries every few
			// seconds, although the preset had a Werewolf race hint. A hint for the
			// beast race is how a preset says "voice this form as a creature". Without
			// one he keeps his own voice type, as a vampire lord does in the game, and
			// ends at default_creature_slot below when that routes nowhere either.
			const auto* liveRace = a_actor->GetRace();
			const bool beastForm = IsBeastForm(base, liveRace);

			const auto byVoicetype = [&]() -> const Config::Slot* {
				std::string voicetype;
				if (const auto* vt = base->GetVoiceType()) {
					voicetype = Config::Normalize(SafeEditorID(vt->GetFormEditorID()));
				}
				if (voicetype.empty()) {
					return nullptr;
				}
				if (a_settings.voicetypeRemapEnabled) {
					if (const auto it = a_settings.voicetypeRemap.find(voicetype);
						it != a_settings.voicetypeRemap.end()) {
						voicetype = it->second;
					}
				}
				if (const auto it = a_settings.voicetypeMap.find(voicetype);
					it != a_settings.voicetypeMap.end()) {
					return PickFromSlotList(a_settings, it->second, base, usable);
				}
				return nullptr;
			};

			// first [race_map] hint (most specific first) found in the actor's live
			// race editor id ("nord" matches NordRace / NordRaceVampire); only
			// same-sex slots qualify
			const auto byRace = [&]() -> const Config::Slot* {
				if (a_settings.raceMap.empty() || !liveRace) {
					return nullptr;
				}
				const std::string raceID = Config::Normalize(SafeEditorID(liveRace->GetFormEditorID()));
				if (raceID.empty()) {
					return nullptr;
				}
				// a race-mapped slot qualifies if it matches the actor's sex OR is
				// sex-neutral ('A'): this is what lets one creature slot serve a
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
				return nullptr;
			};

			if (beastForm) {
				if (const auto* slot = byRace()) {
					return slot;
				}
				if (const auto* slot = byVoicetype()) {
					return slot;
				}
			} else {
				if (const auto* slot = byVoicetype()) {
					return slot;
				}
				if (const auto* slot = byRace()) {
					return slot;
				}
			}

			// 6. default by sex; if reserved (or missing), first free slot of the sex.
			// This blind scan is F/M only — 'A' (sex-neutral) slots are reached only
			// by explicit routing, so an sfx/creature slot never leaks onto a
			// voiceless human here. (default_*_slot may still name an 'A' slot: that
			// goes through FindSlot above, not this scan.)
			//
			// 5. The mirror of that guard: a CREATURE no explicit route claimed takes
			// default_creature_slot and stops, never the by-sex default below: every
			// slot that can reach is a human voice pack, so an unmapped frostbite
			// spider or fox would otherwise speak human lines. Unset = silent. A
			// humanoid in beast form is a creature here whatever his NPC record says.
			if (beastForm || IsCreature(a_actor)) {
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
		// a_log, when given, makes this play a line in the pack-author voice log —
		// the request it came from is known only to the callers above, and the file
		// it landed on only down here, so the record is stitched at this seam.
		// a_voice: a spoken line (the PlayVoice family, PlayFolderWithLipSync), which
		// gets the game's Voice volume; decided by the caller, not by whether an
		// actor came along, so a 2D voice line gets it too.
		// a_dialogueGuard = false: the mouth lipsyncs even while a_mouth talks to the
		// player (the Ex natives' dialogueGuard argument; see LipSync::Start).
		std::int32_t PlayFromKey(const std::string& a_folderKey, RE::Actor* a_follow,
			float a_volume, const std::string& a_group, const std::string& a_channel,
			RE::Actor* a_mouth = nullptr, bool a_noInterrupt = false,
			RE::Actor* a_speaker = nullptr, Tags::Mask a_facts = 0,
			bool a_blockCaption = false, const VoiceLog::Request* a_log = nullptr,
			bool a_voice = false, bool a_dialogueGuard = true)
		{
			const auto miss = [&](std::string_view a_reason) {
				if (a_log) {
					VoiceLog::Miss(*a_log, a_reason);
				}
				return 0;
			};
			if (a_folderKey.empty()) {
				return miss("no folder, alias, fallback or fallback slot");
			}
			Tags::Mask poolTags = 0;
			bool       descended = false;
			const auto file = FolderCache::PickNext(a_folderKey, a_facts, &poolTags, &descended);
			if (file.empty()) {
				return miss("folder has no qualifying pool");
			}
			AudioEngine::Played played;
			auto handle = AudioEngine::PlayPath(file, a_follow, a_volume, &played);
			if (!handle.IsValid()) {
				return miss("engine refused the file");  // PlayPath released any acquired slot
			}
			const auto id = InstanceManager::Register(handle, a_volume, a_group, file, a_follow,
				a_voice, played.outputModel);
			if (!a_channel.empty()) {
				// claim atomically: if no-interrupt loses the race for a channel
				// still playing, drop this one (it's within startup grace, silent)
				if (!InstanceManager::PlayOnChannel(a_channel, id, a_noInterrupt)) {
					InstanceManager::Stop(id);
					return miss("channel busy (voice_no_interrupt)");
				}
			}
			if (a_mouth) {
				LipSync::Start(a_mouth, file, handle, id, a_dialogueGuard);
			}
			if (!a_blockCaption) {
				CaptionManager::Start(a_speaker ? a_speaker : a_follow, file, handle, id);
			}
			if (a_log) {
				VoiceLog::Pick(*a_log, a_folderKey, file, poolTags, descended, id);
			}
			return id;
		}

		// ---------- natives: core ----------

		std::int32_t GetAPIVersion(RE::StaticFunctionTag*)
		{
			return API_VERSION;
		}

		// True while the game world is frozen behind a menu, so a caller's own
		// scheduling loop can hold instead of advancing to its next line over a
		// frozen scene. Two independent freezes are reported:
		//   * UI::GameIsPaused() - a real menu-mode menu (numPausesGame > 0).
		//   * Main::freezeTime   - what the ImGui overlay frameworks set (SKSE
		//     Menu Framework's GameLock). These never enter menu mode, so the
		//     Papyrus VM keeps ticking - Utility.Wait, RegisterForSingleUpdate
		//     and vanilla Utility.IsInMenuMode() all miss them, which is why a
		//     script loop needs this check to notice the pause at all. The same
		//     flag is set by `tfc 1`, so [general] pause_on_freeze_time = false
		//     drops this half and reports menu-mode pauses only.
		bool IsGamePaused(RE::StaticFunctionTag*)
		{
			if (const auto ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
				return true;
			}
			if (const auto settings = Config::Get(); !settings || !settings->pauseOnFreezeTime) {
				return false;
			}
			const auto main = RE::Main::GetSingleton();
			return main && main->GetRuntimeData().freezeTime;
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
			AudioEngine::ApplyConfig();
			CaptionManager::ApplyConfig();
			VoiceLog::ApplyConfig();
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
				VoiceLog::Miss({ a_actor, {}, a_category, a_facts, false }, "actor routes to no slot");
				return 0;
			}
			VoiceLog::Request log{ a_actor, slot->id, a_category, a_facts, false };
			auto key = ResolveGaggedKey(*settings, *slot, a_category, a_actor, a_facts);
			if (key.empty()) {
				// last resort: non-voice scene sounds (PullOutGape, Smack, ...) live in
				// the sfx slot / [sfx] table. The facts ride along — the sfx slot is a
				// real [[slot]], so its categories can carry tag pools too, and
				// PlayFromKey picks with these same facts.
				key = ResolveSfxKey(*settings, a_category, a_facts);
				log.viaSfx = !key.empty();
			}
			// no-interrupt early-out: cheap pre-check to avoid building a sound we'd
			// drop (the atomic claim in PlayFromKey closes the check/claim race)
			if (settings->voiceNoInterrupt && a_channel[0] != '\0' &&
				InstanceManager::IsChannelBusy(a_channel)) {
				VoiceLog::Miss(log, "channel busy (voice_no_interrupt)");
				return 0;
			}
			// 3D-follow only when voice3D is on; either way the mouth actor drives lipsync
			RE::Actor* follow = settings->voice3D ? a_actor : nullptr;
			// per-call opt-out OR a category configured to never lipsync (oral sfx, climax)
			const bool blockLip = a_blockLipSync ||
				settings->lipsyncBlockCategories.contains(Config::Normalize(a_category));
			// the sfx slot / [sfx] last resort plays a scene sound, not speech
			return PlayFromKey(key, follow, a_volume, a_group, a_channel,
				blockLip ? nullptr : a_actor, settings->voiceNoInterrupt, a_actor, a_facts,
				a_blockCaption, &log, !log.viaSfx);
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
			bool a_blockCaption = false, bool a_dialogueGuard = true)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot);
			if (!slot) {
				logger::warn("PlayVoiceFromSlot: unknown slot '{}'", a_slot);
				VoiceLog::Miss({ a_follow, a_slot, a_category, a_facts, false }, "no such slot");
				return 0;
			}
			VoiceLog::Request log{ a_follow, slot->id, a_category, a_facts, false };
			const auto key = ResolveGaggedKey(*settings, *slot, a_category, a_follow, a_facts);
			if (settings->voiceNoInterrupt && a_channel[0] != '\0' &&
				InstanceManager::IsChannelBusy(a_channel)) {
				VoiceLog::Miss(log, "channel busy (voice_no_interrupt)");
				return 0;
			}
			RE::Actor* follow = settings->voice3D ? a_follow : nullptr;
			const bool blockLip = a_blockLipSync ||
				settings->lipsyncBlockCategories.contains(Config::Normalize(a_category));
			return PlayFromKey(key, follow, a_volume, a_group, a_channel,
				blockLip ? nullptr : a_follow, settings->voiceNoInterrupt, a_follow, a_facts,
				a_blockCaption, &log, true, a_dialogueGuard);
		}

		std::int32_t PlayVoiceFromSlot(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category, RE::Actor* a_follow, float a_volume,
			RE::BSFixedString a_group, RE::BSFixedString a_channel, bool a_blockLipSync)
		{
			return PlayVoiceFromSlotImpl(a_slot.c_str(), a_category.c_str(), 0, a_follow,
				a_volume, a_group.c_str(), a_channel.c_str(), a_blockLipSync);
		}

		// PlayVoiceFromSlot plus `dialogueGuard`: false lifts the in-dialogue lipsync block
		// for this line (a mod voicing an unvoiced topic itself). A separate native
		// because the VM rejects a call whose arg count differs from the native's,
		// so a new argument on PlayVoiceFromSlot would break every compiled caller.
		std::int32_t PlayVoiceFromSlotEx(RE::StaticFunctionTag*, RE::BSFixedString a_slot,
			RE::BSFixedString a_category, RE::Actor* a_follow, float a_volume,
			RE::BSFixedString a_group, RE::BSFixedString a_channel, bool a_blockLipSync,
			bool a_dialogueGuard)
		{
			return PlayVoiceFromSlotImpl(a_slot.c_str(), a_category.c_str(), 0, a_follow,
				a_volume, a_group.c_str(), a_channel.c_str(), a_blockLipSync, false, a_dialogueGuard);
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

		// PlayFileWithLipSync plus `dialogueGuard` (see PlayVoiceFromSlotEx)
		std::int32_t PlayFileWithLipSyncEx(RE::StaticFunctionTag*, RE::BSFixedString a_path,
			RE::Actor* a_follow, float a_volume, RE::BSFixedString a_group,
			RE::BSFixedString a_channel, bool a_dialogueGuard)
		{
			return PlayFileByPath(a_path.c_str(), a_follow, a_volume,
				a_group.c_str(), a_channel.c_str(), true, a_dialogueGuard);
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
				a_follow, false, nullptr, 0, false, nullptr, true);
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

		// Optional per-slot schema label, exactly as Config parsed it: "A", "B" or
		// "D" ("A" for a slot that doesn't set it, and for an unknown slot id).
		// AudioUtil does not interpret it - consumers gate their own routing on it.
		// Config already normalizes the value, so hand it back verbatim: a previous
		// build clamped it to A/B here, so a "D" pack read back as "A" and its
		// consumer routed every line down the A layout (fixed 0.9.24).
		RE::BSFixedString GetSlotVariation(RE::StaticFunctionTag*, RE::BSFixedString a_slot)
		{
			const auto settings = Config::Get();
			const auto* slot = Config::FindSlot(*settings, a_slot.c_str());
			return slot ? slot->variation.c_str() : "A";
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

		// ---------- natives: mouth claims ----------
		//
		// Cross-mod jaw arbitration - see MouthClaim.h. IsLipSyncActive above keeps
		// its old meaning (AUDIOUTIL is driving this mouth); the union predicate
		// consumers want is IsMouthBusy.

		void ClaimMouth(RE::StaticFunctionTag*, RE::Actor* a_actor, float a_seconds,
			RE::BSFixedString a_owner)
		{
			MouthClaim::Claim(a_actor, a_seconds, a_owner.empty() ? ""sv : a_owner.c_str());
		}

		void ReleaseMouth(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::BSFixedString a_owner)
		{
			MouthClaim::Release(a_actor, a_owner.empty() ? ""sv : a_owner.c_str());
		}

		bool IsMouthClaimed(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			return MouthClaim::IsClaimed(a_actor);
		}

		bool IsMouthBusy(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			return MouthClaim::IsBusy(a_actor);
		}

		RE::BSFixedString GetMouthClaimOwner(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			return RE::BSFixedString(MouthClaim::Owner(a_actor));
		}

		float GetMouthClaimTimeLeft(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			return MouthClaim::TimeLeft(a_actor);
		}

		// Every actor with a live claim. With GetMouthClaimOwner + GetMouthClaimTimeLeft
		// this is the whole picture, so a consumer can render its own claim display
		// (an MCM debug page) without parsing a text dump. Papyrus arrays cap at 128;
		// a claim list that long means something is looping, so truncating is fine.
		std::vector<RE::Actor*> GetClaimedActors(RE::StaticFunctionTag*)
		{
			constexpr std::size_t   PAPYRUS_ARRAY_MAX = 128;
			std::vector<RE::Actor*> out;
			for (const auto formID : MouthClaim::ClaimedActorIDs()) {
				// a claimed actor can have been unloaded/deleted since; skip those
				// rather than handing script a None it has to filter
				if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID)) {
					out.push_back(actor);
					if (out.size() >= PAPYRUS_ARRAY_MAX) {
						break;
					}
				}
			}
			return out;
		}

		// Opt a playing instance into lipsync after the fact: drive akActor's
		// mouth from the clip's loudness, exactly like a PlayVoice line. For
		// PlayFile/PlayFolder callers whose file IS a spoken line (their play
		// calls never lipsync on their own - they also serve non-vocal
		// one-shots). Works for loose PCM wav and fuz. No-op
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

		// Where, rather than what kind of scene — and unlike GetContext this one
		// follows PPA's own mid-scene redirect. See AudioUtilPPA.psc for the value
		// table; 0 (None) is "no measurement", same ambiguity as GetContext's 0.
		std::int32_t GetPenetrationSite(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? static_cast<std::int32_t>(snapshot->site) : 0;
		}

		std::int32_t GetPenetrationSites(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? static_cast<std::int32_t>(snapshot->siteMask) : 0;
		}

		std::int32_t GetSelfPenetrationSite(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			return snapshot ? static_cast<std::int32_t>(snapshot->selfSite) : 0;
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

		// The whole per-receiver snapshot in ONE call (API v10) — for a consumer
		// that asks several questions about one moment (a voice scheduler reads
		// depth + context + site per spoken line; separate natives are separate
		// VM round-trips into the same mutex-guarded map). Empty array = no
		// measurement (not connected, actor unknown, nothing tracked) — the same
		// deliberate ambiguity as the scalar getters' 0. All slots float; the
		// integer fields are exact (their ranges sit far below float's 2^24
		// integer ceiling). Layout is append-only, mirrored in AudioUtilPPA.psc:
		//   [0] depth  [1] context bitmask  [2] penetration site
		//   [3] sites bitmask  [4] self-penetration site
		//   [5] vaginal opening  [6] anal opening
		std::vector<float> GetSnapshot(RE::StaticFunctionTag*, RE::Actor* a_receiver)
		{
			const auto snapshot = PPABridge::GetFor(a_receiver);
			if (!snapshot) {
				// all zeros, never an empty vector: a zero-length result reaches the VM as a
				// None-typed value (CommonLib's PackValue leaves the Variable None when the
				// engine refuses the zero-length CreateArray) and the calling script then logs
				// "Cannot cast from None to Float[]" on every call - SLO VE saw ~100 a session
				// on its female-partner lines, each one a partner PPA was not tracking. Zeros
				// read exactly like the scalar getters' nothing-measured (depth 0, site 0,
				// context 0), so the one-call contract stays "test the slots".
				return std::vector<float>(7, 0.0f);
			}
			return {
				snapshot->depth,
				static_cast<float>(snapshot->context),
				static_cast<float>(snapshot->site),
				static_cast<float>(snapshot->siteMask),
				static_cast<float>(snapshot->selfSite),
				snapshot->vaginalOpening,
				snapshot->anusOpening
			};
		}

		// ---------- natives: debug ----------

		std::int32_t DebugPlayFile(RE::StaticFunctionTag*, RE::BSFixedString a_path,
			RE::Actor* a_follow, std::int32_t a_flags, std::int32_t a_priority)
		{
			AudioEngine::Played played;
			auto handle = AudioEngine::PlayPath(a_path.c_str(), a_follow, 1.0f,
				static_cast<std::uint32_t>(a_flags), static_cast<std::uint32_t>(a_priority), &played);
			if (!handle.IsValid()) {
				return 0;
			}
			return InstanceManager::Register(handle, 1.0f, "", a_path.c_str(), a_follow, false,
				played.outputModel);
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

		// runtime voice-log control (autest voicelog) - "off" | "player" | "all"
		void SetVoiceLogMode(RE::StaticFunctionTag*, RE::BSFixedString a_mode)
		{
			if (const auto mode = VoiceLog::ParseMode(a_mode.c_str())) {
				VoiceLog::SetMode(*mode);
			} else {
				logger::warn("voicelog: '{}' is not off/player/all", a_mode.c_str());
			}
		}

		RE::BSFixedString GetVoiceLogMode(RE::StaticFunctionTag*)
		{
			switch (VoiceLog::CurrentMode()) {
			case VoiceLog::Mode::Player:
				return RE::BSFixedString{ "player" };
			case VoiceLog::Mode::All:
				return RE::BSFixedString{ "all" };
			default:
				return RE::BSFixedString{ "off" };
			}
		}

		// live mouth claims (actor, owner, seconds left) - who is holding a jaw
		RE::BSFixedString GetMouthClaims(RE::StaticFunctionTag*)
		{
			return RE::BSFixedString(MouthClaim::Describe());
		}

		// runtime A/B of [general] dialogue_output_model / voice_slider for new
		// lines: -1 = back to the config, 0 = off, 1 = on
		void SetOutputModelMode(RE::StaticFunctionTag*, std::int32_t a_mode)
		{
			AudioEngine::SetOutputModelMode(a_mode);
		}

		RE::BSFixedString GetOutputModelMode(RE::StaticFunctionTag*)
		{
			return RE::BSFixedString(AudioEngine::OutputModelStatus());
		}

		void SetVoiceSliderMode(RE::StaticFunctionTag*, std::int32_t a_mode)
		{
			AudioEngine::SetVoiceSliderMode(a_mode);
		}

		RE::BSFixedString GetVoiceSliderMode(RE::StaticFunctionTag*)
		{
			return RE::BSFixedString(AudioEngine::VoiceSliderStatus());
		}

		// a line's output model and volume factors, plus the dialogue defaults,
		// also written to AudioUtil.log
		RE::BSFixedString DescribeSound(RE::StaticFunctionTag*, std::int32_t a_handle)
		{
			const auto text = InstanceManager::DescribeSound(a_handle);
			logger::info("sndinfo: {}", text);
			return RE::BSFixedString(text);
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
		float a_volume, const char* a_group, const char* a_channel, bool a_lipSync,
		bool a_dialogueGuard)
	{
		std::string path = a_dataRelPath;
		std::replace(path.begin(), path.end(), '/', '\\');
		AudioEngine::Played played;
		auto handle = AudioEngine::PlayPath(path, a_follow, a_volume, &played);
		if (!handle.IsValid()) {
			return 0;  // PlayPath released any acquired slot on failure
		}
		// the WithLipSync variant is the spoken-line one: it gets the Voice volume
		const auto id = InstanceManager::Register(handle, a_volume, a_group, path, a_follow,
			a_lipSync, played.outputModel);
		if (*a_channel) {
			InstanceManager::PlayOnChannel(a_channel, id);
		}
		// explicit files caption too when a sidecar exists (needs an actor
		// to attribute the subtitle to)
		CaptionManager::Start(a_follow, path, handle, id);
		if (a_lipSync && a_follow) {
			LipSync::Start(a_follow, path, handle, id, a_dialogueGuard);
		}
		return id;
	}

	bool RegisterFuncs(VM* a_vm)
	{
		REGISTERFUNC(GetAPIVersion, SCRIPT_NAME);
		REGISTERFUNC(ReloadConfig, SCRIPT_NAME);
		REGISTERFUNC(IsGamePaused, SCRIPT_NAME);
		REGISTERFUNC(PlayVoice, SCRIPT_NAME);
		REGISTERFUNC(PlayVoiceTagged, SCRIPT_NAME);
		REGISTERFUNC(PlayVoiceFromSlot, SCRIPT_NAME);
		REGISTERFUNC(PlayVoiceFromSlotEx, SCRIPT_NAME);
		REGISTERFUNC(PlayVoiceFromSlotTagged, SCRIPT_NAME);
		REGISTERFUNC(PlaySFX, SCRIPT_NAME);
		REGISTERFUNC(PlayFile, SCRIPT_NAME);
		REGISTERFUNC(PlayFileWithLipSync, SCRIPT_NAME);
		REGISTERFUNC(PlayFileWithLipSyncEx, SCRIPT_NAME);
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
		REGISTERFUNC(ClaimMouth, SCRIPT_NAME);
		REGISTERFUNC(ReleaseMouth, SCRIPT_NAME);
		REGISTERFUNC(IsMouthClaimed, SCRIPT_NAME);
		REGISTERFUNC(IsMouthBusy, SCRIPT_NAME);
		REGISTERFUNC(GetMouthClaimOwner, SCRIPT_NAME);
		REGISTERFUNC(GetMouthClaimTimeLeft, SCRIPT_NAME);
		REGISTERFUNC(GetClaimedActors, SCRIPT_NAME);
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
		REGISTERFUNC(SetLipFilesMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetLipFilesMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(SetPseudoLipMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetPseudoLipMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(SetLipLeadMs, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetLipLeadMs, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetMouthClaims, TEST_SCRIPT_NAME);
		REGISTERFUNC(SetVoiceLogMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetVoiceLogMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(SetOutputModelMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetOutputModelMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(SetVoiceSliderMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(GetVoiceSliderMode, TEST_SCRIPT_NAME);
		REGISTERFUNC(DescribeSound, TEST_SCRIPT_NAME);
		REGISTERFUNC(IsConnected, PPA_SCRIPT_NAME);
		REGISTERFUNC(SetEventRate, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetContext, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetDepth, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetPenetrationSite, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetPenetrationSites, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetSelfPenetrationSite, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetVaginalOpening, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetAnalOpening, PPA_SCRIPT_NAME);
		REGISTERFUNC(GetSnapshot, PPA_SCRIPT_NAME);
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
