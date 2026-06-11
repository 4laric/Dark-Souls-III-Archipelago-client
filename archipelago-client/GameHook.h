#pragma once

#include <string>

#include "Core.h"
#include "GameTypes.h"
#include "Params.h"   // pulls er_ap::game:: (Init, GrantGoods, SetEventFlag, ParamRepoInstance)

// Goods-only ER game hook. This is a slim shim over er_ap::game:: that preserves the CGameHook
// interface the rest of the client calls. The DS3 implementation (function-pattern scanning,
// ItemGib/OnGetItem/world-loaded/serialize hooks, HP/Soul-of-Cinder reads, equip + inventory
// manipulation) is retired: ER pickup detection and item granting go through the AddItemFunc detour
// (er_gamehook_win.cpp), installed by er_ap::game::Init(). DS3-only features (DeathLink, auto-equip,
// banners, DLC checks, Path of the Dragon) are stubbed for the MVP — see the .cpp for TODOs.
class CGameHook {
public:
	// Install the ER AddItemFunc detour and resolve singletons. False if any signature fails.
	BOOL initialize();

	// Apply connection options. No-op for the MVP (option toggles not yet wired into ER behavior).
	BOOL applySettings();

	// Per-tick runtime read (HP, goal flag). No-op for the MVP.
	VOID updateRuntimeValues();

	// Grant one queued received item via the ER goods path.
	VOID GiveNextItem();

	// ER goal detection (Elden Lord) not wired yet; always false for the MVP.
	BOOL isSoulOfCinderDefeated();

	// DeathLink out of scope for the MVP.
	VOID manageDeathLink();

	// True once it's safe to act (param repository resolvable).
	BOOL isEverythingLoaded();

	// In-game banner. Not wired to ER UI yet (string form logs; wide form is a no-op).
	VOID showBanner(std::wstring message);
	VOID showBanner(std::string message);

	// Set an in-game event flag via the ER event-flag function.
	VOID setEventFlag(DWORD eventId, BOOL enabled);

	// DS3 gesture; no ER equivalent (no-op).
	VOID grantPathOfTheDragon();

	// Auto-equip out of scope for the MVP (no-op).
	VOID equipItem(EquipSlot equipSlot, DWORD inventorySlot);

	// TODO(ER inventory removal): remove an item from the player's inventory. No-op for now.
	VOID removeFromInventory(int32_t itemCategory, int32_t itemId, uint64_t quantity = 1);

	// Debug command (/itemGib): grant a goods item by base id.
	VOID itemGib(int goodsId);

	int healthPoint = -1, lastHealthPoint = -1;
	char soulOfCinderDefeated = 0;

	DWORD dLockEquipSlots = 0;
	DWORD dIsNoWeaponRequirements = 0;
	DWORD dIsNoSpellsRequirements = 0;
	DWORD dIsNoEquipLoadRequirements = 0;
	DWORD dIsDeathLink = 0;
	DWORD dEnableDLC = 0;
	HANDLE hHeap = nullptr;

	BOOL deathLinkData = false;
};
