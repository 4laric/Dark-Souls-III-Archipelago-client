// er_gamehook.h — runtime glue for the ER Archipelago client.
//
// Split into two layers:
//   * PURE core (inline, below): the param-table walk, row lookup, decode, and pickup decision.
//     Operates on raw addresses via direct dereference (correct in-process) and is host-testable by
//     building a fake repo->cap->blob->row layout from real allocations (see walk_test.cpp).
//   * WINDOWS layer (declared here, defined in er_gamehook.cpp / er_item_handler.cpp): AOB scanning,
//     singleton/pointer resolution, the AddItemFunc grant call, event-flag set, and the detour.
//
// All binary facts come from er_hooks.h (build-pinned + CE-cross-validated).
#pragma once
#include <cstdint>
#include <cstring>
#include "er_item_decode.h"
#include "er_goods_row.h"
#include "er_hooks.h"

namespace er_ap { namespace game {

// ---------------------------------------------------------------------------------------------
// PURE CORE  (no platform dependencies; host-testable)
// ---------------------------------------------------------------------------------------------

// Resolve the in-memory EquipParamGoods PARAM blob from the param-repository instance.
// repo -> ParamResCap (hdr) -> PARAM blob. (Goods is a normal param: one +0x80 deref past hdr.)
inline const uint8_t* GoodsBlobFromRepo(uintptr_t paramRepoInstance) {
    using namespace hooks;
    if (!paramRepoInstance) return nullptr;
    uint64_t hdr;
    std::memcpy(&hdr, reinterpret_cast<const void*>(
        paramRepoInstance + (uintptr_t)PARAM_INDEX_GOODS * PARAM_ENTRY_STRIDE + PARAM_ENTRY_OFF), 8);
    if (!hdr) return nullptr;
    // GetParam in the exe derefs +0x80 TWICE to reach the PARAM blob (rowCount @ +0x0A):
    //   blob = *(*(hdr + 0x80) + 0x80).
    // The old single-deref read an intermediate struct as the param header (garbage rowCount).
    // Confirmed by disassembling eldenring.exe 2.6.2.0 (the generic GetParam routine).
    uint64_t mid;
    std::memcpy(&mid, reinterpret_cast<const void*>((uintptr_t)hdr + PARAM_HEADER_DEREF), 8);
    if (!mid) return nullptr;
    uint64_t blob;
    std::memcpy(&blob, reinterpret_cast<const void*>((uintptr_t)mid + PARAM_HEADER_DEREF), 8);
    return reinterpret_cast<const uint8_t*>((uintptr_t)blob);
}

// Find a row's data pointer by id within an in-memory PARAM blob (row index identical to on-disk).
inline const uint8_t* RowInBlob(const uint8_t* blob, int32_t targetId) {
    using namespace hooks;
    if (!blob) return nullptr;
    uint16_t rowCount;
    std::memcpy(&rowCount, blob + PARAM_ROWCOUNT_OFF, sizeof rowCount);
    for (uint16_t i = 0; i < rowCount; ++i) {
        const uint8_t* e = blob + PARAM_ROWIDX_OFF + (size_t)PARAM_ROWIDX_STRIDE * i;
        int32_t id;
        std::memcpy(&id, e + PARAM_ENTRY_ID_OFF, sizeof id);
        if (id == targetId) {
            int32_t dataOff;                                   // low 32 bits suffice (blob < 4 GiB)
            std::memcpy(&dataOff, e + PARAM_ENTRY_DOFF, sizeof dataOff);
            return blob + static_cast<uint32_t>(dataOff);
        }
    }
    return nullptr;
}

inline const uint8_t* GoodsRow(uintptr_t paramRepoInstance, int32_t goodsRowId) {
    return RowInBlob(GoodsBlobFromRepo(paramRepoInstance), goodsRowId);
}

// Given a raw "gib" item id from the grant/pickup path, decode it iff it is one of our synthetic
// goods placeholders. Returns false (and leaves `out` untouched) for any real or non-goods item.
inline bool DecodeSyntheticPickup(uintptr_t paramRepoInstance, uint32_t rawItemId, SyntheticItem& out) {
    if (!IsSyntheticGoods(rawItemId)) return false;
    const uint8_t* row = GoodsRow(paramRepoInstance, static_cast<int32_t>(RowIdOf(rawItemId)));
    if (!row) return false;
    out = DecodeSyntheticRow(row);
    return true;
}

// What the detour should do with a confirmed synthetic placeholder.
enum class PickupAction {
    Suppress,             // foreign / no-local: drop the placeholder, report the check only
    SuppressAndGrant,     // local: drop the placeholder, grant (localItemId, localQuantity)
};
inline PickupAction DecidePickup(const SyntheticItem& s) {
    if (!s.foreignRemove && s.localItemId != 0) return PickupAction::SuppressAndGrant;
    return PickupAction::Suppress;   // foreign check, or a check with no local item
}

// ---------------------------------------------------------------------------------------------
// WINDOWS layer  (defined in er_gamehook.cpp / er_item_handler.cpp; in-process, requires MinHook)
// ---------------------------------------------------------------------------------------------

// Resolve every signature/pointer in er_hooks.h against the loaded eldenring.exe and install the
// AddItemFunc detour. Returns false if any required signature failed to resolve.
bool Init();

// Resolved instances (read fresh each call; null until the game initializes them).
uintptr_t ParamRepoInstance();   // *(module + resolved ParamBase ptr-loc)
uintptr_t InventoryInstance();   // *(module + resolved inventory ptr-loc)

// Reads an in-game event flag (true = set). Used to detect AP checks whose acquisition path
// bypasses the AddItemFunc detour (shop purchases, NPC gifts, pickups made while disconnected):
// the randomizer emits each AP location's guarding flag into apconfig.json ("location_flags")
// and Core polls them. Safe before init / if signatures missed: returns false.
bool GetEventFlagState(uint32_t flagId);

// Reads the player's current PlayRegionId from FieldArea (region-lock physical enforcement) -- the
// open world's "which area am I in" signal (same one that drives the area-name banner). Returns -1
// if FieldArea is unresolved or not yet in-world. See er_hooks.h FieldArea_AOB / FIELDAREA_*.
int32_t GetPlayRegionId();

// Grant an item by its full gib id (id | category<<28). For our purposes idWithCategory is always
// (realGoodsId | CATEGORY_GOODS); gem = -1 for goods. Calls the real AddItemFunc (bypasses the detour).
void GrantItem(int32_t idWithCategory, int32_t quantity, int32_t gem = -1);

// Auto-upgrade (slot_data auto_upgrade; patch_client_autoupgrade.py). Rewrite an inbound weapon
// id to the player's current highest reinforce level on the SAME smithing track (cap from
// ReinforceParamWeapon: normal up to +25, somber up to +10), read live from inventory. No-op
// unless enabled and the weapon params resolve + calibrate.
void SetAutoUpgrade(int on);
bool AutoUpgradeWeaponId(int32_t inItemId, int32_t* outItemId);   // true => *outItemId rewritten
void RefreshAutoUpgradeTargets();                                  // recompute targets from live inventory

// Global Scadutree Blessing (slot_data global_scadutree_blessing; SPEC-global-scadutree-blessing.md).
// mode: 0=off, 1=player_only, 2=scaled. When != off, TickGlobalScaduBlessing() (called each in-world
// tick from Core) counts held Scadutree Fragments, converts to a blessing level via the vanilla cost
// curve, and RAISES the game's stored combat blessing byte (PlayerGameData +0xFC) so the DLC blessing
// buff applies in the base game. Never reduces the stored level. Combat track only (v1).
void SetGlobalScaduBlessing(int mode);
void TickGlobalScaduBlessing();


// Convenience for the goods-only path.
inline void GrantGoods(int32_t realGoodsId, int32_t quantity) {
    GrantItem(static_cast<int32_t>(static_cast<uint32_t>(realGoodsId) | CATEGORY_GOODS), quantity, -1);
}

// Grant by the randomizer's FullID encoding (top nibble: weapon=0, armor=1, ring/talisman=2,
// goods=4) — which is IDENTICAL to the game's gib category nibble, so the value passes straight
// through. This is what placeholder replaceWith payloads (basicPrice) and received-item local ids
// carry; granting them as goods (the old behavior) broke every non-goods item (e.g. armor
// "goods 2901" = Zamor Legwraps). KNOWN GAP: the C# FullID encoder folds GEM (Ashes of War) into
// the goods nibble, so gems still mis-grant — tracked on the randomizer side.
inline void GrantFullID(int32_t fullId, int32_t quantity) {
    GrantItem(fullId, quantity, -1);
}

// Set an in-game event flag (used to mark a location collected so the game won't re-offer it).
bool SetEventFlag(uint32_t eventFlagId, bool on);
// Warp to a grace/warp point (Hexinton CT). warpId = CT dropdown id, e.g. 68000=Gravesite Plain,
// 12052950=Cocoon of the Empyrean. Returns false if the warp fn/singleton AOBs were not resolved.
bool Warp(int32_t warpId);

// Remove a granted item from the player's bag (TODO #6 client half). `fullItemId` carries the
// category nibble exactly as granted (goods placeholder = realGoodsId | CATEGORY_GOODS). Walks the
// player's EquipInventoryData (chain + layout in er_hooks.h) and decrements the matching entry's
// quantity by `quantity` (clamped at 0). SAFE BY CONSTRUCTION: every read is page-validated
// (no crash on a stale/null chain) and a write happens ONLY on an exact itemId match, so a
// mis-resolved container degrades to a no-op rather than corrupting the bag. Returns true iff an
// entry was found and decremented. Idempotent: removing an absent item is a harmless no-op.
bool RemoveInventoryItem(int32_t fullItemId, int32_t quantity);

}} // namespace er_ap::game
