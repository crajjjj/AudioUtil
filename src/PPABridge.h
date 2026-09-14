#pragma once

namespace PPABridge
{
	struct Snapshot
	{
		std::uint32_t context = 0;   // AccuratePenetration SceneContext bitmask
		float         depth = 0.0f;  // max penetration depth across partners

		// WHERE, as opposed to context's WHAT KIND OF SCENE. PPA carries a
		// PenetrationSite per InteractionPartner (0 None, 1 Mouth, 2 Anus,
		// 3 Vagina, 4 Both, 5 HandL, 6 HandR, 7 Hands), which is what its own
		// redirect menu rewrites — so unlike the context bits (scene
		// classification, fixed for the scene) this tracks a switch mid-scene.
		//
		// `site` is the site of the deepest PARTNER. Note this is a narrower pick
		// than `depth`, which also takes selfInteraction and site-None partners
		// into account — so `depth` and `site` can describe different
		// interactions, and a consumer must not read `depth > 0 && site == X` as
		// "X is penetrated this deep". `siteMask` is
		// 1 << site for every partner, so DP reports both at once (bit 0 is
		// never set: None is the absence of a site, not a site).
		std::uint8_t  site = 0;      // deepest partner's site — where THIS actor is taken
		std::uint32_t siteMask = 0;  // every partner's site at once
		// self-penetration: the hole on the receiver that the RECEIVER is using
		// on themselves (masturbation). Also a site on this actor, but a
		// different act from the partner sites above, so it stays its own field.
		// 0 for most receivers. (PPA's docs: "the site in this struct refers to
		// where the receiver is self-penetrating".)
		std::uint8_t selfSite = 0;

		float anusOpening = 0.0f;
		float vaginalOpening = 0.0f;
		bool  ending = false;
	};

	// attempt to connect to AccuratePenetration.dll (kDataLoaded). Safe to call repeatedly.
	void TryConnect();
	bool Connected();

	std::optional<Snapshot> GetFor(RE::Actor* a_receiver);

	void SetEventRateMs(std::uint32_t a_ms);
}
