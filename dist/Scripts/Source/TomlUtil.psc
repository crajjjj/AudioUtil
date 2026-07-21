Scriptname TomlUtil Hidden
{Read TOML config files from Papyrus. Hosted by AudioUtil.dll but fully
 independent of the audio API - any mod may use this to keep its settings in
 a .toml file instead of JSON/ini.}

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
;   Read-only by design: TOML write-back would destroy user comments and
;          formatting. Runtime-changeable state belongs in JSON/StorageUtil.
; =============================================================================

; Version of the TomlUtil API (independent of AudioUtil's audio API version).
; Also the cheapest "is the DLL installed?" probe - a missing DLL returns 0.
int Function GetAPIVersion() global native

; Typed getters. An integer TOML value satisfies GetFloat; nothing else
; converts across types (a string "5" does NOT satisfy GetInt).
int    Function GetInt(string file, string key, int default = 0) global native
float  Function GetFloat(string file, string key, float default = 0.0) global native
string Function GetString(string file, string key, string default = "") global native
bool   Function GetBool(string file, string key, bool default = false) global native

; TOML array of strings -> Papyrus array. Non-string elements are skipped;
; missing key/file -> empty array.
string[] Function GetStringArray(string file, string key) global native

; True if the key exists at all (any type) - use to distinguish "absent"
; from "present with the default's value".
bool Function HasKey(string file, string key) global native

; Re-parse one file (live tuning: edit the toml, call this in-game).
; Returns false AND KEEPS the previously cached contents on parse failure.
; Console: cgf "TomlUtil.Reload" "SKSE\Plugins\MyMod\MyMod.toml"
bool Function Reload(string file) global native
