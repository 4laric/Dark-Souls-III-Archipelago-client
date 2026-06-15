#include <spdlog/spdlog.h>

#include "GameHook.h"
#include "ItemRandomiser.h"

extern CCore* Core;
extern CItemRandomiser* ItemRandomiser;

// Install the ER AddItemFunc detour and resolve the build-pinned signatures/singletons.
BOOL CGameHook::initialize() {
	return er_ap::game::Init() ? TRUE : FALSE;
}

// Goods-only MVP: lock_equip / death_link / enable_dlc toggles aren't wired into ER behavior yet.
BOOL CGameHook::applySettings() {
	return TRUE;
}

// DS3 read player HP and the Soul-of-Cinder world flag here (for DeathLink + goal). Not wired for ER.
VOID CGameHook::updateRuntimeValues() {
}

// Map fragments are TWO mechanisms: the goods item (inventory) and a separate event flag
// that actually reveals the map region (vanilla pickup events set both; a raw gib only
// does the item). Table from the randomizer's MiscSetup mapFlags. DLC values are from
// community CE tables — VERIFY in-game (the log line prints the flag on every set).
static const std::unordered_map<int32_t, uint32_t> kMapUnlockFlags = {
	{8600, 62010}, {8601, 62011}, {8602, 62012},   // Limgrave W, Weeping, Limgrave E
	{8603, 62020}, {8604, 62021}, {8605, 62022},   // Liurnia E/N/W
	{8606, 62030}, {8607, 62031}, {8608, 62032},   // Altus, Leyndell, Gelmir
	{8609, 62040}, {8610, 62041},                  // Caelid, Dragonbarrow
	{8611, 62050}, {8612, 62051}, {8618, 62052},   // Mountaintops W/E, Snowfield
	{8613, 62060}, {8614, 62061}, {8616, 62062},   // Ainsel, Lake of Rot, Mohgwyn
	{8615, 62063}, {8617, 62064},                  // Siofra, Deeproot
	// DLC (UNVERIFIED -- check against the Hexinton CT):
	{2008600, 62080},  // Gravesite Plain
	{2008601, 62081},  // Scadu Altus
	{2008602, 62082},  // Southern Shore
	{2008603, 62083},  // Rauh Ruins
	{2008604, 62084},  // Abyss
};

// Grant one queued received item (item.address = randomizer FullID; category-aware grant —
// passing it through GrantGoods broke every non-goods item, e.g. armor as "goods 2901").
VOID CGameHook::GiveNextItem() {
	if (!ItemRandomiser || ItemRandomiser->receivedItemsQueue.empty()) return;
	SReceivedItem item = ItemRandomiser->receivedItemsQueue.back();
	ItemRandomiser->receivedItemsQueue.pop_back();
	// The apworld's region-lock keys are logic-only: sentinel er_code 99999, no real param
	// row. Granting that id would hand the game a nonexistent goods row; skip the grant
	// (the received index still advances, so it won't replay on reconnect).
	if ((item.address & 0x0FFFFFFF) == 99999) {
		spdlog::info("Received logic-only lock item (sentinel 99999); no in-game grant");
	} else {
		er_ap::game::GrantFullID(static_cast<int32_t>(item.address), static_cast<int32_t>(item.count));
		// Map fragments: also set the map-reveal event flag (see kMapUnlockFlags).
		if ((item.address & 0xF0000000) == 0x40000000) {
			auto mapIt = kMapUnlockFlags.find(static_cast<int32_t>(item.address & 0x0FFFFFFF));
			if (mapIt != kMapUnlockFlags.end()) {
				// Log the actual outcome — a stubbed/failed setter used to log "set" unconditionally.
				bool flagOk = er_ap::game::SetEventFlag(mapIt->second, true);
				spdlog::info("Map fragment {}: reveal flag {} {}", item.address & 0x0FFFFFFF,
				             mapIt->second, flagOk ? "SET" : "FAILED (setter unavailable)");
			}
		}

		// Notify v2 (SPEC-notify-item-source.md): surface the received item + its SOURCE at GRANT
		// time, so it respects the paced/boss-deferred drain and shows the right sender at the moment
		// the item lands. showBanner is the single sink — a console line now (it logs), the ER
		// bottom-center event banner once Task B stands it up. Own-world / server (starting-inventory)
		// grants drop the "from". The AP item name is used verbatim (already encodes any quantity).
		if (!item.itemName.empty()) {
			bool anon = item.ownItem || item.sender.empty() || item.sender == "Server";
			showBanner(anon ? item.itemName : item.itemName + " from " + item.sender);
		}
	}
	Core->writeSaveFileNextTick = true;
}

// Map reveal (beta.3): set every region's map-reveal flag (kMapUnlockFlags) without granting the
// map items — used for map_option=give via slot_data "reveal_all_maps" (CCore::Run drains it).
// DLC map flags (base id >= 2000000) are skipped unless includeDlc, matching the apworld's per-DLC
// map set. The flags share one holder, so a failed SetEventFlag means the holder isn't ready yet —
// return false so the caller retries next tick. Idempotent (flags persist once set).
bool CGameHook::revealAllMaps(bool includeDlc) {
	bool anySet = false;
	for (const auto& kv : kMapUnlockFlags) {
		if (!includeDlc && kv.first >= 2000000) continue;   // DLC map; skip when DLC is off
		if (!er_ap::game::SetEventFlag(kv.second, true)) {
			return false;                                   // holder not ready; retry next tick
		}
		anySet = true;
		spdlog::info("reveal_all_maps: map {} reveal flag {} SET", kv.first, kv.second);
	}
	return anySet;
}

// ER goal detection via boss DEFEAT FLAGS (from the randomizer's enemy config; arena-bound,
// so enemy randomization can't move them):
//   ending_condition 0 + DLC: Promised Consort Radahn (Enir-Ilim), flag 20012802
//   ending_condition 0 (no DLC) or 1: Elden Beast, flag 19000800
//   ending_condition 2/3 (all remembrances / all bosses): no single flag; not auto-detected.
// Mirrors the apworld's completion_condition (worlds/eldenring/__init__.py ~924).
BOOL CGameHook::isSoulOfCinderDefeated() {
	if (dEndingCondition >= 2) return FALSE;
	uint32_t flag = (dEndingCondition == 0 && dEnableDLC) ? 20012802u : 19000800u;
	return er_ap::game::GetEventFlagState(flag) ? TRUE : FALSE;
}

// DeathLink out of scope for the goods-only MVP.
VOID CGameHook::manageDeathLink() {
}

// Safe to act once the param repository instance is resolvable.
BOOL CGameHook::isEverythingLoaded() {
	return er_ap::game::ParamRepoInstance() != 0 ? TRUE : FALSE;
}

// Banner UI isn't wired to ER yet. Log the string form so messages aren't lost; wide form no-ops.
VOID CGameHook::showBanner(std::wstring message) {
	(void)message;
}
VOID CGameHook::showBanner(std::string message) {
	spdlog::info("[banner] {}", message);
}

// Set an in-game event flag via the resolved ER event-flag function.
VOID CGameHook::setEventFlag(DWORD eventId, BOOL enabled) {
	er_ap::game::SetEventFlag(static_cast<uint32_t>(eventId), enabled != FALSE);
}

// DS3 gesture; no ER equivalent.
VOID CGameHook::grantPathOfTheDragon() {
}

// Auto-equip is out of scope for the goods-only MVP.
VOID CGameHook::equipItem(EquipSlot equipSlot, DWORD inventorySlot) {
	(void)equipSlot;
	(void)inventorySlot;
}

// Pull a granted item back out of the bag (goods-placeholder cleanup, TODO #6 client half).
// Composes the categorized id the inventory stores, then delegates to the page-safe walker.
VOID CGameHook::removeFromInventory(int32_t itemCategory, int32_t itemId, uint64_t quantity) {
	uint32_t nibble = (itemCategory >= 0 && itemCategory < 0x10) ? static_cast<uint32_t>(itemCategory) : 0u;
	int32_t fullId = (static_cast<uint32_t>(itemId) & 0xF0000000u)
	                 ? itemId
	                 : static_cast<int32_t>((static_cast<uint32_t>(itemId) & 0x0FFFFFFFu) | (nibble << 28));
	int32_t qty = (quantity > 0x7FFFFFFFull) ? 0x7FFFFFFF : static_cast<int32_t>(quantity);
	bool ok = er_ap::game::RemoveInventoryItem(fullId, qty);
	spdlog::info("removeFromInventory(cat={}, id={:#x}, qty={}) -> {}",
	             itemCategory, static_cast<uint32_t>(itemId), quantity, ok ? "removed" : "no-op");
}

// Debug command: grant a goods item by base id.
VOID CGameHook::itemGib(int goodsId) {
	er_ap::game::GrantGoods(static_cast<int32_t>(goodsId), 1);
}
