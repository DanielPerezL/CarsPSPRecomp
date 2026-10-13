# Cars (PSP) — port nativo para PC

Recompilación estática de *Disney·Pixar Cars* para PSP (ULES00324, versión española) a un ejecutable
nativo de PC, con resolución libre, fotogramas interpolados por encima de 120 fps y texturas
reescaladas con IA.

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
| Sustitución de texturas + reescalado con IA | ✅ |
| Multijugador ad hoc | ❌ (la red se inicializa, nunca hay rivales) |
| Diálogos del sistema con interfaz | ❌ (se responden solos) |
| Windows | 🚧 |

## Requisitos (Linux)

```bash
sudo apt install build-essential python3 git zlib1g-dev libssl-dev \
                 libsdl2-dev libavcodec-dev libavutil-dev
```

GPU con OpenGL 3.3 o superior.

## Instalación

```bash
python3 tools/setup.py --iso "Disney-Pixar Cars (Spain).iso"
build/cars --iso "Disney-Pixar Cars (Spain).iso"
```

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

F11 alterna la pantalla completa.

## Opciones

```
--scale N            resolución interna 480x272 × N (por defecto 4)
--fps N              fotogramas presentados por segundo (por defecto, los del monitor)
--no-interpolation   30 fps como en la PSP
--fullscreen         pantalla completa
--textures DIR       texturas de sustitución <hash>.png (por defecto textures/)
--dump-textures DIR  vuelca las texturas originales para reescalarlas
```

## Texturas reescaladas con IA

```bash
build/cars --iso juego.iso --dump-textures texdump     # juega: se vuelcan las texturas que aparezcan
python3 tools/textures/upscale.py texdump textures     # x4 con Real-ESRGAN (ncnn/Vulkan)
build/cars --iso juego.iso                             # usa textures/ automáticamente
```

`upscale.py` necesita [realesrgan-ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases) en
`~/tools/realesrgan/` (o `--esrgan RUTA`). Respeta el canal alfa y evita costuras en las texturas que se
repiten.

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
  [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN).
- Inspirado en [N64Recomp](https://github.com/N64Recomp/N64Recomp) y RT64.

*Cars* es © Disney/Pixar. Este proyecto no está afiliado a Disney, Pixar, THQ ni Sony.

## Licencia

GPL-3.0-or-later. Ver [LICENSE](LICENSE).
