#include "Core.h"
#include "ItemRandomiser.h"

extern CCore* Core;
extern CItemRandomiser* ItemRandomiser;

// Report a collected AP location to the outgoing queue. Called from the ER AddItemFunc detour
// (er_gamehook_win.cpp) on the game thread; ArchipelagoInterface drains checkedLocationsList and
// sends the checks to the server.
// TODO(threading): checkedLocationsList is shared with the AP thread without a lock, matching the
// original DS3 client. Revisit if we observe races once the detour is live.
void Archipelago_SendLocationCheck(int64_t apLocationId) {
	if (ItemRandomiser) ItemRandomiser->checkedLocationsList.push_front(apLocationId);
}

// Grant any queued received items. item.address carries the randomizer's FullID encoding
// (weapon/armor/ring/goods in the top nibble — identical to the game's gib categories), so it is
// granted as-is via GrantFullID. With items_handling 0b111 this is the SINGLE granting path:
// foreign items, own-world echoes, and starting inventory all arrive here.
VOID CItemRandomiser::grantReceivedItems() {
	while (!receivedItemsQueue.empty()) {
		SReceivedItem item = receivedItemsQueue.back();
		receivedItemsQueue.pop_back();
		er_ap::game::GrantFullID(static_cast<int32_t>(item.address), static_cast<int32_t>(item.count));
	}
}

// See header. No-op until the ER 2.6.2.0 inventory layout is resolved.
VOID CItemRandomiser::sendMissedItems() {
}
