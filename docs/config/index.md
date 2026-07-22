# TOML Configuration — Overview

AudioUtil reads a single file, **`Data\SKSE\Plugins\AudioUtil\AudioUtil.toml`**, at `kDataLoaded` (and again on [`ReloadConfig()`](../api/audioutil.md#reloadconfig)). It maps voice-slot ids to folders, categories to subfolders, SFX names to folders, and sets group volumes plus lipsync/PPA options.

!!! info "Who ships this file"
    The plugin's own bundled `AudioUtil.toml` is **SFW-neutral**: it defines no slots and no SFX. **Consumer mods overwrite it via load order** — install the content mod after AudioUtil and its TOML wins. So as a content author, *your* mod ships the `AudioUtil.toml`; there is exactly one active file (last in load order), not a merge.

On a parse error the **previous (or default) settings are kept** and the error is logged to `AudioUtil.log` — a broken edit never leaves the game silent, it just doesn't take effect.

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
