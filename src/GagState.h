#pragma once

#include "Config.h"

namespace GagState
{
	// Resolve the [gag] keyword forms (plugin|formid) into live BGSKeyword
	// pointers via TESDataHandler. Call once the game data is loaded and again
	// on config reload; safe to call with no keywords (detection stays dormant).
	void Resolve(const Config::Settings& a_settings);

	// Does this actor currently wear any configured gag keyword? False when gag
	// detection is disabled or no keywords resolved. Walks the actor's worn
	// items, so call it at voice-line rate, not per frame.
	bool IsGagged(RE::Actor* a_actor);
}
