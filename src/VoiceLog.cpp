#include "VoiceLog.h"

#include <format>
#include <fstream>

#include "Config.h"

namespace VoiceLog
{
	namespace
	{
		std::mutex    g_lock;
		std::ofstream g_file;
		Mode          g_mode{ Mode::Off };

		std::filesystem::path LogPath()
		{
			auto dir = logger::log_directory();
			if (!dir) {
				return {};
			}
			return *dir / "AudioUtil_Voices.log";
		}

		// wall clock, same shape as the [HH:MM:SS.mmm] stamp spdlog puts in
		// AudioUtil.log, so an author can line the two files up on one event
		std::string Stamp()
		{
			using namespace std::chrono;
			const auto now = system_clock::now();
			const auto tt = system_clock::to_time_t(now);
			const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
			std::tm tm{};
			localtime_s(&tm, &tt);
			return std::format("{:02}:{:02}:{:02}.{:03}", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
		}

		// the requested slot is empty only for a call that never routed through one
		std::string OrDash(std::string_view a_text)
		{
			return a_text.empty() ? std::string{ "-" } : std::string{ a_text };
		}

		// CommonLib's GetName()/GetDisplayFullName() are game-virtual and can hand
		// back a null pointer (see the crash notes in CLAUDE.md) — never feed one
		// straight to std::format.
		std::string SpeakerName(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return "-";
			}
			const char* name = a_actor->GetDisplayFullName();
			std::string out = (name && *name) ? name : "?";
			if (a_actor->IsPlayerRef()) {
				out += " (PC)";
			}
			return out;
		}

		// which slot the folder key came out of, in its configured spelling. The
		// key is "<normalized slot>/<resolved category>"; FindSlot normalizes both
		// sides, so a display id comes back when the slot is still configured.
		void SplitKey(const std::string& a_key, std::string& a_slot, std::string& a_category)
		{
			const auto slash = a_key.find('/');
			if (slash == std::string::npos) {
				a_slot = "?";
				a_category = a_key;
				return;
			}
			a_slot = a_key.substr(0, slash);
			a_category = a_key.substr(slash + 1);
			if (const auto settings = Config::Get()) {
				if (const auto* slot = Config::FindSlot(*settings, a_slot)) {
					a_slot = slot->id;
				}
			}
		}

		bool Wants(RE::Actor* a_speaker)
		{
			switch (g_mode) {
			case Mode::All:
				return true;
			case Mode::Player:
				return a_speaker && a_speaker->IsPlayerRef();
			default:
				return false;
			}
		}

		void WriteLine(const std::string& a_line)
		{
			if (!g_file.is_open()) {
				return;
			}
			g_file << a_line << '\n';
			g_file.flush();  // a CTD mid-scene must not eat the line that caused it
		}

		void Open()
		{
			const auto path = LogPath();
			if (path.empty()) {
				logger::warn("voice log: no SKSE log directory — disabled");
				g_mode = Mode::Off;
				return;
			}
			g_file.open(path, std::ios::out | std::ios::trunc);
			if (!g_file.is_open()) {
				logger::warn("voice log: could not open {} — disabled", path.string());
				g_mode = Mode::Off;
				return;
			}
			WriteLine("# AudioUtil voice log — one line per voice line, for voicepack authors.");
			WriteLine("# mode: " + std::string(g_mode == Mode::Player ? "player" : "all") +
				"   (set [general] voice_log, or `autest voicelog off|player|all`)");
			WriteLine("#");
			WriteLine("# time | speaker | slot | category asked for | facts | via | file played | pool | handle");
			WriteLine("#");
			WriteLine("# 'via' is the column that matters: the <slot>/<category> resolution ACTUALLY landed");
			WriteLine("# on, and how far it had to go to get there:");
			WriteLine("#     (blank) the speaker's own slot answered the category asked for");
			WriteLine("#     ~       same slot, different category (an alias or a category fallback)");
			WriteLine("#     !       a DIFFERENT slot supplied it - the pack had no folder, or no pool");
			WriteLine("#             qualifying for the facts, so the fallback chain went past it");
			WriteLine("# A 'v' before the pool name means the best-scoring pool that qualified had run");
			WriteLine("# out of lines this draw and yielded one to the ladder below it - so a one-clip");
			WriteLine("# tagged pool alternates with the floor instead of repeating itself all scene.");
			WriteLine("# 'MISS' means nothing played at all, with the reason.");
			WriteLine("#");
			logger::info("voice log: writing {}", path.string());
		}

		void Close()
		{
			if (g_file.is_open()) {
				g_file.close();
			}
		}
	}

	std::optional<Mode> ParseMode(std::string_view a_text)
	{
		const auto norm = Config::Normalize(a_text);
		if (norm == "off" || norm == "false" || norm == "none" || norm == "0") {
			return Mode::Off;
		}
		if (norm == "player" || norm == "pc") {
			return Mode::Player;
		}
		if (norm == "all" || norm == "on" || norm == "true" || norm == "1") {
			return Mode::All;
		}
		return std::nullopt;
	}

	void ApplyConfig()
	{
		std::scoped_lock lock{ g_lock };
		const auto settings = Config::Get();
		auto       wanted = Mode::Off;
		if (settings) {
			if (const auto parsed = ParseMode(settings->voiceLog)) {
				wanted = *parsed;
			}
		}
		if (wanted == g_mode && (wanted == Mode::Off) == !g_file.is_open()) {
			return;  // unchanged — keep the session's lines rather than truncating
		}
		g_mode = wanted;
		Close();
		if (g_mode != Mode::Off) {
			Open();
		}
	}

	// Unlike ApplyConfig this ALWAYS restarts the file, including when the mode is
	// already what was asked for. Running the console command is an explicit "start
	// a fresh capture here" — which is the whole workflow the log is for ("run it
	// right before the scene you care about"). Short-circuiting on an unchanged mode
	// silently handed back a whole-session file instead, and only when the config
	// happened to already name that mode, which is the worst kind of surprise.
	void SetMode(Mode a_mode)
	{
		std::scoped_lock lock{ g_lock };
		g_mode = a_mode;
		Close();
		if (g_mode != Mode::Off) {
			Open();
		}
	}

	Mode CurrentMode()
	{
		std::scoped_lock lock{ g_lock };
		return g_mode;
	}

	void Pick(const Request& a_req, const std::string& a_folderKey,
		const std::string& a_file, Tags::Mask a_poolTags, bool a_descended,
		std::int32_t a_id)
	{
		std::scoped_lock lock{ g_lock };
		if (g_mode == Mode::Off || !Wants(a_req.speaker)) {
			return;
		}
		std::string viaSlot, viaCategory;
		SplitKey(a_folderKey, viaSlot, viaCategory);

		// own slot AND the category asked for = the pack answered exactly. Anything
		// else is a substitution the author almost certainly wants to know about.
		const bool ownSlot = Config::Normalize(viaSlot) == Config::Normalize(a_req.slot);
		const bool sameCat = Config::Normalize(viaCategory) == Config::Normalize(a_req.category);
		const char* mark = ownSlot ? (sameCat ? "  " : "~ ") : "! ";

		// "untagged" rather than Describe's "-" here: the pool column is read by pack
		// authors, and the floor pool is a thing they author, not a missing value.
		const std::string pool = std::string(a_descended ? "v " : "") +
			(a_poolTags ? Tags::Describe(a_poolTags) : "untagged");

		WriteLine(std::format("{} | {} | {} | {} | {} | {}{}/{}{} | {} | {} | id={}",
			Stamp(), SpeakerName(a_req.speaker),
			OrDash(a_req.slot), a_req.category,
			Tags::Describe(a_req.facts),
			mark, viaSlot, viaCategory, a_req.viaSfx ? " (sfx)" : "",
			a_file, pool, a_id));
	}

	void Miss(const Request& a_req, std::string_view a_reason)
	{
		std::scoped_lock lock{ g_lock };
		if (g_mode == Mode::Off || !Wants(a_req.speaker)) {
			return;
		}
		WriteLine(std::format("{} | {} | {} | {} | {} | MISS ({}) | - | - | -",
			Stamp(), SpeakerName(a_req.speaker),
			OrDash(a_req.slot), a_req.category,
			Tags::Describe(a_req.facts), a_reason));
	}

}
