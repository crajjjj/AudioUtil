# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Mod Is

**AudioUtil** is an SKSE64 plugin (C++ DLL, built on CommonLibSSE-NG for Skyrim SE 1.6.1170, all-runtime) plus a small Papyrus API layer. It provides **native, folder-path-based WAV playback** — playing loose audio files directly with **no SNDR/SOUN sound-descriptor forms and no ESP edits**.

Core mechanism: `RE::BSAudioManager::BuildSoundDataFromFile` + `BSResource::ID::GenerateFromPath` play loose files; a `BSSoundHandle` gives per-instance volume / stop / 3D actor-follow. Voice audio is organized on disk as `<slot path>\<Category>\*.wav`, where the slot path is the `[[slot]]` `path` (a full `Sound\...` Data-relative folder); slots are named like `F1`, `M1`..`M8`, `C1`.. A `PlayVoice(actor, "Category")` call resolves the actor's slot (npc_overrides → voicetype_remap → voicetype_map → race_map → per-sex default), then **shuffle-bag** picks a wav from the category folder (no repeats until the deck empties).

Beyond raw playback it also provides:
- **Lipsync** — reads the wav's amplitude envelope (100 Hz RMS) and drives the actor's MFG `Aah`/`BigAah` phonemes per game frame. No `.lip` baking, no dialogue records; works for any loose PCM wav.
- **PPA bridge** — optional integration with the third-party Accurate Penetration plugin (`AccuratePenetration.dll`), republishing penetration depth/context to Papyrus as throttled mod events. See [PPA Bridge](#ppa-bridge-accurate-penetration).
- **TomlUtil** — a generic, audio-independent TOML-file reader API hosted by the same DLL, usable by any consumer mod.

The plugin itself is **game-neutral and ships SFW by default** (the default `AudioUtil.toml` defines no slots or SFX). Game-specific presets (voice-slot / category / SFX mappings) are shipped by **consumer mods** (e.g. *Hentairim p+*, *SLO VE*) via a TOML that overwrites the neutral default through load order.

Author: `crajjjj`. License: GPLv3.

## Build

Two independent build steps, both driven by **xmake** (`xmake.lua`).

### C++ DLL (xmake + CommonLibSSE-NG)

```
xmake f -m release        # configure release (VS2022 v143, C++23)
xmake                     # build DLL -> dist\SKSE\Plugins\AudioUtil.dll
```

- Requires **VS 2022 (v143, C++23)** and **xmake ≥ 2.9.5**.
- **CommonLibSSE-NG is a git submodule** at `lib/commonlibsse-ng`. Before the first build you MUST run `git clone --recurse-submodules` or `git submodule update --init --recursive` — it is not vendored.
- `toml++` (v3.4.0) comes via xmake `add_requires`; the version is pinned in `xmake-requires.lock` (`package.requires_lock` enabled).
- Sources: `add_files("src/**.cpp")`; includes `src` and `include`; PCH is `src/PCH.h`.
- Exports are controlled by `exports.def` (only `SKSEPlugin_Load`).
- `after_build` copies the DLL (and PDB in debug) into `dist\SKSE\Plugins\`. With `xmake f --copy_to_mod=y` and env `XSE_TES5_MODS_PATH` set (e.g. `E:\nefaram\mods`), it also copies `dist\*` into `<mods>\AudioUtil` for live testing.
- Debug: `MTd`, `DEBUG`, `/bigobj`. Release: `MT`, `NDEBUG`, `/O fastest`, debug symbols.

### Papyrus scripts + release packaging (Pyro)

```
xmake build papyrus       # Pyro: papyrus\Source\*.psc -> dist\Scripts + Release\AudioUtil.zip
xmake build release       # rebuilds the DLL first (add_deps AudioUtil), then runs Pyro
```

- `scripts/pyro.lua` mirrors `papyrus\Source\*.psc` into `dist\Scripts\Source`, then runs `pyro.exe -i AudioUtil.ppj --game-path <game>`.
- `pyro.exe` is auto-located from the VSCode papyrus-lang extension (`%USERPROFILE%\.vscode\extensions\joelday.papyrus-lang-vscode-*\pyro\pyro.exe`). Override with env **`PYRO_EXE`**.
- Game root defaults to `C:\SteamLibrary\steamapps\common\Skyrim Special Edition`; override with env **`SKYRIM_GAME_PATH`**.
- **VSCode default build task** (`Ctrl+Shift+B`, `.vscode/tasks.json`) runs the same `AudioUtil.ppj` via the `pyro` task type, with `gamePath` hardcoded to the same Steam path.

`AudioUtil.ppj` has `Zip="true"`, so **every Pyro run refreshes `Release\AudioUtil.zip`** — a ready-to-install mod archive (`<ZipFiles>` RootDir `.\dist`, matching `Scripts\*.pex`, `Scripts\Source\*.psc`, `SKSE\*.*`). This is the BFNG convention where Pyro owns release packaging.

### Env vars summary

| Var | Purpose | Default |
|---|---|---|
| `PYRO_EXE` | path to `pyro.exe` | auto-located from VSCode extension |
| `SKYRIM_GAME_PATH` | game root | `C:\SteamLibrary\steamapps\common\Skyrim Special Edition` |
| `XSE_TES5_MODS_PATH` | dev-copy target for `--copy_to_mod=y` | (unset) |

## C++ Source Architecture (`src/`)

`main` wires SKSE messaging; on `kDataLoaded` it runs `Config::Load` → `FolderCache::Rebuild` → `GagState::Resolve` → `InstanceManager::ApplyConfigGroupVolumes` → `LipSync::ApplyConfig` → `PPABridge::TryConnect`. A `PlayVoice` call flows: `PapyrusAPI::ResolveSlotForActor` (uses `Config` maps) → `ResolveGaggedKey` (gag redirect, see `GagState`) → `FolderCache::ResolveVoiceKey` + `PickNext` → `AudioEngine::PlayPath` → `InstanceManager::Register` (+ optional channel) → `LipSync::Start`.

| File | Role |
|---|---|
| `main.cpp` | SKSE entry point; spdlog init; registers Papyrus funcs; runs the kDataLoaded config/scan pipeline; on kPreLoadGame/kNewGame stops all audio and resets lipsync |
| `AudioEngine` | Thin wrapper over `BuildSoundDataFromFile`; `PlayPath(dataRelPath, follow, volume[, flags, priority])` → `BSSoundHandle`. `follow` (an actor) 3D-positions the sound at that actor; passing `nullptr` plays it flat/2D at full volume. Voice calls pass `follow` only when `[general] voice_3d` is set (default true) |
| `Config` | Merges the base `Data\SKSE\Plugins\AudioUtil\AudioUtil.toml` then every `AudioUtil\config\*.toml` overlay (sorted filename order) into one `Settings` struct (slots, resolution maps, category aliases/fallbacks, sfx table, group volumes, flags/priority, ppa + lipsync + gag). `MergeFile(settings, root, isBase)` accumulates **additive** collections (slots/maps/sfx/`[gag].keywords` union, last-writer-wins per key) from every file, but **global scalar** sections (`[general]`, `[ppa]`, `[lipsync]`, and `[gag]` enable/default_category) are honored **only from the base** — an overlay that sets them is ignored with a warning. `[[slot]]` is keyed by `id` (whole-slot last-wins, no deep merge); `race_map` is sorted longest-hint-first once after all files merge. Slot `sex` is `'F'`/`'M'`/`'A'` — `'A'` (all/neutral: creatures, sfx) matches either sex on explicit routes but is skipped by the blind default-by-sex fallback; for the category layer it shares the male aliases/fallbacks (where creature/neutral fallbacks are authored) and skips `male_only_remap`. Each file parses independently — a broken overlay is skipped, and settings are only replaced if ≥1 file parsed. Slot `path` and `[sfx]` values are always full `Sound\...` Data-relative paths (resolved identically). A `[slot.categories]` value may be an **array** (explicit file list, BSA-capable) or a **string** (one folder to scan; `Sound\...` = full Data path, else relative to the slot's path); a slot may name a `fallback` slot consulted per-category when it resolves nothing (chain capped at 4 hops), and a `gag_slot` used when the actor is gagged. `[gag]` parses `enable`, `default_category`, and a `keywords` list of `Plugin.esp|FormID` gag markers. `Normalize` (lowercase + strip non-alphanumerics), `MakeNpcKey`, `FindSlot`. Keeps prior settings on parse error |
| `GagState` | Resolves `[gag]` keyword strings to live `BGSKeyword*` via `TESDataHandler` (on load + reload); `IsGagged(actor)` walks the actor's worn items for any gag keyword. Dormant when no keywords resolve. Used by `PapyrusAPI::ResolveGaggedKey` to route a gagged speaker's voice through the slot's `gag_slot` (with `default_category` as the muffled catch-all) |
| `FolderCache` | Scans slot roots + per-category folder refs + sfx folders, registers explicit file lists (which win over same-named scanned folders); resolves slot+category (aliases / male_only_remap / category fallbacks, then the slot's `fallback` slot chain) to a folder; shuffle-bag `PickNext` + `FileCount`. **Scans see loose files only — BSA audio needs explicit file lists** |
| `InstanceManager` | Owns live sound instances behind `int32` ids; per-instance IsPlaying/Stop/Duration/volume; group volume/duck/stop; channel exclusivity (`PlayOnChannel` stops previous occupant); StopAll |
| `LipSync` | Drives MFG `Aah`/`BigAah` phonemes from a wav's amplitude envelope in sync with a playing instance (attack/release/gain/min-level). Skips non-PCM/BSA-packed audio and creatures without facegen. Suppressed for gagged actors via `GagState::IsGagged` (the device owns the mouth): checked at `Start` and re-checked on a 500 ms throttle so a gag equipped mid-line hands the mouth over. Also skipped per-call (`blockLipSync`) and per-category (`[lipsync] block_categories`, matched on the requested category name in `PlayVoice`/`PlayVoiceFromSlot` — for oral-sfx / climax pools that shouldn't move the mouth) |
| `PPABridge` | Dynamically connects to `AccuratePenetration.dll`; per-receiver snapshot cache under a mutex; sends throttled `AudioUtilPPA_Update`/`AudioUtilPPA_End` mod events via the SKSE task interface |
| `PapyrusAPI` | Registers all natives across `AudioUtil`, `AudioUtilPPA`, `TomlUtil`; holds actor→slot resolution (`ResolveSlotForActor`, `PickFromSlotList` with per-NPC spreading via `StableLocalID`), the `ResolveGaggedKey` gag-routing helper, and the shared `PlayFromKey` helper |
| `TomlStore` | Generic lazily-parsed/cached TOML reader backing `TomlUtil`; caches parse failure (one warning); missing file/key/type-mismatch returns caller's default; rejects absolute paths and `..` traversal |
| `PCH.h` | Precompiled header |

## Papyrus API Surface (`papyrus/Source/*.psc`)

Every `Play*` returns an `int` instance handle: `>0` success, `0` = nothing played. Effective volume = `volume × group_volume × duck_factor`. A **channel** is an exclusivity lane (playing on an occupied channel stops the previous instance).

**`AudioUtil.psc`** (`Hidden`) — core API. Natives:
- Play: `PlayVoice(Actor, category, volume, group, channel, blockLipSync=false)`, `PlayVoiceFromSlot(slot, category, akFollow, volume, group, channel, blockLipSync=false)`, `PlaySFX(sfxName, Actor, volume, group="sfx", channel)` (resolves `sfxName` first as a category of the **sfx slot** — `[general] sfx_slot`, default `SFX0` — then the flat `[sfx]` table, via `PapyrusAPI::ResolveSfxKey`; the sfx slot gives sfx pools the full `[[slot]]` toolset incl. BSA file-lists), `PlayFile(dataRelPath, Actor, volume, group, channel)`, `PlayFolder(dataRelFolder, Actor, volume, group, channel)`. `blockLipSync=true` = play this one line without moving the mouth (per-call opt-out; the standing per-actor state is `SetLipSyncBlocked`).
- Handles: `IsHandlePlaying`, `StopHandle`, `GetHandleDuration`, `SetHandleVolume`.
- Groups/channels: `SetGroupVolume`, `DuckGroup(group, factor=0.0)`, `UnduckGroup`, `StopGroup`, `StopAllAudio`, `StopChannel`.
- Lipsync: `IsLipSyncActive(Actor)`, `StopLipSync(Actor)`, `SetLipSyncEnabled(bool)`, `IsLipSyncEnabled()`, `SetLipSyncGain(float)`, `SetLipSyncBlocked(Actor, bool, callerMod="")` / `IsLipSyncBlocked(Actor)` (per-actor block for mods that own the face, e.g. an ahegao overlay; drops the active entry without zeroing the mouth; cleared on game load; callerMod is logged with each toggle).
- Introspection: `GetSlotForActor(Actor)`, `GetCategoryFileCount(slot, category)`, `CategoryExists(slot, category)`.
- Config/debug: `GetAPIVersion()`, `ReloadConfig()`, `DebugPlayFile(path, Actor, flags, priority)`.
- Papyrus (non-native) wrappers: `WaitForHandle`, `PlayVoiceAndWait`, `PlaySFXAndWait`, `Play(category, Actor, waitForCompletion, ...)`.

**`AudioUtilPPA.psc`** (`Hidden`) — optional PPA bridge: `IsConnected()`, `SetEventRate(ms)`, `GetContext(akReceiver)` (bitmask), `GetDepth(Actor)`, `GetVaginalOpening(Actor)`, `GetAnalOpening(Actor)`. Consumers read the cached snapshot cheaply and/or handle mod events `AudioUtilPPA_Update` / `AudioUtilPPA_End`.

**`TomlUtil.psc`** (`Hidden`) — generic, audio-independent TOML reader: `GetAPIVersion()`, `GetInt/GetFloat/GetString/GetBool(asFile, asKey, default)`, `GetStringArray(asFile, asKey)`, `HasKey`, `Reload(asFile)`. File path is Data-relative; dotted keys (`voice.pcvolume`); read-only by design.

**`AudioUtilTest.psc`** (`Hidden`) — console harness (needs ConsoleUtil): `cgf "AudioUtilTest.T1"` etc. T1 basic play, T2F flags/priority sweep, T3 PlayVoice, T4 shuffle-bag, T5 SFX, T6 channel replacement, T7 group duck, T8 PPA status, TReload.

Native registration lives in `PapyrusAPI::RegisterFuncs`, split across `SCRIPT_NAME="AudioUtil"`, `PPA_SCRIPT_NAME="AudioUtilPPA"`, `TOML_SCRIPT_NAME="TomlUtil"`. Both `API_VERSION` and `TOML_API_VERSION` are `1`.

## PPA Bridge (Accurate Penetration)

- `include/API/AccuratePenetrationAPI.h` is a **recreated** PPA v1 interface (from the public PPA SKSE docs), resolved **dynamically** — the code never links against `AccuratePenetration.dll`. Key types: `SceneContext` bit flags (Vaginal=1, Anal=2, Oral=4, Aggressive=8, FemDom=16, Loving=32, Dirty=64, Boobjob=128, Handjob=256, Footjob=512, Masturbation=1024), `InteractionPartner` (penetrationDepth …), `AnimationUpdateEvent`, `InterfaceV1`. Entrypoint `AccuratePenetration_GetAPI_V1`.
- `PPABridge::TryConnect()` (kDataLoaded, gated on `[ppa] enable`) does GetModuleHandleW → GetProcAddress → version/size check → RegisterAnimationUpdateListener. The listener runs on PPA's thread **every frame for every actor**; it copies a per-receiver snapshot (context, depth = max across selfInteraction + partners, opening values, ending flag) into a mutex-guarded map. It sends `AudioUtilPPA_End` on `ending` (then erases state), else `AudioUtilPPA_Update` at most once per `event_rate_ms` per receiver **or immediately when the context bitmask changes**. Events marshal to the game thread via `SKSE::GetTaskInterface()` → `ModCallbackEvent`.
- `docs/ppa-api-assumptions.md` records the PPA author's answers: opening values are magic unsigned numbers (0.0 = closed, no defined scale — calibrate empirically); context bits are scene **classification** (like SexLab tags), NOT live collision — for "physically inserted now" require a context bit **AND** `depth > 0`; `ending` is always sent (state-erase is safe); the per-frame firehose must stay in C++ to avoid Papyrus VM lock contention.

Design: the firehose stays in the DLL; Papyrus only sees throttled events (2 s default) and cheap polls. Fails closed when PPA is absent.

## Distribution Layout (`dist/`)

`dist/` maps directly onto a Skyrim `Data\` install:

```
dist/
  SKSE/Plugins/AudioUtil.dll                   ← built by xmake
  SKSE/Plugins/AudioUtil/AudioUtil.toml        ← hand-written neutral SFW base config (owns globals)
  SKSE/Plugins/AudioUtil/config.example/*.toml  ← hand-written additive-overlay template (NOT loaded; copy into config\)
  SKSE/Plugins/AudioUtil/config/*.toml          ← additive overlays shipped by consumer add-on mods (merged on top)
  Scripts/AudioUtil.pex  AudioUtilPPA.pex       ← compiled by Pyro
          AudioUtilTest.pex  TomlUtil.pex
  Scripts/Source/*.psc                         ← source copies mirrored by pyro.lua
```

`.gitignore` ignores all of `dist/**` **except** the tracked, hand-written `dist/SKSE/Plugins/AudioUtil/AudioUtil.toml` and `dist/SKSE/Plugins/AudioUtil/config.example/*.toml` — everything else (DLL from xmake, `.pex`/`.psc` from Pyro) is generated. Pyro zips the whole `dist/` tree into `Release\AudioUtil.zip`.

## Gotchas & Non-obvious Details

- **Master Papyrus sources live in `papyrus\Source\`.** `dist\Scripts\Source\` is a generated mirror — never edit it (pyro.lua overwrites it).
- **CommonLibSSE-NG is a submodule** — init it before building (see [Build](#build)).
- All key/name matching in config and API is **case- and space-insensitive** (`Config::Normalize`).
- **Loose-file constraint:** folder scans (slot roots, folder-string categories, sfx) can't see BSA-packed audio. To play from a BSA, use `PlayFile` or an explicit `[slot.categories]` file list (the engine's resource loader resolves those from BSAs). **Lipsync needs a loose PCM wav.**
- `sound_flags` / `sound_priority` default `0x1A` / `128`. If audio is silent or not 3D-positioned, sweep values with `DebugPlayFile`.
- Logs go to the SKSE logs dir as `AudioUtil.log`.
- Hardcoded game paths appear in `AudioUtil.ppj`, `.vscode/tasks.json`, `scripts/pyro.lua`, and the README — override via env for other machines.
- The default TOML ships with **no** slots/SFX (SFW-neutral by design). A preset mod that needs custom globals ships its **own** base `AudioUtil.toml` (overwriting the neutral one via load order); pure add-ons ship additive `AudioUtil\config\*.toml` overlays. Globals (`[general]`/`[ppa]`/`[lipsync]`/`[gag]` toggles) are **base-only** — overlays that set them are ignored+warned; additive data (slots, sfx, maps, gag keywords) unions across all files (see the `Config` row). Whole slots are last-wins by `id` in sorted filename order.
