#include "Tags.h"

#include <unordered_set>

namespace Tags
{
	namespace
	{
		struct Vocab
		{
			// normalized token -> bit index
			std::unordered_map<std::string, int> tokenBit;
			int  bitWeight[64] = {};      // per-bit axis weight
			Mask bitAxisMask[64] = {};    // per-bit: mask of ALL tokens on its axis
			std::vector<std::string> bitToken;  // bit index -> display token
		};

		std::shared_ptr<const Vocab> g_vocab = std::make_shared<Vocab>();
		std::mutex g_vocabLock;

		std::shared_ptr<const Vocab> GetVocab()
		{
			std::scoped_lock lock{ g_vocabLock };
			return g_vocab;
		}

		// split on whitespace/commas and normalize each token
		std::vector<std::string> Tokenize(std::string_view a_text)
		{
			std::vector<std::string> out;
			std::string current;
			const auto flush = [&]() {
				if (!current.empty()) {
					auto norm = Config::Normalize(current);
					if (!norm.empty()) {
						out.push_back(std::move(norm));
					}
					current.clear();
				}
			};
			for (const char c : a_text) {
				if (c == ' ' || c == '\t' || c == ',' || c == ';') {
					flush();
				} else {
					current += c;
				}
			}
			flush();
			return out;
		}
	}

	void Configure(const Config::Settings& a_settings)
	{
		// the vocabulary comes ENTIRELY from the merged [tags] config — the DLL
		// ships none (SFW-neutral default, same principle as the empty slot and
		// sfx tables). No [tags] anywhere = empty vocab = tag layer dormant.
		const auto& axes = a_settings.tagAxes;

		auto vocab = std::make_shared<Vocab>();
		int bit = 0;
		for (const auto& axis : axes) {
			Mask axisMask = 0;
			std::vector<int> axisBits;
			for (const auto& token : axis.tokens) {
				if (bit >= 64) {
					logger::warn("[tags]: vocabulary exceeds 64 tokens — token '{}' (axis {}) dropped",
						token, axis.name);
					continue;
				}
				if (vocab->tokenBit.contains(token)) {
					logger::warn("[tags]: duplicate token '{}' (axis {}) ignored", token, axis.name);
					continue;
				}
				vocab->tokenBit[token] = bit;
				vocab->bitWeight[bit] = axis.weight;
				vocab->bitToken.push_back(token);
				axisMask |= Mask{ 1 } << bit;
				axisBits.push_back(bit);
				++bit;
			}
			for (const int b : axisBits) {
				vocab->bitAxisMask[b] = axisMask;
			}
		}
		if (!vocab->tokenBit.empty()) {
			logger::info("Tags: vocabulary configured — {} tokens across {} axes",
				vocab->tokenBit.size(), axes.size());
		}

		std::scoped_lock lock{ g_vocabLock };
		g_vocab = std::move(vocab);
	}

	bool IsConfigured()
	{
		return !GetVocab()->tokenBit.empty();
	}

	bool ParseConstraints(std::string_view a_text, Mask& a_out, std::string& a_error)
	{
		const auto vocab = GetVocab();
		a_out = 0;
		for (const auto& token : Tokenize(a_text)) {
			const auto it = vocab->tokenBit.find(token);
			if (it == vocab->tokenBit.end()) {
				a_error = "unknown tag token '" + token + "'";
				return false;
			}
			const Mask bitMask = Mask{ 1 } << it->second;
			if (a_out & vocab->bitAxisMask[it->second] & ~bitMask) {
				// a second token on an axis that already contributed one: the
				// request carries at most one fact per axis, so this set could
				// never qualify — surface it instead of stranding the file
				a_error = "contradictory axis tokens ('" + token + "' conflicts with an earlier token on its axis)";
				return false;
			}
			a_out |= bitMask;
		}
		return true;
	}

	Mask ParseFacts(std::string_view a_text)
	{
		if (a_text.empty()) {
			return 0;
		}
		const auto vocab = GetVocab();
		Mask facts = 0;
		std::string unknown;
		std::string conflicting;
		const auto note = [](std::string& a_list, const std::string& a_token) {
			if (!a_list.empty()) {
				a_list += ' ';
			}
			a_list += a_token;
		};
		for (const auto& token : Tokenize(a_text)) {
			const auto it = vocab->tokenBit.find(token);
			if (it == vocab->tokenBit.end()) {
				note(unknown, token);
				continue;
			}
			const Mask bitMask = Mask{ 1 } << it->second;
			if (facts & vocab->bitAxisMask[it->second] & ~bitMask) {
				// a request states ONE fact per axis. Two would cover both
				// single-token pools at an identical score, and PickNext's mask
				// tie-break would then make the higher-bit pool unreachable for
				// good — so keep the first token of the axis and say so.
				note(conflicting, token);
				continue;
			}
			facts |= bitMask;
		}
		if (!unknown.empty() || !conflicting.empty()) {
			// warn once per distinct fact string: play calls repeat every line
			static std::unordered_set<std::string> warned;
			static std::mutex warnedLock;
			std::scoped_lock lock{ warnedLock };
			if (warned.insert(std::string(a_text)).second) {
				if (!unknown.empty()) {
					logger::warn("PlayVoiceTagged: unknown fact token(s) '{}' in '{}' ignored", unknown, a_text);
				}
				if (!conflicting.empty()) {
					logger::warn("PlayVoiceTagged: fact token(s) '{}' in '{}' share an axis with an earlier "
					             "token — dropped (a request carries at most one fact per axis)",
						conflicting, a_text);
				}
			}
		}
		return facts;
	}

	bool ContainsKnownToken(std::string_view a_text)
	{
		const auto vocab = GetVocab();
		for (const auto& token : Tokenize(a_text)) {
			if (vocab->tokenBit.contains(token)) {
				return true;
			}
		}
		return false;
	}

	bool HasAxisConflict(Mask a_mask)
	{
		const auto vocab = GetVocab();
		for (int b = 0; b < 64; ++b) {
			const Mask bit = Mask{ 1 } << b;
			if ((a_mask & bit) && (a_mask & vocab->bitAxisMask[b] & ~bit)) {
				return true;
			}
		}
		return false;
	}

	int Score(Mask a_mask)
	{
		const auto vocab = GetVocab();
		int score = 0;
		for (int b = 0; b < 64 && a_mask; ++b) {
			if (a_mask & (Mask{ 1 } << b)) {
				score += vocab->bitWeight[b];
				a_mask &= ~(Mask{ 1 } << b);
			}
		}
		return score;
	}

	std::string Describe(Mask a_mask)
	{
		if (a_mask == 0) {
			return "-";
		}
		const auto vocab = GetVocab();
		std::string out;
		for (std::size_t b = 0; b < vocab->bitToken.size(); ++b) {
			if (a_mask & (Mask{ 1 } << b)) {
				if (!out.empty()) {
					out += ' ';
				}
				out += vocab->bitToken[b];
			}
		}
		return out;
	}
}
