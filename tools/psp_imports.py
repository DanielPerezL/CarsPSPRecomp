#!/usr/bin/env python3
"""Resuelve los imports (stubs por NID) de un EBOOT descifrado a nombres de función.

Los nombres salen de las tablas HLE de PPSSPP (third_party/ppsspp/Core/HLE).
Salida CSV: dirección (con la base indicada), librería, NID, nombre.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

NID_ENTRY = re.compile(r'\{\s*0[xX]([0-9A-Fa-f]{8})\s*,[^"\n]*"(\w+)"')


def load_nids(hle_dir):
    nids = {}
    for f in sorted(Path(hle_dir).glob("*.cpp")):
        for m in NID_ENTRY.finditer(f.read_text(errors="replace")):
            nids.setdefault(int(m.group(1), 16), m.group(2))
    return nids


def sections(elf):
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
    hdrs = [struct.unpack_from("<IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx][4]
    out = {}
    for h in hdrs:
        name = elf[strtab + h[0]:elf.index(b"\0", strtab + h[0])].decode()
        out[name] = (h[3], h[4], h[5])  # addr, offset, size
    return out


def imports(elf):
    secs = sections(elf)
    text_addr, text_off, _ = secs[".text"]

    def at(addr, fmt):
        return struct.unpack_from(fmt, elf, addr - text_addr + text_off)

    def cstr(addr):
        o = addr - text_addr + text_off
        return elf[o:elf.index(b"\0", o)].decode()

    addr, _, size = secs[".lib.stub"]
    end = addr + size
    while addr < end:
        name_ptr, _ver, _attr, ent_len, _vcount, fcount, nid_tab, stub_tab = at(addr, "<IHHBBHII")
        lib = cstr(name_ptr) if name_ptr else "?"
        for i in range(fcount):
            yield stub_tab + i * 8, lib, at(nid_tab + i * 4, "<I")[0]
        addr += ent_len * 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", type=Path)
    ap.add_argument("--hle", type=Path, default=Path("third_party/ppsspp/Core/HLE"))
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x08804000)
    args = ap.parse_args()

    nids = load_nids(args.hle)
    elf = args.elf.read_bytes()
    total = unresolved = 0
    for stub, lib, nid in imports(elf):
        total += 1
        name = nids.get(nid)
        if name is None:
            unresolved += 1
            name = f"{lib}_{nid:08X}"
        print(f"{stub + args.base:08X},{lib},{nid:08X},{name}")
    print(f"{total} imports, {unresolved} sin resolver", file=sys.stderr)


if __name__ == "__main__":
    main()
