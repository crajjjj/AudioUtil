#include "FuzCache.h"

#include "Config.h"
#include "FuzSlots.h"

#include <atomic>
#include <filesystem>
#include <fstream>

#include <mediaobj.h>
#include <mmreg.h>
#include <objbase.h>

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

		std::uint16_t ReadU16LE(const std::uint8_t* a_bytes)
		{
			std::uint16_t value;
			std::memcpy(&value, a_bytes, sizeof(value));
			return value;
		}

		// ---------- xWMA -> PCM decode (Windows WMA decoder DMO) ----------
		// The xWMA payload is WMA v2 packets in a RIFF/XWMA container. Decoding
		// it to PCM lets the cache hold a plain wav: the engine plays PCM most
		// reliably AND the amplitude-envelope lipsync (which needs raw samples)
		// works for fuz lines. Uses the OS's own WMA decoder — no bundled codec.

		// GUIDs spelled out to avoid linking wmcodecdspuuid/strmiids
		constexpr GUID kCLSID_CWMADecMediaObject{ 0x2EEB4ADF, 0x4578, 0x4D10, { 0xBC, 0xA7, 0xBB, 0x95, 0x5F, 0x56, 0x32, 0x0A } };
		constexpr GUID kMEDIATYPE_Audio{ 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
		constexpr GUID kMEDIASUBTYPE_WMAUDIO2{ 0x00000161, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
		constexpr GUID kMEDIASUBTYPE_PCM{ 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
		constexpr GUID kFORMAT_WaveFormatEx{ 0x05589F81, 0xC356, 0x11CE, { 0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A } };

		// minimal IMediaBuffer over a byte vector (the DMO API's buffer contract)
		class MediaBuffer : public IMediaBuffer
		{
		public:
			explicit MediaBuffer(DWORD a_maxLength) : data_(a_maxLength) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
			{
				if (!a_out) {
					return E_POINTER;
				}
				if (a_riid == IID_IUnknown || a_riid == __uuidof(IMediaBuffer)) {
					*a_out = static_cast<IMediaBuffer*>(this);
					AddRef();
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const auto count = InterlockedDecrement(&refs_);
				if (count == 0) {
					delete this;
				}
				return count;
			}
			HRESULT STDMETHODCALLTYPE SetLength(DWORD a_length) override
			{
				if (a_length > data_.size()) {
					return E_INVALIDARG;
				}
				length_ = a_length;
				return S_OK;
			}
			HRESULT STDMETHODCALLTYPE GetMaxLength(DWORD* a_out) override
			{
				if (!a_out) {
					return E_POINTER;
				}
				*a_out = static_cast<DWORD>(data_.size());
				return S_OK;
			}
			HRESULT STDMETHODCALLTYPE GetBufferAndLength(BYTE** a_buffer, DWORD* a_length) override
			{
				if (a_buffer) {
					*a_buffer = data_.data();
				}
				if (a_length) {
					*a_length = length_;
				}
				return S_OK;
			}

			BYTE* Data() { return data_.data(); }

		private:
			std::vector<BYTE> data_;
			DWORD             length_ = 0;
			LONG              refs_ = 1;
		};

		// decode a full RIFF/XWMA blob to 16-bit PCM. On success fills a_pcm +
		// the source channel count / sample rate. Any failure returns false
		// (caller falls back to caching the raw xwm - playback still works).
		bool DecodeXwmaToPcm(const std::uint8_t* a_riff, std::size_t a_size,
			std::vector<std::uint8_t>& a_pcm, std::uint16_t& a_channels, std::uint32_t& a_sampleRate)
		{
			// -- parse the container: fmt fields + data chunk
			if (a_size < 12) {
				return false;
			}
			const std::uint8_t* fmt = nullptr;
			std::uint32_t       fmtSize = 0;
			const std::uint8_t* packets = nullptr;
			std::uint32_t       packetsSize = 0;
			std::size_t         pos = 12;
			while (pos + 8 <= a_size) {
				const auto chunkSize = ReadU32LE(a_riff + pos + 4);
				const auto body = pos + 8;
				if (body + chunkSize > a_size) {
					break;
				}
				if (std::memcmp(a_riff + pos, "fmt ", 4) == 0) {
					fmt = a_riff + body;
					fmtSize = chunkSize;
				} else if (std::memcmp(a_riff + pos, "data", 4) == 0) {
					packets = a_riff + body;
					packetsSize = chunkSize;
				}
				pos = body + chunkSize + (chunkSize & 1);
			}
			if (!fmt || fmtSize < 16 || !packets || packetsSize == 0) {
				return false;
			}
			const auto formatTag = ReadU16LE(fmt);
			a_channels = ReadU16LE(fmt + 2);
			a_sampleRate = ReadU32LE(fmt + 4);
			const auto avgBytesPerSec = ReadU32LE(fmt + 8);
			const auto blockAlign = ReadU16LE(fmt + 12);
			if (formatTag != 0x0161 || a_channels == 0 || a_sampleRate == 0) {
				return false;  // only WMA v2 (the fuz codec); 0x162 WMA Pro unseen in the wild
			}

			// -- COM + the decoder DMO
			const auto coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const bool coBalanced = SUCCEEDED(coInit);  // RPC_E_CHANGED_MODE: COM already up, usable, don't uninit
			bool ok = false;
			IMediaObject* dmo = nullptr;
			MediaBuffer*  inBuffer = nullptr;
			MediaBuffer*  outBuffer = nullptr;
			do {
				if (FAILED(CoCreateInstance(kCLSID_CWMADecMediaObject, nullptr,
						CLSCTX_INPROC_SERVER, __uuidof(IMediaObject), reinterpret_cast<void**>(&dmo)))) {
					break;
				}

				// input type: the fmt-chunk WAVEFORMATEX + the 10-byte codec blob
				// xWMA omits. ffmpeg's xwma demuxer fabricates it as zeros with
				// encode-options 0x1F ("experimentally obtained") - same here.
				struct
				{
					WAVEFORMATEX wfx;
					BYTE         codec[10];
				} inFormat{};
				inFormat.wfx.wFormatTag = 0x0161;
				inFormat.wfx.nChannels = a_channels;
				inFormat.wfx.nSamplesPerSec = a_sampleRate;
				inFormat.wfx.nAvgBytesPerSec = avgBytesPerSec;
				inFormat.wfx.nBlockAlign = blockAlign;
				inFormat.wfx.wBitsPerSample = 16;
				inFormat.wfx.cbSize = sizeof(inFormat.codec);
				inFormat.codec[4] = 0x1F;  // wEncodeOptions

				DMO_MEDIA_TYPE mtIn{};
				mtIn.majortype = kMEDIATYPE_Audio;
				mtIn.subtype = kMEDIASUBTYPE_WMAUDIO2;
				mtIn.bFixedSizeSamples = TRUE;
				mtIn.lSampleSize = blockAlign;
				mtIn.formattype = kFORMAT_WaveFormatEx;
				mtIn.cbFormat = sizeof(inFormat);
				mtIn.pbFormat = reinterpret_cast<BYTE*>(&inFormat);
				if (FAILED(dmo->SetInputType(0, &mtIn, 0))) {
					break;
				}

				WAVEFORMATEX outFormat{};
				outFormat.wFormatTag = WAVE_FORMAT_PCM;
				outFormat.nChannels = a_channels;
				outFormat.nSamplesPerSec = a_sampleRate;
				outFormat.wBitsPerSample = 16;
				outFormat.nBlockAlign = static_cast<WORD>(a_channels * 2);
				outFormat.nAvgBytesPerSec = a_sampleRate * outFormat.nBlockAlign;

				DMO_MEDIA_TYPE mtOut{};
				mtOut.majortype = kMEDIATYPE_Audio;
				mtOut.subtype = kMEDIASUBTYPE_PCM;
				mtOut.bFixedSizeSamples = TRUE;
				mtOut.lSampleSize = outFormat.nBlockAlign;
				mtOut.formattype = kFORMAT_WaveFormatEx;
				mtOut.cbFormat = sizeof(outFormat);
				mtOut.pbFormat = reinterpret_cast<BYTE*>(&outFormat);
				if (FAILED(dmo->SetOutputType(0, &mtOut, 0))) {
					break;
				}

				inBuffer = new MediaBuffer(packetsSize);
				std::memcpy(inBuffer->Data(), packets, packetsSize);
				inBuffer->SetLength(packetsSize);
				if (FAILED(dmo->ProcessInput(0, inBuffer, 0, 0, 0))) {
					break;
				}

				outBuffer = new MediaBuffer(512 * 1024);
				const auto drain = [&]() {
					for (;;) {
						outBuffer->SetLength(0);
						DMO_OUTPUT_DATA_BUFFER odb{};
						odb.pBuffer = outBuffer;
						DWORD status = 0;
						if (FAILED(dmo->ProcessOutput(0, 1, &odb, &status))) {
							return;
						}
						BYTE* bytes = nullptr;
						DWORD produced = 0;
						outBuffer->GetBufferAndLength(&bytes, &produced);
						if (produced == 0) {
							return;
						}
						a_pcm.insert(a_pcm.end(), bytes, bytes + produced);
					}
				};
				drain();
				dmo->Discontinuity(0);  // end of stream - flush the tail
				drain();
				ok = !a_pcm.empty();
			} while (false);

			if (outBuffer) {
				outBuffer->Release();
			}
			if (inBuffer) {
				inBuffer->Release();
			}
			if (dmo) {
				dmo->Release();
			}
			if (coBalanced) {
				CoUninitialize();
			}
			return ok;
		}

		// 44-byte canonical PCM wav header in front of the sample data
		void WriteWavHeader(std::ofstream& a_out, std::uint32_t a_dataSize,
			std::uint16_t a_channels, std::uint32_t a_sampleRate)
		{
			const std::uint16_t blockAlign = a_channels * 2;
			const std::uint32_t byteRate = a_sampleRate * blockAlign;
			const std::uint32_t riffSize = 36 + a_dataSize;
			const std::uint32_t fmtSize = 16;
			const std::uint16_t pcmTag = 1, bits = 16;
			a_out.write("RIFF", 4);
			a_out.write(reinterpret_cast<const char*>(&riffSize), 4);
			a_out.write("WAVEfmt ", 8);
			a_out.write(reinterpret_cast<const char*>(&fmtSize), 4);
			a_out.write(reinterpret_cast<const char*>(&pcmTag), 2);
			a_out.write(reinterpret_cast<const char*>(&a_channels), 2);
			a_out.write(reinterpret_cast<const char*>(&a_sampleRate), 4);
			a_out.write(reinterpret_cast<const char*>(&byteRate), 4);
			a_out.write(reinterpret_cast<const char*>(&blockAlign), 2);
			a_out.write(reinterpret_cast<const char*>(&bits), 2);
			a_out.write("data", 4);
			a_out.write(reinterpret_cast<const char*>(&a_dataSize), 4);
		}

		// Write via a unique temp file + atomic rename. Two threads extracting the
		// same fuz at once (crowd scene: two NPCs share a voice line) otherwise both
		// open the same cache file with trunc and interleave their writes, leaving a
		// corrupt wav that the next session's "exists && size>44" check accepts as
		// good — corruption that persists until the cache is cleared. Each writer
		// now lays down its own temp file and renames; rename replaces atomically on
		// one volume, so a reader only ever sees a complete file. No lock is held
		// across the slow decode/write.
		std::atomic<std::uint32_t> g_tmpSeq{ 0 };

		template <class Fn>
		bool WriteFileAtomic(const std::filesystem::path& a_final, Fn&& a_writeBody)
		{
			const auto tmp = std::filesystem::path(
				a_final.string() + std::format(".tmp{:x}", g_tmpSeq.fetch_add(1)));
			std::error_code ec;
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if (!out) {
					return false;
				}
				a_writeBody(out);
				if (!out) {  // a write failed — drop the partial temp
					out.close();
					std::filesystem::remove(tmp, ec);
					return false;
				}
			}  // ofstream closed here, before the rename
			std::filesystem::rename(tmp, a_final, ec);
			if (ec) {
				std::filesystem::remove(tmp, ec);
				return false;
			}
			return true;
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

			// payload sniff: RIFF form tells us what we hold
			const auto* audio = data.data() + audioStart;
			const auto audioSize = data.size() - audioStart;
			const bool isXwma = std::memcmp(audio, "RIFF", 4) == 0 && std::memcmp(audio + 8, "XWMA", 4) == 0;
			const bool isWave = std::memcmp(audio, "RIFF", 4) == 0 && std::memcmp(audio + 8, "WAVE", 4) == 0;
			if (!isXwma && !isWave) {
				logger::warn("Fuz '{}': payload is not RIFF xWMA/wav", a_key);
				return {};
			}

			// cache name: readable stem + stable path hash (collisions across
			// same-named files in different folders). Preferred form is .wav
			// (xWMA decoded to PCM: most reliable playback AND envelope lipsync);
			// .xwm is the raw-payload fallback when the decode fails (playback
			// only, no lipsync).
			const auto stemBegin = a_key.find_last_of('\\') + 1;  // npos+1 == 0
			auto stem = a_key.substr(stemBegin, a_key.rfind('.') - stemBegin);
			const auto cacheBase = std::format("{}\\{}_{:08x}", CACHE_DIR_REL, stem,
				static_cast<std::uint32_t>(Fnv1a64(a_key)));
			const auto diskBase = std::filesystem::current_path() / "Data" / cacheBase;

			// an earlier session may have extracted (and decoded) it already
			std::error_code ec;
			const auto wavDisk = std::filesystem::path(diskBase.string() + ".wav");
			if (std::filesystem::exists(wavDisk, ec) && std::filesystem::file_size(wavDisk, ec) > 44) {
				return cacheBase + ".wav";
			}

			std::filesystem::create_directories(wavDisk.parent_path(), ec);

			// xWMA: decode to PCM and cache a plain wav
			if (isXwma) {
				std::vector<std::uint8_t> pcm;
				std::uint16_t channels = 0;
				std::uint32_t sampleRate = 0;
				if (DecodeXwmaToPcm(audio, audioSize, pcm, channels, sampleRate)) {
					const bool wrote = WriteFileAtomic(wavDisk, [&](std::ofstream& out) {
						WriteWavHeader(out, static_cast<std::uint32_t>(pcm.size()), channels, sampleRate);
						out.write(reinterpret_cast<const char*>(pcm.data()),
							static_cast<std::streamsize>(pcm.size()));
					});
					if (wrote) {
						logger::info("Fuz '{}' -> {}.wav (xWMA decoded, {} PCM bytes{})", a_key,
							cacheBase, pcm.size(),
							lipSize ? std::format(", lip {} bytes skipped", lipSize) : "");
						return cacheBase + ".wav";
					}
					logger::warn("Fuz '{}': failed to write {}.wav", a_key, cacheBase);
				} else {
					logger::warn("Fuz '{}': xWMA decode failed - caching raw xwm (plays, but no lipsync)", a_key);
				}
			}

			// WAVE payload, or the decode fallback: cache the raw payload bytes
			const char* ext = isWave ? ".wav" : ".xwm";
			const auto rawDisk = std::filesystem::path(diskBase.string() + ext);
			if (!isWave && std::filesystem::exists(rawDisk, ec) &&
				std::filesystem::file_size(rawDisk, ec) == audioSize) {
				return cacheBase + ext;  // fallback from an earlier session
			}
			const bool wrote = WriteFileAtomic(rawDisk, [&](std::ofstream& out) {
				out.write(reinterpret_cast<const char*>(audio),
					static_cast<std::streamsize>(audioSize));
			});
			if (!wrote) {
				logger::warn("Fuz '{}': failed to write cache file {}{}", a_key, cacheBase, ext);
				return {};
			}
			logger::info("Fuz '{}' -> {}{} ({} bytes{})", a_key, cacheBase, ext, audioSize,
				lipSize ? std::format(", lip {} bytes skipped", lipSize) : "");
			return cacheBase + ext;
		}
	}

	bool IsFuzPath(std::string_view a_path)
	{
		return a_path.size() > 4 &&
		       _strnicmp(a_path.data() + a_path.size() - 4, ".fuz", 4) == 0;
	}

	std::vector<std::uint8_t> ReadResourceBytes(const std::string& a_dataRelPath)
	{
		return ReadResource(NormalizeKey(a_dataRelPath));
	}

	std::vector<std::uint8_t> ReadLipBlock(const std::string& a_fuzDataRelPath)
	{
		auto data = ReadResource(NormalizeKey(a_fuzDataRelPath));
		if (data.size() < FUZ_HEADER_SIZE || std::memcmp(data.data(), "FUZE", 4) != 0) {
			return {};
		}
		const auto lipSize = ReadU32LE(data.data() + 8);
		if (lipSize < 24 || FUZ_HEADER_SIZE + static_cast<std::size_t>(lipSize) > data.size()) {
			return {};
		}
		return { data.begin() + FUZ_HEADER_SIZE, data.begin() + FUZ_HEADER_SIZE + lipSize };
	}

	void EnforceCacheCap()
	{
		const auto capMB = Config::Get()->fuzCacheMaxMB;
		if (capMB == 0) {
			return;
		}
		const auto dir = std::filesystem::current_path() / "Data" / CACHE_DIR_REL;
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec)) {
			return;
		}

		struct File
		{
			std::filesystem::path           path;
			std::uintmax_t                  size;
			std::filesystem::file_time_type written;
		};
		std::vector<File> files;
		std::uintmax_t    total = 0;
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
			if (!entry.is_regular_file(ec)) {
				continue;
			}
			// FuzSlots placeholder wavs live here but must never be evicted — they have
			// to exist at launch for in-session fuz playback, and their size is transient
			// (they hold whatever line is currently playing). Prefix owned by FuzSlots.
			if (FuzSlots::IsSlotName(entry.path().filename().string())) {
				continue;
			}
			const auto size = entry.file_size(ec);
			files.push_back({ entry.path(), size, entry.last_write_time(ec) });
			total += size;
		}
		const std::uintmax_t cap = static_cast<std::uintmax_t>(capMB) * 1024 * 1024;
		if (total <= cap) {
			return;
		}

		// oldest first — effectively FIFO (cache files are written once)
		std::sort(files.begin(), files.end(),
			[](const File& a_lhs, const File& a_rhs) { return a_lhs.written < a_rhs.written; });
		std::size_t evicted = 0;
		for (const auto& file : files) {
			if (total <= cap) {
				break;
			}
			if (std::filesystem::remove(file.path, ec)) {
				total -= file.size;
				++evicted;
			}
		}
		logger::info("Fuz cache over {} MB cap: evicted {} oldest file(s), {} MB kept",
			capMB, evicted, total / (1024 * 1024));

		// evicted paths may be in this session's resolve map - drop it so they
		// re-extract on demand instead of pointing at deleted files
		std::scoped_lock lock{ g_lock };
		g_resolved.clear();
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
