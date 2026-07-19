#pragma once

namespace Config
{
	struct Slot
	{
		std::string id;    // "F1", "M3"
		std::string root;  // data-relative folder, e.g. "Sound\\fx\\IVDT\\F1"
		char        sex;   // 'F' or 'M'
	};

	using StringMap = std::unordered_map<std::string, std::string>;

	struct Settings
	{
		std::vector<Slot> slots;

		std::string defaultFemaleSlot{ "F1" };
		std::string defaultMaleSlot{ "M1" };

		// voicetype resolution (keys normalized)
		bool      voicetypeRemapEnabled{ true };
		StringMap voicetypeRemap;  // "maleguard" -> "malenord" (values are voicetype names)
		StringMap voicetypeMap;    // "malenord"  -> "M4"       (values are slot ids)
		StringMap npcOverrides;    // "plugin.esp|formid-lowercase-hex" -> slot id

		// category layer (keys normalized; values are raw folder/category names)
		StringMap femaleAliases;
		StringMap maleAliases;
		StringMap maleOnlyRemap;  // female-engine category -> male category
		StringMap femaleFallbacks;
		StringMap maleFallbacks;

		StringMap sfxTable;  // normalized name -> data-relative folder

		std::unordered_map<std::string, float> groupVolumes;

		std::string voiceRoot{ "Sound\\fx\\IVDT" };
		std::string sfxRoot{ "Sound\\fx\\HentairimSFX" };

		std::uint32_t soundFlags{ 0x1A };
		std::uint32_t soundPriority{ 128 };

		bool          ppaEnabled{ true };
		std::uint32_t ppaEventRateMs{ 500 };
	};

	// lowercase + strip non-alphanumerics: "About To Cum" == "AboutToCum" == "abouttocum"
	std::string Normalize(std::string_view a_text);

	// canonical npc-override lookup key (same form as [npc_overrides] keys after parsing)
	std::string MakeNpcKey(std::string_view a_plugin, std::uint32_t a_localID);

	// parse Data\SKSE\Plugins\HentairimAudio\HentairimAudio.toml.
	// On error the previous (or default) settings are kept and the error is logged.
	bool Load();

	std::shared_ptr<const Settings> Get();

	const Slot* FindSlot(const Settings& a_settings, std::string_view a_id);
}
