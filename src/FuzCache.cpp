#include "FuzCache.h"

#include <filesystem>
#include <fstream>

namespace FuzCache
{
	namespace
	{
		// FUZ container: "FUZE" magic, u32 version, u32 lip-data size, the LIP
		// block, then the audio payload (RIFF: form "XWMA" = xWMA, "WAVE" = wav)
		constexpr std::size_t FUZ_HEADER_SIZE = 12;

		constexpr auto CACHE_DIR_REL = "Sound\\AudioUtilFuzCache"sv;  // data-relative

		// resolved results per normalized fuz path; "" = known failure (don't
		// re-attempt a broken file every call)
		std::unordered_map<std::string, std::string> g_resolved;
		std::mutex g_lock;

		std::string NormalizeKey(const std::string& a_path)
		{
			std::string out = a_path;
			std::replace(out.begin(), out.end(), '/', '\\');
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return out;
		}

		// stable per-path hash for the cache filename (std::hash isn't
		// guaranteed stable across builds; FNV-1a is)
		std::uint64_t Fnv1a64(std::string_view a_text)
		{
			std::uint64_t hash = 0xCBF29CE484222325ull;
			for (const unsigned char ch : a_text) {
				hash ^= ch;
				hash *= 0x100000001B3ull;
			}
			return hash;
		}

		// whole resource through the engine's loader, so BSA-packed fuz work.
		// Chunked binary_read until short read - the stream has no size accessor.
		std::vector<std::uint8_t> ReadResource(const std::string& a_dataRelPath)
		{
			std::vector<std::uint8_t> out;
			RE::BSResourceNiBinaryStream stream{ a_dataRelPath.c_str() };
			if (!stream.good()) {
				return out;
			}
			constexpr std::uint32_t CHUNK = 64 * 1024;
			for (;;) {
				const auto base = out.size();
				out.resize(base + CHUNK);
				// read() is all-or-nothing bool; the tell() delta recovers the
				// actual byte count of the final short read
				const auto before = stream.tell();
				stream.read(reinterpret_cast<char*>(out.data() + base), CHUNK);
				const auto got = stream.tell() - before;
				out.resize(base + got);
				if (got < CHUNK) {
					break;
				}
			}
			return out;
		}

		std::uint32_t ReadU32LE(const std::uint8_t* a_bytes)
		{
			std::uint32_t value;
			std::memcpy(&value, a_bytes, sizeof(value));
			return value;
		}

		// extract + persist; returns the cache file's data-relative path, "" on failure
		std::string Extract(const std::string& a_key)
		{
			const auto data = ReadResource(a_key);
			if (data.size() < FUZ_HEADER_SIZE) {
				logger::warn("Fuz '{}': unreadable or too small ({} bytes)", a_key, data.size());
				return {};
			}
			if (std::memcmp(data.data(), "FUZE", 4) != 0) {
				logger::warn("Fuz '{}': missing FUZE magic", a_key);
				return {};
			}
			const auto lipSize = ReadU32LE(data.data() + 8);
			const std::size_t audioStart = FUZ_HEADER_SIZE + lipSize;
			if (audioStart + 12 > data.size()) {
				logger::warn("Fuz '{}': lip size {} exceeds file ({} bytes)", a_key, lipSize, data.size());
				return {};
			}

			// payload sniff: RIFF form decides the extension the engine's decoder
			// picks its codec from
			const auto* audio = data.data() + audioStart;
			const auto audioSize = data.size() - audioStart;
			const char* ext = nullptr;
			if (std::memcmp(audio, "RIFF", 4) == 0) {
				if (std::memcmp(audio + 8, "XWMA", 4) == 0) {
					ext = ".xwm";
				} else if (std::memcmp(audio + 8, "WAVE", 4) == 0) {
					ext = ".wav";
				}
			}
			if (!ext) {
				logger::warn("Fuz '{}': payload is not RIFF xWMA/wav", a_key);
				return {};
			}

			// cache name: readable stem + stable path hash (collisions across
			// same-named files in different folders)
			const auto stemBegin = a_key.find_last_of('\\') + 1;  // npos+1 == 0
			auto stem = a_key.substr(stemBegin, a_key.rfind('.') - stemBegin);
			const auto cacheRel = std::format("{}\\{}_{:08x}{}", CACHE_DIR_REL, stem,
				static_cast<std::uint32_t>(Fnv1a64(a_key)), ext);
			const auto cacheDisk = std::filesystem::current_path() / "Data" / cacheRel;

			// an earlier session may have extracted it already
			std::error_code ec;
			if (std::filesystem::exists(cacheDisk, ec) &&
				std::filesystem::file_size(cacheDisk, ec) == audioSize) {
				return cacheRel;
			}

			std::filesystem::create_directories(cacheDisk.parent_path(), ec);
			std::ofstream out(cacheDisk, std::ios::binary | std::ios::trunc);
			if (!out || !out.write(reinterpret_cast<const char*>(audio),
							static_cast<std::streamsize>(audioSize))) {
				logger::warn("Fuz '{}': failed to write cache file {}", a_key, cacheRel);
				return {};
			}
			out.close();
			logger::info("Fuz '{}' -> {} ({} bytes{})", a_key, cacheRel, audioSize,
				lipSize ? std::format(", lip {} bytes skipped", lipSize) : "");
			return cacheRel;
		}
	}

	bool IsFuzPath(std::string_view a_path)
	{
		return a_path.size() > 4 &&
		       _strnicmp(a_path.data() + a_path.size() - 4, ".fuz", 4) == 0;
	}

	std::string Resolve(const std::string& a_fuzDataRelPath)
	{
		const auto key = NormalizeKey(a_fuzDataRelPath);
		{
			std::scoped_lock lock{ g_lock };
			if (const auto it = g_resolved.find(key); it != g_resolved.end()) {
				return it->second;
			}
		}
		auto resolved = Extract(key);
		std::scoped_lock lock{ g_lock };
		g_resolved[key] = resolved;
		return resolved;
	}
}
