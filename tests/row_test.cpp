#include "er_goods_row.h"
#include <cstdio>
#include <cstring>
using namespace er_ap;

static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static void put_s32(uint8_t* r, size_t off, uint32_t v) { std::memcpy(r + off, &v, 4); }

int main() {
    std::printf("EquipParamGoods row read (Paramdex offsets, row size %zu):\n", EQG_ROW_SIZE);
    uint8_t row[EQG_ROW_SIZE];

    // local-replacement synthetic: loc id with bit-31-set low half + high=1, real id 100100 x3
    std::memset(row, 0, sizeof row);
    put_s32(row, EQG_OFF_vagrantItemLotId,             0x8FA0E6B8u); // low (bit 31 set)
    put_s32(row, EQG_OFF_vagrantBonusEneDropItemLotId, 0x00000001u); // high
    put_s32(row, EQG_OFF_basicPrice,                   100100u);
    put_s32(row, EQG_OFF_sellValue,                    3u);

    GoodsRowFields f = ReadGoodsRow(row);
    CHECK(f.vagrantItemLotId == (int32_t)0x8FA0E6B8u);
    CHECK(f.basicPrice == 100100);
    CHECK(f.sellValue  == 3);
    CHECK(f.disableUseAtOutOfColiseum == false);

    SyntheticItem s = DecodeSyntheticRow(row);
    CHECK(s.apLocationId  == 0x18FA0E6B8LL);   // sign-safe recombine straight off the raw row
    CHECK(s.localItemId   == 100100);
    CHECK(s.localQuantity == 3);
    CHECK(s.foreignRemove == false);

    // bit isolation: set the two neighbor bools (bit4, bit6) but NOT bit5 -> foreignRemove false
    std::memset(row, 0, sizeof row);
    row[EQG_OFF_disableUseAtOutOfColiseum] = 0x10 | 0x40;
    CHECK(ReadGoodsRow(row).disableUseAtOutOfColiseum == false);

    // foreign-remove: bit5 set, no local item
    std::memset(row, 0, sizeof row);
    put_s32(row, EQG_OFF_vagrantItemLotId, 7004362u);  // a real ER-range location id
    row[EQG_OFF_disableUseAtOutOfColiseum] = EQG_BIT_disableUseAtOutOfColiseum;
    SyntheticItem g = DecodeSyntheticRow(row);
    CHECK(g.apLocationId  == 7004362LL);
    CHECK(g.foreignRemove == true);
    CHECK(g.localItemId   == 0);

    std::printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
