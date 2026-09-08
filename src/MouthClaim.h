#pragma once

namespace MouthClaim
{
	// ---------------------------------------------------------------------------
	// Cross-mod mouth arbitration.
	//
	// A CLAIM means: something other than AudioUtil is driving this actor's mouth
	// for a spoken line right now. It is symmetric — while a claim is live,
	// AudioUtil's own lipsync stays off that mouth too (checked at LipSync::Start
	// and on the mid-line handover re-check), and expression mods asking
	// IsMouthBusy() are told to leave the phoneme half of their presets alone.
	//
	// A claim is NOT an audio lock: playback, captions, ducking and volume groups
	// are all unaffected. It only ever answers "who moves the jaw".
	//
	// Claims are owner-scoped: one slot per (actor, owner). A second mod claiming
	// the same actor does not displace the first, and Release only clears the
	// caller's own slot, so no mod can end another's claim early. Claims live in
	// the DLL only — never in the save — and are dropped on load / new game.
	// ---------------------------------------------------------------------------

	// the longest a single claim can hold a mouth. A line longer than this
	// re-claims (senders are expected to; a lost release then costs 30s, not a
	// session). Anything <= 0 gets kDefaultClaimSeconds instead of holding forever.
	inline constexpr float kMaxClaimSeconds = 30.0f;
	inline constexpr float kDefaultClaimSeconds = 5.0f;

	// Claim a_actor's mouth for a_seconds (clamped to kMaxClaimSeconds; <= 0 means
	// kDefaultClaimSeconds). The claim is a DEADLINE, not a flag, so a sender that
	// dies mid-line cannot strand a mouth. a_owner keys the claim slot — a re-claim
	// by the same owner replaces only that owner's claim; an empty owner is a legal
	// key shared by anonymous callers. Matching is case-insensitive. No-op for null.
	void Claim(RE::Actor* a_actor, float a_seconds, std::string_view a_owner);

	// Release this owner's claim early (a skipped line, a scene cut). Idempotent
	// and scoped: releasing an actor claimed only by someone else does nothing.
	void Release(RE::Actor* a_actor, std::string_view a_owner);

	// Is any foreign claim on this actor still live? (Claims only — see IsBusy.)
	bool IsClaimed(RE::Actor* a_actor);

	// Owner tag of the live claim with the furthest deadline, or "" when nothing holds
	// this mouth — diagnostics, so a held jaw can name its holder. A claim made with an
	// empty owner tag reports as "<anonymous>", so "" always means UNCLAIMED.
	std::string Owner(RE::Actor* a_actor);

	// Is the game itself speaking a line through this actor? True while the engine
	// has facegen dialogue data on it — which covers the player under a voice mod
	// that speaks via Player.SpeakSound (DBVO), where the engine drives the mouth
	// and never tells anyone. This is the check MFG Fix's IsInDialogue reports; it
	// is NOT LipSync's MenuTopicManager::speaker test, which answers only for the
	// NPC the player is talking to.
	bool IsEngineDialogue(RE::Actor* a_actor);

	// The union predicate for expression mods: engine dialogue, or AudioUtil
	// lipsyncing this actor, or a live foreign claim. One call, all three cases.
	bool IsBusy(RE::Actor* a_actor);

	// Seconds until this actor's furthest claim expires, 0.0 when unclaimed. With
	// Owner() this is enough for a consumer to render its own claim display.
	float TimeLeft(RE::Actor* a_actor);

	// Every actor with a live claim right now, as form ids (the caller resolves them —
	// this layer never touches the form table). Swept like every other query.
	std::vector<RE::FormID> ClaimedActorIDs();

	// One line per live claim (actor, owner, seconds left) for `autest claims`.
	std::string Describe();

	// drop every claim (preload / new game)
	void Reset();
}
