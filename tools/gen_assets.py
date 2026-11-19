#!/usr/bin/env python3
"""Builds Shmees Media Center's UI assets into one C header.

  - atlas: a 1024x1024 8-bit coverage atlas (Inter text in five styles, icons, UI shapes), stored as PNG bytes.
    On the console it is uploaded as an R8 texture swizzled to (1, 1, 1, red), so one shader draws text, icons,
    shapes and solid fills by multiplying a vertex colour by it.
  - badge: the Shmees badge (lime S on charcoal), RGBA PNG bytes.
  - glyph metrics for each text style, icon and shape rectangles.

The theme is the established Shmees one: lime (150, 230, 60) on charcoal (16, 20, 24), Inter type, rounded S badge
(same drawing as the Shmees Aurora skin). Inter is under the SIL Open Font License 1.1 (see NOTICES.md).

usage: gen_assets.py <output header> [<preview png>]
"""

import io
import math
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_DIR = "/usr/share/fonts/opentype/inter/"
LIME = (150, 230, 60)
CHARCOAL = (16, 20, 24)
ATLAS_W = 2048
ATLAS_H = 512
PAD = 2

# (enum name, font file, pixel size)
STYLES = [
    ("FONT_SMALL", "Inter-Medium.otf", 19),
    ("FONT_BODY", "Inter-Regular.otf", 23),
    ("FONT_LABEL", "Inter-SemiBold.otf", 24),
    ("FONT_TITLE", "Inter-Bold.otf", 34),
    ("FONT_HERO", "InterDisplay-Black.otf", 62),
]

# ASCII, Latin-1, and a few typographic marks the UI uses.
CODE_POINTS = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + [
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x00B7]

ICONS = ["home", "film", "tv", "clock", "heart", "heart_fill", "search", "gear", "play", "pause", "info", "check",
         "star", "drive", "usb", "back", "plus", "list", "rewind", "forward", "sparkle"]
ICON_PX = 64
SS = 4  # supersampling for icons and shapes


class Packer:
    """Shelf packer: rows left to right, top to bottom."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.x = PAD
        self.y = PAD
        self.row = 0

    def place(self, w, h):
        if self.x + w + PAD > self.width:
            self.x = PAD
            self.y += self.row + PAD
            self.row = 0
        if self.y + h + PAD > self.height:
            raise SystemExit("atlas full")
        at = (self.x, self.y)
        self.x += w + PAD
        self.row = max(self.row, h)
        return at


def supersampled(px, draw_fn):
    big = Image.new("L", (px * SS, px * SS), 0)
    draw_fn(ImageDraw.Draw(big), px * SS)
    return big.resize((px, px), Image.LANCZOS)


def icon(name):
    def fn(d, s):
        w = max(2, int(s * 0.085))  # stroke

        def P(x, y):
            return (x * s, y * s)

        if name == "home":
            d.line([P(.14, .5), P(.5, .17), P(.86, .5)], fill=255, width=w, joint="curve")
            d.line([P(.25, .42), P(.25, .84), P(.75, .84), P(.75, .42)], fill=255, width=w, joint="curve")
            d.rectangle([P(.43, .6), P(.57, .84)], fill=255)
        elif name == "film":
            d.rounded_rectangle([P(.16, .2), P(.84, .8)], radius=s * .07, outline=255, width=w)
            for i in range(4):
                y = .27 + i * .15
                d.rectangle([P(.2, y), P(.27, y + .07)], fill=255)
                d.rectangle([P(.73, y), P(.8, y + .07)], fill=255)
            d.line([P(.33, .2), P(.33, .8)], fill=255, width=max(1, w // 2))
            d.line([P(.67, .2), P(.67, .8)], fill=255, width=max(1, w // 2))
        elif name == "tv":
            d.rounded_rectangle([P(.12, .2), P(.88, .72)], radius=s * .07, outline=255, width=w)
            d.line([P(.36, .84), P(.64, .84)], fill=255, width=w)
            d.line([P(.5, .72), P(.5, .84)], fill=255, width=w)
        elif name == "clock":
            d.ellipse([P(.14, .14), P(.86, .86)], outline=255, width=w)
            d.line([P(.5, .5), P(.5, .28)], fill=255, width=w)
            d.line([P(.5, .5), P(.66, .6)], fill=255, width=w)
        elif name in ("heart", "heart_fill"):
            pts = []
            for i in range(120):
                t = i / 120 * 2 * math.pi
                x = 16 * math.sin(t) ** 3
                y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
                pts.append(P(.5 + x / 38, .48 - y / 38))
            if name == "heart_fill":
                d.polygon(pts, fill=255)
            else:
                d.line(pts + [pts[0]], fill=255, width=w, joint="curve")
        elif name == "search":
            d.ellipse([P(.16, .16), P(.64, .64)], outline=255, width=w)
            d.line([P(.58, .58), P(.84, .84)], fill=255, width=int(w * 1.3))
        elif name == "gear":
            cx = cy = .5
            for i in range(8):
                a = i * math.pi / 4
                x, y = cx + math.cos(a) * .3, cy + math.sin(a) * .3
                d.ellipse([P(x - .085, y - .085), P(x + .085, y + .085)], fill=255)
            d.ellipse([P(.22, .22), P(.78, .78)], fill=255)
            d.ellipse([P(.38, .38), P(.62, .62)], fill=0)
        elif name == "play":
            d.polygon([P(.3, .18), P(.3, .82), P(.82, .5)], fill=255)
        elif name == "pause":
            d.rounded_rectangle([P(.26, .2), P(.42, .8)], radius=s * .03, fill=255)
            d.rounded_rectangle([P(.58, .2), P(.74, .8)], radius=s * .03, fill=255)
        elif name == "info":
            d.ellipse([P(.14, .14), P(.86, .86)], outline=255, width=w)
            d.ellipse([P(.45, .27), P(.55, .37)], fill=255)
            d.rectangle([P(.455, .44), P(.545, .72)], fill=255)
        elif name == "check":
            d.line([P(.2, .52), P(.42, .72), P(.82, .3)], fill=255, width=int(w * 1.2), joint="curve")
        elif name == "star":
            pts = []
            for i in range(10):
                r = .38 if i % 2 == 0 else .16
                a = -math.pi / 2 + i * math.pi / 5
                pts.append(P(.5 + math.cos(a) * r, .53 + math.sin(a) * r))
            d.polygon(pts, fill=255)
        elif name == "drive":
            d.rounded_rectangle([P(.12, .3), P(.88, .7)], radius=s * .08, outline=255, width=w)
            d.ellipse([P(.66, .45), P(.76, .55)], fill=255)
            d.line([P(.24, .5), P(.5, .5)], fill=255, width=w)
        elif name == "usb":
            d.rounded_rectangle([P(.3, .36), P(.7, .86)], radius=s * .06, outline=255, width=w)
            d.rectangle([P(.38, .14), P(.62, .36)], outline=255, width=w)
            d.rectangle([P(.43, .2), P(.48, .27)], fill=255)
            d.rectangle([P(.52, .2), P(.57, .27)], fill=255)
        elif name == "back":
            d.line([P(.8, .5), P(.22, .5)], fill=255, width=w)
            d.line([P(.42, .28), P(.2, .5), P(.42, .72)], fill=255, width=w, joint="curve")
        elif name == "plus":
            d.line([P(.5, .2), P(.5, .8)], fill=255, width=w)
            d.line([P(.2, .5), P(.8, .5)], fill=255, width=w)
        elif name == "list":
            for i in range(3):
                y = .3 + i * .2
                d.ellipse([P(.16, y - .045), P(.25, y + .045)], fill=255)
                d.line([P(.34, y), P(.84, y)], fill=255, width=w)
        elif name == "rewind":
            d.polygon([P(.5, .24), P(.5, .76), P(.14, .5)], fill=255)
            d.polygon([P(.86, .24), P(.86, .76), P(.5, .5)], fill=255)
        elif name == "forward":
            d.polygon([P(.5, .24), P(.5, .76), P(.86, .5)], fill=255)
            d.polygon([P(.14, .24), P(.14, .76), P(.5, .5)], fill=255)
        elif name == "sparkle":
            for cx, cy, r in ((.42, .52, .3), (.74, .26, .13)):
                d.polygon([P(cx, cy - r), P(cx + r * .22, cy - r * .22), P(cx + r, cy), P(cx + r * .22, cy + r * .22),
                           P(cx, cy + r), P(cx - r * .22, cy + r * .22), P(cx - r, cy), P(cx - r * .22, cy - r * .22)],
                          fill=255)
        else:
            raise SystemExit("no drawing for icon " + name)

    return supersampled(ICON_PX, fn)


def rounded_tile(radius):
    """A filled rounded rectangle tile for 9-slicing: corners radius px, 4 px of flat centre."""
    size = radius * 2 + 4
    big = Image.new("L", (size * SS, size * SS), 0)
    ImageDraw.Draw(big).rounded_rectangle([0, 0, size * SS - 1, size * SS - 1], radius=radius * SS, fill=255)
    return big.resize((size, size), Image.LANCZOS)


def ring_tile(radius, width):
    size = radius * 2 + 4
    big = Image.new("L", (size * SS, size * SS), 0)
    ImageDraw.Draw(big).rounded_rectangle([0, 0, size * SS - 1, size * SS - 1], radius=radius * SS, outline=255,
                                          width=width * SS)
    return big.resize((size, size), Image.LANCZOS)


def shadow_tile(radius, blur):
    """A soft shadow for 9-slicing: a rounded rectangle blurred; edge = blur*2 + radius."""
    edge = radius + blur * 2
    size = edge * 2 + 4
    im = Image.new("L", (size, size), 0)
    ImageDraw.Draw(im).rounded_rectangle([blur * 2, blur * 2, size - 1 - blur * 2, size - 1 - blur * 2],
                                         radius=radius, fill=255)
    return im.filter(ImageFilter.GaussianBlur(blur)), edge


def circle(px):
    return supersampled(px, lambda d, s: d.ellipse([0, 0, s - 1, s - 1], fill=255))


def badge(size):
    """The Shmees badge, as on the Shmees Aurora skin: lime frame, charcoal fill, lime S, SHMEES under it."""
    s = size * 2
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rad, bw = int(s * 0.2), max(3, s // 22)
    d.rounded_rectangle([0, 0, s - 1, s - 1], rad, fill=LIME + (255,))
    d.rounded_rectangle([bw, bw, s - 1 - bw, s - 1 - bw], rad - bw, fill=CHARCOAL + (255,))
    d.text((s / 2, s * 0.43), "S", font=ImageFont.truetype(FONT_DIR + "Inter-Black.otf", int(s * 0.62)),
           fill=LIME + (255,), anchor="mm")
    d.text((s / 2, s * 0.83), "SHMEES", font=ImageFont.truetype(FONT_DIR + "Inter-Bold.otf", int(s * 0.13)),
           fill=(225, 230, 228, 255), anchor="mm")
    return im.resize((size, size), Image.LANCZOS)


def c_bytes(name, data):
    lines = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        lines.append("\t" + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    out = sys.argv[1]
    atlas = Image.new("L", (ATLAS_W, ATLAS_H), 0)
    pk = Packer(ATLAS_W, ATLAS_H)

    # A solid white block first, for fills: its centre texel is sampled.
    sx, sy = pk.place(8, 8)
    atlas.paste(255, (sx, sy, sx + 8, sy + 8))
    solid = (sx + 4, sy + 4)

    shapes = {}
    for name, img, extra in [("ROUND", rounded_tile(32), 32), ("RING", ring_tile(32, 4), 32), ("CIRCLE", circle(64), 32)]:
        x, y = pk.place(*img.size)
        atlas.paste(img, (x, y))
        shapes[name] = (x, y, img.size[0], img.size[1], extra)
    sh, edge = shadow_tile(24, 18)
    x, y = pk.place(*sh.size)
    atlas.paste(sh, (x, y))
    shapes["SHADOW"] = (x, y, sh.size[0], sh.size[1], edge)

    icons = {}
    for name in ICONS:
        img = icon(name)
        x, y = pk.place(ICON_PX, ICON_PX)
        atlas.paste(img, (x, y))
        icons[name] = (x, y)

    styles = []
    for enum, file, px in STYLES:
        font = ImageFont.truetype(FONT_DIR + file, px)
        cmap = TTFont(FONT_DIR + file).getBestCmap()
        ascent, descent = font.getmetrics()
        glyphs = []
        points = sorted(CODE_POINTS if px < 30 else [c for c in CODE_POINTS if c < 0x7F or c in (0x2013, 0x2014, 0x2019, 0x2026, 0x00B7, 0x00E9)])
        for cp in points:
            if cp not in cmap:
                continue
            ch = chr(cp)
            adv = font.getlength(ch)
            mask, offset = font.getmask2(ch, mode="L", anchor="ls")
            w, h = mask.size
            if w and h and cp != 0x20:
                gx, gy = pk.place(w, h)
                atlas.paste(Image.frombytes("L", (w, h), bytes(mask)), (gx, gy))
            else:
                gx = gy = w = h = 0
            glyphs.append((cp, gx, gy, w, h, offset[0], offset[1], adv))
        styles.append((enum, px, ascent, descent, glyphs))

    buf = io.BytesIO()
    atlas.save(buf, "PNG", optimize=True)
    atlas_png = buf.getvalue()
    b = badge(256)
    buf = io.BytesIO()
    b.save(buf, "PNG", optimize=True)
    badge_png = buf.getvalue()

    h = ["// Generated by tools/gen_assets.py; do not edit. Inter: SIL Open Font License 1.1 (NOTICES.md).",
         "#ifndef SMC_ASSETS_H", "#define SMC_ASSETS_H", "", "#include <stdint.h>", "",
         f"#define SMC_ATLAS_W {ATLAS_W}", f"#define SMC_ATLAS_H {ATLAS_H}", f"#define SMC_SOLID_X {solid[0]}", f"#define SMC_SOLID_Y {solid[1]}",
         f"#define SMC_ICON_PX {ICON_PX}", ""]
    for name, (x, y, w, hh, extra) in shapes.items():
        h.append(f"#define SMC_SHAPE_{name}_X {x}\n#define SMC_SHAPE_{name}_Y {y}\n#define SMC_SHAPE_{name}_W {w}\n"
                 f"#define SMC_SHAPE_{name}_H {hh}\n#define SMC_SHAPE_{name}_EDGE {extra}")
    h.append("")
    h.append("enum {")
    for i, name in enumerate(ICONS):
        h.append(f"\tICON_{name.upper()} = {i},")
    h.append(f"\tICON_COUNT = {len(ICONS)}\n}};")
    h.append("static const uint16_t smc_icon_xy[ICON_COUNT][2] = {")
    for name in ICONS:
        h.append(f"\t{{ {icons[name][0]}, {icons[name][1]} }}, // {name}")
    h.append("};\n")
    h.append("typedef struct { uint16_t code_point, x, y, w, h; int16_t xoff, yoff; float advance; } SmcGlyph;")
    h.append("typedef struct { const SmcGlyph *glyphs; uint16_t count; uint16_t px; int16_t ascent, descent; } "
             "SmcFontStyle;\n")
    h.append("enum {")
    for i, (enum, *_rest) in enumerate(styles):
        h.append(f"\t{enum} = {i},")
    h.append(f"\tFONT_COUNT = {len(styles)}\n}};")
    for enum, px, ascent, descent, glyphs in styles:
        h.append(f"static const SmcGlyph smc_glyphs_{enum.lower()}[{len(glyphs)}] = {{")
        for g in glyphs:
            h.append("\t{ %d, %d, %d, %d, %d, %d, %d, %.4ff }," % g)
        h.append("};")
    h.append("static const SmcFontStyle smc_fonts[FONT_COUNT] = {")
    for enum, px, ascent, descent, glyphs in styles:
        h.append(f"\t{{ smc_glyphs_{enum.lower()}, {len(glyphs)}, {px}, {ascent}, {descent} }},")
    h.append("};\n")
    h.append(c_bytes("smc_atlas_png", atlas_png))
    h.append(c_bytes("smc_badge_png", badge_png))
    h.append("\n#endif")
    open(out, "w").write("\n".join(h) + "\n")
    used = pk.y + pk.row
    print(f"atlas {ATLAS_W}x{ATLAS_H}, rows used to {used}; atlas.png {len(atlas_png)} bytes, badge.png {len(badge_png)} bytes")
    if len(sys.argv) > 2:
        prev = Image.new("RGB", (ATLAS_W, used + 4), CHARCOAL)
        prev.paste(Image.new("RGB", (ATLAS_W, used + 4), LIME), (0, 0), atlas.crop((0, 0, ATLAS_W, used + 4)))
        prev.save(sys.argv[2])


if __name__ == "__main__":
    main()
