#pragma once

#include "Config.h"

// Tag-scored playback: a closed token vocabulary organized into weighted
// axes (mood/intensity/direction/...). A play request carries FACTS (what the
// scene is); a scanned file/folder carries CONSTRAINTS (what the line commits
// to). A pool qualifies iff constraints ⊆ facts, and the highest weight-sum
// wins — so an empty fact set reproduces legacy (untagged-only) behavior by
// construction, with no variation branching anywhere else in the plugin.
namespace Tags
{
	using Mask = std::uint64_t;

	// facts value that qualifies every pool — for introspection paths that ask
	// "does this category have ANY content" (legacy CategoryExists semantics)
	inline constexpr Mask kAllFacts = ~Mask{ 0 };

	// rebuild the vocabulary from the merged [tags] config (additive across
	// base + overlays — each consumer mod ships its own axes; see Config).
	// The DLL ships NO vocabulary of its own (SFW-neutral, like the empty
	// default slot/sfx tables), and with no [tags] anywhere the whole tag
	// layer is dormant (scans flat, facts parse to 0). Call after
	// Config::Load, before FolderCache::Rebuild (the scan parses folder/file
	// tags against it).
	void Configure(const Config::Settings& a_settings);

	// true when a [tags] vocabulary is loaded; false = tag layer dormant
	// (FolderCache scans category folders flat, exactly pre-tags behavior)
	bool IsConfigured();

	// parse a token string ("rcv victim intense", any whitespace/comma
	// separated, case/punct-insensitive per token) into constraint form.
	// Returns false when a token is unknown or two tokens share an axis —
	// the caller logs context and excludes the file/folder (never wrong-tone).
	// a_error receives a short reason for the log line.
	bool ParseConstraints(std::string_view a_text, Mask& a_out, std::string& a_error);

	// parse a request's fact string: unknown tokens are dropped, and so is a
	// second token on an axis that already contributed one (a request carries
	// at most ONE fact per axis — two would qualify both single-token pools at
	// the same score and strand the tie-breaker's loser forever). Both cases
	// warn once per distinct text, so a consumer-side mistake degrades to
	// fewer facts, not a dead call. Empty/blank text = 0 (legacy behavior).
	Mask ParseFacts(std::string_view a_text);

	// true when at least one token of a_text is in the vocabulary. Lets the
	// scan tell an intentional tag carrier from an unrelated pack's naming
	// convention ("moan_04 [loud].wav" in a pack that knows nothing about
	// tags) — the latter is not a broken tag set, it simply isn't one.
	bool ContainsKnownToken(std::string_view a_text);

	// constraints ⊆ facts
	inline bool Qualifies(Mask a_pool, Mask a_facts)
	{
		return (a_pool & ~a_facts) == 0;
	}

	// true when the mask holds two tokens of one axis (a request carries at
	// most one fact per axis, so such a set can never qualify). Used by the
	// scan to flag a folder-tag ∪ file-tag contradiction (e.g. soft ∪ intense).
	bool HasAxisConflict(Mask a_mask);

	// sum of the axis weights of the set tokens (specificity score)
	int Score(Mask a_mask);

	// human-readable token list for logs ("victim intense"); "-" for 0
	std::string Describe(Mask a_mask);
}
