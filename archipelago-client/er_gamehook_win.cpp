// er_gamehook_win.cpp — Windows/in-process implementation of er_gamehook.h.
// Requires: Windows, MinHook. Built into the injected client DLL alongside the existing client.
// The pure logic it calls (DecodeSyntheticPickup / DecidePickup / GoodsRow) is validated in walk_test.cpp.
//
// Client-provided hooks (defined by the existing ArchipelagoInterface; declared extern here):
//   void Archipelago_SendLocationCheck(int64_t apLocationId);   // report a collected location to AP
// The AP receive path calls Er_GrantReceivedGoods(realGoodsId, qty) (defined below) once it has
// resolved an incoming AP item to a concrete ER goods id.

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "er_gamehook.h"

extern void Archipelago_SendLocationCheck(int64_t apLocationId);

namespace er_ap { namespace game {
namespace {

// ---- module / scan range -------------------------------------------------------------------
uintptr_t   g_base = 0;
const uint8_t* g_textStart = nullptr;
size_t      g_textSize = 0;

bool LocateText() {
    HMODULE h = GetModuleHandleW(nullptr);              // the host exe (eldenring.exe)
    if (!h) return false;
    g_base = reinterpret_cast<uintptr_t>(h);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            g_textStart = reinterpret_cast<const uint8_t*>(g_base + sec[i].VirtualAddress);
            g_textSize  = sec[i].Misc.VirtualSize;
            return true;                                // first .text = the readable code section
        }
    }
    return false;
}

// ---- IDA-style pattern scan ("48 8B 0D ?? ?? ?? ??", ?? = wildcard) ------------------------
bool parseByte(const char* s, uint8_t& b, bool& wild) {
    if (s[0] == '?') { wild = true; return true; }
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = hex(s[0]), lo = hex(s[1]);
    if (hi < 0 || lo < 0) return false;
    b = uint8_t((hi << 4) | lo); wild = false; return true;
}

uintptr_t FindPattern(const char* pat) {
    uint8_t bytes[256]; bool wild[256]; size_t n = 0;
    for (const char* p = pat; *p && n < 256; ) {
        if (*p == ' ') { ++p; continue; }
        if (!parseByte(p, bytes[n], wild[n])) return 0;
        ++n; p += (*p == '?') ? 1 : 2;
    }
    if (!n || !g_textStart) return 0;
    const uint8_t* end = g_textStart + g_textSize - n;
    for (const uint8_t* s = g_textStart; s <= end; ++s) {
        size_t k = 0;
        for (; k < n; ++k) if (!wild[k] && s[k] != bytes[k]) break;
        if (k == n) return reinterpret_cast<uintptr_t>(s);
    }
    return 0;
}

// rip-relative pointer-location: target = match + instrLen + i32(match + dispOff)
uintptr_t RipTarget(uintptr_t match, int dispOff, int instrLen) {
    if (!match) return 0;
    int32_t disp; std::memcpy(&disp, reinterpret_cast<const void*>(match + dispOff), 4);
    return match + instrLen + disp;
}

// ---- resolved targets ----------------------------------------------------------------------
using AddItemFn = uint64_t(*)(void* inventory, void* entry, void* buffer, void* zero);
AddItemFn  g_addItemOrig    = nullptr;   // MinHook trampoline (real AddItemFunc)
uintptr_t  g_addItemTarget  = 0;
uintptr_t  g_invPtrLoc      = 0;         // *(g_invPtrLoc)  == inventory instance
uintptr_t  g_paramBasePtrLoc= 0;         // *(g_paramBasePtrLoc) == param-repository instance

// The synthetic placeholder's id is read from the descriptor the game passes to AddItemFunc.
// Per the CE recipe the descriptor entry is at rdx and the id sits at +0x04 (== buffer+0x24).
// VERIFY on first runs by logging `rawId` here before trusting the offset.
constexpr int ENTRY_ID_OFF = 0x04;

uint64_t Detour(void* inventory, void* entry, void* buffer, void* zero) {
    uint32_t rawId = 0;
    if (entry) std::memcpy(&rawId, reinterpret_cast<const uint8_t*>(entry) + ENTRY_ID_OFF, 4);

    SyntheticItem s{};
    if (DecodeSyntheticPickup(ParamRepoInstance(), rawId, s)) {
        Archipelago_SendLocationCheck(s.apLocationId);                  // always report the check
        if (DecidePickup(s) == PickupAction::SuppressAndGrant)
            GrantGoods(s.localItemId, s.localQuantity);                 // swap placeholder -> real item
        return 0;                                                       // suppress: never add the placeholder
    }
    return g_addItemOrig(inventory, entry, buffer, zero);               // real item: untouched
}

} // anonymous namespace

// ---- public API (er_gamehook.h) ------------------------------------------------------------
uintptr_t ParamRepoInstance() {
    if (!g_paramBasePtrLoc) return 0;
    uintptr_t inst; std::memcpy(&inst, reinterpret_cast<const void*>(g_paramBasePtrLoc), sizeof inst);
    return inst;
}
uintptr_t InventoryInstance() {
    if (!g_invPtrLoc) return 0;
    uintptr_t inst; std::memcpy(&inst, reinterpret_cast<const void*>(g_invPtrLoc), sizeof inst);
    return (inst >= 0x10000) ? inst : 0;                                // game's own validity guard
}

void GrantItem(int32_t idWithCategory, int32_t quantity, int32_t gem) {
    uintptr_t inv = InventoryInstance();
    if (!inv || !g_addItemOrig) return;
    // descriptor template (from the CE table): count@+0x20=1, id@+0x24, qty@+0x28, gem@+0x30=-1, trailing -1s.
    alignas(8) uint8_t buf[0x50];
    std::memset(buf, 0, sizeof buf);
    *reinterpret_cast<int32_t*>(buf + 0x20) = 1;                        // entry count
    *reinterpret_cast<int32_t*>(buf + hooks::ITEMBUF_ID_OFF)  = idWithCategory;  // 0x24
    *reinterpret_cast<int32_t*>(buf + hooks::ITEMBUF_QTY_OFF) = quantity;        // 0x28
    *reinterpret_cast<int32_t*>(buf + hooks::ITEMBUF_GEM_OFF) = gem;             // 0x30 (-1 = none)
    *reinterpret_cast<int32_t*>(buf + 0x34) = -1;
    *reinterpret_cast<int64_t*>(buf + 0x40) = -1;
    *reinterpret_cast<int32_t*>(buf + 0x4C) = -1;
    g_addItemOrig(reinterpret_cast<void*>(inv), buf + hooks::ITEMBUF_ENTRY_OFF, buf, nullptr);
}

bool SetEventFlag(uint32_t /*eventFlagId*/, bool /*on*/) {
    // Not required for the goods-only core loop: a picked-up goods placeholder is consumed on pickup,
    // so the location is not re-offered. The set/get function (er_hooks.h EventFlag_SetGet_AOB) and
    // CSEventFlagMan instance (er_singletons.h) are resolved-but-unused until its exact signature is
    // confirmed; wire this only if a location type needs an explicit collected-flag.
    return false;
}

bool Init() {
    if (!LocateText()) return false;
    if (MH_Initialize() != MH_OK) return false;

    g_addItemTarget = FindPattern(hooks::AddItemFunc_AOB);

    uintptr_t invMatch = FindPattern(hooks::InventoryAccessor_AOB);
    g_invPtrLoc = RipTarget(invMatch, /*dispOff*/0x19, /*instrLen*/0x1D);   // ...mov rcx,[rip+disp]

    uintptr_t pbMatch = FindPattern(hooks::ParamBase_AOB);
    g_paramBasePtrLoc = RipTarget(pbMatch, /*dispOff*/3, /*instrLen*/7);    // mov rcx,[rip+disp]

    if (!g_addItemTarget || !g_invPtrLoc || !g_paramBasePtrLoc) return false;

    if (MH_CreateHook(reinterpret_cast<void*>(g_addItemTarget), reinterpret_cast<void*>(&Detour),
                      reinterpret_cast<void**>(&g_addItemOrig)) != MH_OK) return false;
    return MH_EnableHook(reinterpret_cast<void*>(g_addItemTarget)) == MH_OK;
}

// ---- AP receive path: grant an item the server sent to this player ------------------------
void Er_GrantReceivedGoods(int32_t realGoodsId, int32_t quantity) { GrantGoods(realGoodsId, quantity); }

}} // namespace er_ap::game
#endif // _WIN32
