#include "FuzSlots.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <vector>

namespace FuzSlots
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr auto CACHE_DIR_REL = "Sound\\AudioUtilFuzCache";
		constexpr auto SLOT_PREFIX = "_au_slot";

		std::mutex        g_lock;
		std::vector<bool> g_busy;    // per-slot, true = in use
		int               g_count = 0;

		fs::path DataRoot() { return fs::current_path() / "Data"; }
		std::string SlotName(int a_i) { return std::format("{}{:02d}.wav", SLOT_PREFIX, a_i); }
		std::string SlotDataRel(int a_i) { return std::string(CACHE_DIR_REL) + "\\" + SlotName(a_i); }
		fs::path SlotDisk(int a_i) { return DataRoot() / CACHE_DIR_REL / SlotName(a_i); }

		// a valid but tiny 16-bit mono 44.1k silent wav — a placeholder must be a
		// parseable file at launch; its bytes are overwritten before every real play
		void WriteMinimalWav(const fs::path& a_path)
		{
			constexpr std::uint32_t rate = 44100;
			constexpr std::uint16_t channels = 1;
			constexpr std::uint16_t bits = 16;
			constexpr std::uint32_t dataBytes = 2000;
			constexpr std::uint32_t byteRate = rate * channels * bits / 8;
			constexpr std::uint16_t blockAlign = channels * bits / 8;

			std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
			if (!out) {
				return;
			}
			const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
			const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
			out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
			out.write("fmt ", 4); u32(16); u16(1); u16(channels); u32(rate); u32(byteRate); u16(blockAlign); u16(bits);
			out.write("data", 4); u32(dataBytes);
			const std::vector<char> zeros(dataBytes, 0);
			out.write(zeros.data(), dataBytes);
		}
	}

	void Configure(int a_count)
	{
		std::scoped_lock lock{ g_lock };
		g_count = a_count > 0 ? a_count : 0;
		g_busy.assign(g_count, false);
		if (!g_count) {
			logger::info("FuzSlots: disabled (fuz_slots = 0) — a first-session fuz stays silent until a restart");
			return;
		}
		std::error_code ec;
		fs::create_directories(DataRoot() / CACHE_DIR_REL, ec);
		int created = 0;
		for (int i = 0; i < g_count; ++i) {
			const auto disk = SlotDisk(i);
			if (!fs::exists(disk, ec) || fs::file_size(disk, ec) < 44) {
				WriteMinimalWav(disk);
				++created;
			}
		}
		logger::info("FuzSlots: pool of {} ready ({} created this launch). Newly created slots "
					 "become usable after the next restart; pre-existing ones work now.",
			g_count, created);
	}

	bool Enabled()
	{
		std::scoped_lock lock{ g_lock };
		return g_count > 0;
	}

	bool IsSlotName(std::string_view a_nameOrPath)
	{
		const auto slash = a_nameOrPath.find_last_of("\\/");
		const auto name = slash == std::string_view::npos ? a_nameOrPath : a_nameOrPath.substr(slash + 1);
		return name.starts_with(SLOT_PREFIX);
	}

	int AcquireWithCopy(const std::string& a_srcCacheDataRelPath, std::string& a_outSlotDataRelPath)
	{
		int idx = -1;
		{
			std::scoped_lock lock{ g_lock };
			for (int i = 0; i < g_count; ++i) {
				if (!g_busy[i]) {
					g_busy[i] = true;
					idx = i;
					break;
				}
			}
		}
		if (idx < 0) {
			return -1;  // pool full (or disabled)
		}

		// Plain file IO, deliberately NOT the engine resource loader: USVFS serves a
		// mid-session-decoded cache wav that the frozen resource index can't, and
		// takes our slot write too. The slot path IS in the launch index, so the
		// subsequent BuildSoundDataFromFile reads these fresh bytes. Streamed in
		// chunks — no whole-file buffer per play. (Every fuz play routes through a
		// slot, even one whose cache wav is already launch-indexed — the pool is
		// sized for concurrency (fuz_slots), and skipping "already indexed" files
		// would need session-state FuzCache doesn't track; on a full pool the
		// direct-path fallback in AudioEngine covers exactly those files.)
		std::ifstream in(DataRoot() / a_srcCacheDataRelPath, std::ios::binary);
		std::ofstream out(SlotDisk(idx), std::ios::binary | std::ios::trunc);
		if (!in || !out) {
			Release(idx);
			return -1;
		}
		char           buf[64 * 1024];
		std::uintmax_t copied = 0;
		while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
			out.write(buf, in.gcount());
			copied += static_cast<std::uintmax_t>(in.gcount());
		}
		out.close();
		if (!out || copied < 44) {  // smaller than a wav header = not a playable wav
			Release(idx);
			return -1;
		}

		a_outSlotDataRelPath = SlotDataRel(idx);
		return idx;
	}

	void Release(int a_index)
	{
		std::scoped_lock lock{ g_lock };
		if (a_index >= 0 && a_index < g_count) {
			g_busy[a_index] = false;
		}
	}
}
