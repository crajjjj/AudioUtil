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

		// Header: const14 discriminates the layout — 3 = 24-byte classic
		// (preroll i32 at 16), 7 = 20-byte prerollless (the former "variant C"),
		// variant B = classic with one extra byte at offset 14 (dropped here).
		struct Header
		{
			std::vector<std::uint8_t> shifted;   // owns the buffer for variant B
			const std::uint8_t*       d = nullptr;
			std::size_t               size = 0;
			std::size_t               payloadOff = 0;
			std::int32_t              preroll = 0;
			std::uint32_t             frames = 0;
			std::uint32_t             numCurves = 0;
			std::uint32_t             startCells = 0;  // leading zero cells (start token >> 2)
			char                      variant = '?';
		};

		bool FillHeader(Header& a_h, const std::uint8_t* a_d, std::size_t a_size,
			std::size_t a_off, bool a_hasPreroll, char a_variant)
		{
			const auto version = ReadLE<std::uint32_t>(a_d);
			const auto frames = static_cast<std::uint32_t>(ReadLE<std::uint16_t>(a_d + 12));
			if (version != 1 || frames == 0 || frames > MAX_FRAMES) {
				return false;
			}
			a_h.d = a_d;
			a_h.size = a_size;
			a_h.payloadOff = a_off;
			a_h.preroll = a_hasPreroll ? ReadLE<std::int32_t>(a_d + 16) : 0;
			a_h.frames = frames;
			a_h.numCurves = ReadLE<std::uint32_t>(a_d + 8);
			// the u16 right before the payload is the START TOKEN: token>>2 =
			// leading zero cells hoisted out of the payload (the low 2 bits are
			// a tag, ~always 3). Corpus-verified: honoring it lands 9741/9760
			// files at exactly frames*33*4 decompressed bytes (0 without it) —
			// and skipping it decodes every channel rotated by (33 - start)
			// slots. (Correction due to Raynor1511.)
			a_h.startCells = ReadLE<std::uint16_t>(a_d + a_off - 2) >> 2;
			a_h.variant = a_variant;
			return true;
		}

		bool ParseHeader(Header& a_h, const std::uint8_t* a_data, std::size_t a_size)
		{
			if (!a_data || a_size < 20) {
				return false;
			}
			const auto u16 = [&](std::size_t a_off) -> std::int32_t {
				return a_off + 2 <= a_size ? ReadLE<std::uint16_t>(a_data + a_off) : -1;
			};
			// each layout is tried in turn; a FillHeader that fails its sanity
			// checks (bad version/frames) falls through to the next candidate
			if (u16(14) == 7 && u16(16) == 16 &&
				FillHeader(a_h, a_data, a_size, 20, false, 'C')) {
				return true;  // 20-byte prerollless ("variant C")
			}
			if (a_size >= 24 && u16(20) == 16 &&
				FillHeader(a_h, a_data, a_size, 24, true, 'A')) {
				return true;  // 24-byte classic (const14==3, or tolerated odd values)
			}
			if (a_size >= 25) {  // variant B: drop the extra byte at offset 14
				a_h.shifted.reserve(a_size - 1);
				a_h.shifted.assign(a_data, a_data + 14);
				a_h.shifted.insert(a_h.shifted.end(), a_data + 15, a_data + a_size);
				const auto* d = a_h.shifted.data();
				if (a_h.shifted.size() >= 24 && ReadLE<std::uint16_t>(d + 20) == 16 &&
					FillHeader(a_h, d, a_h.shifted.size(), 24, true, 'B')) {
					return true;
				}
				a_h.shifted.clear();
			}
			return false;
		}

		// The payload is ZERO-RLE compressed (engine-verified against the game's
		// own FUZE/lip loader, SkyrimSE.exe 1.6.1170 sub_140243*): a nonzero
		// byte is a literal; a 0x00 byte introduces a run — the next two bytes
		// are a little-endian u16 count of zero bytes to emit. `a_prefixBytes`
		// zero bytes (the header start token's hoisted leading rest run) are
		// emitted first. The decompressed buffer is a DENSE float32 grid,
		// frame-major, 33 slots per frame, every value a signed channel weight
		// in [-1,1] (slots 0–15 phonemes, 16–32 the 17 modifiers — no padding
		// slot). The trailing all-zero run is omitted by the encoder, so the
		// grid is zero-filled up to frames*33. This single decode replaces the
		// whole "dup/tangent/marker" grammar the format was long mis-modeled
		// as (those were artifacts of reading the compressed bytes directly).
		std::vector<std::uint8_t> Decompress(const std::uint8_t* a_p, std::size_t a_n,
			std::size_t a_cap, std::size_t a_prefixBytes)
		{
			std::vector<std::uint8_t> out;
			out.reserve(a_cap);
			out.assign(std::min(a_prefixBytes, a_cap), std::uint8_t{ 0 });
			std::size_t i = 0;
			while (i < a_n && out.size() < a_cap) {
				const std::uint8_t b = a_p[i];
				if (b != 0) {
					out.push_back(b);
					++i;
				} else if (i + 3 <= a_n) {
					std::size_t run = ReadLE<std::uint16_t>(a_p + i + 1);
					run = std::min(run, a_cap - out.size());
					out.insert(out.end(), run, std::uint8_t{ 0 });
					i += 3;
				} else {
					break;  // dangling 0 at EOF
				}
			}
			return out;
		}
	}

	std::shared_ptr<const Anim> Parse(const std::uint8_t* a_data, std::size_t a_size)
	{
		Header h;
		if (!ParseHeader(h, a_data, a_size)) {
			return nullptr;
		}
		const std::uint32_t frames = h.frames;

		// preroll (variant A/B) is the grid's negative first-frame index: the
		// first |preroll| frames are pre-audio lead-in and grid frame |preroll|
		// lands on audio t=0. Skip the lead-in, else the lip plays late.
		auto lead = static_cast<std::uint32_t>(std::clamp(
			-static_cast<std::int64_t>(h.preroll), std::int64_t{ 0 }, std::int64_t{ 150 }));
		if (lead + 1 >= frames) {
			lead = 0;  // nonsense preroll — keep the whole grid
		}
		const std::uint32_t effFrames = frames - lead;

		// decompress to the dense grid (start-token zero cells first), then
		// read frame-major float32 cells
		const std::size_t cap = static_cast<std::size_t>(frames) * STRIDE * sizeof(float);
		const auto grid = Decompress(h.d + h.payloadOff, h.size - h.payloadOff, cap,
			static_cast<std::size_t>(h.startCells) * sizeof(float));
		const std::size_t cells = grid.size() / sizeof(float);

		auto anim = std::make_shared<Anim>();
		anim->frames = effFrames;
		anim->durationSec = static_cast<float>(effFrames) / FPS;

		for (std::uint32_t ch = 0; ch < CHANNELS; ++ch) {
			anim->values[ch].assign(effFrames, 0.0f);
		}
		// The grid plays VERBATIM: authored curves already ramp down to 0 on
		// their own (baked at 30 fps), so no smoothing/fades are added — a cell
		// the file encodes as 0 stays 0.
		bool          chAny[CHANNELS] = {};
		bool          any = false;
		std::uint64_t examined = 0;
		std::uint64_t corrupt = 0;
		for (std::size_t pos = 0; pos < cells; ++pos) {
			const auto frame = static_cast<std::uint32_t>(pos / STRIDE);
			const auto slot = static_cast<std::uint32_t>(pos % STRIDE);
			if (frame < lead || frame >= frames || slot >= CHANNELS) {
				continue;
			}
			const float v = ReadLE<float>(grid.data() + pos * sizeof(float));
			++examined;
			if (!std::isfinite(v) || v < -1.0001f || v > 1.0001f) {
				// a real cell is a signed weight in [-1,1] or exact rest 0
				// (negatives are routine on the head channels and undershoot on
				// phonemes) — NaN/inf and huge values are corrupt/misaligned
				// data (or a non-compressed lip). Skip rather than clamp:
				// clamping a huge value to 1.0 would flash the mouth fully open.
				++corrupt;
				continue;
			}
			if (v == 0.0f) {
				continue;  // rest (the vast majority — grid is sparse-active)
			}
			anim->values[slot][frame - lead] = std::clamp(v, -1.0f, 1.0f);
			chAny[slot] = true;
			any = true;
		}
		// wholesale garbage (e.g. an uncompressed lip mis-decompressed) → fall
		// back to the envelope rather than mouthing noise. Corrupt is measured
		// against the cells actually examined (lead frames are skipped before
		// classification, so `cells` would dilute the ratio).
		if (!any || corrupt * 4 > examined) {
			// visible at the shipped log level: with use_lip_files on by default,
			// a lip class silently regressing to the envelope would be invisible
			logger::warn(
				"LipData: rejecting lip (variant {}, {} frames): {} of {} cells corrupt, {} usable — envelope fallback",
				h.variant, frames, corrupt, examined, any ? "some" : "none");
			return nullptr;
		}
		for (std::uint32_t ch = 0; ch < CHANNELS; ++ch) {
			if (!chAny[ch]) {
				// truly free it — clear() would keep the effFrames*4B capacity
				// alive in every cached anim (~MBs across a 128-entry cache)
				std::vector<float>{}.swap(anim->values[ch]);
			}
		}
		logger::debug("LipData: variant {} frames={} curves={} start={} -> {} cells",
			h.variant, frames, h.numCurves, h.startCells, cells);
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
