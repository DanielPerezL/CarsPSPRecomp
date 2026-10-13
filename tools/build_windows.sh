#!/usr/bin/env bash
# Compila el port para Windows (x86_64) desde Linux con MinGW-w64 y lo empaqueta en build/windows/Cars/.
#
#   python3 tools/setup.py --iso juego.iso   # antes: genera build/gen (el C recompilado e image.bin)
#   tools/build_windows.sh
#
# Necesita mingw-w64 (variante posix de los hilos), cmake, ninja, pkg-config, curl y unzip
# (Debian/Ubuntu: sudo apt install mingw-w64 cmake ninja-build pkg-config curl unzip).
# SDL2 y FFmpeg para Windows se descargan precompilados (versiones fijadas y verificadas por SHA-256) en
# third_party/win64/.
#
# El paquete contiene el código del juego recompilado y su imagen: es solo para uso propio, junto a tu
# ISO. No lo subas ni lo compartas.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/third_party/win64"
BUILD="$ROOT/build/win64"
OUT="$ROOT/build/windows/Cars"

SDL_VER=2.32.10
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
SDL_SHA=83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2
# Compilación LGPL con DLL de BtbN/FFmpeg-Builds (misma versión mayor de libavcodec que en Linux).
# Si la publicación desaparece, cualquier compilación "win64-lgpl-shared" de la rama 8.x sirve:
# cambia URL y SHA-256.
FF_NAME=ffmpeg-n8.1.3-18-g46d899d7a0-win64-lgpl-shared-8.1
FF_URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-13-13-05/$FF_NAME.zip"
FF_SHA=19d92dfb1481e72e4e8f94844bf609418297a646f33deb6864c409ca752469c4

CC=x86_64-w64-mingw32-gcc-posix
CXX=x86_64-w64-mingw32-g++-posix
for tool in $CC $CXX cmake ninja pkg-config curl unzip; do
    command -v "$tool" >/dev/null || { echo "falta $tool" >&2; exit 1; }
done
[ -f "$ROOT/build/gen/funcs.h" ] && [ -f "$ROOT/build/gen/image.bin" ] ||
    { echo "no hay build/gen: ejecuta antes tools/setup.py --iso juego.iso" >&2; exit 1; }

fetch() {  # url destino sha256
    if [ ! -f "$2" ] || ! echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then
        echo "descargando $(basename "$1")"
        curl -fSL --progress-bar -o "$2.part" "$1"
        echo "$3  $2.part" | sha256sum -c --quiet - || { echo "SHA-256 incorrecto: $1" >&2; exit 1; }
        mv "$2.part" "$2"
    fi
}

mkdir -p "$DEPS/dl"
fetch "$SDL_URL" "$DEPS/dl/SDL2-$SDL_VER.tar.gz" "$SDL_SHA"
fetch "$FF_URL" "$DEPS/dl/$FF_NAME.zip" "$FF_SHA"
SDL_DIR="$DEPS/SDL2-$SDL_VER/x86_64-w64-mingw32"
FF_DIR="$DEPS/$FF_NAME"
if [ ! -d "$SDL_DIR" ]; then
    tar xzf "$DEPS/dl/SDL2-$SDL_VER.tar.gz" -C "$DEPS"
    # El .pc trae la ruta de la máquina donde se compiló
    sed -i 's|^prefix=.*|prefix=${pcfiledir}/../..|' "$SDL_DIR/lib/pkgconfig/sdl2.pc"
fi
[ -d "$FF_DIR" ] || unzip -q "$DEPS/dl/$FF_NAME.zip" -d "$DEPS"

export PKG_CONFIG_LIBDIR="$SDL_DIR/lib/pkgconfig:$FF_DIR/lib/pkgconfig"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres
cmake --build "$BUILD"

# Paquete: ejecutable, DLL, imagen del juego y licencias de las bibliotecas
WINPTHREAD="$($CC -print-file-name=libwinpthread-1.dll)"
[ -f "$WINPTHREAD" ] || WINPTHREAD=/usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll
rm -rf "$OUT"
mkdir -p "$OUT/licencias"
cp "$ROOT/build/cars.exe" "$ROOT/build/gen/image.bin" "$OUT/"
cp "$SDL_DIR/bin/SDL2.dll" "$OUT/"
cp "$FF_DIR"/bin/{avcodec-62,avutil-60,swresample-6}.dll "$OUT/"
cp "$WINPTHREAD" "$OUT/"
x86_64-w64-mingw32-strip "$OUT/cars.exe"
cp "$ROOT/LICENSE" "$OUT/licencias/cars-port-GPL-3.0.txt"
cp "$DEPS/SDL2-$SDL_VER/LICENSE.txt" "$OUT/licencias/SDL2.txt"
cp "$FF_DIR/LICENSE.txt" "$OUT/licencias/FFmpeg-LGPL.txt"

sed 's/$/\r/' > "$OUT/LEEME.txt" <<'EOF'
Cars (PSP) - port para PC (Windows 10/11, 64 bits)

1. Copia tu ISO del juego (Disney-Pixar Cars, ULES00324, versión española) en esta carpeta.
2. Doble clic en cars.exe. F11 alterna la pantalla completa.

Requisitos: GPU con OpenGL 3.3 o superior (cualquier equipo actual).

Controles
  Cruceta          Flechas            Cruceta del mando
  Stick            I J K L            Stick izquierdo
  X O Cuadrado Tri Z X A S            A B X Y
  L / R            Q / W              LB / RB o gatillos
  Start / Select   Intro / Retroceso  Start / Back
  F11 pantalla completa, F5 recarga las texturas de textures\

Opciones: crea un acceso directo a cars.exe y añádelas al final de Propiedades > Destino
(p. ej. "...\cars.exe" --scale 6 --fullscreen), o lanza cars.exe desde una consola:
  --iso FICHERO        otra ISO
  --scale N            resolución interna 480x272 x N (por defecto 4)
  --fps N              fotogramas por segundo (por defecto, los del monitor)
  --no-interpolation   30 fps como en la PSP
  --fullscreen         pantalla completa

Las partidas se guardan en saves\, en esta carpeta.

Este paquete incluye el código del juego recompilado: es solo para tu uso, con tu copia del juego.
No lo compartas. Cars es (c) Disney/Pixar.
EOF

# Un único fichero para copiarlo al equipo con Windows
if command -v zip >/dev/null; then
    rm -f "$OUT.zip"
    (cd "$(dirname "$OUT")" && zip -qr9 "$(basename "$OUT").zip" "$(basename "$OUT")")
    echo
    echo "listo: $OUT.zip"
else
    echo
    echo "listo: $OUT"
fi
