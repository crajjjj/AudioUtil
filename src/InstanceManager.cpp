#include "InstanceManager.h"

#include "Config.h"

namespace InstanceManager
{
	namespace
	{
		struct Instance
		{
			RE::BSSoundHandle handle;
			float             baseVolume;
			std::string       group;
		};

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
			a_instance.handle.SetVolume(std::clamp(a_instance.baseVolume * mult, 0.0f, 1.0f));
		}

		// caller holds g_lock — drop finished instances so the table stays small
		void Sweep()
		{
			std::erase_if(g_instances, [](auto& a_pair) {
				auto& handle = a_pair.second.handle;
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

	std::int32_t Register(RE::BSSoundHandle a_handle, float a_baseVolume, std::string a_group)
	{
		std::scoped_lock lock{ g_lock };
		Sweep();
		const auto id = g_nextId++;
		auto& instance = g_instances[id] = Instance{ a_handle, a_baseVolume, std::move(a_group) };
		ApplyEffectiveVolume(instance);
		return id;
	}

	bool IsPlaying(std::int32_t a_id)
	{
		std::scoped_lock lock{ g_lock };
		auto* instance = Find(a_id);
		return instance && instance->handle.IsValid() && instance->handle.IsPlaying();
	}

	bool Stop(std::int32_t a_id)
	{
		std::scoped_lock lock{ g_lock };
		auto* instance = Find(a_id);
		if (!instance) {
			return false;
		}
		const bool ok = instance->handle.IsValid() && instance->handle.Stop();
		g_instances.erase(a_id);
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
		std::scoped_lock lock{ g_lock };
		std::erase_if(g_instances, [&](auto& a_pair) {
			auto& instance = a_pair.second;
			if (instance.group != a_group) {
				return false;
			}
			if (instance.handle.IsValid()) {
				instance.handle.Stop();
			}
			return true;
		});
	}

	void StopAll()
	{
		std::scoped_lock lock{ g_lock };
		for (auto& [id, instance] : g_instances) {
			if (instance.handle.IsValid()) {
				instance.handle.Stop();
			}
		}
		g_instances.clear();
		g_channels.clear();
	}

	void PlayOnChannel(const std::string& a_channel, std::int32_t a_id)
	{
		std::int32_t previous = 0;
		{
			std::scoped_lock lock{ g_lock };
			const auto it = g_channels.find(a_channel);
			if (it != g_channels.end()) {
				previous = it->second;
			}
			g_channels[a_channel] = a_id;
		}
		if (previous > 0 && previous != a_id) {
			Stop(previous);
		}
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
			GetGroup(name).volume = std::clamp(volume, 0.0f, 1.0f);
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
