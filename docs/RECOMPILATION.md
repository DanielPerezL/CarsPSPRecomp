# Static recompilation of Cars (PSP) to PC

The game's executable (`EBOOT.BIN`, MIPS Allegrex) is translated into C and linked against a custom
runtime that replaces the PSP's operating system (HLE). The result is a native binary that reads the
original ISO.

Nothing derived from the game is versioned: the ISO, `decrypted/`, `ghidra/`, `build/` (which holds the
generated C), `textures/`, `texdump/` and `saves/` are in `.gitignore`. The only game-specific file in
the repository is the symbol file (`tools/recomp/games/ULES00324.*.csv`: function addresses and import
names), which contains metadata only.

## Process

`tools/setup.py --iso game.iso` does everything; step by step:

```bash
python3 tools/recomp/recomp.py build/EBOOT.elf tools/recomp/games/ULES00324.funcs.csv --out build/gen
make -C runtime -j$(nproc)
build/cars --iso game.iso [-v | -vv]
```

Regenerating the symbol file requires Ghidra 12.1.3 + ghidra-allegrex (`tools/ghidra_pipeline.sh` and
the `ExportFunctions.java` script).

Debugging: `-v` prints the state of every thread once per second; `-vv` traces every system call along
with the thread that makes it. Testing without a visible window: `--hidden`, `--screenshot T:file.ppm`,
`--exit-after T`, `--record-audio f.wav` (with `SDL_AUDIODRIVER=dummy`), and
`CARS_PRESS="T:button[:dur],..."` to simulate button presses (`reload` simulates F5). `--check-sp` in the
recompiler adds `$sp` checks. Graphics: `CARS_GE_DUMP=T` (with `-v`) logs the GE state of every draw in
the first frame after second T (blending, alpha test, stencil, textures and, for small draws, vertices in
screen coordinates); with `CARS_GE_DUMP_DIR=folder` it also saves every texture as it is used (with its
palette) and, on exit, every framebuffer (`fb_<address>_fmt<n>.png`, with its alpha).

## Architecture

| Component | File | Notes |
|---|---|---|
| ELF loader + PSP relocation | `tools/recomp/pspelf.py` | Validated word by word against Ghidra |
| Allegrex → C recompiler | `tools/recomp/recomp.py` | One C function per MIPS function; `goto` for internal branches |
| Memory | `runtime/src/core.cpp` | 512 MB reserved, `addr & 0x1FFFFFFF` covers the mirrors |
| Threads | `runtime/src/threads.cpp` | Fibers (`ucontext`) with a priority scheduler, no time slicing |
| HLE | `runtime/src/hle_*.cpp` | Handlers by name (`HLE(sceXxx)`); generated NID→name table |
| VFPU | `runtime/src/vfpu.cpp` | One function per operation, PPSSPP semantics |
| UMD | `runtime/src/iso.cpp` | Direct ISO9660, including `sce_lbn0x…_size0x…` access |
| GE | `runtime/src/gpu/ge.cpp` | Display lists, vertices (skinning/morphing on the CPU), primitives |
| Rendering | `runtime/src/gpu/renderer_gl.cpp` | OpenGL 3.3; the PSP's fixed pipeline in shaders; one FBO per framebuffer |
| Interpolation | `runtime/src/gpu/renderer_gl.cpp` | Records each logic frame and replays it with interpolated matrices |
| Textures | `runtime/src/gpu/texture*.cpp` | Every format; cache and replacement by hash |
| Audio | `runtime/src/audio/` | sceAudio (SDL), SAS (from PPSSPP), ATRAC3/3+ (libavcodec) |
| Video | `runtime/src/media/mpeg.cpp` | Ring buffer + custom MPEG-PS demuxer + H.264/ATRAC3+ |
| Save games | `runtime/src/hle_savedata.cpp` | `saves/PSP/SAVEDATA/<game><save>/` |

Non-obvious recompiler decisions:

- **Function boundaries:** Ghidra + every `jal` target + code pointers in data and `lui/addiu` pairs
  (HI16/LO16 relocations). Jump table targets (runs of consecutive pointers in `.rodata`) are treated as
  `switch` labels, not as functions.
- **setjmp/longjmp** from the game's libc (detected by pattern): the host's `setjmp` runs in the
  caller's C frame, and `recomp_longjmp` restores the MIPS registers and jumps back to that frame.
- **Branch delay slot:** emitted before the branch target is evaluated (the condition is computed
  beforehand).

Runtime decisions:

- **GE:** `FINISH+END` ends a list, but `SIGNAL+END` is a signal and the list continues. The game relies
  on this to know that the GPU has finished (otherwise its FrameWatchdog caps it at ~7 fps).
- **Interpolation:** each draw is matched with one from the previous frame by (vertex source, format,
  vertex count, texture, primitive) and, among draws with the same key (each clump of vegetation, each
  identical car), with the one whose world matrix is closest: the game does not always draw them in the
  same order. Matrices are interpolated and, for skinned meshes, vertices too, unless the deformation is
  large (a wheel spinning tens of degrees per frame would shrink towards its axle). 2D and camera cuts
  are not interpolated. One logic frame of latency.
- **Framebuffers written by the CPU** (videos via DMA) are presented from memory until the GE draws to
  them again.
- **Stencil:** on the PSP it is the framebuffer's alpha channel; here the FBO's stencil buffer is used
  (test, operations, a 0xE9 write mask and clearing with alpha in *clear* mode). The minimap uses it to
  clip itself to a circle. Since alpha *is* the stencil, it is only written when the stencil test
  modifies it (as in PPSSPP).
- **Depth of field:** during races the game blurs distant geometry (and, with boost, the sides as well).
  It reads the depth buffer as a 16-bit texture through the 0x04600000 VRAM mirror and runs it through a
  CLUT; blurs the result in 16-bit buffers; writes the mask in 16 bits at twice the width so that it ends
  up in the alpha of a 32-bit image, and blends the downscaled, blurred scene with it. To support this,
  framebuffers keep their format and stride: if they change while keeping the same width in bytes, the
  bytes are reinterpreted on the GPU (two 16-bit pixels = one 32-bit pixel). Depth is a texture;
  framebuffers read through CLUT16/CLUT32 rebuild the 16/32-bit value in the shader and are filtered as
  on the PSP (CLUT per original pixel and a mipmap at the original resolution when downscaling). Blending
  with two different fixed colours (0xDD source + 0x22 destination) applies the source one in the shader,
  because GL only has one `glBlendColor`: using the same one for both saturated the blur to white and
  washed out the image.
- **High-resolution replacement textures:** mipmaps only down to the original resolution (the PSP does
  not use them here) and anisotropic filtering. With the full chain, a very thin quad (the boost bar)
  averaged the alpha of transparent areas and showed up as a semi-transparent strip. F5 rereads the
  replacement folder and forces each texture to be re-uploaded the next time it is used.
- **Texture names:** the first 8 bytes of the MD5 of (width, height, decoded RGBA). They do not depend
  on the address, the format or leftover palette bytes, so `tools/textures/export.py` gets the same names
  by reading the ISO (verified: 134 of the 136 textures seen in a menu + race run; the other two are
  generated by the game).
- **System dialogs** (`sceUtilityMsgDialog`): on the PSP the firmware draws them on top of the game's
  scene, which keeps rendering and calls `Update` every frame. Here they are drawn with Dear ImGui over
  the window, at its real resolution and scaled to the 480x272 rectangle (`ui/overlay.cpp`); the HLE
  (`hle_dialog.cpp`) follows PPSSPP's state machine and options. While a dialog is open, the game does
  not receive button input, including the press that closes it.
- **SAS:** `KeyOn` on a voice that is still playing does not restart it (nor does `KeyOff` on a silent
  one), as on the PSP. The game repeats `KeyOn` for looping sounds (scraping, screeching); restarting them
  cut them off.
- **Windows:** cross-compiled from Linux with `tools/build_windows.sh` (MinGW-w64, *posix* threading
  variant: Debian's *win32* variant lacks `std::condition_variable`). Fibers are Win32 fibers. The
  `jmp_buf` lives inside a struct to keep its 16-byte alignment (x64 `setjmp` saves the XMM registers with
  aligned instructions). A manifest sets UTF-8 as the code page (paths with accented characters) and
  declares per-monitor DPI awareness. It is a windowed application with no console: fatal errors are shown in a
  message box.
- **Save games** (`sceUtilitySavedata`): autosave and loading at boot use the automatic modes
  (AUTOSAVE/AUTOLOAD/SIZES), which on the PSP show nothing and finish immediately. *Options → Save/Load*
  uses SAVE and LOAD, which do open the system screen: confirmation (overwrite, with "No" as the
  default), "Saving…"/"Loading…" (held for ~1 s, as on the console), the result or "No data". It is drawn
  with the same overlay as the message dialogs, using PPSSPP's layout (`PSPSaveDialog.cpp`): banner,
  ICON0 and the save's title/date/size on the left, message on the right. Without a window (`--headless`)
  or with `CARS_DIALOG_ANSWER`, saving or loading happens directly, with no screen.

## Status

- Playable: boot, videos, menus, save games and races with sound.
- Pending: ad hoc multiplayer, system PGF fonts, SAS reverb.
