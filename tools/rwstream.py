#!/usr/bin/env python3
"""Descomprime los contenedores Z01 (.DFF de Cars PSP) y hace inventario de chunks RenderWare.

Formato Z01: b"Z01\\0" + u32 LE tamaño descomprimido + stream zlib.
Dentro: stream de RenderWare Studio (chunks RW 3.6.0.2):
  0x71C  lista de clases de comportamiento (BLevelLoader, BHud...)
  0x716  asset: nombre, GUID, tipo (rwID_*/PCPL_*), ruta de origen y,
         detrás, el chunk RW nativo del asset (RW 3.7.0.2, lib id 0x1C02xxxx)
  0x704  entidades/atributos de RW Studio
"""
import argparse
import collections
import re
import struct
import zlib
from pathlib import Path

KNOWN = {
    0x01: "Struct", 0x02: "String", 0x03: "Extension", 0x06: "Texture",
    0x07: "Material", 0x08: "MaterialList", 0x0B: "World", 0x0E: "FrameList", 0x0F: "Geometry",
    0x10: "Clump", 0x14: "Atomic", 0x15: "TextureNative", 0x16: "TexDictionary",
    0x1A: "GeometryList", 0x1B: "AnimAnimation", 0x2B: "UVAnimDict",
    0x704: "RWS_Entity", 0x716: "RWS_Asset", 0x71C: "RWS_ClassList",
}


def unpack(data):
    if data[:4] != b"Z01\x00":
        return data
    size = struct.unpack("<I", data[4:8])[0]
    out = zlib.decompress(data[8:])
    if len(out) != size:
        raise ValueError(f"tamaño {len(out)} != {size}")
    return out


def rw_version(lib_id):
    if lib_id & 0xFFFF0000:
        ver = ((lib_id >> 14) & 0x3FF00) + 0x30000 | (lib_id >> 16) & 0x3F
        return f"{ver >> 16}.{(ver >> 12) & 0xF}.{(ver >> 8) & 0xF}.{ver & 0xFF}"
    return f"{lib_id >> 8}.{lib_id & 0xFF}"


def chunks(data):
    off = 0
    while off + 12 <= len(data):
        ctype, size, lib = struct.unpack_from("<III", data, off)
        if off + 12 + size > len(data):
            break
        yield off, ctype, size, lib
        off += 12 + size


ASSET_TYPE = re.compile(rb"((?:rwID|PCPL)_[A-Z0-9_]+)\x00")


def asset_info(body):
    """Devuelve (tipo de asset, tipo del chunk RW nativo o None) de un chunk 0x716."""
    m = ASSET_TYPE.search(body, 0, 256)
    kind = m.group(1).decode() if m else "?"
    start = (m.end() + 3) & ~3 if m else 0
    for i in range(start, len(body) - 12, 4):
        ctype, size, lib = struct.unpack_from("<III", body, i)
        if lib >> 16 == 0x1C02 and ctype < 0x1000 and i + 12 + size <= len(body):
            return kind, ctype
    return kind, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--out", type=Path, help="directorio donde volcar los .rws descomprimidos")
    args = ap.parse_args()

    totals = collections.Counter()
    versions = collections.Counter()
    assets = collections.Counter()
    for path in args.files:
        data = unpack(path.read_bytes())
        if args.out:
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / (path.stem + ".rws")).write_bytes(data)
        for off, ctype, size, lib in chunks(data):
            totals[ctype] += 1
            versions[rw_version(lib)] += 1
            if ctype == 0x716:
                assets[asset_info(data[off + 12:off + 12 + size])] += 1

    print("Versiones RW:", dict(versions))
    for ctype, n in totals.most_common():
        print(f"  0x{ctype:04X} {KNOWN.get(ctype, '?'):16} {n}")
    print("Assets (0x716) por tipo -> chunk RW nativo:")
    for (kind, native), n in assets.most_common():
        name = "-" if native is None else f"0x{native:X} {KNOWN.get(native, '?')}"
        print(f"  {n:5} {kind:34} {name}")


if __name__ == "__main__":
    main()
