#pragma once

namespace TomlStore
{
	// All lookups: a_file is a path relative to Data\ ("SKSE\Plugins\MyMod\MyMod.toml"),
	// a_key is a dotted TOML path ("voice.pcvolume"). Files are parsed lazily and
	// cached; a parse failure is cached too (one warning) so a broken file doesn't
	// re-parse every call. Missing file/key/type mismatch -> the caller's default.

	std::optional<std::int64_t> GetInt(const std::string& a_file, const std::string& a_key);
	std::optional<double>       GetFloat(const std::string& a_file, const std::string& a_key);
	std::optional<std::string>  GetString(const std::string& a_file, const std::string& a_key);
	std::optional<bool>         GetBool(const std::string& a_file, const std::string& a_key);
	std::vector<std::string>    GetStringArray(const std::string& a_file, const std::string& a_key);
	bool                        HasKey(const std::string& a_file, const std::string& a_key);

	// Comment-preserving writers. The value is spliced into the file's TEXT in
	// place (all comments/formatting survive; a new key is inserted under its
	// table's header, a new table appended), the result is validated, written to
	// disk, and the cache refreshed - readers see the new value immediately.
	// Returns false with nothing written when the edit can't be made safely:
	// unsafe path, unparseable file, non-scalar target, or unsupported layout
	// (inline tables, quoted keys, tables built from root-level dotted keys).
	// A missing file is created (parent directories included).
	bool SetInt(const std::string& a_file, const std::string& a_key, std::int64_t a_value);
	bool SetFloat(const std::string& a_file, const std::string& a_key, double a_value);
	bool SetString(const std::string& a_file, const std::string& a_key, const std::string& a_value);
	bool SetBool(const std::string& a_file, const std::string& a_key, bool a_value);

	// Re-parse one file. Returns false (keeping the previous cache) on failure.
	bool Reload(const std::string& a_file);
}
