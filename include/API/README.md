# AudioUtil integration kit

Everything another mod needs to talk to AudioUtil, and nothing else. Shipped as
`AudioUtil-API-<version>+.zip` (built with `xmake build sdk`); the same files live in the
AudioUtil repo, so you can also just copy them from there.

The **`+`** means what it looks like: the kit describes that AudioUtil version *and later*.
The API only grows — exports are appended, never reordered or removed — so a kit stays valid
for every newer release. `VERSIONS.txt` inside carries the two numbers to gate an optional
integration on: the C++ interface version and the Papyrus `GetAPIVersion()`.

```
cpp/AudioUtilAPI.h        C++ inter-plugin API (SKSE plugins)
papyrus/AudioUtil.psc     the Papyrus API (script mods compile against this)
papyrus/AudioUtilPPA.psc  optional Accurate Penetration bridge
papyrus/TomlUtil.psc      generic TOML reader/writer hosted by the same DLL
VERSIONS.txt              the two versions to gate on (C interface + Papyrus API)
```

**You do not need AudioUtil installed to build against these**, and nothing here creates a
hard dependency — that is the point of shipping them separately from the mod archive.

## C++ (SKSE plugins)

`cpp/AudioUtilAPI.h` is a **reference, not a library**: no `.lib`, no import library, and the
declarations carry no `dllimport`/`dllexport`, so copying it into your project adds zero
build-time dependency. Resolve the exports at runtime and treat a null module handle as
"AudioUtil not installed":

```cpp
auto h = GetModuleHandleA("AudioUtil.dll");
if (h) {
    auto claim = reinterpret_cast<decltype(&AudioUtil_ClaimMouth)>(
                     GetProcAddress(h, "AudioUtil_ClaimMouth"));
    if (claim) {
        claim(player, lineLengthSeconds, "MyVoiceMod");
    }
}
```

Never call the `AudioUtil_*` names directly — they are declarations of functions that live in
AudioUtil's DLL, so a direct call is an unresolved external at link time.

Feature-detect with `AudioUtil_GetInterfaceVersion()` (packed `MMmmpp`; `10100` = 1.1.0, the
build that added the mouth claims). Exports are append-only, never reordered or removed, so a
version check covers the whole surface — but still null-check the individual pointer you are
about to call, since an older AudioUtil resolves the old names and leaves newer ones null.
`AudioUtil_GetVersion()` reports the mod version instead, packed `MMmmppp`.

## Papyrus (script mods)

Drop the `.psc` files into your compiler's source path and call the globals:

```papyrus
int h = AudioUtil.PlayVoice(akSpeaker, "Moan")
if AudioUtil.IsMouthBusy(akActor)
    ; someone else is speaking through this mouth - leave the phonemes alone
endif
```

Guard an optional dependency with `AudioUtil.GetAPIVersion()`, which returns `0` when the DLL
is absent, and gate any newer call on the version that introduced it (v8 = mouth claims).

## Where the documentation is

- Papyrus API reference: <https://crajjjj.github.io/AudioUtil/api/audioutil/>
- Mouth claims (voice mods + expression mods): <https://crajjjj.github.io/AudioUtil/api/audioutil/#mouth-claims>
- TomlUtil: <https://crajjjj.github.io/AudioUtil/api/tomlutil/>
- Source: <https://github.com/crajjjj/AudioUtil>

GPLv3, same as AudioUtil itself. Copying these headers/scripts into your own project is the
intended use.
