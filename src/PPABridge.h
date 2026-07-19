#pragma once

namespace PPABridge
{
	struct Snapshot
	{
		std::uint32_t context = 0;      // AccuratePenetration SceneContext bitmask
		float         depth = 0.0f;     // max penetration depth across partners
		float         anusOpening = 0.0f;
		float         vaginalOpening = 0.0f;
		bool          ending = false;
	};

	// attempt to connect to AccuratePenetration.dll (kDataLoaded). Safe to call repeatedly.
	void TryConnect();
	bool Connected();

	std::optional<Snapshot> GetFor(RE::Actor* a_receiver);

	void SetEventRateMs(std::uint32_t a_ms);
}
