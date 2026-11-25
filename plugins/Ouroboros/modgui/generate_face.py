#!/usr/bin/env python3
"""
Bakes the Mental Looper face's assets (design: docs/superpowers/specs/2026-11-25-mental-looper-face-design.md; the
mockup next to it is the reference for every shape):

  fonts()  Anton and Archivo (SIL OFL) from the google/fonts repo, subset to woff2 in fonts/, with their licences.
  title()  "MENTAL LOOPER" in cut-out letters as outlines in face/title.svg: the mockup's ransom() with seed 11 and the
           Fluoro looks, so the face ships no fonts for it.
  head()   the Mohawk head as inline SVG between the <!-- ml-head:begin/end --> markers of icon-ouroboros.html: the
           mockup's spiral(), hairPath() and mouth(o, 'gap', 6), baked for every state (no ids: two pedals share a page).

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


def f1(v):
    s = "%.1f" % v
    return "0.0" if s == "-0.0" else s


def spiral(R, turns=3):
    n, d = turns * 40, []
    for i in range(n + 1):
        a, r = i / n * turns * 2 * math.pi, R * i / n
        d.append(("L" if i else "M") + f1(r * math.cos(a)) + "," + f1(r * math.sin(a)))
    return "".join(d)


def hair_path():
    def P(a, r):
        return f1(r * math.cos(math.radians(a))) + "," + f1(r * math.sin(math.radians(a)))
    n, a0, a1 = 6, -124, -56
    st = (a1 - a0) / n
    pts = [P(a0, 88)]
    for k in range(n):
        pts += [P(a0 + st * (k + .5), 104 if k % 2 else 111), P(a0 + st * (k + 1), 88)]
    a = a1 - 4
    while a > a0:
        pts.append(P(a, 87.5))
        a -= 4
    return "M" + "L".join(pts) + "Z"


def mouth(o, n=6):
    """The mockup's mouth(o, 'gap', n): the cavity, and the teeth (the third top one missing). The mockup clips the
    teeth to the cavity; with no clip (no ids), each row stops at the outline's centre line (its 4 px stroke covers the
    rest), and the outer teeth stay inside the slanted sides."""
    w, y0 = 44, 20
    yb, A, B, bw, L, s = y0 + 12 + 30 * o, 5 * o, 8 * o, w * .86, 8, 2 * w / n
    yt = lambda x: y0 - A * (1 - min(1, (x / w) ** 2))
    ybx = lambda x: yb + B * (1 - min(1, (x / bw) ** 2))
    side = lambda y: w - (w - bw) * min(1, max(0, (y - y0) / (yb - y0)))   # the cavity's half-width at y
    cav = "M%d,%dQ0,%s %d,%dL%s,%sQ0,%s %s,%sZ" % (-w, y0, f1(y0 - 2 * A), w, y0, f1(bw), f1(yb), f1(yb + 2 * B),
                                                f1(-bw), f1(yb))
    top, bottom = [], []
    for k in range(n):
        x0 = -w + k * s + 1.2
        x1 = x0 + s - 2.4
        e, b = yt((x0 + x1) / 2), ybx((x0 + x1) / 2)
        if k != 2:
            h = side(e + L - 2) - .6
            tx0, tx1 = max(x0, -h), min(x1, h)
            txm = (tx0 + tx1) / 2
            top.append("M%s,%sH%sV%sQ%s,%s %s,%sZ" % (f1(tx0), f1(e), f1(tx1), f1(e + L - 2), f1(txm), f1(e + L + 1),
                                                     f1(tx0), f1(e + L - 2)))
        h = side(b) - .6
        bx0, bx1 = max(x0, -h), min(x1, h)
        bxm = (bx0 + bx1) / 2
        bottom.append("M%s,%sH%sV%sQ%s,%s %s,%sZ" % (f1(bx0), f1(b), f1(bx1), f1(b - L + 2), f1(bxm), f1(b - L - 1),
                                                    f1(bx0), f1(b - L + 2)))
    return cav, "".join(top), "".join(bottom)   # two paths: their teeth overlap, and wind opposite ways


DEAD_MOUTH = "M-30,34Q0,30 30,34"
EYES = [(-36, -24, 25, 1, "l"), (38, -28, 15, -2, "r")]      # x, y, R, turns per loop, side
MOUTHS = [("grit", 0.0), ("wait", 0.36), ("scream", 0.91)]


def head_svg():
    s = ['<svg class="ml-head" viewBox="-110 -110 220 220" aria-hidden="true"><g class="ml-hd">',
         '<path class="ml-hair" d="%s"/>' % hair_path(), '<circle class="ml-skull" r="88"/>',
         '<path class="ml-scar" d="M50,6L72,24M53,16l7,-7M59,21l7,-7M65,26l7,-7"/>']
    for x, y, R, spd, side in EYES:
        s.append('<g transform="translate(%d,%d)"><path class="ml-brow ml-brow-%s" d="M%d,%dL%d,%d"/></g>'
                 % (x, y, side, -R + 2, -R - 12, R - 2, -R - 12))
        s.append('<g transform="translate(%d,%d)"><circle class="ml-white" r="%d"/>'
                 '<g class="ml-free ml-free-%s"><circle r="%d" fill="none"/><g class="ml-spin" data-spd="%d">'
                 '<path class="ml-spiral" style="stroke-width:%spx" d="%s"/></g></g>'
                 '<path class="ml-xx" d="M-10,-10L10,10M10,-10L-10,10"/></g>'
                 % (x, y, R + 3, side, R, spd, ("%.2f" % (R / 3 * .44)), spiral(R)))
    for name, o in MOUTHS:
        cav, top, bottom = mouth(o)
        s.append('<g class="ml-mouth ml-m-%s"><path class="ml-cav" d="%s"/><path class="ml-teeth" d="%s"/>'
                 '<path class="ml-teeth" d="%s"/><path class="ml-cav-line" d="%s"/></g>' % (name, cav, top, bottom, cav))
    s.append('<g class="ml-mouth ml-m-dead"><path class="ml-cav-line" d="%s"/></g>' % DEAD_MOUTH)
    s.append('<text class="ml-chin" y="81">REC</text></g></svg>')
    return "\n".join(s)


def head(template=os.path.join(HERE, "icon-ouroboros.html")):
    src = open(template, encoding="utf-8").read()
    m = re.search(r"(<!-- ml-head:begin -->)(.*?)([ \t]*<!-- ml-head:end -->)", src, re.DOTALL)
    assert m, "the icon has no <!-- ml-head:begin/end --> markers"
    with open(template, "w", encoding="utf-8") as f:
        f.write(src[:m.end(1)] + "\n" + head_svg() + "\n" + src[m.start(3):])


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
    if sys.argv[1:] == ["--head"]:       # the head alone: no fonts or network needed
        head()
    elif sys.argv[1:] != ["--check"]:
        fonts()
        title()
        head()
    check()
