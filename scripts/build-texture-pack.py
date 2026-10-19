"""Builds an HD texture pack for the pc-compat layer from its texture dumps.

Input: TGA files written by native/pc-compat (<W>x<H>_<hash>.tga, alpha = the game's transparency).
Output: <pack>/<hash>.png (RGBA, scaled), plus manifest.json. The layer loads <hash>.png in place of the texture whose
uploaded pixels hash to <hash>. Packs are derived from the user's game data: they live under work/ or next to the
installed game, never in the repository.

Upscaling runs either with Real-ESRGAN (pinned optional toolchain "realesrgan", scripts/setup-toolchains.ps1 -Only
realesrgan) or with any other tool the user prefers (Topaz Gigapixel, their own upscaler): --prepare writes the inputs,
the tool turns <work>/in_color and <work>/in_mask into same-named PNGs, --from-upscaled builds the pack from them.

Colour and transparency are upscaled separately: transparent pixels are first filled with nearby opaque colours
(alpha bleeding) so the model never paints dark halos into outlines, and the upscaled mask is sharpened back to a
crisp, anti-aliased edge. Stencils (every opaque pixel the same colour, e.g. white font glyphs tinted by the game) keep
that exact colour and only their mask is upscaled.

Scales: --scale for everything, --size-scale WxH=N for one texture size (a game's font sheets usually have their own
size; record it in games/<SERIAL>/profile.json pcCompat.texturePack). 8x = a 4x model pass followed by a 2x
realesr-animevideov3 pass, which gives cleaner glyph curves than one pass plus resampling; with mip filtering (the
layer enables it whenever a pack is loaded) the GPU shrinks such sheets cleanly at any resolution.

usage:
  python scripts/build-texture-pack.py --dump <dir> --pack <dir> [--work <dir>] [--model realesr-animevideov3]
                                       [--scale 4] [--size-scale 128x128=8 ...] [--only <hash> ...]
  python scripts/build-texture-pack.py --dump <dir> --work <dir> --prepare
  python scripts/build-texture-pack.py --dump <dir> --pack <dir> --work <dir> --from-upscaled <dir>
Requires Pillow."""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone

from PIL import Image, ImageFilter

NAME = re.compile(r"^(\d+)x(\d+)_([0-9a-f]{16})\.(tga|png)$", re.I)
MODELS = {  # all BSD-3-Clause (xinntao/Real-ESRGAN); non-commercial models are never offered
    "realesr-animevideov3": "realesr-animevideov3",
    "realesrgan-x4plus-anime": "realesrgan-x4plus-anime",
    "realesrgan-x4plus": "realesrgan-x4plus",
}


def default_exe():
    root = os.path.join(os.environ.get("USERPROFILE", ""), "Tools")
    manifest = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "toolchains", "toolchains.json")
    try:
        tools = json.load(open(manifest, encoding="utf-8"))["tools"]
        t = next(t for t in tools if t["id"] == "realesrgan")
        return os.path.join(root, t["bin"], t["probe"][0]), t["version"]
    except (OSError, StopIteration, KeyError, ValueError):
        return os.path.join(root, "realesrgan-ncnn-vulkan-20220424", "realesrgan-ncnn-vulkan.exe"), "unknown"


def bleed(img):
    """RGB copy whose transparent pixels carry the colour of the nearest opaque ones (iterative dilation)."""
    rgba = img.convert("RGBA")
    a = rgba.getchannel("A")
    if a.getextrema() == (255, 255):
        return rgba.convert("RGB"), None
    if a.getextrema()[1] == 0:  # fully transparent: nothing to bleed from
        return Image.new("RGB", rgba.size, (0, 0, 0)), a
    w, h = rgba.size
    px = rgba.load()
    known = [[px[x, y][3] > 127 for x in range(w)] for y in range(h)]
    col = [[px[x, y][:3] for x in range(w)] for y in range(h)]
    frontier = True
    while frontier:
        frontier = False
        add = []
        for y in range(h):
            for x in range(w):
                if known[y][x]:
                    continue
                n = [col[yy][xx] for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1), (y - 1, x - 1), (y - 1, x + 1), (y + 1, x - 1), (y + 1, x + 1))
                     if 0 <= yy < h and 0 <= xx < w and known[yy][xx]]
                if n:
                    add.append((x, y, tuple(sum(c[i] for c in n) // len(n) for i in range(3))))
        for x, y, c in add:
            col[y][x] = c
            known[y][x] = True
            frontier = True
    out = Image.new("RGB", (w, h))
    out.putdata([col[y][x] for y in range(h) for x in range(w)])
    return out, a


def flat_colour(img):
    """The single RGB colour of every opaque pixel, or None (a stencil such as a font sheet has exactly one)."""
    rgba = img.convert("RGBA")
    a = rgba.getchannel("A")
    if a.getextrema()[0] > 127:
        return None  # fully opaque: an ordinary picture
    opaque = {c for c, al in zip(flat(rgba.convert("RGB")), flat(a)) if al > 127}
    return next(iter(opaque)) if len(opaque) == 1 else None


def flat(img):
    return list(img.get_flattened_data() if hasattr(img, "get_flattened_data") else img.getdata())


def prepare(dump, work, scale_of, only=None):
    """Writes upscaler inputs grouped by scale: in_color/x<N>/<hash>.png and in_mask/x<N>/<hash>.png."""
    items = {}
    for f in sorted(os.listdir(dump)):
        m = NAME.match(f)
        if not m:
            continue
        h = m.group(3).lower()
        if h in items or (only and h not in only):
            continue
        size = [int(m.group(1)), int(m.group(2))]
        scale = scale_of(size)
        cin, min_ = os.path.join(work, "in_color", "x%d" % scale), os.path.join(work, "in_mask", "x%d" % scale)
        os.makedirs(cin, exist_ok=True)
        os.makedirs(min_, exist_ok=True)
        img = Image.open(os.path.join(dump, f))
        fc = flat_colour(img)
        rgb, mask = bleed(img) if fc is None else (None, img.convert("RGBA").getchannel("A"))
        if rgb is not None:
            rgb.save(os.path.join(cin, h + ".png"))
        if mask is not None:
            mask.convert("RGB").save(os.path.join(min_, h + ".png"))
        items[h] = {"source": f, "size": size, "scale": scale, "mask": mask is not None,
                    "flat": list(fc) if fc is not None else None}
    json.dump(items, open(os.path.join(work, "items.json"), "w"), indent=1)
    return items


def run_realesrgan(exe, model, src, dst, scale):
    os.makedirs(dst, exist_ok=True)
    if not os.path.isdir(src) or not os.listdir(src):
        return
    if scale == 8:  # 4x with the chosen model, then 2x with the only model that has a 2x variant
        stage = dst.rstrip("\\/") + "_x4"
        run_realesrgan(exe, model, src, stage, 4)
        run_realesrgan(exe, "realesr-animevideov3", stage, dst, 2)
        shutil.rmtree(stage, ignore_errors=True)
        return
    cmd = [exe, "-i", src, "-o", dst, "-n", MODELS[model], "-s", str(scale), "-f", "png"]
    if model == "realesr-animevideov3":
        cmd[cmd.index("-n") + 1] = "realesr-animevideov3-x%d" % scale
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("Real-ESRGAN failed (%d): %s" % (r.returncode, (r.stderr or r.stdout)[-800:]))


MASK_BLUR = {2: 0.5, 3: 0.55, 4: 0.6, 8: 0.8}  # smooths the model's grain before the edge is steepened again


def combine(items, up, pack):
    os.makedirs(pack, exist_ok=True)
    made = 0
    for h, it in items.items():
        scale = it["scale"]
        tw, th = it["size"][0] * scale, it["size"][1] * scale
        if it.get("flat"):
            rgb = Image.new("RGB", (tw, th), tuple(it["flat"]))
        else:
            cpath = os.path.join(up, "color", h + ".png")
            if not os.path.exists(cpath):
                print("missing upscaled colour for", h)
                continue
            rgb = Image.open(cpath).convert("RGB")
        if rgb.size != (tw, th):
            rgb = rgb.resize((tw, th), Image.LANCZOS)
        if it["mask"]:
            mpath = os.path.join(up, "mask", h + ".png")
            if os.path.exists(mpath):
                m = Image.open(mpath).convert("L")
                if m.size != (tw, th):
                    m = m.resize((tw, th), Image.LANCZOS)
            else:  # tool skipped the mask: plain upscale of the original alpha
                src = Image.open(os.path.join(it["dir"], it["source"])).convert("RGBA").getchannel("A")
                m = src.resize((tw, th), Image.BICUBIC)
            # crisp but anti-aliased edge: steepen around the middle grey
            m = m.filter(ImageFilter.GaussianBlur(MASK_BLUR.get(scale, 0.6))).point(lambda v: max(0, min(255, (v - 128) * 3 + 128)))
            out = rgb.convert("RGBA")
            out.putalpha(m)
        else:
            out = rgb.convert("RGBA")
        out.save(os.path.join(pack, h + ".png"), optimize=True)
        made += 1
    return made


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--pack")
    ap.add_argument("--work")
    ap.add_argument("--model", default="realesr-animevideov3", choices=sorted(MODELS))
    ap.add_argument("--scale", type=int, default=4, choices=[2, 3, 4, 8])
    ap.add_argument("--size-scale", nargs="*", default=[], metavar="WxH=N",
                    help="scale for textures of one size, e.g. 128x128=8 for a game's font sheets")
    ap.add_argument("--only", nargs="*", help="limit to these hashes (e.g. to add a few textures to a pack)")
    ap.add_argument("--exe")
    ap.add_argument("--prepare", action="store_true", help="only write the upscaler inputs")
    ap.add_argument("--from-upscaled", help="folder with color/ and mask/ PNGs from another upscaler")
    a = ap.parse_args()
    work = a.work or os.path.join(a.pack or a.dump, "_work")
    per_size = {}
    for spec in a.size_scale:
        m = re.match(r"^(\d+)x(\d+)=(2|3|4|8)$", spec)
        if not m:
            sys.exit("--size-scale expects WxH=N with N in 2, 3, 4, 8: %s" % spec)
        per_size[(int(m.group(1)), int(m.group(2)))] = int(m.group(3))
    for d in ("in_color", "in_mask"):
        shutil.rmtree(os.path.join(work, d), ignore_errors=True)
    only = set(h.lower() for h in a.only) if a.only else None
    items = prepare(a.dump, work, lambda size: per_size.get(tuple(size), a.scale), only)
    for it in items.values():
        it["dir"] = a.dump
    print("%d image(s) prepared in %s" % (len(items), work))
    if a.prepare:
        print("Upscale %s/in_color/x<N> -> <out>/color and %s/in_mask/x<N> -> <out>/mask (same names, N times), then rerun "
              "with --from-upscaled <out>" % (work, work))
        return
    if not a.pack:
        sys.exit("--pack is required")
    if a.from_upscaled:
        up, tool = a.from_upscaled, "external upscaler"
    else:
        exe, ver = (a.exe, "custom") if a.exe else default_exe()
        if not os.path.exists(exe):
            sys.exit("Real-ESRGAN not found at %s (scripts/setup-toolchains.ps1 -Only realesrgan)" % exe)
        up = os.path.join(work, "out")
        if os.path.isdir(up):
            shutil.rmtree(up)
        for scale in sorted(set(it["scale"] for it in items.values())):
            for kind in ("color", "mask"):
                run_realesrgan(exe, a.model, os.path.join(work, "in_" + kind, "x%d" % scale), os.path.join(up, kind), scale)
        tool = "Real-ESRGAN %s, model %s (BSD-3-Clause)" % (ver, a.model)
    made = combine(items, up, a.pack)
    manifest = os.path.join(a.pack, "manifest.json")
    old = {}
    if os.path.exists(manifest):
        try:
            old = json.load(open(manifest, encoding="utf-8"))
        except ValueError:
            old = {}
    builds = old.get("builds", [{k: old[k] for k in ("built", "tool", "scale", "images") if k in old}] if old else [])
    builds.append({"built": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ"), "tool": tool, "scale": a.scale,
                   "sizeScale": a.size_scale, "images": made})
    json.dump({"builds": builds, "note": "Derived from the user's own game data: keep out of any repository."},
              open(manifest, "w"), indent=1)
    print("%d replacement(s) written to %s" % (made, a.pack))


if __name__ == "__main__":
    main()
