#include "er_item_decode.h"
#include "codec_vectors_generated.h"
#include <cstdio>
using namespace er_ap;
int main() {
    int pass = 0, fail = 0;
    std::printf("client RecombineLocationId  vs  vagrant_codec.py golden vectors:\n");
    for (const auto& v : kCodecVectors) {
        int64_t got   = RecombineLocationId(v.low, v.high);
        int64_t naive = (int64_t)v.low | ((int64_t)v.high << 32);   // the buggy form, for contrast
        bool ok = (got == v.ap);
        std::printf("  %-34s ap=%-21lld %-9s naive:%s\n", v.name, (long long)v.ap,
                    ok ? "OK" : "MISMATCH", (naive == v.ap) ? "same" : "CORRUPT");
        ok ? ++pass : ++fail;
    }
    std::printf("\n%d matched, %d mismatched\n", pass, fail);
    return fail ? 1 : 0;
}
