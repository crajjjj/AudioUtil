#include "TongueState.h"

// Deliberately a parallel of GagState: same worn-item walk, different marker set
// and no voice rerouting. Kept separate (rather than folded into GagState) so the
// two features resolve and log independently. See GagState.cpp for the notes on
// the TESBoundObject raw-lookup cast and the per-line (not per-frame) cost.
namespace TongueState
{
	namespace
	{
		std::mutex                              g_lock;
		std::vector<RE::BGSKeyword*>            g_keywords;  // guarded by g_lock
		std::unordered_set<RE::TESBoundObject*> g_items;     // guarded by g_lock
		bool                                    g_active = false;
	}

	void Resolve(const Config::Settings& a_settings)
	{
		std::vector<RE::BGSKeyword*>            resolvedKeywords;
		std::unordered_set<RE::TESBoundObject*> resolvedItems;
		if (a_settings.tongueEnabled) {
			if (auto* handler = RE::TESDataHandler::GetSingleton()) {
				for (const auto& kw : a_settings.tongueKeywords) {
					if (auto* form = handler->LookupForm<RE::BGSKeyword>(kw.localID, kw.plugin)) {
						resolvedKeywords.push_back(form);
					} else {
						logger::debug("[tongue] keyword {}|{:X} not in the load order - skipped", kw.plugin, kw.localID);
					}
				}
				for (const auto& it : a_settings.tongueItems) {
					// raw lookup + As<>, not LookupForm<TESBoundObject> — that gates on
					// T::FORMTYPE, which TESBoundObject lacks, so it would drop every
					// real item (see GagState for the full note).
					auto* raw = handler->LookupForm(it.localID, it.plugin);
					if (!raw) {
						logger::debug("[tongue] item {}|{:X} not in the load order - skipped", it.plugin, it.localID);
						continue;
					}
					if (auto* obj = raw->As<RE::TESBoundObject>()) {
						resolvedItems.insert(obj);
					} else {
						logger::warn("[tongue] item {}|{:X} is not a wearable object - skipped", it.plugin, it.localID);
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
			g_active = a_settings.tongueEnabled && (!g_keywords.empty() || !g_items.empty());
		}
		logger::info("Tongue detection: {} ({} keyword(s), {} item(s) resolved)",
			(a_settings.tongueEnabled ? ((kwCount + itemCount) > 0 ? "active" : "enabled but no markers resolved") : "disabled"),
			kwCount, itemCount);
	}

	bool IsWearingTongue(RE::Actor* a_actor)
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
			// a specific worn tongue-armor form marks the actor directly...
			if (g_items.contains(object)) {
				return true;
			}
			// ...as does any worn item carrying a configured tongue keyword.
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
