# AudioUtil

SKSE64 plugin for **native, folder-path-based WAV playback** — no SNDR/SOUN forms, no ESP edits
to add audio. Config-driven in the style of Accurate Penetration's sound configs, plus an optional
PPA C++ API bridge that republishes penetration depth/context to Papyrus as mod events.

Originally built to replace the Hentairim / IVDT ESP sound-descriptor layer; that mapping now ships
as an optional install preset (see [Install](#install)), while the plugin itself is game-neutral.

Target runtime: Skyrim SE 1.6.1170 (CommonLibSSE-NG, all-runtime build).

## How it works

- `RE::BSAudioManager::BuildSoundDataFromFile` + `BSResource::ID::GenerateFromPath` play loose
  audio files directly. `BSSoundHandle` gives per-instance volume, stop, and 3D actor-follow.
- Voice audio is organized as `<voice_root>\<Slot>\<Category>\*.wav` (slots like `F1`, `M1`..`M8`).
  A play request (`PlayVoice(actor, "Aggressive")`) resolves the slot from the actor
  (npc_overrides → voicetype_remap → voicetype_map → default per sex) and shuffle-bag-picks a
  wav from the category folder (no repeats until the deck is exhausted).
- SFX are named in the `[sfx]` table of the config and play the same way.
- Groups (`pc_low`, `pc_high`, `partner_low`, `partner_high`, `sfx`, `oneshot`) provide
  volume/duck/stop semantics.
- Channels (named latest-instance slots) natively provide stop-previous-on-channel behaviour.
- **Lipsync**: `PlayVoice` / `PlayVoiceFromSlot` also move the speaking actor's mouth in sync
  with the clip — the DLL reads the wav's amplitude envelope (100 Hz RMS) and drives the MFG
  `Aah`/`BigAah` phonemes per frame on the game thread, easing with configurable attack/release.
  No dialogue records, no `.lip` baking, works for any loose PCM wav. See `[lipsync]` in the
  TOML and the lipsync natives in `AudioUtil.psc` (`IsLipSyncActive`, `SetLipSyncEnabled`,
  `SetLipSyncGain`, `StopLipSync`). Expression mods that write phonemes themselves should skip
  their own mouth writes while `IsLipSyncActive(actor)` is true.

**Adding a voice slot** = create the folder tree + one `[[slot]]` entry in the TOML. That's it.

Papyrus entry point: the `AudioUtil` script (`papyrus\Source\AudioUtil.psc`). Call e.g.
`AudioUtil.PlayVoice(...)`, `AudioUtil.PlaySFX(...)`, `AudioUtil.SetGroupVolume(...)`.

## Install

Plain, directly-installable mod (any mod manager), **SFW by default**. It installs the DLL,
scripts, and a neutral `Data\SKSE\Plugins\AudioUtil\AudioUtil.toml` with no slots/SFX predefined
— point it at your own folders.

Game-specific presets are **not** shipped here. A mod that uses AudioUtil ships its own
`Data\SKSE\Plugins\AudioUtil\AudioUtil.toml` and, loaded after AudioUtil, overwrites the neutral
default. For example, **Hentairim p+** carries the full IVDT voice-slot / category / SFX mapping
and enables the PPA bridge.

## Layout

- `src\` — plugin source (Config, FolderCache, AudioEngine, InstanceManager, PapyrusAPI, PPABridge)
- `include\API\AccuratePenetrationAPI.h` — recreated PPA v1 interface (dynamically resolved,
  fails closed when PPA is absent)
- `papyrus\Source\AudioUtil.psc` — native declarations + `PlayAndWait` wrappers
  (master copy; `Hentairim p+ 3.0.4\Scripts\Source\` carries a copy for compilation)
- `papyrus\Source\AudioUtilTest.psc` — console test harness (`cgf "AudioUtilTest.T1"`)
- `dist\` — mod-shaped output: DLL, compiled scripts + sources, and the SFW-neutral default
  `SKSE\Plugins\AudioUtil\AudioUtil.toml`
- `lib\commonlibsse-ng` — CommonLibSSE-NG as a git submodule (pinned to the same commit
  Beeing Female NG builds against; clone with `git clone --recurse-submodules`, or run
  `git submodule update --init --recursive` in an existing checkout)

## Build

```
xmake f -m release
xmake                     # DLL only -> dist\SKSE\Plugins\
xmake build papyrus       # Pyro: papyrus\Source\*.psc -> dist\Scripts + Release\AudioUtil.zip
xmake build release       # same, but rebuilds the DLL first (fresh DLL in the zip)
```

Requires VS 2022 (v143, C++23) and xmake ≥ 2.9.5. Papyrus compilation and release packaging
are owned by Pyro via `AudioUtil.ppj` (`Zip="true"` + `<ZipFiles>`, same pattern as
Beeing Female NG): every Pyro run compiles the scripts and refreshes `Release\AudioUtil.zip`,
a ready-to-install mod archive. The VSCode default build task (`Ctrl+Shift+B`) runs the same
ppj. The xmake targets auto-locate `pyro.exe` from the VSCode papyrus-lang extension; override
with `PYRO_EXE`, and the game root with `SKYRIM_GAME_PATH`
(default `C:\SteamLibrary\steamapps\common\Skyrim Special Edition`).

With `xmake f --copy_to_mod=y` and `XSE_TES5_MODS_PATH` set (e.g. `E:\nefaram\mods`),
`dist\*` is also copied into `<mods>\AudioUtil` after each DLL build for direct dev testing.

## Config

`Data\SKSE\Plugins\AudioUtil\AudioUtil.toml` — see the comments in the default file.
All key/name matching is case- and space-insensitive. `ReloadConfig()` (or
`cgf "AudioUtilTest.TReload"`) re-parses and rescans without restarting the game.

`sound_flags` / `sound_priority` are the `BuildSoundDataFromFile` parameters (default `0x1A` /
`128`). If audio doesn't play or isn't 3D, sweep values in-game with
`AudioUtil.DebugPlayFile(path, actor, flags, priority)` (harness: `T2F`).

## PPA bridge

When `AccuratePenetration.dll` is present (and `[ppa] enable = true`), the plugin subscribes to
its animation updates and:

- keeps a per-receiver snapshot readable via `GetPPAContext` / `GetPPADepth` /
  `GetPPAVaginalOpening` / `GetPPAAnalOpening`
- sends mod event `AudioUtilPPA_Update` (sender = receiver actor, numArg = depth,
  strArg = context bitmask as decimal string) at most every `event_rate_ms` per receiver,
  or immediately when the context bitmask changes
- sends `AudioUtilPPA_End` when a receiver's interaction ends

Context bits: 1=Vaginal 2=Anal 4=Oral 8=Aggressive 16=FemDom 32=Loving 64=Dirty 128=Boobjob
256=Handjob 512=Footjob 1024=Masturbation.

## Known constraints

- **Loose files only** for the folder scan — audio packed into a BSA is invisible to it.
- WAVs should be PCM; mono attenuates in 3D, stereo may play 2D.
- Lipsync needs a loose PCM wav to read the envelope from — `.xwm` and BSA-packed audio play
  normally but skip the mouth. Creatures without facegen data are silently skipped.
- `Sound.PlayAndWait` semantics are emulated in Papyrus (`WaitForHandle` polls `IsHandlePlaying`).
