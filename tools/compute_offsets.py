#!/usr/bin/env python3
"""Recompute EquipParamGoods byte offsets from the Paramdex def and validate the row size.

Usage:
    python3 compute_offsets.py [EquipParamGoods.xml] [--csv vanilla_EquipParamGoods.csv]

With no path it reads EquipParamGoods.xml next to this script (run fetch_def.py to obtain it).
The optional CSV is a Smithbox vanilla EquipParamGoods dump; it cross-checks field ORDER against
your exact game version. The offset computation itself follows SoulsFormats PARAMDEF rules
(contiguous packing; dummy8 bitfields normalize to u8 storage; no implicit C alignment).
"""
import os, re, sys, csv
import xml.etree.ElementTree as ET

BASE = {'s8':1,'u8':1,'dummy8':1,'s16':2,'u16':2,'s32':4,'u32':4,'f32':4,'f64':8,'fixstr':1,'fixstrw':1}

def parse_def(d):
    d = d.split('=')[0].strip()                       # drop " = default"
    m = re.match(r'^([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)(?:\[(\d+)\])?(?::(\d+))?$', d)
    if not m:
        raise ValueError(f'unparsed Def: {d!r}')
    t, name, arr, bits = m.group(1).lower(), m.group(2), m.group(3), m.group(4)
    return t, name, (int(arr) if arr else None), (int(bits) if bits else None)

def compute(def_path):
    fields = [f.attrib['Def'] for f in ET.parse(def_path).getroot().find('Fields')]
    off = 0; uo = 0; uopen = False; utype = None; bit = 0
    rows = []
    for i, d in enumerate(fields):
        t, name, arr, bits = parse_def(d)
        if bits is None:                              # non-bitfield: flush any open bit unit
            if uopen:
                off = uo + BASE[utype]; uopen = False
            rows.append((i, name, t, off, None, None, None))
            off += BASE[t] * (arr or 1)
        else:                                         # bitfield (dummy8 -> u8 storage)
            st = 'u8' if t == 'dummy8' else t
            sbits = BASE[st] * 8
            if (not uopen) or st != utype or bit + bits > sbits:
                if uopen:
                    off = uo + BASE[utype]
                uo = off; uopen = True; utype = st; bit = 0
            rows.append((i, name, t, uo, bits, bit, ((1 << bits) - 1) << bit))
            bit += bits
    if uopen:
        off = uo + BASE[utype]
    return rows, off, [parse_def(d)[1] for d in fields]

def main():
    pos = [a for a in sys.argv[1:] if not a.startswith('--')]
    def_path = pos[0] if pos else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'EquipParamGoods.xml')
    csv_path = sys.argv[sys.argv.index('--csv') + 1] if '--csv' in sys.argv else None

    rows, row_size, names = compute(def_path)
    by = {r[1]: r for r in rows}
    targets = ['basicPrice', 'sellValue', 'disableUseAtColiseum', 'disableUseAtOutOfColiseum',
               'vagrantItemLotId', 'vagrantBonusEneDropItemLotId']

    print(f"row size: {row_size} (0x{row_size:X})  -> " +
          ("OK (canonical ER goods row stride)" if row_size == 176 else "UNEXPECTED (expected 176)"))
    for n in targets:
        r = by[n]
        if r[4] is None:
            print(f"  {n:30s} 0x{r[3]:03X} ({r[3]:>3d})  {r[2]}")
        else:
            print(f"  {n:30s} 0x{r[3]:03X} bit {r[5]} mask 0x{r[6]:02X}  ({r[2]}:{r[4]})")

    if csv_path:
        with open(csv_path, newline='') as fh:
            header = [h for h in next(csv.reader(fh)) if h.strip()]
        csvf = header[2:]                             # drop ID, Name
        diffs = [(i, a, b) for i, (a, b) in enumerate(zip(names, csvf)) if a != b]
        vagrant_ord = names.index('vagrantBonusEneDropItemLotId')
        pre = [d for d in diffs if d[0] <= vagrant_ord]
        print(f"CSV field-order check: {len(diffs)} name diffs total; "
              f"{len(pre)} before vagrant (these MUST be type-preserving): {pre[:8]}")

    sys.exit(0 if row_size == 176 else 1)

if __name__ == '__main__':
    main()
