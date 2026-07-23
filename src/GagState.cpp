#include "GagState.h"

namespace GagState
{
	namespace
	{
		std::mutex                   g_lock;
		std::vector<RE::BGSKeyword*> g_keywords;  // guarded by g_lock
		bool                         g_active = false;
	}

	void Resolve(const Config::Settings& a_settings)
	{
		std::vector<RE::BGSKeyword*> resolved;
		if (a_settings.gagEnabled) {
			if (auto* handler = RE::TESDataHandler::GetSingleton()) {
				for (const auto& kw : a_settings.gagKeywords) {
					if (auto* form = handler->LookupForm<RE::BGSKeyword>(kw.localID, kw.plugin)) {
						resolved.push_back(form);
					} else {
						// expected when the [gag] list names an optional mod the user
						// doesn't run - debug, not warn. The summary below reports the
						// resolved count (and flags the all-missing case).
						logger::debug("[gag] keyword {}|{:X} not in the load order - skipped", kw.plugin, kw.localID);
					}
				}
			}
		}

		const std::size_t count = resolved.size();
		{
			std::scoped_lock lock{ g_lock };
			g_keywords = std::move(resolved);
			g_active = a_settings.gagEnabled && !g_keywords.empty();
		}
		logger::info("Gag detection: {} ({} keyword(s) resolved)",
			(a_settings.gagEnabled ? (count > 0 ? "active" : "enabled but no keywords resolved") : "disabled"),
			count);
	}

	bool IsGagged(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::scoped_lock lock{ g_lock };
		if (!g_active) {
			return false;
		}
		auto* changes = a_actor->GetInventoryChanges();
		if (!changes || !changes->entryList) {
			return false;
		}
		for (auto* entry : *changes->entryList) {
			if (!entry || !entry->IsWorn()) {
				continue;
			}
			auto* object = entry->GetObject();
			if (!object) {
				continue;
			}
			if (auto* keyworded = object->As<RE::BGSKeywordForm>()) {
				for (auto* kw : g_keywords) {
					if (keyworded->HasKeyword(kw)) {
						return true;
					}
				}
			}
		}
		return false;
	}
}
