#include "LipData.h"

#include "FuzCache.h"

namespace LipData
{
	namespace
	{
		constexpr std::uint32_t STRIDE = 33;      // grid slots per frame
		constexpr std::uint32_t MAX_FRAMES = 60 * 30 * 30;  // 30 min — sanity cap

		std::unordered_map<std::string, std::shared_ptr<const Anim>> g_cache;
		std::mutex g_lock;

		std::string NormalizeKey(const std::string& a_path)
		{
			std::string out = a_path;
			std::replace(out.begin(), out.end(), '/', '\\');
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return out;
		}

		template <class T>
		T ReadLE(const std::uint8_t* a_bytes)
		{
			T value;
			std::memcpy(&value, a_bytes, sizeof(T));
			return value;
		}
	}

	std::shared_ptr<const Anim> Parse(const std::uint8_t* a_data, std::size_t a_size)
	{
		if (!a_data || a_size < 24) {
			return nullptr;
		}

		// header: <u32 version><u32 duration><u32 numCurves><u16 frames>
		//         <u16 const14><i32 preroll><u16 vocab><u16 u22>
		// Variant B has one extra byte at offset 14 (and const14=2) — detected
		// by the vocab field not reading 16; normalize by dropping that byte.
		std::vector<std::uint8_t> shifted;
		const std::uint8_t*       d = a_data;
		std::size_t               size = a_size;
		auto vocab = ReadLE<std::uint16_t>(d + 20);
		if (vocab != 16) {
			shifted.reserve(a_size - 1);
			shifted.insert(shifted.end(), a_data, a_data + 14);
			shifted.insert(shifted.end(), a_data + 15, a_data + a_size);
			d = shifted.data();
			size = shifted.size();
			vocab = ReadLE<std::uint16_t>(d + 20);
			if (vocab != 16) {
				return nullptr;  // third (unknown) variant — skip gracefully
			}
		}
		const auto version = ReadLE<std::uint32_t>(d);
		const auto frames = static_cast<std::uint32_t>(ReadLE<std::uint16_t>(d + 12));
		if (version != 1 || frames == 0 || frames > MAX_FRAMES) {
			return nullptr;
		}

		// token walk: [f32 value] [optional exact dup] [optional 00,4*skip,00
		// marker]. Position advances 1 per float plus `skip` resting slots;
		// frame = pos / 33, slot = pos % 33. Weights are values in [0,1] at
		// slots 0..31; sentinels (~1e-15) count as explicit zero keys; anything
		// out of range is Hermite tangent data and carries no weight.
		std::vector<std::pair<std::uint32_t, float>> keys[CHANNELS];
		std::size_t   i = 24;
		std::uint64_t pos = 0;
		while (i + 4 <= size) {
			const float value = ReadLE<float>(d + i);
			std::uint32_t floats = 1;
			i += 4;
			if (i + 4 <= size && std::memcmp(d + i, d + i - 4, 4) == 0) {
				floats = 2;  // dup: value + equal tangent
				i += 4;
			}
			std::uint32_t skip = 0;
			if (i + 3 <= size && d[i] == 0 && d[i + 2] == 0 &&
				d[i + 1] != 0 && d[i + 1] % 4 == 0) {
				skip = d[i + 1] / 4u;
				i += 3;
			}
			const auto frame = static_cast<std::uint32_t>(pos / STRIDE);
			const auto slot = static_cast<std::uint32_t>(pos % STRIDE);
			if (frame < frames && slot < CHANNELS) {
				if (std::fabs(value) < 1.0e-6f) {
					keys[slot].emplace_back(frame, 0.0f);  // sentinel/true zero
				} else if (value >= 0.0f && value <= 1.0001f) {
					keys[slot].emplace_back(frame, std::min(value, 1.0f));
				}
			}
			pos += floats + skip;
		}

		auto anim = std::make_shared<Anim>();
		anim->frames = frames;
		anim->durationSec = static_cast<float>(frames) / FPS;
		for (std::uint32_t ch = 0; ch < CHANNELS; ++ch) {
			auto& series = anim->values[ch];
			const auto& src = keys[ch];
			if (src.empty()) {
				continue;  // resting channel stays empty (samples as 0)
			}
			series.assign(frames, 0.0f);
			// linear interpolation between keys, ends held (stream order is
			// frame-ascending; a same-frame rewrite takes the later value)
			std::size_t k = 0;
			for (std::uint32_t f = 0; f < frames; ++f) {
				while (k + 1 < src.size() && src[k + 1].first <= f) {
					++k;
				}
				if (f <= src.front().first) {
					series[f] = src.front().second;
				} else if (k + 1 >= src.size()) {
					series[f] = src.back().second;
				} else {
					const auto [f0, v0] = src[k];
					const auto [f1, v1] = src[k + 1];
					series[f] = f1 > f0
					              ? v0 + (v1 - v0) * (static_cast<float>(f - f0) / static_cast<float>(f1 - f0))
					              : v1;
				}
			}
		}
		return anim;
	}

	std::shared_ptr<const Anim> GetFor(const std::string& a_dataRelPath)
	{
		const auto key = NormalizeKey(a_dataRelPath);
		{
			std::scoped_lock lock{ g_lock };
			if (const auto it = g_cache.find(key); it != g_cache.end()) {
				return it->second;
			}
		}

		std::vector<std::uint8_t> bytes;
		if (FuzCache::IsFuzPath(key)) {
			bytes = FuzCache::ReadLipBlock(key);
		} else if (const auto dot = key.rfind('.'); dot != std::string::npos) {
			bytes = FuzCache::ReadResourceBytes(key.substr(0, dot) + ".lip");
		}

		std::shared_ptr<const Anim> anim;
		if (!bytes.empty()) {
			anim = Parse(bytes.data(), bytes.size());
			if (anim && !anim->HasMouthData()) {
				anim = nullptr;  // empty/broken lip: envelope fallback beats a frozen mouth
			}
			if (anim) {
				logger::debug("LipData: '{}' -> {} frames ({:.2f}s)", a_dataRelPath,
					anim->frames, anim->durationSec);
			} else {
				logger::debug("LipData: '{}' has a lip block but no usable curves", a_dataRelPath);
			}
		}

		std::scoped_lock lock{ g_lock };
		if (g_cache.size() > 128) {
			g_cache.clear();
		}
		g_cache[key] = anim;  // misses cached too
		return anim;
	}

	void ClearCache()
	{
		std::scoped_lock lock{ g_lock };
		g_cache.clear();
	}
}
