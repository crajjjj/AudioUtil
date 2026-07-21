#include "LipSync.h"

#include "Config.h"

#include <cmath>
#include <condition_variable>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace LipSync
{
	namespace
	{
		// MFG phoneme indices (facegen humanoid rigs carry 16)
		constexpr std::uint32_t kPhonemeAah = 0;
		constexpr std::uint32_t kPhonemeBigAah = 1;

		constexpr auto TICK = std::chrono::milliseconds(15);
		constexpr auto AUDIBLE_TIMEOUT = std::chrono::milliseconds(2500);

		// ---------- amplitude envelope ----------

		struct Envelope
		{
			std::vector<float> levels;  // RMS per window, normalized+shaped to 0..1
			float              windowSec = 0.01f;
			float              durationSec = 0.0f;

			float Sample(float a_t) const
			{
				if (levels.empty() || a_t < 0.0f) {
					return 0.0f;
				}
				const float pos = a_t / windowSec;
				const auto  index = static_cast<std::size_t>(pos);
				if (index + 1 >= levels.size()) {
					return index < levels.size() ? levels[index] : 0.0f;
				}
				const float frac = pos - static_cast<float>(index);
				return levels[index] + (levels[index + 1] - levels[index]) * frac;
			}
		};

		std::unordered_map<std::string, std::shared_ptr<const Envelope>> g_envelopeCache;
		std::mutex g_envelopeLock;

		template <class T>
		T ReadLE(const std::uint8_t* a_bytes)
		{
			T value;
			std::memcpy(&value, a_bytes, sizeof(T));
			return value;
		}

		// decode one sample frame position to a mono float in [-1, 1]
		float DecodeSample(const std::uint8_t* a_frame, std::uint16_t a_format,
			std::uint16_t a_bits)
		{
			switch (a_bits) {
			case 8:
				return (static_cast<float>(a_frame[0]) - 128.0f) / 128.0f;
			case 16:
				return static_cast<float>(ReadLE<std::int16_t>(a_frame)) / 32768.0f;
			case 24:
				{
					std::int32_t value = a_frame[0] | (a_frame[1] << 8) | (a_frame[2] << 16);
					if (value & 0x800000) {
						value |= ~0xFFFFFF;
					}
					return static_cast<float>(value) / 8388608.0f;
				}
			case 32:
				if (a_format == 3) {  // IEEE float
					return std::clamp(ReadLE<float>(a_frame), -1.0f, 1.0f);
				}
				return static_cast<float>(ReadLE<std::int32_t>(a_frame)) / 2147483648.0f;
			default:
				return 0.0f;
			}
		}

		// parse a PCM RIFF wav into a 100Hz RMS envelope; nullptr on any mismatch
		std::shared_ptr<const Envelope> ParseWav(const std::filesystem::path& a_file)
		{
			std::ifstream in(a_file, std::ios::binary);
			if (!in) {
				return nullptr;
			}

			std::uint8_t header[12];
			if (!in.read(reinterpret_cast<char*>(header), 12) ||
				std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
				return nullptr;
			}

			std::uint16_t format = 0, channels = 0, bits = 0;
			std::uint32_t sampleRate = 0;
			bool          haveFmt = false;

			while (in) {
				std::uint8_t chunkHeader[8];
				if (!in.read(reinterpret_cast<char*>(chunkHeader), 8)) {
					return nullptr;
				}
				const auto chunkSize = ReadLE<std::uint32_t>(chunkHeader + 4);

				if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
					std::vector<std::uint8_t> fmt(chunkSize);
					if (chunkSize < 16 || !in.read(reinterpret_cast<char*>(fmt.data()), chunkSize)) {
						return nullptr;
					}
					format = ReadLE<std::uint16_t>(fmt.data());
					channels = ReadLE<std::uint16_t>(fmt.data() + 2);
					sampleRate = ReadLE<std::uint32_t>(fmt.data() + 4);
					bits = ReadLE<std::uint16_t>(fmt.data() + 14);
					if (format == 0xFFFE && chunkSize >= 26) {  // extensible: first GUID word
						format = ReadLE<std::uint16_t>(fmt.data() + 24);
					}
					haveFmt = true;
				} else if (std::memcmp(chunkHeader, "data", 4) == 0) {
					if (!haveFmt || (format != 1 && format != 3) || channels == 0 ||
						sampleRate == 0 || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
						return nullptr;
					}
					const std::uint32_t bytesPerSample = bits / 8u;
					const std::uint32_t frameSize = bytesPerSample * channels;
					const std::uint32_t frames = chunkSize / frameSize;
					const std::uint32_t framesPerWindow = std::max(1u, sampleRate / 100u);

					auto envelope = std::make_shared<Envelope>();
					envelope->windowSec = static_cast<float>(framesPerWindow) / static_cast<float>(sampleRate);
					envelope->levels.reserve(frames / framesPerWindow + 1);

					std::vector<std::uint8_t> block(static_cast<std::size_t>(framesPerWindow) * frameSize);
					std::uint32_t remaining = frames;
					while (remaining > 0) {
						const auto take = std::min(remaining, framesPerWindow);
						if (!in.read(reinterpret_cast<char*>(block.data()),
								static_cast<std::streamsize>(take) * frameSize)) {
							break;
						}
						double sum = 0.0;
						for (std::uint32_t frame = 0; frame < take; ++frame) {
							float mono = 0.0f;
							const auto* framePtr = block.data() + static_cast<std::size_t>(frame) * frameSize;
							for (std::uint16_t ch = 0; ch < channels; ++ch) {
								mono += DecodeSample(framePtr + ch * bytesPerSample, format, bits);
							}
							mono /= static_cast<float>(channels);
							sum += static_cast<double>(mono) * mono;
						}
						envelope->levels.push_back(static_cast<float>(std::sqrt(sum / take)));
						remaining -= take;
					}

					float peak = 0.0f;
					for (const float level : envelope->levels) {
						peak = std::max(peak, level);
					}
					if (peak < 1.0e-4f) {
						return nullptr;  // silence — nothing worth syncing to
					}
					// normalize + perceptual shaping so mid-loud moans still open the mouth
					for (float& level : envelope->levels) {
						level = std::pow(std::clamp(level / peak, 0.0f, 1.0f), 0.6f);
					}
					envelope->durationSec = static_cast<float>(envelope->levels.size()) * envelope->windowSec;
					return envelope;
				} else {
					in.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
				}
			}
			return nullptr;
		}

		std::shared_ptr<const Envelope> GetEnvelope(const std::string& a_dataRelPath)
		{
			{
				std::scoped_lock lock{ g_envelopeLock };
				if (const auto it = g_envelopeCache.find(a_dataRelPath); it != g_envelopeCache.end()) {
					return it->second;
				}
			}
			auto envelope = ParseWav(std::filesystem::current_path() / "Data" / a_dataRelPath);
			if (!envelope) {
				logger::debug("LipSync: no readable PCM wav at '{}' — skipping", a_dataRelPath);
			}
			std::scoped_lock lock{ g_envelopeLock };
			if (g_envelopeCache.size() > 256) {
				g_envelopeCache.clear();
			}
			// negative results are cached too (nullptr): don't re-parse known misses
			g_envelopeCache[a_dataRelPath] = envelope;
			return envelope;
		}

		// ---------- active entries ----------

		struct Entry
		{
			RE::ActorHandle actor;
			RE::FormID      actorID = 0;
			std::int32_t    instanceId = 0;
			std::shared_ptr<const Envelope> env;
			RE::BSSoundHandle handle;
			std::chrono::steady_clock::time_point createdAt;
			std::chrono::steady_clock::time_point audibleAt;
			bool  audible = false;
			bool  stopping = false;
			bool  drove = false;  // wrote a non-zero phoneme → zero the mouth on removal
			float current = 0.0f;
		};

		std::vector<Entry> g_entries;
		std::unordered_set<RE::FormID> g_blocked;  // guarded by g_entriesLock
		std::mutex g_entriesLock;

		std::atomic<bool>  g_enabled{ true };
		std::atomic<float> g_gain{ 1.0f };
		std::atomic<float> g_attackTau{ 0.03f };
		std::atomic<float> g_releaseTau{ 0.09f };
		std::atomic<float> g_minLevel{ 0.04f };

		std::atomic<bool> g_applyPending{ false };
		std::once_flag    g_tickerOnce;
		std::condition_variable g_cv;

		std::chrono::steady_clock::time_point g_lastApply;  // main thread only

		void ZeroMouth(RE::Actor* a_actor)
		{
			auto* faceData = a_actor->GetFaceGenAnimationData();
			if (!faceData) {
				return;
			}
			RE::BSSpinLockGuard guard{ faceData->lock };
			auto& phonemes = faceData->phenomeKeyFrame;
			if (phonemes.values && phonemes.count > kPhonemeBigAah) {
				phonemes.SetValue(kPhonemeAah, 0.0f);
				phonemes.SetValue(kPhonemeBigAah, 0.0f);
			}
		}

		// main thread (SKSE task): advance every entry and write the phonemes
		void ApplyAll()
		{
			const auto now = std::chrono::steady_clock::now();
			float dt = std::chrono::duration<float>(now - g_lastApply).count();
			g_lastApply = now;
			dt = std::clamp(dt, 0.001f, 0.1f);

			const float gain = g_gain.load();
			const float minLevel = g_minLevel.load();
			const float attackTau = std::max(0.005f, g_attackTau.load());
			const float releaseTau = std::max(0.005f, g_releaseTau.load());

			std::scoped_lock lock{ g_entriesLock };
			std::erase_if(g_entries, [&](Entry& a_entry) {
				const auto actorPtr = a_entry.actor.get();
				auto*      actor = actorPtr.get();
				if (!actor || !actor->Get3D()) {
					return true;  // face is gone with the 3D; nothing to restore
				}

				float target = 0.0f;
				if (a_entry.stopping) {
					// fall through with target 0; removed once faded
				} else if (!a_entry.audible) {
					if (a_entry.handle.IsPlaying()) {
						a_entry.audible = true;
						a_entry.audibleAt = now;
					} else if (now - a_entry.createdAt > AUDIBLE_TIMEOUT) {
						return true;  // stream never started; nothing was driven
					}
				} else {
					const float t = std::chrono::duration<float>(now - a_entry.audibleAt).count();
					if (t >= a_entry.env->durationSec) {
						a_entry.stopping = true;
					} else {
						target = a_entry.env->Sample(t) * gain;
						if (target < minLevel) {
							target = 0.0f;
						}
						target = std::min(target, 1.0f);
					}
				}

				const float tau = target > a_entry.current ? attackTau : releaseTau;
				a_entry.current += (target - a_entry.current) * (1.0f - std::exp(-dt / tau));

				if (a_entry.stopping && a_entry.current <= 0.015f) {
					if (a_entry.drove) {
						ZeroMouth(actor);
					}
					return true;
				}

				if (auto* faceData = actor->GetFaceGenAnimationData()) {
					RE::BSSpinLockGuard guard{ faceData->lock };
					auto& phonemes = faceData->phenomeKeyFrame;
					if (phonemes.values && phonemes.count > kPhonemeBigAah) {
						phonemes.SetValue(kPhonemeAah, a_entry.current);
						// spill into BigAah on loud peaks for a wider open
						const float spill = std::max(0.0f, a_entry.current - 0.55f) / 0.45f;
						phonemes.SetValue(kPhonemeBigAah, spill * 0.35f);
						a_entry.drove = a_entry.drove || a_entry.current > 0.001f;
					}
				}
				return false;
			});
		}

		void EnsureTicker()
		{
			std::call_once(g_tickerOnce, []() {
				std::thread([]() {
					for (;;) {
						{
							std::unique_lock lock{ g_entriesLock };
							g_cv.wait(lock, []() { return !g_entries.empty(); });
						}
						if (!g_applyPending.exchange(true)) {
							SKSE::GetTaskInterface()->AddTask([]() {
								ApplyAll();
								g_applyPending.store(false);
							});
						}
						std::this_thread::sleep_for(TICK);
					}
				}).detach();
			});
		}
	}

	void Start(RE::Actor* a_actor, const std::string& a_dataRelPath,
		RE::BSSoundHandle a_handle, std::int32_t a_instanceId)
	{
		if (!g_enabled.load() || !a_actor || !a_handle.IsValid()) {
			return;
		}
		const auto envelope = GetEnvelope(a_dataRelPath);
		if (!envelope || envelope->durationSec < 0.05f) {
			return;
		}

		Entry entry;
		entry.actor = a_actor->GetHandle();
		entry.actorID = a_actor->GetFormID();
		entry.instanceId = a_instanceId;
		entry.env = envelope;
		entry.handle = a_handle;
		entry.createdAt = std::chrono::steady_clock::now();

		EnsureTicker();
		{
			std::scoped_lock lock{ g_entriesLock };
			if (g_blocked.contains(entry.actorID)) {
				return;  // another mod owns this actor's face right now
			}
			// one lipsync per actor: a new line replaces the old, keeping the
			// current mouth level so the transition doesn't snap shut
			for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
				if (it->actorID == entry.actorID) {
					entry.current = it->current;
					entry.drove = it->drove;
					g_entries.erase(it);
					break;
				}
			}
			g_entries.push_back(std::move(entry));
		}
		g_cv.notify_one();
	}

	void OnInstanceStopped(std::int32_t a_instanceId)
	{
		std::scoped_lock lock{ g_entriesLock };
		for (auto& entry : g_entries) {
			if (entry.instanceId == a_instanceId) {
				entry.stopping = true;
			}
		}
	}

	void StopFor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		const auto actorID = a_actor->GetFormID();
		std::scoped_lock lock{ g_entriesLock };
		for (auto& entry : g_entries) {
			if (entry.actorID == actorID) {
				entry.stopping = true;
			}
		}
	}

	void SetBlockedFor(RE::Actor* a_actor, bool a_blocked)
	{
		if (!a_actor) {
			return;
		}
		const auto actorID = a_actor->GetFormID();
		std::scoped_lock lock{ g_entriesLock };
		if (a_blocked) {
			g_blocked.insert(actorID);
			// drop, don't fade: a fade would end in ZeroMouth and stomp the face
			// the blocker is about to (or just did) apply
			std::erase_if(g_entries, [&](const Entry& a_entry) {
				return a_entry.actorID == actorID;
			});
		} else {
			g_blocked.erase(actorID);
		}
	}

	bool IsBlockedFor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::scoped_lock lock{ g_entriesLock };
		return g_blocked.contains(a_actor->GetFormID());
	}

	bool IsActiveFor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		const auto actorID = a_actor->GetFormID();
		std::scoped_lock lock{ g_entriesLock };
		for (const auto& entry : g_entries) {
			if (entry.actorID == actorID && !entry.stopping) {
				return true;
			}
		}
		return false;
	}

	void SetEnabled(bool a_enabled)
	{
		g_enabled.store(a_enabled);
		if (!a_enabled) {
			std::scoped_lock lock{ g_entriesLock };
			for (auto& entry : g_entries) {
				entry.stopping = true;
			}
		}
	}

	bool Enabled()
	{
		return g_enabled.load();
	}

	void SetGain(float a_gain)
	{
		g_gain.store(std::clamp(a_gain, 0.0f, 2.0f));
	}

	void ApplyConfig()
	{
		const auto settings = Config::Get();
		g_gain.store(std::clamp(settings->lipsyncGain, 0.0f, 2.0f));
		g_attackTau.store(static_cast<float>(settings->lipsyncAttackMs) / 1000.0f);
		g_releaseTau.store(static_cast<float>(settings->lipsyncReleaseMs) / 1000.0f);
		g_minLevel.store(settings->lipsyncMinLevel);
		SetEnabled(settings->lipsyncEnabled);
	}

	void Reset()
	{
		std::scoped_lock lock{ g_entriesLock };
		g_entries.clear();  // faces are rebuilt on load; nothing to restore
		g_blocked.clear();  // blockers re-seed from their own saved state on load
	}
}
