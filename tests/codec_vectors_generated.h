// AUTO-GENERATED from spec-2 vagrant_codec.py VECTORS — do not edit.
#pragma once
#include <cstdint>
namespace er_ap {
struct CodecVector { const char* name; int64_t ap; int32_t low; int32_t high; };
static const CodecVector kCodecVectors[] = {
    { "small id", 1000LL, (int32_t)0x000003E8u, (int32_t)0x00000000u },
    { "low32 bit-31 set (low<0 as s32)", 2147483648LL, (int32_t)0x80000000u, (int32_t)0x00000000u },
    { "low32 all-ones (reads as -1)", 4294967295LL, (int32_t)0xFFFFFFFFu, (int32_t)0x00000000u },
    { "just over 2^32 (high=1)", 4294967296LL, (int32_t)0x00000000u, (int32_t)0x00000001u },
    { "high bit-31 set", 4611686018427392564LL, (int32_t)0x00001234u, (int32_t)0x40000000u },
    { "plausible AP base+index", 11000003704LL, (int32_t)0x8FA6BC78u, (int32_t)0x00000002u },
    { "max int64", 9223372036854775807LL, (int32_t)0xFFFFFFFFu, (int32_t)0x7FFFFFFFu },
};
}
