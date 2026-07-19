#pragma once

namespace InstanceManager
{
	// hand a freshly played handle over; returns the public int32 id (>0)
	std::int32_t Register(RE::BSSoundHandle a_handle, float a_baseVolume, std::string a_group);

	bool  IsPlaying(std::int32_t a_id);
	bool  Stop(std::int32_t a_id);
	float DurationSec(std::int32_t a_id);
	void  SetInstanceVolume(std::int32_t a_id, float a_volume);

	void SetGroupVolume(const std::string& a_group, float a_volume);
	void DuckGroup(const std::string& a_group, float a_factor);
	void UnduckGroup(const std::string& a_group);
	void StopGroup(const std::string& a_group);
	void StopAll();

	// stop the channel's previous occupant and record a_id as the new one
	void PlayOnChannel(const std::string& a_channel, std::int32_t a_id);
	void StopChannel(const std::string& a_channel);

	// initial group volumes from config (called after Config::Load)
	void ApplyConfigGroupVolumes();

	// effective multiplier for a group at play time (groupVolume * duckFactor); 1.0 for ""
	float GroupMultiplier(const std::string& a_group);
}
