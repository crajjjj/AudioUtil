#include "LipCapture.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <thread>

namespace LipCapture
{
	namespace
	{
		constexpr auto TICK = std::chrono::milliseconds(33);  // ~30 Hz, the .lip frame rate
		constexpr std::size_t MAX_ROWS = 100000;              // ~55 min @ 30 Hz — runaway guard
		constexpr std::uint32_t VALUES_PER_BLOCK = 16;

		// every keyframe block in BSFaceGenAnimationData, in struct order. The
		// engine's dialogue lipsync writes one of the unk* blocks (a first capture
		// proved scripted phenomeKeyFrame stays zero during dialogue) — recording
		// all of them lets the CSV identify the real carrier empirically.
		constexpr std::uint32_t BLOCK_COUNT = 13;
		constexpr const char* BLOCK_NAMES[BLOCK_COUNT] = {
			"transition",  // transitionTargetKeyFrame (pointer, may be null)
			"expression", "unk040", "modifier", "phenome", "custom",
			"unk0C0", "unk0E0", "unk100", "unk120", "unk140", "unk160", "unk180"
		};

		struct Row
		{
			std::uint32_t ms;  // since capture start
			RE::FormID    id;
			RE::FormID    infoID;  // TESTopicInfo being spoken (fuz filenames embed it) — 0 if unknown
			std::uint8_t  counts[BLOCK_COUNT];  // real count per block (255 cap display)
			float         v[BLOCK_COUNT][VALUES_PER_BLOCK];
		};

		// Leaked on purpose — same detached-ticker teardown story as LipSync:
		// the thread may touch this state during static destruction at exit.
		std::vector<Row>& g_rows = *new std::vector<Row>();
		std::unordered_map<RE::FormID, std::string>& g_names =
			*new std::unordered_map<RE::FormID, std::string>();
		std::mutex&              g_lock = *new std::mutex();
		std::condition_variable& g_cv = *new std::condition_variable();

		std::atomic<bool> g_active{ false };
		std::atomic<bool> g_samplePending{ false };
		std::once_flag    g_tickerOnce;
		std::chrono::steady_clock::time_point g_startedAt;  // set on Start (game thread reads)
		bool g_warnedFull = false;

		void CopyBlock(Row& a_row, std::uint32_t a_slot, const RE::BSFaceGenKeyframeMultiple* a_kf)
		{
			if (!a_kf || !a_kf->values || a_kf->count == 0) {
				return;
			}
			a_row.counts[a_slot] = static_cast<std::uint8_t>(std::min<std::uint32_t>(a_kf->count, 255));
			const auto n = std::min(a_kf->count, VALUES_PER_BLOCK);
			for (std::uint32_t i = 0; i < n; ++i) {
				a_row.v[a_slot][i] = a_kf->values[i];
			}
		}

		// game thread (SKSE task): snapshot every facegen keyframe block of the
		// current dialogue speaker. The engine's own voice playback is what
		// writes these — that's the point.
		void SampleOnce()
		{
			auto* topicManager = RE::MenuTopicManager::GetSingleton();
			if (!topicManager) {
				return;
			}
			auto speakerPtr = topicManager->speaker.get();
			if (!speakerPtr) {
				speakerPtr = topicManager->lastSpeaker.get();  // still-talking tail
			}
			if (!speakerPtr) {
				return;
			}
			auto* actor = speakerPtr->As<RE::Actor>();
			if (!actor || !actor->Get3D()) {
				return;
			}
			auto* faceData = actor->GetFaceGenAnimationData();
			if (!faceData) {
				return;
			}

			Row row{};
			row.ms = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - g_startedAt).count());
			row.id = actor->GetFormID();
			// which line: the topic info being spoken names the voice file on disk
			// (…_<infoFormID>_<n>.fuz), making the capture→lip pairing exact
			if (const auto* info = topicManager->currentTopicInfo
					? topicManager->currentTopicInfo : topicManager->lastTopicInfo) {
				row.infoID = info->GetFormID();
			}
			{
				RE::BSSpinLockGuard guard{ faceData->lock };
				CopyBlock(row, 0, faceData->transitionTargetKeyFrame);
				CopyBlock(row, 1, &faceData->expressionKeyFrame);
				CopyBlock(row, 2, &faceData->unk040);
				CopyBlock(row, 3, &faceData->modifierKeyFrame);
				CopyBlock(row, 4, &faceData->phenomeKeyFrame);
				CopyBlock(row, 5, &faceData->customKeyFrame);
				CopyBlock(row, 6, &faceData->unk0C0);
				CopyBlock(row, 7, &faceData->unk0E0);
				CopyBlock(row, 8, &faceData->unk100);
				CopyBlock(row, 9, &faceData->unk120);
				CopyBlock(row, 10, &faceData->unk140);
				CopyBlock(row, 11, &faceData->unk160);
				CopyBlock(row, 12, &faceData->unk180);
			}

			std::scoped_lock lock{ g_lock };
			if (g_rows.size() >= MAX_ROWS) {
				if (!g_warnedFull) {
					g_warnedFull = true;
					logger::warn("LipCapture: row cap {} reached — stop and flush", MAX_ROWS);
				}
				return;
			}
			g_rows.push_back(row);
			if (!g_names.contains(row.id)) {
				const char* name = actor->GetDisplayFullName();
				g_names[row.id] = name ? name : "";
			}
		}

		void EnsureTicker()
		{
			std::call_once(g_tickerOnce, []() {
				std::thread([]() {
					for (;;) {
						{
							std::unique_lock lock{ g_lock };
							g_cv.wait(lock, []() { return g_active.load(); });
						}
						if (!g_samplePending.exchange(true)) {
							if (auto* task = SKSE::GetTaskInterface()) {
								task->AddTask([]() {
									if (g_active.load()) {
										SampleOnce();
									}
									g_samplePending.store(false);
								});
							} else {
								g_samplePending.store(false);
							}
						}
						std::this_thread::sleep_for(TICK);
					}
				}).detach();
			});
		}
	}

	bool Start()
	{
		if (g_active.exchange(true)) {
			return false;
		}
		{
			std::scoped_lock lock{ g_lock };
			g_rows.clear();
			g_rows.reserve(8192);
			g_names.clear();
			g_warnedFull = false;
		}
		g_startedAt = std::chrono::steady_clock::now();
		EnsureTicker();
		g_cv.notify_one();
		logger::info("LipCapture: started (13-block facegen probe)");
		return true;
	}

	std::string Stop()
	{
		if (!g_active.exchange(false)) {
			return {};
		}

		std::vector<Row> rows;
		std::unordered_map<RE::FormID, std::string> names;
		{
			std::scoped_lock lock{ g_lock };
			rows.swap(g_rows);
			names.swap(g_names);
		}
		if (rows.empty()) {
			logger::info("LipCapture: stopped, nothing captured (no dialogue speaker seen)");
			return {};
		}

		// timestamped file so repeated sessions never clobber each other
		const auto now = std::chrono::system_clock::now();
		const auto t = std::chrono::system_clock::to_time_t(now);
		std::tm tm{};
		localtime_s(&tm, &t);
		const auto relPath = std::format(
			"SKSE\\Plugins\\AudioUtil\\lipcap_{:04}{:02}{:02}_{:02}{:02}{:02}.csv",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
		const auto diskPath = std::filesystem::current_path() / "Data" / relPath;

		std::error_code ec;
		std::filesystem::create_directories(diskPath.parent_path(), ec);
		std::ofstream out(diskPath, std::ios::trunc);
		if (!out) {
			logger::warn("LipCapture: cannot write {}", diskPath.string());
			return {};
		}

		out << "# AudioUtil lip capture v3 — facegen keyframe blocks + topic info @ ~30 Hz\n";
		for (const auto& [id, name] : names) {
			out << std::format("# speaker {:08X} {}\n", id, name);
		}
		out << "t_ms,formid,info";
		for (std::uint32_t b = 0; b < BLOCK_COUNT; ++b) {
			out << ',' << BLOCK_NAMES[b] << "_n";
			for (std::uint32_t i = 0; i < VALUES_PER_BLOCK; ++i) {
				out << ',' << BLOCK_NAMES[b] << '_' << i;
			}
		}
		out << '\n';
		for (const auto& row : rows) {
			out << row.ms << ',' << std::format("{:08X}", row.id)
			    << ',' << std::format("{:08X}", row.infoID);
			for (std::uint32_t b = 0; b < BLOCK_COUNT; ++b) {
				out << ',' << static_cast<unsigned>(row.counts[b]);
				for (std::uint32_t i = 0; i < VALUES_PER_BLOCK; ++i) {
					out << ',' << std::format("{:.4f}", row.v[b][i]);
				}
			}
			out << '\n';
		}
		logger::info("LipCapture: stopped — {} rows, {} speaker(s) -> {}", rows.size(),
			names.size(), relPath);
		return relPath;
	}

	bool IsActive()
	{
		return g_active.load();
	}
}
