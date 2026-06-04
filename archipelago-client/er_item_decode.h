// er_item_decode.h — ER Archipelago runtime-client decode core (spec-3)
//
// Portable, Windows-free, unit-testable. Encodes the LOCKED decode contract (decisions A-F):
//   - Goods-only routing: every synthetic placeholder is an EquipParamGoods row; weapon/armor/
//     accessory synthetic detection is dead. Detection = goods category + row id > 3,780,000 (Decision D).
//   - Location id = vagrantItemLotId (low32) + vagrantBonusEneDropItemLotId (high32), each stored
//     SIGNED s32 in the ER paramdef -> the recombine MUST cast unsigned (see RecombineLocationId).
//   - Local replacement = basicPrice (real item id; 0 = no local item) + sellValue (qty).
//   - Foreign-remove = the disableUseAtOutOfColiseum bit (ER paramdef spelling, capital O-F).
//
// PENDING (not in this file): the exact byte offsets of these fields within the ER EquipParamGoods
// row come from the regenerated EquipParamGoodsRow struct, which needs the typed ER paramdef
// (regulation.bin). This file keeps the decode contract struct-layout-independent on purpose.
#pragma once
#include <cstdint>

namespace er_ap {

// Category flags occupy the top nibble of a "gib" item id (ER scheme, inherited from DS3).
constexpr uint32_t CATEGORY_WEAPON        = 0x00000000u;
constexpr uint32_t CATEGORY_PROTECTOR     = 0x10000000u;
constexpr uint32_t CATEGORY_ACCESSORY     = 0x20000000u;
constexpr uint32_t CATEGORY_GOODS         = 0x40000000u;
constexpr uint32_t CATEGORY_MASK          = 0xF0000000u;
constexpr uint32_t ROW_ID_MASK            = 0x0FFFFFFFu;

// Decision D, post goods-only: a synthetic placeholder is a goods row whose category-stripped id
// exceeds this. Bounds vanilla goods comfortably (max real vanilla goods id = 2,220,010).
constexpr uint32_t SYNTHETIC_GOODS_MIN_ID = 3780000u;

inline uint32_t ItemCategoryOf(uint32_t qItemId) { return qItemId & CATEGORY_MASK; }
inline uint32_t RowIdOf(uint32_t qItemId)        { return qItemId & ROW_ID_MASK; }

// True iff a picked-up gib id is one of our synthetic placeholders. Goods-only: a real item in any
// other category is never synthetic, regardless of how large its id is (e.g. the ~99M NPC weapons).
inline bool IsSyntheticGoods(uint32_t qItemId) {
    return ItemCategoryOf(qItemId) == CATEGORY_GOODS
        && RowIdOf(qItemId) > SYNTHETIC_GOODS_MIN_ID;
}

// Recombine the int64 AP location id from the two vagrant carrier fields.
//
// CRITICAL: vagrantItemLotId / vagrantBonusEneDropItemLotId are SIGNED s32 in the ER paramdef
// (vanilla stores -1, which an unsigned field can't hold). The (uint32_t) casts are load-bearing:
// the naive signed widen  ((int64)low | ((int64)high << 32))  sign-extends any half whose bit 31
// is set and clobbers the other half. ER's live ids are all in [7,000,000, 7,004,362] (bit-31
// clear, high word zero), so no live corruption today and the byte-diff is blind to this — the
// fix is validated only by the bit-31 vectors in vagrant_codec.py and tests.cpp.
inline int64_t RecombineLocationId(int32_t vagrantLow, int32_t vagrantHigh) {
    return  static_cast<int64_t>(static_cast<uint32_t>(vagrantLow))
        |  (static_cast<int64_t>(static_cast<uint32_t>(vagrantHigh)) << 32);
}

// Field values read off a synthetic EquipParamGoods row (offsets come from the regen'd struct).
struct GoodsRowFields {
    int32_t vagrantItemLotId;              // location id, low 32
    int32_t vagrantBonusEneDropItemLotId;  // location id, high 32
    int32_t basicPrice;                    // local real item id; 0 => no local item (foreign)
    int32_t sellValue;                     // local quantity
    bool    disableUseAtOutOfColiseum;     // foreign-remove flag
};

// Decoded synthetic placeholder.
struct SyntheticItem {
    int64_t apLocationId;
    int32_t localItemId;     // 0 => no local grant
    int32_t localQuantity;
    bool    foreignRemove;   // report the check, remove the placeholder, grant nothing locally
};

inline SyntheticItem DecodeSynthetic(const GoodsRowFields& f) {
    return SyntheticItem{
        RecombineLocationId(f.vagrantItemLotId, f.vagrantBonusEneDropItemLotId),
        f.basicPrice,
        f.sellValue,
        f.disableUseAtOutOfColiseum,
    };
}

} // namespace er_ap
