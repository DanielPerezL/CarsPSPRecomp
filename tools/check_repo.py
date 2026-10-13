#!/usr/bin/env python3
"""Comprueba que no entra en el repositorio nada del juego (Disney/THQ) ni binarios de Sony.

Uso:
  python3 tools/check_repo.py            # ficheros preparados para el commit (hook pre-commit)
  python3 tools/check_repo.py --all      # todos los ficheros versionados
  python3 tools/check_repo.py --history  # todos los ficheros de todo el historial (antes de publicar)

Activar el hook una vez por clon:  git config core.hooksPath .githooks

Reglas: solo se admiten ficheros de texto; ninguna ruta de las carpetas de material del juego; ninguna
extensión de formatos de la PSP o del juego; ninguna firma conocida (ISO9660, ELF/PRX, PSMF, RIFF/AT3,
contenedores Z01 de RenderWare Studio...); tamaño máximo por fichero salvo cabeceras de terceros listadas.
"""
import subprocess
import sys

FORBIDDEN_DIRS = ("extracted/", "decrypted/", "dumps/", "ghidra/", "build/", "textures/", "texdump/",
                  "saves/", "third_party/")
ALLOWED_DIRS = ("runtime/third_party/",)  # cabeceras de terceros con licencia libre (stb, Khronos)

FORBIDDEN_EXT = {
    # imágenes de disco y ejecutables de la PSP
    ".iso", ".cso", ".pbp", ".elf", ".prx", ".bin", ".sfo",
    # formatos del juego
    ".dff", ".txd", ".rws", ".rwt", ".vbk", ".at3", ".pmf", ".pgf", ".tbl", ".str",
    # derivados (volcados, conversiones, capturas, audio y vídeo)
    ".glb", ".gltf", ".png", ".jpg", ".jpeg", ".ppm", ".bmp", ".tga", ".dds", ".wav", ".ogg", ".mp3",
    ".flac", ".mp4", ".mkv", ".webm", ".h264", ".gzf", ".rep", ".sav",
}

SIGNATURES = [
    (b"CD001", "imagen ISO9660"),
    (b"\x7fELF", "ejecutable ELF/PRX"),
    (b"~PSP", "módulo PSP cifrado"),
    (b"\x00PBP", "EBOOT.PBP"),
    (b"PSMF", "vídeo PSMF"),
    (b"RIFF", "audio RIFF/AT3"),
    (b"Z01", "contenedor Z01 de RenderWare Studio"),
    (b"\x89PNG", "imagen PNG"),
]

MAX_SIZE = 300 * 1024  # el mayor fichero propio (ULES00324.funcs.csv) ronda los 270 KB


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def entries(mode):
    """(ruta, contenido) de cada fichero a comprobar."""
    if mode == "--history":
        seen = set()
        for line in git("rev-list", "--objects", "--all").decode().splitlines():
            obj, _, path = line.partition(" ")
            if path and obj not in seen and git("cat-file", "-t", obj).strip() == b"blob":
                seen.add(obj)
                yield path, git("cat-file", "blob", obj)
        return
    if mode == "--all":  # versión en disco de los ficheros versionados
        for p in filter(None, git("ls-files", "-z").decode().split("\0")):
            with open(p, "rb") as f:
                yield p, f.read()
        return
    # versión preparada (índice), que es la que entrará en el commit
    for p in filter(None, git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").decode().split("\0")):
        yield p, git("cat-file", "blob", ":" + p)


def problems(path, data):
    low = path.lower()
    if low.startswith(ALLOWED_DIRS):
        return
    for d in FORBIDDEN_DIRS:  # carpetas de la raíz del repo, donde las crean las herramientas
        if low.startswith(d):
            yield f"está en {d} (material del juego o generado a partir de él)"
    ext = low[low.rfind("."):] if "." in low.rsplit("/", 1)[-1] else ""
    if ext in FORBIDDEN_EXT:
        yield f"extensión {ext} no permitida"
    if b"\0" in data:
        yield "es binario (solo se admite texto)"
    for sig, what in SIGNATURES:
        if data.startswith(sig) or (sig == b"CD001" and data[0x8001:0x8006] == sig):
            yield f"firma de {what}"
    if len(data) > MAX_SIZE:
        yield f"ocupa {len(data) // 1024} KB (máximo {MAX_SIZE // 1024} KB)"


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--staged"
    bad = 0
    for path, data in entries(mode):
        for msg in problems(path, data):
            print(f"✗ {path}: {msg}", file=sys.stderr)
            bad += 1
    if bad:
        print("\nNada del juego puede entrar en el repositorio. Si es un falso positivo, revisa "
              "tools/check_repo.py.", file=sys.stderr)
        sys.exit(1)
    if mode != "--staged":
        print("OK: ningún fichero del juego ni binario")


if __name__ == "__main__":
    main()
