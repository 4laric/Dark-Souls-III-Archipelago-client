#pragma once

#include "GameTypes.h"
#include "Params.h"

#include <cstdint>
#include <map>
#include <deque>
#include <list>
#include <string>

// A struct representing an item received from another world, queued for granting in-game.
struct SReceivedItem {
	// The Elden Ring goods id for this item.
	DWORD address;

	// The number of copies of this item that were received.
	DWORD count;

	// Notify v2 (SPEC-notify-item-source.md): the SOURCE shown at grant time. Resolved in the
	// receive handler (ArchipelagoInterface) and carried here because receive and grant are
	// time-decoupled (pacing + boss-defer) — the notification must travel with the item.
	std::string sender;     // get_player_alias(item.player); "Server" for starting inventory, "" if unresolved
	std::string itemName;   // get_item_name(...) — AP canonical name, used verbatim (already encodes qty)
	bool        ownItem;    // item.player == own slot (self-found): notification drops the "from"
};

// Goods-only ER item handler. Pickup detection, synthetic decode, local grant, and location-check
// reporting now happen in the AddItemFunc detour (er_gamehook_win.cpp); the DS3 ItemGib/OnGetItem
// hooks and the typed-param RandomiseItem path are retired. What remains here is the data the rest
// of the client shares: the outgoing checked-locations queue (filled by the detour via
// Archipelago_SendLocationCheck), the incoming received-items queue (filled by ArchipelagoInterface,
// granted through the ER goods path), and the AP id<->item maps.
class CItemRandomiser {
public:
	// Grant any queued items received from other worlds via the ER goods grant path.
	VOID grantReceivedItems();

	// Report foreign synthetic placeholders still in the player's inventory to AP, then remove them.
	// TODO(ER inventory walk): the DS3 version walked GameDataMan's equipInventoryData; the ER 2.6.2.0
	// inventory layout isn't resolved yet, so this is a no-op for the goods-only MVP (reconnect
	// missed-item recovery is disabled until then).
	VOID sendMissedItems();

	DWORD dIsAutoEquip = 0;
	// Keyed by AP item id (int64): AP ids can exceed 32 bits (location ids already do), and the old
	// DWORD key truncated -- std::stol even throws out_of_range past 2^31. Values are 32-bit ER item
	// ids / counts.
	std::map<int64_t, DWORD> pApItemsToItemIds = { };
	std::map<int64_t, DWORD> pItemCounts = { };
	std::deque<SReceivedItem> receivedItemsQueue = { };
	std::list<int64_t> checkedLocationsList = { };
};

// Report a collected AP location. Called from the ER AddItemFunc detour (er_gamehook_win.cpp) when a
// synthetic placeholder is picked up; pushes onto the global ItemRandomiser's checkedLocationsList,
// which ArchipelagoInterface drains and sends to the server.
void Archipelago_SendLocationCheck(int64_t apLocationId);
