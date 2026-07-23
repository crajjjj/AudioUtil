#pragma once

namespace Config
{
	struct Slot
	{
		std::string id;    // "F1", "M3"
		std::string root;  // data-relative folder, e.g. "Sound\\fx\\IVDT\\F1"; may be
		                   // empty for slots defined purely by explicit categories
		char        sex;   // 'F' female, 'M' male, 'A' all/any (sex-neutral: creature
		                   // and sfx slots). 'A' matches either sex on explicit routes
		                   // (race_map / voicetype_map / overrides) but is excluded from
		                   // the blind default-by-sex fallback. For the category layer it
		                   // shares the MALE aliases/fallbacks (where presets author
		                   // creature/neutral fallbacks) but skips male_only_remap.

		// explicit [slot.categories]: normalized category -> data-relative files.
		// These bypass the filesystem scan, so they can reference BSA-packed audio
		// (the engine's resource loader resolves archives; directory scans cannot).
		std::unordered_map<std::string, std::vector<std::string>> categories;

		// folder-string [slot.categories] values: normalized category -> one
		// folder to scan ('Sound\...' = full Data-relative path, otherwise
		// relative to the slot's path). Scanned like the [sfx] table, so loose
		// files only - BSA-packed audio needs the file-list form above.
		std::unordered_map<std::string, std::string> categoryDirs;

		// optional slot consulted per-category when this slot resolves a
		// category to nothing - lets a scanned pack slot backfill from a stock
		// slot (chains allowed, capped at 4 hops)
		std::string fallbackSlot;  // normalized id; empty = none

		// optional muffled parallel slot used instead of this one when the
		// speaking actor is gagged (see [gag]). Same category names, gagged
		// audio. Empty = this slot has no gagged variant.
		std::string gagSlot;  // normalized id; empty = none
	};

	// a worn keyword that marks an actor as gagged, resolved from a plugin +
	// local form id at load (e.g. "Devious Devices - Assets.esm|7EB8")
	struct GagKeyword
	{
		std::string   plugin;
		std::uint32_t localID{ 0 };
	};

	using StringMap = std::unordered_map<std::string, std::string>;
	using SlotList = std::vector<std::string>;  // candidate slot ids, picked per actor

	struct Settings
	{
		std::vector<Slot> slots;

		std::string defaultFemaleSlot{ "F1" };
		std::string defaultMaleSlot{ "M1" };

		// slots reserved for the player: the PC resolves to these first, and other
		// actors never resolve to them. Empty = no reservation for that sex.
		std::string pcFemaleSlot;
		std::string pcMaleSlot;

		// voicetype resolution (keys normalized). Map values may list several
		// candidate slots ("MaleBandit" -> ["M3", "M4"]); the resolver spreads
		// actors across them deterministically by form id.
		bool      voicetypeRemapEnabled{ true };
		StringMap voicetypeRemap;  // "maleguard" -> "malenord" (values are voicetype names)
		std::unordered_map<std::string, SlotList> voicetypeMap;  // "malenord" -> slot ids
		StringMap npcOverrides;    // "plugin.esp|formid-lowercase-hex" -> slot id

		// race fallback when no voicetype maps: normalized race hints matched as
		// substrings of the actor's race editor id ("nord" matches NordRaceVampire).
		// Sorted longest-hint-first at load so the most specific entry wins.
		std::vector<std::pair<std::string, SlotList>> raceMap;  // hint -> slot ids

		// category layer (keys normalized; values are raw folder/category names)
		StringMap femaleAliases;
		StringMap maleAliases;
		StringMap maleOnlyRemap;  // female-engine category -> male category
		StringMap femaleFallbacks;
		StringMap maleFallbacks;

		StringMap sfxTable;  // normalized name -> data-relative folder

		std::unordered_map<std::string, float> groupVolumes;

		// PlaySFX (and the voice-category last resort) resolves a name first as a
		// category of this slot, then as an entry of the flat [sfx] table. Routing
		// sfx through a normal [[slot]] lets sfx pools use the full slot toolset:
		// explicit file lists (BSA-capable), folder refs, or a scanned path — none
		// of which the string-only [sfx] table can express. Empty = [sfx] only.
		std::string sfxSlot{ "SFX0" };

		std::uint32_t soundFlags{ 0x1A };
		std::uint32_t soundPriority{ 128 };

		// 3D-position voices at the speaker (distance attenuation) vs play them
		// flat/2D at full volume. Off makes every speaker equally audible - the
		// player's voice is otherwise at the listener while partners attenuate
		// with distance. Lipsync is unaffected (it uses the mouth actor, not 3D).
		bool voice3D{ true };

		// when a PlayVoice call names a channel that is still playing a line,
		// skip the new line instead of cutting the old one off. Makes a speaker
		// finish their line before the next starts (per-channel, so different
		// speakers still overlap). SFX and PlayFile/PlayFolder are unaffected.
		bool voiceNoInterrupt{ false };

		bool          ppaEnabled{ true };
		std::uint32_t ppaEventRateMs{ 2000 };

		// amplitude-envelope lipsync for PlayVoice/PlayVoiceFromSlot
		bool          lipsyncEnabled{ true };
		float         lipsyncGain{ 1.0f };
		std::uint32_t lipsyncAttackMs{ 30 };
		std::uint32_t lipsyncReleaseMs{ 90 };
		float         lipsyncMinLevel{ 0.04f };
		// when the speaker is in a dialogue with the player, the game's own
		// dialogue/voice system drives their mouth from the real voice file, so
		// AudioUtil stays off it (default true). Checked at Start and re-checked
		// mid-line, like the gag guard.
		bool          lipsyncBlockInDialogue{ true };
		// requested categories that never drive lipsync — the line plays mouth-still.
		// For pools that aren't vocalization (oral sfx: slurping) or where another
		// system owns the mouth (a climax/ahegao face). Matched on the REQUESTED
		// category name (normalized), before aliasing/remap.
		std::unordered_set<std::string> lipsyncBlockCategories;
		// note: a gagged actor's lipsync is suppressed via [gag] device detection
		// (see GagState), not an MFG mouth-open threshold.

		// gagged-voice routing: when a speaking actor wears any of gagKeywords,
		// a slot's voice resolves from its gag_slot instead. If the gag slot
		// lacks the requested category, gagDefaultCategory (a muffled catch-all)
		// plays there rather than leaking the clear line. Dormant with no
		// keywords configured, so the SFW-neutral default is unaffected.
		bool                    gagEnabled{ true };
		std::string             gagDefaultCategory;  // normalized; empty = none
		std::vector<GagKeyword> gagKeywords;
	};

	// lowercase + strip non-alphanumerics: "About To Cum" == "AboutToCum" == "abouttocum"
	std::string Normalize(std::string_view a_text);

	// canonical npc-override lookup key (same form as [npc_overrides] keys after parsing)
	std::string MakeNpcKey(std::string_view a_plugin, std::uint32_t a_localID);

	// parse Data\SKSE\Plugins\AudioUtil\AudioUtil.toml.
	// On error the previous (or default) settings are kept and the error is logged.
	bool Load();

	std::shared_ptr<const Settings> Get();

	const Slot* FindSlot(const Settings& a_settings, std::string_view a_id);
}
