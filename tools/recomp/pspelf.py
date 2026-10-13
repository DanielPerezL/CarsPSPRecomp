#!/usr/bin/env python3
"""Carga un ELF/PRX de PSP descifrado y aplica sus relocalizaciones (tipo 0x700000A0) a una base fija.

Relocalización PSP "tipo A": entradas (r_offset, r_info) con
  tipo = info & 0xFF, segmento del offset = (info >> 8) & 0xFF, segmento destino = (info >> 16) & 0xFF.
El offset es relativo al segmento indicado y el valor a sumar es base + vaddr del segmento destino.
Cada HI16 se empareja con el siguiente LO16 para propagar el acarreo del signo.
"""
import struct
from dataclasses import dataclass, field

R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


@dataclass
class Section:
    name: str
    type: int
    addr: int
    offset: int
    size: int
    info: int


@dataclass
class PspImage:
    base: int
    memory: bytearray              # imagen relocalizada desde base (incluye .bss a cero)
    entry: int
    sections: dict = field(default_factory=dict)  # nombre -> Section (direcciones ya con base)
    data_pointers: list = field(default_factory=list)  # (dirección del puntero, valor) de R_MIPS_32
    code_addresses: list = field(default_factory=list)  # direcciones fabricadas con pares HI16/LO16

    def u32(self, addr):
        return struct.unpack_from("<I", self.memory, addr - self.base)[0]


def load(path, base=0x08804000):
    elf = open(path, "rb").read()
    entry, phoff, shoff = struct.unpack_from("<III", elf, 0x18)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", elf, 0x2A)
    segs = [struct.unpack_from("<8I", elf, phoff + i * phentsize) for i in range(phnum)]
    loads = [s for s in segs if s[0] == 1]
    size = max(s[2] + s[5] for s in loads)
    mem = bytearray(size)
    for _, off, vaddr, _, filesz, _, _, _ in loads:
        mem[vaddr:vaddr + filesz] = elf[off:off + filesz]

    raw = [struct.unpack_from("<10I", elf, shoff + i * shentsize) for i in range(shnum)]
    strtab = raw[shstrndx][4]
    sections = {}
    for h in raw:
        name = elf[strtab + h[0]:elf.index(b"\0", strtab + h[0])].decode()
        sections[name] = Section(name, h[1], h[3] + base, h[4], h[5], h[7])

    seg_vaddr = [s[2] for s in loads]
    data_pointers = []
    code_addresses = []
    for h in raw:
        if h[1] != 0x700000A0:
            continue
        rels = [struct.unpack_from("<II", elf, h[4] + k * 8) for k in range(h[5] // 8)]
        for k, (r_off, info) in enumerate(rels):
            rtype, ofs_seg, addr_seg = info & 0xFF, info >> 8 & 0xFF, info >> 16 & 0xFF
            at = seg_vaddr[ofs_seg] + r_off
            reloc_to = base + seg_vaddr[addr_seg]
            word, = struct.unpack_from("<I", mem, at)
            if rtype == R_MIPS_32:
                word = (word + reloc_to) & 0xFFFFFFFF
                data_pointers.append((at + base, word))
            elif rtype == R_MIPS_26:
                word = (word & 0xFC000000) | ((word & 0x03FFFFFF) + (reloc_to >> 2)) & 0x03FFFFFF
            elif rtype == R_MIPS_HI16:
                lo = 0
                for r2_off, info2 in rels[k + 1:]:
                    if info2 & 0xFF == R_MIPS_LO16:
                        lo_word, = struct.unpack_from("<I", mem, seg_vaddr[info2 >> 8 & 0xFF] + r2_off)
                        lo = (lo_word & 0xFFFF) - ((lo_word & 0x8000) << 1)
                        break
                full = ((word & 0xFFFF) << 16) + lo + reloc_to
                code_addresses.append(full & 0xFFFFFFFF)
                word = (word & 0xFFFF0000) | ((full + 0x8000) >> 16 & 0xFFFF)
            elif rtype == R_MIPS_LO16:
                word = (word & 0xFFFF0000) | ((word + reloc_to) & 0xFFFF)
            else:
                raise ValueError(f"relocalización no soportada: {rtype}")
            struct.pack_into("<I", mem, at, word)
    return PspImage(base, mem, entry + base, sections, data_pointers, code_addresses)


if __name__ == "__main__":
    import sys
    img = load(sys.argv[1])
    print(f"entrada {img.entry:08x}, {len(img.memory)} bytes, {len(img.data_pointers)} punteros en datos")
    if len(sys.argv) > 2:
        ref = open(sys.argv[2], "rb").read()
        n = min(len(ref), len(img.memory))
        diffs = [i for i in range(0, n, 4) if img.memory[i:i + 4] != ref[i:i + 4]]
        print(f"palabras distintas frente a la referencia: {len(diffs)} de {n // 4}")
        for i in diffs[:10]:
            print(f"  {i + img.base:08x}: nuestro {img.memory[i:i + 4].hex()} ref {ref[i:i + 4].hex()}")
