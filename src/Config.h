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

		// true = this slot is reached only by script (PlayVoiceFromSlot), never by
		// actor->slot routing, so the wiring audit must NOT flag it as an orphan
		// ("has audio but no route"). For deliberately manual pools (e.g. a mod's
		// own SFX/ambient slot it plays directly). Default false.
		bool scriptOnly{ false };

		// optional per-slot schema label, for consumers whose voice packs ship in
		// more than one folder/category layout. "B" = the alternate layout, "A"
		// (default) = the primary one. AudioUtil does not interpret it; a consumer
		// mod sets it and gates its own category routing on it. Empty/unset = "A".
		std::string variation;  // "A" or "B"; default "A"
	};

	// a plugin + local form id reference, resolved to a live form at load
	// (e.g. "Devious Devices - Assets.esm|7EB8"). Used for the [gag] and [tongue]
	// markers: both worn keywords and specific worn item forms.
	struct FormRef
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

		// Sound\AudioUtilFuzCache\ size cap in MB, enforced once per launch
		// (oldest files deleted first). ~88 KB per second of decoded audio, so
		// the default holds on the order of a thousand voice lines. 0 = unlimited.
		std::uint32_t fuzCacheMaxMB{ 256 };

		// Placeholder-slot pool size for in-session fuz playback (see FuzSlots): a
		// first-session fuz decode is routed through one of N pre-indexed slot wavs
		// so it's audible WITHOUT a restart, and released when the line ends. N caps
		// simultaneously-playing fuz lines (each concurrent line needs its own slot);
		// 24 comfortably covers overlapping voices. 0 disables (revert to the
		// decode-now-audible-next-launch behavior). Base-only global.
		int fuzSlots{ 24 };

		// Decode-ahead every .fuz the config knows about, on a background thread at
		// game start, so their cache wavs exist BEFORE the next launch. A cache wav
		// written mid-session is invisible to the engine's loose-file resource index
		// (frozen at launch — and doubly so under MO2's USVFS), so a first-ever fuz
		// plays silent until it has been cached by a prior session. This warms the
		// whole set automatically: first launch after adding fuz decodes them (still
		// silent that session); every launch after, they play first try. Idempotent
		// and cheap once cached (a disk-exists check per file). Base-only global.
		bool prewarmFuz{ false };

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

		// Distance attenuation for follow-positioned sounds (voice + sfx). Loose-file
		// playback has no sound-descriptor rolloff curve, so a followed sound barely
		// gets quieter with distance (positioning still works). When enabled, the
		// per-instance volume is scaled once at play time by the listener->speaker
		// distance: full within attenuationNear, quadratic falloff to attenuationFloor
		// at attenuationFar (units). Off by default = no change for existing consumers.
		// Only meaningful when a sound plays far from the player (e.g. NPC-only scenes).
		bool  voiceAttenuation{ false };
		float attenuationNear{ 400.0f };
		float attenuationFar{ 3000.0f };
		float attenuationFloor{ 0.0f };

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
		// real phoneme lipsync from .lip data (same-stem .lip / fuz-embedded):
		// when a played line has one, its authored curves drive the mouth
		// instead of the amplitude envelope (runtime toggle: autest lipfiles)
		bool          lipsyncUseLipFiles{ true };
		// also drive the 16 MFG modifier channels (blinks/brows/gaze) a .lip
		// carries — off by default: expression mods own those channels
		bool          lipsyncDriveModifiers{ false };
		// lines WITHOUT lip data: synthesize pseudo-phoneme curves from the
		// amplitude envelope (per-syllable vowel variety + lip closures after
		// silence gaps) instead of the plain Aah jaw-flap (runtime toggle:
		// autest pseudolip). Deterministic per file path.
		bool          lipsyncPseudoPhonemes{ false };
		// mouth timing lead in ms, all lipsync modes: positive samples the
		// curves ahead of the playback clock, compensating the engine's
		// start-detection + mix-ahead latency when the mouth visibly trails
		// the sound. Calibrate live: autest liplead <ms>
		std::int32_t  lipsyncLeadMs{ 0 };
		// pseudo-synthesis tuning (SynthesizePseudoLip). Calibrate visually in
		// tools/lipsim ("copy constants"), paste here, `au reload` — no rebuild.
		float         lipsyncPseudoVoicedFloor{ 0.05f };  // voiced when envelope >= max(min_level, this)
		float         lipsyncPseudoValleyRatio{ 0.6f };   // split syllables at valleys below this * running peak
		std::uint32_t lipsyncPseudoMinSylFrames{ 3 };     // minimum syllable length (30 fps frames)
		std::uint32_t lipsyncPseudoGapFrames{ 5 };        // silence frames (30 fps) that earn a BMP lip closure
		float         lipsyncPseudoClosure{ 0.85f };      // closure peak weight (lead-in frame = 0.65x this)
		// requested categories that never drive lipsync — the line plays mouth-still.
		// For pools that aren't vocalization (oral sfx: slurping) or where another
		// system owns the mouth (a climax/ahegao face). Matched on the REQUESTED
		// category name (normalized), before aliasing/remap.
		std::unordered_set<std::string> lipsyncBlockCategories;
		// note: a gagged actor's lipsync is suppressed via [gag] device detection
		// (see GagState), not an MFG mouth-open threshold.

		// gagged-voice routing: when a speaking actor wears any gag marker — a
		// worn keyword from gagKeywords or a specific worn item from gagItems —
		// a slot's voice resolves from its gag_slot instead. If the gag slot
		// lacks the requested category, gagDefaultCategory (a muffled catch-all)
		// plays there rather than leaking the clear line. Dormant with no
		// markers configured, so the SFW-neutral default is unaffected.
		bool                  gagEnabled{ true };
		std::string           gagDefaultCategory;  // normalized; empty = none
		std::vector<FormRef>  gagKeywords;
		std::vector<FormRef>  gagItems;  // specific worn item forms (ARMO/…)

		// on-screen captions: when a played wav has a same-named .toml sidecar
		// next to it (line01.wav + line01.toml) holding per-language text
		// (en = "...", ru = "..."), the resolved text is shown as a game
		// subtitle attributed to the speaker while the line plays, and an
		// "AudioUtil_Caption" mod event is sent (see CaptionManager). Loose
		// files only — a sidecar can't be read out of a BSA.
		bool        captionsEnabled{ true };
		// sidecar key to read: "auto" resolves the game's sLanguage ini setting
		// to a two-letter code ("ENGLISH" -> "en"); any other value is used
		// verbatim (lowercased), so a pack may invent its own keys
		std::string captionsLanguage{ "auto" };
		// false = don't inject the HUD subtitle, only send the mod event — for
		// a consumer that draws captions with its own UI
		bool        captionsHud{ true };

		// worn-tongue lipsync suppression: when a speaking actor wears any tongue
		// marker — a worn keyword from tongueKeywords or a specific worn item form
		// from tongueItems — lipsync is suppressed for that line (a visible tongue
		// needs the mouth held open; a lipsync jaw-flap would clip it). Detection
		// only, no voice rerouting. Dormant with no markers, so the SFW-neutral
		// default is unaffected. Mirrors the [gag] marker lists (see TongueState).
		bool                  tongueEnabled{ true };
		std::vector<FormRef>  tongueKeywords;
		std::vector<FormRef>  tongueItems;  // specific worn tongue-armor forms (ARMO)
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
