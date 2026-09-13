#pragma once

#include "Tags.h"

namespace VoiceLog
{
	// ---------------------------------------------------------------------------
	// Pack-author playback log — a second, deliberately boring log file
	// (<SKSE logs>\AudioUtil_Voices.log) holding ONE LINE PER VOICE LINE:
	// who spoke, the category the consumer asked for, the facts it carried, where
	// resolution actually landed, and the exact file that played.
	//
	// It exists because AudioUtil.log cannot answer the only question a voicepack
	// author has — "my pack has a folder for this beat; why did I hear a stock
	// moan instead?" The main log is a plugin diagnostic (scan rosters, decode
	// errors, one warning per missing category for the whole session); this one is
	// a transcript. The `via` column is the payload: a line that resolved out of
	// the author's own slot is marked, so a naming/alias mismatch shows up as a
	// column of '!' instead of as silence to be reverse-engineered.
	//
	// Off by default ([general] voice_log). Runtime toggle: `autest voicelog`.
	// ---------------------------------------------------------------------------

	enum class Mode
	{
		Off,     // no file
		Player,  // the player character's lines only (the common authoring case)
		All      // every voice line, PC and NPC
	};

	// parse the [general] voice_log value ("off" / "player" / "all"; also accepts
	// the bools true/false as all/off). Unknown text -> Off, warned by the caller.
	std::optional<Mode> ParseMode(std::string_view a_text);

	// open/close/truncate the file to match the configured mode. kDataLoaded +
	// ReloadConfig, like every other ApplyConfig.
	void ApplyConfig();

	// runtime override (autest voicelog on|off|all) — same effect as the config
	// value, not persisted. Current mode for the status readout.
	void SetMode(Mode a_mode);
	Mode CurrentMode();

	// what a consumer asked for, carried alongside the play so the log line can
	// state the request and the outcome together. Filled by the PlayVoice natives;
	// PlayFile/PlayFolder pass nothing (they name a path, not a category, so there
	// is no resolution story to tell).
	struct Request
	{
		RE::Actor*       speaker{ nullptr };
		std::string_view slot;      // the slot the actor resolved to ("F2")
		std::string_view category;  // exactly as the consumer spelled it
		Tags::Mask       facts{ 0 };
		bool             viaSfx{ false };  // resolved through the sfx slot / [sfx] table
	};

	// a line played: a_folderKey is FolderCache's "<normalized slot>/<category>",
	// a_poolTags the tag set of the pool the pick came from, and a_descended
	// whether that pool was BELOW the best qualifying one — i.e. the best pool had
	// run out of lines this draw and yielded. That is the signal an author needs to
	// see their one-clip tagged pool being padded out by the floor beneath it.
	void Pick(const Request& a_req, const std::string& a_folderKey,
		const std::string& a_file, Tags::Mask a_poolTags, bool a_descended,
		std::int32_t a_id);

	// nothing played. a_reason is a short cause ("no folder", "channel busy", ...).
	// A miss is the most useful line in the file, so it is never filtered out.
	void Miss(const Request& a_req, std::string_view a_reason);

}
