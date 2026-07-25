#pragma once

#include "Config.h"

namespace GagState
{
	// Resolve the [gag] markers (plugin|formid) into live forms via
	// TESDataHandler: keywords into BGSKeyword pointers, items into
	// TESBoundObject pointers. Call once the game data is loaded and again on
	// config reload; safe to call with no markers (detection stays dormant).
	void Resolve(const Config::Settings& a_settings);

	// Does this actor currently wear any configured gag marker — an item form
	// from [gag].items, or an item carrying a keyword from [gag].keywords?
	// False when gag detection is disabled or nothing resolved. Walks the
	// actor's worn items, so call it at voice-line rate, not per frame.
	bool IsGagged(RE::Actor* a_actor);
}
