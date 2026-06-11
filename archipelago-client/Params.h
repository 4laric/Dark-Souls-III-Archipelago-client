// Params.h — Elden Ring goods-only param access for the Archipelago runtime client.
//
// This replaces the DS3 client's param layer. The DS3 version (kept in git history) defined typed
// EquipParam{Goods,Weapon,Protector,Accessory}Row structs, an FD4Singleton-based CSRegulationManager
// walk (ParamTable / ParamResCap / FindRow), and index-based GetXParam() accessors. None of that
// ports to Elden Ring:
//   * ER is goods-only (Decision D) — weapon/armor/accessory synthetic detection is dead, so those
//     three row structs and their accessors are gone.
//   * The runtime row read no longer goes through CSRegulationManager + a typed struct. It uses the
//     build-pinned, CE-cross-validated param walk in er_gamehook.h (repo -> ParamResCap -> blob ->
//     row), with the synthetic-carrier field offsets in er_goods_row.h and the decode contract in
//     er_item_decode.h. The fd4_singleton subproject is superseded by er_singletons.h.
//
// Net effect: callers no longer dereference a typed param row. They get a decoded er_ap::SyntheticItem
// (apLocationId / localItemId / localQuantity / foreignRemove) via the helpers below, or call into
// er_ap::game:: directly.
#pragma once

#include "er_item_decode.h"   // er_ap::SyntheticItem, GoodsRowFields, IsSyntheticGoods, decode contract
#include "er_goods_row.h"     // er_ap::ReadGoodsRow / DecodeSyntheticRow (ER field offsets, row size 176)
#include "er_gamehook.h"      // er_ap::game:: param walk, ParamRepoInstance(), GrantGoods(), Init()

// Decode the synthetic goods row for a base goods id (category flag already stripped) against the
// live param repository. Returns false if the row can't be resolved (repo not yet initialized, or
// no such row), leaving `out` untouched.
inline bool GetSyntheticGoods(int32_t goodsRowId, er_ap::SyntheticItem& out) {
    const uint8_t* row = er_ap::game::GoodsRow(er_ap::game::ParamRepoInstance(), goodsRowId);
    if (!row) return false;
    out = er_ap::DecodeSyntheticRow(row);
    return true;
}
