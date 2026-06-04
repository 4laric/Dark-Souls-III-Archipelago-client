// tests.cpp — unit tests for the ER AP client decode core + version checker.
//   build: g++ -std=c++17 -Wall -Wextra -O2 tests.cpp -o tests && ./tests
#include "er_item_decode.h"
#include "er_version_check.h"
#include <cstdio>
#include <cstdint>

using namespace er_ap;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { if (cond) { ++g_pass; } else { ++g_fail; \
    std::printf("  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_recombine() {
    std::printf("recombine (signed s32 halves -> int64, unsigned cast):\n");
    CHECK(RecombineLocationId(18007000, 0) == 18007000LL);   // sample world-pickup lot id
    CHECK(RecombineLocationId(101898, 0)   == 101898LL);     // sample shop id
    CHECK(RecombineLocationId(0, 2)        == 8589934592LL); // high word only (0x2_00000000)

    // bit 31 of the low half set: the field reads back as s32 = INT32_MIN. Must NOT sign-extend.
    CHECK(RecombineLocationId(INT32_MIN, 0) == 2147483648LL);   // 0x80000000
    CHECK(RecombineLocationId(INT32_MIN, 1) == 6442450944LL);   // 0x1_80000000
    CHECK(RecombineLocationId(-1, INT32_MAX) == 0x7FFFFFFFFFFFFFFFLL); // both halves max

    // prove the naive signed recombine would corrupt these, and ours doesn't.
    int64_t naive = static_cast<int64_t>(INT32_MIN) | (static_cast<int64_t>(0) << 32);
    CHECK(naive != 2147483648LL);                         // naive is wrong (sign-extended)...
    CHECK(RecombineLocationId(INT32_MIN, 0) != naive);    // ...the sign-safe recombine differs
}

static void test_detection() {
    std::printf("synthetic-goods detection (goods-only, id > 3,780,000):\n");
    CHECK(IsSyntheticGoods(CATEGORY_GOODS | 4000000u) == true);
    CHECK(IsSyntheticGoods(CATEGORY_GOODS | 3780001u) == true);
    CHECK(IsSyntheticGoods(CATEGORY_GOODS | 3780000u) == false);  // boundary: strictly greater
    CHECK(IsSyntheticGoods(CATEGORY_GOODS | 2220010u) == false);  // max real vanilla goods id

    // goods-only payoff: real items in other categories never misdetect, regardless of id magnitude
    CHECK(IsSyntheticGoods(CATEGORY_WEAPON    | 99060000u) == false);  // a real high-id NPC weapon
    CHECK(IsSyntheticGoods(CATEGORY_PROTECTOR | 5330000u)  == false);
    CHECK(IsSyntheticGoods(CATEGORY_ACCESSORY | 4000000u)  == false);

    CHECK(ItemCategoryOf(CATEGORY_GOODS | 7004362u) == CATEGORY_GOODS);
    CHECK(RowIdOf(CATEGORY_GOODS | 7004362u)        == 7004362u);
}

static void test_decode() {
    std::printf("full row decode:\n");
    GoodsRowFields local{ 18007000, 0, 1000000, 5, false };   // local check: real id 1000000 x5
    SyntheticItem s = DecodeSynthetic(local);
    CHECK(s.apLocationId  == 18007000LL);
    CHECK(s.localItemId   == 1000000);
    CHECK(s.localQuantity == 5);
    CHECK(s.foreignRemove == false);

    GoodsRowFields foreign{ 7004362, 0, 0, 0, true };         // foreign: no local, remove flag set
    SyntheticItem f = DecodeSynthetic(foreign);
    CHECK(f.apLocationId  == 7004362LL);
    CHECK(f.localItemId   == 0);
    CHECK(f.foreignRemove == true);
}

struct VCase { const char* ver; const char* range; bool expect; };
static void test_versions() {
    std::printf("version satisfies (node-semver pre-release-aware):\n");
    const char* L = ">=0.1.0-beta.1 <0.1.0-beta.2";   // lockstep now
    const char* G = ">=0.1.0 <0.2.0";                 // graduated at freeze
    const char* N = ">=0.1.0";                         // naive trap
    const VCase cases[] = {
        {"0.1.0-beta.1", L, true },
        {"0.1.0-beta.2", L, false},
        {"0.1.0",        L, false},
        {"0.2.0-beta.1", L, false},
        {"0.2.0",        L, false},
        {"0.1.0-beta.1", G, false},   // <-- the trap: hardcoding the graduated form rejects beta.1
        {"0.1.0",        G, true },
        {"0.2.0-beta.1", G, false},   // future-breaking pre-release does NOT leak (includePrerelease off)
        {"0.2.0",        G, false},
        {"0.1.0-beta.1", N, false},   // <-- the gotcha: plain >=0.1.0 rejects the matching beta.1
        {"0.1.0",        N, true },
        {"0.2.0",        N, true },    // ungated upper bound
    };
    for (const auto& c : cases) {
        bool got = versionSatisfies(c.ver, c.range);
        CHECK(got == c.expect);
        if (got != c.expect)
            std::printf("    (%-14s vs \"%s\" -> got %d want %d)\n", c.ver, c.range, got, c.expect);
    }
    CHECK(compareSemVer(parseSemVer("0.1.0-beta.1"), parseSemVer("0.1.0")) < 0);
    CHECK(compareSemVer(parseSemVer("0.1.0-beta.1"), parseSemVer("0.1.0-beta.2")) < 0);
    CHECK(compareSemVer(parseSemVer("0.1.0"), parseSemVer("0.1.0")) == 0);
}

int main() {
    test_recombine();
    test_detection();
    test_decode();
    test_versions();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
