#include "PPABridge.h"

#include "API/AccuratePenetrationAPI.h"
#include "Config.h"

namespace PPABridge
{
	namespace
	{
		namespace PPA = AccuratePenetration::API;

		struct ReceiverState
		{
			Snapshot      snapshot;
			std::uint64_t lastSentMs = 0;
			std::uint32_t lastSentContext = 0;
		};

		const PPA::InterfaceV1* g_api = nullptr;
		PPA::ListenerHandle     g_listener = 0;
		std::atomic<bool>       g_connected{ false };
		std::atomic<std::uint32_t> g_eventRateMs{ 500 };

		std::unordered_map<std::uint32_t, ReceiverState> g_receivers;  // native actor handle -> state
		std::mutex g_lock;

		std::uint64_t NowMs()
		{
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch())
					.count());
		}

		void QueueModEvent(RE::ActorHandle a_receiver, const char* a_eventName,
			float a_numArg, std::uint32_t a_context)
		{
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}
			const std::string eventName = a_eventName;
			const std::string strArg = std::to_string(a_context);
			task->AddTask([a_receiver, eventName, strArg, a_numArg]() {
				auto actor = a_receiver.get();
				if (!actor) {
					return;
				}
				auto* source = SKSE::GetModCallbackEventSource();
				if (!source) {
					return;
				}
				SKSE::ModCallbackEvent modEvent{
					RE::BSFixedString(eventName), RE::BSFixedString(strArg),
					a_numArg, actor.get()
				};
				source->SendEvent(&modEvent);
			});
		}

		void __cdecl OnAnimationUpdate(const PPA::AnimationUpdateEvent* a_event, void*)
		{
			if (!a_event || a_event->apiVersion != PPA::kVersion) {
				return;
			}

			// copy everything we need before returning — event data is transient
			Snapshot snapshot;
			snapshot.context = static_cast<std::uint32_t>(a_event->context);
			snapshot.anusOpening = a_event->anusOpening;
			snapshot.vaginalOpening = a_event->vaginalOpening;
			snapshot.ending = a_event->ending;

			float depth = 0.0f;
			if (a_event->selfInteraction) {
				depth = std::max(depth, a_event->selfInteraction->penetrationDepth);
			}
			if (a_event->actors) {
				for (std::uint32_t i = 0; i < a_event->actorCount; ++i) {
					depth = std::max(depth, a_event->actors[i].penetrationDepth);
				}
			}
			snapshot.depth = depth;

			const auto receiver = a_event->receiver;
			const auto key = receiver.native_handle();
			const auto now = NowMs();
			const auto rate = g_eventRateMs.load();

			bool sendUpdate = false;
			bool sendEnd = false;
			{
				std::scoped_lock lock{ g_lock };
				auto& state = g_receivers[key];
				state.snapshot = snapshot;

				if (snapshot.ending) {
					sendEnd = true;
				} else if (now - state.lastSentMs >= rate || state.lastSentContext != snapshot.context) {
					state.lastSentMs = now;
					state.lastSentContext = snapshot.context;
					sendUpdate = true;
				}
				if (snapshot.ending) {
					g_receivers.erase(key);
				}
			}

			if (sendEnd) {
				QueueModEvent(receiver, "AudioUtilPPA_End", snapshot.depth, snapshot.context);
			} else if (sendUpdate) {
				QueueModEvent(receiver, "AudioUtilPPA_Update", snapshot.depth, snapshot.context);
			}
		}
	}

	void TryConnect()
	{
		if (g_connected.load()) {
			return;
		}

		const auto settings = Config::Get();
		if (!settings->ppaEnabled) {
			logger::info("PPA bridge disabled by config");
			return;
		}
		g_eventRateMs.store(settings->ppaEventRateMs);

		const auto module = REX::W32::GetModuleHandleW(PPA::kPluginDLL);
		if (!module) {
			logger::info("AccuratePenetration.dll not loaded — PPA bridge disabled");
			return;
		}

		const auto getAPI = reinterpret_cast<PPA::GetAPIFn>(
			REX::W32::GetProcAddress(module, PPA::kGetAPIFunctionNameV1));
		if (!getAPI) {
			logger::warn("AccuratePenetration.dll found but {} export missing — PPA bridge disabled",
				PPA::kGetAPIFunctionNameV1);
			return;
		}

		g_api = getAPI();
		if (!g_api || g_api->version != PPA::kVersion || g_api->size < sizeof(PPA::InterfaceV1) ||
			!g_api->RegisterAnimationUpdateListener) {
			logger::warn("PPA API mismatch (version={}, size={}) — PPA bridge disabled",
				g_api ? g_api->version : 0, g_api ? g_api->size : 0);
			g_api = nullptr;
			return;
		}

		g_listener = g_api->RegisterAnimationUpdateListener(&OnAnimationUpdate, nullptr);
		if (g_listener == 0) {
			logger::warn("PPA listener registration failed — PPA bridge disabled");
			g_api = nullptr;
			return;
		}

		g_connected.store(true);
		logger::info("PPA bridge connected (API v{})", PPA::kVersion);
	}

	bool Connected()
	{
		return g_connected.load();
	}

	std::optional<Snapshot> GetFor(RE::Actor* a_receiver)
	{
		if (!a_receiver) {
			return std::nullopt;
		}
		const auto key = a_receiver->GetHandle().native_handle();
		std::scoped_lock lock{ g_lock };
		const auto it = g_receivers.find(key);
		if (it == g_receivers.end()) {
			return std::nullopt;
		}
		return it->second.snapshot;
	}

	void SetEventRateMs(std::uint32_t a_ms)
	{
		g_eventRateMs.store(std::max(50u, a_ms));
	}
}
