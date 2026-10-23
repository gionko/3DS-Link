#!/usr/bin/env python
"""Draws the 48x48 app icon of "3DS Link" (deterministic, Pillow only).

Usage: python make_icon.py [output.png]     (default: icon.png beside this script)

Method: everything is drawn at S x the final size, then reduced with a BOX filter
(exact area coverage, no ringing).

The result is a FULLY OPAQUE, FULL-BLEED SQUARE (PNG colour type 2, no alpha channel at
all).  An SMDH icon is RGB565 with no alpha, and smdhtool flattens the PNG onto BLACK
(c * a / 255, rounded down), so every transparent pixel of an RGBA icon becomes black on
the HOME menu and in the Download Play list.  The rim colour therefore runs out to the
square edge and the dark window inside it has rounded corners: the corners of the square
are rim-coloured (never black), and wherever the host rounds the icon itself (HOME's
frame) it only eats rim-coloured pixels, leaving a rim of even width.  The chain and the
text stay 6 px inside every edge.
"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

N = 48       # final size in pixels
S = 16       # supersampling factor

FILL = (20, 22, 27)        # tile body
RIM = (173, 179, 191)      # tile rim
AZURE = (71, 180, 255)     # left link
WHITE = (235, 237, 241)    # right link
TEXT = (255, 255, 255)

CORNER = 8                 # nominal corner radius (px); the dark window's own corners are CORNER - RIM_W = 6
RIM_W = 2                  # rim width (px), along the square edge

# chain links (px of the 48x48 image): two identical stadium rings
LINK_H, LINK_W, LINK_T = 20, 24, 4   # outer height, outer width, stroke
LINK_Y = 6                           # top of the links
LINK_SPAN = 36                       # left edge of link 1 .. right edge of link 2
GAP = 1.0                            # dark gap where a link passes over the other

# text "3DS": cap height 12 px, from y=30 to y=42
FONTS = ["ariblk.ttf", "arialbd.ttf", "segoeuib.ttf", "verdanab.ttf"]
FONT_DIR = r"C:\Windows\Fonts"
CAP_TOP, CAP_H = 30, 12


def blank(v=0):
    return Image.new("L", (N * S, N * S), v)


def stadium(box, fill, draw):
    """Rounded rectangle with full-round ends; box in output pixels."""
    x0, y0, x1, y1 = (round(v * S) for v in box)
    if x1 - x0 > 0 and y1 - y0 > 0:
        draw.rounded_rectangle([x0, y0, x1 - 1, y1 - 1], radius=(y1 - y0) / 2, fill=fill)


def ring(box, t, grow=0.0):
    """Mask of a stadium ring of stroke t, grown by `grow` px on both sides."""
    x0, y0, x1, y1 = box
    m = blank()
    d = ImageDraw.Draw(m)
    stadium((x0 - grow, y0 - grow, x1 + grow, y1 + grow), 255, d)
    stadium((x0 + t + grow, y0 + t + grow, x1 - t - grow, y1 - t - grow), 0, d)
    return m


def find_font(size):
    for name in FONTS:
        path = os.path.join(FONT_DIR, name)
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    raise SystemExit("no bold sans font found (tried %s)" % ", ".join(FONTS))


def text_mask():
    """'3DS' mask at final resolution, cap height exactly CAP_H px, centred."""
    # find the font size whose hinted cap height ("D") is CAP_H pixels
    for size in range(CAP_H, 4 * CAP_H):
        f = find_font(size)
        b = f.getbbox("D")
        if b[3] - b[1] >= CAP_H:
            break
    x0, y0, x1, y1 = f.getbbox("3DS")
    m = Image.new("L", (N, N), 0)
    dx = (N - (x1 - x0)) // 2 - x0
    dy = CAP_TOP - f.getbbox("D")[1]
    ImageDraw.Draw(m).text((dx, dy), "3DS", font=f, fill=255)
    return m


def build():
    # ---- tile: full-bleed square, rim colour to the edge, rounded dark window (no alpha) ----
    img = Image.new("RGB", (N * S, N * S), RIM)          # the whole square is rim colour ...
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([RIM_W * S, RIM_W * S, (N - RIM_W) * S - 1, (N - RIM_W) * S - 1],
                        radius=(CORNER - RIM_W) * S, fill=FILL)   # ... except the dark window

    # ---- chain links ----
    ax = (N - LINK_SPAN) / 2
    bx = (N + LINK_SPAN) / 2 - LINK_W
    A = (ax, LINK_Y, ax + LINK_W, LINK_Y + LINK_H)       # left link
    B = (bx, LINK_Y, bx + LINK_W, LINK_Y + LINK_H)       # right link
    top = blank()                                        # upper half: left link is over
    ImageDraw.Draw(top).rectangle([0, 0, N * S, round((LINK_Y + LINK_H / 2) * S) - 1], fill=255)

    def put(box, colour, clip=None, halo=True):
        for mask, col in ((ring(box, LINK_T, GAP) if halo else None, FILL),
                          (ring(box, LINK_T), colour)):
            if mask is not None:
                if clip is not None:
                    mask = ImageChops.multiply(mask, clip)
                img.paste(col, (0, 0), mask)

    put(A, AZURE, halo=False)    # left link first
    put(B, WHITE)                # right link over it (lower crossing), with dark gap
    put(A, AZURE, clip=top)      # left link back over the right one in the upper half

    # ---- reduce, add text ----
    small = img.resize((N, N), Image.BOX)
    small.paste(TEXT, (0, 0), text_mask())
    return small                                         # mode "RGB": saved without an alpha channel


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "icon.png")
    build().save(path)
    print("wrote", path)
