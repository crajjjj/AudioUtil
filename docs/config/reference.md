# Config Reference

Every table and key in the config — the base `Data\SKSE\Plugins\AudioUtil\AudioUtil.toml` and any `AudioUtil\config\*.toml` overlays, which are [merged](index.md) into one effective config. Defaults shown are the DLL's internal defaults, applied when a key is absent from *every* file. All keys/names are [normalized](index.md#normalization) (case- and space-insensitive).

Paths written `'Sound\...'` are **Data-relative**. Single-quoted TOML literal strings are used throughout so backslashes need no escaping.

## `[general]`

!!! warning "Base-only section"
    `[general]` (along with `[ppa]`, `[lipsync]`, and the `[gag]` `enable`/`default_category` toggles) is a **global** section, read **only from the base `AudioUtil.toml`**. If a `config\*.toml` overlay sets any of these, it is ignored with a warning in `AudioUtil.log`. See [the merge rules](index.md).

```toml
[general]
log_level = "info"            # trace | debug | info | warn | error
sound_flags = 0x1A            # BuildSoundDataFromFile flags
sound_priority = 128
default_female_slot = "F1"
default_male_slot = "M1"
pc_female_slot = ""           # reserved player slot; empty = no reservation
pc_male_slot = ""
voice_3d = true               # 3D-position voices at the speaker; false = flat/2D
voice_no_interrupt = false    # skip a new line while its channel is still playing
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `log_level` | string | `"info"` | Verbosity of `AudioUtil.log`. |
| `sound_flags` | int | `0x1A` | `BuildSoundDataFromFile` flags. Sweep with [`DebugPlayFile`](../api/audioutil.md#debugplayfile) if audio is silent or not 3D. |
| `sound_priority` | int | `128` | Sound priority passed to the engine. |
| `sfx_slot` | slot id | `"SFX0"` | The `[[slot]]` whose categories [`PlaySFX`](../api/audioutil.md#playsfx) resolves before the flat `[sfx]` table (see [SFX](#sfx-the-sfx-slot-sfx-table)). `""` = table only. |
| `default_female_slot` | slot id | `"F1"` | Slot for unrouted female actors. |
| `default_male_slot` | slot id | `"M1"` | Slot for unrouted male actors (and creatures). |
| `pc_female_slot` | slot id | `""` | Reserved for the player; no NPC ever resolves to it. |
| `pc_male_slot` | slot id | `""` | Same, for a male PC. |
| `voice_3d` | bool | `true` | `true` = 3D-position each voice at the speaker (distance attenuation). `false` = play flat/2D at full volume so every speaker is equally audible. Lipsync is unaffected either way. |
| `voice_no_interrupt` | bool | `false` | `true` = when a `PlayVoice` names a channel still playing a line, skip the new line instead of cutting the old one off (per channel — different speakers still overlap). SFX and `PlayFile`/`PlayFolder` are unaffected. |

## `[ppa]`

```toml
[ppa]
enable = false                # neutral default ships disabled
event_rate_ms = 2000
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `enable` | bool | `true` *(internal)* | Gate the [Accurate Penetration bridge](../api/ppa.md). The **shipped neutral TOML sets `false`**; the bridge also needs the PPA plugin present. |
| `event_rate_ms` | int | `2000` | Min interval per receiver for `AudioUtilPPA_Update` mod events. Context-bit changes still fire immediately. |

## `[lipsync]`

```toml
[lipsync]
enable = true
gain = 1.0                    # 0.0-2.0 mouth-open strength
attack_ms = 30                # how fast the mouth opens toward a louder level
release_ms = 90               # how fast it closes on quiet / clip end
min_level = 0.04              # envelope levels below this keep the mouth closed
block_categories = ["BlowjobActionSoft", "Orgasm"]   # never drive the mouth
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `enable` | bool | `true` | Amplitude-envelope lipsync master switch. |
| `gain` | float | `1.0` | Mouth-open strength, `0.0`–`2.0` (`1.0` = envelope as-is). |
| `attack_ms` | int | `30` | Opening speed toward a louder level. |
| `release_ms` | int | `90` | Closing speed on quiet / clip end. |
| `min_level` | float | `0.04` | Envelope levels below this keep the mouth closed. |
| `block_categories` | string list | *(none)* | Requested categories that **never** drive lipsync — the line plays mouth-still. Matched on the requested name (normalized), before aliasing, across every slot. For pools that aren't vocalization (oral SFX / slurping) or where another system owns the mouth (a climax/ahegao face). Same effect as passing `blockLipSync=true` to [`PlayVoice`](../api/audioutil.md#playvoice), but declared once in config. |

Runtime overrides: [`SetLipSyncEnabled`](../api/audioutil.md#setlipsyncenabled-islipsyncenabled) / [`SetLipSyncGain`](../api/audioutil.md#setlipsyncgain). `ReloadConfig` restores these TOML values.

Lipsync is **suppressed automatically for a gagged actor** — when the speaker wears a device configured in [`[gag]`](#gag), the device owns the mouth and the DLL won't drive it (checked when the line starts and re-checked on a 500 ms throttle, so a gag equipped mid-line hands the mouth over). No mouth-open threshold to tune.

## `[gag]`

Gag / deepthroat / mouth-owning devices. When a speaking actor wears any configured keyword, AudioUtil (1) routes their voice through the slot's [`gag_slot`](#slot) so a muffled clip plays instead of the clear line, and (2) suppresses lipsync (see above). Detection is **native** — the DLL reads the actor's worn items, no Papyrus wiring.

Dormant unless `keywords` is set, so the SFW-neutral default is unaffected.

```toml
[gag]
enable = true
default_category = "GagMoan"     # muffled catch-all for categories the gag slot lacks
keywords = [                     # 'Plugin.esp|FormID' (hex), like [npc_overrides] keys
  'Devious Devices - Assets.esm|7EB8',
  'ZaZAnimationPack.esm|8A4D',
  'Toys.esm|8C2',
]
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `enable` | bool | `true` | Master switch. Feature is also inert while `keywords` is empty. |
| `default_category` | category | `""` | Played from the gag slot when the requested category has no audio there — a muffled catch-all so a gagged actor never leaks a clear line (and never falls silent). Empty = no catch-all. |
| `keywords` | list of `'Plugin.esp\|FormID'` | *(none)* | Worn keywords that mark an actor as gagged. Hex form id, `0x` optional; same format as `[npc_overrides]` keys. A keyword whose plugin isn't in the load order is **skipped** (one `debug` line), so listing optional mods is harmless. |

See [Resolution → gag redirect](resolution.md#gag_slot-muffled-voice-when-gagged) for how the redirect interacts with the category chain.

## `[[slot]]`

An array of tables — one per voice pack. See [Overview → three ways](index.md#a-slot-can-get-its-files-three-ways) and [Resolution](resolution.md#fallback-backfill-from-another-slot).

```toml
[[slot]]
id = "M4"
sex = "male"                  # "male" | "female" | "all"
path = 'Sound\fx\MyMod\M4'    # scanned for <Category>\*.wav subfolders
fallback = "M1"               # optional: backfill empty categories from this slot
gag_slot = "M4gag"            # optional: muffled parallel slot used when gagged
```

| Key | Type | Meaning |
|-----|------|---------|
| `id` | string | Slot id (`"F1"`, `"M4"`, `"C2"`, …). |
| `sex` | string | `"male"`, `"female"`, or `"all"`. `"all"` is sex-neutral: it matches either sex on explicit routes (`[race_map]`/`[voicetype_map]`/`[npc_overrides]`) but is skipped by the blind default-by-sex fallback. For the category layer it shares the **male** aliases/`[category_fallbacks.male]` (where creature/neutral fallbacks are authored) and skips `[male_only_remap]`. Use it for **creature** slots (a creature's reported sex is unreliable) and the sfx slot. Plain `"male"`/`"female"` otherwise; creatures otherwise read as male. |
| `path` | path | Folder scanned for `<Category>\*.wav` subfolders. Optional if the slot is defined purely by explicit categories. Loose files only. |
| `fallback` | slot id | Optional. Per-category backfill slot when a category resolves to nothing here. Chains capped at **4 hops**. |
| `gag_slot` | slot id | Optional. A parallel slot (another `[[slot]]`, same category names, muffled audio) used **instead of this one** when the speaking actor is gagged. See [`[gag]`](#gag). |
| `[slot.categories]` | table | Optional explicit categories (below). Win over same-named scanned folders. |

### `[slot.categories]` — explicit categories

A category value may be an **array** (a file list) or a **string** (one folder):

```toml
[[slot]]
id = "C1"
sex = "male"
[slot.categories]
# array: explicit file list — needs no loose files, may point into a BSA
BattleCry = [
  'Sound\FX\NPC\Giant\AttackVocal\NPC_Giant_AttackVocal_01.wav',
  'Sound\FX\NPC\Giant\AttackVocal\NPC_Giant_AttackVocal_02.wav',
]
# string: one folder scanned for this category ('Sound\...' = full Data path,
# otherwise relative to the slot's path). Loose files only.
WarCry = 'Sound\fx\SomeOtherMod\shouts'
Rally = 'Sound\fx\SomeOtherMod\shouts'
```

- **Array (file list)** — bypasses the filesystem scan, so it can reference **BSA-packed** audio (loose files still win over archives at play time).
- **String (folder)** — scanned like a slot folder; lets several categories share one pool of files without copies. Loose files only.

## Slot resolution tables

Full behavior in [Voice & Category Resolution](resolution.md#1-slot-resolution-the-actors-voice).

```toml
[voicetype_remap]             # rename a voicetype (one hop) before slot lookup
enable = true
MaleGuard = "MaleNord"        # values are voicetype names, not slot ids

[voicetype_map]               # voicetype -> slot id, or a list of candidates
MaleEvenToned = "M1"
MaleBandit = ["M3", "M4"]

[race_map]                    # race-editor-id substring -> slot id(s); longest hint wins
Nord = "M4"
Troll = "C1"

[npc_overrides]               # 'Plugin.esp|FormID' -> slot; ESL: last 3 hex digits
'MyFollower.esp|000D62' = "F2"
```

| Table | Key | Value |
|-------|-----|-------|
| `[voicetype_remap]` | `enable` (bool) + voicetype name | Target **voicetype name** (must be a `[voicetype_map]` key). |
| `[voicetype_map]` | voicetype name | Slot id **or** list of slot ids (spread deterministically per actor). |
| `[race_map]` | race-id substring | Slot id or list. Longest matching hint wins. |
| `[npc_overrides]` | `'Plugin.esp\|FormID'` | Slot id. Checked before voicetype/race. |

## Category layer tables

Full behavior in [Category resolution](resolution.md#2-category-resolution-the-folder-inside-the-slot).

```toml
[category_aliases.female]     # script name -> on-disk folder
BattleCry = "War Shout"
[category_aliases.male]
# ...

[male_only_remap]             # male slots only: female-engine category -> male category
ComfortLines = "Calm Lines"

[category_fallbacks.female]   # substitute when a category has no folder (one hop)
Whisper = "Murmur"
[category_fallbacks.male]
# ...
```

| Table | Applies to | Meaning |
|-------|-----------|---------|
| `[category_aliases.female]` / `.male` | that sex's slots | Rename a requested category to the actual folder name. |
| `[male_only_remap]` | male slots only | Substitute a male category for a female-engine category name. |
| `[category_fallbacks.female]` / `.male` | that sex's slots | Substitute (one hop) when a category resolves to no folder. |

## `[groups]`

Startup volumes, `0.0`–`1.0`. **Startup state only** — MCM sliders override at runtime via [`SetGroupVolume`](../api/audioutil.md#setgroupvolume). Groups are created on first use; these are just the conventional ones.

```toml
[groups]
pc_high = 1.0
pc_low = 1.0
partner_high = 1.0
partner_low = 1.0
sfx = 1.0
oneshot = 1.0
```

## SFX — the sfx slot + `[sfx]` table

[`PlaySFX(name, ...)`](../api/audioutil.md#playsfx) plays a named shuffle-bag pool, and a voice category with no folder falls through to an sfx pool of the same name (last resort). A name resolves in two places, **in order**:

1. **A category of the sfx slot** — id **`SFX0`** by default, set `sfx_slot` in [`[general]`](#general) to rename (`""` disables it). Since it's a normal `[[slot]]`, each sfx pool can use **any** [category form](#slot) — a scanned folder, an explicit **file list (BSA-capable)**, or a folder ref. Preferred, and the only way to give an sfx pool BSA-packed audio.
2. **The `[sfx]` table** — flat `name = folder`, loose files only. The value is a full `Sound\...` Data-relative path, same as a slot path.

Names are matched **directly** (no `category_aliases`/fallbacks). The slot's `sex` is irrelevant to sfx lookup.

```toml
[[slot]]
id = "SFX0"                          # PlaySFX resolves categories here first
sex = "all"                          # sex-neutral (irrelevant to sfx lookup)
[slot.categories]
Clap = 'Sound\fx\MyMod\SFX\Clap'          # scanned folder
Slap = ['Sound\fx\MyMod\SFX\slap01.wav']  # explicit list — may point into a BSA

[sfx]                                # legacy flat table, still supported
Thud = 'Sound\fx\MyMod\thuds'        # full Data path (loose files only)
Clap = 'Sound\fx\MyMod\SFX\Clap'     # full Data path
```
