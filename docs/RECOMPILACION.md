# Recompilación estática de Cars (PSP) a PC

El ejecutable del juego (`EBOOT.BIN`, MIPS Allegrex) se traduce a C y se enlaza con un runtime propio
que sustituye al sistema de la PSP (HLE). El resultado es un binario nativo que lee la ISO original.

Nada derivado del juego se versiona: la ISO, `decrypted/`, `ghidra/` y `build/` (que incluye el C generado)
están en `.gitignore`. Cada usuario genera todo a partir de su propia copia.

## Requisitos

- `gcc`/`g++` (C11/C++17), `make`, Python 3.
- `third_party/pspdecrypt` compilado (ver historial) para descifrar el EBOOT.
- `third_party/pylib/rabbitizer` (wheel de PyPI descomprimido) y `third_party/ppsspp` (checkout parcial de
  `Core/HLE` y `Core/MIPS`, solo para la tabla de NIDs y como referencia).
- Ghidra 12.1.3 + ghidra-allegrex, para la lista de funciones (`ghidra/funcs.csv`).

## Proceso

```bash
# 1. Descifrar el ejecutable
third_party/pspdecrypt/pspdecrypt extracted/PSP_GAME/SYSDIR/EBOOT.BIN -o decrypted/EBOOT.elf

# 2. Análisis en Ghidra y exportación de funciones
tools/ghidra_pipeline.sh
~/tools/ghidra_12.1.3_PUBLIC/support/analyzeHeadless ghidra Cars -process EBOOT.elf -noanalysis -readOnly \
    -scriptPath tools/ghidra_scripts -postScript ExportFunctions.java ghidra/funcs.csv

# 3. Recompilar a C (build/gen) y compilar
python3 tools/recomp/recomp.py decrypted/EBOOT.elf ghidra/funcs.csv --out build/gen [--check-sp]
make -C runtime -j$(nproc)

# 4. Ejecutar
build/cars --iso "Disney-Pixar Cars (Spain).iso" [-v | -vv]
```

`-v` muestra el estado de los hilos cada segundo; `-vv` traza todas las llamadas al sistema con el hilo que
las hace. `CARS_PRESS="2.5:start,4:cross"` simula pulsaciones (segundos:botón) mientras no hay ventana.

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
| GE | `runtime/src/hle_ge.cpp` | F0: solo control de flujo de las display lists, sin dibujar |

Decisiones del recompilador que no son obvias:

- **Límites de función:** Ghidra + todos los destinos de `jal` + punteros a código en datos y pares
  `lui/addiu` (relocalizaciones HI16/LO16). Los destinos de tablas de salto (series de punteros consecutivos
  en `.rodata`) se tratan como etiquetas de `switch`, no como funciones.
- **setjmp/longjmp** de la libc del juego (detectados por patrón): `setjmp` del host en el marco C del
  llamador y `recomp_longjmp` restaura registros MIPS y salta a ese marco.
- **Hueco de retardo** de los saltos: se emite antes de evaluar el destino (la condición se calcula antes).

## Estado

- **F0 (hecho):** el juego arranca, inicializa RenderWare, carga datos del UMD y mantiene su bucle principal
  enviando display lists a la GE de forma continua.
- **Siguiente:** renderizador de la GE sobre Vulkan/OpenGL (resolución libre), ventana y mandos, audio
  (SAS + ATRAC3+), vídeo PMF y fuentes PGF; después interpolación de fotogramas para >120 fps y sustitución
  de texturas/modelos.
