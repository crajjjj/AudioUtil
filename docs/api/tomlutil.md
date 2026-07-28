# TomlUtil (config reader/writer)

`Scriptname TomlUtil Hidden` — a generic TOML file reader/writer hosted by `AudioUtil.dll` but **fully independent of the audio API**. Any mod may use it to keep its settings in a `.toml` file instead of JSON/INI. Writes are **comment-preserving** (API v2+).

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

**writes**
: `Set*` edits the value **in place in the file's text** — every comment and all formatting survive, including a comment trailing the edited value. A missing key is inserted directly under its table's `[header]` line; a missing table (or file) is created. On success the cache updates too, so a `Get*` right after a `Set*` sees the new value with no `Reload()`.

!!! note "Writes that refuse"
    `Set*` returns `false` **with nothing written** whenever the edit can't be made safely: the file doesn't parse (a broken file is never "fixed" for you), the key names a table/array (scalars only), or the layout is unsupported — inline tables (`t = { a = 1 }` values *can* be replaced, but new keys can't be inserted into them), quoted keys, and tables built purely from root-level dotted keys. Every edit is re-parsed and the key read back before anything touches the disk, so a failed `Set*` can never corrupt a file.

## Functions

### `GetAPIVersion`

```papyrus
int Function GetAPIVersion() global native
```

Version of the TomlUtil API (independent of AudioUtil's audio API version). Also the cheapest "is the DLL installed?" probe — a missing DLL returns `0`. Currently `2` (v2 added the typed writers).

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

Re-parse one file (live tuning: edit the TOML, call this in-game). Returns `false` **and keeps the previously cached contents** on parse failure. Console: `toml reload "SKSE\Plugins\MyMod\MyMod.toml"` — see [Console Commands](../console.md).

### Typed writers (API v2+)

```papyrus
bool Function SetInt(string asFile, string asKey, int aiValue) global native
bool Function SetFloat(string asFile, string asKey, float afValue) global native
bool Function SetString(string asFile, string asKey, string asValue) global native
bool Function SetBool(string asFile, string asKey, bool abValue) global native
```

Persist one scalar back to the file (see **writes** above). `True` = written to disk **and** immediately visible to the getters. The literal lands in the type asked: `SetFloat` always keeps a decimal point so the TOML value stays float-typed; `SetString` quotes/escapes as needed (TOML literal `'…'` strings when possible). Console: `toml setint`/`setfloat`/`setstring`/`setbool`.

Guard on `GetAPIVersion() >= 2` — on an older DLL the natives don't exist and the call fails to bind.

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

; persist a change from an in-game control (API v2+); comments in the
; file survive the write
if TomlUtil.GetAPIVersion() >= 2
    TomlUtil.SetInt(kFile, "voice.pcvolume", 80)
endif
```
