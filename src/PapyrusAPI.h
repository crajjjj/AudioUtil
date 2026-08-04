#pragma once

namespace PapyrusAPI
{
	bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

	// shared body of the PlayFile / PlayFileWithLipSync natives; also the backend
	// of the C exports in AudioUtilAPI.cpp (see include/API/AudioUtilAPI.h)
	std::int32_t PlayFileByPath(const char* a_dataRelPath, RE::Actor* a_follow,
		float a_volume, const char* a_group, const char* a_channel, bool a_lipSync);
}
