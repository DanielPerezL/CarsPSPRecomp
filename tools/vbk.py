#!/usr/bin/env python3
"""Extrae los bancos de sonido .VBK de Cars PSP a WAV.

El índice de cada banco (.VBI) no está en disco: va embebido en los streams .DFF
como asset PCPL_SOUNDBANKVBI[_VOLATILE]. Tras la ruta de origen hay
u32 4, 4 bytes, u32 0, u32 tamaño de tabla y entradas de 32 bytes:
    GUID (16) | u32 offset en el VBK | f32 frecuencia | u32 tamaño | u32 flags (1 = bucle)
El .VBK es la concatenación de las muestras en PS-ADPCM (VAG sin cabecera), mono.
"""
import argparse
import re
import struct
import wave
from pathlib import Path

from rwstream import chunks, unpack

VBI_TYPE = re.compile(rb"PCPL_SOUNDBANKVBI(?:_VOLATILE)?\x00")
ADPCM_COEFS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


def vbi_tables(stream):
    for off, ctype, size, _ in chunks(stream):
        body = stream[off + 12:off + 12 + size]
        if ctype != 0x716 or not VBI_TYPE.search(body, 0, 256):
            continue
        name = body[8:body.index(b"\x00", 8)].decode()
        pos = (body.index(b".VBI\x00") + 5 + 3) & ~3
        table_size = struct.unpack_from("<I", body, pos + 12)[0]
        table = body[pos + 16:pos + 16 + table_size]
        yield name, [struct.unpack_from("<16sIfII", table, k) for k in range(0, table_size - 31, 32)]


def decode_ps_adpcm(data):
    out = bytearray()
    h1 = h2 = 0
    for f in range(0, len(data) - 15, 16):
        shift, predictor = data[f] & 0x0F, min(data[f] >> 4, 4)
        flags = data[f + 1]
        c1, c2 = ADPCM_COEFS[predictor]
        for byte in data[f + 2:f + 16]:
            for nib in (byte & 0x0F, byte >> 4):
                s = (nib - 16 if nib & 8 else nib) << 12
                s = (s >> shift) + ((h1 * c1 + h2 * c2 + 32) >> 6)
                s = max(-32768, min(32767, s))
                out += struct.pack("<h", s)
                h2, h1 = h1, s
        if flags & 1 and not flags & 2:  # fin sin bucle
            break
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("streams", nargs="+", type=Path, help=".DFF del juego (Z01) o .rws ya descomprimidos")
    ap.add_argument("--vbk-dir", type=Path, default=Path("extracted/PSP_GAME/USRDIR/AUDIO/VBK"))
    ap.add_argument("--out", type=Path, default=Path("dumps/audio"))
    args = ap.parse_args()

    done = set()
    for path in args.streams:
        for name, entries in vbi_tables(unpack(path.read_bytes())):
            if name in done:
                continue
            done.add(name)
            bank = (args.vbk_dir / f"{name}.VBK").read_bytes()
            dest = args.out / name
            dest.mkdir(parents=True, exist_ok=True)
            for i, (guid, offset, rate, size, flags) in enumerate(entries):
                pcm = decode_ps_adpcm(bank[offset:offset + size])
                suffix = "_loop" if flags & 1 else ""
                with wave.open(str(dest / f"{i:03d}_{guid.hex()[:8]}{suffix}.wav"), "wb") as w:
                    w.setnchannels(1)
                    w.setsampwidth(2)
                    w.setframerate(int(rate))
                    w.writeframes(pcm)
            print(f"{name}: {len(entries)} muestras")
    print(f"{len(done)} bancos extraídos en {args.out}")


if __name__ == "__main__":
    main()
