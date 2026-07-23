# Papyrus API — Overview & Concepts

AudioUtil exposes three `Hidden` script namespaces, all backed by the one DLL. You call them as global native functions (`AudioUtil.PlayVoice(...)`) — there are no forms to fill and nothing to attach in the Creation Kit.

| Script | Purpose | Reference |
|--------|---------|-----------|
| `AudioUtil` | The core player: voice / SFX / file / folder playback, handles, groups, channels, lipsync. | [AudioUtil](audioutil.md) |
| `AudioUtilPPA` | Optional bridge to the third-party *Accurate Penetration* plugin. Inert unless PPA is installed and enabled. | [AudioUtilPPA](ppa.md) |
| `TomlUtil` | Generic, audio-independent TOML file reader. Any mod may use it for its own settings. | [TomlUtil](tomlutil.md) |

The source scripts (`Scripts\Source\AudioUtil.psc` etc.) ship with the mod and carry the same documentation inline.

## Is the DLL installed?

`GetAPIVersion()` returns `0` when the DLL is missing, so it doubles as a presence probe. Both `AudioUtil` and `TomlUtil` expose their own version; they are independent counters.

```papyrus
if AudioUtil.GetAPIVersion() == 0
    Debug.Notification("AudioUtil is not installed")
    return
endif
```

The current API version is **1** for both `AudioUtil` and `TomlUtil`. The version increases only when signatures or behavior change **incompatibly**.

!!! note "Adding optional params doesn't bump the version"
    Papyrus fills defaulted trailing parameters automatically, so a new optional argument on an existing native (e.g. `blockLipSync` on `PlayVoice`) is backward-compatible and does **not** raise `GetAPIVersion()`. Guard on `>= N` only for genuinely new behavior, and be prepared for a missing-native error if you call a function an older install never registered.

## Concepts shared by every `Play*` call

Every playback function (`PlayVoice`, `PlayVoiceFromSlot`, `PlaySFX`, `PlayFile`, `PlayFolder`) shares the same model.

### Handle

Every `Play*` returns an `int` **instance handle**:

- `> 0` — success. The handle stays valid until the sound ends, is stopped, or is replaced on its channel.
- `0` — nothing played (unknown category, empty folder, invalid actor, …).

Failures are **logged to `AudioUtil.log`, never thrown**. Calls on a dead or `0` handle are safe no-ops, so you rarely need to guard them.

### Volume

`volume` is the `0.0`–`1.0` base volume of this instance. The **effective** volume is:

```
effective = volume × group_volume × group_duck_factor
```

…and it is re-applied live whenever the group's volume or duck changes.

### Group

A group is a **volume / duck bucket** the instance joins. Any string; `""` = no group. `[groups]` in the TOML sets startup volumes; [`SetGroupVolume`](audioutil.md#setgroupvolume) / [`DuckGroup`](audioutil.md#duckgroup-unduckgroup) change them at runtime and affect **all current and future** members of the group. Conventional names: `pc_high` / `pc_low`, `partner_high` / `partner_low`, `sfx`, `oneshot` — but any name works and is created on first use.

### Channel

A channel is an **exclusivity lane**. Any string; `""` = none. Starting a sound on a channel first **stops whatever previously played on that channel**. Use one channel per logical stream (e.g. one per actor's body SFX) so a new line *replaces* the old instead of stacking.

### File selection — the shuffle bag

A category or folder with N files acts as a **shuffle bag**: random order, no repeats until the bag empties, then it reshuffles. You don't pick individual files for voice/SFX categories — AudioUtil spreads them out for you. The reshuffle also guards the **seam** between bags — the first clip of a fresh bag is never the same file that just played, so you never hear a back-to-back repeat.

**Weighting by duplication.** In an explicit `[slot.categories]` file list you can list the same file more than once to make it more likely — a clip listed twice is drawn about twice as often. The no-back-to-back guard compares the file *path*, not the list position, so a weighted clip still never plays twice in a row (unless it's the *only* distinct file in the pool, where a repeat is unavoidable).

### 3D positioning

Sounds **follow the given actor's position** while playing. Pass a `None` actor to play unattached (roughly 2D, at the listener).

## Blocking vs. fire-and-forget

The `Play*` natives return immediately. For "play this line and wait until it finishes" there are non-native Papyrus wrappers ([`PlayVoiceAndWait`](audioutil.md#wrappers), `PlaySFXAndWait`, `Play`, `WaitForHandle`) that poll `IsHandlePlaying` + `Utility.Wait` — no suspended VM stack, no latent-function marshalling. They block the **calling** Papyrus thread only.
