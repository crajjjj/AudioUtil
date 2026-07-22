# Voice & Category Resolution

A single `PlayVoice(actor, "Category")` call runs **two** lookups in order:

1. **Slot resolution** — which voice pack does this *actor* use?
2. **Category resolution** — which folder inside that slot does `"Category"` map to?

Understanding these two chains is the whole game when authoring an `AudioUtil.toml`.

## 1. Slot resolution — the actor's voice

Checked top to bottom; **first hit wins**.

| # | Source | Notes |
|---|--------|-------|
| 1 | `pc_female_slot` / `pc_male_slot` | **Player only.** The PC always resolves here, and no other actor ever does. Empty = no reservation. |
| 2 | `[npc_overrides]` | Explicit per-NPC pin, `'Plugin.esp\|FormID' = "Slot"`. May target any slot, including a pc-reserved one. |
| 3 | `[voicetype_remap]` → `[voicetype_map]` | Rename the actor's voicetype (one hop) to one you have, then map that voicetype to a slot. |
| 4 | `[race_map]` | Substring match against the race editor id; most specific (longest) hint wins. Creatures resolve here. |
| 5 | `default_female_slot` / `default_male_slot` | Last resort, by the actor's sex. |

`GetSlotForActor(actor)` returns exactly what this chain produces (`""` if nothing resolves). Non-player actors **never** resolve to a pc-reserved slot.

### Spreading across a slot list

`[voicetype_map]` and `[race_map]` values may be a **single slot** or a **list of candidates**:

```toml
[voicetype_map]
MaleEvenToned = "M1"
MaleBandit = ["M3", "M4"]   # bandits spread across M3 and M4
```

With a list, actors sharing the voicetype are spread across the slots **deterministically by form id** — the *same* NPC always resolves to the *same* slot, every scene, load-order independent.

### `[voicetype_remap]` — covering voicetypes you lack

Renames a voicetype to one you *do* have a pack for, **before** the slot lookup. Values are voicetype names (a single hop — the target must be a `[voicetype_map]` key), not slot ids:

```toml
[voicetype_remap]
enable = true
MaleGuard = "MaleNord"          # a guard now resolves as a Nord
MaleCommander = "MaleBrute"
```

`enable = false` turns the whole layer off, so only exact `[voicetype_map]` matches get voices.

### `[race_map]` — race and creatures

Hints are **substring-matched** against the race editor id, longest hint first:

```toml
[race_map]
Nord = "M4"       # matches NordRace and NordRaceVampire
Troll = "C1"      # matches TrollRace and TrollFrostRace
```

The longest-match rule is what keeps `Werewolf` and `Wolf`, or `Dog` and a custom husky race, apart. Creature races usually have no `ActorBase` sex and read as **male**.

### `[npc_overrides]` — pinning one NPC

```toml
[npc_overrides]
'MyFollower.esp|000D62' = "F2"
```

The key is `'Plugin.esp|FormID'`. For **ESL-flagged** plugins use the last **3** hex digits of the form id (`'MyEslMod.esp|D62'`). Checked before voicetype/race resolution; may target any slot, including a reserved one. This is how you give a specific follower her own voice.

## 2. Category resolution — the folder inside the slot

Once the slot is known, `"Category"` resolves against that slot, in this order:

```
exact folder
  → [category_aliases]        (rename: script name -> on-disk folder)
  → [male_only_remap]         (male slots only)
  → [category_fallbacks]      (substitute, one hop)
  → the slot's `fallback` slot (retry the whole chain there)
  → [sfx] table               (last resort, same name)
```

If nothing resolves, `PlayVoice` returns `0` and logs it.

### `[category_aliases]` — script name vs. folder name

When your Papyrus category name doesn't match the on-disk folder name, alias it (split by sex):

```toml
[category_aliases.female]
BattleCry = "War Shout"        # PlayVoice(...,"BattleCry") plays the "War Shout" folder

[category_aliases.male]
# ...
```

### `[male_only_remap]` — female-engine categories on a male voice

When a male slot receives a category named for the female script set (male-only scenes route female-engine decisions to male voices), substitute the closest male category. **Male slots only:**

```toml
[male_only_remap]
ComfortLines = "Calm Lines"
```

### `[category_fallbacks]` — substitute an empty category

A single hop: the substitute must resolve directly. Split by sex:

```toml
[category_fallbacks.female]
Whisper = "Murmur"             # if "Whisper" has no folder, play "Murmur"
```

### `fallback` — backfill from another slot

A `[[slot]]` may name a `fallback` slot. Any category that resolves to **nothing** in this slot (no folder, alias, or category fallback) is retried in the fallback slot — per category, running the whole category chain there. Chains are allowed, **capped at 4 hops**.

```toml
[[slot]]
id = "F1"
sex = "female"
path = 'Sound\fx\MyMod\F1'
fallback = "F0"                # categories F1 lacks fall through to F0
```

This is how a scanned pack slot backfills from a stock slot: an empty pack folder means every category falls through; a partial pack, only the ones it lacks. It lets a player drop a partial voice pack into `F1` and still have every category covered.

### `[sfx]` last resort

If a category still has no folder after everything above, `PlayVoice` tries the `[sfx]` table under the same name before giving up — so a body-SFX name can double as a voice category when it makes sense.

## Debugging resolution

- `GetSlotForActor(actor)` → the slot the chain picks (`""` = none).
- `CategoryExists(slot, category)` → does category resolution find at least one file?
- `GetCategoryFileCount(slot, category)` → how many files, after aliases/fallbacks.
- `ReloadConfig()` → re-read the TOML and rescan folders live, then re-test.
