// walk_test.cpp — host test of the pure gamehook core: build a fake param-repository -> ParamResCap
// -> PARAM blob -> row layout from real allocations, then run the full decode path through it.
#include "er_gamehook.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace er_ap;
using namespace er_ap::game;

static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static void wr32(uint8_t* p, size_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
static void wr16(uint8_t* p, size_t off, uint16_t v) { std::memcpy(p + off, &v, 2); }
static void wrptr(uint8_t* p, size_t off, void* v) { uint64_t q = (uint64_t)(uintptr_t)v; std::memcpy(p + off, &q, 8); }

int main() {
    using namespace er_ap::hooks;
    std::printf("pure gamehook walk (fake repo->cap->blob->row built from real allocations):\n");

    // allocate the fake structures (zeroed)
    uint8_t* repo = (uint8_t*)calloc(1, 0x400);
    uint8_t* cap  = (uint8_t*)calloc(1, 0x200);
    uint8_t* mid  = (uint8_t*)calloc(1, 0x200);    // intermediate struct: GetParam derefs +0x80 TWICE
    uint8_t* blob = (uint8_t*)calloc(1, 0x2000);

    // repo + goods_index*0x48 + 0x88  ->  cap        (the ParamResCap pointer for EquipParamGoods)
    wrptr(repo, (size_t)PARAM_INDEX_GOODS * PARAM_ENTRY_STRIDE + PARAM_ENTRY_OFF, cap);
    // cap + 0x80  ->  mid  ;  mid + 0x80  ->  blob    (GetParam derefs +0x80 twice; see er_gamehook.h)
    wrptr(cap, PARAM_HEADER_DEREF, mid);
    wrptr(mid, PARAM_HEADER_DEREF, blob);

    // blob row index: rowCount @ 0x0A; 24-byte entries @ 0x40 (id @ +0, dataOffset @ +8)
    wr16(blob, PARAM_ROWCOUNT_OFF, 2);
    const uint32_t DOFF0 = 0x100, DOFF1 = 0x100 + (uint32_t)EQG_ROW_SIZE;   // 176-byte stride
    wr32(blob, PARAM_ROWIDX_OFF + 0,  4000001); wr32(blob, PARAM_ROWIDX_OFF + 8,  DOFF0);
    wr32(blob, PARAM_ROWIDX_OFF + 24, 4000002); wr32(blob, PARAM_ROWIDX_OFF + 24 + 8, DOFF1);

    // row 0 (id 4000001): LOCAL synthetic — loc id with bit-31-set low half + high=1, real 100100 x3
    uint8_t* row0 = blob + DOFF0;
    wr32(row0, EQG_OFF_vagrantItemLotId,             0x8FA0E6B8u);
    wr32(row0, EQG_OFF_vagrantBonusEneDropItemLotId, 1u);
    wr32(row0, EQG_OFF_basicPrice,                   100100u);
    wr32(row0, EQG_OFF_sellValue,                    3u);
    // row 1 (id 4000002): FOREIGN — bit5 set, no local item
    uint8_t* row1 = blob + DOFF1;
    wr32(row1, EQG_OFF_vagrantItemLotId, 7004362u);
    row1[EQG_OFF_disableUseAtOutOfColiseum] = EQG_BIT_disableUseAtOutOfColiseum;

    uintptr_t REPO = (uintptr_t)repo;

    // ---- blob/row resolution ----
    CHECK(GoodsBlobFromRepo(REPO) == blob);
    CHECK(GoodsRow(REPO, 4000001) == row0);
    CHECK(GoodsRow(REPO, 4000002) == row1);
    CHECK(GoodsRow(REPO, 9999999) == nullptr);     // missing id

    // ---- decode of the LOCAL placeholder (raw gib id carries the goods category nibble) ----
    SyntheticItem s{};
    bool ok = DecodeSyntheticPickup(REPO, CATEGORY_GOODS | 4000001u, s);
    CHECK(ok);
    CHECK(s.apLocationId  == 0x18FA0E6B8LL);       // sign-safe recombine through the whole chain
    CHECK(s.localItemId   == 100100);
    CHECK(s.localQuantity == 3);
    CHECK(s.foreignRemove == false);
    CHECK(DecidePickup(s) == PickupAction::SuppressAndGrant);

    // ---- decode of the FOREIGN placeholder ----
    SyntheticItem f{};
    CHECK(DecodeSyntheticPickup(REPO, CATEGORY_GOODS | 4000002u, f));
    CHECK(f.apLocationId  == 7004362LL);
    CHECK(f.foreignRemove == true);
    CHECK(f.localItemId   == 0);
    CHECK(DecidePickup(f) == PickupAction::Suppress);

    // ---- real / non-synthetic ids must pass through (no decode) ----
    SyntheticItem dummy{};
    CHECK(!DecodeSyntheticPickup(REPO, CATEGORY_GOODS | 2220010u, dummy));  // max real vanilla goods
    CHECK(!DecodeSyntheticPickup(REPO, CATEGORY_WEAPON | 99060000u, dummy)); // a real high-id weapon
    CHECK(!DecodeSyntheticPickup(REPO, CATEGORY_GOODS | 4000001u, dummy) == false); // sanity: this one IS synthetic

    std::printf("\n%d passed, %d failed\n", pass, fail);
    free(repo); free(cap); free(mid); free(blob);
    return fail ? 1 : 0;
}
