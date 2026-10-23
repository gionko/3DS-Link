#!/usr/bin/env python
"""make_guide.py -- the pictures of the "How to get boot9.bin" guide (AzaharLink/ConfigApp/guide/).

Usage: python make_guide.py [--check]      (Windows python with Pillow, like AzaharLink/ConfigApp/tools/make_icon.py; run by hand, its OUTPUT is committed)

Input : AzaharLink/ConfigApp/guide/src/snap_*.png -- GodMode9 v2.1.1 screenshots the user made with R+L: 400x480 each, the top screen (400x240) above the
        bottom screen (320x240, at x 40..360, y 240..480). The menus are on the bottom screen.
Output: AzaharLink/ConfigApp/guide/gfx/stepN.png + stepN.t3s for the steps that have a picture (N = step number; step 8 is text only). The Makefiles of the
        app and of the installer (GRAPHICS := the gfx folder) turn each t3s into stepN.t3x with tex3ds and link it in as the symbol stepN_t3x.

Every picture is made the same way: a crop of what matters (a menu, at most 196x82 pixels of the screenshot), the arrows and a highlight drawn on it in the
pixel size of GodMode9's own font (so they look like part of the same picture), then the whole thing enlarged 2x with no smoothing (GodMode9's 6x8 letters are
too small on the top screen of a console at 1x). The picture is shown 1:1 by the program: no filter and no scaling on the console. A picture is at most
392x164 pixels (the top screen is 400x240 and keeps its title bar above and three lines of text under it).

What the user asked for, "two little arrows" that show where to select and press, is: an ARROW that points at the line to choose (with a yellow frame around the
line) and, at its tail, a CHIP with the key to press ("A", "HOME"). The last picture has two arrows: one at the folder the dump goes to, one at "<A> yes".

The script prints the size of each picture. `--check` also runs tex3ds on every t3s (it needs `tex3ds` on PATH, or it runs it in the devkitpro/devkitarm Docker
image when it is not) and prints the size of every t3x: that is what travels inside the installer (the pictures are in the installer child and in the app).
"""
import os
import subprocess
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
GUIDE = os.path.normpath(os.path.join(HERE, "..", "..", "AzaharLink", "ConfigApp", "guide"))
SRC = os.path.join(GUIDE, "src")
OUT = os.path.join(GUIDE, "gfx")

ZOOM = 2
YELLOW = (255, 196, 40)
BLACK = (0, 0, 0)

# ---- a 5x7 pixel font for the key chips (only the letters that are needed) ----
GLYPHS = {
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
}


def chip_size(label):
    return 5 * len(label) + (len(label) - 1) + 4, 7 + 4


def draw_chip(d, x, y, label):
    """A yellow tab with the key in black, its top left corner at (x, y). Returns (w, h)."""
    w, h = chip_size(label)
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=YELLOW)
    cx = x + 2
    for ch in label:
        for gy, row in enumerate(GLYPHS[ch]):
            for gx, c in enumerate(row):
                if c == "#":
                    d.point((cx + gx, y + 2 + gy), fill=BLACK)
        cx += 6
    return w, h


def frame(d, box, pad=2):
    """A one pixel yellow frame around the text box (x0, y0, x1, y1) (both ends included)."""
    x0, y0, x1, y1 = box
    d.rectangle([x0 - pad, y0 - pad, x1 + pad, y1 + pad], outline=YELLOW)


def arrow(d, tip, direction, length, label=None):
    """An arrow whose tip is at `tip`, pointing `direction` ('left': it points to the left, so it sits at the right of what it shows; 'right': the other
    way round; 'up': it sits below). The shaft is `length` pixels long, 3 thick; the head is 7 long and 11 across. At the tail a chip with `label`, when
    there is one."""
    tx, ty = tip
    if direction == "right":
        d.polygon([(tx, ty), (tx - 6, ty - 5), (tx - 6, ty + 5)], fill=YELLOW)
        d.rectangle([tx - 6 - length, ty - 1, tx - 7, ty + 1], fill=YELLOW)
        if label:
            w, h = chip_size(label)
            draw_chip(d, tx - 6 - length - w, ty - h // 2, label)
    elif direction == "left":
        d.polygon([(tx, ty), (tx + 6, ty - 5), (tx + 6, ty + 5)], fill=YELLOW)
        d.rectangle([tx + 6, ty - 1, tx + 6 + length - 1, ty + 1], fill=YELLOW)
        if label:
            w, h = chip_size(label)
            draw_chip(d, tx + 6 + length, ty - h // 2, label)
    elif direction == "up":
        d.polygon([(tx, ty), (tx - 5, ty + 6), (tx + 5, ty + 6)], fill=YELLOW)
        d.rectangle([tx - 1, ty + 6, tx + 1, ty + 6 + length - 1], fill=YELLOW)
        if label:
            w, h = chip_size(label)
            draw_chip(d, tx - w // 2, ty + 6 + length, label)
    else:
        raise ValueError(direction)


def screenshot(name, hide=()):
    """A screenshot of guide/src, with the parts in `hide` (rectangles) painted black (the label of the user's SD card)."""
    im = Image.open(os.path.join(SRC, name)).convert("RGB")
    for r in hide:
        ImageDraw.Draw(im).rectangle(r, fill=BLACK)
    return im


class Pic:
    """A picture under construction: a canvas (a crop of a screenshot, or pieces of screenshots pasted on a black one) the overlays are drawn on, and `save`
    enlarges and writes it. `origin` is where the canvas's corner is in the screenshot, so that the numbers of the overlays can be read off the screenshot."""

    def __init__(self, name, canvas, origin=(0, 0)):
        self.name = name
        self.canvas = canvas
        self.origin = origin
        self.d = ImageDraw.Draw(canvas)

    @classmethod
    def crop(cls, name, shot, box, hide=(), pad_bottom=0):
        """`pad_bottom` rows of black are added under the crop (a margin the screenshot has no room for)."""
        canvas = screenshot(shot, hide).crop(box)
        if pad_bottom:
            padded = Image.new("RGB", (canvas.width, canvas.height + pad_bottom), BLACK)
            padded.paste(canvas, (0, 0))
            canvas = padded
        return cls(name, canvas, (box[0], box[1]))

    # screenshot coordinates -> canvas coordinates
    def p(self, x, y):
        return x - self.origin[0], y - self.origin[1]

    def frame(self, box):
        x0, y0, x1, y1 = box
        a, b = self.p(x0, y0)
        c, e = self.p(x1, y1)
        frame(self.d, (a, b, c, e))

    def arrow(self, tip, direction, length, label=None):
        arrow(self.d, self.p(*tip), direction, length, label)

    def save(self):
        im = self.canvas.resize((self.canvas.width * ZOOM, self.canvas.height * ZOOM), Image.NEAREST)
        os.makedirs(OUT, exist_ok=True)
        png = os.path.join(OUT, self.name + ".png")
        im.save(png, optimize=True)
        with open(os.path.join(OUT, self.name + ".t3s"), "w", newline="\n") as f:
            f.write("-f rgb565 -z auto\n%s.png\n" % self.name)
        return im.size


def picture_1():
    # GodMode9's file browser (top screen): what appears after START + power on. The label of the user's SD card is painted out.
    p = Pic.crop("step1", "snap_261016235315.png", (0, 0, 196, 72), hide=[(72, 13, 152, 24)])
    return p


def picture_2():
    # the keys list (bottom screen): the last line says what HOME does
    p = Pic.crop("step2", "snap_261016235315.png", (98, 396, 294, 476))
    p.frame((99, 467, 247, 473))
    p.arrow((253, 470), "left", 5, "HOME")
    return p


def picture_3():
    # the HOME menu: the arrow is on Scripts...
    p = Pic.crop("step3", "snap_261016235335.png", (118, 296, 314, 376))
    p.frame((129, 364, 204, 372))
    p.arrow((210, 368), "left", 14, "A")
    return p


def picture_4():
    # the list of scripts: GM9Megascript is the line to choose
    p = Pic.crop("step4", "snap_261016235347.png", (120, 326, 316, 406))
    p.frame((129, 358, 223, 366))
    p.arrow((229, 362), "left", 14, "A")
    return p


def picture_5():
    # the Megascript's main menu: Dump Options. The crop starts 4 pixels above the first line ("Backup Options" is at row 299 of the screenshot, the rows over it are black): a
    # picture whose first line touches its top edge sits against the title bar of the screen. It ends with "Dump Options" and 4 rows of black (the line under it, "Inject
    # Options", is left out: a taller crop would pass the 82 rows the top screen has room for).
    p = Pic.crop("step5", "snap_261016235503.png", (108, 295, 304, 371), pad_bottom=4)
    p.frame((114, 359, 202, 367))
    p.arrow((208, 363), "left", 14, "A")
    return p


def picture_6():
    # the Dump Options menu: "Dump Boot9.bin & Boot11.bin" is long, so the arrow comes up from below it
    p = Pic.crop("step6", "snap_261016235508.png", (112, 339, 308, 399))
    p.frame((120, 353, 298, 361))
    p.arrow((284, 366), "up", 10, "A")
    return p


def picture_7():
    # the question: it is one line of 250 pixels on the console; the picture breaks it in two so that it can be shown at 2x
    shot = screenshot("snap_261016235516.png")
    canvas = Image.new("RGB", (196, 62), BLACK)
    canvas.paste(shot.crop((75, 344, 200, 356)), (10, 4))    # "Create boot9 & boot11"
    canvas.paste(shot.crop((207, 344, 326, 356)), (10, 20))  # "dumps in 0:/gm9/out?"
    canvas.paste(shot.crop((76, 364, 175, 376)), (62, 42))   # "(<A> yes, <B> no)"
    p = Pic("step7", canvas)
    # where the files go: "0:/gm9/out" (x 261..318 of the screenshot, in the second piece), framed, with an arrow at it from the right. The "?" of the question comes right
    # after it (one more character of 6 pixels, x 124..128 of this picture): the tip of the arrow is one character further, so that it does not cover it.
    gx0, gx1 = 10 + (261 - 207), 10 + (318 - 207)
    frame(p.d, (gx0, 22, gx1, 29))
    arrow(p.d, (gx1 + 6 + 6, 26), "left", 12)
    # the key: "(<A> yes," (x 76..125 of the screenshot, 49 pixels), framed, with an arrow at it from the left and the chip of the key
    frame(p.d, (62, 44, 62 + 49, 51))
    arrow(p.d, (62 - 5, 48), "right", 12, "A")
    return p


PICTURES = [picture_1, picture_2, picture_3, picture_4, picture_5, picture_6, picture_7]


def run_tex3ds(args, cwd):
    """tex3ds on PATH, else in the devkitpro/devkitarm image (the same one the Makefiles are run in)."""
    try:
        return subprocess.run(["tex3ds"] + args, cwd=cwd, check=True, capture_output=True, text=True)
    except FileNotFoundError:
        top = os.path.normpath(os.path.join(GUIDE, "..", "..", ".."))
        rel = os.path.relpath(OUT, top).replace("\\", "/")
        env = dict(os.environ, MSYS_NO_PATHCONV="1")
        cmd = ["docker", "run", "--rm", "-v", top + ":/src", "-w", "/src/" + rel, "devkitpro/devkitarm", "tex3ds"] + args
        return subprocess.run(cmd, check=True, capture_output=True, text=True, env=env)


def check():
    """Runs tex3ds on every t3s (into a scratch folder next to the output, removed after) and prints the size of every t3x."""
    tmp = os.path.join(OUT, "_t3x_check")
    os.makedirs(tmp, exist_ok=True)
    total = 0
    try:
        for i in range(1, len(PICTURES) + 1):
            name = "step%d" % i
            run_tex3ds(["-i", name + ".t3s", "-o", "_t3x_check/" + name + ".t3x"], OUT)
            size = os.path.getsize(os.path.join(tmp, name + ".t3x"))
            total += size
            print("%s.t3x %6d bytes" % (name, size))
    finally:
        for f in os.listdir(tmp):
            os.remove(os.path.join(tmp, f))
        os.rmdir(tmp)
    print("total t3x %d bytes" % total)


def main():
    for i, make in enumerate(PICTURES, 1):
        pic = make()
        size = pic.save()
        png = os.path.join(OUT, pic.name + ".png")
        print("%s.png %dx%d (%d bytes)" % (pic.name, size[0], size[1], os.path.getsize(png)))
    if "--check" in sys.argv:
        check()


if __name__ == "__main__":
    main()
