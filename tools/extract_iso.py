#!/usr/bin/env python3
"""Extrae una ISO9660 (UMD de PSP) sin depender de 7z/xorriso."""
import struct
import sys
from pathlib import Path

SECTOR = 2048


def read_dir(f, lba, size):
    f.seek(lba * SECTOR)
    data = f.read(size)
    i = 0
    while i < len(data):
        length = data[i]
        if length == 0:
            i = (i // SECTOR + 1) * SECTOR
            continue
        rec = data[i:i + length]
        i += length
        name = rec[33:33 + rec[32]]
        if name in (b"\x00", b"\x01"):
            continue
        yield (name.decode("latin1").split(";")[0],
               struct.unpack("<I", rec[2:6])[0],
               struct.unpack("<I", rec[10:14])[0],
               bool(rec[25] & 2))


def extract(f, lba, size, out):
    out.mkdir(parents=True, exist_ok=True)
    for name, elba, esize, is_dir in read_dir(f, lba, size):
        target = out / name
        if is_dir:
            extract(f, elba, esize, target)
            continue
        f.seek(elba * SECTOR)
        with open(target, "wb") as o:
            remaining = esize
            while remaining:
                chunk = f.read(min(remaining, 1 << 20))
                o.write(chunk)
                remaining -= len(chunk)


def main():
    if len(sys.argv) != 3:
        sys.exit(f"uso: {sys.argv[0]} <imagen.iso> <directorio_salida>")
    with open(sys.argv[1], "rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[1:6] != b"CD001":
            sys.exit("no es una ISO9660 válida")
        root = pvd[156:190]
        extract(f, struct.unpack("<I", root[2:6])[0],
                struct.unpack("<I", root[10:14])[0], Path(sys.argv[2]))


if __name__ == "__main__":
    main()
