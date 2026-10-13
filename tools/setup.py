#!/usr/bin/env python3
"""Instalación completa del port a partir de la ISO propia del jugador.

Uso: python3 tools/setup.py --iso "Disney-Pixar Cars (Spain).iso" [-j N]

1. Prepara las dependencias en third_party/ (versiones fijadas y comprobadas):
   - rabbitizer (desensamblador MIPS/Allegrex, wheel de PyPI)
   - pspdecrypt (descifrado del EBOOT; se compila con make, requiere zlib y OpenSSL)
2. Extrae PSP_GAME/SYSDIR/EBOOT.BIN de la ISO y comprueba que es la versión soportada.
3. Lo descifra, lo recompila a C (build/gen) y compila el runtime (build/cars).

Nada del juego sale de la máquina del usuario ni entra en el repositorio.
"""
import argparse
import hashlib
import io
import os
import struct
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD = ROOT / "third_party"
BUILD = ROOT / "build"

GAME = "ULES00324"
EBOOT_SHA256 = "5b2d1a56d6ace42f7fd5f0dbf76f84126e104b0cedc2b70ae90c8bb91d5ca7e6"
ELF_SHA256 = "bfe93c09e17e74e4b411f59a9f15b68204fc1073dc8a362e6b9343af49cc5d0a"

RABBITIZER_URL = ("https://files.pythonhosted.org/packages/0a/d8/0e8e996904e7295f968512281cd7079c80c39f7d4f51c3956e1669ef6cfa/"
                  "rabbitizer-1.16.2-cp34-abi3-manylinux1_x86_64.manylinux_2_28_x86_64.manylinux_2_5_x86_64.whl")
RABBITIZER_SHA256 = "eafe91e07c9f400d2abc44c029c3b380cb68e65d2c32eb7e2015d391a51d2182"
PSPDECRYPT_REPO = "https://github.com/John-K/pspdecrypt"
PSPDECRYPT_COMMIT = "c156627db7634d395c380c0a9589130f603307fc"


def step(msg):
    print(f"\n== {msg}")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def run(cmd, **kw):
    print("  $", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def ensure_rabbitizer():
    if (THIRD / "pylib" / "rabbitizer").exists():
        return
    step("Descargando rabbitizer")
    data = urllib.request.urlopen(RABBITIZER_URL).read()
    if sha256(data) != RABBITIZER_SHA256:
        sys.exit("el wheel de rabbitizer no coincide con el hash esperado")
    zipfile.ZipFile(io.BytesIO(data)).extractall(THIRD / "pylib")


def ensure_pspdecrypt(jobs):
    tool = THIRD / "pspdecrypt" / "pspdecrypt"
    if tool.exists():
        return tool
    step("Compilando pspdecrypt")
    src = THIRD / "pspdecrypt"
    if not src.exists():
        run(["git", "clone", "--recurse-submodules", PSPDECRYPT_REPO, src])
    run(["git", "-C", src, "checkout", PSPDECRYPT_COMMIT])
    run(["make", "-C", src, f"-j{jobs}", "CC=gcc", "CXX=g++"])
    return tool


def extract_eboot(iso_path):
    """Lee PSP_GAME/SYSDIR/EBOOT.BIN directamente de la ISO9660."""
    with open(iso_path, "rb") as f:
        def read(lba, n):
            f.seek(lba * 2048)
            return f.read(n)

        pvd = read(16, 2048)
        if pvd[1:6] != b"CD001":
            sys.exit(f"{iso_path} no es una imagen ISO9660")
        lba, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        for comp in ("PSP_GAME", "SYSDIR", "EBOOT.BIN"):
            data, i, found = read(lba, size), 0, None
            while i < len(data):
                n = data[i]
                if n == 0:
                    i = (i // 2048 + 1) * 2048
                    continue
                name = data[i + 33:i + 33 + data[i + 32]].decode("latin1").split(";")[0]
                if name.upper() == comp:
                    found = struct.unpack_from("<I", data, i + 2)[0], struct.unpack_from("<I", data, i + 10)[0]
                    break
                i += n
            if not found:
                sys.exit(f"no se encuentra {comp} en la ISO")
            lba, size = found
        return read(lba, size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True, type=Path)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    ensure_rabbitizer()
    pspdecrypt = ensure_pspdecrypt(args.j)
    BUILD.mkdir(exist_ok=True)

    step("Extrayendo el ejecutable de la ISO")
    eboot = extract_eboot(args.iso)
    if sha256(eboot) != EBOOT_SHA256:
        sys.exit(f"EBOOT.BIN no reconocido (sha256 {sha256(eboot)}). Versión soportada: {GAME} (España).")
    (BUILD / "EBOOT.BIN").write_bytes(eboot)

    step("Descifrando")
    run([pspdecrypt, BUILD / "EBOOT.BIN", "-o", BUILD / "EBOOT.elf"])
    if sha256((BUILD / "EBOOT.elf").read_bytes()) != ELF_SHA256:
        sys.exit("el EBOOT descifrado no coincide con el esperado")

    step("Recompilando a C")
    run([sys.executable, ROOT / "tools/recomp/recomp.py", BUILD / "EBOOT.elf",
         ROOT / f"tools/recomp/games/{GAME}.funcs.csv", "--out", BUILD / "gen"])

    step("Compilando el runtime")
    run(["make", "-C", ROOT / "runtime", f"-j{args.j}"])

    print(f"\nListo. Para jugar:\n  {BUILD / 'cars'} --iso \"{args.iso}\"")


if __name__ == "__main__":
    main()
