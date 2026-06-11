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

// Grant one queued received item per tick through the ER goods path (bypasses the detour).
VOID CGameHook::GiveNextItem() {
	if (!ItemRandomiser || ItemRandomiser->receivedItemsQueue.empty()) return;
	SReceivedItem item = ItemRandomiser->receivedItemsQueue.back();
	ItemRandomiser->receivedItemsQueue.pop_back();
	er_ap::game::GrantGoods(static_cast<int32_t>(item.address), static_cast<int32_t>(item.count));
	Core->writeSaveFileNextTick = true;
}

// ER goal detection (Elden Lord) is not wired yet; never auto-completes for the MVP.
BOOL CGameHook::isSoulOfCinderDefeated() {
	return FALSE;
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

// TODO(ER inventory removal): walk the player inventory and remove the placeholder. No-op for now.
VOID CGameHook::removeFromInventory(int32_t itemCategory, int32_t itemId, uint64_t quantity) {
	(void)itemCategory;
	(void)itemId;
	(void)quantity;
}

// Debug command: grant a goods item by base id.
VOID CGameHook::itemGib(int goodsId) {
	er_ap::game::GrantGoods(static_cast<int32_t>(goodsId), 1);
}
