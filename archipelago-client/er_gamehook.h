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
    uint64_t blob;
    std::memcpy(&blob, reinterpret_cast<const void*>((uintptr_t)hdr + PARAM_HEADER_DEREF), 8);
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

// Grant an item by its full gib id (id | category<<28). For our purposes idWithCategory is always
// (realGoodsId | CATEGORY_GOODS); gem = -1 for goods. Calls the real AddItemFunc (bypasses the detour).
void GrantItem(int32_t idWithCategory, int32_t quantity, int32_t gem = -1);

// Convenience for the goods-only path.
inline void GrantGoods(int32_t realGoodsId, int32_t quantity) {
    GrantItem(static_cast<int32_t>(static_cast<uint32_t>(realGoodsId) | CATEGORY_GOODS), quantity, -1);
}

// Set an in-game event flag (used to mark a location collected so the game won't re-offer it).
bool SetEventFlag(uint32_t eventFlagId, bool on);

}} // namespace er_ap::game
