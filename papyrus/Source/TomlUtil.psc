Scriptname TomlUtil Hidden
{Read and write TOML config files from Papyrus. Hosted by AudioUtil.dll but
 fully independent of the audio API - any mod may use this to keep its
 settings in a .toml file instead of JSON/ini.}

; =============================================================================
; CONCEPTS
;
;   file   Path relative to Data\, e.g. "SKSE\Plugins\MyMod\MyMod.toml".
;          Absolute paths and ".." traversal are rejected. Files are parsed
;          lazily on first access and cached; a parse FAILURE is cached too
;          (one warning in AudioUtil.log), so a broken file costs nothing
;          per call and serves defaults until fixed and Reload()ed.
;
;   key    Dotted TOML path: "voice.pcvolume" reads
;              [voice]
;              pcvolume = 60
;          Deeper nesting works the same ("a.b.c").
;
;   defaults   Returned when the file is missing/broken, the key is absent,
;          or the value's type doesn't match the getter. Nothing throws.
;
;   writes  Set* edits the value IN PLACE in the file's text - all user
;          comments and formatting survive, including a comment trailing the
;          edited value. A missing key is inserted directly under its table's
;          [header] line; a missing table (or file) is created. Set* returns
;          false WITH NOTHING WRITTEN when the edit can't be done safely: the
;          file doesn't parse, the key names a table/array (scalars only), or
;          the layout is unsupported (inline tables, quoted keys, tables built
;          from root-level dotted keys). The cache updates on success, so a
;          Get* right after a Set* sees the new value - no Reload() needed.
; =============================================================================

; Version of the TomlUtil API (independent of AudioUtil's audio API version).
; Also the cheapest "is the DLL installed?" probe - a missing DLL returns 0.
int Function GetAPIVersion() global native

; Typed getters. An integer TOML value satisfies GetFloat; nothing else
; converts across types (a string "5" does NOT satisfy GetInt).
int    Function GetInt(string asFile, string asKey, int aiDefault = 0) global native
float  Function GetFloat(string asFile, string asKey, float afDefault = 0.0) global native
string Function GetString(string asFile, string asKey, string asDefault = "") global native
bool   Function GetBool(string asFile, string asKey, bool abDefault = false) global native

; TOML array of strings -> Papyrus array. Non-string elements are skipped;
; missing key/file -> empty array.
string[] Function GetStringArray(string asFile, string asKey) global native

; True if the key exists at all (any type) - use to distinguish "absent"
; from "present with the default's value".
bool Function HasKey(string asFile, string asKey) global native

; Re-parse one file (live tuning: edit the toml, call this in-game).
; Returns false AND KEEPS the previously cached contents on parse failure.
; Console (ConsoleUtil Extended): toml reload "SKSE\Plugins\MyMod\MyMod.toml"
bool Function Reload(string asFile) global native

; Typed writers (TomlUtil API v2+) - persist a value back to the file,
; preserving every comment and all formatting (see "writes" above). True =
; written to disk AND visible to the getters immediately. The literal is
; written in the type asked: SetFloat always keeps a decimal point so the
; TOML value stays float-typed; SetString quotes/escapes as needed.
bool Function SetInt(string asFile, string asKey, int aiValue) global native
bool Function SetFloat(string asFile, string asKey, float afValue) global native
bool Function SetString(string asFile, string asKey, string asValue) global native
bool Function SetBool(string asFile, string asKey, bool abValue) global native

; ===================== console helpers (non-native) =====================
; Back the `toml setint/setfloat/setstring/setbool` console commands
; (SKSE\CustomConsole\TomlUtil.yaml): console args arrive as strings, these
; cast and forward to the writers above. Not intended for mod code - call
; the typed natives directly.

bool Function ConsoleSetInt(string asFile, string asKey, string asValue) global
	return SetInt(asFile, asKey, asValue as int)
EndFunction

bool Function ConsoleSetFloat(string asFile, string asKey, string asValue) global
	return SetFloat(asFile, asKey, asValue as float)
EndFunction

bool Function ConsoleSetString(string asFile, string asKey, string asValue) global
	return SetString(asFile, asKey, asValue)
EndFunction

bool Function ConsoleSetBool(string asFile, string asKey, string asValue) global
	; accepts true/false (any case, via case-insensitive Find) or 1/0
	bool value = asValue == "1" || (StringUtil.GetLength(asValue) == 4 && StringUtil.Find(asValue, "true") == 0)
	return SetBool(asFile, asKey, value)
EndFunction
