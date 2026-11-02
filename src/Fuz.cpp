#include "Fuz.h"

#include "AudioEngine.h"

#include <mediaobj.h>
#include <mmreg.h>
#include <objbase.h>

namespace Fuz
{
	namespace
	{
		// FUZ container: "FUZE" magic, u32 version, u32 lip-data size, the LIP
		// block, then the audio payload (RIFF: form "XWMA" = xWMA, "WAVE" = wav)
		constexpr std::size_t FUZ_HEADER_SIZE = 12;

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

		// the header's lip size, validated against the bytes; false (warned) if
		// the bytes are not a well-formed container
		bool ParseHeader(const std::string& a_path, const std::vector<std::uint8_t>& a_data,
			std::uint32_t& a_lipSize)
		{
			if (a_data.size() < FUZ_HEADER_SIZE) {
				logger::warn("Fuz '{}': unreadable or too small ({} bytes)", a_path, a_data.size());
				return false;
			}
			if (std::memcmp(a_data.data(), "FUZE", 4) != 0) {
				logger::warn("Fuz '{}': missing FUZE magic", a_path);
				return false;
			}
			a_lipSize = ReadU32LE(a_data.data() + 8);
			if (FUZ_HEADER_SIZE + static_cast<std::size_t>(a_lipSize) > a_data.size()) {
				logger::warn("Fuz '{}': lip size {} exceeds file ({} bytes)", a_path, a_lipSize, a_data.size());
				return false;
			}
			return true;
		}

		// ---------- xWMA -> PCM decode (Windows WMA decoder DMO) ----------
		// The xWMA payload is WMA v2 packets in a RIFF/XWMA container. The engine
		// plays it as it is; the decode here only feeds the amplitude-envelope
		// lipsync (which needs raw samples) for a fuz without a usable lip. Uses
		// the OS's own WMA decoder, no bundled codec.

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
		// the source channel count / sample rate. Any failure returns false.
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

		void Append(std::vector<std::uint8_t>& a_out, const void* a_bytes, std::size_t a_size)
		{
			const auto* p = static_cast<const std::uint8_t*>(a_bytes);
			a_out.insert(a_out.end(), p, p + a_size);
		}

		// 44-byte canonical PCM wav header in front of the sample data
		void AppendWavHeader(std::vector<std::uint8_t>& a_out, std::uint32_t a_dataSize,
			std::uint16_t a_channels, std::uint32_t a_sampleRate)
		{
			const std::uint16_t blockAlign = a_channels * 2;
			const std::uint32_t byteRate = a_sampleRate * blockAlign;
			const std::uint32_t riffSize = 36 + a_dataSize;
			const std::uint32_t fmtSize = 16;
			const std::uint16_t pcmTag = 1, bits = 16;
			Append(a_out, "RIFF", 4);
			Append(a_out, &riffSize, 4);
			Append(a_out, "WAVEfmt ", 8);
			Append(a_out, &fmtSize, 4);
			Append(a_out, &pcmTag, 2);
			Append(a_out, &a_channels, 2);
			Append(a_out, &a_sampleRate, 4);
			Append(a_out, &byteRate, 4);
			Append(a_out, &blockAlign, 2);
			Append(a_out, &bits, 2);
			Append(a_out, "data", 4);
			Append(a_out, &a_dataSize, 4);
		}
	}

	bool IsFuzPath(std::string_view a_path)
	{
		return a_path.size() > 4 &&
		       _strnicmp(a_path.data() + a_path.size() - 4, ".fuz", 4) == 0;
	}

	bool IsPlayable(const std::string& a_fuzDataRelPath)
	{
		static std::mutex                            lock;
		static std::unordered_map<std::string, bool> verdicts;

		std::string key = a_fuzDataRelPath;
		std::replace(key.begin(), key.end(), '/', '\\');
		std::transform(key.begin(), key.end(), key.begin(),
			[](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
		{
			std::scoped_lock guard{ lock };
			if (const auto it = verdicts.find(key); it != verdicts.end()) {
				return it->second;
			}
		}

		const auto verdict = [&]() -> std::string {
			RE::BSResourceNiBinaryStream stream{ key.c_str() };
			if (!stream.good()) {
				return "unreadable (no such loose file or archive entry)";
			}
			std::uint8_t header[FUZ_HEADER_SIZE];
			if (!stream.read(reinterpret_cast<char*>(header), FUZ_HEADER_SIZE)) {
				return "too small for a fuz header";
			}
			if (std::memcmp(header, "FUZE", 4) != 0) {
				return "missing FUZE magic";
			}
			// skip the lip block: read, not seek, so a block running past the end of
			// the file shows as a short read
			auto       left = ReadU32LE(header + 8);
			const auto lipSize = left;
			std::vector<char> skip(std::min<std::uint32_t>(left, 64 * 1024));
			while (left > 0) {
				const auto take = std::min<std::uint32_t>(left, static_cast<std::uint32_t>(skip.size()));
				if (!stream.read(skip.data(), take)) {
					return std::format("lip size {} exceeds the file", lipSize);
				}
				left -= take;
			}
			std::uint8_t riff[12];
			if (!stream.read(reinterpret_cast<char*>(riff), sizeof(riff)) || std::memcmp(riff, "RIFF", 4) != 0 ||
				(std::memcmp(riff + 8, "XWMA", 4) != 0 && std::memcmp(riff + 8, "WAVE", 4) != 0)) {
				return "payload is not RIFF xWMA/wav";
			}
			return {};
		}();
		if (!verdict.empty()) {
			logger::warn("Fuz '{}': {} - not played", a_fuzDataRelPath, verdict);
		}
		std::scoped_lock guard{ lock };
		verdicts[key] = verdict.empty();
		return verdict.empty();
	}

	std::vector<std::uint8_t> ReadLipBlock(const std::string& a_fuzDataRelPath)
	{
		const auto    data = AudioEngine::ReadResourceBytes(a_fuzDataRelPath);
		std::uint32_t lipSize = 0;
		if (!ParseHeader(a_fuzDataRelPath, data, lipSize) || lipSize < 24) {
			return {};  // a lipless fuz is ordinary: no warning
		}
		return { data.begin() + FUZ_HEADER_SIZE, data.begin() + FUZ_HEADER_SIZE + lipSize };
	}

	std::vector<std::uint8_t> DecodeToWav(const std::string& a_fuzDataRelPath)
	{
		const auto    data = AudioEngine::ReadResourceBytes(a_fuzDataRelPath);
		std::uint32_t lipSize = 0;
		if (!ParseHeader(a_fuzDataRelPath, data, lipSize)) {
			return {};
		}
		const auto* audio = data.data() + FUZ_HEADER_SIZE + lipSize;
		const auto  audioSize = data.size() - FUZ_HEADER_SIZE - lipSize;
		if (audioSize < 12 || std::memcmp(audio, "RIFF", 4) != 0) {
			logger::warn("Fuz '{}': payload is not RIFF xWMA/wav", a_fuzDataRelPath);
			return {};
		}
		if (std::memcmp(audio + 8, "WAVE", 4) == 0) {
			return std::vector<std::uint8_t>(audio, audio + audioSize);
		}
		if (std::memcmp(audio + 8, "XWMA", 4) != 0) {
			logger::warn("Fuz '{}': payload is not RIFF xWMA/wav", a_fuzDataRelPath);
			return {};
		}

		std::vector<std::uint8_t> pcm;
		std::uint16_t             channels = 0;
		std::uint32_t             sampleRate = 0;
		if (!DecodeXwmaToPcm(audio, audioSize, pcm, channels, sampleRate)) {
			logger::warn("Fuz '{}': xWMA decode failed - the line plays, but without an amplitude envelope",
				a_fuzDataRelPath);
			return {};
		}
		std::vector<std::uint8_t> wav;
		wav.reserve(44 + pcm.size());
		AppendWavHeader(wav, static_cast<std::uint32_t>(pcm.size()), channels, sampleRate);
		wav.insert(wav.end(), pcm.begin(), pcm.end());
		logger::debug("Fuz '{}': xWMA decoded in memory, {} PCM bytes{}", a_fuzDataRelPath, pcm.size(),
			lipSize ? std::format(", lip {} bytes", lipSize) : "");
		return wav;
	}
}
