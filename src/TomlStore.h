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

	// Re-parse one file. Returns false (keeping the previous cache) on failure.
	bool Reload(const std::string& a_file);
}
