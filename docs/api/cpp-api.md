# C++ API (SKSE plugins)

For **SKSE plugins written in C++**, AudioUtil exposes a small native inter-plugin API — the [`PlayFile` / `PlayFileWithLipSync`](audioutil.md#playfile) natives, plus the [mouth-claim](audioutil.md#mouth-claims) group, callable directly in C++ with no Papyrus round-trip. Use it when your own DLL wants to fire a loose audio file (a UI sound, a scripted line, a reactive one-shot) with AudioUtil's engine-level playback, captions and lipsync — instead of going through a script.

The single consumer header is **`include/API/AudioUtilAPI.h`** (self-contained — copy it into your project). It also ships on its own as **`AudioUtil-API-<version>+.zip`** (the integration kit: this header plus the `.psc` files, with no mod content), so you can build an integration without installing AudioUtil at all. The `+` is a minimum: the kit describes that AudioUtil version and later, since exports are only ever appended.

!!! info "How to use the header"
    `AudioUtilAPI.h` is a **reference, not a library**: it gives you the documented signatures to cast `GetProcAddress` results to. There is no `.lib` and no import library anywhere, so copying it into your project adds **zero** build-time dependency on AudioUtil. (Including it is optional — hand-writing the few signatures you use works identically.)

    Never call the `AudioUtil_*` names directly: they're declarations of functions that live in *our* DLL, so a direct call is an unresolved-external link error. Always call through a function pointer obtained from `GetProcAddress`, as shown below.

!!! danger "The Papyrus API ≠ this API — but the exports mirror it exactly"
    This C++ API covers the two file-playback natives and the five mouth-claim calls: every export is `AudioUtil_<PapyrusName>` and runs the **exact same code path** as the Papyrus native — same handles, groups, channels, caption sidecars and lipsync guards. Everything else (`PlayVoice`, `PlaySFX`, `PlayFolder`, handle/group/channel ops, lipsync control, introspection, TomlUtil) is **not** exported; for those, call the [Papyrus API](index.md). A handle returned here lives in the **same id space**, so script code can `AudioUtil.StopHandle()` / `IsHandlePlaying()` a sound your DLL started.

## Resolving the exports

Consumers do **not** link against AudioUtil. Resolve the exports at runtime with `GetProcAddress`; a null module handle means AudioUtil is not installed, so treat the integration as optional.

```cpp
#include <Windows.h>          // GetModuleHandleA, GetProcAddress
#include "AudioUtilAPI.h"     // declares AudioUtil_PlayFile, ...

// One holder for the exports you actually call. Each pointer's TYPE is taken
// straight from the header via decltype(&AudioUtil_Xxx), so you never hand-retype
// a signature. NOTE decltype catches a *signature* change in our API at compile
// time; it does NOT catch an unresolved export -- that stays a null pointer and
// crashes at the first call. Null-check the specific pointer before use.
// (decltype only inspects the declaration; it creates no link dependency.)
struct AudioUtilC {
    decltype(&AudioUtil_GetVersion)           GetVersion           = nullptr;
    decltype(&AudioUtil_GetInterfaceVersion)  GetInterfaceVersion  = nullptr;
    decltype(&AudioUtil_PlayFile)             PlayFile             = nullptr;
    decltype(&AudioUtil_PlayFileWithLipSync)  PlayFileWithLipSync  = nullptr;

    // "AudioUtil installed & the version export resolved" -- NOT "every export
    // resolved". A future export you add here may be missing from an older
    // AudioUtil, so null-check the SPECIFIC pointer before use.
    bool available() const { return GetVersion != nullptr; }
};

// Call once, after AudioUtil's DLL has loaded (see the tip below). If AudioUtil
// isn't installed every pointer stays null and available() returns false, so the
// whole integration is opt-in with no hard dependency.
inline AudioUtilC LoadAudioUtil() {
    AudioUtilC au;
    HMODULE h = GetModuleHandleA("AudioUtil.dll");   // null => AudioUtil absent
    if (!h) return au;

    // Resolve each export BY NAME and store it as a callable pointer. The
    // reinterpret_cast target is the pointer's own type, so the string name
    // and the signature always agree. (C4191 is MSVC's expected warning for a
    // FARPROC->function-pointer cast; the push/disable/pop lets /W4 /WX pass.)
#pragma warning(push)
#pragma warning(disable : 4191)
    au.GetVersion          = reinterpret_cast<decltype(au.GetVersion)>         (GetProcAddress(h, "AudioUtil_GetVersion"));
    au.GetInterfaceVersion = reinterpret_cast<decltype(au.GetInterfaceVersion)>(GetProcAddress(h, "AudioUtil_GetInterfaceVersion"));
    au.PlayFile            = reinterpret_cast<decltype(au.PlayFile)>           (GetProcAddress(h, "AudioUtil_PlayFile"));
    au.PlayFileWithLipSync = reinterpret_cast<decltype(au.PlayFileWithLipSync)>(GetProcAddress(h, "AudioUtil_PlayFileWithLipSync"));
#pragma warning(pop)
    return au;
}
```

Then call **through the pointer**, never the `AudioUtil_*` name directly:

```cpp
// Store the result once (e.g. in a global or your plugin's state object).
AudioUtilC au = LoadAudioUtil();

if (au.available() && au.PlayFile) {
    RE::Actor* speaker = ...;

    // 3D at the actor, volume group "sfx", exclusivity channel "mymod_voice"
    int32_t id = au.PlayFile("Sound\\FX\\MyMod\\whoosh.wav", speaker, 1.0f, "sfx", "");

    // spoken line: also drives the actor's mouth (lip curves or envelope)
    if (au.PlayFileWithLipSync)
        au.PlayFileWithLipSync("Sound\\Voice\\MyMod.esp\\line01.wav", speaker,
                               1.0f, "pc_high", "mymod_voice");
}
```

`au.PlayFile(...)` calls the resolved pointer. Writing `AudioUtil_PlayFile(...)` instead would be an unresolved-external link error — that name only exists inside *our* DLL.

!!! tip "Resolve after AudioUtil has loaded, play after `kDataLoaded`"
    `GetModuleHandleA` only sees AudioUtil once its DLL is loaded. Call `LoadAudioUtil()` on or after SKSE's `kPostLoad`/`kPostPostLoad` message (or lazily on first use) — not from a static initializer, which runs too early. Don't **play** anything before `kDataLoaded`: the game's audio engine and AudioUtil's config aren't up yet.

## Version gating

Gate on both presence and version before using any export — `au.available()` catches "AudioUtil not installed"; the version compare catches "installed but too old for the export you need". Then null-check the exact pointer (`if (au.PlayFileWithLipSync) …`) — that's the guard that actually proves an individual export resolved.

- **`AudioUtil_GetVersion()`** — packed `MMmmppp` mod/DLL version (`major*10000000 + minor*100000 + patch`, e.g. `909` for 0.9.9, `10000000` for 1.0.0). Tracks the release version automatically.
- **`AudioUtil_GetInterfaceVersion()`** — the C API surface version, packed `MMmmpp` (`10000` == 1.0.0), bumped only when exports are added. Exports are **append-only** (never reordered or removed), so a value check is enough to feature-detect.

The C API first shipped in AudioUtil **0.9.9** (interface `10000`) — on older installs the module handle resolves but every `GetProcAddress` returns null, which the per-pointer null checks handle for free. The mouth-claim exports, listing included, arrived in **0.9.19** (interface `10100`): gate on `AudioUtil_GetInterfaceVersion() >= 10100`, or simply null-check each pointer.

## Threading & lifecycle

!!! note "Same threads as the Papyrus natives"
    The playback exports are the exact code path of the Papyrus natives, which run on Papyrus VM threads — call them from the game thread, an SKSE task, or a VM thread. They are **not** validated from arbitrary background threads. All arguments are null-safe (`nullptr` path returns `0`; `nullptr` group/channel mean `""`).

!!! note "The mouth claims are safe from any thread"
    `AudioUtil_ClaimMouth`, `AudioUtil_ReleaseMouth`, `AudioUtil_IsMouthClaimed`, `AudioUtil_GetMouthClaimOwner` and `AudioUtil_GetMouthClaimTimeLeft` touch no engine state: they are in-memory work behind a single mutex, never held across a call into the game or the Papyrus VM, so a voice mod can claim and release straight from its own audio/decode worker. **`AudioUtil_IsMouthBusy` and `AudioUtil_GetClaimedActors` are the exceptions** — the first reads the actor's facegen dialogue data and AudioUtil's live lipsync entries, the second resolves form ids through the form table, so give those two the game thread like the playback calls. Claims are session state: they are dropped on `kPreLoadGame`/`kNewGame` along with playing sounds.

Playing sounds stop automatically on `kPreLoadGame`/`kNewGame` (AudioUtil stops all audio and resets lipsync), so handles don't survive a load — don't cache them across saves.

## Semantics

Identical to the Papyrus natives, so the [Papyrus reference](audioutil.md#playfile) and the [shared concepts](index.md#concepts-shared-by-every-play-call) (handles, volume math, groups, channels) are the source of truth. In short:

- `dataRelPath` is `Data`-relative (`"Sound\\FX\\MyMod\\whoosh.wav"`; forward slashes are normalized). Playable formats: **wav, xwm, fuz** — loose or BSA-packed (a `.fuz` plays via its decoded PCM cache).
- `follow` 3D-positions the sound at that actor and tracks it while playing; `nullptr` plays flat/2D at the listener.
- `volume` is `0.0`–`1.0`; effective volume = `volume × group_volume × duck_factor`, re-applied live.
- `group` joins a [volume/duck bucket](index.md#group); `channel` is an [exclusivity lane](index.md#channel) (starting a sound on an occupied channel stops the previous instance). Empty/null = none.
- Returns the instance handle: `> 0` success, `0` = nothing played. Failures are logged to `AudioUtil.log`, never thrown.
- A same-named `.toml` **caption sidecar** next to the file shows its text as a game subtitle attributed to `follow` ([captions](audioutil.md#captions); loose files only).
- `AudioUtil_PlayFileWithLipSync` additionally drives `follow`'s mouth like a voice line — authored `.lip`/fuz phoneme curves when available, else the amplitude envelope (loose PCM wav or fuz). The global `[lipsync]` toggle and the gag / tongue / player-dialogue guards all apply. `AudioUtil_PlayFile` **never** moves the mouth.

## Function reference

| Export | Returns |
|--------|---------|
| `uint32_t AudioUtil_GetVersion()` | Packed mod/DLL version (`MMmmppp`) |
| `uint32_t AudioUtil_GetInterfaceVersion()` | Packed C API version (`MMmmpp`) |
| `int32_t AudioUtil_PlayFile(const char* dataRelPath, RE::Actor* follow, float volume, const char* group, const char* channel)` | Instance handle (`> 0`) or `0`; never drives the mouth |
| `int32_t AudioUtil_PlayFileWithLipSync(const char* dataRelPath, RE::Actor* follow, float volume, const char* group, const char* channel)` | Same, plus voice-call lipsync on `follow` |
| `void AudioUtil_ClaimMouth(RE::Actor* actor, float seconds, const char* owner)` | — ([mouth claims](audioutil.md#mouth-claims); deadline capped at 30 s, `owner` keys the claim) |
| `void AudioUtil_ReleaseMouth(RE::Actor* actor, const char* owner)` | — (clears only this owner's claim) |
| `bool AudioUtil_IsMouthClaimed(RE::Actor* actor)` | Is any foreign claim live on this actor |
| `bool AudioUtil_IsMouthBusy(RE::Actor* actor)` | Engine dialogue **or** AudioUtil lipsync **or** a live claim |
| `uint32_t AudioUtil_GetMouthClaimOwner(RE::Actor* actor, char* buffer, uint32_t size)` | Chars written into `buffer` (owner tag, always null-terminated) |
| `float AudioUtil_GetMouthClaimTimeLeft(RE::Actor* actor)` | Seconds left on the claim, `0.0` when unclaimed |
| `uint32_t AudioUtil_GetClaimedActors(RE::Actor** out, uint32_t max)` | Claim count; fills `out` with up to `max` claimed actors (`out = nullptr` counts only) |
