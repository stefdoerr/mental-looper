#!/usr/bin/env python3
"""
Bakes the Mental Looper face's assets (design: docs/superpowers/specs/2026-11-25-mental-looper-face-design.md; the
mockup next to it is the reference for every shape):

  fonts()  Anton and Archivo (SIL OFL) from the google/fonts repo, subset to woff2 in fonts/, with their licences.
  title()  "MENTAL LOOPER" in cut-out letters as outlines in face/title.svg: the mockup's ransom() with seed 11 and the
           Fluoro looks, so the face ships no fonts for it.

Fonts are downloaded into build/face-fonts/ (not committed). Run from the repo root:
    make face-assets     (uv run --with fonttools --with brotli python3 plugins/Ouroboros/modgui/generate_face.py)
"""

import math
import os
import re
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CACHE = os.path.join(ROOT, "build", "face-fonts")
GF = "https://github.com/google/fonts/raw/main/"

# The mockup's titleHTML(…, rn=true): [background, letter] per look; ransom()'s fonts in its order.
LOOKS = [("#fff", "#111"), ("#111", "#f6ec1a"), ("#ff3d8b", "#111"), ("#111", "#fff"), ("#fff", "#ff3d8b"),
         ("#e8e2cc", "#111")]
TITLE_FONTS = ["ofl/anton/Anton-Regular.ttf", "ofl/abrilfatface/AbrilFatface-Regular.ttf",
               "ofl/courierprime/CourierPrime-Bold.ttf", "ofl/bungee/Bungee-Regular.ttf",
               "ofl/playfairdisplay/PlayfairDisplay[wght].ttf", "ofl/rubikmonoone/RubikMonoOne-Regular.ttf",
               "apache/specialelite/SpecialElite-Regular.ttf"]
TITLE = "MENTAL LOOPER"
W, H = 520, 48               # title.svg; the face places it at left 0, top 23 (the row's centre at y 47)


def fetch(path):
    """A file of the google/fonts repo, cached in build/face-fonts/."""
    out = os.path.join(CACHE, path.replace("/", "_"))
    if not os.path.exists(out):
        os.makedirs(CACHE, exist_ok=True)
        with urllib.request.urlopen(GF + urllib.parse.quote(path)) as r, open(out + ".part", "wb") as f:
            f.write(r.read())
        os.replace(out + ".part", out)
    return out


def fonts():
    from fontTools import subset
    os.makedirs(os.path.join(HERE, "fonts"), exist_ok=True)
    unicodes = list(range(0x20, 0x7F)) + [0xD7, 0x2212, 0xB7]
    for src, dst, lic, name in [("ofl/anton/Anton-Regular.ttf", "anton-400.woff2", "ofl/anton/OFL.txt", "Anton"),
                                ("ofl/archivo/Archivo[wdth,wght].ttf", "archivo-var.woff2", "ofl/archivo/OFL.txt",
                                 "Archivo")]:
        opts = subset.Options()
        opts.flavor = "woff2"
        opts.layout_features = ["*"]
        font = subset.load_font(fetch(src), opts)
        sub = subset.Subsetter(opts)
        sub.populate(unicodes=unicodes)
        sub.subset(font)
        subset.save_font(font, os.path.join(HERE, "fonts", dst), opts)
        with open(fetch(lic), "rb") as f, open(os.path.join(HERE, "fonts", "OFL-%s.txt" % name), "wb") as g:
            g.write(f.read())


def title_font(path):
    from fontTools.ttLib import TTFont
    font = TTFont(fetch(path))
    if "fvar" in font:       # Playfair Display: the 900 the mockup asks for
        from fontTools.varLib import instancer
        font = instancer.instantiateVariableFont(font, {"wght": 900})
    return font


def title():
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen

    fonts_ = [title_font(p) for p in TITLE_FONTS]
    seed = 11

    def rnd():
        nonlocal seed
        seed = seed * 16807 % 2147483647
        return seed / 2147483647

    def jsround(x):
        return math.floor(x + 0.5)

    # The mockup's ransom(): per letter the font (never the last one twice), the look, size, tilt and drop.
    scraps, last = [], -1
    for ch in TITLE:
        if ch == " ":
            scraps.append(None)
            continue
        while True:
            fi = math.floor(rnd() * len(fonts_))
            if fi != last:
                break
        last = fi
        bg, fg = LOOKS[math.floor(rnd() * len(LOOKS))]
        size = 15 + jsround(rnd() * 6)
        rot = round(rnd() * 14 - 7, 1)
        dy = round(rnd() * 4 - 2, 1)
        font = fonts_[fi]
        gname = font.getBestCmap()[ord(ch)]
        gs = font.getGlyphSet()
        s = size / font["head"].unitsPerEm
        adv = font["hmtx"][gname][0] * s
        bp = BoundsPen(gs)
        gs[gname].draw(bp)
        xmin, ymin, xmax, ymax = bp.bounds
        w, h = adv + 6, size + 3
        pen = SVGPathPen(gs, ntos=lambda v: ("%.2f" % v).rstrip("0").rstrip("."))
        # The glyph's box centred in the scrap (y flipped).
        tx, ty = -((xmin + xmax) / 2) * s, ((ymin + ymax) / 2) * s
        gs[gname].draw(TransformPen(pen, (s, 0, 0, -s, tx, ty)))
        scraps.append(dict(w=w, h=h, rot=rot, dy=dy, bg=bg, fg=fg, d=pen.getCommands()))

    total = sum(8 if c is None else c["w"] for c in scraps) + 2 * len(scraps)
    x = (W - total) / 2 + 1
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d">' % (W, H, W, H)]
    for c in scraps:
        if c is None:
            x += 8 + 2
            continue
        cx, cy, w, h = x + c["w"] / 2, H / 2 + c["dy"], c["w"], c["h"]
        out.append('<g class="l" transform="translate(%.2f %.2f) rotate(%.1f)">' % (cx, cy, c["rot"]))
        out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="#111"/>' % (-w / 2 + 1.5, -h / 2 + 1.5, w, h))
        out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"/>' % (-w / 2, -h / 2, w, h, c["bg"]))
        out.append('<path fill="%s" d="%s"/></g>' % (c["fg"], c["d"]))
        x += c["w"] + 2
    out.append("</svg>")
    os.makedirs(os.path.join(HERE, "face"), exist_ok=True)
    with open(os.path.join(HERE, "face", "title.svg"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def check():
    svg = open(os.path.join(HERE, "face", "title.svg"), encoding="utf-8").read()
    root = ET.fromstring(svg)
    letters = [g for g in root if g.get("class") == "l"]
    assert len(letters) == len(TITLE.replace(" ", "")), len(letters)
    assert "<text" not in svg and "font-family" not in svg and "id=" not in svg
    allowed = {c for look in LOOKS for c in look} | {"#111"}
    assert set(re.findall(r'fill="([^"]+)"', svg)) <= allowed, set(re.findall(r'fill="([^"]+)"', svg)) - allowed
    for f in ("anton-400.woff2", "archivo-var.woff2"):
        assert open(os.path.join(HERE, "fonts", f), "rb").read(4) == b"wOF2", f
    for f in ("OFL-Anton.txt", "OFL-Archivo.txt"):
        assert "SIL OPEN FONT LICENSE" in open(os.path.join(HERE, "fonts", f), encoding="utf-8").read().upper(), f
    print("ok")


if __name__ == "__main__":
    import sys
    if sys.argv[1:] != ["--check"]:
        fonts()
        title()
    check()
