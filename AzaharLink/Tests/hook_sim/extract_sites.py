#!/usr/bin/env python3
"""Takes the few words around a game's call site, once, for Tests/hook_sim/gate_sites.json (read by gate_sim.py).

    python Tests/hook_sim/extract_sites.py <code.bin | Luma dump> <site> --name N --game G --kind stub|inline
        [--caller arm|thumb] [--end ADDR] [--words N] [--restore ADDR=WORD ...] [--source TEXT] [--append FILE]
    python Tests/hook_sim/extract_sites.py --hand W0,W1,... <site> --name N ...        (words written by hand)

A code.bin (the decompressed .code of the exefs) is loaded at 0x00100000. A Luma3DS ARM11 crash dump (magic DEADC0DE
DEADCAFE) carries the code that ends at its PC: the words are taken from there, and --restore puts back the game's own
words where the dump shows ours (a hook of 1.1.1). Read only: nothing is written but the JSON (--append replaces the
entry with the same name, or adds it; without --append the entry is printed).

kind "stub": a shared `svc; bx lr` the game calls (the sim calls it from an ARM `bl`, or a Thumb `blx` with --caller thumb).
kind "inline": a site inside its function: the sim starts there and stops at --end (or when the pc leaves the words).
"""
import argparse
import json
import os
import struct
import sys

CODE_BASE = 0x00100000


def words_from(path, site, count, restore):
    data = open(path, 'rb').read()
    if data[:8] == struct.pack('<2I', 0xDEADC0DE, 0xDEADCAFE):
        h = struct.unpack_from('<10I', data, 0)
        regs_size, code_size = h[6], h[7]
        pc = struct.unpack_from('<I', data, 40 + 15 * 4)[0]
        code = data[40 + regs_size:40 + regs_size + code_size]
        base = pc + 4 - code_size
        kind = 'Luma dump (code %08X-%08X)' % (base, base + code_size)
    else:
        code, base, kind = data, CODE_BASE, 'code.bin'
    out = []
    for i in range(count):
        a = site + 4 * i
        off = a - base
        if off < 0 or off + 4 > len(code):
            break
        out.append(restore.get(a, struct.unpack_from('<I', code, off)[0]))
    return out, kind


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source', nargs='?')
    ap.add_argument('site')
    ap.add_argument('--hand')
    ap.add_argument('--name', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--kind', choices=('stub', 'inline'), required=True)
    ap.add_argument('--caller', choices=('arm', 'thumb'), default='arm')
    ap.add_argument('--end')
    ap.add_argument('--words', type=int, default=8)
    ap.add_argument('--restore', action='append', default=[])
    ap.add_argument('--source-note', dest='note', default='')
    ap.add_argument('--append')
    a = ap.parse_args()
    site = int(a.site, 16)
    restore = {}
    for r in a.restore:
        k, v = r.split('=')
        restore[int(k, 16)] = int(v, 16)
    if a.hand:
        words, kind = [int(w, 16) for w in a.hand.split(',')], 'written by hand'
    else:
        words, kind = words_from(a.source, site, a.words, restore)
    if not words or words[0] not in (0xEF000032, 0xEF000023):
        sys.exit('no svc 0x32 / 0x23 at %08X (%s)' % (site, ['%08X' % w for w in words[:2]]))
    entry = {
        'name': a.name,
        'game': a.game,
        'source': '%s%s%s' % (kind, ': ' if a.note else '', a.note),
        'kind': a.kind,
        'svc': '0x%02X' % (words[0] & 0xFF),
        'site': '0x%08X' % site,
        'words': ['%08X' % w for w in words],
    }
    if a.kind == 'stub':
        entry['caller'] = a.caller
    if a.end:
        entry['end'] = '0x%08X' % int(a.end, 16)
    if restore:
        entry['restored'] = {'0x%08X' % k: '%08X' % v for k, v in sorted(restore.items())}
    if not a.append:
        print(json.dumps(entry, indent=2))
        return
    doc = json.load(open(a.append)) if os.path.exists(a.append) else {'sites': []}
    doc['sites'] = [s for s in doc['sites'] if s['name'] != a.name] + [entry]
    with open(a.append, 'w', newline='\n') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')
    print('%s: %d words at %08X (%s)' % (a.name, len(words), site, kind))


if __name__ == '__main__':
    main()
