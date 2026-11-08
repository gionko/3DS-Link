#!/usr/bin/env python3
"""Makes Includes/azl/font_spleen.inc, the fonts of the in-game chat, from the BDF files of Spleen (Frederic Cambus, BSD 2-Clause,
https://github.com/fcambus/spleen) and its LICENSE, all three taken from one release tag:

    python -I Tests/make_font_spleen.py <spleen-6x12.bdf> <spleen-8x16.bdf> <LICENSE> Includes/azl/font_spleen.inc

Only printable ASCII (0x20-0x7E, 95 glyphs) is kept: the chat cleans every message to it (azl/chat_core.hpp), and every other glyph would be
bytes of the plugin that nothing draws. A glyph is stored by COLUMNS, one u16 per column, bit 0 = the BOTTOM row of the cell: the framebuffers
of the 3DS are rotated (a column of the screen is contiguous in memory, from the bottom row up), so a column of a glyph is one run of pixels
and its bits come in the order of the addresses (azl/gfx_surface.hpp). 6x12: 12 bytes a glyph, 1140 in all; 8x16: 16 bytes a glyph, 1520.

A third table, 6x10, is the 6x12 one in a cell two rows shorter (the chat's lines on the top screen, azl/chat_draw.hpp: smaller and tighter,
the user's 2026-11-08 request; no Spleen of that size is at hand). The top row and the bottom row of the 6x12 cell are empty in 82 of the 95
glyphs, which keep rows 1-10 as they are. The other 13 are made whole again, not cut: the 10 that reach the top row ($ ( ) / [ backslash
] { | }) have the two bottom rows empty and keep rows 2-11, drawn one row lower; p, q and y reach the bottom row (their descender's last
row), which is OR-ed into the row above it (p and q one row shorter, the hook of y one row higher, joined to its stem). The script checks
those numbers: a new release of the font that breaks them stops it.

The output carries the license (the BSD 2-Clause asks a copy of the source to keep it; the binary's copy is in CREDITS.md and in the app's
credits), the font's version and the SHA-256 of the three inputs, so that a file edited by hand, or made from other files, can be told.
"""
import hashlib
import sys

FIRST = 0x20
LAST = 0x7E


def parse_bdf(path):
    """the cell (w, h, x0, y0), the version and {code: [(row bits, ...)]} of a BDF file; every glyph placed in the cell"""
    text = open(path, 'rb').read().decode('ascii')
    lines = text.splitlines()
    cell = None
    version = None
    glyphs = {}
    i = 0
    while i < len(lines):
        parts = lines[i].split()
        if not parts:
            i += 1
            continue
        if parts[0] == 'FONTBOUNDINGBOX':
            cell = tuple(int(v) for v in parts[1:5])
        elif parts[0] == 'FONT_VERSION':
            version = lines[i].split('"')[1]
        elif parts[0] == 'STARTCHAR':
            code = None
            bbx = None
            i += 1
            while not lines[i].startswith('BITMAP'):
                p = lines[i].split()
                if p[0] == 'ENCODING':
                    code = int(p[1])
                elif p[0] == 'BBX':
                    bbx = tuple(int(v) for v in p[1:5])
                i += 1
            i += 1
            rows = []
            while lines[i].strip() != 'ENDCHAR':
                rows.append(int(lines[i].strip(), 16))
                i += 1
            if code is not None and FIRST <= code <= LAST:
                glyphs[code] = (bbx, rows)
        i += 1
    if cell is None or version is None:
        sys.exit('%s: no FONTBOUNDINGBOX or FONT_VERSION' % path)
    return cell, version, glyphs


def columns(cell, bbx, rows):
    """the glyph as cell-width u16 columns, bit 0 = the bottom row of the cell"""
    cw, ch, cx0, cy0 = cell
    w, h, xoff, yoff = bbx
    row_bytes = (w + 7) // 8
    if len(rows) != h:
        sys.exit('a glyph has %d rows, its BBX says %d' % (len(rows), h))
    pixels = [[0] * cw for _ in range(ch)]   # [row from the top][column]
    cell_top = cy0 + ch - 1                    # the font's y of the top row of the cell
    for r, bits in enumerate(rows):
        y = cell_top - (yoff + h - 1) + r       # the glyph's row r, counted from the top of the cell
        for c in range(w):
            if bits >> (row_bytes * 8 - 1 - c) & 1:
                x = xoff - cx0 + c
                if not (0 <= x < cw and 0 <= y < ch):
                    sys.exit('a glyph sets a pixel outside its cell')
                pixels[y][x] = 1
    out = []
    for x in range(cw):
        v = 0
        for y in range(ch):
            if pixels[y][x]:
                v |= 1 << (ch - 1 - y)
        out.append(v)
    return out


def label(code):
    if code == 0x5C:
        return 'backslash'   # (a comment that ends with a backslash would swallow the next line)
    if code == 0x20:
        return 'space'
    return "'%s'" % chr(code)


def crop10(cols):
    """a 6x12 glyph in a 10-row cell (see the docstring): rows 2-11 for a glyph that reaches the top row, rows 1-10 with row 0 OR-ed into
    row 1 for every other one"""
    if any(c >> 11 & 1 for c in cols):
        if any(c & 3 for c in cols):
            sys.exit('a glyph reaches both the top row and the two bottom rows of the 6x12 cell: it does not fit 10 rows')
        return [c >> 2 for c in cols]
    return [((c | (c & 1) << 1) >> 1) & 0x3FF for c in cols]


def table(name, path, crop=None, size=None):
    cell, version, glyphs = parse_bdf(path)
    cw, ch = cell[0], cell[1]
    missing = [c for c in range(FIRST, LAST + 1) if c not in glyphs]
    if missing:
        sys.exit('%s has no glyph for %s' % (path, ', '.join('0x%02X' % c for c in missing)))
    if ch > 16:
        sys.exit('%s: cells taller than 16 rows do not fit a u16 column' % path)
    oh = size if size else ch
    what = 'Spleen %dx%d %s' % (cw, ch, version) if not crop else 'Spleen %dx%d %s in a %d-row cell (Tests/make_font_spleen.py, crop10)' % (
        cw, ch, version, oh)
    out = ['/// %s: %d glyphs (0x%02X-0x%02X) of %d columns, bit 0 = the bottom row of the %d-row cell.' %
           (what, LAST - FIRST + 1, FIRST, LAST, cw, oh),
           # inline: one copy in the plugin for all the units that use it (a namespace constexpr has a copy in each)
           'inline constexpr u16 %s[%d][%d] = {' % (name, LAST - FIRST + 1, cw)]
    moved = {'top': [], 'bottom': []}
    for code in range(FIRST, LAST + 1):
        cols = columns(cell, *glyphs[code])
        if crop:
            if any(c >> 11 & 1 for c in cols):
                moved['top'].append(code)
            elif any(c & 1 for c in cols):
                moved['bottom'].append(code)
            cols = crop(cols)
        out.append('    {%s}, // 0x%02X %s' % (', '.join('0x%04X' % v for v in cols), code, label(code)))
    out.append('};')
    if crop and (''.join(map(chr, moved['top'])) != '$()/[' + chr(92) + ']{|}' or ''.join(map(chr, moved['bottom'])) != 'pqy'):
        sys.exit('the glyphs that reach the top or the bottom row of the 6x12 cell are not the 13 the 6x10 table was checked for')
    return (cw, ch, version), out


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    bdf6, bdf8, license_path, out_path = sys.argv[1:5]
    (w6, h6, v6), t6 = table('kSpleen6x12', bdf6)
    (w8, h8, v8), t8 = table('kSpleen8x16', bdf8)
    _, t10 = table('kSpleen6x10', bdf6, crop10, 10)
    if (w6, h6, w8, h8) != (6, 12, 8, 16):
        sys.exit('the cells are %dx%d and %dx%d, not 6x12 and 8x16' % (w6, h6, w8, h8))
    if v6 != v8:
        sys.exit('the two files are of two releases (%s, %s)' % (v6, v8))
    license_text = open(license_path, 'rb').read().decode('ascii').replace('\r\n', '\n').rstrip('\n')
    if 'Frederic Cambus' not in license_text or 'Redistribution and use in source and binary forms' not in license_text:
        sys.exit('%s is not the BSD 2-Clause license of Spleen' % license_path)
    sums = ['//   %s  %s' % (hashlib.sha256(open(p, 'rb').read()).hexdigest(), p.replace('\\', '/').split('/')[-1])
            for p in (bdf6, bdf8, license_path)]
    lines = ['// Generated by Tests/make_font_spleen.py from the BDF files of Spleen %s - do not edit by hand.' % v6,
             '// Spleen: https://github.com/fcambus/spleen (release %s). SHA-256 of the inputs:' % v6] + sums + [
             '// Only printable ASCII (0x20-0x7E). Included by azl/gfx_surface.hpp inside namespace azl::gfx. kSpleen6x10 is kSpleen6x12 in a',
             '// cell two rows shorter, for the lines of the chat: see Tests/make_font_spleen.py.',
             '//',
             '// The license of Spleen, as its LICENSE file says it:',
             '//']
    lines += [('// ' + l).rstrip() for l in license_text.split('\n')]
    lines += ['', 'constexpr u32 kFontFirst = 0x%02X; ///< the first glyph of the tables (space)' % FIRST,
              'constexpr u32 kFontGlyphs = %d;  ///< 0x%02X-0x%02X' % (LAST - FIRST + 1, FIRST, LAST), '']
    lines += t6 + [''] + t8 + [''] + t10
    with open(out_path, 'wb') as f:
        f.write(('\n'.join(lines) + '\n').encode('ascii'))


if __name__ == '__main__':
    main()
