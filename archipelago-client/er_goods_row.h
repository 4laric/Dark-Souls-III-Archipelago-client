// er_goods_row.h — byte offsets of the synthetic-carrier fields within an in-memory
// EquipParamGoods row (EQUIP_PARAM_GOODS_ST), for reading them out of CSRegulationManager's
// already-decrypted param table at runtime. This is the offset layer; er_item_decode.h is the
// transform layer; keeping them separate means a paramdef bump only touches this file.
//
// Offsets computed from ER Paramdex EquipParamGoods.xml (ParamType EQUIP_PARAM_GOODS_ST,
// FormatVersion 203, little-endian) and validated three ways:
//   1. row size recomputes to 176 (0xB0) — the canonical ER goods row stride (full-sequence checksum);
//   2. the six carrier-field ordinals match the vanilla Smithbox CSV exactly;
//   3. every name difference before the vagrant fields is type-preserving (a renamed 1-byte pad and
//      two u8:1 bitfield renames), so the byte layout through vagrant is identical to your dump.
// Belt-and-suspenders: regulation_io.read_param_field("EquipParamGoods", <synth id>, 0x54, "<i")
// against your own regulation reproduces vagrantItemLotId if you want a live confirmation.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "er_item_decode.h"

namespace er_ap {

constexpr size_t  EQG_ROW_SIZE                          = 176;    // 0xB0
constexpr size_t  EQG_OFF_basicPrice                    = 0x10;   // s32, ordinal 6   (16)
constexpr size_t  EQG_OFF_sellValue                     = 0x14;   // s32, ordinal 7   (20)
constexpr size_t  EQG_OFF_disableUseAtOutOfColiseum     = 0x4A;   // u8 bitfield byte, ordinal 54 (74)
constexpr uint8_t EQG_BIT_disableUseAtOutOfColiseum     = 0x20;   //   bit 5 of that byte
constexpr size_t  EQG_OFF_vagrantItemLotId              = 0x54;   // s32, ordinal 60  (84)
constexpr size_t  EQG_OFF_vagrantBonusEneDropItemLotId  = 0x58;   // s32, ordinal 61  (88)

// Alignment-safe little-endian load. The in-game row pointer carries no alignment guarantee for our
// access and the param blob is little-endian (matches the x64 host), so memcpy is the correct load.
inline int32_t ReadS32(const uint8_t* row, size_t off) {
    int32_t v;
    std::memcpy(&v, row + off, sizeof v);
    return v;
}

// Pull the synthetic-carrier fields out of a raw EquipParamGoods row.
inline GoodsRowFields ReadGoodsRow(const uint8_t* rowBase) {
    GoodsRowFields f;
    f.vagrantItemLotId             = ReadS32(rowBase, EQG_OFF_vagrantItemLotId);
    f.vagrantBonusEneDropItemLotId = ReadS32(rowBase, EQG_OFF_vagrantBonusEneDropItemLotId);
    f.basicPrice                   = ReadS32(rowBase, EQG_OFF_basicPrice);
    f.sellValue                    = ReadS32(rowBase, EQG_OFF_sellValue);
    f.disableUseAtOutOfColiseum =
        (rowBase[EQG_OFF_disableUseAtOutOfColiseum] & EQG_BIT_disableUseAtOutOfColiseum) != 0;
    return f;
}

// Convenience: raw row pointer -> decoded synthetic item.
inline SyntheticItem DecodeSyntheticRow(const uint8_t* rowBase) {
    return DecodeSynthetic(ReadGoodsRow(rowBase));
}

} // namespace er_ap
