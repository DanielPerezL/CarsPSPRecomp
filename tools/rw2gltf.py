#!/usr/bin/env python3
"""Exporta los clumps y worlds de los streams .DFF de Cars PSP a glTF binario (.glb) con texturas.

Uso: rw2gltf.py STREAM.DFF [...] [--tex OTRO.DFF ...] [--out dumps/models]
Las texturas se buscan por nombre en los diccionarios del propio stream y en los de --tex.
"""
import argparse
import io
import json
import struct
from pathlib import Path

from PIL import Image

import rwpsp
from rwstream import unpack


class GlbBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.gltf = {"asset": {"version": "2.0", "generator": "Cars tools/rw2gltf.py"},
                     "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
                     "accessors": [], "bufferViews": [], "materials": [], "textures": [],
                     "images": [], "samplers": [{"magFilter": 9729, "minFilter": 9987}]}
        self.texture_index = {}
        self.material_index = {}

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            bv["target"] = target
        self.bin += data
        self.gltf["bufferViews"].append(bv)
        return len(self.gltf["bufferViews"]) - 1

    def accessor(self, values, kind, ctype=5126, normalized=False, target=34962):
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[kind]
        fmt = {5126: "f", 5121: "B", 5125: "I"}[ctype]
        flat = [c for v in values for c in (v if n > 1 else (v,))]
        acc = {"bufferView": self.view(struct.pack("<%d%s" % (len(flat), fmt), *flat), target),
               "componentType": ctype, "count": len(values), "type": kind}
        if normalized:
            acc["normalized"] = True
        if kind == "VEC3" and ctype == 5126:
            acc["min"] = [min(v[k] for v in values) for k in range(3)]
            acc["max"] = [max(v[k] for v in values) for k in range(3)]
        self.gltf["accessors"].append(acc)
        return len(self.gltf["accessors"]) - 1

    def texture(self, tex):
        if tex.name not in self.texture_index:
            png = io.BytesIO()
            Image.frombytes("RGBA", (tex.width, tex.height), tex.rgba).save(png, "PNG")
            self.gltf["images"].append({"name": tex.name, "mimeType": "image/png",
                                        "bufferView": self.view(png.getvalue())})
            self.gltf["textures"].append({"source": len(self.gltf["images"]) - 1, "sampler": 0})
            self.texture_index[tex.name] = len(self.gltf["textures"]) - 1
        return self.texture_index[tex.name]

    def material(self, tex_name, color, textures):
        key = (tex_name, color)
        if key not in self.material_index:
            mat = {"name": tex_name or "untextured", "doubleSided": False,
                   "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0,
                                            "baseColorFactor": [c / 255 for c in color]}}
            tex = textures.get(tex_name.lower())
            if tex is not None:
                mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": self.texture(tex)}
                if any(a < 250 for a in tex.rgba[3::4]):
                    mat["alphaMode"] = "MASK" if all(a in (0, 255) for a in tex.rgba[3::4]) else "BLEND"
            self.gltf["materials"].append(mat)
            self.material_index[key] = len(self.gltf["materials"]) - 1
        return self.material_index[key]

    def mesh(self, name, geom, textures):
        prims = []
        for sub in geom.submeshes:
            if not sub.triangles:
                continue
            attrs = {"POSITION": self.accessor(sub.positions, "VEC3")}
            if sub.normals:
                attrs["NORMAL"] = self.accessor([_normalize(n) for n in sub.normals], "VEC3")
            if sub.uvs:
                attrs["TEXCOORD_0"] = self.accessor(sub.uvs, "VEC2")
            if sub.colors:
                attrs["COLOR_0"] = self.accessor(sub.colors, "VEC4", 5121, normalized=True)
            idx = self.accessor([i for t in sub.triangles for i in t], "SCALAR", 5125, target=34963)
            mi = sub.material if sub.material < len(geom.materials) else 0
            prims.append({"attributes": attrs, "indices": idx,
                          "material": self.material(geom.materials[mi], geom.material_colors[mi], textures)})
        if not prims:
            return None
        self.gltf["meshes"].append({"name": name, "primitives": prims})
        return len(self.gltf["meshes"]) - 1

    def add_clump(self, name, clump, textures):
        base = len(self.gltf["nodes"])
        for i, f in enumerate(clump.frames):
            self.gltf["nodes"].append({"name": f.name if f.name != f"frame{i}" else f"{name}_{i}",
                                       "matrix": list(f.matrix)})
        for i, f in enumerate(clump.frames):
            if f.parent >= 0:
                self.gltf["nodes"][base + f.parent].setdefault("children", []).append(base + i)
            else:
                self.gltf["scenes"][0]["nodes"].append(base + i)
        for k, (frame, gi) in enumerate(clump.atomics):
            if gi >= len(clump.geometries):
                continue
            mesh = self.mesh(f"{name}_{k}", clump.geometries[gi], textures)
            if mesh is None:
                continue
            node = {"name": f"{name}_atomic{k}", "mesh": mesh}
            self.gltf["nodes"].append(node)
            self.gltf["nodes"][base + frame].setdefault("children", []).append(len(self.gltf["nodes"]) - 1)

    def add_world(self, name, sectors, textures):
        root = len(self.gltf["nodes"])
        self.gltf["nodes"].append({"name": name, "children": []})
        self.gltf["scenes"][0]["nodes"].append(root)
        for k, geom in enumerate(sectors):
            mesh = self.mesh(f"{name}_sector{k}", geom, textures)
            if mesh is not None:
                self.gltf["nodes"].append({"name": f"{name}_sector{k}", "mesh": mesh})
                self.gltf["nodes"][root]["children"].append(len(self.gltf["nodes"]) - 1)

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.gltf["buffers"] = [{"byteLength": len(self.bin)}]
        for key in ("textures", "images", "materials"):
            if not self.gltf[key]:
                del self.gltf[key]
        js = json.dumps(self.gltf, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        with open(path, "wb") as f:
            f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
            f.write(struct.pack("<II", len(self.bin), 0x004E4942) + self.bin)


def _normalize(n):
    ln = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5 or 1.0
    return n[0] / ln, n[1] / ln, n[2] / ln


def load_textures(streams):
    textures = {}
    for stream in streams:
        for kind, _, ctype, body in rwpsp.rws_assets(stream):
            if ctype == 0x16:
                for tex in rwpsp.parse_texdict(body):
                    textures.setdefault(tex.name.lower(), tex)
    return textures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("streams", nargs="+", type=Path)
    ap.add_argument("--tex", nargs="*", type=Path, default=[], help="streams extra con texturas compartidas")
    ap.add_argument("--out", type=Path, default=Path("dumps/models"))
    ap.add_argument("--only", help="exportar solo los assets cuyo nombre contenga esto")
    args = ap.parse_args()

    extra = [unpack(p.read_bytes()) for p in args.tex]
    for path in args.streams:
        stream = unpack(path.read_bytes())
        textures = load_textures([stream] + extra)
        dest = args.out / path.stem
        used = set()
        for kind, name, ctype, body in rwpsp.rws_assets(stream):
            if ctype not in (0x10, 0x0B) or (args.only and args.only.lower() not in name.lower()):
                continue
            builder = GlbBuilder()
            try:
                if ctype == 0x10:
                    builder.add_clump(name or "clump", rwpsp.parse_clump(body), textures)
                else:
                    builder.add_world(name or "world", rwpsp.parse_world(body), textures)
            except Exception as e:  # formato aún no cubierto: se informa y se sigue
                print(f"  {path.stem}/{name}: error {type(e).__name__}: {e}")
                continue
            if not builder.gltf["meshes"]:
                continue
            fname = name or "clump"
            n = 1
            while fname in used:
                n += 1
                fname = f"{name}_{n}"
            used.add(fname)
            dest.mkdir(parents=True, exist_ok=True)
            builder.write(dest / f"{fname}.glb")
        print(f"{path.stem}: {len(used)} modelos")


if __name__ == "__main__":
    main()
