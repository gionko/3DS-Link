#!/usr/bin/env python
"""Draws the HOME-menu banner art of "3DS Link": banner.png (256x128) and jingle.wav (the 2.4 s tune of the banner).

Usage: python make_banner.py [output folder]     (default: the folder of this script; Pillow only, Windows fonts)

What the HOME menu shows: when a title is selected, the top screen plays the banner: a small 3D box whose front face carries this 256x128 picture, and
the tune. bannertool (DlpLink/tools/bannertool, MIT, Steveice10) turns banner.png and jingle.wav into banner.bnr, which makerom puts in the CIA
(DlpLink/tools/make_3dslink_cia.sh). The picture becomes RGBA4444 (16 levels per channel), so every colour here is a multiple of 17 and the shapes are
drawn at 4x and reduced with a BOX filter, then snapped to the 16 levels: no banding, clean edges.

The look follows the app icon (ConfigApp/tools/make_icon.py): two linked rings, azure and white, on the dark tile; the name big, the claim small.
The two rings interlock like chain links: at BOTH crossings the ring that passes over has the same thin dark halo on the ring under it (the upper crossing
the white over the azure, the lower one the azure over the white), and the logo (rings + name) is centred above the line.
"""
import math
import os
import struct
import sys
import wave

from PIL import Image, ImageChops, ImageDraw, ImageFont

W, H = 256, 128
S = 4                                  # supersampling
LINE_Y = 104                           # the line above the claim band

BG = (17, 17, 34)                      # deep blue-black
BG2 = (34, 34, 51)                     # the lighter band at the bottom
AZURE = (68, 170, 255)
WHITE = (238, 238, 238)
GREY = (187, 187, 204)
RIM = (170, 170, 187)

NAME_TOP = "3DS"
NAME_BOTTOM = "Link"
CLAIM = "Connect, share, play together"     # the claim of 3DS Link (chosen by the user, 2026-10-22): the room with the world, Download Play with nothing but a Luma card

FONTS_BLACK = ["ariblk.ttf", "arialbd.ttf", "segoeuib.ttf"]
FONTS_TEXT = ["segoeuisb.ttf", "segoeui.ttf", "arial.ttf"]
FONT_DIR = r"C:\Windows\Fonts"

RING_W, RING_H, STROKE = 56, 46, 9     # one ring, outer size and stroke (px)
OVERLAP = 18                           # how far the two rings overlap
HALO = 1.6                             # width of the dark halo where one ring passes over the other (px)
GAP_LOGO = 12                          # between the rings and the name


def font(names, size):
    for n in names:
        p = os.path.join(FONT_DIR, n)
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    raise SystemExit("no font found: " + ", ".join(names))


def snap(img):
    """Every channel to a multiple of 17 (the 16 levels of RGBA4444)."""
    lut = [min(255, int(round(v / 17.0)) * 17) for v in range(256)]
    return img.point(lut * len(img.getbands()))


def ring_mask(box, t, grow=0.0):
    """Stadium ring (rounded ends), stroke t, at S x; box in banner pixels; grow widens it on both sides."""
    m = Image.new("L", (W * S, H * S), 0)
    d = ImageDraw.Draw(m)
    x0, y0, x1, y1 = [v * S for v in box]
    g = grow * S
    d.rounded_rectangle([x0 - g, y0 - g, x1 + g - 1, y1 + g - 1], radius=(y1 - y0 + 2 * g) / 2, fill=255)
    inner = t * S
    d.rounded_rectangle([x0 + inner + g, y0 + inner + g, x1 - inner - g - 1, y1 - inner - g - 1],
                        radius=max(1, (y1 - y0 - 2 * inner - 2 * g) / 2), fill=0)
    return m


def half(top):
    """Mask of the upper or the lower half of the picture (the rings cross only above and below the middle)."""
    m = Image.new("L", (W * S, H * S), 0)
    d = ImageDraw.Draw(m)
    mid = None
    return m, d


def draw_banner():
    img = Image.new("RGB", (W * S, H * S), BG)
    d = ImageDraw.Draw(img)
    d.rectangle([0, LINE_Y * S, W * S, H * S], fill=BG2)
    d.rectangle([0, LINE_Y * S, W * S, LINE_Y * S + S - 1], fill=RIM)

    f_big = font(FONTS_BLACK, 38 * S)
    f_claim = font(FONTS_TEXT, 13 * S)
    probe = ImageDraw.Draw(Image.new("L", (4, 4)))
    tw = max(probe.textlength(NAME_TOP, font=f_big), probe.textlength(NAME_BOTTOM, font=f_big)) / S

    # the logo (rings + name) centred in the area above the line
    emblem_w = RING_W * 2 - OVERLAP
    group_w = emblem_w + GAP_LOGO + tw
    x_left = round((W - group_w) / 2)
    area_h = LINE_Y
    top = round((area_h - RING_H) / 2)
    x_right = x_left + RING_W - OVERLAP
    box_l = (x_left, top, x_left + RING_W, top + RING_H)
    box_r = (x_right, top, x_right + RING_W, top + RING_H)
    ml, mr = ring_mask(box_l, STROKE), ring_mask(box_r, STROKE)
    azure = Image.new("RGB", img.size, AZURE)
    white = Image.new("RGB", img.size, WHITE)
    dark = Image.new("RGB", img.size, BG)

    mid = (top + RING_H / 2) * S
    upper = Image.new("L", img.size, 0)
    ImageDraw.Draw(upper).rectangle([0, 0, W * S, mid], fill=255)
    lower = ImageChops.invert(upper)

    # base: both rings flat
    img.paste(azure, (0, 0), ml)
    img.paste(white, (0, 0), mr)
    # upper crossing: the white ring over the azure one, with the dark halo on the azure
    halo_up = ImageChops.multiply(ImageChops.multiply(ring_mask(box_r, STROKE, grow=HALO), ml), upper)
    img.paste(dark, (0, 0), halo_up)
    img.paste(white, (0, 0), ImageChops.multiply(mr, upper))
    # lower crossing: the azure ring over the white one, the same halo on the white
    halo_lo = ImageChops.multiply(ImageChops.multiply(ring_mask(box_l, STROKE, grow=HALO), mr), lower)
    img.paste(dark, (0, 0), halo_lo)
    img.paste(azure, (0, 0), ImageChops.multiply(ml, lower))

    # the name, big, two lines to the right of the rings
    x_text = (x_left + emblem_w + GAP_LOGO) * S
    asc, desc = f_big.getmetrics()
    line_h = (asc + desc) / S
    block_h = line_h * 2 - 8
    y0 = (area_h - block_h) / 2 - 3
    d.text((x_text, y0 * S), NAME_TOP, font=f_big, fill=AZURE)
    d.text((x_text, (y0 + line_h - 6) * S), NAME_BOTTOM, font=f_big, fill=WHITE)

    cw = d.textlength(CLAIM, font=f_claim)
    cy = LINE_Y + (H - LINE_Y - 13) / 2 - 3
    d.text(((W * S - cw) / 2, cy * S), CLAIM, font=f_claim, fill=GREY)
    out = img.resize((W, H), Image.BOX)
    return snap(out)


def draw_jingle(path):
    """A short rising arpeggio of the chain (E5 G5 B5 E6) that rings out: mono, 16 bit, 32 kHz, 2.4 s."""
    rate = 32000
    notes = [659.25, 783.99, 987.77, 1318.51]
    total = 2.4
    n = int(rate * total)
    frames = []
    for i in range(n):
        t = i / rate
        v = 0.0
        for k, f in enumerate(notes):
            start = 0.16 * k
            if t < start:
                continue
            dt = t - start
            env = min(1.0, dt / 0.012) * math.exp(-dt * (2.4 if k < 3 else 1.5))
            v += env * (math.sin(2 * math.pi * f * dt) + 0.25 * math.sin(2 * math.pi * 2 * f * dt)) * (0.30 if k < 3 else 0.36)
        fade = min(1.0, (total - t) / 0.4)
        s = max(-1.0, min(1.0, v * fade * 0.7))
        frames.append(struct.pack("<h", int(s * 32767)))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(frames))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    os.makedirs(out, exist_ok=True)
    banner = draw_banner()
    banner.save(os.path.join(out, "banner.png"), optimize=True)
    banner.resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(out, "banner_preview_x3.png"))
    draw_jingle(os.path.join(out, "jingle.wav"))
    print("wrote banner.png (%dx%d), banner_preview_x3.png and jingle.wav in %s" % (W, H, out))


if __name__ == "__main__":
    main()
