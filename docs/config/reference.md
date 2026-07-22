# Config Reference

Every table and key in `Data\SKSE\Plugins\AudioUtil\AudioUtil.toml`. Defaults shown are the DLL's internal defaults, applied when a key is absent. All keys/names are [normalized](index.md#normalization) (case- and space-insensitive).

Paths written `'Sound\...'` are **Data-relative**. Single-quoted TOML literal strings are used throughout so backslashes need no escaping.

## `[general]`

```toml
[general]
log_level = "info"            # trace | debug | info | warn | error
sound_flags = 0x1A            # BuildSoundDataFromFile flags
sound_priority = 128
voice_root = 'Sound\fx\AudioUtil'
sfx_root = 'Sound\fx\AudioUtil\SFX'
default_female_slot = "F1"
default_male_slot = "M1"
pc_female_slot = ""           # reserved player slot; empty = no reservation
pc_male_slot = ""
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `log_level` | string | `"info"` | Verbosity of `AudioUtil.log`. |
| `sound_flags` | int | `0x1A` | `BuildSoundDataFromFile` flags. Sweep with [`DebugPlayFile`](../api/audioutil.md#debugplayfile) if audio is silent or not 3D. |
| `sound_priority` | int | `128` | Sound priority passed to the engine. |
| `voice_root` | path | `Sound\fx\AudioUtil` | Base folder documentation/convention for slot paths. |
| `sfx_root` | path | `Sound\fx\AudioUtil\SFX` | Base for relative `[sfx]` folder values. |
| `default_female_slot` | slot id | `"F1"` | Slot for unrouted female actors. |
| `default_male_slot` | slot id | `"M1"` | Slot for unrouted male actors (and creatures). |
| `pc_female_slot` | slot id | `""` | Reserved for the player; no NPC ever resolves to it. |
| `pc_male_slot` | slot id | `""` | Same, for a male PC. |

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
```

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `enable` | bool | `true` | Amplitude-envelope lipsync master switch. |
| `gain` | float | `1.0` | Mouth-open strength, `0.0`–`2.0` (`1.0` = envelope as-is). |
| `attack_ms` | int | `30` | Opening speed toward a louder level. |
| `release_ms` | int | `90` | Closing speed on quiet / clip end. |
| `min_level` | float | `0.04` | Envelope levels below this keep the mouth closed. |

Runtime overrides: [`SetLipSyncEnabled`](../api/audioutil.md#setlipsyncenabled-islipsyncenabled) / [`SetLipSyncGain`](../api/audioutil.md#setlipsyncgain). `ReloadConfig` restores these TOML values.

## `[[slot]]`

An array of tables — one per voice pack. See [Overview → three ways](index.md#a-slot-can-get-its-files-three-ways) and [Resolution](resolution.md#fallback-backfill-from-another-slot).

```toml
[[slot]]
id = "M4"
sex = "male"                  # "male" | "female"
path = 'Sound\fx\MyMod\M4'    # scanned for <Category>\*.wav subfolders
fallback = "M1"               # optional: backfill empty categories from this slot
```

| Key | Type | Meaning |
|-----|------|---------|
| `id` | string | Slot id (`"F1"`, `"M4"`, `"C2"`, …). |
| `sex` | string | `"male"` or `"female"`. Creatures read as male. |
| `path` | path | Folder scanned for `<Category>\*.wav` subfolders. Optional if the slot is defined purely by explicit categories. Loose files only. |
| `fallback` | slot id | Optional. Per-category backfill slot when a category resolves to nothing here. Chains capped at **4 hops**. |
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

## `[sfx]`

SFX name → folder, relative to `sfx_root` (`'Sound\...'` = full Data path). Each entry is a shuffle-bag folder played by [`PlaySFX`](../api/audioutil.md#playsfx), and also reachable as a category last-resort.

```toml
[sfx]
Clap = 'Clap'                        # -> <sfx_root>\Clap
Thud = 'Sound\fx\MyMod\thuds'        # full Data path
```
