#include "InstanceManager.h"

#include "CaptionManager.h"
#include "Config.h"
#include "LipSync.h"

namespace InstanceManager
{
	namespace
	{
		struct Instance
		{
			RE::BSSoundHandle handle;
			float             baseVolume;
			std::string       group;
			std::string       path;  // data-relative file this instance played
			// distance-attenuation factor baked in at registration (1.0 = none), so a
			// far follow-positioned sound is quieter; kept as a separate multiplier so
			// group-volume / duck recomputes preserve it (see ApplyEffectiveVolume)
			float             distanceFactor = 1.0f;
			// stream startup is asynchronous: for a short window after Play() the
			// engine still reports "not playing", which made IsPlaying()/Sweep()
			// treat brand-new instances as finished (PlayAndWait returned instantly)
			std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		};

		constexpr auto STARTUP_GRACE = std::chrono::milliseconds(400);

		bool InStartupGrace(const Instance& a_instance)
		{
			return std::chrono::steady_clock::now() - a_instance.started < STARTUP_GRACE;
		}

		struct Group
		{
			float volume = 1.0f;
			float duckFactor = 1.0f;  // 1.0 = not ducked
		};

		std::unordered_map<std::int32_t, Instance> g_instances;
		std::unordered_map<std::string, Group>     g_groups;
		std::unordered_map<std::string, std::int32_t> g_channels;
		std::int32_t g_nextId = 1;
		std::mutex   g_lock;

		// caller holds g_lock
		Group& GetGroup(const std::string& a_group)
		{
			return g_groups[a_group];
		}

		// caller holds g_lock
		void ApplyEffectiveVolume(Instance& a_instance)
		{
			float mult = 1.0f;
			if (!a_instance.group.empty()) {
				const auto& group = GetGroup(a_instance.group);
				mult = group.volume * group.duckFactor;
			}
			a_instance.handle.SetVolume(
				std::clamp(a_instance.baseVolume * mult * a_instance.distanceFactor, 0.0f, 1.0f));
		}

		// Static distance-attenuation factor for a follow-positioned sound, computed
		// once at registration from the player->speaker distance. Full within
		// attenuationNear, quadratic falloff to attenuationFloor at attenuationFar.
		// 1.0 when disabled, no follow actor, or the player can't be resolved.
		float ComputeDistanceFactor(RE::Actor* a_follow)
		{
			const auto settings = Config::Get();
			if (!settings->voiceAttenuation || !a_follow) {
				return 1.0f;
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return 1.0f;
			}
			const auto pf = a_follow->GetPosition();
			const auto pp = player->GetPosition();
			const float dx = pf.x - pp.x;
			const float dy = pf.y - pp.y;
			const float dz = pf.z - pp.z;
			const float d = std::sqrt(dx * dx + dy * dy + dz * dz);

			const float nearD = settings->attenuationNear;
			const float farD = settings->attenuationFar;
			const float floor = std::clamp(settings->attenuationFloor, 0.0f, 1.0f);
			if (farD <= nearD || d <= nearD) {
				return 1.0f;
			}
			if (d >= farD) {
				return floor;
			}
			const float t = (d - nearD) / (farD - nearD);  // 0..1
			const float f = (1.0f - t) * (1.0f - t);        // quadratic - steeper than linear
			return floor + (1.0f - floor) * f;
		}

		// caller holds g_lock — drop finished instances so the table stays small
		void Sweep()
		{
			std::erase_if(g_instances, [](auto& a_pair) {
				auto& instance = a_pair.second;
				if (InStartupGrace(instance)) {
					return false;
				}
				auto& handle = instance.handle;
				return !handle.IsValid() || !handle.IsPlaying();
			});
		}

		// caller holds g_lock
		Instance* Find(std::int32_t a_id)
		{
			const auto it = g_instances.find(a_id);
			return it != g_instances.end() ? &it->second : nullptr;
		}
	}

	std::int32_t Register(RE::BSSoundHandle a_handle, float a_baseVolume, std::string a_group,
		std::string a_path, RE::Actor* a_follow)
	{
		const float distanceFactor = ComputeDistanceFactor(a_follow);
		std::scoped_lock lock{ g_lock };
		Sweep();
		const auto id = g_nextId++;
		auto& instance = g_instances[id] =
			Instance{ a_handle, a_baseVolume, std::move(a_group), std::move(a_path) };
		instance.distanceFactor = distanceFactor;
		ApplyEffectiveVolume(instance);
		return id;
	}

	bool IsPlaying(std::int32_t a_id)
	{
		std::scoped_lock lock{ g_lock };
		auto* instance = Find(a_id);
		if (!instance || !instance->handle.IsValid()) {
			return false;
		}
		return instance->handle.IsPlaying() || InStartupGrace(*instance);
	}

	bool Stop(std::int32_t a_id)
	{
		bool ok = false;
		bool found = false;
		{
			std::scoped_lock lock{ g_lock };
			if (auto* instance = Find(a_id)) {
				found = true;
				ok = instance->handle.IsValid() && instance->handle.Stop();
				g_instances.erase(a_id);
			}
		}
		if (found) {
			LipSync::OnInstanceStopped(a_id);
			CaptionManager::OnInstanceStopped(a_id);
		}
		return ok;
	}

	float DurationSec(std::int32_t a_id)
	{
		std::scoped_lock lock{ g_lock };
		auto* instance = Find(a_id);
		if (!instance || !instance->handle.IsValid()) {
			return 0.0f;
		}
		return static_cast<float>(instance->handle.GetDuration()) / 1000.0f;
	}

	void SetInstanceVolume(std::int32_t a_id, float a_volume)
	{
		std::scoped_lock lock{ g_lock };
		if (auto* instance = Find(a_id)) {
			instance->baseVolume = a_volume;
			ApplyEffectiveVolume(*instance);
		}
	}

	std::string InstancePath(std::int32_t a_id)
	{
		std::scoped_lock lock{ g_lock };
		auto* instance = Find(a_id);
		return instance ? instance->path : std::string{};
	}

	void SetGroupVolume(const std::string& a_group, float a_volume)
	{
		std::scoped_lock lock{ g_lock };
		GetGroup(a_group).volume = std::clamp(a_volume, 0.0f, 1.0f);
		for (auto& [id, instance] : g_instances) {
			if (instance.group == a_group) {
				ApplyEffectiveVolume(instance);
			}
		}
	}

	void DuckGroup(const std::string& a_group, float a_factor)
	{
		std::scoped_lock lock{ g_lock };
		GetGroup(a_group).duckFactor = std::clamp(a_factor, 0.0f, 1.0f);
		for (auto& [id, instance] : g_instances) {
			if (instance.group == a_group) {
				ApplyEffectiveVolume(instance);
			}
		}
	}

	void UnduckGroup(const std::string& a_group)
	{
		DuckGroup(a_group, 1.0f);
	}

	void StopGroup(const std::string& a_group)
	{
		std::vector<std::int32_t> stopped;
		{
			std::scoped_lock lock{ g_lock };
			std::erase_if(g_instances, [&](auto& a_pair) {
				auto& instance = a_pair.second;
				if (instance.group != a_group) {
					return false;
				}
				if (instance.handle.IsValid()) {
					instance.handle.Stop();
				}
				stopped.push_back(a_pair.first);
				return true;
			});
		}
		for (const auto id : stopped) {
			LipSync::OnInstanceStopped(id);
			CaptionManager::OnInstanceStopped(id);
		}
	}

	void StopAll()
	{
		std::vector<std::int32_t> stopped;
		{
			std::scoped_lock lock{ g_lock };
			for (auto& [id, instance] : g_instances) {
				if (instance.handle.IsValid()) {
					instance.handle.Stop();
				}
				stopped.push_back(id);
			}
			g_instances.clear();
			g_channels.clear();
		}
		for (const auto id : stopped) {
			LipSync::OnInstanceStopped(id);
			CaptionManager::OnInstanceStopped(id);
		}
	}

	bool PlayOnChannel(const std::string& a_channel, std::int32_t a_id, bool a_noInterrupt)
	{
		std::int32_t previous = 0;
		{
			std::scoped_lock lock{ g_lock };
			const auto it = g_channels.find(a_channel);
			if (it != g_channels.end()) {
				previous = it->second;
				// no-interrupt: if the channel's current sound is still playing,
				// keep it and reject the newcomer. Decided under the lock so two
				// concurrent claims can't both pass (the check+claim is atomic).
				if (a_noInterrupt && previous > 0 && previous != a_id) {
					const auto* inst = Find(previous);
					const bool prevPlaying = inst && inst->handle.IsValid() &&
						(inst->handle.IsPlaying() || InStartupGrace(*inst));
					if (prevPlaying) {
						return false;
					}
				}
			}
			g_channels[a_channel] = a_id;
		}
		if (previous > 0 && previous != a_id) {
			Stop(previous);
		}
		return true;
	}

	bool IsChannelBusy(const std::string& a_channel)
	{
		std::int32_t current = 0;
		{
			std::scoped_lock lock{ g_lock };
			const auto it = g_channels.find(a_channel);
			if (it == g_channels.end()) {
				return false;
			}
			current = it->second;
		}
		// IsPlaying takes g_lock itself, so query it after releasing above
		return current > 0 && IsPlaying(current);
	}

	void StopChannel(const std::string& a_channel)
	{
		std::int32_t current = 0;
		{
			std::scoped_lock lock{ g_lock };
			const auto it = g_channels.find(a_channel);
			if (it != g_channels.end()) {
				current = it->second;
				g_channels.erase(it);
			}
		}
		if (current > 0) {
			Stop(current);
		}
	}

	void ApplyConfigGroupVolumes()
	{
		const auto settings = Config::Get();
		std::scoped_lock lock{ g_lock };
		for (const auto& [name, volume] : settings->groupVolumes) {
			auto& group = GetGroup(name);
			group.volume = std::clamp(volume, 0.0f, 1.0f);
			// clear any stuck duck: a consumer's duck/unduck (e.g. lowering a
			// moan while a partner speaks) can be skipped if its scene ends or
			// is interrupted mid-duck, leaving the group silent. Reload then
			// doubles as a reliable "reset audio" - ReloadConfig calls this.
			group.duckFactor = 1.0f;
		}
	}

	float GroupMultiplier(const std::string& a_group)
	{
		if (a_group.empty()) {
			return 1.0f;
		}
		std::scoped_lock lock{ g_lock };
		const auto& group = GetGroup(a_group);
		return group.volume * group.duckFactor;
	}
}
