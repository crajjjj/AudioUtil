#include "GagState.h"

namespace GagState
{
	namespace
	{
		std::mutex                                  g_lock;
		std::vector<RE::BGSKeyword*>                g_keywords;  // guarded by g_lock
		std::unordered_set<RE::TESBoundObject*>     g_items;     // guarded by g_lock
		bool                                        g_active = false;
	}

	void Resolve(const Config::Settings& a_settings)
	{
		std::vector<RE::BGSKeyword*>            resolvedKeywords;
		std::unordered_set<RE::TESBoundObject*> resolvedItems;
		if (a_settings.gagEnabled) {
			if (auto* handler = RE::TESDataHandler::GetSingleton()) {
				for (const auto& kw : a_settings.gagKeywords) {
					if (auto* form = handler->LookupForm<RE::BGSKeyword>(kw.localID, kw.plugin)) {
						resolvedKeywords.push_back(form);
					} else {
						// expected when the [gag] list names an optional mod the user
						// doesn't run - debug, not warn. The summary below reports the
						// resolved count (and flags the all-missing case).
						logger::debug("[gag] keyword {}|{:X} not in the load order - skipped", kw.plugin, kw.localID);
					}
				}
				for (const auto& it : a_settings.gagItems) {
					// Resolve to the raw form, then cast to TESBoundObject so IsGagged
					// can compare object identity directly. NOTE: LookupForm<T> gates
					// on T::FORMTYPE, and TESBoundObject has none (it's a base over
					// ARMO/WEAP/...), so the templated overload would drop every real
					// item - use the raw lookup + As<> instead.
					auto* raw = handler->LookupForm(it.localID, it.plugin);
					if (!raw) {
						logger::debug("[gag] item {}|{:X} not in the load order - skipped", it.plugin, it.localID);
						continue;
					}
					if (auto* obj = raw->As<RE::TESBoundObject>()) {
						resolvedItems.insert(obj);
					} else {
						// misconfig: the form exists but can't be worn (e.g. a keyword or
						// spell id pasted into items by mistake). GetName() can be null
						// here, so identify by plugin|formid only - never format it.
						logger::warn("[gag] item {}|{:X} is not a wearable object - skipped", it.plugin, it.localID);
					}
				}
			}
		}

		const std::size_t kwCount = resolvedKeywords.size();
		const std::size_t itemCount = resolvedItems.size();
		{
			std::scoped_lock lock{ g_lock };
			g_keywords = std::move(resolvedKeywords);
			g_items = std::move(resolvedItems);
			g_active = a_settings.gagEnabled && (!g_keywords.empty() || !g_items.empty());
		}
		logger::info("Gag detection: {} ({} keyword(s), {} item(s) resolved)",
			(a_settings.gagEnabled ? ((kwCount + itemCount) > 0 ? "active" : "enabled but no markers resolved") : "disabled"),
			kwCount, itemCount);
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
			// a specific worn item form marks the actor gagged directly...
			if (g_items.contains(object)) {
				return true;
			}
			// ...as does any worn item carrying a configured gag keyword.
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
