# AudioUtil

**AudioUtil** is an SKSE64 plugin (a C++ DLL built on CommonLibSSE-NG for Skyrim SE 1.6.1170, all-runtime) plus a small Papyrus API layer. It plays **loose WAV files directly by folder path** — with **no SNDR/SOUN sound-descriptor forms and no ESP edits**.

Point it at a folder of `.wav` files and call `AudioUtil.PlayVoice(actor, "Category")` from Papyrus. The plugin resolves which voice pack the actor uses, shuffle-picks a file from the category folder, plays it as a 3D sound that follows the actor, and — for loose PCM wavs — drives the actor's mouth in sync with the clip's loudness.

!!! info "This is a dependency, not a standalone mod"
    The plugin is **game-neutral and ships SFW by default**: the bundled `AudioUtil.toml` defines no voice slots and no SFX. Content mods (voice-slot / category / SFX mappings) are shipped by **consumer mods** that overwrite that TOML through load order. These pages are the developer reference for **authors building on AudioUtil** — the Papyrus API and the TOML config schema. All examples use neutral placeholder categories (`BattleCry`, `Taunt`, …).

## What it gives you

- **Folder-based voice playback** — `PlayVoice(actor, category)` resolves the actor's voice slot (per-NPC pin → voicetype → race → default by sex) and shuffle-picks a wav from `<slot>\<category>\`. No repeats until the bag empties.
- **SFX, files, and folders** — `PlaySFX`, `PlayFile` (loose *or* BSA-packed), `PlayFolder`.
- **Per-instance & group control** — every `Play*` returns an `int` handle: stop it, revolume it, query duration. Group volumes and ducking apply live to all members; channels give exclusivity lanes (a new line on a channel stops the previous one).
- **Automatic lipsync** — reads the wav's amplitude envelope and drives the MFG `Aah`/`BigAah` phonemes per frame. No `.lip` baking, no dialogue records; works for any loose PCM wav.
- **PPA bridge** — optional integration with the third-party *Accurate Penetration* plugin, republishing depth/context to Papyrus as throttled mod events. See [AudioUtilPPA](api/ppa.md).
- **TomlUtil** — a generic, audio-independent TOML reader hosted by the same DLL, usable by any mod. See [TomlUtil](api/tomlutil.md).

## Start here

<div class="grid cards" markdown>

- :material-language-lua: **[Papyrus API](api/index.md)**

    The functions you call from `.psc`. Start with [Overview & Concepts](api/index.md) for the handle / volume / group / channel model, then the per-script references.

- :material-file-cog: **[TOML Configuration](config/index.md)**

    How a consumer mod's `AudioUtil.toml` maps voice slots, categories, and SFX. See [Voice & Category Resolution](config/resolution.md) for the lookup order and the full [Config Reference](config/reference.md).

</div>

## Minimal example

```papyrus
; slot auto-resolved from the actor; shuffle-picks a file from the category folder
int h = AudioUtil.PlayVoice(npc, "BattleCry")

; volume + group, then a channel that replaces the actor's previous line
AudioUtil.PlayVoice(npc, "Taunt", 0.8, "npc_voice", "npc_main")

; block the calling thread until the clip ends
AudioUtil.PlayVoiceAndWait(npc, "BattleCry")
```

The changelog and downloads live on the [GitHub releases page](https://github.com/crajjjj/AudioUtil/releases). AudioUtil is licensed under **GPLv3**.
