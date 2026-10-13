# Recompilación estática de Cars (PSP) a PC

El ejecutable del juego (`EBOOT.BIN`, MIPS Allegrex) se traduce a C y se enlaza con un runtime propio
que sustituye al sistema de la PSP (HLE). El resultado es un binario nativo que lee la ISO original.

Nada derivado del juego se versiona: la ISO, `decrypted/`, `ghidra/`, `build/` (con el C generado),
`textures/`, `texdump/` y `saves/` están en `.gitignore`. Lo único específico del juego que hay en el
repositorio es el fichero de símbolos (`tools/recomp/games/ULES00324.*.csv`: direcciones de funciones y
nombres de imports), que solo contiene metadatos.

## Proceso

`tools/setup.py --iso juego.iso` hace todo; por partes:

```bash
python3 tools/recomp/recomp.py build/EBOOT.elf tools/recomp/games/ULES00324.funcs.csv --out build/gen
make -C runtime -j$(nproc)
build/cars --iso juego.iso [-v | -vv]
```

Para regenerar el fichero de símbolos hace falta Ghidra 12.1.3 + ghidra-allegrex
(`tools/ghidra_pipeline.sh` y el script `ExportFunctions.java`).

Depuración: `-v` muestra el estado de los hilos cada segundo; `-vv` traza todas las llamadas al
sistema con el hilo que las hace. Pruebas sin ventana visible: `--hidden`, `--screenshot T:fichero.ppm`,
`--exit-after T`, `--record-audio f.wav` (con `SDL_AUDIODRIVER=dummy`), y `CARS_PRESS="T:botón[:dur],..."`
para simular pulsaciones. `--check-sp` en el recompilador añade comprobaciones de `$sp`.

## Arquitectura

| Pieza | Fichero | Notas |
|---|---|---|
| Cargador ELF + relocalización PSP | `tools/recomp/pspelf.py` | Validado palabra a palabra contra Ghidra |
| Recompilador Allegrex → C | `tools/recomp/recomp.py` | Una función C por función MIPS; `goto` para saltos internos |
| Memoria | `runtime/src/core.cpp` | 512 MB reservados, `addr & 0x1FFFFFFF` cubre espejos |
| Hilos | `runtime/src/threads.cpp` | Fibers (`ucontext`) con planificador por prioridad, sin reparto de tiempo |
| HLE | `runtime/src/hle_*.cpp` | Manejadores por nombre (`HLE(sceXxx)`); NID→nombre generado |
| VFPU | `runtime/src/vfpu.cpp` | Una función por operación, semántica de PPSSPP |
| UMD | `runtime/src/iso.cpp` | ISO9660 directo, incluido el acceso `sce_lbn0x…_size0x…` |
| GE | `runtime/src/gpu/ge.cpp` | Display lists, vértices (skinning/morph en CPU), primitivas |
| Render | `runtime/src/gpu/renderer_gl.cpp` | OpenGL 3.3; tubería fija de la PSP en shaders; FBO por framebuffer |
| Interpolación | `runtime/src/gpu/renderer_gl.cpp` | Graba cada fotograma lógico y reproduce interpolando matrices |
| Texturas | `runtime/src/gpu/texture*.cpp` | Todos los formatos; caché y sustitución por hash |
| Audio | `runtime/src/audio/` | sceAudio (SDL), SAS (de PPSSPP), ATRAC3/3+ (libavcodec) |
| Vídeo | `runtime/src/media/mpeg.cpp` | Ring buffer + demux MPEG-PS propio + H.264/ATRAC3+ |
| Partidas | `runtime/src/hle_savedata.cpp` | `saves/PSP/SAVEDATA/<juego><partida>/` |

Decisiones del recompilador que no son obvias:

- **Límites de función:** Ghidra + todos los destinos de `jal` + punteros a código en datos y pares
  `lui/addiu` (relocalizaciones HI16/LO16). Los destinos de tablas de salto (series de punteros
  consecutivos en `.rodata`) se tratan como etiquetas de `switch`, no como funciones.
- **setjmp/longjmp** de la libc del juego (detectados por patrón): `setjmp` del host en el marco C del
  llamador y `recomp_longjmp` restaura registros MIPS y salta a ese marco.
- **Hueco de retardo** de los saltos: se emite antes de evaluar el destino (la condición se calcula antes).

Decisiones del runtime:

- **GE:** `FINISH+END` termina una lista, pero `SIGNAL+END` es una señal y la lista sigue. El juego
  depende de ello para saber que la GPU ha terminado (si no, su FrameWatchdog lo limita a ~7 fps).
- **Interpolación:** cada dibujo se empareja con el del fotograma anterior por (origen de vértices,
  formato, nº de vértices, textura, primitiva) + orden de aparición; se interpolan matrices y, en mallas
  con skinning, vértices. El 2D y los cortes de cámara no se interpolan. Un fotograma lógico de latencia.
- **Framebuffers escritos por la CPU** (vídeos vía DMA) se presentan desde memoria hasta que la GE vuelva
  a dibujar en ellos.
- **Diálogos del sistema** (`sceUtilityMsgDialog`): en la PSP los dibuja el firmware encima de la escena
  del juego, que sigue renderizando y llama a `Update` en cada fotograma. Aquí se dibujan con Dear ImGui
  sobre la ventana, a su resolución real y escalados al rectángulo de 480x272 (`ui/overlay.cpp`); el HLE
  (`hle_dialog.cpp`) sigue la máquina de estados y las opciones de PPSSPP. Mientras están abiertos, el
  juego no recibe los botones, ni tampoco la pulsación que los cierra. Las partidas solo usan los modos
  automáticos de Savedata, que en la PSP no muestran interfaz.

## Estado

- Jugable: arranque, vídeos, menús, partidas guardadas y carreras con sonido.
- Pendiente: multijugador ad hoc, fuentes PGF del sistema,
  reverberación del SAS, port a Windows.
