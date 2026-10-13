#!/usr/bin/env python3
"""Recompilador estático Allegrex (PSP) -> C.

Uso: recomp.py EBOOT.elf funcs.csv --out build/gen

- Cada función MIPS pasa a `void f_XXXXXXXX(Ctx *c)`; los saltos internos son `goto`.
- Límites de función: los de Ghidra (funcs.csv) + destinos de punteros en datos que no
  caen dentro de ninguna función + todos los destinos de `jal` (Ghidra fusiona a veces
  funciones que acaban en salto de cola). Cada función acaba donde empieza la siguiente.
- Etiquetas de `switch`: los punteros relocalizados que caen dentro de una función.
- jalr/jr indirectos se resuelven en el runtime (recomp_call) con la tabla de funciones.
- Instrucciones VFPU -> vfpu_<op>(c, palabra), implementadas en el runtime (runtime/src/vfpu.cpp).
- Stubs de import (.sceStub.text.*) -> hle_call(c, NID).
- setjmp/longjmp de la libc del juego: setjmp del host en el marco del llamador + recomp_longjmp.

La salida contiene código derivado del juego: es local (build/), nunca se versiona.
"""
import argparse
import bisect
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "third_party" / "pylib"))

import rabbitizer  # noqa: E402

import pspelf  # noqa: E402
from psp_imports import imports as elf_imports, load_nids  # noqa: E402

VFPU_PREFIXES = ("v", "lv", "sv", "mtv", "mfv")


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def vfpu_func(name):
    """Nombre de la función del runtime para una instrucción VFPU (sin sufijo de tamaño salvo lv/sv)."""
    base, _, suffix = name.partition(".")
    if base in ("lv", "sv", "lvl", "lvr", "svl", "svr"):
        return f"vfpu_{base}_{suffix}"
    return f"vfpu_{base}"


def R(n):
    return "0u" if n == 0 else f"c->r[{n}]"


def SR(n):
    return "0" if n == 0 else f"(int32_t)c->r[{n}]"


def W(n, expr):
    return "" if n == 0 else f"c->r[{n}] = {expr};"


class Recompiler:
    check_sp = False
    setjmp_addr = None
    longjmp_addr = None

    def __init__(self, img, func_starts, stub_nids):
        self.img = img
        text = img.sections[".text"]
        self.text_lo, self.text_hi = text.addr, text.addr + text.size
        self.stub_nids = stub_nids  # dirección del stub -> NID
        self.starts = sorted(func_starts)
        self.start_set = set(self.starts) | set(stub_nids)  # destinos válidos de llamada
        self.unimplemented = 0
        self.bad_targets = 0
        self.vfpu = 0
        self.vfpu_funcs = set()
        self.cat = rabbitizer.InstrCategory.R4000ALLEGREX

    def word(self, addr):
        return self.img.u32(addr)

    def end_of(self, start):
        i = bisect.bisect_right(self.starts, start)
        return self.starts[i] if i < len(self.starts) else self.text_hi

    # ------------------------------------------------------------ instrucciones simples

    def simple(self, pc, w):
        """Traduce una instrucción sin control de flujo. Devuelve C o None si es salto."""
        op = w >> 26
        rs, rt, rd, sa, fn = w >> 21 & 31, w >> 16 & 31, w >> 11 & 31, w >> 6 & 31, w & 63
        imm = w & 0xFFFF
        simm = s16(imm)
        ea = f"{R(rs)} + {simm}u" if simm >= 0 else f"{R(rs)} - {-simm}u"
        if op == 0:
            if fn == 0x00:
                if w == 0:
                    return ""
                return W(rd, f"{R(rt)} << {sa}")
            if fn == 0x02:
                if rs == 1:
                    return W(rd, f"({R(rt)} >> {sa}) | ({R(rt)} << {(32 - sa) & 31})") if sa else W(rd, R(rt))
                return W(rd, f"{R(rt)} >> {sa}")
            if fn == 0x03:
                return W(rd, f"(uint32_t)({SR(rt)} >> {sa})")
            if fn == 0x04:
                return W(rd, f"{R(rt)} << ({R(rs)} & 31)")
            if fn == 0x06:
                if sa == 1:
                    return W(rd, f"({R(rt)} >> ({R(rs)} & 31)) | ({R(rt)} << ((32 - ({R(rs)} & 31)) & 31))")
                return W(rd, f"{R(rt)} >> ({R(rs)} & 31)")
            if fn == 0x07:
                return W(rd, f"(uint32_t)({SR(rt)} >> ({R(rs)} & 31))")
            if fn == 0x0A:
                return "" if rd == 0 else f"if ({R(rt)} == 0) c->r[{rd}] = {R(rs)};"
            if fn == 0x0B:
                return "" if rd == 0 else f"if ({R(rt)} != 0) c->r[{rd}] = {R(rs)};"
            if fn == 0x0D:
                return f"recomp_break(c, 0x{pc:08x}u, {w >> 6 & 0xFFFFF});"
            if fn == 0x0F:
                return ""  # sync
            if fn == 0x10:
                return W(rd, "c->hi")
            if fn == 0x11:
                return f"c->hi = {R(rs)};"
            if fn == 0x12:
                return W(rd, "c->lo")
            if fn == 0x13:
                return f"c->lo = {R(rs)};"
            if fn == 0x16:
                return W(rd, f"op_clz({R(rs)})")
            if fn == 0x17:
                return W(rd, f"op_clz(~{R(rs)})")
            if fn == 0x18:
                return f"op_mult(c, (int64_t){SR(rs)} * {SR(rt)});"
            if fn == 0x19:
                return f"op_mult(c, (int64_t)((uint64_t){R(rs)} * {R(rt)}));"
            if fn == 0x1A:
                return f"op_div(c, {SR(rs)}, {SR(rt)});"
            if fn == 0x1B:
                return f"op_divu(c, {R(rs)}, {R(rt)});"
            if fn == 0x1C:
                return f"op_mult(c, hilo(c) + (int64_t){SR(rs)} * {SR(rt)});"
            if fn == 0x1D:
                return f"op_mult(c, (int64_t)((uint64_t)hilo(c) + (uint64_t){R(rs)} * {R(rt)}));"
            if fn == 0x2E:
                return f"op_mult(c, hilo(c) - (int64_t){SR(rs)} * {SR(rt)});"
            if fn == 0x2F:
                return f"op_mult(c, (int64_t)((uint64_t)hilo(c) - (uint64_t){R(rs)} * {R(rt)}));"
            if fn in (0x20, 0x21):
                return W(rd, f"{R(rs)} + {R(rt)}")
            if fn in (0x22, 0x23):
                return W(rd, f"{R(rs)} - {R(rt)}")
            if fn == 0x24:
                return W(rd, f"{R(rs)} & {R(rt)}")
            if fn == 0x25:
                return W(rd, f"{R(rs)} | {R(rt)}")
            if fn == 0x26:
                return W(rd, f"{R(rs)} ^ {R(rt)}")
            if fn == 0x27:
                return W(rd, f"~({R(rs)} | {R(rt)})")
            if fn == 0x2A:
                return W(rd, f"{SR(rs)} < {SR(rt)}")
            if fn == 0x2B:
                return W(rd, f"{R(rs)} < {R(rt)}")
            if fn == 0x2C:
                return W(rd, f"{SR(rs)} > {SR(rt)} ? {R(rs)} : {R(rt)}")
            if fn == 0x2D:
                return W(rd, f"{SR(rs)} < {SR(rt)} ? {R(rs)} : {R(rt)}")
            if fn in (0x08, 0x09, 0x0C):
                return None
        elif op in (0x08, 0x09):
            return W(rt, f"{R(rs)} + {simm}u" if simm >= 0 else f"{R(rs)} - {-simm}u")
        elif op == 0x0A:
            return W(rt, f"{SR(rs)} < {simm}")
        elif op == 0x0B:
            return W(rt, f"{R(rs)} < 0x{simm & 0xFFFFFFFF:x}u")
        elif op == 0x0C:
            return W(rt, f"{R(rs)} & 0x{imm:x}u")
        elif op == 0x0D:
            return W(rt, f"{R(rs)} | 0x{imm:x}u")
        elif op == 0x0E:
            return W(rt, f"{R(rs)} ^ 0x{imm:x}u")
        elif op == 0x0F:
            return W(rt, f"0x{imm << 16:x}u")
        elif op == 0x20:
            return W(rt, f"(uint32_t)(int8_t)R8({ea})")
        elif op == 0x21:
            return W(rt, f"(uint32_t)(int16_t)R16({ea})")
        elif op == 0x22:
            return W(rt, f"op_lwl({ea}, {R(rt)})")
        elif op == 0x23:
            return W(rt, f"R32({ea})")
        elif op == 0x24:
            return W(rt, f"R8({ea})")
        elif op == 0x25:
            return W(rt, f"R16({ea})")
        elif op == 0x26:
            return W(rt, f"op_lwr({ea}, {R(rt)})")
        elif op == 0x28:
            return f"W8({ea}, (uint8_t){R(rt)});"
        elif op == 0x29:
            return f"W16({ea}, (uint16_t){R(rt)});"
        elif op == 0x2B:
            return f"W32({ea}, {R(rt)});"
        elif op == 0x2F:
            return ""  # cache
        elif op == 0x31:
            return f"c->fi[{rt}] = R32({ea});"
        elif op == 0x39:
            return f"W32({ea}, c->fi[{rt}]);"
        elif op == 0x1F:
            if fn == 0x00:
                return W(rt, f"op_ext({R(rs)}, {sa}, {rd + 1})")
            if fn == 0x04:
                return W(rt, f"op_ins({R(rt)}, {R(rs)}, {sa}, {rd - sa + 1})")
            if fn == 0x20:
                if sa == 0x02:
                    return W(rd, f"((({R(rt)} & 0xFF00FF00u) >> 8) | (({R(rt)} & 0x00FF00FFu) << 8))")
                if sa == 0x03:
                    return W(rd, f"__builtin_bswap32({R(rt)})")
                if sa == 0x10:
                    return W(rd, f"(uint32_t)(int8_t){R(rt)}")
                if sa == 0x18:
                    return W(rd, f"(uint32_t)(int16_t){R(rt)}")
                if sa == 0x14:
                    return W(rd, f"op_bitrev({R(rt)})")
        elif op == 0x11:
            return self.cop1(w)
        if op in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17, 0x01):
            return None
        name = rabbitizer.Instruction(w, vram=pc, category=self.cat).getOpcodeName()
        if name.startswith(VFPU_PREFIXES):
            self.vfpu += 1
            fn = vfpu_func(name)
            self.vfpu_funcs.add(fn)
            return f"{fn}(c, 0x{w:08x}u);"
        self.unimplemented += 1
        return f"recomp_unimplemented(c, 0x{pc:08x}u, 0x{w:08x}u); /* {name} */"

    def cop1(self, w):
        fmt, ft, fs, fd, fn = w >> 21 & 31, w >> 16 & 31, w >> 11 & 31, w >> 6 & 31, w & 63
        if fmt == 0:
            return W(ft, f"c->fi[{fs}]")
        if fmt == 2:
            return W(ft, "c->fcr31" if fs == 31 else "0u")
        if fmt == 4:
            return f"c->fi[{fs}] = {R(ft)};"
        if fmt == 6:
            return f"c->fcr31 = {R(ft)};" if fs == 31 else ""
        if fmt == 8:
            return None  # bc1x
        if fmt == 16:
            a, b = f"c->f[{fs}]", f"c->f[{ft}]"
            ops = {0: f"{a} + {b}", 1: f"{a} - {b}", 2: f"{a} * {b}", 3: f"{a} / {b}",
                   4: f"sqrtf({a})", 5: f"fabsf({a})", 6: a, 7: f"-{a}"}
            if fn in ops:
                return f"c->f[{fd}] = {ops[fn]};"
            if fn == 0x0C:
                return f"c->fi[{fd}] = op_f2i(rint({a}));"
            if fn == 0x0D:
                return f"c->fi[{fd}] = op_f2i(truncf({a}));"
            if fn == 0x0E:
                return f"c->fi[{fd}] = op_f2i(ceilf({a}));"
            if fn == 0x0F:
                return f"c->fi[{fd}] = op_f2i(floorf({a}));"
            if fn == 0x24:
                return f"c->fi[{fd}] = op_cvt_w_s(c, {a});"
            if fn >= 0x30:
                cond = fn & 0xF
                parts = []
                if cond & 1:
                    parts.append(f"({a} != {a} || {b} != {b})")
                if cond & 2:
                    parts.append(f"{a} == {b}")
                if cond & 4:
                    parts.append(f"{a} < {b}")
                return f"c->fpcond = {' || '.join(parts) if parts else '0'};"
        if fmt == 20 and fn == 0x20:
            return f"c->f[{fd}] = (float)(int32_t)c->fi[{fs}];"
        self.unimplemented += 1
        return f"recomp_unimplemented(c, c->pc, 0x{w:08x}u);"

    # ------------------------------------------------------------ control de flujo

    def branch_info(self, pc, w):
        """(condición C o None si incondicional, destino, likely, enlaza) o None si no es branch."""
        op, rs, rt = w >> 26, w >> 21 & 31, w >> 16 & 31
        target = (pc + 4 + (s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
        conds = {0x04: f"{R(rs)} == {R(rt)}", 0x05: f"{R(rs)} != {R(rt)}",
                 0x06: f"{SR(rs)} <= 0", 0x07: f"{SR(rs)} > 0"}
        if op in conds:
            cond = None if op == 0x04 and rs == rt else conds[op]
            return cond, target, False, False
        likely = {0x14: 0x04, 0x15: 0x05, 0x16: 0x06, 0x17: 0x07}
        if op in likely:
            return conds[likely[op]], target, True, False
        if op == 0x01:
            base = {0: f"{SR(rs)} < 0", 1: f"{SR(rs)} >= 0"}
            if rt in (0x00, 0x01):
                return base[rt], target, False, False
            if rt in (0x02, 0x03):
                return base[rt - 2], target, True, False
            if rt in (0x10, 0x11):
                return base[rt - 0x10], target, False, True
        if op == 0x11 and (w >> 21 & 31) == 8:
            tf, lk = w >> 16 & 1, w >> 17 & 1
            return ("c->fpcond" if tf else "!c->fpcond"), target, bool(lk), False
        return None

    def gen_function(self, start):
        end = self.end_of(start)
        labels = {start}
        switch_labels = set()
        for pc in range(start, end, 4):
            bi = self.branch_info(pc, self.word(pc))
            if bi and start <= bi[1] < end:
                labels.add(bi[1])
            w = self.word(pc)
            if w >> 26 == 0x02:
                t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if start <= t < end and t not in self.start_set - {start}:
                    labels.add(t)
        for t in self.jump_labels.get(start, ()):
            labels.add(t)
            switch_labels.add(t)

        name = f"f_{start:08x}"
        if start == self.longjmp_addr:
            name = f"longjmp_body_{start:08x}"
        out = [f"{'static ' if start == self.longjmp_addr else ''}void {name}(Ctx *c) {{"]
        pc = start
        skip_delay = False
        while pc < end:
            w = self.word(pc)
            if skip_delay:
                skip_delay = False
                if pc in labels:
                    # alguien salta directamente al hueco de retardo
                    out.append(f"  goto L_{pc + 4:08x};")
                    labels.add(pc + 4)
                else:
                    pc += 4
                    continue
            if pc in labels:
                out.append(f"L_{pc:08x}:;")
            code, consumed_delay = self.translate(pc, w, start, end, switch_labels, labels)
            if self.check_sp and "c->r[29] =" in code:
                code += f" if ((c->r[29] & 0x1FFFFFFFu) - 0x08800000u >= 0x01800000u) recomp_bad_sp(c, 0x{pc:08x}u);"
            out.append("  " + code if code else "")
            if consumed_delay:
                skip_delay = True
            pc += 4
        # caída al final sin retorno: continúa en la función siguiente
        if end in self.start_set:
            out.append(f"  f_{end:08x}(c);")
        out.append("}")
        text = "\n".join(line for line in out if line != "")
        # etiquetas añadidas tarde (salto al hueco de retardo) que no se emitieron
        missing = [l for l in labels if f"L_{l:08x}:" not in text and f"L_{l:08x}" in text]
        for l in missing:
            text = text.replace("\n}", f"\nL_{l:08x}:;\n  f_{l:08x}(c);\n}}") if l in self.start_set else \
                text.replace("\n}", f"\nL_{l:08x}:;\n  recomp_bad_jump(c, 0x{start:08x}u, 0x{l:08x}u);\n}}")
        if start == self.longjmp_addr:
            text += (f"\nvoid f_{start:08x}(Ctx *c) {{ uint32_t jb = c->r[4]; "
                     f"longjmp_body_{start:08x}(c); recomp_longjmp(c, jb); }}")
        return text

    def jump_or_call(self, target, start, end, labels):
        """C para transferir control a `target` (goto interno o llamada de cola)."""
        if start <= target < end and target in labels:
            return f"goto L_{target:08x};"
        if target in self.start_set:
            return f"{{ f_{target:08x}(c); return; }}"
        self.bad_targets += 1
        return f"{{ recomp_bad_jump(c, c->pc, 0x{target:08x}u); return; }}"

    def translate(self, pc, w, start, end, switch_labels, labels):
        """Devuelve (código C, consumió hueco de retardo)."""
        simple = self.simple(pc, w)
        if simple is not None:
            return simple, False
        delay = self.simple(pc + 4, self.word(pc + 4)) or ""
        op, rs, rd, fn = w >> 26, w >> 21 & 31, w >> 11 & 31, w & 63
        set_pc = f"c->pc = 0x{pc:08x}u;"
        bi = self.branch_info(pc, w)
        if bi:
            cond, target, likely, link = bi
            go = self.jump_or_call(target, start, end, labels)
            lnk = f"c->r[31] = 0x{pc + 8:08x}u; " if link else ""
            if cond is None:
                return f"{delay} {lnk}{go}", True
            if likely:
                return f"if ({cond}) {{ {delay} {lnk}{go} }}", True
            return f"{{ int t_ = {cond}; {delay} {lnk}if (t_) {go} }}", True
        if op == 0x02:
            target = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            return f"{delay} {self.jump_or_call(target, start, end, labels)}", True
        if op == 0x03:
            target = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if target == self.setjmp_addr:
                # setjmp guarda los registros MIPS en el jmp_buf del juego; el setjmp del host fija este
                # marco C como destino del longjmp (recomp_longjmp restaura registros y salta aquí).
                return (f"{delay} {{ uint32_t jb_ = c->r[4]; c->r[31] = 0x{pc + 8:08x}u; {set_pc} "
                        f"f_{target:08x}(c); if (setjmp(*recomp_jmpbuf(jb_))) {{}} }}"), True
            if target in self.start_set:
                call = f"f_{target:08x}(c);"
            else:
                self.bad_targets += 1
                call = f"recomp_call(c, 0x{target:08x}u);"
            return f"{delay} c->r[31] = 0x{pc + 8:08x}u; {set_pc} {call}", True
        if op == 0 and fn == 0x08:
            if rs == 31:
                return f"{delay} return;", True
            cases = " ".join(f"case 0x{t:08x}u: goto L_{t:08x};" for t in sorted(switch_labels))
            return (f"{{ uint32_t t_ = {R(rs)}; {delay} {set_pc} switch (t_) {{ {cases} "
                    f"default: recomp_call(c, t_); return; }} }}"), True
        if op == 0 and fn == 0x09:
            return f"{{ uint32_t t_ = {R(rs)}; {delay} {W(rd, f'0x{pc + 8:08x}u')} {set_pc} recomp_call(c, t_); }}", True
        if op == 0 and fn == 0x0C:
            return f"recomp_unimplemented(c, 0x{pc:08x}u, 0x{w:08x}u); /* syscall */", False
        self.unimplemented += 1
        return f"recomp_unimplemented(c, 0x{pc:08x}u, 0x{w:08x}u);", False

    def gen_stub(self, addr, nid):
        return f"void f_{addr:08x}(Ctx *c) {{ hle_call(c, 0x{nid:08x}u); }}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", type=Path)
    ap.add_argument("funcs", type=Path, help="CSV de Ghidra (dirección,nombre,tamaño)")
    ap.add_argument("--out", type=Path, default=Path("build/gen"))
    ap.add_argument("--per-file", type=int, default=300)
    ap.add_argument("--check-sp", action="store_true", help="comprobar $sp tras cada escritura (depuración)")
    args = ap.parse_args()

    img = pspelf.load(args.elf)
    text = img.sections[".text"]
    lo, hi = text.addr, text.addr + text.size

    ghidra = []
    for line in args.funcs.read_text().splitlines():
        a, _, size = line.split(",")
        ghidra.append((int(a, 16), int(size)))
    ghidra.sort()
    g_starts = [a for a, _ in ghidra]

    def inside_known(t):
        i = bisect.bisect_right(g_starts, t) - 1
        return i >= 0 and t != ghidra[i][0] and t < ghidra[i][0] + ghidra[i][1]

    # Tablas de salto: series de >= 2 punteros consecutivos en .rodata a código cercano.
    rodata = img.sections[".rodata"]
    code_ptrs = {at: val for at, val in img.data_pointers if lo <= val < hi}
    jt_targets = set()
    for at, val in code_ptrs.items():
        if not rodata.addr <= at < rodata.addr + rodata.size:
            continue
        for nb in (at - 4, at + 4):
            if nb in code_ptrs and abs(code_ptrs[nb] - val) < 0x4000:
                jt_targets.add(val)
                break
    jal_targets = set()
    for pc in range(lo, hi, 4):
        w = img.u32(pc)
        if w >> 26 == 0x03:
            t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if lo <= t < hi:
                jal_targets.add(t)  # un jal siempre apunta a un inicio de función
    starts = {a for a, _ in ghidra if lo <= a < hi} | jal_targets
    starts |= {v for v in code_ptrs.values() if v not in jt_targets}
    # direcciones de código fabricadas con lui/addiu (callbacks): suelen ser funciones; las bases de
    # tablas de salto también se construyen así, pero apuntan a .rodata y no pasan el filtro de .text
    starts |= {a for a in img.code_addresses if lo <= a < hi and a % 4 == 0 and a not in jt_targets}
    starts -= jt_targets - jal_targets
    jump_label_targets = sorted(jt_targets | {v for v in code_ptrs.values() if inside_known(v)} - starts)

    elf_bytes = args.elf.read_bytes()
    stub_nids = {addr + img.base: nid for addr, _, nid in elf_imports(elf_bytes)}

    rc = Recompiler(img, starts, stub_nids)
    rc.check_sp = args.check_sp
    # setjmp/longjmp de la libc del juego: sw/lw $s0, 0($a0); sw/lw $s1, 4($a0)...
    for a in rc.starts:
        w0, w1 = img.u32(a), img.u32(a + 4)
        if (w0, w1) == (0xAC900000, 0xAC910004):
            rc.setjmp_addr = a
        elif (w0, w1) == (0x8C900000, 0x8C910004):
            rc.longjmp_addr = a
    rc.jump_labels = {}
    for t in jump_label_targets:
        i = bisect.bisect_right(rc.starts, t) - 1
        if i >= 0:
            rc.jump_labels.setdefault(rc.starts[i], set()).add(t)

    args.out.mkdir(parents=True, exist_ok=True)
    funcs = rc.starts
    all_funcs = funcs + sorted(stub_nids)
    nfiles = 0
    for k in range(0, len(funcs), args.per_file):
        with open(args.out / f"funcs_{k // args.per_file:03d}.c", "w") as f:
            f.write('#include "funcs.h"\n\n')
            for a in funcs[k:k + args.per_file]:
                f.write(rc.gen_function(a) + "\n\n")
        nfiles += 1
    with open(args.out / "funcs.h", "w") as h:
        h.write('#pragma once\n#include "recomp.h"\n')
        for fn in sorted(rc.vfpu_funcs):
            h.write(f"void {fn}(Ctx *c, uint32_t op);\n")
        for a in all_funcs:
            h.write(f"void f_{a:08x}(Ctx *c);\n")
    with open(args.out / "stubs.c", "w") as f:
        f.write('#include "funcs.h"\n\n')
        for a, nid in sorted(stub_nids.items()):
            f.write(rc.gen_stub(a, nid) + "\n")
    bss_end = img.base + len(img.memory)
    with open(args.out / "table.c", "w") as f:
        f.write('#include "funcs.h"\n\n')
        f.write(f"const uint32_t g_recomp_func_count = {len(all_funcs)};\n")
        f.write(f"const uint32_t g_recomp_entry = 0x{img.entry:08x}u;\n")
        f.write(f"const uint32_t g_recomp_image_base = 0x{img.base:08x}u;\n")
        f.write(f"const uint32_t g_recomp_image_size = 0x{len(img.memory):x}u;\n")
        f.write(f"const uint32_t g_recomp_bss_end = 0x{bss_end:08x}u;\n")
        f.write("const uint32_t g_recomp_func_addrs[] = {\n")
        f.write(",\n".join(f"  0x{a:08x}u" for a in all_funcs) + "\n};\n")
        f.write("const RecompFunc g_recomp_funcs[] = {\n")
        f.write(",\n".join(f"  f_{a:08x}" for a in all_funcs) + "\n};\n")
    names = load_nids(Path(__file__).resolve().parents[2] / "third_party" / "ppsspp" / "Core" / "HLE")
    gp = img.u32(img.sections[".rodata.sceModuleInfo"].addr + 0x20)
    with open(args.out / "imports.c", "w") as f:
        f.write('#include "recomp.h"\n\n')
        f.write(f"const uint32_t g_recomp_gp = 0x{gp:08x}u;\n")
        f.write(f"const uint32_t g_recomp_import_count = {len(stub_nids)};\n")
        f.write("const RecompImport g_recomp_imports[] = {\n")
        f.write(",\n".join(f'  {{0x{nid:08x}u, "{names.get(nid, f"nid_{nid:08X}")}"}}'
                            for nid in sorted(set(stub_nids.values()))) + "\n};\n")
    (args.out / "image.bin").write_bytes(img.memory)
    print(f"setjmp={rc.setjmp_addr and hex(rc.setjmp_addr)} longjmp={rc.longjmp_addr and hex(rc.longjmp_addr)}")
    print(f"{len(funcs)} funciones en {nfiles} ficheros, {len(stub_nids)} stubs; "
          f"VFPU: {rc.vfpu}, sin implementar: {rc.unimplemented}, destinos fuera de función: {rc.bad_targets}")


if __name__ == "__main__":
    main()
