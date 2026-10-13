#!/usr/bin/env python3
"""Reescala con IA (Real-ESRGAN, ncnn/Vulkan) las texturas volcadas por el runtime.

Uso: upscale.py DIR_VOLCADO DIR_SALIDA [--model realesrgan-x4plus-anime] [--scale 4]
     [--esrgan ~/tools/realesrgan/realesrgan-ncnn-vulkan]

- Entrada: <hash>.png de `cars --dump-textures`. Salida: <hash>.png para `cars --textures`.
- Antes de reescalar se añade un margen envolvente (wrap) para que las texturas que se repiten
  no muestren costuras, y se recorta después.
- El canal alfa se reescala aparte (como imagen en grises) y se vuelve a unir: así los recortes
  (vallas, banderines, letras) mantienen bordes limpios.
- Solo procesa las texturas que aún no estén en la salida (se puede relanzar de forma incremental).
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image

PAD = 8


def pad_wrap(img, pad):
    w, h = img.size
    out = Image.new(img.mode, (w + 2 * pad, h + 2 * pad))
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            out.paste(img, (pad + dx * w, pad + dy * h))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("dst", type=Path)
    ap.add_argument("--model", default="realesrgan-x4plus-anime")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--esrgan", type=Path, default=Path.home() / "tools/realesrgan/realesrgan-ncnn-vulkan")
    ap.add_argument("--min-size", type=int, default=8, help="no reescalar texturas más pequeñas que esto")
    args = ap.parse_args()

    args.dst.mkdir(parents=True, exist_ok=True)
    todo = [p for p in sorted(args.src.glob("*.png")) if not (args.dst / p.name).exists()]
    print(f"{len(todo)} texturas por reescalar")
    if not todo:
        return
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for d in ("rgb", "alpha", "rgb_up", "alpha_up"):
            (tmp / d).mkdir()
        meta = {}
        for p in todo:
            img = Image.open(p).convert("RGBA")
            w, h = img.size
            if min(w, h) < args.min_size:
                img.save(args.dst / p.name)  # demasiado pequeña: se copia tal cual
                continue
            pad = min(PAD, w, h)
            rgb, alpha = img.convert("RGB"), img.getchannel("A")
            pad_wrap(rgb, pad).save(tmp / "rgb" / p.name)
            has_alpha = alpha.getextrema() != (255, 255)
            if has_alpha:
                pad_wrap(alpha, pad).convert("RGB").save(tmp / "alpha" / p.name)
            meta[p.name] = (w, h, pad, has_alpha)

        def esrgan(src, dst):
            if not any(src.iterdir()):
                return
            subprocess.run([str(args.esrgan), "-i", str(src), "-o", str(dst), "-n", args.model,
                            "-s", str(args.scale), "-m", str(args.esrgan.parent / "models"), "-f", "png"],
                           check=True, stdout=subprocess.DEVNULL)

        esrgan(tmp / "rgb", tmp / "rgb_up")
        esrgan(tmp / "alpha", tmp / "alpha_up")

        s = args.scale
        for name, (w, h, pad, has_alpha) in meta.items():
            box = (pad * s, pad * s, (pad + w) * s, (pad + h) * s)
            rgb = Image.open(tmp / "rgb_up" / name).convert("RGB").crop(box)
            if has_alpha:
                a = Image.open(tmp / "alpha_up" / name).convert("L").crop(box)
            else:
                a = Image.new("L", rgb.size, 255)
            out = rgb.copy()
            out.putalpha(a)
            out.save(args.dst / name)
        print(f"reescaladas {len(meta)} texturas x{s} con {args.model} en {args.dst}")


if __name__ == "__main__":
    main()
