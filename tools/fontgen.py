#!/usr/bin/env python3
"""Makes ports/node/fonts.c, the typefaces and icons a colour screen draws with (ports/node/fonts.h).

Each glyph is rendered by FreeType, through Pillow, and kept as 4-bit coverage. Run it again only
to change a size, a weight or the glyphs kept:

    python3 tools/fontgen.py --inter <Inter's extras/ttf directory> \
        --symbols <MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf> > ports/node/fonts.c

Inter: https://github.com/rsms/inter (SIL Open Font License 1.1, ports/node/fonts/OFL.txt).
Material Symbols: https://github.com/google/material-design-icons (Apache License 2.0).
Needs Pillow (`pip install pillow`).
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# name, file, pixel size, whether Latin-1 follows ASCII, extra code points
FACES = [
    ("small", "Inter-SemiBold.ttf", 13, True),
    ("body", "Inter-Medium.ttf", 17, True),
    ("bold", "Inter-Bold.ttf", 21, False),
    ("hero", "Inter-Bold.ttf", 31, False),
]
EXTRAS = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x203A]

# In the order of enum icon (fonts.h), after ICON_NONE: the Material Symbols name and code point.
ICONS = [
    ("home", 0xE9B2),
    ("chat", 0xE0C9),
    ("group", 0xEA21),
    ("qr_code_2", 0xE00A),
    ("apps", 0xE5C3),
    ("air", 0xEFD8),
    ("groups", 0xF233),
    ("info", 0xE88E),
    ("smartphone", 0xE7BA),
    ("delete_forever", 0xE92B),
    ("arrow_back", 0xE5C4),
    ("bluetooth", 0xE1A7),
    ("send", 0xE163),
    ("check", 0xE668),
    ("done_all", 0xE877),
    ("schedule", 0xEFD6),
    ("error", 0xF8B6),
    ("cell_tower", 0xEBBA),
    ("power_settings_new", 0xF8C7),
    ("bolt", 0xEA0B),
    ("key", 0xE73C),
    ("download", 0xF090),
    ("warning", 0xF083),
]


def coverage(font, ch):
    """The glyph's box from the pen and the line's top, its 4-bit pixels and its advance."""
    ascent, _ = font.getmetrics()
    left, top, right, bottom = font.getbbox(ch, anchor="ls")
    adv = round(font.getlength(ch))
    w, h = max(0, right - left), max(0, bottom - top)
    if w == 0 or h == 0:
        return 0, 0, 0, 0, [], adv
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-left, -top), ch, font=font, fill=255, anchor="ls")
    px = [(v * 15 + 127) // 255 for v in img.tobytes()]
    return w, h, left, ascent + top, px, adv


def pack(nibbles):
    if len(nibbles) % 2:
        nibbles = nibbles + [0]
    return bytes(nibbles[i] << 4 | nibbles[i + 1] for i in range(0, len(nibbles), 2))


def emit_bytes(out, name, data):
    out.append(f"static const uint8_t {name}[] = {{")
    for i in range(0, len(data), 24):
        out.append("    " + ", ".join(f"0x{b:02X}" for b in data[i : i + 24]) + ",")
    out.append("};")


def emit_glyphs(out, name, glyphs, static=True):
    out.append(f"{'static ' if static else ''}const struct font_glyph {name}[] = {{")
    for g in glyphs:
        out.append("    {%d, %d, %d, %d, %d, %d}," % g)
    out.append("};")


def face(out, name, path, size, latin1):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    points = list(range(0x20, 0x7F))
    if latin1:
        points += list(range(0xA0, 0x100)) + EXTRAS
    nibbles, glyphs = [], []
    for c in points:
        w, h, x, y, px, adv = coverage(font, chr(c))
        glyphs.append((len(nibbles), w, h, x, y, adv))
        nibbles += px
    emit_bytes(out, f"{name}_bits", pack(nibbles))
    emit_glyphs(out, f"{name}_glyphs", glyphs)
    if latin1:
        out.append(f"static const uint16_t {name}_extra[] = {{" +
                   ", ".join(f"0x{c:04X}" for c in EXTRAS) + "};")
    extra = f"{name}_extra" if latin1 else "NULL"
    out.append(f"const struct font font_{name} = {{{name}_glyphs, {name}_bits, {extra}, "
               f"{len(EXTRAS) if latin1 else 0}, {1 if latin1 else 0}, {ascent + descent}, "
               f"{ascent}}};")
    out.append("")


def icons(out, name, path, size):
    font = ImageFont.truetype(path, size)
    # FILL, GRAD, opsz, wght: outlined, at the optical size drawn, a weight that holds up small.
    font.set_variation_by_axes([0, 0, min(max(size, 20), 48), 500])
    nibbles, glyphs = [], [(0, 0, 0, 0, 0, 0)]
    for _, c in ICONS:
        img = Image.new("L", (size, size), 0)
        ImageDraw.Draw(img).text((size // 2, size // 2), chr(c), font=font, fill=255, anchor="mm")
        box = img.getbbox() or (0, 0, 0, 0)
        crop = img.crop(box)
        px = [(v * 15 + 127) // 255 for v in crop.tobytes()]
        glyphs.append((len(nibbles), crop.width, crop.height, box[0], box[1], size))
        nibbles += px
    out.append(f"const uint8_t {name}_bits[] = {{")
    data = pack(nibbles)
    for i in range(0, len(data), 24):
        out.append("    " + ", ".join(f"0x{b:02X}" for b in data[i : i + 24]) + ",")
    out.append("};")
    emit_glyphs(out, name, glyphs, static=False)
    out.append("")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--inter", required=True, help="the directory of Inter's static .ttf files")
    ap.add_argument("--symbols", required=True, help="Material Symbols Rounded's variable .ttf")
    a = ap.parse_args()
    out = [
        "/* Made by tools/fontgen.py: change that, not this. Inter, Copyright 2016 The Inter Project",
        " * Authors, under the SIL Open Font License 1.1 (fonts/OFL.txt). Material Symbols, Copyright",
        " * Google LLC, under the Apache License 2.0. */",
        "",
        '#include "fonts.h"',
        "",
        "#include <stddef.h>",
        "",
    ]
    for name, file, size, latin1 in FACES:
        face(out, name, os.path.join(a.inter, file), size, latin1)
    icons(out, "icon_small", a.symbols, 16)
    icons(out, "icon_large", a.symbols, 24)
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
