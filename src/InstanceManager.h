#pragma once

namespace InstanceManager
{
	// hand a freshly played handle over; returns the public int32 id (>0).
	// a_path is the data-relative file that was played (the shuffle-bag pick),
	// retained so a script can read back exactly which clip a handle played.
	// a_follow: the actor this sound is 3D-positioned at (or nullptr for flat/2D).
	// Used once at registration to bake a distance-attenuation factor into the
	// instance volume when [general] voice_attenuation is on (see Config).
	std::int32_t Register(RE::BSSoundHandle a_handle, float a_baseVolume, std::string a_group,
		std::string a_path = {}, RE::Actor* a_follow = nullptr);

	bool  IsPlaying(std::int32_t a_id);
	bool  Stop(std::int32_t a_id);
	float DurationSec(std::int32_t a_id);
	void  SetInstanceVolume(std::int32_t a_id, float a_volume);

	// data-relative path of the file this handle played ("" if the id is unknown)
	std::string InstancePath(std::int32_t a_id);

	void SetGroupVolume(const std::string& a_group, float a_volume);
	void DuckGroup(const std::string& a_group, float a_factor);
	void UnduckGroup(const std::string& a_group);
	void StopGroup(const std::string& a_group);
	void StopAll();

	// stop the channel's previous occupant and record a_id as the new one
	// claim a channel for an instance. With a_noInterrupt, returns false (without
	// claiming) if the channel's current sound is still playing - the caller
	// should drop a_id. Otherwise stops any previous occupant and returns true.
	bool PlayOnChannel(const std::string& a_channel, std::int32_t a_id, bool a_noInterrupt = false);
	bool IsChannelBusy(const std::string& a_channel);  // channel's current sound still playing?
	void StopChannel(const std::string& a_channel);

	// initial group volumes from config (called after Config::Load)
	void ApplyConfigGroupVolumes();

	// effective multiplier for a group at play time (groupVolume * duckFactor); 1.0 for ""
	float GroupMultiplier(const std::string& a_group);
}
