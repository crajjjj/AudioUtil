#pragma once

namespace Config
{
	struct Slot
	{
		std::string id;    // "F1", "M3"
		std::string root;  // data-relative folder, e.g. "Sound\\fx\\IVDT\\F1"; may be
		                   // empty for slots defined purely by explicit categories
		char        sex;   // 'F' or 'M'

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

		std::string voiceRoot{ "Sound\\fx\\AudioUtil" };
		std::string sfxRoot{ "Sound\\fx\\AudioUtil\\SFX" };

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
