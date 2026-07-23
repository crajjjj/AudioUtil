# AudioUtil (core player)

`Scriptname AudioUtil Hidden` — the native folder-based audio player. Read [Overview & Concepts](index.md) first for the handle / volume / group / channel model that every `Play*` call shares.

All name and key matching (categories, slots, groups, SFX names) is **case- and space-insensitive**: `"Battle Cry"` == `"BattleCry"` == `"battlecry"`.

## Core

### `GetAPIVersion`

```papyrus
int Function GetAPIVersion() global native
```

API version of the loaded DLL, for compatibility checks. `0` = DLL not installed. Increases only when signatures/behavior change incompatibly. Currently `1`.

### `ReloadConfig`

```papyrus
bool Function ReloadConfig() global native
```

Re-read `AudioUtil.toml` and rescan all slot folders in-game — no restart needed. Returns `false` if the file failed to parse, in which case the **previous config stays active**. Console: `cgf "AudioUtil.ReloadConfig"`.

## Playback

### `PlayVoice`

```papyrus
int Function PlayVoice(Actor akActor, string category, float volume = 1.0, string group = "", string channel = "", bool blockLipSync = false) global native
```

Play a voice line for an actor. Resolves **which** voice pack (slot) the actor uses, then **which** folder inside it the `category` names, then shuffle-picks a wav. Returns a handle; `0` if no slot or no audio resolved.

- **Slot resolution** and **category resolution** are described in full under [Voice & Category Resolution](../config/resolution.md).
- `blockLipSync = true` plays this one line **without moving the speaker's mouth** — a per-call opt-out. If your mod owns the actor's face across a whole scene, pass it `true` on every line while the face is up (see [the tip under lipsync](#setlipsyncgain)).

```papyrus
int h = AudioUtil.PlayVoice(npc, "BattleCry")
AudioUtil.PlayVoice(npc, "Taunt", 0.8, "npc_voice")            ; + volume, group
AudioUtil.PlayVoice(npc, "Taunt", 1.0, "npc_voice", "npc_main") ; channel: replaces prior line
```

### `PlayVoiceFromSlot`

```papyrus
int Function PlayVoiceFromSlot(string slot, string category, Actor akFollow, float volume = 1.0, string group = "", string channel = "", bool blockLipSync = false) global native
```

Same as `PlayVoice`, but the slot is named **explicitly** (`"F1"`, `"M4"`, `"C2"`, …) instead of resolved from an actor — for samples, tests, or when the caller already decided the voice. `akFollow` only provides the 3D position.

### `PlaySFX`

```papyrus
int Function PlaySFX(string sfxName, Actor akFollow, float volume = 1.0, string group = "sfx", string channel = "") global native
```

Play a named SFX. `sfxName` resolves **first as a category of the sfx slot** (`SFX0` by default; set via [`[general] sfx_slot`](../config/reference.md#general)), **then the flat `[sfx]` table** — so an sfx pool can use a scanned folder, an explicit file list (BSA-capable), or a folder ref, exactly like a voice category. See [SFX resolution](../config/reference.md#sfx-the-sfx-slot-sfx-table). Defaults into the **`sfx` group** so SFX volume/ducking applies unless you override it.

### `PlayFile`

```papyrus
int Function PlayFile(string dataRelativePath, Actor akFollow, float volume = 1.0, string group = "", string channel = "") global native
```

Play one specific file. Path is relative to `Data\` (`'Sound\fx\MyMod\a.wav'`) and may resolve to a **loose file or to audio packed inside a BSA** — the engine's resource loader handles both (loose wins over archive).

### `PlayFolder`

```papyrus
int Function PlayFolder(string dataRelativeFolder, Actor akFollow, float volume = 1.0, string group = "", string channel = "") global native
```

Play a random file from a loose folder (scanned on first use, then cached as a shuffle bag).

!!! warning "Folder scans can't see into BSAs"
    `PlayFolder` (and folder-string categories, and the `[sfx]` table) scan the loose filesystem only. For **BSA-packed audio** use `PlayFile` or an explicit `[slot.categories]` file list in the TOML — those go through the engine's resource loader, which resolves archives.

## Handles

### `IsHandlePlaying`

```papyrus
bool Function IsHandlePlaying(int handle) global native
```

`True` while the instance is audible. Also `true` during a short **startup grace (~0.4 s)** right after `Play*` returns: the engine starts streams asynchronously and briefly reports "not playing" for a sound that is about to start.

### `StopHandle`

```papyrus
bool Function StopHandle(int handle) global native
```

Stop the instance now. Returns `true` if it was alive and stopped.

### `GetHandleDuration`

```papyrus
float Function GetHandleDuration(int handle) global native
```

Clip length in seconds. May return `0.0` while the stream is still being prepared — see [`WaitForHandle`](#wrappers) for how it copes with that.

### `SetHandleVolume`

```papyrus
Function SetHandleVolume(int handle, float volume) global native
```

Change one instance's base volume mid-play (group multipliers still apply).

## Groups and channels

Conventional group names: `pc_high` / `pc_low` (player voice), `partner_high` / `partner_low` (NPC voice), `sfx` (effects), `oneshot` (ambient one-shots). Any other name works — groups are created on first use.

### `SetGroupVolume`

```papyrus
Function SetGroupVolume(string group, float volume) global native
```

Set a group's volume (`0.0`–`1.0`). Applies immediately to playing members and to everything started later. **MCM sliders typically call this** — the TOML `[groups]` values are only the startup state.

### `DuckGroup` / `UnduckGroup`

```papyrus
Function DuckGroup(string group, float factor = 0.0) global native
Function UnduckGroup(string group) global native
```

`DuckGroup` temporarily scales a group by `factor` (`0.0` = silent, `1.0` = no duck) **without** touching its stored volume — e.g. duck background voice while an important line plays. Ducks do **not** stack; the last factor wins. `UnduckGroup` restores the group to its normal volume (equivalent to factor `1.0`).

### `StopGroup` / `StopAllAudio` / `StopChannel`

```papyrus
Function StopGroup(string group) global native
Function StopAllAudio() global native
Function StopChannel(string channel) global native
```

- `StopGroup` — stop every playing instance in the group.
- `StopAllAudio` — stop everything AudioUtil is playing and clear all channel bindings. Does **not** touch group volumes or the config.
- `StopChannel` — stop whatever occupies the channel (if anything) and free it.

## Lipsync

`PlayVoice` / `PlayVoiceFromSlot` automatically move the speaking actor's mouth in sync with the clip's loudness: the DLL reads the wav's amplitude envelope and drives the MFG `Aah` / `BigAah` phonemes per frame. Works for **loose PCM wav files**; xwm or BSA-packed audio plays normally but skips the mouth. Configure defaults in the TOML `[lipsync]` table.

Lipsync is also suppressed automatically for a **gagged** actor (one wearing a device configured in the TOML [`[gag]`](../config/reference.md#gag) table) — the device owns the mouth, so the DLL won't fight it. No Papyrus call needed.

!!! tip "Playing nice with expression mods"
    Only the two phonemes above are touched, and they are zeroed when the clip ends. If your mod sets phonemes itself (e.g. an expression cycler), skip your own mouth writes for an actor while `IsLipSyncActive(actor)` is `true` to avoid fighting over the jaw. If your mod fully **owns** the face (an overlay that sets its own expression), pass `blockLipSync = true` on the lines you play for that actor while the face is up so AudioUtil never touches its mouth (see the tip above).

### `IsLipSyncActive`

```papyrus
bool Function IsLipSyncActive(Actor akActor) global native
```

`True` while AudioUtil is driving this actor's mouth (a voice line with a readable envelope is playing and lipsync is enabled).

### `StopLipSync`

```papyrus
Function StopLipSync(Actor akActor) global native
```

Fade this actor's mouth closed now; the audio itself keeps playing.

### `SetLipSyncEnabled` / `IsLipSyncEnabled`

```papyrus
Function SetLipSyncEnabled(bool enable) global native
bool Function IsLipSyncEnabled() global native
```

Master switch (runtime; the TOML value is restored on `ReloadConfig`). Turning it off fades all active mouths closed.

### `SetLipSyncGain`

```papyrus
Function SetLipSyncGain(float gain) global native
```

Mouth-open strength, `0.0`–`2.0` (`1.0` = envelope as-is). For MCM sliders.

!!! tip "Owning an actor's face across many lines"
    There is no standing per-actor lipsync block. If your mod takes over an actor's face (an ahegao / expression overlay), pass **`blockLipSync = true`** on each [`PlayVoice`](#playvoice) for that actor while the face is up — decide it per call from your own face-ownership state. Because it's decided per line rather than latched, there's no state to leak or to be cleared on load, and two systems that both play lines for the same actor can't clobber each other's block. For a whole category that should never lipsync (oral SFX, climax pools), list it in [`[lipsync] block_categories`](../config/reference.md#lipsync) instead.

## Introspection

### `GetSlotForActor`

```papyrus
string Function GetSlotForActor(Actor akActor) global native
```

Which slot id `PlayVoice` would resolve for this actor right now (`"M4"`, `"F1"`, …; `""` if none). Useful for debugging voice assignment.

### `GetCategoryFileCount`

```papyrus
int Function GetCategoryFileCount(string slot, string category) global native
```

Number of playable files behind slot/category after full category resolution (aliases/fallbacks applied). `0` = a `PlayVoice` would fail.

### `CategoryExists`

```papyrus
bool Function CategoryExists(string slot, string category) global native
```

`True` if slot/category resolves to at least one file. Cheap way to guard optional content ("play the rare line only if the pack ships it").

## Debug

### `DebugPlayFile`

```papyrus
int Function DebugPlayFile(string dataRelativePath, Actor akFollow, int flags, int priority) global native
```

Play a file with explicit `BuildSoundDataFromFile` flags/priority instead of the TOML's `sound_flags` / `sound_priority` — for experimenting when a file is silent or behaves oddly. **Not for production use.** If a sound won't play or isn't 3D-positioned, sweep values here.

## Wrappers

These are **non-native** Papyrus helpers (they live in `AudioUtil.psc`, not the DLL). They block the **calling** Papyrus thread by polling `IsHandlePlaying` + `Utility.Wait`.

### `WaitForHandle`

```papyrus
Function WaitForHandle(int handle) global
```

Block until the instance finishes, or a safety timeout: clip duration + 1 s, or **30 s** when the duration isn't known yet. Returns immediately for dead/invalid handles. Polls every `0.1 s`, so expect up to ~0.1 s of overshoot past the actual clip end.

### `PlayVoiceAndWait` / `PlaySFXAndWait`

```papyrus
Function PlayVoiceAndWait(Actor akActor, string category, float volume = 1.0, string group = "", string channel = "") global
Function PlaySFXAndWait(string sfxName, Actor akFollow, float volume = 1.0, string group = "sfx", string channel = "") global
```

Play, then block until the line finishes. Same arguments and resolution as `PlayVoice` / `PlaySFX`; the handle is discarded.

### `Play`

```papyrus
Function Play(string category, Actor akActor, bool waitForCompletion = true, float volume = 1.0, string group = "", string channel = "") global
```

Drop-in equivalent of a legacy `PlaySound(Sound form, Actor, wait)` helper: a category string instead of a `Sound` form, with optional blocking.
