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
para simular pulsaciones (`reload` simula F5). `--check-sp` en el recompilador añade comprobaciones de `$sp`.
Gráficos: `CARS_GE_DUMP=T` (con `-v`) registra el estado de la GE de cada dibujo del primer fotograma tras el
segundo T (blend, test de alfa, stencil, texturas y, en los dibujos pequeños, vértices en coordenadas de
pantalla); con `CARS_GE_DUMP_DIR=carpeta` guarda además cada textura tal y como se usa (con su paleta) y,
al terminar, todos los framebuffers (`fb_<dirección>_fmt<n>.png`, con su alfa).

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
  formato, nº de vértices, textura, primitiva) y, entre los de la misma clave (cada mata de vegetación, cada
  coche igual), con el de matriz de mundo más cercana: el juego no siempre los dibuja en el mismo orden. Se
  interpolan matrices y, en mallas con skinning, vértices, salvo si la deformación es grande (una rueda que
  gira decenas de grados por fotograma se encogería hacia el eje). El 2D y los cortes de cámara no se
  interpolan. Un fotograma lógico de latencia.
- **Framebuffers escritos por la CPU** (vídeos vía DMA) se presentan desde memoria hasta que la GE vuelva
  a dibujar en ellos.
- **Stencil:** en la PSP es el canal alfa del framebuffer; aquí se usa el stencil del FBO (test, operaciones,
  máscara de escritura de 0xE9 y borrado con el alfa en modo *clear*). El minimapa lo usa para recortarse
  al círculo. Como el alfa *es* el stencil, solo se escribe cuando el test de stencil lo modifica (igual que
  PPSSPP).
- **Profundidad de campo:** en carrera el juego desenfoca lo lejano (y, con turbo, también los lados). Lee
  el búfer de profundidad como textura de 16 bits por el espejo 0x04600000 de la VRAM y lo pasa por una
  CLUT; desenfoca el resultado en búferes de 16 bits; escribe la máscara en 16 bits con el doble de ancho
  para que acabe en el alfa de una imagen de 32 bits, y mezcla con ella la escena reducida y difuminada.
  Para eso los framebuffers guardan su formato y stride: si cambian con el mismo ancho en bytes, los bytes
  se reinterpretan en la GPU (dos píxeles de 16 bits = uno de 32). La profundidad es una textura; los
  framebuffers leídos con CLUT16/CLUT32 reconstruyen el valor de 16/32 bits en el shader, y se filtran como
  en la PSP (CLUT por píxel original y mipmap de la resolución original al reducir). La mezcla con dos
  colores fijos distintos (0xDD fuente + 0x22 destino) aplica el de la fuente en el shader, porque GL solo
  tiene un `glBlendColor`: usar el mismo para los dos saturaba el desenfoque a blanco y velaba la imagen.
- **Texturas sustituidas en alta resolución:** mipmaps solo hasta la resolución original (la PSP no los usa
  aquí) y filtrado anisotrópico. Con la cadena completa, un quad muy estrecho (la barra de nitro) promediaba
  el alfa de zonas transparentes y aparecía como una franja semitransparente. F5 vuelve a leer la carpeta
  de sustituciones y fuerza la subida de cada textura en su próximo uso.
- **Nombre de las texturas:** 8 primeros bytes del MD5 de (ancho, alto, RGBA decodificado). No depende de la
  dirección, el formato ni los bytes sobrantes de la paleta, así que `tools/textures/export.py` obtiene los
  mismos nombres leyendo la ISO (comprobado: 134 de 136 texturas de un recorrido menú + carrera; las otras
  dos las genera el juego).
- **Diálogos del sistema** (`sceUtilityMsgDialog`): en la PSP los dibuja el firmware encima de la escena
  del juego, que sigue renderizando y llama a `Update` en cada fotograma. Aquí se dibujan con Dear ImGui
  sobre la ventana, a su resolución real y escalados al rectángulo de 480x272 (`ui/overlay.cpp`); el HLE
  (`hle_dialog.cpp`) sigue la máquina de estados y las opciones de PPSSPP. Mientras están abiertos, el
  juego no recibe los botones, ni tampoco la pulsación que los cierra.
- **SAS:** `KeyOn` sobre una voz que sigue sonando no la reinicia (ni `KeyOff` sobre una apagada), como en
  la PSP. El juego repite `KeyOn` de los sonidos en bucle (roces, chirridos); reiniciarlos los cortaba.
- **Partidas** (`sceUtilitySavedata`): el autoguardado y la carga al arrancar usan los modos automáticos
  (AUTOSAVE/AUTOLOAD/SIZES), que en la PSP no muestran nada y terminan al momento. *Opciones → Guardar/Cargar*
  usa SAVE y LOAD, que sí abren la pantalla del sistema: confirmación (sobrescribir con «No» por defecto),
  «Guardando…»/«Cargando…» (se mantiene ~1 s, como en la consola), resultado o «No hay datos». Se dibuja con
  el mismo overlay que los mensajes, con la disposición de PPSSPP (`PSPSaveDialog.cpp`): banner, ICON0 y
  título/fecha/tamaño de la partida a la izquierda, mensaje a la derecha. Sin ventana (`--headless`) o con
  `CARS_DIALOG_ANSWER` se guarda o carga directamente, sin pantalla.

## Estado

- Jugable: arranque, vídeos, menús, partidas guardadas y carreras con sonido.
- Pendiente: multijugador ad hoc, fuentes PGF del sistema,
  reverberación del SAS, port a Windows.
