#include "LipSync.h"

#include "Config.h"
#include "FuzCache.h"
#include "GagState.h"
#include "LipData.h"
#include "TongueState.h"

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
		// how often a live entry re-checks whether something else has taken over
		// the actor's mouth mid-line — a gag device equipped, or a dialogue
		// started. Walking worn items every 15ms tick is too heavy, so throttle it.
		constexpr auto HANDOVER_RECHECK = std::chrono::milliseconds(500);

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
			std::shared_ptr<const LipData::Anim> lip;  // real phoneme curves; null = envelope mode
			RE::BSSoundHandle handle;
			std::chrono::steady_clock::time_point createdAt;
			std::chrono::steady_clock::time_point audibleAt;
			std::chrono::steady_clock::time_point handoverCheckAt;  // next gag/dialogue re-check
			bool  audible = false;
			bool  stopping = false;
			bool  drove = false;  // wrote a non-zero phoneme → zero the mouth on removal
			bool  droveModifiers = false;  // lip mode also drove blink/brow channels
			// envelope mode: the smoothed mouth level. Lip mode: a master fade
			// (0..1) multiplied over the authored curves so starts/stops ramp.
			float current = 0.0f;
			float lipT = 0.0f;  // lip mode: last playback time sampled (held while fading out)
		};

		// Leaked on purpose: the detached ticker thread (EnsureTicker) runs until
		// the process dies, so it can still touch this state during static teardown
		// at game exit. Binding each name to a heap object that is never freed keeps
		// them valid for the thread's whole lifetime (no use-after-destruction
		// crash-on-exit). All usages below are unchanged - still plain references.
		std::vector<Entry>&             g_entries = *new std::vector<Entry>();
		std::mutex&                     g_entriesLock = *new std::mutex();

		std::atomic<bool>  g_enabled{ true };
		std::atomic<bool>  g_blockInDialogue{ true };
		std::atomic<bool>  g_useLipFiles{ true };
		std::atomic<bool>  g_driveModifiers{ false };
		std::atomic<bool>  g_pseudoPhonemes{ false };
		std::atomic<float> g_leadSec{ 0.0f };  // mouth timing lead ([lipsync] lead_ms)
		std::atomic<float> g_gain{ 1.0f };
		std::atomic<float> g_attackTau{ 0.03f };
		std::atomic<float> g_releaseTau{ 0.09f };
		std::atomic<float> g_minLevel{ 0.04f };

		std::atomic<bool> g_applyPending{ false };
		std::once_flag    g_tickerOnce;
		std::condition_variable& g_cv = *new std::condition_variable();  // leaked, see g_entries above

		std::chrono::steady_clock::time_point g_lastApply;  // main thread only

		// ---------- pseudo-phoneme synthesis ----------
		// For lines that ship no .lip ([lipsync] pseudo_phonemes): synthesize
		// phoneme curves from the amplitude envelope so envelope-only audio still
		// articulates instead of the single-channel Aah jaw-flap. The envelope is
		// segmented into syllables (voiced runs, split at deep valleys); each
		// syllable opens ONE vowel picked deterministically from the file path
		// (a given wav always mouths the same way), and a brief BMP lip-closure
		// lands just before any syllable that follows a real silence gap — the
		// engine renders 0/closure frames distinctly, so mouths visibly shut
		// between moans/phrases. The output is a LipData::Anim, so playback goes
		// through the exact lip-mode path (gain, master fade, full-channel
		// zeroing) with no special casing; the shaping the envelope mode applies
		// live (attack/release, min_level) is baked in here instead, because lip
		// mode plays curves verbatim.

		constexpr std::uint32_t kPhonemeBMP = 2;
		constexpr std::uint32_t kPhonemeEh = 6;
		constexpr std::uint32_t kPhonemeOh = 11;
		constexpr std::uint32_t kPhonemeOohQ = 12;

		std::uint64_t Fnv1a64(std::string_view a_text)
		{
			std::uint64_t hash = 0xCBF29CE484222325ull;
			for (const char c : a_text) {
				hash ^= static_cast<std::uint8_t>(c);
				hash *= 0x100000001B3ull;
			}
			return hash;
		}

		// splitmix64 finalizer: stateless per-syllable pseudo-random pick
		std::uint64_t Mix64(std::uint64_t a_x)
		{
			a_x += 0x9E3779B97F4A7C15ull;
			a_x = (a_x ^ (a_x >> 30)) * 0xBF58476D1CE4E5B9ull;
			a_x = (a_x ^ (a_x >> 27)) * 0x94D049BB133111EBull;
			return a_x ^ (a_x >> 31);
		}

		std::shared_ptr<const LipData::Anim> SynthesizePseudoLip(
			const Envelope& a_env, const std::string& a_pathSeed)
		{
			const auto frames =
				static_cast<std::uint32_t>(a_env.durationSec * LipData::FPS) + 1;
			if (frames < 3) {
				return nullptr;
			}

			std::vector<float> level(frames);
			const float voiced = std::max(g_minLevel.load(), 0.05f);
			for (std::uint32_t f = 0; f < frames; ++f) {
				const float sample = a_env.Sample(static_cast<float>(f) / LipData::FPS);
				level[f] = sample >= voiced ? sample : 0.0f;  // min_level baked in
			}

			// voiced runs, split at deep valleys (local min under 60% of the
			// loudest frame since the last boundary) — each piece is a "syllable"
			struct Syllable
			{
				std::uint32_t start;
				std::uint32_t end;  // exclusive
			};
			std::vector<Syllable> syllables;
			std::uint32_t runStart = UINT32_MAX;
			for (std::uint32_t f = 0; f <= frames; ++f) {
				const bool on = f < frames && level[f] > 0.0f;
				if (on && runStart == UINT32_MAX) {
					runStart = f;
				} else if (!on && runStart != UINT32_MAX) {
					std::uint32_t segStart = runStart;
					float peak = level[runStart];
					for (std::uint32_t i = runStart + 1; i + 1 < f; ++i) {
						peak = std::max(peak, level[i]);
						if (level[i] <= level[i - 1] && level[i] <= level[i + 1] &&
							level[i] < 0.6f * peak && i - segStart >= 3) {
							syllables.push_back({ segStart, i });
							segStart = i;
							peak = level[i];
						}
					}
					syllables.push_back({ segStart, f });
					runStart = UINT32_MAX;
				}
			}
			if (syllables.empty()) {
				return nullptr;
			}

			auto anim = std::make_shared<LipData::Anim>();
			anim->frames = frames;
			anim->durationSec = a_env.durationSec;
			for (const auto ch : { kPhonemeAah, kPhonemeBigAah, kPhonemeBMP,
					 kPhonemeEh, kPhonemeOh, kPhonemeOohQ }) {
				anim->values[ch].assign(frames, 0.0f);
			}

			const std::uint64_t seed = Fnv1a64(a_pathSeed);
			std::uint32_t prevVowel = UINT32_MAX;
			std::uint32_t prevEnd = 0;
			for (std::size_t s = 0; s < syllables.size(); ++s) {
				const auto& syl = syllables[s];
				// weighted vowel pick: Aah 4, Oh 3, OohQ 2, Eh 1; never the same
				// vowel twice in a row
				const auto roll = Mix64(seed + s) % 10;
				std::uint32_t vowel = roll < 4 ? kPhonemeAah :
				                      roll < 7 ? kPhonemeOh :
				                      roll < 9 ? kPhonemeOohQ :
				                                 kPhonemeEh;
				if (vowel == prevVowel) {
					vowel = vowel == kPhonemeAah  ? kPhonemeOh :
					        vowel == kPhonemeOh   ? kPhonemeOohQ :
					        vowel == kPhonemeOohQ ? kPhonemeAah :
					                                kPhonemeOh;
				}
				prevVowel = vowel;

				for (std::uint32_t f = syl.start; f < syl.end; ++f) {
					anim->values[vowel][f] = level[f];
					if (vowel == kPhonemeAah) {
						// same loud-peak spill into BigAah as the live envelope mode
						anim->values[kPhonemeBigAah][f] =
							std::max(0.0f, level[f] - 0.55f) / 0.45f * 0.35f;
					}
				}

				// lips close briefly before a syllable that follows a real gap
				// (>=5 frames ≈ 165 ms of silence)
				if (syl.start >= 2 && syl.start - prevEnd >= 5) {
					anim->values[kPhonemeBMP][syl.start - 2] = 0.55f;
					anim->values[kPhonemeBMP][syl.start - 1] = 0.85f;
				}
				prevEnd = syl.end;
			}

			// bake the envelope mode's attack/release shaping into the vowel
			// curves (closures stay crisp on purpose)
			const float dt = 1.0f / LipData::FPS;
			const float attack = 1.0f - std::exp(-dt / std::max(0.005f, g_attackTau.load()));
			const float release = 1.0f - std::exp(-dt / std::max(0.005f, g_releaseTau.load()));
			for (const auto ch : { kPhonemeAah, kPhonemeBigAah, kPhonemeEh,
					 kPhonemeOh, kPhonemeOohQ }) {
				float current = 0.0f;
				for (float& value : anim->values[ch]) {
					current += (value - current) * (value > current ? attack : release);
					value = current;
				}
			}
			return anim;
		}

		// a_allPhonemes: zero every phoneme channel (lip / pseudo-lip mode wrote
		// all 16). Envelope mode only ever writes Aah/BigAah, so it clears just
		// those two — zeroing 2..15 there would stomp phoneme channels another
		// mod (expression / mouth systems) may own on this actor.
		void ZeroMouth(RE::Actor* a_actor, bool a_alsoModifiers = false, bool a_allPhonemes = false)
		{
			auto* faceData = a_actor->GetFaceGenAnimationData();
			if (!faceData) {
				return;
			}
			RE::BSSpinLockGuard guard{ faceData->lock };
			auto& phonemes = faceData->phenomeKeyFrame;
			if (phonemes.values) {
				const auto limit = a_allPhonemes ? LipData::PHONEME_CHANNELS : kPhonemeBigAah + 1;
				const auto count = std::min<std::uint32_t>(phonemes.count, limit);
				for (std::uint32_t ch = 0; ch < count; ++ch) {
					phonemes.SetValue(ch, 0.0f);
				}
			}
			if (a_alsoModifiers) {
				auto& modifiers = faceData->modifierKeyFrame;
				if (modifiers.values) {
					const auto count = std::min<std::uint32_t>(modifiers.count, 16);
					for (std::uint32_t ch = 0; ch < count; ++ch) {
						modifiers.SetValue(ch, 0.0f);
					}
				}
			}
		}

		// true while the player is in a dialogue with this actor: the game's own
		// dialogue/voice system drives the speaker's mouth from the real voice
		// file, so AudioUtil must not fight it. `speaker` self-clears when the
		// dialogue menu closes (the still-talking tail moves to `lastSpeaker`).
		bool IsInDialogue(RE::Actor* a_actor)
		{
			auto* topicManager = RE::MenuTopicManager::GetSingleton();
			if (!topicManager) {
				return false;
			}
			const auto speaker = topicManager->speaker.get();
			return speaker && speaker->GetFormID() == a_actor->GetFormID();
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
			const bool  driveModifiers = g_driveModifiers.load();

			std::scoped_lock lock{ g_entriesLock };
			std::erase_if(g_entries, [&](Entry& a_entry) {
				const auto actorPtr = a_entry.actor.get();
				auto*      actor = actorPtr.get();
				if (!actor || !actor->Get3D()) {
					return true;  // face is gone with the 3D; nothing to restore
				}

				// hand-over guard: if a gag device is equipped or a dialogue starts
				// mid-line, that system owns the mouth now, so drop the entry rather
				// than fighting it (flicker). Throttled - the worn-item walk is too
				// heavy to run every tick. Drop without zeroing so the new owner's
				// face stays put.
				if (now >= a_entry.handoverCheckAt) {
					a_entry.handoverCheckAt = now + HANDOVER_RECHECK;
					if (GagState::IsGagged(actor) || TongueState::IsWearingTongue(actor) ||
						(g_blockInDialogue.load() && IsInDialogue(actor))) {
						return true;
					}
				}

				// `target` is the envelope mouth level, or in lip mode the master
				// fade (0..1) multiplied over the authored curves so starts,
				// stops and handovers ramp instead of snapping
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
					// lead: sample the curves slightly ahead of the playback clock
					// to compensate start-detection + mix-ahead latency (the audio
					// itself still ends at durationSec, so stop timing is unshifted)
					const float tMouth = t + g_leadSec.load();
					if (t >= a_entry.env->durationSec) {
						a_entry.stopping = true;
					} else if (a_entry.lip) {
						a_entry.lipT = tMouth;
						target = 1.0f;
					} else {
						target = a_entry.env->Sample(tMouth) * gain;
						if (target < minLevel) {
							target = 0.0f;
						}
						target = std::min(target, 1.0f);
					}
				}

				const float tau = target > a_entry.current ? attackTau : releaseTau;
				a_entry.current += (target - a_entry.current) * (1.0f - std::exp(-dt / tau));

				if (a_entry.stopping && a_entry.current <= 0.015f) {
					if (a_entry.drove || a_entry.droveModifiers) {
						// lip / pseudo-lip lines drove all 16 phonemes; envelope
						// lines only Aah/BigAah — clear only what we wrote
						ZeroMouth(actor, a_entry.droveModifiers, a_entry.lip != nullptr);
					}
					return true;
				}

				if (auto* faceData = actor->GetFaceGenAnimationData()) {
					RE::BSSpinLockGuard guard{ faceData->lock };
					auto& phonemes = faceData->phenomeKeyFrame;
					if (a_entry.lip) {
						// authored curves, sampled at playback time; while fading
						// out the last shape is held and scaled down (lipT keeps
						// its final value once the audible branch stops updating)
						const float fade = a_entry.current;
						if (phonemes.values) {
							const auto count = std::min<std::uint32_t>(phonemes.count, LipData::PHONEME_CHANNELS);
							for (std::uint32_t ch = 0; ch < count; ++ch) {
								const float value = std::clamp(
									a_entry.lip->Sample(ch, a_entry.lipT) * gain, 0.0f, 1.0f) * fade;
								phonemes.SetValue(ch, value);
								a_entry.drove = a_entry.drove || value > 0.001f;
							}
						}
						if (driveModifiers) {
							auto& modifiers = faceData->modifierKeyFrame;
							if (modifiers.values) {
								const auto count = std::min<std::uint32_t>(modifiers.count, 16);
								for (std::uint32_t ch = 0; ch < count; ++ch) {
									const float value = std::clamp(a_entry.lip->Sample(
										LipData::PHONEME_CHANNELS + ch, a_entry.lipT), 0.0f, 1.0f) * fade;
									modifiers.SetValue(ch, value);
									a_entry.droveModifiers = a_entry.droveModifiers || value > 0.001f;
								}
							}
						}
					} else if (phonemes.values && phonemes.count > kPhonemeBigAah) {
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
							// null during very early load / teardown; reset the flag so
							// the loop isn't wedged waiting for a task that never queued
							if (auto* task = SKSE::GetTaskInterface()) {
								task->AddTask([]() {
									ApplyAll();
									g_applyPending.store(false);
								});
							} else {
								g_applyPending.store(false);
							}
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
		// a gagged actor's mouth belongs to the device - don't lipsync over it
		if (GagState::IsGagged(a_actor)) {
			return;
		}
		// a visible tongue (equipped tongue armor) needs the mouth held open - a
		// lipsync jaw-flap would clip it, so stay off the mouth, same as a gag
		if (TongueState::IsWearingTongue(a_actor)) {
			return;
		}
		// in a dialogue with the player the game drives the mouth from the real
		// voice file; stay off it (toggle: [lipsync] block_in_dialogue)
		if (g_blockInDialogue.load() && IsInDialogue(a_actor)) {
			return;
		}
		// a .fuz plays via its FuzCache-extracted file; when that is the decoded
		// PCM wav, the envelope reads from it and fuz lines lipsync like wavs.
		// (A raw-xwm fallback fails the PCM parse below and skips, as before.)
		std::string envelopeSource = a_dataRelPath;
		if (FuzCache::IsFuzPath(envelopeSource)) {
			envelopeSource = FuzCache::Resolve(envelopeSource);
			if (envelopeSource.empty()) {
				return;
			}
		}
		const auto envelope = GetEnvelope(envelopeSource);
		if (!envelope || envelope->durationSec < 0.05f) {
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		Entry entry;
		entry.actor = a_actor->GetHandle();
		entry.actorID = a_actor->GetFormID();
		entry.instanceId = a_instanceId;
		entry.env = envelope;
		entry.handle = a_handle;
		entry.createdAt = now;
		entry.handoverCheckAt = now + HANDOVER_RECHECK;  // checked just above; next re-check later
		// real phoneme curves when the line ships a lip (same-stem .lip, or the
		// fuz's embedded block — looked up via the ORIGINAL path, not the
		// FuzCache-extracted wav). Null = amplitude-envelope mode, as before.
		if (g_useLipFiles.load()) {
			entry.lip = LipData::GetFor(a_dataRelPath);
		}
		// no authored lip -> optionally synthesize pseudo-phoneme curves from
		// the envelope (deterministic per path, cheap: a few KB of floats)
		if (!entry.lip && g_pseudoPhonemes.load()) {
			entry.lip = SynthesizePseudoLip(*envelope, a_dataRelPath);
		}

		EnsureTicker();
		{
			std::scoped_lock lock{ g_entriesLock };
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
		// Explicit hand-back: the consumer is taking the mouth (tongue/climax face).
		// DROP the line and leave the mouth as-is — do NOT fade to closed. Fading to
		// 0 here snaps the jaw shut over a just-shown tongue before the consumer's
		// expression pass reopens it. Same "drop without zeroing" as the ApplyAll
		// hand-over guard.
		std::erase_if(g_entries, [&](const Entry& e) { return e.actorID == actorID; });
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

	void SetLipFilesEnabled(bool a_enabled)
	{
		g_useLipFiles.store(a_enabled);
	}

	bool LipFilesEnabled()
	{
		return g_useLipFiles.load();
	}

	void SetPseudoLipEnabled(bool a_enabled)
	{
		g_pseudoPhonemes.store(a_enabled);
	}

	bool PseudoLipEnabled()
	{
		return g_pseudoPhonemes.load();
	}

	void SetLeadMs(std::int32_t a_ms)
	{
		g_leadSec.store(static_cast<float>(std::clamp(a_ms, -300, 300)) / 1000.0f);
	}

	std::int32_t LeadMs()
	{
		return static_cast<std::int32_t>(std::lround(g_leadSec.load() * 1000.0f));
	}

	void ApplyConfig()
	{
		const auto settings = Config::Get();
		g_gain.store(std::clamp(settings->lipsyncGain, 0.0f, 2.0f));
		g_attackTau.store(static_cast<float>(settings->lipsyncAttackMs) / 1000.0f);
		g_releaseTau.store(static_cast<float>(settings->lipsyncReleaseMs) / 1000.0f);
		g_minLevel.store(settings->lipsyncMinLevel);
		g_blockInDialogue.store(settings->lipsyncBlockInDialogue);
		g_useLipFiles.store(settings->lipsyncUseLipFiles);
		g_driveModifiers.store(settings->lipsyncDriveModifiers);
		g_pseudoPhonemes.store(settings->lipsyncPseudoPhonemes);
		g_leadSec.store(static_cast<float>(settings->lipsyncLeadMs) / 1000.0f);
		LipData::ClearCache();
		SetEnabled(settings->lipsyncEnabled);
	}

	void Reset()
	{
		std::scoped_lock lock{ g_entriesLock };
		g_entries.clear();  // faces are rebuilt on load; nothing to restore
	}
}
