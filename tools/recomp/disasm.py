#!/usr/bin/env python3
"""Desensambla un rango del EBOOT relocalizado: disasm.py EBOOT.elf DIRECCIÓN [N]"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "third_party" / "pylib"))
import rabbitizer  # noqa: E402

import pspelf  # noqa: E402

img = pspelf.load(sys.argv[1])
addr = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
for pc in range(addr, addr + 4 * n, 4):
    ins = rabbitizer.Instruction(img.u32(pc), vram=pc, category=rabbitizer.InstrCategory.R4000ALLEGREX)
    print(f"{pc:08x}: {img.u32(pc):08x}  {ins.disassemble()}")
