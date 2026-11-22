#!/usr/bin/env python3
"""Makes Tests/settings_test/sold_catalog.inc: the low word of every title of titles.txt, the list of every 3DS title the public lobbies use
(repository 3DS-Link-Public-Lobbies, made there by tools/build_titles.py; lines "id:region:family", the id in hex).

    python3 Tests/make_sold_catalog.py <titles.txt>            writes the file
    python3 Tests/make_sold_catalog.py <titles.txt> --check    says whether the file is the one this list gives, writes nothing

Tests/settings_test/games_test.cpp holds every id of the file against the two ranges of setup::IsSoldGameTid (ConfigApp/source/games.hpp): a new
list with a title outside them fails that test, and the ranges are looked at again. The list is not part of this tree: the file is made by hand,
when the list changes.

(2026-11-15) It also makes Tests/title_families.inc: every title of the list with its family (the three letters of the product code), so that
Tests/flow_relief_test.cpp holds every row of Includes/azl/title_rules.hpp to the WHOLE family of its game - every region, with no pin for any game.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'settings_test', 'sold_catalog.inc')
OUT_FAMILIES = os.path.join(HERE, 'title_families.inc')
NL = chr(10)


def families(path):
    """the second file: {low word, family packed in 24 bits} for every title, sorted by the id"""
    stamp = ''
    rows = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line.startswith('#titles'):
                stamp = line[1:]
            if not line or line.startswith('#'):
                continue
            low, _region, family = line.split(':')
            if len(family) != 3 or not family.isalnum():
                raise SystemExit('make_sold_catalog.py: %s has a family that is not three letters: %s' % (path, line))
            rows.append((int(low, 16), (ord(family[0]) << 16) | (ord(family[1]) << 8) | ord(family[2])))
    rows.sort()
    text = ['// Every title of titles.txt (%s) with its family, {low word, the three letters of the family as 0xAABBCC}, sorted: made by' % stamp,
            '// Tests/make_sold_catalog.py, do not edit by hand. %d titles' % len(rows)]
    for at in range(0, len(rows), 6):
        text.append(' '.join('{0x%08X, 0x%06X},' % r for r in rows[at:at + 6]))
    return (NL.join(text) + NL).encode('ascii')


def make(path):
    stamp = ''
    ids = []
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if line.startswith('#titles'):
                stamp = line[1:]
            if not line or line.startswith('#'):
                continue
            ids.append(int(line.split(':')[0], 16))
    if not ids or len(set(ids)) != len(ids) or max(ids) > 0xFFFFFFFF:
        raise SystemExit('make_sold_catalog.py: %s is not the list (no title, or an id twice, or one above 32 bits)' % path)
    ids.sort()
    text = ['// The low word of every title of titles.txt (%s), sorted: made by Tests/make_sold_catalog.py, do not edit by hand.' % stamp,
            '// %d titles' % len(ids)]
    for at in range(0, len(ids), 12):
        text.append(' '.join('0x%08X,' % i for i in ids[at:at + 12]))
    return (NL.join(text) + NL).encode('ascii')


def main():
    args = [a for a in sys.argv[1:] if a != '--check']
    if len(args) != 1:
        raise SystemExit(__doc__)
    outputs = [(OUT, make(args[0])), (OUT_FAMILIES, families(args[0]))]
    if '--check' in sys.argv[1:]:
        bad = 0
        for out, data in outputs:
            with open(out, 'rb') as f:
                same = f.read() == data
            print('%s is %s' % (os.path.basename(out), 'the one this list gives' if same else 'NOT the one this list gives'))
            bad += 0 if same else 1
        sys.exit(1 if bad else 0)
    for out, data in outputs:
        with open(out, 'wb') as f:
            f.write(data)
        print('%s: %d bytes' % (out, len(data)))


if __name__ == '__main__':
    main()
