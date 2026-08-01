#pragma once

#include "Config.h"

// Worn-tongue lipsync suppression. Mirrors GagState's device detection: resolves
// the [tongue] markers into live forms and reports whether an actor currently
// wears one. A visible tongue (an equipped tongue armor) needs the mouth held
// open, so a lipsync jaw-flap would clip it — LipSync consults this to stay off
// that actor's mouth, exactly as it does for a gagged actor. Detection only; no
// voice rerouting (so there is no [tongue] analogue to [gag]'s gag_slot).
namespace TongueState
{
	// Resolve the [tongue] markers (plugin|formid) into live forms via
	// TESDataHandler: keywords into BGSKeyword pointers, items into
	// TESBoundObject pointers. Call once the game data is loaded and again on
	// config reload; safe to call with no markers (detection stays dormant).
	void Resolve(const Config::Settings& a_settings);

	// Does this actor currently wear any configured tongue marker — an item form
	// from [tongue].items, or an item carrying a keyword from [tongue].keywords?
	// False when tongue detection is disabled or nothing resolved. Walks the
	// actor's worn items, so call it at voice-line rate, not per frame.
	bool IsWearingTongue(RE::Actor* a_actor);
}
