# TOML Configuration — Overview

AudioUtil reads its config at `kDataLoaded` (and again on [`ReloadConfig()`](../api/audioutil.md#reloadconfig)) from **two places, merged in order**:

1. The base file **`Data\SKSE\Plugins\AudioUtil\AudioUtil.toml`** — loaded first.
2. Every **`Data\SKSE\Plugins\AudioUtil\config\*.toml`** overlay — merged on top in **sorted filename order**.

Together they map voice-slot ids to folders, categories to subfolders, SFX names to folders, and set group volumes plus lipsync/PPA options. Either place may be absent; only one file needs to exist.

!!! info "Who ships config — one mod or many"
    The plugin's own bundled `AudioUtil.toml` is **SFW-neutral**: it defines no slots and no SFX. A content mod that needs custom globals ships its **own** base `AudioUtil.toml` (overwriting the neutral one via load order). Pure add-ons (extra voice packs, an SFX set) instead drop an additive overlay in `config\` (e.g. `config\MyPack.toml`) — different filenames don't collide in the virtual file system, so any number of add-ons **compose** on top of whichever base is active.

    **Merge rules — two kinds of data:**

    - **Globals** — `[general]`, `[ppa]`, the `[lipsync]` scalar tuning, and the `[gag]` `enable`/`default_category` toggles — are read **only from the base `AudioUtil.toml`**. An overlay that sets them is **ignored, with a warning** in `AudioUtil.log`. This is deliberate: an add-on (or the user's own tuning) can never silently change engine-wide settings.
    - **Additive** — `[[slot]]`, `[sfx]`, `[npc_overrides]`, voicetype/race maps, category aliases/fallbacks, `[male_only_remap]`, `[groups]`, `[gag].keywords`, and `[lipsync] block_categories` — **accumulate** from the base *and* every overlay (union, last-writer-wins per key). A `[[slot]]` is keyed by `id`: if two files define the same id, the one that sorts last wins that **whole slot** (no per-category deep merge; logged). Note the last two live inside otherwise-base-only tables: only those specific keys merge, the surrounding scalars don't.

    Give every `[[slot]]` a full `Sound\...` `path` and every `[sfx]` entry a full `Sound\...` path — paths are always Data-relative, resolved identically for slots and sfx.

Each file is parsed independently: a parse error in one overlay **skips just that file** (logged to `AudioUtil.log`) and the rest still merge. If *every* file fails to parse, the **previous (or default) settings are kept** — a broken edit never leaves the game silent.

### Recommended layout

Split config by *nature* — which is also where the base-only/additive line falls:

- **Base `AudioUtil.toml`** — the singleton globals (`[general]`, `[ppa]`, `[lipsync]`, `[gag]` toggles) plus the resolution/routing maps a user tweaks. Only the base can set globals, so a preset mod ships this file.
- **`config\<YourMod>.toml`** — the bulky additive content: every `[[slot]]` and the `[sfx]` table. Add-on mods ship *only* this.

Use a stable, unique filename prefix so an add-on's files sort together and never collide with another mod's. A copy-paste additive-overlay template ships in the plugin's **`config.example\`** folder — that folder is *not* loaded; copy the file into `config\` to activate it.

## Normalization

Every key and name (slot ids, categories, group names, SFX names, voicetype names, race hints) is **normalized**: lowercased with all non-alphanumerics stripped. So `"Battle Cry"`, `"BattleCry"`, and `"battlecry"` are the same key, and `"About To Cum"` == `"AboutToCum"`. You never have to worry about spacing or case matching between your Papyrus calls and your TOML.

## The shape of the file

```toml
[general]   # roots, defaults, sound flags, reserved player slots
[ppa]       # optional Accurate Penetration bridge
[lipsync]   # amplitude-envelope mouth movement

[[slot]]    # one per voice pack: id + sex + a folder and/or explicit categories
# ...

[voicetype_remap]     # rename a voicetype to one you have a pack for
[voicetype_map]       # voicetype -> slot id(s)
[race_map]            # race-editor-id substring -> slot id(s)
[npc_overrides]       # 'Plugin.esp|FormID' -> slot id

[category_aliases.female] / [category_aliases.male]   # script name -> on-disk folder
[male_only_remap]                                      # female-engine category -> male category
[category_fallbacks.female] / [category_fallbacks.male]  # substitute when a category is empty

[groups]    # startup volumes per group
[sfx]       # SFX name -> folder
```

## Two documents to read next

- **[Voice & Category Resolution](resolution.md)** — the two lookup chains that turn `PlayVoice(actor, "Category")` into an actual file. This is the part worth understanding before you write any mappings.
- **[Config Reference](reference.md)** — every table and key, its type, and its default.

## A slot can get its files three ways

Each `[[slot]]` supplies audio by any mix of:

1. **A scanned folder** (`path = 'Sound\fx\MyMod\M4'`) holding `<Category>\*.wav` subfolders. Loose files only — folder scans cannot see into BSAs.
2. **Explicit per-category file lists** (`[slot.categories]` *array* values) — no folders needed, and they **can point into BSA archives** (the engine's resource loader resolves them).
3. **Per-category folder references** (`[slot.categories]` *string* values) — one folder scanned for that category, letting several categories share one pool without copies.

Explicit categories (forms 2 and 3) **win** over a same-named folder found by the path scan. See the [Config Reference](reference.md#slot) for the exact syntax.
