// er_singletons.h — FD4 singleton resolution for Elden Ring 2.6.2.0
// (sha256 34102b1c08bb5f769a724427a6f70fe29b3b732c31cf73693f861c48d3492ddb, the ~1.16.1-era exe
// the client targets). Replaces the DS3 client's fd4_singleton finder, which does not port.
//
// All key managers share ONE uniform GET_SINGLETON accessor shape on this build:
//
//     sub  rsp, 0x28
//     mov  rax, [rip+SLOT]          ; 48 8B 05 <disp32>   <- cached instance pointer (BSS slot)
//     test rax, rax                 ; 48 85 C0
//     jne  ret                      ; 75 05
//     call <singleton_init>         ; E8 <disp32>         (target unique per singleton)
//     lea  rcx, [rip+CLASSNAME]     ; 48 8D 0D <disp32>   -> class-name string in .rdata
//     mov  [rax+0x38], rcx          ; 48 89 48 38         (descriptor stores name at +0x38)
//   ret: ...
//
// RUNTIME RESOLUTION (preferred — name-string-anchored, resilient to small patch shifts):
//     1. find the null-terminated class-name string in .rdata (table below);
//     2. find the single code LEA that references it (scan .text for a 48 8D /r rip-relative
//        instruction whose target == className RVA);
//     3. the instance-slot load sits a fixed 0x11 bytes before that LEA:
//            uint32_t slot_mov_rva = name_lea_rva - ACCESSOR_SLOT_BACKSTEP;   // 48 8B 05 <disp32>
//            uint32_t slot_rva     = slot_mov_rva + 7 + read_i32(slot_mov_rva + SLOT_MOV_DISP_OFF);
//     4. void* instance = *(void**)(module_base + slot_rva);   // null until the singleton inits.
//
// The slot RVAs below are the resolved result for THIS exe — use directly when pinning to 2.6.2.0,
// otherwise resolve at runtime via the steps above. Slots live in .data BSS (zero on disk).
// Each resolved INSTANCE still needs its own internal offsets to reach useful data (e.g.
// SoloParamRepository -> ParamResCap list -> ParamHeader -> row); those are tracked separately.
#pragma once
#include <cstdint>

namespace er_ap { namespace fd4 {

struct SingletonInfo {
    const char* className;
    uint32_t    classNameRva;     // .rdata
    uint32_t    instanceSlotRva;  // .data BSS; *(module_base + slot) == instance (null until init)
};

// Resolved on eldenring.exe 2.6.2.0. ImageBase 0x140000000.
constexpr SingletonInfo kSoloParamRepository = { "SoloParamRepository", 0x02BB26A0u, 0x03D81F08u };
constexpr SingletonInfo kCSRegulationManager = { "CSRegulationManager", 0x02BD88D0u, 0x03D86C68u };
constexpr SingletonInfo kCSEventFlagMan      = { "CSEventFlagMan",      0x02A6CEE8u, 0x03D68458u };

// notify v2 Task B (SPEC-notify-banner.md). slotRva = name-anchored ground truth (the client's FD4
// resolver in er_gamehook_win.cpp reads the actual accessor at runtime); these confirmed the Hexinton
// CE-table-derived guesses were ~0x28-0x50 off, so trust these.
constexpr SingletonInfo kCSItemGetMenuMan    = { "CSItemGetMenuMan",    0x02A9E120u, 0x03D6C3D8u };  // native item-acquired popup (Phase 3 suppress target)
constexpr SingletonInfo kCSMenuMan           = { "CSMenuMan",           0x02A9BAA8u, 0x03D6B800u };
// MsgRepository (Phase 2 FMG text) is NOT resolvable via the uniform accessor shape — its name string
// (RVA 0x029DA758) is referenced from many non-accessor sites, so name-anchoring is ambiguous and the
// CE-derived slot (~0x03D7D4F8) is UNCONFIRMED. Phase 2 needs a dedicated AOB on its accessor.

// WorldChrMan: class-name string @ RVA 0x02A4A8C0, but its accessor is not the uniform shape in the
//   immediate window — resolve separately (secondary: player / HP, not on the item-decode path).
// GameDataMan: no class-name string present -> classic AOB-resolved global, as in the DS3 client.

constexpr int ACCESSOR_SLOT_BACKSTEP = 0x11;  // bytes from the name-LEA back to the slot mov (uniform)
constexpr int SLOT_MOV_DISP_OFF      = 3;     // disp32 offset within the 7-byte (48 8B 05 ..) slot mov

}} // namespace er_ap::fd4
