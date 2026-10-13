# Cars (PSP) — port nativo para PC

Recompilación estática de *Disney·Pixar Cars* para PSP (ULES00324, versión española) a un ejecutable
nativo de PC, con resolución libre, fotogramas interpolados por encima de 120 fps y texturas
sustituibles.

El ejecutable del juego se traduce instrucción a instrucción a C y se enlaza con un runtime propio que
sustituye al sistema de la PSP (HLE). La lógica, la física y la IA son las originales, sin emulación de
CPU.

> **Necesitas tu propia copia del juego.** Este repositorio no contiene nada de Cars: ni la ISO, ni el
> código recompilado, ni sus gráficos o sonidos. Todo se genera en tu máquina a partir de tu ISO.

## Estado

| | |
|---|---|
| Arranque, menús, partidas guardadas | ✅ |
| Carreras (física Havok, IA, HUD) | ✅ |
| Gráficos a 480x272 × N (por defecto 1920x1088) | ✅ |
| Fotogramas interpolados (lógica a 30, presentación a la frecuencia del monitor) | ✅ |
| Música (ATRAC3+), efectos (SAS) y vídeos (PSMF) | ✅ |
| Exportación y sustitución de texturas (recarga en caliente con F5) | ✅ |
| Multijugador ad hoc | ❌ (la red se inicializa, nunca hay rivales) |
| Diálogos del sistema (mando, teclado o ratón) | ✅ |
| Windows (MSYS2/MinGW-w64) | 🚧 preparado, pendiente de probar |

## Requisitos (Linux)

```bash
sudo apt install build-essential python3 git zlib1g-dev libssl-dev \
                 libsdl2-dev libavcodec-dev libavutil-dev
```

GPU con OpenGL 3.3 o superior.

## Requisitos (Windows 10/11)

Con [MSYS2](https://www.msys2.org/), en la consola **MSYS2 MINGW64**:

```bash
pacman -S git make mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,ffmpeg,zlib,openssl,python,python-pip}
python -m pip install rabbitizer==1.16.2
```

## Instalación

```bash
python3 tools/setup.py --iso "Disney-Pixar Cars (Spain).iso"
build/cars --iso "Disney-Pixar Cars (Spain).iso"
```

En Windows el ejecutable es `build/cars.exe` y necesita las DLL de SDL2 y FFmpeg de MSYS2
(`/mingw64/bin`) en el `PATH` o junto al ejecutable.

`setup.py` descarga y compila las herramientas necesarias (versiones fijadas), extrae y descifra el
ejecutable de tu ISO, comprueba que es la versión soportada, lo recompila y compila el runtime.

## Controles

| PSP | Teclado | Mando |
|---|---|---|
| Cruceta | Flechas | Cruceta |
| Stick | I J K L | Stick izquierdo |
| ✕ ○ □ △ | Z X A S | A B X Y |
| L / R | Q / W | LB / RB o gatillos |
| Start / Select | Intro / Retroceso | Start / Back |

F11 alterna la pantalla completa y F5 recarga las texturas propias. En los diálogos del sistema también valen el ratón, Intro y Escape.

## Opciones

```
--scale N            resolución interna 480x272 × N (por defecto 4)
--fps N              fotogramas presentados por segundo (por defecto, los del monitor)
--no-interpolation   30 fps como en la PSP
--fullscreen         pantalla completa
--textures DIR       texturas de sustitución <hash>.png (por defecto textures/)
--dump-textures DIR  vuelca las texturas originales para editarlas
--ui-font FICHERO    fuente TTF de los diálogos (por defecto Noto Sans, DejaVu Sans o Segoe UI)
```

## Texturas propias

Puedes exportar todas las texturas del juego de golpe, editarlas y ver los cambios en el juego sin
reiniciarlo.

```bash
python3 tools/textures/export.py --iso juego.iso --out texdump   # 1. ~1.860 texturas en unos segundos
mkdir -p textures && cp texdump/<hash>.png textures/            # 2. copia las que quieras cambiar y edítalas
build/cars --iso juego.iso                                      # 3. textures/ se carga automáticamente
```

- `export.py` lee la ISO directamente: las texturas de RenderWare de los `.DFF` y los PNG de `USRDIR`.
  `texdump/index.csv` dice de qué fichero sale cada una y su nombre original (p. ej. las de un coche están
  en `CARSKIN_<COCHE>_<VARIANTE>.DFF`).
- Cada textura se llama por el hash de su imagen (`<hash>.png`), el mismo con el que el juego la busca.
- La versión editada puede tener el mismo tamaño o más resolución, siempre que mantenga la proporción
  (128x64 → 512x256). El canal alfa se respeta.
- **F5** recarga la carpeta `textures/` en caliente: edita, guarda y pulsa F5 para verlo en el juego.
- Unas pocas texturas las genera el juego mientras corre (no están en los ficheros). Para esas,
  `build/cars --iso juego.iso --dump-textures texdump` guarda, jugando, las que aparezcan en pantalla.

## Contribuir

El repositorio no puede contener nada del juego (ISO, ejecutables, assets, volcados, capturas ni
conversiones). Activa la comprobación automática en cada commit:

```bash
git config core.hooksPath .githooks
python3 tools/check_repo.py --history   # revisa todo el historial antes de publicar
```

## Documentación

- [docs/RECOMPILACION.md](docs/RECOMPILACION.md): arquitectura del recompilador y del runtime.
- `tools/`: herramientas de análisis (extracción de la ISO, streams de RenderWare Studio, conversión de
  modelos a glTF, bancos de sonido) y scripts de Ghidra.

## Créditos

- [PPSSPP](https://github.com/hrydgard/ppsspp) (GPL-2.0+): referencia de la semántica de la VFPU, el SAS,
  Savedata, sceMpeg y las tablas de NIDs.
- [rabbitizer](https://github.com/Decompollaborate/rabbitizer) (MIT): desensamblador Allegrex.
- [pspdecrypt](https://github.com/John-K/pspdecrypt) (GPL-3.0): descifrado del ejecutable.
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) y [ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex): análisis.
- [SDL2](https://www.libsdl.org/), [FFmpeg](https://ffmpeg.org/), [stb](https://github.com/nothings/stb),
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT).
- Inspirado en [N64Recomp](https://github.com/N64Recomp/N64Recomp) y RT64.

*Cars* es © Disney/Pixar. Este proyecto no está afiliado a Disney, Pixar, THQ ni Sony.

## Licencia

GPL-3.0-or-later. Ver [LICENSE](LICENSE).
