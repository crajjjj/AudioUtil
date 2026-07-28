#include "TomlStore.h"

#include "TomlEdit.h"

#include <filesystem>
#include <fstream>
#include <toml++/toml.hpp>

namespace TomlStore
{
	namespace
	{
		struct Entry
		{
			toml::table table;
			bool        ok = false;
		};

		std::unordered_map<std::string, Entry> g_files;
		std::mutex                             g_lock;

		std::string NormalizePath(const std::string& a_file)
		{
			std::string out = a_file;
			std::replace(out.begin(), out.end(), '/', '\\');
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return out;
		}

		// keep lookups inside Data\ - reject absolute paths and traversal
		bool PathIsSafe(const std::string& a_norm)
		{
			if (a_norm.empty() || a_norm.find(':') != std::string::npos ||
				a_norm.starts_with('\\') || a_norm.find("..") != std::string::npos) {
				return false;
			}
			return true;
		}

		bool ParseInto(const std::string& a_norm, Entry& a_entry)
		{
			const auto diskPath = "Data\\" + a_norm;
			try {
				a_entry.table = toml::parse_file(diskPath);
				a_entry.ok = true;
				logger::info("Parsed {}", diskPath);
				return true;
			} catch (const toml::parse_error& e) {
				logger::warn("Failed to parse {}: {} (line {})", diskPath,
					e.description(), e.source().begin.line);
			} catch (const std::exception& e) {
				logger::warn("Failed to read {}: {}", diskPath, e.what());
			}
			return false;
		}

		// caller holds g_lock
		Entry* GetEntry(const std::string& a_file)
		{
			const auto norm = NormalizePath(a_file);
			if (!PathIsSafe(norm)) {
				logger::warn("Rejected unsafe path '{}'", a_file);
				return nullptr;
			}
			const auto it = g_files.find(norm);
			if (it != g_files.end()) {
				return &it->second;
			}
			auto& entry = g_files[norm];
			ParseInto(norm, entry);  // failure is cached too - no re-parse spam
			return &entry;
		}

		toml::node_view<const toml::node> Find(const std::string& a_file, const std::string& a_key)
		{
			// caller holds g_lock
			const auto* entry = GetEntry(a_file);
			if (!entry || !entry->ok) {
				return {};
			}
			return std::as_const(entry->table).at_path(a_key);
		}

		// shared writer behind the typed Set* fronts: splice the literal into the
		// file's text (comment-preserving, see TomlEdit.h), persist, refresh cache
		bool SetLiteral(const std::string& a_file, const std::string& a_key,
			const std::string& a_literal)
		{
			std::scoped_lock lock{ g_lock };
			const auto norm = NormalizePath(a_file);
			if (!PathIsSafe(norm)) {
				logger::warn("Rejected unsafe path '{}'", a_file);
				return false;
			}
			const auto diskPath = "Data\\" + norm;

			std::string text;  // missing file -> edit from empty (creates it)
			if (std::ifstream in{ diskPath, std::ios::binary }; in) {
				text.assign(std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{});
			}

			const auto edited = TomlEdit::SetInText(text, a_key, a_literal);
			if (!edited) {
				logger::warn("TomlUtil: cannot set {} = {} in {} (unparseable file, "
					"non-scalar target, or unsupported layout)", a_key, a_literal, diskPath);
				return false;
			}

			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(diskPath).parent_path(), ec);
			std::ofstream out{ diskPath, std::ios::binary | std::ios::trunc };
			if (!out || !(out << *edited)) {
				logger::warn("TomlUtil: failed to write {}", diskPath);
				return false;
			}
			out.close();

			// refresh the cache from the exact text we validated, so readers see
			// the new value immediately without re-touching the disk
			try {
				auto& entry = g_files[norm];
				entry.table = toml::parse(*edited);
				entry.ok = true;
			} catch (...) {
				g_files.erase(norm);  // can't happen (validated); re-parse lazily if it does
			}
			logger::info("TomlUtil: set {} = {} in {}", a_key, a_literal, diskPath);
			return true;
		}
	}

	std::optional<std::int64_t> GetInt(const std::string& a_file, const std::string& a_key)
	{
		std::scoped_lock lock{ g_lock };
		return Find(a_file, a_key).value<std::int64_t>();
	}

	std::optional<double> GetFloat(const std::string& a_file, const std::string& a_key)
	{
		std::scoped_lock lock{ g_lock };
		// an integer literal is a valid float value for consumers
		return Find(a_file, a_key).value<double>();
	}

	std::optional<std::string> GetString(const std::string& a_file, const std::string& a_key)
	{
		std::scoped_lock lock{ g_lock };
		return Find(a_file, a_key).value<std::string>();
	}

	std::optional<bool> GetBool(const std::string& a_file, const std::string& a_key)
	{
		std::scoped_lock lock{ g_lock };
		return Find(a_file, a_key).value<bool>();
	}

	std::vector<std::string> GetStringArray(const std::string& a_file, const std::string& a_key)
	{
		std::scoped_lock lock{ g_lock };
		std::vector<std::string> out;
		const auto node = Find(a_file, a_key);
		if (const auto* arr = node.as_array()) {
			out.reserve(arr->size());
			for (const auto& item : *arr) {
				if (const auto str = item.value<std::string>()) {
					out.push_back(*str);
				}
			}
		}
		return out;
	}

	bool HasKey(const std::string& a_file, const std::string& a_key)
	{
		std::scoped_lock lock{ g_lock };
		return static_cast<bool>(Find(a_file, a_key));
	}

	bool SetInt(const std::string& a_file, const std::string& a_key, std::int64_t a_value)
	{
		return SetLiteral(a_file, a_key, TomlEdit::Literal(a_value));
	}

	bool SetFloat(const std::string& a_file, const std::string& a_key, double a_value)
	{
		return SetLiteral(a_file, a_key, TomlEdit::Literal(a_value));
	}

	bool SetString(const std::string& a_file, const std::string& a_key, const std::string& a_value)
	{
		return SetLiteral(a_file, a_key, TomlEdit::Literal(a_value));
	}

	bool SetBool(const std::string& a_file, const std::string& a_key, bool a_value)
	{
		return SetLiteral(a_file, a_key, TomlEdit::Literal(a_value));
	}

	bool Reload(const std::string& a_file)
	{
		std::scoped_lock lock{ g_lock };
		const auto norm = NormalizePath(a_file);
		if (!PathIsSafe(norm)) {
			return false;
		}
		Entry fresh;
		if (!ParseInto(norm, fresh)) {
			return false;  // keep whatever was cached before
		}
		g_files[norm] = std::move(fresh);
		return true;
	}
}
