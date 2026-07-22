# TomlUtil (config reader)

`Scriptname TomlUtil Hidden` — a generic TOML file reader hosted by `AudioUtil.dll` but **fully independent of the audio API**. Any mod may use it to keep its settings in a `.toml` file instead of JSON/INI. Read-only by design.

## Concepts

**file**
: Path relative to `Data\`, e.g. `"SKSE\Plugins\MyMod\MyMod.toml"`. Absolute paths and `..` traversal are **rejected**. Files are parsed lazily on first access and cached; a parse **failure is cached too** (one warning in `AudioUtil.log`), so a broken file costs nothing per call and serves defaults until fixed and `Reload()`ed.

**key**
: Dotted TOML path. `"voice.pcvolume"` reads:

    ```toml
    [voice]
    pcvolume = 60
    ```

    Deeper nesting works the same (`"a.b.c"`).

**defaults**
: Returned when the file is missing/broken, the key is absent, or the value's type doesn't match the getter. **Nothing throws.**

!!! note "Why read-only"
    TOML write-back would destroy user comments and formatting. Runtime-changeable state belongs in JSON / StorageUtil; TOML is for hand-authored config.

## Functions

### `GetAPIVersion`

```papyrus
int Function GetAPIVersion() global native
```

Version of the TomlUtil API (independent of AudioUtil's audio API version). Also the cheapest "is the DLL installed?" probe — a missing DLL returns `0`. Currently `1`.

### Typed getters

```papyrus
int    Function GetInt(string asFile, string asKey, int aiDefault = 0) global native
float  Function GetFloat(string asFile, string asKey, float afDefault = 0.0) global native
string Function GetString(string asFile, string asKey, string asDefault = "") global native
bool   Function GetBool(string asFile, string asKey, bool abDefault = false) global native
```

Type conversion is deliberately strict: an integer TOML value satisfies `GetFloat`, but **nothing else converts across types** — a string `"5"` does **not** satisfy `GetInt`, and a mismatch returns your default.

### `GetStringArray`

```papyrus
string[] Function GetStringArray(string asFile, string asKey) global native
```

A TOML array of strings → a Papyrus array. Non-string elements are skipped; a missing key/file returns an **empty array**.

### `HasKey`

```papyrus
bool Function HasKey(string asFile, string asKey) global native
```

`True` if the key exists at all (any type) — use it to distinguish "absent" from "present with a value that happens to equal the default".

### `Reload`

```papyrus
bool Function Reload(string asFile) global native
```

Re-parse one file (live tuning: edit the TOML, call this in-game). Returns `false` **and keeps the previously cached contents** on parse failure. Console: `cgf "TomlUtil.Reload" "SKSE\Plugins\MyMod\MyMod.toml"`.

## Example

```papyrus
string kFile = "SKSE\\Plugins\\MyMod\\MyMod.toml"

if TomlUtil.GetAPIVersion() == 0
    ; DLL missing — fall back to your own defaults
    return
endif

int    pcVolume = TomlUtil.GetInt(kFile, "voice.pcvolume", 60)
bool   enabled  = TomlUtil.GetBool(kFile, "general.enable", true)
string[] packs  = TomlUtil.GetStringArray(kFile, "packs.enabled")
```
