#!/usr/bin/env python3
"""Exporta de golpe todas las texturas del juego a PNG, con los mismos nombres que usa el port.

Uso: python3 tools/textures/export.py --iso "Disney-Pixar Cars (Spain).iso" [--out texdump]

Lee la ISO directamente (sin extraerla), descomprime los .DFF (contenedores Z01 de RenderWare Studio) y
decodifica cada textura nativa de PSP (TextureNative) exactamente como lo hace el runtime al dibujarla.
También incluye las imágenes PNG de USRDIR (pantallas de carga, postales...), que el juego usa tal cual.
Cada una se guarda como <hash>.png, donde hash = 8 primeros bytes del MD5 de (ancho, alto, RGBA): es el
mismo nombre con el que el port busca las sustituciones, así que basta con editar el PNG y copiarlo a
textures/ (F5 las recarga con el juego en marcha).

Además escribe index.csv (hash, fichero de origen, nombre de la textura en RenderWare, tamaño) para
encontrar cada textura por su nombre (p. ej. las de un coche).

Las texturas que el juego genera o modifica mientras corre (barras del HUD que cambian la paleta, vídeos,
render a textura) no están en los ficheros: para esas sigue sirviendo --dump-textures jugando.
"""
import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

SECTOR = 2048
LIB_37 = 0x1C02


# ---------------------------------------------------------------- ISO9660

def iso_files(f):
    """(ruta, lba, tamaño) de cada fichero de la ISO."""
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[1:6] != b"CD001":
        sys.exit("no es una imagen ISO9660")
    root = pvd[156:190]

    def walk(lba, size, prefix):
        f.seek(lba * SECTOR)
        data = f.read(size)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // SECTOR + 1) * SECTOR
                continue
            rec = data[i:i + n]
            i += n
            name = rec[33:33 + rec[32]]
            if name in (b"\0", b"\1"):
                continue
            path = prefix + name.decode("latin1").split(";")[0]
            elba, esize = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            if rec[25] & 2:
                yield from walk(elba, esize, path + "/")
            else:
                yield path, elba, esize

    yield from walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")


# ---------------------------------------------------------------- decodificación (igual que runtime/src/gpu/texture.cpp)

def c565(v):
    return bytes(((v & 0x1F) * 255 // 31, (v >> 5 & 0x3F) * 255 // 63, (v >> 11) * 255 // 31, 255))


def c5551(v):
    return bytes(((v & 0x1F) * 255 // 31, (v >> 5 & 0x1F) * 255 // 31, (v >> 10 & 0x1F) * 255 // 31,
                  255 if v >> 15 else 0))


def c4444(v):
    return bytes(((v & 0xF) * 17, (v >> 4 & 0xF) * 17, (v >> 8 & 0xF) * 17, (v >> 12) * 17))


def unswizzle(data, row_bytes, rows):
    """Bloques de 16 bytes x 8 filas -> orden lineal."""
    out = bytearray(row_bytes * rows)
    bx = row_bytes // 16
    i = 0
    for by in range(0, rows, 8):
        for x in range(bx):
            for r in range(8):
                o = (by + r) * row_bytes + x * 16
                out[o:o + 16] = data[i:i + 16]
                i += 16
    return out


def decode_native(st):
    """Struct de un TextureNative -> (nombre, ancho, alto, RGBA) o None si el formato no se reconoce."""
    if len(st) < 0xAC:
        return None
    fmt, = struct.unpack_from("<I", st, 0)
    w, h = struct.unpack_from("<HH", st, 4)
    bpp = st[8]
    if not (w and h and w <= 1024 and h <= 1024 and bpp in (4, 8, 16, 32)):
        return None
    name = st[0x6C:0x8C].split(b"\0")[0].decode("latin1")
    pos = 0xAC
    palette = None
    if bpp in (4, 8):
        n = 1 << bpp
        palette = [bytes(st[pos + k * 4:pos + k * 4 + 4]) for k in range(n)]
        pos += n * 4
    row_bytes = max(w * bpp // 8, 16)
    rows = (h + 7) // 8 * 8
    raw = bytes(st[pos:pos + row_bytes * rows]).ljust(row_bytes * rows, b"\0")
    lin = unswizzle(raw, row_bytes, rows)

    out = []
    kind = fmt >> 8 & 0xF
    for y in range(h):
        row = lin[y * row_bytes:(y + 1) * row_bytes]
        if bpp == 8:
            out.extend(palette[b] for b in row[:w])
        elif bpp == 4:
            for x in range(w):
                out.append(palette[row[x >> 1] >> (4 * (x & 1)) & 0xF])
        elif bpp == 16:
            conv = c4444 if kind == 0x3 else c5551 if kind == 0x1 else c565
            vals = struct.unpack_from(f"<{w}H", row)
            out.extend(conv(v) for v in vals)
        else:
            out.append(bytes(row[:w * 4]))
    return name, w, h, b"".join(out)


def content_hash(w, h, rgba):
    """Igual que texrep_content_hash() en runtime/src/gpu/texture_replace.cpp."""
    return hashlib.md5(struct.pack("<II", w, h) + rgba).digest()[:8].hex()


def find_textures(stream):
    """Busca todos los chunks TextureNative (0x15, RW 3.7) del stream, estén en el contenedor que estén."""
    i = 0
    needle = b"\x15\x00\x00\x00"
    while True:
        i = stream.find(needle, i)
        if i < 0 or i + 12 > len(stream):
            return
        size, lib = struct.unpack_from("<II", stream, i + 4)
        body = i + 12
        if lib >> 16 == LIB_37 and body + size <= len(stream) and body + 12 <= len(stream):
            stype, ssize, slib = struct.unpack_from("<III", stream, body)
            if stype == 1 and slib >> 16 == LIB_37 and ssize <= size:
                yield stream[body + 12:body + 12 + ssize]
                i = body + size
                continue
        i += 4


# ---------------------------------------------------------------- PNG mínimo (sin dependencias)

def read_png(data):
    """PNG de 8 bits (gris, RGB, paleta con tRNS, gris+alfa o RGBA) -> (ancho, alto, RGBA) o None.

    El juego decodifica sus PNG a RGBA tal cual (los píxeles transparentes conservan el color de la paleta),
    así que el resultado coincide con lo que acaba en la textura."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, plte, trns = 8, [], b"", b""
    while pos + 8 <= len(data):
        n, t = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif t == b"PLTE":
            plte = body
        elif t == b"tRNS":
            trns = body
        elif t == b"IDAT":
            idat.append(body)
        pos += 12 + n
    if depth != 8 or interlace or ctype not in (0, 2, 3, 4, 6):
        return None
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    prev = bytearray(stride)
    rows = []
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif f == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                b = prev[i]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(bytes(cur))
        prev = cur
    if ctype == 3:
        pal = [plte[i * 3:i * 3 + 3] + bytes((trns[i] if i < len(trns) else 255,)) for i in range(len(plte) // 3)]
        return w, h, b"".join(pal[v] for row in rows for v in row)
    if ctype == 6:
        return w, h, b"".join(rows)
    out = bytearray()
    for row in rows:
        for x in range(w):
            px = row[x * bpp:(x + 1) * bpp]
            out += (px * 3 + b"\xff") if ctype == 0 else (px[:1] * 3 + px[1:]) if ctype == 4 else px + b"\xff"
    return w, h, bytes(out)


def write_png(path, w, h, rgba):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    path.write_bytes(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--iso", required=True, type=Path)
    ap.add_argument("--out", type=Path, default=Path("texdump"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    seen = {}
    index = []
    with open(args.iso, "rb") as f:
        files = list(iso_files(f))
        # Imágenes PNG que el juego carga como texturas (pantallas de carga, postales, "¡YA!")
        for path, lba, size in files:
            if not (path.upper().startswith("PSP_GAME/USRDIR/") and path.upper().endswith(".PNG")):
                continue
            f.seek(lba * SECTOR)
            img = read_png(f.read(size))
            if not img:
                print(f"  {path}: formato PNG no soportado, se omite")
                continue
            w, h, rgba = img
            key = content_hash(w, h, rgba)
            index.append((key, path, Path(path).stem, w, h))
            if key not in seen:
                seen[key] = True
                write_png(args.out / f"{key}.png", w, h, rgba)
        dffs = [(p, lba, size) for p, lba, size in files if p.upper().endswith(".DFF")]
        for n, (path, lba, size) in enumerate(dffs, 1):
            f.seek(lba * SECTOR)
            data = f.read(size)
            if data[:4] == b"Z01\0":
                data = zlib.decompress(data[8:])
            for st in find_textures(data):
                tex = decode_native(st)
                if not tex:
                    continue
                name, w, h, rgba = tex
                key = content_hash(w, h, rgba)
                index.append((key, path, name, w, h))
                if key not in seen:
                    seen[key] = True
                    write_png(args.out / f"{key}.png", w, h, rgba)
            if sys.stdout.isatty():
                print(f"\r[{n}/{len(dffs)}] {len(seen)} texturas", end="", flush=True)
    if sys.stdout.isatty():
        print()
    with open(args.out / "index.csv", "w", encoding="utf-8") as idx:
        idx.write("hash,fichero,textura,ancho,alto\n")
        for row in sorted(set(index), key=lambda r: (r[1], r[2])):
            idx.write(",".join(str(c) for c in row) + "\n")
    print(f"{len(seen)} texturas distintas en {args.out} (índice: {args.out / 'index.csv'})")


if __name__ == "__main__":
    main()
