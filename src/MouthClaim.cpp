#include "MouthClaim.h"

#include "LipSync.h"

namespace MouthClaim
{
	namespace
	{
		struct Slot
		{
			std::string                           key;       // owner, lowercased (the identity)
			std::string                           display;   // owner as the caller spelled it
			std::chrono::steady_clock::time_point deadline;
		};

		// what an empty owner tag reports as - see Owner()
		constexpr std::string_view kAnonymousOwner = "<anonymous>"sv;

		std::unordered_map<RE::FormID, std::vector<Slot>> g_claims;
		std::mutex                                        g_lock;

		std::string LowerKey(std::string_view a_owner)
		{
			std::string key{ a_owner };
			std::transform(key.begin(), key.end(), key.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return key;
		}

		// drop expired slots for one actor; erases the map entry when none are left.
		// Sweeping on query keeps this ticker-free — nothing here runs per frame.
		// Caller holds g_lock.
		std::vector<Slot>* SweepLocked(RE::FormID a_actorID,
			std::chrono::steady_clock::time_point a_now)
		{
			const auto it = g_claims.find(a_actorID);
			if (it == g_claims.end()) {
				return nullptr;
			}
			auto& slots = it->second;
			std::erase_if(slots, [&](const Slot& s) { return s.deadline <= a_now; });
			if (slots.empty()) {
				g_claims.erase(it);
				return nullptr;
			}
			return &slots;
		}

		// the live claim that expires LAST, which is the one every single-answer query
		// reports (a second mod's claim never displaces the first, so "who holds this
		// mouth" is the furthest deadline). null when nothing holds it. Caller holds g_lock.
		const Slot* FurthestLocked(RE::FormID a_actorID,
			std::chrono::steady_clock::time_point a_now)
		{
			const auto* slots = SweepLocked(a_actorID, a_now);
			if (!slots) {
				return nullptr;
			}
			return &*std::max_element(slots->begin(), slots->end(),
				[](const Slot& a, const Slot& b) { return a.deadline < b.deadline; });
		}

		// SweepLocked across the WHOLE map, for the two listing queries. Expiry lives
		// in one place on purpose: four hand-rolled sweeps were four places to forget,
		// and one forgotten leaves two queries reporting a claim the others dropped.
		// Caller holds g_lock.
		void SweepAllLocked(std::chrono::steady_clock::time_point a_now)
		{
			for (auto it = g_claims.begin(); it != g_claims.end();) {
				std::erase_if(it->second, [&](const Slot& s) { return s.deadline <= a_now; });
				it = it->second.empty() ? g_claims.erase(it) : std::next(it);
			}
		}
	}

	void Claim(RE::Actor* a_actor, float a_seconds, std::string_view a_owner)
	{
		if (!a_actor) {
			return;
		}
		float seconds = a_seconds > 0.0f ? a_seconds : kDefaultClaimSeconds;
		if (seconds > kMaxClaimSeconds) {
			seconds = kMaxClaimSeconds;
		}
		const auto now = std::chrono::steady_clock::now();
		const auto deadline = now + std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000.0f));
		const auto actorID = a_actor->GetFormID();
		auto       key = LowerKey(a_owner);

		{
			std::scoped_lock lock{ g_lock };
			auto& slots = g_claims[actorID];
			std::erase_if(slots, [&](const Slot& s) { return s.deadline <= now || s.key == key; });
			slots.push_back({ key, std::string{ a_owner }, deadline });
		}
		// Hand the mouth over immediately. Without this, a line AudioUtil is already
		// lipsyncing keeps writing phonemes until the next HANDOVER_RECHECK tick (up to
		// 500 ms - most of a short line), with both systems driving the same jaw.
		// StopFor drops the entry WITHOUT fading to closed, which is what a handover
		// wants: the mouth stays where it is for the new owner to take over.
		// MUST be outside g_lock - ApplyAll takes g_entriesLock then g_lock, so taking
		// them the other way round here would be a lock-order inversion.
		LipSync::StopFor(a_actor);

		logger::debug("MouthClaim: {:08X} claimed for {:.2f}s by '{}'", actorID, seconds,
			a_owner.empty() ? kAnonymousOwner : a_owner);
	}

	void Release(RE::Actor* a_actor, std::string_view a_owner)
	{
		if (!a_actor) {
			return;
		}
		const auto actorID = a_actor->GetFormID();
		const auto key = LowerKey(a_owner);
		bool       released = false;
		{
			std::scoped_lock lock{ g_lock };
			const auto       it = g_claims.find(actorID);
			if (it != g_claims.end()) {
				const auto before = it->second.size();
				std::erase_if(it->second, [&](const Slot& s) { return s.key == key; });
				released = it->second.size() != before;
				if (it->second.empty()) {
					g_claims.erase(it);
				}
			}
		}
		logger::debug("MouthClaim: {:08X} release by '{}' ({})", actorID,
			a_owner.empty() ? kAnonymousOwner : a_owner, released ? "held"sv : "no claim"sv);
	}

	bool IsClaimed(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::scoped_lock lock{ g_lock };
		return SweepLocked(a_actor->GetFormID(), std::chrono::steady_clock::now()) != nullptr;
	}

	std::string Owner(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return {};
		}
		std::scoped_lock lock{ g_lock };
		const auto*      furthest = FurthestLocked(a_actor->GetFormID(), std::chrono::steady_clock::now());
		if (!furthest) {
			return {};
		}
		// an anonymous claim (empty owner key) is still a claim - report it as the
		// same placeholder Describe() prints, so an empty return means UNCLAIMED and
		// nothing else. Callers use "" as their "nobody holds this mouth" test.
		return furthest->display.empty() ? std::string{ kAnonymousOwner } : furthest->display;
	}

	float TimeLeft(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return 0.0f;
		}
		const auto       now = std::chrono::steady_clock::now();
		std::scoped_lock lock{ g_lock };
		const auto*      furthest = FurthestLocked(a_actor->GetFormID(), now);
		if (!furthest) {
			return 0.0f;
		}
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			furthest->deadline - now).count();
		return static_cast<float>(ms) / 1000.0f;
	}

	std::vector<RE::FormID> ClaimedActorIDs()
	{
		const auto              now = std::chrono::steady_clock::now();
		std::scoped_lock        lock{ g_lock };
		SweepAllLocked(now);
		std::vector<RE::FormID> out;
		out.reserve(g_claims.size());
		for (const auto& entry : g_claims) {
			out.push_back(entry.first);
		}
		return out;
	}

	bool IsEngineDialogue(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		const auto faceGen = a_actor->GetFaceGenAnimationData();
		return faceGen && faceGen->dialogueData != nullptr;
	}

	bool IsBusy(RE::Actor* a_actor)
	{
		return a_actor &&
		       (IsEngineDialogue(a_actor) || LipSync::IsActiveFor(a_actor) || IsClaimed(a_actor));
	}

	std::string Describe()
	{
		const auto       now = std::chrono::steady_clock::now();
		std::scoped_lock lock{ g_lock };
		SweepAllLocked(now);
		std::string out;
		for (const auto& [actorID, slots] : g_claims) {
			for (const auto& slot : slots) {
				const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
					slot.deadline - now).count();
				out += std::format("{:08X} '{}' {:.1f}s\n", actorID,
					slot.display.empty() ? kAnonymousOwner : std::string_view{ slot.display },
					static_cast<float>(left) / 1000.0f);
			}
		}
		return out.empty() ? "no live mouth claims" : out;
	}

	void Reset()
	{
		std::scoped_lock lock{ g_lock };
		g_claims.clear();
	}
}
