# HentairimAudio

SKSE64 plugin that replaces the Hentairim / IVDT ESP sound-descriptor layer with **native,
folder-path-based WAV playback** — no SNDR/SOUN forms, no ESP edits to add audio. Config-driven
in the style of Accurate Penetration's sound configs, plus a PPA C++ API bridge that republishes
penetration depth/context to Papyrus as mod events.

Target runtime: Skyrim SE 1.6.1170 (CommonLibSSE-NG, all-runtime build).

## How it works

- `RE::BSAudioManager::BuildSoundDataFromFile` + `BSResource::ID::GenerateFromPath` play loose
  audio files directly. `BSSoundHandle` gives per-instance volume, stop, and 3D actor-follow.
- Voice audio is organized as `Sound\fx\IVDT\<Slot>\<Category>\*.wav` (slots `F1`, `M1`..`M8`).
  A play request (`PlayVoice(actor, "Aggressive")`) resolves the slot from the actor
  (npc_overrides → voicetype_remap → voicetype_map → default per sex) and shuffle-bag-picks a
  wav from the category folder (no repeats until the deck is exhausted).
- SFX are named in the `[sfx]` table of the config and play the same way.
- Groups (`pc_low`, `pc_high`, `partner_low`, `partner_high`, `sfx`, `oneshot`) provide the
  volume/duck/stop semantics the old SoundCategory mute hacks provided.
- Channels (named latest-instance slots) natively replace the StorageUtil
  `HentairimLatestSoundInstance` / `contactsoundinstance` stop-previous patterns.

**Adding a voice slot** = create the folder tree + one `[[slot]]` entry in the TOML. That's it —
this replaces the old 5-place CK/MCM/script edit.

## Layout

- `src\` — plugin source (Config, FolderCache, AudioEngine, InstanceManager, PapyrusAPI, PPABridge)
- `include\API\AccuratePenetrationAPI.h` — recreated PPA v1 interface (dynamically resolved,
  fails closed when PPA is absent)
- `papyrus\Source\HentairimAudio.psc` — native declarations + `PlayAndWait` wrappers
  (master copy; `Hentairim p+ 3.0.4\Scripts\Source\` carries a copy for compilation)
- `papyrus\Source\HentairimAudioTest.psc` — console test harness (`cgf "HentairimAudioTest.T1"`)
- `dist\` — mod-shaped output: DLL, default `HentairimAudio.toml`, compiled scripts
- `lib\commonlibsse-ng` — vendored CommonLibSSE-NG (copied from Beeing Female NG, known-good)

## Build

```
xmake f -m release
xmake                     # DLL only -> dist\SKSE\Plugins\
xmake build papyrus       # Pyro: papyrus\Source\*.psc -> dist\Scripts + Release\HentairimAudio.zip
xmake build release       # same, but rebuilds the DLL first (fresh DLL in the zip)
```

Requires VS 2022 (v143, C++23) and xmake ≥ 2.9.5. Papyrus compilation and release packaging
are owned by Pyro via `HentairimAudio.ppj` (`Zip="true"` + `<ZipFiles>`, same pattern as
Beeing Female NG): every Pyro run compiles the scripts and refreshes
`Release\HentairimAudio.zip`, a ready-to-install mod archive. The VSCode default build task
(`Ctrl+Shift+B`) runs the same ppj. The xmake targets auto-locate `pyro.exe` from the VSCode
papyrus-lang extension; override with `PYRO_EXE`, and the game root with `SKYRIM_GAME_PATH`
(default `C:\SteamLibrary\steamapps\common\Skyrim Special Edition`).

With `xmake f --copy_to_mod=y` and `XSE_TES5_MODS_PATH` set (e.g. `E:\nefaram\mods`),
`dist\*` is also copied into `<mods>\HentairimAudio` after each DLL build.

## Config

`Data\SKSE\Plugins\HentairimAudio\HentairimAudio.toml` — see the comments in the default file.
All key/name matching is case- and space-insensitive. `ReloadConfig()` (or
`cgf "HentairimAudioTest.TReload"`) re-parses and rescans without restarting the game.

`sound_flags` / `sound_priority` are the `BuildSoundDataFromFile` parameters (default `0x1A` /
`128`). If audio doesn't play or isn't 3D, sweep values in-game with
`HentairimAudio.DebugPlayFile(path, actor, flags, priority)` (harness: `T2F`).

## PPA bridge

When `AccuratePenetration.dll` is present (and `[ppa] enable = true`), the plugin subscribes to
its animation updates and:

- keeps a per-receiver snapshot readable via `GetPPAContext` / `GetPPADepth` /
  `GetPPAVaginalOpening` / `GetPPAAnalOpening`
- sends mod event `HentairimPPA_Update` (sender = receiver actor, numArg = depth,
  strArg = context bitmask as decimal string) at most every `event_rate_ms` per receiver,
  or immediately when the context bitmask changes
- sends `HentairimPPA_End` when a receiver's interaction ends

Context bits: 1=Vaginal 2=Anal 4=Oral 8=Aggressive 16=FemDom 32=Loving 64=Dirty 128=Boobjob
256=Handjob 512=Footjob 1024=Masturbation.

## Known constraints

- **Loose files only** for the folder scan — audio packed into a BSA is invisible to it.
- WAVs should be PCM; mono attenuates in 3D, stereo may play 2D.
- The female voice pack (Karryn) lives in a separate MO2 mod — at runtime the merged VFS makes
  it visible; offline tooling must look in both mod folders.
- `Sound.PlayAndWait` semantics are emulated in Papyrus (`WaitForHandle` polls `IsHandlePlaying`).
