#!/usr/bin/env python3
"""Lectura de assets RenderWare 3.7 nativos de PSP tal como vienen en los streams de Cars.

Geometría nativa (Geometry con flag 0x01000000 + NativeDataPLG 0x510):
  NativeDataPLG = Struct(u32 plataforma = 0x0A, datos). Los offsets internos cuentan desde
  datos+0 (= inicio del Struct + 4). En datos+0x20 hay un registro de 0x58 bytes por
  submalla del BinMeshPLG:
    +0x04 display list de la GE (clumps; en sectores de world va en +0x50)
    +0x38 offset de la matriz de descompresión
    +0x40 escala U, escala V, offset U, offset V (las UV vienen cuantizadas)
  La display list (VTYPE/BASE/VADDR/IADDR/PRIM) describe los vértices; la matriz 4x4
  convierte las posiciones normalizadas de la GE a espacio de modelo.

Texturas (TextureNative 0x15, Struct):
  +0x00 formato RW (0x2000 PAL8, 0x4000 PAL4, 0x8000 mipmaps; bits 8-11: 0x2 565,
        0x3 4444, 0x5 8888, 0x6 888), +0x04 u16 ancho, u16 alto, +0x08 u8 bpp, u8 mips,
  +0x6C nombre[32], +0x8C máscara[32], +0xAC paleta (RGBA8888) y píxeles swizzled
  (bloques de 16 bytes x 8 filas), filas de al menos 16 bytes.
"""
import re
import struct
from dataclasses import dataclass, field

LIB_37 = 0x1C02
ASSET_TYPE = re.compile(rb"((?:rwID|PCPL)_[A-Z0-9_]+)\x00")


# ---------------------------------------------------------------- chunks

def _header_ok(data, off, end):
    if off < 0 or off + 12 > end:
        return False
    ctype, size, lib = struct.unpack_from("<III", data, off)
    return lib >> 16 == LIB_37 and ctype < 0x10000000 and off + 12 + size <= end


def read_chunks(data, start=0, end=None):
    """Chunks RW 3.7 consecutivos en data[start:end] -> [(tipo, cuerpo)].

    Algunos exportes declaran tamaños con unos bytes de más o de menos (p. ej. el
    GeometryList de McQueen, +3): si la cabecera siguiente no es válida se busca a ±8 bytes.
    """
    end = len(data) if end is None else end
    out = []
    off = start
    while off + 12 <= end:
        if not _header_ok(data, off, end):
            fixed = next((off + d for d in (-1, -2, -3, -4, 1, 2, 3, 4, -5, -6, -7, -8, 5, 6, 7, 8)
                          if off + d >= start and _header_ok(data, off + d, end)), None)
            if fixed is None:
                break
            off = fixed
        ctype, size, _ = struct.unpack_from("<III", data, off)
        out.append((ctype, data[off + 12:off + 12 + size]))
        off += 12 + size
    return out


def child(chunks, ctype):
    return next((body for t, body in chunks if t == ctype), None)


def asset_name(body):
    """Nombre del asset (u32 longitud + cadena en +4); si está vacío, el fichero de origen."""
    n, = struct.unpack_from("<I", body, 4)
    name = body[8:8 + n].split(b"\0")[0].decode("latin1")
    if name:
        return name
    m = re.search(rb"project_assets\\[^\0]*?([^\\\0]+)\.\w+\0", body[:512], re.I)
    return m.group(1).decode("latin1") if m else ""


def rws_assets(stream):
    """Assets (0x716) de un stream de RW Studio: (tipo, nombre, tipo_chunk_nativo, cuerpo_nativo)."""
    from rwstream import chunks as stream_chunks
    for off, ctype, size, _ in stream_chunks(stream):
        if ctype != 0x716:
            continue
        body = stream[off + 12:off + 12 + size]
        m = ASSET_TYPE.search(body, 0, 256)
        kind = m.group(1).decode() if m else "?"
        name = asset_name(body)
        start = (m.end() + 3) & ~3 if m else 0
        for i in range(start, len(body) - 12, 4):
            t, s, lib = struct.unpack_from("<III", body, i)
            if lib >> 16 == LIB_37 and t < 0x1000 and i + 12 + s <= len(body):
                yield kind, name, t, body[i + 12:i + 12 + s]
                break


# ---------------------------------------------------------------- texturas

@dataclass
class Texture:
    name: str
    width: int
    height: int
    rgba: bytes


def unswizzle(data, row_bytes, height):
    out = bytearray(len(data))
    blocks_x = row_bytes // 16
    i = 0
    for by in range(0, height, 8):
        for bx in range(blocks_x):
            for r in range(min(8, height - by)):
                o = (by + r) * row_bytes + bx * 16
                out[o:o + 16] = data[i:i + 16]
                i += 16
    return bytes(out)


def _rgb565(v):
    r, g, b = v & 0x1F, (v >> 5) & 0x3F, (v >> 11) & 0x1F
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255)


def _rgba4444(v):
    return tuple((v >> s & 0xF) * 17 for s in (0, 4, 8, 12))


def parse_texture_native(body):
    st = child(read_chunks(body), 1)
    fmt, = struct.unpack_from("<I", st, 0)
    w, h = struct.unpack_from("<HH", st, 4)
    bpp = st[8]
    name = st[0x6C:0x8C].split(b"\0")[0].decode("latin1")
    pos = 0xAC
    palette = None
    if bpp in (4, 8):
        n = 1 << bpp
        palette = [struct.unpack_from("<4B", st, pos + k * 4) for k in range(n)]
        pos += n * 4
    row_bytes = max(w * bpp // 8, 16)
    rows = max(h, 8)
    raw = st[pos:pos + row_bytes * rows]
    raw = unswizzle(raw.ljust(row_bytes * ((rows + 7) // 8 * 8), b"\0"), row_bytes, rows)
    out = bytearray()
    for y in range(h):
        row = raw[y * row_bytes:(y + 1) * row_bytes]
        for x in range(w):
            if bpp == 8:
                px = palette[row[x]]
            elif bpp == 4:
                px = palette[row[x >> 1] >> (4 * (x & 1)) & 0xF]
            elif bpp == 16:
                v, = struct.unpack_from("<H", row, x * 2)
                px = _rgba4444(v) if fmt & 0xF00 == 0x300 else _rgb565(v)
            else:
                px = tuple(row[x * 4:x * 4 + 4])
            if fmt & 0xF00 == 0x600:  # 888: sin alfa
                px = (*px[:3], 255)
            out += bytes(px)
    return Texture(name, w, h, bytes(out))


def parse_texdict(body):
    return [parse_texture_native(b) for t, b in read_chunks(body) if t == 0x15]


# ---------------------------------------------------------------- vértices GE

@dataclass
class Submesh:
    material: int
    positions: list
    normals: list
    uvs: list
    colors: list
    weights: list
    triangles: list  # índices locales


_SIZE = {1: 1, 2: 2, 3: 4}


def _color(fmt, raw):
    if fmt == 7:
        return tuple(raw[:4])
    v, = struct.unpack_from("<H", raw)
    if fmt == 4:
        return _rgb565(v)
    if fmt == 5:
        return (v & 0x1F) * 255 // 31, (v >> 5 & 0x1F) * 255 // 31, (v >> 10 & 0x1F) * 255 // 31, 255 * (v >> 15)
    return _rgba4444(v)


def vertex_layout(vtype):
    """Layout de vértice de la GE: [(campo, offset, tamaño elemento, n)], stride."""
    tc, col, nrm, pos = vtype & 3, vtype >> 2 & 7, vtype >> 5 & 3, vtype >> 7 & 3
    wt, wcount = vtype >> 9 & 3, (vtype >> 14 & 7) + 1
    layout = []
    off, max_align = 0, 1

    def add(kind, esize, n):
        nonlocal off, max_align
        off = (off + esize - 1) // esize * esize
        layout.append((kind, off, esize, n))
        off += esize * n
        max_align = max(max_align, esize)

    if wt:
        add("w", _SIZE[wt], wcount)
    if tc:
        add("t", _SIZE[tc], 2)
    if col:
        add("c", 4 if col == 7 else 2, 1)
    if nrm:
        add("n", _SIZE[nrm], 3)
    if pos:
        add("p", _SIZE[pos], 3)
    return layout, (off + max_align - 1) // max_align * max_align


def decode_vertices(data, vaddr, count, vtype):
    layout, stride = vertex_layout(vtype)
    col = vtype >> 2 & 7

    def comps(raw, kind, esize, n):
        fmt = {1: "b", 2: "h", 4: "f"}[esize]
        if kind in ("t", "w") and esize < 4:
            fmt = fmt.upper()
        vals = struct.unpack_from("<%d%s" % (n, fmt), raw)
        scale = {1: 128.0, 2: 32768.0, 4: 1.0}[esize]
        return [v / scale for v in vals]

    out = {"p": [], "n": [], "t": [], "c": [], "w": []}
    for i in range(count):
        base = vaddr + i * stride
        for kind, o, esize, n in layout:
            raw = data[base + o:base + o + esize * n]
            if kind == "c":
                out["c"].append(_color(col, raw))
            else:
                out[kind].append(comps(raw, kind, esize, n))
    return out


def run_display_list(data, dl_off):
    """Ejecuta una display list de la GE y devuelve las llamadas de dibujo."""
    draws = []
    vtype, vaddr, iaddr, base = 0, 0, 0, 0
    pc = dl_off
    for _ in range(4096):
        if pc + 4 > len(data):
            break
        word, = struct.unpack_from("<I", data, pc)
        cmd, arg = word >> 24, word & 0xFFFFFF
        pc += 4
        if cmd == 0x12:
            vtype = arg
        elif cmd == 0x10:
            base = (arg << 8) & 0xFF000000
        elif cmd == 0x01:
            vaddr = base | arg
        elif cmd == 0x02:
            iaddr = base | arg
        elif cmd == 0x04:
            prim, count = arg >> 16 & 7, arg & 0xFFFF
            draws.append((prim, count, vtype, vaddr, iaddr))
            idx = vtype >> 11 & 3
            if idx:
                iaddr += count * _SIZE[idx]
            else:
                vaddr += count * vertex_layout(vtype)[1]
        elif cmd in (0x0B, 0x0C):
            break
    return draws


def parse_native_geometry(native, mesh_materials, strips=True):
    """NativeDataPLG PSP -> lista de Submesh (posiciones ya en espacio de modelo)."""
    st = child(read_chunks(native), 1)
    if st is None or struct.unpack_from("<I", st, 0)[0] != 0x0A:
        return []
    data = st[4:]
    meshes = []
    for i, mat in enumerate(mesh_materials):
        rec = 0x20 + i * 0x58
        # la display list está en +0x04 (clumps) o +0x50 (sectores de world); la otra ranura vale -1
        draws = []
        for slot in (0x04, 0x50):
            dl_off, = struct.unpack_from("<I", data, rec + slot)
            if dl_off < len(data) - 4:
                draws = run_display_list(data, dl_off)
                if draws:
                    break
        if not draws:
            # sin display list precompilada (el juego la monta en ejecución, p. ej. UV animadas):
            # +0x10 VTYPE, +0x18 nº de vértices, +0x1C offset de vértices
            vtype, _, count, vaddr = struct.unpack_from("<4I", data, rec + 0x10)
            draws = [(4 if strips else 3, count, vtype, vaddr, 0)]
        mat_off, = struct.unpack_from("<I", data, rec + 0x38)
        su, sv, ou, ov = struct.unpack_from("<4f", data, rec + 0x40)
        m = struct.unpack_from("<16f", data, mat_off)
        sub = Submesh(mat, [], [], [], [], [], [])
        for prim, count, vtype, vaddr, iaddr in draws:
            idx = vtype >> 11 & 3
            if idx:
                fmt = {1: "B", 2: "H", 3: "I"}[idx]
                order = struct.unpack_from("<%d%s" % (count, fmt), data, iaddr)
                v = decode_vertices(data, vaddr, max(order) + 1, vtype)
            else:
                order = range(count)
                v = decode_vertices(data, vaddr, count, vtype)
            first = len(sub.positions)
            for p in v["p"]:
                x, y, z = p
                sub.positions.append((x * m[0] + y * m[4] + z * m[8] + m[12],
                                      x * m[1] + y * m[5] + z * m[9] + m[13],
                                      x * m[2] + y * m[6] + z * m[10] + m[14]))
            for n in v["n"]:
                sub.normals.append(tuple(n))
            for u, vv in v["t"]:
                sub.uvs.append((u * su + ou, vv * sv + ov))
            sub.colors += v["c"]
            sub.weights += v["w"]
            seq = [first + k for k in order]
            if prim == 4:
                sub.triangles += [(seq[k], seq[k + 1], seq[k + 2]) if k % 2 == 0 else (seq[k + 1], seq[k], seq[k + 2])
                                  for k in range(len(seq) - 2)]
            elif prim == 3:
                sub.triangles += [tuple(seq[k:k + 3]) for k in range(0, len(seq) - 2, 3)]
            elif prim == 5:
                sub.triangles += [(seq[0], seq[k], seq[k + 1]) for k in range(1, len(seq) - 1)]
        P = sub.positions
        sub.triangles = [t for t in sub.triangles if P[t[0]] != P[t[1]] and P[t[1]] != P[t[2]] and P[t[0]] != P[t[2]]]
        meshes.append(sub)
    return meshes


# ---------------------------------------------------------------- clumps

@dataclass
class Geometry:
    materials: list          # nombre de textura por material ("" = sin textura)
    material_colors: list
    submeshes: list


@dataclass
class Frame:
    name: str
    matrix: tuple            # 4x4 por columnas (glTF)
    parent: int


@dataclass
class Clump:
    frames: list = field(default_factory=list)
    geometries: list = field(default_factory=list)
    atomics: list = field(default_factory=list)  # (frame, geometría)


def parse_material(body):
    ch = read_chunks(body)
    st = child(ch, 1)
    r, g, b, a = st[4:8]
    tex = child(ch, 6)
    name = ""
    if tex is not None:
        strings = [b for t, b in read_chunks(tex) if t == 2]
        if strings:
            name = strings[0].split(b"\0")[0].decode("latin1")
    return name, (r, g, b, a)


def parse_matlist(body):
    """MaterialList -> [(nombre de textura, color RGBA)]; -1 = material nuevo, n = repetido."""
    matlist = read_chunks(body)
    mats = iter(parse_material(b) for t, b in matlist if t == 7)
    st = matlist[0][1]
    count, = struct.unpack_from("<I", st, 0)
    resolved = []
    for ref in struct.unpack_from("<%di" % count, st, 4):
        resolved.append(next(mats) if ref < 0 else resolved[ref])
    return resolved


def native_submeshes(extension, material_base=0):
    ext = read_chunks(extension or b"")
    binmesh, native = child(ext, 0x50E), child(ext, 0x510)
    if not binmesh or not native:
        return []
    flags, nmesh, _ = struct.unpack_from("<III", binmesh, 0)
    mesh_mats = [material_base + struct.unpack_from("<II", binmesh, 12 + k * 8)[1] for k in range(nmesh)]
    return parse_native_geometry(native, mesh_mats, strips=bool(flags & 1))


def plain_submeshes(st, nmats):
    """Geometría RW estándar (no nativa): vértices y triángulos en el propio Struct."""
    flags, ntris, nverts, _ = struct.unpack_from("<IIII", st, 0)
    ntex = flags >> 16 & 0xFF or (1 if flags & 0x04 else 0) | (2 if flags & 0x80 else 0)
    pos = 16
    colors, uvs = [], []
    if flags & 0x08:
        colors = [tuple(st[pos + i * 4:pos + i * 4 + 4]) for i in range(nverts)]
        pos += nverts * 4
    for t in range(ntex):
        if t == 0:
            uvs = [struct.unpack_from("<2f", st, pos + i * 8) for i in range(nverts)]
        pos += nverts * 8
    tris = []
    for i in range(ntris):
        w0, w1 = struct.unpack_from("<II", st, pos + i * 8)
        tris.append((w0 >> 16, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF))  # v0, v1, v2, material
    pos += ntris * 8
    pos += 16  # esfera envolvente del morph target 0
    has_pos, has_nrm = struct.unpack_from("<II", st, pos)
    pos += 8
    positions = [struct.unpack_from("<3f", st, pos + i * 12) for i in range(nverts)] if has_pos else []
    pos += nverts * 12 if has_pos else 0
    normals = [struct.unpack_from("<3f", st, pos + i * 12) for i in range(nverts)] if has_nrm else []
    subs = []
    for mat in range(max(nmats, 1)):
        mine = [(a, b, c) for a, b, c, m in tris if m == mat]
        if mine and positions:
            subs.append(Submesh(mat, positions, normals, uvs, colors, [], mine))
    return subs


def parse_geometry(body):
    ch = read_chunks(body)
    mats = parse_matlist(child(ch, 8))
    st = child(ch, 1)
    flags, = struct.unpack_from("<I", st, 0)
    subs = native_submeshes(child(ch, 3)) if flags & 0x01000000 else plain_submeshes(st, len(mats))
    return Geometry([m[0] for m in mats], [m[1] for m in mats], subs)


def parse_world(body):
    """World BSP -> una Geometry por AtomicSector (todas comparten la lista de materiales)."""
    ch = read_chunks(body)
    mats = parse_matlist(child(ch, 8))
    names, colors = [m[0] for m in mats], [m[1] for m in mats]
    sectors = []

    def walk(chunks):
        for t, b in chunks:
            if t == 0x0A:
                walk(read_chunks(b)[1:])
            elif t == 0x09:
                sch = read_chunks(b)
                base, = struct.unpack_from("<I", child(sch, 1), 0)
                subs = native_submeshes(child(sch, 3), base)
                if subs:
                    sectors.append(Geometry(names, colors, subs))

    walk(ch)
    return sectors


def parse_clump(body):
    ch = read_chunks(body)
    clump = Clump()
    frames = read_chunks(child(ch, 0x0E))
    fst = frames[0][1]
    nframes, = struct.unpack_from("<I", fst, 0)
    names = []
    for t, b in frames[1:]:
        if t == 3:
            nm = child(read_chunks(b), 0x253F2FE)
            names.append(nm.split(b"\0")[0].decode("latin1") if nm else "")
    for i in range(nframes):
        r = struct.unpack_from("<12f", fst, 4 + i * 56)
        parent, = struct.unpack_from("<i", fst, 4 + i * 56 + 48)
        matrix = (r[0], r[1], r[2], 0, r[3], r[4], r[5], 0, r[6], r[7], r[8], 0, r[9], r[10], r[11], 1)
        clump.frames.append(Frame(names[i] if i < len(names) and names[i] else f"frame{i}", matrix, parent))
    glist = child(ch, 0x1A)
    if glist:
        clump.geometries = [parse_geometry(b) for t, b in read_chunks(glist) if t == 0x0F]
    for t, b in ch:
        if t != 0x14:
            continue
        ach = read_chunks(b)
        frame, geom = struct.unpack_from("<II", child(ach, 1), 0)
        inline = child(ach, 0x0F)
        if inline is not None:
            clump.geometries.append(parse_geometry(inline))
            geom = len(clump.geometries) - 1
        clump.atomics.append((frame, geom))
    return clump
