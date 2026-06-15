// er_hooks.h — Elden Ring runtime binding map for the Archipelago client (spec-3).
// Build-pinned to eldenring.exe 2.6.2.0
// (sha256 34102b1c08bb5f769a724427a6f70fe29b3b732c31cf73693f861c48d3492ddb). ImageBase 0x140000000.
//
// Every AOB below matches this exe UNIQUELY and was cross-validated against the community Hexinton
// all-in-one CE table and against static analysis of this exe. Prefer runtime AOB resolution (it
// survives patch shifts); the *_RVA constants are the resolved values for this specific build.
//
// The client's four jobs and what binds each:
//   DETECT a synthetic-placeholder pickup  -> hook AddItemFunc (item lots route through it)
//   DECODE the placeholder payload         -> read its EquipParamGoods row (ParamBase walk + er_goods_row.h)
//   GRANT a local item / RECEIVE a foreign -> call AddItemFunc with an item descriptor
//   REPORT the AP check                    -> CSEventFlagMan (er_singletons.h) + the flag set/get fn
#pragma once
#include <cstdint>

namespace er_ap { namespace hooks {

// ---- item grant / pickup linchpin -----------------------------------------------------------
// AddItemFunc(rcx = inventory instance, rdx = &itembuf + ITEMBUF_ENTRY_OFF, r8 = &itembuf, r9 = 0).
// The same routine grants items from lots, so hooking its entry yields pickup DETECTION and calling
// it performs the GRANT. The item id carries its category in the top nibble (goods = 0x40000000).
constexpr uint32_t  AddItemFunc_RVA = 0x005605B0;
constexpr const char* AddItemFunc_AOB =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00 "
    "48 C7 45 C8 FE FF FF FF 48 89 9C 24 D8 01 00 00 48 8B 05";

// item descriptor ("itembuffer") layout consumed by AddItemFunc:
constexpr int ITEMBUF_ENTRY_OFF = 0x20;  // rdx points here (start of the {id,qty,...} entry)
constexpr int ITEMBUF_ID_OFF    = 0x24;  // s32: itemId | (categoryNibble << 28)
constexpr int ITEMBUF_QTY_OFF   = 0x28;  // s32: quantity
constexpr int ITEMBUF_GEM_OFF   = 0x30;  // s32: gem / ash-of-war id, -1 = none
// category nibble (<<28): weapon=0, armor=1, accessory/talisman=2, goods=4, gem=8.
// (goods << 28 == 0x40000000 == er_ap::CATEGORY_GOODS — the client only ever emits goods.)

// ---- inventory instance (the AddItemFunc rcx argument) --------------------------------------
// AOB ends in `mov rcx,[rip+disp32]`. Resolve at runtime:
//   uint32_t ptrLoc = match_rva + 0x1D + i32(match_rva + 0x19);
//   void* inventory = *(void**)(module_base + ptrLoc);
constexpr uint32_t  InventoryAccessor_RVA = 0x005AB620;
constexpr uint32_t  Inventory_PtrLoc_RVA  = 0x03D67A50;   // resolved value for 2.6.2.0
constexpr const char* InventoryAccessor_AOB =
    "44 8B 61 1C 41 8B FC C1 EF 07 40 80 E7 01 41 C1 EC 08 41 80 E4 01 48 8B 0D";

// ---- param read path: synthetic id -> EquipParamGoods row pointer ---------------------------
//   void* paramRepo = *(void**)(module_base + ParamBase_PtrLoc_RVA);
//   uint64 hdr      = *(uint64*)((uint8*)paramRepo + index*PARAM_ENTRY_STRIDE + PARAM_ENTRY_OFF);
//   uint64 blob     = *(uint64*)(hdr + PARAM_HEADER_DEREF);   // normal params incl. goods: one deref
//                                                             // (bullet/magic add one more +0x80)
// then the in-memory PARAM row lookup on blob (identical to the on-disk row index):
//   uint16 rowCount = *(uint16*)(blob + PARAM_ROWCOUNT_OFF);
//   entry[i]        =  blob + PARAM_ROWIDX_OFF + PARAM_ROWIDX_STRIDE*i;
//   for the entry whose *(s32*)(entry + PARAM_ENTRY_ID_OFF) == targetId:
//       uint8* rowBase = (uint8*)blob + *(uint32*)(entry + PARAM_ENTRY_DOFF);
// then apply er_goods_row.h offsets to rowBase. (Goods row stride == 176, re-confirmed by the
// table's own paramlength = dataOffset(row1) - dataOffset(row0).)
constexpr uint32_t  ParamBase_PtrLoc_RVA = 0x03D81EE8;     // resolved value for 2.6.2.0
constexpr const char* ParamBase_AOB =
    "48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 45 33 C0 BA 8E 00 00 00";  // mov rcx,[rip+ParamBase]
constexpr int PARAM_ENTRY_STRIDE  = 0x48;   // 72
constexpr int PARAM_ENTRY_OFF     = 0x88;
constexpr int PARAM_HEADER_DEREF  = 0x80;
constexpr int PARAM_INDEX_GOODS   = 3;      // EquipParamGoods. CONFIRMED LIVE: with the correct two-
                                            // deref blob walk, the in-game scan found goods (rowCount
                                            // 3571, firstRowId 0) at repo index 3. The original value
                                            // was right; the only bug was the missing 2nd +0x80 deref
                                            // (er_gamehook.h). (The repo is NOT BND-ordered; "44" was
                                            // a wrong detour.)
constexpr int PARAM_ROWCOUNT_OFF  = 0x0A;   // u16
constexpr int PARAM_ROWIDX_OFF    = 0x40;   // first 24-byte index entry
constexpr int PARAM_ROWIDX_STRIDE = 24;
constexpr int PARAM_ENTRY_ID_OFF  = 0x00;   // s32
constexpr int PARAM_ENTRY_DOFF    = 0x08;   // dataOffset (low 32 bits used)

// ---- event flags (AP location reporting) ----------------------------------------------------
// CSEventFlagMan instance: resolve via er_singletons.h (slot RVA 0x03D68458). Set/get function:
constexpr uint32_t  EventFlag_SetGet_RVA = 0x005D2110;
constexpr const char* EventFlag_SetGet_AOB =
    "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 48 8B DA 41 0F B6 F8 8B 12 48 8B F1 85 D2 0F 84";

// ---- event flag READ (check detection for acquisitions that bypass AddItemFunc) -------------
// GetEventFlag(rcx = flag holder, edx = flag id) -> eax nonzero if set. Derived by disassembling
// EventFlag_SetGet above on this exe: it calls this fn (0x5F9400) with its own rcx to read the
// current state before writing. Divisor at holder+0x1C, group tree at +0x38 — the classic layout.
// AOB verified UNIQUE in .text of 2.6.2.0.
constexpr uint32_t  GetEventFlagFunc_RVA = 0x005F9400;
constexpr const char* GetEventFlagFunc_AOB =
    "44 8B 41 1C 44 8B DA 33 D2 41 8B C3 41 F7 F0 4C 8B D1 45 33 C9 44 0F AF C0 45 2B D8 4C 8B 41 38";

// The flag-holder global the game itself passes as rcx (from the Hexinton CE table's EventFlagMan
// symbol; NOT the same global as the er_singletons CSEventFlagMan slot — they differ by 0x10 on
// this build). `mov rdi,[rip+disp]`: ptrLoc = match + 7 + i32(match + 3). Resolves to 0x03D68448.
constexpr uint32_t  EventFlagMan_PtrLoc_RVA = 0x03D68448;
constexpr const char* EventFlagMan_AOB =
    "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9";

// ---- inventory removal (synthetic-placeholder cleanup, TODO #6 client half) -----------------
// Goal: pull a synthetic / own-world goods placeholder back OUT of the player's bag. The GIVE
// path (AddItemFunc, RVA 0x005605B0) is NOT a usable inverse: it is the full CSMapItemMan
// acquisition pipeline (~335 instrs, 35 calls) and FIRES THE VANILLA "item acquired" popup, so a
// negative-quantity call would both be unverified AND spam the very popup Task #11 fights. Removal
// instead edits the player's EquipInventoryData directly: no popup, no notification.
//
// Pointer chain (resolved against THIS exe, sha256 3410...3492ddb):
//   GameDataMan instance : *(module + <global resolved from GameDataMan_AOB>)      (global RVA 0x03D5DF38)
//   PlayerGameData       : *(gameDataMan + GAMEDATAMAN_PGD_OFF)                     (== 0x08)
//   EquipInventoryData   : an EMBEDDED sub-object of PlayerGameData. Its PlayerGameData-relative
//                          offset is reached at runtime via a virtual accessor and is NOT pinned
//                          statically here; the client AUTO-DISCOVERS it by shape + a target-id
//                          match (see RemoveInventoryItem in er_gamehook_win.cpp). Once a build is
//                          confirmed in-game, hard-pin it and drop the scan.
//
// EquipInventoryData layout — VERIFIED by disassembling the game's OWN inventory accessor
// (eldenring.exe 0x0024C4E0..0x0024C61A, the routine Hexinton's "Inventory Editor" hooks):
//   container +0x1C  u32  slotCount         (# populated slots in the primary array; scan bound)
//   container +0x50  ptr  primaryItems[]    (normal items; index 0..slotCount-1)   stride 0x18
//   container +0x40  ptr  overflowItems[]   (index >= slotCount: key/quest spill)  stride 0x18
//   each entry +0x00 s32  itemId            (game matches on this: `cmp [entry],key`)
//   each entry +0x0C s32  quantity          (game's get-count returns `mov eax,[entry+0x0C]`)
// itemId carries the same top-nibble category as a grant (goods = 0x40000000), so a goods
// placeholder is matched as (goodsId | CATEGORY_GOODS).
constexpr const char* GameDataMan_AOB =
    "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3";  // mov rax,[rip+GameDataMan]; ...
constexpr int  GameDataMan_DISP_OFF   = 3;     // disp32 offset within the 7-byte (48 8B 05 ..) load
constexpr int  GAMEDATAMAN_PGD_OFF    = 0x08;  // PlayerGameData = *(GameDataMan + 0x08)
constexpr int  INV_SLOTCOUNT_OFF      = 0x1C;
constexpr int  INV_PRIMARY_PTR_OFF    = 0x50;
constexpr int  INV_OVERFLOW_PTR_OFF   = 0x40;
constexpr int  INV_ENTRY_STRIDE       = 0x18;
constexpr int  INV_ENTRY_ID_OFF       = 0x00;
constexpr int  INV_ENTRY_QTY_OFF      = 0x0C;
// Bounds for the shape-validation scan (a real bag is dozens..a few hundred slots).
constexpr int  INV_SCAN_OFF_LO        = 0x100;   // PlayerGameData-relative scan window for the
constexpr int  INV_SCAN_OFF_HI        = 0x2000;  // embedded EquipInventoryData container
constexpr int  INV_MAX_SLOTS          = 0x800;   // hard cap on entries walked per array

// FieldArea -> PlayRegionId (region-lock physical enforcement). AOB verified UNIQUE on 2.6.2.0;
// mov rcx,[rip+disp] -> ptrLoc RVA 0x03D691D8. PlayRegionId = *(s32*)(*(FieldArea_ptrLoc) + 0xE4).
constexpr const char* FieldArea_AOB =
    "48 8B 0D ?? ?? ?? ?? 48 ?? ?? ?? 44 0F B6 61 ?? E8";   // mov rcx,[rip+FieldArea]; ...
constexpr int  FieldArea_DISP_OFF       = 3;       // disp32 within the 7-byte mov rcx,[rip+..]
constexpr int  FIELDAREA_PLAYREGION_OFF = 0xE4;    // PlayRegionId (s32) off the FieldArea instance

}} // namespace er_ap::hooks
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  