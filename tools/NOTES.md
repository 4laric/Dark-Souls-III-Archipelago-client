# ER runtime-client — RE notes & provenance

How the addresses, offsets, and the encoding contract used by the client were derived and validated.
Everything here is build-pinned and cross-checked; re-resolve via AOB (not the literal RVAs) for any
other game patch.

## Build target

| | |
|---|---|
| Binary | `eldenring.exe` 2.6.2.0 (~patch 1.16.1-era) |
| SHA-256 | `34102b1c08bb5f769a724427a6f70fe29b3b732c31cf73693f861c48d3492ddb` |
| ImageBase | `0x140000000` |
| Primary `.text` | RVA `0x1000`, ~43 MB, entropy 6.96 — readable x64 (statically scannable) |

## Sources

- **Paramdex** (`soulsmods/Paramdex`, `ER/Defs/EquipParamGoods.xml`, FormatVersion 203) — typed field
  layout for `EQUIP_PARAM_GOODS_ST`. Used to compute byte offsets; see `compute_offsets.py`.
- **Community CE table** — *Hexinton all-in-one v5.0 (CE 7.5)*. Source of `AddItemFunc`,
  `InventoryAccessor`, the `ParamBase` walk, and the event-flag function. **Not committed** (third-party);
  every signature lifted from it was confirmed to match this exe **uniquely**.
- **Static analysis of the exe** — the FD4 singleton accessor pattern and instance-pointer slots
  (`er_singletons.h`), and validation scans (`xcheck`-style).

## Resolved addresses (build 2.6.2.0)

Authoritative copies live in `er_hooks.h` / `er_singletons.h`; this is the summary.

| Symbol | RVA | Kind | Use |
|---|---|---|---|
| `AddItemFunc` | `0x005605B0` | function | pickup-detection hook **and** item grant |
| `InventoryAccessor` | `0x005AB620` | function | resolves the inventory instance |
| inventory ptr-loc | `0x03D67A50` | .data | `*loc` = inventory instance (AddItemFunc rcx) |
| `ParamBase` ptr-loc | `0x03D81EE8` | .data | `*loc` = param-repository instance |
| EventFlag set/get | `0x005D2110` | function | unused under goods-only (see below) |
| `SoloParamRepository` slot | `0x03D81F08` | .data BSS | FD4 instance ptr (name str `0x02BB26A0`) |
| `CSRegulationManager` slot | `0x03D86C68` | .data BSS | FD4 instance ptr (name str `0x02BD88D0`) |
| `CSEventFlagMan` slot | `0x03D68458` | .data BSS | FD4 instance ptr (name str `0x02A6CEE8`) |

The CE `ParamBase` (`0x03D81EE8`) and `CSRegulationManagerImp` (`0x03D86C58`) pointers sit 0x20 / 0x10
from the statically-resolved `SoloParamRepository` / `CSRegulationManager` slots — same `.data` regions,
mutually corroborating.

### FD4 singleton resolution (version-resilient)
All key managers share one accessor shape. Resolve by name: find the class-name string in `.rdata`,
find its single code `LEA`, then the instance-slot `mov` is a fixed `0x11` bytes earlier:
`slot_rva = (lea_rva - 0x11) + 7 + i32((lea_rva - 0x11) + 3)`.

## Param read chain (synthetic id → goods row)

```
repo  = *(module + 0x03D81EE8)                       // param-repository instance
hdr   = *(repo + index*0x48 + 0x88)                  // index = 3 for EquipParamGoods (0 wep,1 armor,2 talisman)
blob  = *(hdr + 0x80)                                 // in-memory PARAM blob (normal params: one deref)
// PARAM row index (== on-disk; matches regulation_io.param_row_offsets):
rowCount = *(u16*)(blob + 0x0A)
entry[i] =  blob + 0x40 + 24*i                        // id @ +0 (s32), dataOffset @ +8 (lo32)
rowBase  =  blob + dataOffset(entry whose id == target)
```
Goods row stride = **176 (0xB0)**. Carrier-field offsets within the row (`er_goods_row.h`):
`basicPrice 0x10` · `sellValue 0x14` · `disableUseAtOutOfColiseum 0x4A` bit 5 (mask 0x20) ·
`vagrantItemLotId 0x54` · `vagrantBonusEneDropItemLotId 0x58`.

## Encoding / decode contract

- **Category nibble** (top 4 bits, `id & 0xF0000000`): weapon 0, armor 1, talisman/accessory 2,
  **goods 4 (`0x40000000`)**, gem 8. The client only ever emits goods.
- **Synthetic detection**: category == goods AND `(id & 0x0FFFFFFF) > 3,780,000` (bounds the max real
  vanilla goods id, 2,220,010).
- **Decode**: AP location id = `((long)(uint)vagrantItemLotId) | ((long)(uint)vagrantBonusEneDrop...) << 32`
  — the **unsigned** casts are load-bearing (both fields are signed s32; a naive widen corrupts bit-31
  halves). Local item = `basicPrice` (0 = none) × `sellValue`. Foreign-remove = `disableUseAtOutOfColiseum`.

## Cross-validation summary

- **Row layout (176 + field order)** agreed by three independent sources: Paramdex offsets,
  `regulation_io.param_row_offsets`, and the CE table's own `paramlength = dataOff(row1) − dataOff(row0)`.
- **Recombine** is bit-identical to spec-2's `vagrant_codec.py` across all 7 vectors incl. the bit-31
  corruption cases (`tests/reconcile_test`).
- **All CE signatures** matched this exe uniquely (`AddItemFunc`, `InventoryAccessor`, `ParamBase`,
  EventFlag).
- **Pure walk + decode** validated against a real-layout mock (`tests/walk_test`, 18/18).

## To confirm on first live run (flagged in `er_gamehook_win.cpp`)

1. The picked-up id sits at `entry + 0x04` in the descriptor the game passes to `AddItemFunc`
   (log `rawId` in the detour for a few pickups).
2. Returning `0` from the detour cleanly drops the placeholder.

## Event flags

Not required for the goods-only loop: a picked-up goods placeholder is consumed on pickup, so the
location is not re-offered, and the AP check is reported over the protocol. The set/get function and
`CSEventFlagMan` instance are resolved-but-unused; wire `SetEventFlag` only if some location type needs
an explicit collected-flag, and confirm the function signature first.

## Reproduction

```
python3 fetch_def.py        # fetch EquipParamGoods.xml from Paramdex
python3 compute_offsets.py  # recompute offsets; validates field order vs a vanilla CSV + row-size 176
```
Pointer/signature cross-checks against the exe were done with short capstone/pefile scripts; re-run
against a new `eldenring.exe` to re-resolve for other patches.

## Not committed here

The Hexinton `.ct` (third-party — referenced, not vendored), spec-2's `vagrant_codec.py` (only the
generated `tests/codec_vectors_generated.h` is committed), `eldenring.exe`, and the Nightreign toolkit.
