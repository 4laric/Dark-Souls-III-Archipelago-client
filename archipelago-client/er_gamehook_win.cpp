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
#include <spdlog/spdlog.h>
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
        ++n;
        // Advance past the whole token, whether it's "XX", "??", or a single "?". The previous
        // (*p=='?')?1:2 advanced only 1 on a wildcard, so "??" was parsed as TWO wildcard bytes —
        // which silently broke any pattern with mid-sequence wildcards (e.g. ParamBase).
        while (*p && *p != ' ') ++p;
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

// ---- FD4 name-anchored singleton resolver (er_singletons.h method) --------------------------
// Phase 0 diagnostic for notify v2 Task B (SPEC-notify-banner.md): confirm the singleton slots
// Phase 2/3 will need actually resolve on the live build. Read-only; no calls, no writes.

// Scan the loaded module's sections for a null-terminated ASCII string; return its VA (0 if absent).
uintptr_t FindModuleString(const char* s) {
    if (!g_base) return 0;
    size_t len = std::strlen(s);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(g_base + sec[i].VirtualAddress);
        size_t size = sec[i].Misc.VirtualSize;
        if (size < len + 1) continue;
        for (size_t off = 0; off + len + 1 <= size; ++off) {
            if (base[off] == (uint8_t)s[0] &&
                std::memcmp(base + off, s, len) == 0 && base[off + len] == 0) {
                return reinterpret_cast<uintptr_t>(base + off);
            }
        }
    }
    return 0;
}

// Resolve an FD4 singleton's instance-slot VA via the uniform GET_SINGLETON accessor shape:
//   mov rax,[rip+slot]      (48/4C 8B 05 disp32)   <- the instance-slot load
//   ... (0x11 bytes) ...
//   lea rcx,[rip+className] (48/4C 8D /r rm=101)    <- references the class-name string
// Returns the slot VA (address OF the instance pointer), or 0 if the string or shape isn't found
// (e.g. MsgRepository: its name string is referenced from many non-accessor sites -> 0).
uintptr_t ResolveFd4SlotByName(const char* className) {
    uintptr_t nameVa = FindModuleString(className);
    if (!nameVa || !g_textStart || g_textSize < 8) return 0;
    const uint8_t* p   = g_textStart;
    const uint8_t* end = g_textStart + g_textSize - 7;
    for (; p <= end; ++p) {
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
            int32_t disp; std::memcpy(&disp, p + 3, 4);
            if (reinterpret_cast<uintptr_t>(p) + 7 + disp != nameVa) continue;
            const uint8_t* sm = p - 0x11;                       // uniform 0x11 backstep to slot mov
            if (sm < g_textStart) continue;
            if ((sm[0] == 0x48 || sm[0] == 0x4C) && sm[1] == 0x8B && (sm[2] & 0xC7) == 0x05) {
                int32_t sdisp; std::memcpy(&sdisp, sm + 3, 4);
                return reinterpret_cast<uintptr_t>(sm) + 7 + sdisp;   // slot VA
            }
        }
    }
    return 0;
}

// ---- resolved targets ----------------------------------------------------------------------
using AddItemFn = uint64_t(*)(void* inventory, void* entry, void* buffer, void* zero);
AddItemFn  g_addItemOrig    = nullptr;   // MinHook trampoline (real AddItemFunc)
uintptr_t  g_addItemTarget  = 0;
uintptr_t  g_invPtrLoc      = 0;         // *(g_invPtrLoc)  == inventory instance
uintptr_t  g_paramBasePtrLoc= 0;         // *(g_paramBasePtrLoc) == param-repository instance
uintptr_t  g_getEventFlagFn = 0;         // GetEventFlag(rcx=holder, edx=flagId) -> eax
uintptr_t  g_setEventFlagFn = 0;         // SetEventFlag(rcx=holder, rdx=&flagId, r8b=on)
uintptr_t  g_eventFlagPtrLoc= 0;         // *(g_eventFlagPtrLoc) == flag-holder instance
uintptr_t  g_gameDataManPtrLoc=0;       // *(g_gameDataManPtrLoc) == CSGameDataMan instance (player bag)
uintptr_t  g_luaWarpFn      = 0;         // LuaWarp(rcx=[mgr+0x18], rdx=[mgr+0x08], r8d=warpId-1000)
uintptr_t  g_csLuaEventMgrPtrLoc = 0;    // *(loc) == CSLuaEventManager instance (Hexinton CT warp)
uintptr_t  g_fieldAreaPtrLoc = 0;       // *(g_fieldAreaPtrLoc) == FieldArea instance; PlayRegionId @ +0xE4

// The synthetic placeholder's id is read from the descriptor the game passes to AddItemFunc.
// Per the CE recipe the descriptor entry is at rdx and the id sits at +0x04 (== buffer+0x24).
// VERIFY on first runs by logging `rawId` here before trusting the offset.
constexpr int ENTRY_ID_OFF = 0x04;

uint64_t Detour(void* inventory, void* entry, void* buffer, void* zero) {
    uint32_t rawId = 0;
    if (entry) std::memcpy(&rawId, reinterpret_cast<const uint8_t*>(entry) + ENTRY_ID_OFF, 4);

    SyntheticItem s{};
    if (DecodeSyntheticPickup(ParamRepoInstance(), rawId, s)) {
        // One concise line per AP pickup. The verbose per-item "AddItem detour" log and the repo
        // scan were diagnostics for the index/deref hunt and are removed now that the loop works;
        // the startup build stamp in Init() stays.
        // Report the check and suppress the placeholder. The item itself — own-world or foreign —
        // arrives via the server's ReceivedItems echo (items_handling 0b111) and is granted by
        // grantReceivedItems, the single granting path. Granting locally here too would duplicate.
        spdlog::info("AP pickup: location={} (item via server echo)", s.apLocationId);
        Archipelago_SendLocationCheck(s.apLocationId);
        return 0;                                                       // suppress: never add the placeholder
    }
    // auto_upgrade: bump an inbound REAL weapon (world loot, chest, NPC gift, craft) to the
    // player's current highest reinforce level on its own smithing track before the game adds it.
    if (entry) {
        int32_t upId = 0;
        if (AutoUpgradeWeaponId(rawId, &upId))
            std::memcpy(reinterpret_cast<uint8_t*>(entry) + ENTRY_ID_OFF, &upId, 4);
    }
    return g_addItemOrig(inventory, entry, buffer, zero);               // real item (maybe upgraded)
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
    // auto_upgrade: AP-granted weapons bypass the Detour, so upgrade them here too.
    { int32_t upId = 0; if (AutoUpgradeWeaponId(idWithCategory, &upId)) idWithCategory = upId; }
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

bool SetEventFlag(uint32_t eventFlagId, bool on) {
    // EventFlag_SetGet (er_hooks.h, RVA 0x5D2110). Signature from its prologue: rcx = flag holder,
    // rdx = POINTER to the flag id (`mov ebx,rdx; mov edx,[rdx]`), r8b = state. It forwards its own
    // rcx to the getter (0x5F9400) to read-before-write, so it takes the SAME holder we already use
    // for GetEventFlagState (g_eventFlagPtrLoc, RVA 0x3D68448) — confirmed-working in flag polling.
    if (!g_setEventFlagFn || !g_eventFlagPtrLoc) return false;
    uintptr_t holder;
    std::memcpy(&holder, reinterpret_cast<const void*>(g_eventFlagPtrLoc), sizeof holder);
    if (holder < 0x10000) return false;                                 // not initialized yet
    using SetFlagFn = void(*)(void* holder, uint32_t* flagId, uint8_t on);
    reinterpret_cast<SetFlagFn>(g_setEventFlagFn)(reinterpret_cast<void*>(holder), &eventFlagId, on ? 1 : 0);
    return true;
}

// Warp the player to a grace/warp point -- port of the Hexinton CT 'Fast Travel and Warp'.
// warpId = CT 'Choose Grace' dropdown id (e.g. 68000 = Gravesite Plain). The game routine is
// LuaWarp(rcx=[CSLuaEventManager+0x18], rdx=[CSLuaEventManager+0x08], r8d=warpId-1000).
bool Warp(int32_t warpId) {
    if (!g_luaWarpFn || !g_csLuaEventMgrPtrLoc) return false;
    uintptr_t mgr;
    std::memcpy(&mgr, reinterpret_cast<const void*>(g_csLuaEventMgrPtrLoc), sizeof mgr);
    if (mgr < 0x10000) return false;                                    // not initialized yet
    uint64_t a18, a08;
    std::memcpy(&a18, reinterpret_cast<const void*>(mgr + 0x18), sizeof a18);
    std::memcpy(&a08, reinterpret_cast<const void*>(mgr + 0x08), sizeof a08);
    using LuaWarpFn = void(*)(uint64_t, uint64_t, uint32_t);            // x64: rcx, rdx, r8d
    reinterpret_cast<LuaWarpFn>(g_luaWarpFn)(a18, a08, static_cast<uint32_t>(warpId - 1000));
    return true;
}

bool Init() {
    if (!LocateText()) { spdlog::error("er::Init LocateText failed (.text not found)"); return false; }
    spdlog::info("er::Init .text base={:#x} start={:#x} size={:#x}",
                 g_base, reinterpret_cast<uintptr_t>(g_textStart), g_textSize);

    if (MH_Initialize() != MH_OK) { spdlog::error("er::Init MH_Initialize failed"); return false; }

    g_addItemTarget = FindPattern(hooks::AddItemFunc_AOB);

    uintptr_t invMatch = FindPattern(hooks::InventoryAccessor_AOB);
    g_invPtrLoc = RipTarget(invMatch, /*dispOff*/0x19, /*instrLen*/0x1D);   // ...mov rcx,[rip+disp]

    uintptr_t pbMatch = FindPattern(hooks::ParamBase_AOB);
    g_paramBasePtrLoc = RipTarget(pbMatch, /*dispOff*/3, /*instrLen*/7);    // mov rcx,[rip+disp]

    // Event-flag read path (optional: flag polling for shop/gift/offline checks). Non-fatal if
    // either signature misses — the AddItemFunc detour still covers itemlot pickups.
    g_getEventFlagFn = FindPattern(hooks::GetEventFlagFunc_AOB);
    g_setEventFlagFn = FindPattern(hooks::EventFlag_SetGet_AOB);
    uintptr_t efmMatch = FindPattern(hooks::EventFlagMan_AOB);
    g_eventFlagPtrLoc = efmMatch ? RipTarget(efmMatch, /*dispOff*/3, /*instrLen*/7) : 0;  // mov rdi,[rip+disp]
    spdlog::info("er::Init flag scans: getEventFlagFn={:#x} setEventFlagFn={:#x} efmMatch={:#x} eventFlagPtrLoc={:#x}",
                 g_getEventFlagFn, g_setEventFlagFn, efmMatch, g_eventFlagPtrLoc);

    // Player-inventory removal path (TODO #6 client half). Non-fatal: if GameDataMan misses, the
    // AddItemFunc detour + grants still work; only removeFromInventory degrades to a no-op.
    uintptr_t gdMatch = FindPattern(hooks::GameDataMan_AOB);
    g_gameDataManPtrLoc = gdMatch ? RipTarget(gdMatch, hooks::GameDataMan_DISP_OFF, 7) : 0;  // mov rax,[rip+disp]
    spdlog::info("er::Init GameDataMan: match={:#x} ptrLoc={:#x}", gdMatch, g_gameDataManPtrLoc);

    // FieldArea -> PlayRegionId (region-lock physical enforcement). Non-fatal: if it misses, the
    // region-lock kick poll degrades to a no-op. mov rcx,[rip+disp] -> ptr-loc (expect RVA 0x03D691D8).
    uintptr_t faMatch = FindPattern(hooks::FieldArea_AOB);
    g_fieldAreaPtrLoc = faMatch ? RipTarget(faMatch, hooks::FieldArea_DISP_OFF, 7) : 0;
    spdlog::info("er::Init FieldArea: match={:#x} ptrLoc={:#x}", faMatch, g_fieldAreaPtrLoc);

    // Warp (Hexinton CT 'Fast Travel and Warp'): LuaWarp fn (call target = match+2) + the
    // CSLuaEventManager singleton ptr-loc. Non-fatal: /warp degrades to a no-op if either misses.
    uintptr_t lwMatch  = FindPattern("C3 ?? ?? ?? ?? ?? ?? 57 48 83 EC ?? 48 8B FA 44");
    g_luaWarpFn = lwMatch ? lwMatch + 2 : 0;
    uintptr_t lemMatch = FindPattern("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 41 BE 01 00 00 00 44 89 75");
    g_csLuaEventMgrPtrLoc = lemMatch ? RipTarget(lemMatch, /*dispOff*/3, /*instrLen*/7) : 0;
    spdlog::info("er::Init Warp: luaWarpMatch={:#x} luaWarpFn={:#x} csLuaEventMgr={:#x}",
                 lwMatch, g_luaWarpFn, g_csLuaEventMgrPtrLoc);

    // Notify v2 Task B Phase 0 (SPEC-notify-banner.md): resolve the managers Phase 2/3 will need and
    // LOG ONLY (no calls, no writes) to confirm the name-anchored FD4 slots on the live build.
    // CSItemGetMenuMan = native item-acquired popup (Phase 3 suppression target); CSMenuMan = menu
    // mgr. MsgRepository (Phase 2 FMG text) is NOT resolvable via the uniform accessor shape — its
    // name string is referenced from many non-accessor sites — so it logs "not resolved" by design;
    // Phase 2 needs a dedicated AOB. Expected slot RVAs are from static analysis of this 2.6.2.0 exe.
    auto logFd4 = [](const char* name, uintptr_t expectRva) {
        uintptr_t slot = ResolveFd4SlotByName(name);
        if (!slot) { spdlog::info("er::Init FD4 {}: not resolved via uniform accessor", name); return; }
        uintptr_t slotRva = slot - g_base;
        uintptr_t inst = 0;
        if (slot >= g_base && slot < g_base + 0x08000000)   // slot is module .data (mapped); bounded guard
            std::memcpy(&inst, reinterpret_cast<const void*>(slot), sizeof inst);
        spdlog::info("er::Init FD4 {}: slotRVA={:#x}{} instance={:#x}", name, slotRva,
                     (expectRva && slotRva != expectRva) ? " (MISMATCH vs expected)" : "", inst);
    };
    logFd4("CSItemGetMenuMan", 0x03D6C3D8);
    logFd4("CSMenuMan",        0x03D6B800);
    logFd4("MsgRepository",    0);

    // Build stamp: auto-updates every compile (so a stale DLL is obvious), and prints the
    // compiled-in PARAM_INDEX_GOODS so we can confirm constant changes actually landed.
    spdlog::info("er::Init BUILD {} {} | PARAM_INDEX_GOODS={} | goods blob walk=double +0x80 deref",
                 __DATE__, __TIME__, hooks::PARAM_INDEX_GOODS);
    spdlog::info("er::Init scans: addItem={:#x} invMatch={:#x} invPtrLoc={:#x} pbMatch={:#x} paramBasePtrLoc={:#x}",
                 g_addItemTarget, invMatch, g_invPtrLoc, pbMatch, g_paramBasePtrLoc);

    if (!g_addItemTarget) { spdlog::error("er::Init AddItemFunc signature not found"); return false; }
    if (!g_invPtrLoc)     { spdlog::error("er::Init InventoryAccessor signature not found"); return false; }
    if (!g_paramBasePtrLoc){ spdlog::error("er::Init ParamBase signature not found"); return false; }

    if (MH_CreateHook(reinterpret_cast<void*>(g_addItemTarget), reinterpret_cast<void*>(&Detour),
                      reinterpret_cast<void**>(&g_addItemOrig)) != MH_OK) {
        spdlog::error("er::Init MH_CreateHook failed"); return false;
    }
    bool ok = MH_EnableHook(reinterpret_cast<void*>(g_addItemTarget)) == MH_OK;
    spdlog::info("er::Init AddItemFunc hook {}", ok ? "ENABLED" : "enable FAILED");
    return ok;
}

// ---- AP receive path: grant an item the server sent to this player ------------------------
void Er_GrantReceivedGoods(int32_t realGoodsId, int32_t quantity) { GrantGoods(realGoodsId, quantity); }

// ---- event flag read (check detection for acquisitions that bypass AddItemFunc) -----------
bool GetEventFlagState(uint32_t flagId) {
    if (!g_getEventFlagFn || !g_eventFlagPtrLoc) return false;
    uintptr_t holder;
    std::memcpy(&holder, reinterpret_cast<const void*>(g_eventFlagPtrLoc), sizeof holder);
    if (holder < 0x10000) return false;                                 // not initialized yet
    using GetFlagFn = uint32_t(*)(void* holder, uint32_t flagId);
    return reinterpret_cast<GetFlagFn>(g_getEventFlagFn)(reinterpret_cast<void*>(holder), flagId) != 0;
}

// ---- FieldArea: current PlayRegionId (region-lock physical enforcement) --------------------
// The open world's authoritative "which area am I in" id (same signal that draws the area-name
// banner). -1 until FieldArea resolves + the player is in-world.
int32_t GetPlayRegionId() {
    if (!g_fieldAreaPtrLoc) return -1;
    uintptr_t fa;
    std::memcpy(&fa, reinterpret_cast<const void*>(g_fieldAreaPtrLoc), sizeof fa);
    if (fa < 0x10000) return -1;
    int32_t playRegion;
    std::memcpy(&playRegion, reinterpret_cast<const void*>(fa + hooks::FIELDAREA_PLAYREGION_OFF), sizeof playRegion);
    return playRegion;
}

// ---- inventory removal: pull a synthetic / own-world placeholder out of the bag (TODO #6) ----
namespace {

// Page-validated read: returns false (out untouched) unless [addr, addr+sizeof(T)) is committed
// and readable. This makes the container auto-discovery scan below crash-proof even when it probes
// a wrong PlayerGameData offset or a momentarily-stale pointer.
template <typename T>
bool SafeRead(uintptr_t addr, T& out) {
    if (addr < 0x10000 || addr >= 0x7FFFFFFFFFFFull) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (addr + sizeof(T) > regionEnd) return false;
    std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
    return true;
}

// Walk one item array (stride INV_ENTRY_STRIDE) for an entry whose id matches; on the first match
// with qty>0, decrement (clamped at 0) and return true. `dump` logs every entry (debug level) so
// the id/qty field offsets can be eyeballed from the log on the first real in-game run.
bool ScanArrayAndRemove(uintptr_t arr, int slots, int32_t fullId, int32_t goodsId,
                        int32_t quantity, bool dump, const char* tag) {
    using namespace hooks;
    if (arr < 0x10000) return false;
    if (slots < 0 || slots > INV_MAX_SLOTS) slots = INV_MAX_SLOTS;
    for (int i = 0; i < slots; ++i) {
        uintptr_t e = arr + static_cast<uintptr_t>(INV_ENTRY_STRIDE) * i;
        int32_t id, qty;
        if (!SafeRead(e + INV_ENTRY_ID_OFF,  id))  break;   // walked off the committed array
        if (!SafeRead(e + INV_ENTRY_QTY_OFF, qty)) break;
        if (dump && id != 0)
            spdlog::debug("  inv[{}/{}] id={:#010x} qty={}", tag, i, static_cast<uint32_t>(id), qty);
        if ((id == fullId || id == goodsId) && qty > 0) {
            int32_t newQty = qty - quantity; if (newQty < 0) newQty = 0;
            std::memcpy(reinterpret_cast<void*>(e + INV_ENTRY_QTY_OFF), &newQty, sizeof newQty);
            spdlog::info("removeFromInventory: matched id={:#010x} ({} slot {}): qty {} -> {}",
                         static_cast<uint32_t>(id), tag, i, qty, newQty);
            return true;
        }
    }
    return false;
}

} // anonymous namespace

bool RemoveInventoryItem(int32_t fullItemId, int32_t quantity) {
    using namespace hooks;
    if (quantity <= 0) return false;
    if (!g_gameDataManPtrLoc) { spdlog::warn("removeFromInventory: GameDataMan unresolved; no-op"); return false; }

    uintptr_t gdm = 0, pgd = 0;
    if (!SafeRead(g_gameDataManPtrLoc, gdm) || gdm < 0x10000) return false;        // not in-game yet
    if (!SafeRead(gdm + GAMEDATAMAN_PGD_OFF, pgd) || pgd < 0x10000) return false;  // no PlayerGameData

    const int32_t rawId   = fullItemId & 0x0FFFFFFF;
    const int32_t goodsId = static_cast<int32_t>(static_cast<uint32_t>(rawId) | CATEGORY_GOODS);

    // Auto-discover the embedded EquipInventoryData container by shape, then act ONLY on an exact
    // id match -> a mis-resolved offset is a no-op, never a corrupting write. Act on the first
    // candidate that actually holds the target id; if none do, the item simply isn't carried.
    bool acted = false; int validated = 0;
    for (int off = INV_SCAN_OFF_LO; off + 0x60 <= INV_SCAN_OFF_HI && !acted; off += 8) {
        uintptr_t cont = pgd + off;
        int32_t slotCount;
        if (!SafeRead(cont + INV_SLOTCOUNT_OFF, slotCount)) continue;
        if (slotCount <= 0 || slotCount > INV_MAX_SLOTS) continue;
        uintptr_t primary = 0, overflow = 0;
        if (!SafeRead(cont + INV_PRIMARY_PTR_OFF, primary) || primary < 0x10000) continue;
        if ((primary & 0x3) != 0) continue;                     // entries are 4-byte aligned
        int32_t id0, qty0;
        if (!SafeRead(primary + INV_ENTRY_ID_OFF,  id0))  continue;
        if (!SafeRead(primary + INV_ENTRY_QTY_OFF, qty0)) continue;
        if (qty0 < 0 || qty0 > 0x270F) continue;                // ER stacks cap at 9999
        ++validated;
        SafeRead(cont + INV_OVERFLOW_PTR_OFF, overflow);
        spdlog::debug("removeFromInventory: candidate container pgd+{:#x} slotCount={} primary={:#x}",
                      off, slotCount, primary);
        if (ScanArrayAndRemove(primary, slotCount, fullItemId, goodsId, quantity, true, "prim")) { acted = true; break; }
        if (overflow >= 0x10000 &&
            ScanArrayAndRemove(overflow, INV_MAX_SLOTS, fullItemId, goodsId, quantity, false, "over")) { acted = true; break; }
    }
    if (!acted)
        spdlog::info("removeFromInventory: id={:#010x} not present (validated {} container(s)); no-op",
                     static_cast<uint32_t>(fullItemId), validated);
    return acted;
}

// ===== auto_upgrade (slot_data auto_upgrade) ================================================
// Bump an acquired weapon to the player's current highest reinforce level on its own smithing
// track. ER bakes the level into the item id (id = base + level, base % 100 == 0) but keeps only
// ONE EquipParamWeapon row per weapon; the cap+track come from reinforceTypeId via
// ReinforceParamWeapon (cap = #consecutive rows from rt; >10 => normal, 1..10 => somber). The
// reinforceTypeId byte offset is SELF-CALIBRATED at runtime; on failure the feature stays idle.
namespace {
    int g_autoUpgrade        = 0;
    const uint8_t* g_eqwBlob = nullptr;   // EquipParamWeapon blob (cached)
    const uint8_t* g_rpwBlob = nullptr;   // ReinforceParamWeapon blob (cached)
    int  g_rtOffset          = -1;        // calibrated byte offset of reinforceTypeId in an EQW row
    bool g_calibFailed       = false;
    int  g_normalTarget      = 0;         // live highest +N among normal-track weapons (0..25)
    int  g_somberTarget      = 0;         // live highest +N among somber-track weapons (0..10)

    // perf caches (auto-upgrade was re-walking inventory + rescanning params on every grant):
    int       g_invContainerOff   = -1;   // cached PlayerGameData-relative EquipInventoryData offset
    uint64_t  g_lastRefreshTick    = 0;   // GetTickCount64() of the last full inventory walk
    static const uint64_t kRefreshThrottleMs = 1500;  // coalesce reconnect-replay bursts
    int16_t   g_capRt[48]  = {0};         // small reinforceTypeId -> cap cache (keys)
    int8_t    g_capVal[48] = {0};         // small reinforceTypeId -> cap cache (values)
    int       g_capCount   = 0;

    // base-game anchors (always present in the param) for offset self-calibration:
    const int32_t kNormalAnchors[4] = { 2000000, 1000000, 9000000, 11010000 };  // reinforceTypeId 0
    const int32_t kSomberAnchors[4] = { 9060000, 9040000, 3090000, 3140000 };   // reinforceTypeId 2200
    const int16_t kSomberRT = 2200;

    const uint8_t* SafeBlobFromRepoIndex(uintptr_t repo, int index) {
        using namespace hooks;
        uint64_t hdr = 0, mid = 0, blob = 0;
        if (!SafeRead(repo + (uintptr_t)index * PARAM_ENTRY_STRIDE + PARAM_ENTRY_OFF, hdr) || !hdr) return nullptr;
        if (!SafeRead((uintptr_t)hdr + PARAM_HEADER_DEREF, mid) || !mid) return nullptr;
        if (!SafeRead((uintptr_t)mid + PARAM_HEADER_DEREF, blob) || !blob) return nullptr;
        return reinterpret_cast<const uint8_t*>((uintptr_t)blob);
    }
    inline bool RowS16(const uint8_t* row, int off, int16_t& v) {
        return SafeRead(reinterpret_cast<uintptr_t>(row) + off, v);
    }

    // Resolve both weapon params by content fingerprint (the repo is NOT BND-ordered).
    bool ResolveWeaponParams() {
        if (g_eqwBlob && g_rpwBlob) return true;
        uintptr_t repo = ParamRepoInstance();
        if (repo < 0x10000) return false;
        for (int i = 0; i < 400 && !(g_eqwBlob && g_rpwBlob); ++i) {
            const uint8_t* b = SafeBlobFromRepoIndex(repo, i);
            if (!b) continue;
            if (!g_eqwBlob && RowInBlob(b, 2000000) && RowInBlob(b, 1000000))
                g_eqwBlob = b;                                                  // EquipParamWeapon
            if (!g_rpwBlob && RowInBlob(b, 0) && RowInBlob(b, 25) && RowInBlob(b, 2200)
                           && RowInBlob(b, 2210) && RowInBlob(b, 9010))
                g_rpwBlob = b;                                                  // ReinforceParamWeapon
        }
        return g_eqwBlob && g_rpwBlob;
    }

    // Find the s16 offset where all normal anchors read 0 and all somber anchors read 2200.
    bool CalibrateOffset() {
        if (g_rtOffset >= 0) return true;
        if (g_calibFailed)   return false;
        const uint8_t* na[4]; const uint8_t* sa[4];
        for (int k = 0; k < 4; ++k) { na[k] = RowInBlob(g_eqwBlob, kNormalAnchors[k]);
                                      sa[k] = RowInBlob(g_eqwBlob, kSomberAnchors[k]); }
        for (int o = 0; o <= 1024; ++o) {
            bool ok = true;
            for (int k = 0; k < 4 && ok; ++k) { int16_t v; if (!na[k] || !RowS16(na[k], o, v) || v != 0)         ok = false; }
            for (int k = 0; k < 4 && ok; ++k) { int16_t v; if (!sa[k] || !RowS16(sa[k], o, v) || v != kSomberRT) ok = false; }
            if (ok) { g_rtOffset = o;
                      spdlog::info("auto_upgrade: reinforceTypeId offset calibrated to +{:#x}", o); return true; }
        }
        g_calibFailed = true;
        spdlog::warn("auto_upgrade: reinforceTypeId calibration FAILED; auto-upgrade idle this session");
        return false;
    }

    bool Ready() {
        if (!g_autoUpgrade) return false;
        if (!ResolveWeaponParams()) { spdlog::warn("auto_upgrade: weapon params not found; idle this session"); return false; }
        return CalibrateOffset();
    }

    // cap = max k in [0,25] s.t. ReinforceParamWeapon row (rt + k) exists.
    int CapForRT(int16_t rt) {
        for (int j = 0; j < g_capCount; ++j) if (g_capRt[j] == rt) return g_capVal[j];   // cache hit
        int k = 0; while (k <= 25 && RowInBlob(g_rpwBlob, (int32_t)rt + k)) ++k;
        int cap = k - 1;
        if (g_capCount < (int)(sizeof g_capRt / sizeof g_capRt[0])) {
            g_capRt[g_capCount] = rt; g_capVal[g_capCount] = (int8_t)cap; ++g_capCount;
        }
        return cap;
    }

    struct WInfo { int32_t base; int level; int cap; bool somber; };
    // Decode an item id as an upgradeable weapon (acquirable base-weapon id range only).
    bool WeaponInfo(int32_t itemId, WInfo& out) {
        if ((static_cast<uint32_t>(itemId) & er_ap::CATEGORY_MASK) != er_ap::CATEGORY_WEAPON) return false;
        if (itemId < 1000000 || itemId >= 90000000) return false;   // skip system/NPC + non-weapons
        out.base  = itemId - (itemId % 100);
        out.level = itemId % 100;
        const uint8_t* row = RowInBlob(g_eqwBlob, out.base);
        if (!row) return false;
        int16_t rt; if (!RowS16(row, g_rtOffset, rt)) return false;
        out.cap = CapForRT(rt);
        if (out.cap <= 0) return false;            // not upgradeable
        out.somber = (out.cap <= 10);
        return true;
    }
}

// DIAGNOSTIC kill switch (2026-06-16): when 1, force auto_upgrade OFF regardless of slot_data.
// Purpose = isolate the Load-Game crash. With auto_upgrade off, Ready() short-circuits so the
// detour is fully transparent (its pre-auto_upgrade behavior: no calibration, no inventory walk,
// no id rewrite -- it just forwards to the game's AddItemFunc). If the game STILL crashes on Load
// Game with this on, the crash is NOT in auto_upgrade and we look at the detour/game-load path.
// Flip back to 0 to restore slot_data-driven auto_upgrade once the crash is pinned.
static constexpr int kForceAutoUpgradeOff = 0;

void SetAutoUpgrade(int on) {
    if (kForceAutoUpgradeOff) on = 0;
    g_autoUpgrade = on ? 1 : 0;
    if (!g_autoUpgrade) { g_eqwBlob = g_rpwBlob = nullptr; g_rtOffset = -1; g_calibFailed = false;
                          g_normalTarget = g_somberTarget = 0; }
    spdlog::info("auto_upgrade: {}{}", g_autoUpgrade ? "ENABLED" : "off",
                 kForceAutoUpgradeOff ? "  (FORCED OFF - diagnostic kill switch)" : "");
}

// Recompute the two track targets from live inventory (reuses the RemoveInventoryItem shape walk).
// Cheap integer test: could this id be an acquirable, upgradeable weapon? Avoids the linear
// EquipParamWeapon lookup for the ~all non-weapon inventory entries (goods/armor/garbage).
static inline bool LooksLikeWeaponId(int32_t id) {
    if ((static_cast<uint32_t>(id) & er_ap::CATEGORY_MASK) != er_ap::CATEGORY_WEAPON) return false;
    if (id < 1000000 || id >= 90000000) return false;
    return (id % 100) <= 25;
}

// One-shot region validation: VirtualQuery the array ONCE and return how many committed+readable
// bytes are available from addr (capped to want), so the walk can read entries with raw memcpy
// instead of a VirtualQuery per field (that path was thousands of syscalls per walk = slow).
static inline size_t SafeRegionLen(uintptr_t addr, size_t want) {
    if (addr < 0x10000) return 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    size_t avail = regionEnd - addr;
    return avail < want ? avail : want;
}

// SEH-guarded raw read for the inventory walk. SafeRegionLen validates the array with a single
// VirtualQuery, but on reconnect the game thread bulk-grants items concurrently and can realloc/
// free that array between the check and this read (TOCTOU). A raw memcpy would then fault and take
// the process down; catching the access violation here turns the race into a clean bail. POD-only
// helper on purpose: keeps __try out of RefreshAutoUpgradeTargets and avoids C2712 unwinding.
static inline bool SehReadI32(const void* p, int32_t& out) {
    __try { std::memcpy(&out, p, sizeof out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void RefreshAutoUpgradeTargets() {
    using namespace hooks;
    if (!Ready() || !g_gameDataManPtrLoc) return;
    // Throttle: a reconnect re-grants dozens of items back-to-back; a full walk per weapon stalled
    // long enough to drop the AP socket. Between walks we reuse the cached targets (a manual
    // smithing-table upgrade is still reflected within the throttle window).
    uint64_t now = GetTickCount64();
    if (g_lastRefreshTick != 0 && now - g_lastRefreshTick < kRefreshThrottleMs) return;
    uintptr_t gdm = 0, pgd = 0;
    if (!SafeRead(g_gameDataManPtrLoc, gdm) || gdm < 0x10000) return;
    if (!SafeRead(gdm + GAMEDATAMAN_PGD_OFF, pgd) || pgd < 0x10000) return;

    // Walk ONE container. If we already know which PlayerGameData offset is the bag, go straight
    // there; otherwise discover it (first container that actually holds a weapon) and cache it.
    int lo = (g_invContainerOff >= 0) ? g_invContainerOff : INV_SCAN_OFF_LO;
    int hi = (g_invContainerOff >= 0) ? g_invContainerOff : (INV_SCAN_OFF_HI - 0x60);
    int normal = 0, somber = 0; bool found = false;
    for (int off = lo; off <= hi && !found; off += 8) {
        uintptr_t cont = pgd + off; int32_t slotCount = 0;
        if (!SafeRead(cont + INV_SLOTCOUNT_OFF, slotCount)) continue;
        if (slotCount <= 0 || slotCount > INV_MAX_SLOTS) continue;
        uintptr_t primary = 0, overflow = 0;
        if (!SafeRead(cont + INV_PRIMARY_PTR_OFF, primary) || primary < 0x10000) continue;
        if ((primary & 0x3) != 0) continue;
        int32_t id0 = 0, qty0 = 0;
        if (!SafeRead(primary + INV_ENTRY_ID_OFF, id0))  continue;
        if (!SafeRead(primary + INV_ENTRY_QTY_OFF, qty0)) continue;
        if (qty0 < 0 || qty0 > 0x270F) continue;
        SafeRead(cont + INV_OVERFLOW_PTR_OFF, overflow);
        const uintptr_t arrays[2] = { primary, overflow };
        const int       counts[2] = { slotCount, INV_MAX_SLOTS };
        int wn = 0, ws = 0, weapons = 0;
        for (int a = 0; a < 2; ++a) {
            uintptr_t arr = arrays[a]; if (arr < 0x10000) continue;
            size_t okEntries = SafeRegionLen(arr, (size_t)counts[a] * INV_ENTRY_STRIDE) / INV_ENTRY_STRIDE;
            for (size_t i = 0; i < okEntries; ++i) {
                const uint8_t* e = reinterpret_cast<const uint8_t*>(arr + i * INV_ENTRY_STRIDE);
                int32_t itemId;                                                  // +0x04 = resolved item id (base+level)
                if (!SehReadI32(e + 0x04, itemId)) break;   // array freed under us mid-walk (reconnect bulk-grant race) -> bail
                if (!LooksLikeWeaponId(itemId)) continue;
                WInfo wi;
                if (!WeaponInfo(itemId, wi)) continue;
                ++weapons;
                if (wi.somber) { if (wi.level > ws) ws = wi.level; }
                else           { if (wi.level > wn) wn = wi.level; }
            }
        }
        // Accept this container iff it holds a weapon (disambiguates the bag from look-alike
        // memory regions, the way RemoveInventoryItem uses a target-id match). Cache its offset.
        if (weapons > 0 || g_invContainerOff == off) {
            normal = wn; somber = ws; g_invContainerOff = off; found = true;
        }
    }
    if (found) { g_normalTarget = normal; g_somberTarget = somber; g_lastRefreshTick = now; }
    else if (g_invContainerOff >= 0) { g_invContainerOff = -1; }   // cached offset went stale; rediscover next time
}

static bool AutoUpgradeWeaponIdImpl(int32_t inItemId, int32_t* outItemId) {
    if (!outItemId || !Ready()) return false;
    WInfo wi;
    if (!WeaponInfo(inItemId, wi)) return false;            // not an upgradeable weapon
    RefreshAutoUpgradeTargets();                            // live read (captures manual upgrades)
    int target = wi.somber ? g_somberTarget : g_normalTarget;
    if (target > wi.cap) target = wi.cap;
    if (target <= wi.level) return false;                   // already at/above target
    *outItemId = wi.base + target;
    spdlog::info("auto_upgrade: weapon {:#x} (+{}) -> +{} ({} track, cap +{})",
                 static_cast<uint32_t>(wi.base), wi.level, target, wi.somber ? "somber" : "normal", wi.cap);
    return true;
}

// SEH crash-guard for the whole auto_upgrade compute. The detour fires while a save is LOADING --
// the game is (re)building the player inventory concurrently, so the array this path walks can be
// freed/realloced under us (TOCTOU). SehReadI32 covers the per-entry reads, but a fault can also
// land in the container probe, SafeRegionLen's check->read window, or a param-blob row read. That
// is the observed "crash on Load Game": the log dies right after offset calibration, mid-walk,
// with no "weapon -> +N" line, so the fault is inside this compute (WeaponInfo/RefreshTargets),
// not the later game AddItem call. Wrap it so ANY access violation degrades to "no upgrade; add the
// item unchanged" instead of taking the game down. POD-only wrapper (no unwinding) -> __try legal.
bool AutoUpgradeWeaponId(int32_t inItemId, int32_t* outItemId) {
    __try { return AutoUpgradeWeaponIdImpl(inItemId, outItemId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}



// ===== global Scadutree Blessing (slot_data global_scadutree_blessing) ======================
// SPEC-global-scadutree-blessing.md (P1). Count held Scadutree Fragments -> blessing level via
// the vanilla cost curve -> write the stored combat blessing byte at PlayerGameData + 0xFC so the
// 20000100+N buff applies in the base game (the engine recomputes the speffect from this byte on
// the next map load / grace rest). Offset from the Hexinton/TGA CE table: GameDataMan -> +0x08 ->
// +0xFC (signed byte). NOTE: 0xFC is relative to the client's resolved `pgd`; this assumes the
// client's pgd == the table's [GameDataMan+0x08]. If the buff never applies, verify that first.
namespace {
    int g_globalScaduBlessing = 0;   // 0=off, 1=player_only, 2=scaled
    const uintptr_t kScaduCombatLevelOff = 0xFC;   // PlayerGameData + 0xFC (combat blessing, byte)
    // cumulative Scadutree Fragments required to REACH each combat level (0..20):
    const int kScaduCum[21] = {0,1,3,5,7,9,11,13,15,17,20,23,26,29,32,35,38,41,44,47,50};
}

// POD-only SEH write helper (keeps __try out of the spdlog-using tick fn; see SehReadI32).
static inline bool SehWriteU8(void* p, uint8_t v) {
    __try { *reinterpret_cast<volatile uint8_t*>(p) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SetGlobalScaduBlessing(int mode) {
    g_globalScaduBlessing = (mode == 1 || mode == 2) ? mode : 0;
    spdlog::info("global_scadu_blessing: {}", g_globalScaduBlessing ? "ENABLED" : "off");
}

void TickGlobalScaduBlessing() {
    using namespace hooks;
    if (!g_globalScaduBlessing) return;
    if (!Ready() || !g_gameDataManPtrLoc) return;
    // Throttle: this walks the bag; once a second is plenty for a stored-byte watchdog.
    static uint64_t s_lastTick = 0;
    uint64_t now = GetTickCount64();
    if (s_lastTick != 0 && now - s_lastTick < 1000) return;
    s_lastTick = now;

    uintptr_t gdm = 0, pgd = 0;
    if (!SafeRead(g_gameDataManPtrLoc, gdm) || gdm < 0x10000) return;
    if (!SafeRead(gdm + GAMEDATAMAN_PGD_OFF, pgd) || pgd < 0x10000) return;

    // Both AP fragment items ('Scadutree Fragment' / '... x2') share goods id 2010000, so the bag
    // holds ONE stack whose quantity = total fragments. In the bag, goods carry the category nibble.
    const uint32_t kFragFull = 2010000u | CATEGORY_GOODS;
    int fragQty = 0; bool found = false;
    int lo = (g_invContainerOff >= 0) ? g_invContainerOff : INV_SCAN_OFF_LO;
    int hi = (g_invContainerOff >= 0) ? g_invContainerOff : (INV_SCAN_OFF_HI - 0x60);
    for (int off = lo; off <= hi && !found; off += 8) {
        uintptr_t cont = pgd + off; int32_t slotCount = 0;
        if (!SafeRead(cont + INV_SLOTCOUNT_OFF, slotCount)) continue;
        if (slotCount <= 0 || slotCount > INV_MAX_SLOTS) continue;
        uintptr_t primary = 0, overflow = 0;
        if (!SafeRead(cont + INV_PRIMARY_PTR_OFF, primary) || primary < 0x10000) continue;
        if ((primary & 0x3) != 0) continue;
        SafeRead(cont + INV_OVERFLOW_PTR_OFF, overflow);
        const uintptr_t arrays[2] = { primary, overflow };
        const int       counts[2] = { slotCount, INV_MAX_SLOTS };
        for (int a = 0; a < 2 && !found; ++a) {
            uintptr_t arr = arrays[a]; if (arr < 0x10000) continue;
            size_t okEntries = SafeRegionLen(arr, (size_t)counts[a] * INV_ENTRY_STRIDE) / INV_ENTRY_STRIDE;
            for (size_t i = 0; i < okEntries; ++i) {
                const uint8_t* e = reinterpret_cast<const uint8_t*>(arr + i * INV_ENTRY_STRIDE);
                int32_t itemId = 0, qty = 0;
                if (!SehReadI32(e + INV_ENTRY_ID_OFF, itemId)) break;   // array freed mid-walk -> bail
                if (static_cast<uint32_t>(itemId) != kFragFull) continue;
                if (!SehReadI32(e + INV_ENTRY_QTY_OFF, qty)) break;
                if (qty < 0) qty = 0; if (qty > 0x270F) qty = 0x270F;
                fragQty = qty; found = true;
                if (g_invContainerOff < 0) g_invContainerOff = off;   // cache the bag
                break;
            }
        }
    }
    // No fragment stack found this tick -> hold 0; never write a transient 0 (avoids flicker if the
    // walk raced a realloc). Once you hold >=1 fragment, found==true and we set the level.
    if (!found) return;

    int level = 0;
    for (int L = 20; L >= 0; --L) { if (fragQty >= kScaduCum[L]) { level = L; break; } }

    uint8_t cur = 0;
    if (!SafeRead(pgd + kScaduCombatLevelOff, cur)) return;
    // Only ever RAISE the stored level: never stomp a real DLC revere, never down-flicker.
    if (static_cast<int>(cur) >= level) return;
    if (SehWriteU8(reinterpret_cast<void*>(pgd + kScaduCombatLevelOff), static_cast<uint8_t>(level)))
        spdlog::info("global_scadu_blessing: frags={} -> blessing level {} (PGD+0xFC, was {})",
                     fragQty, level, static_cast<int>(cur));
}

}} // namespace er_ap::game
#endif // _WIN32
