#!/usr/bin/env python3
"""update_dist_numbers.py -- after a build: the size and the md5 of the files that are delivered, in the two documents that give them.

3DS Link is ONE package (the user's rule): dist/ holds 3DSLink.cia and 3DSLink.3dsx, and the plugin is INSIDE both of them (a resource of the app, which writes it to
sdmc:/3ds/3DSLink/AzaharLink.3gx by itself): there is no dist/AzaharLink.3gx and no dist/dlp/child.cia (the app makes the Download Play child on the console).
Tests/policy_check.sh wants the real size and md5 of dist/3DSLink.3dsx, of dist/3DSLink.cia (when it is there) and of the plugin the app carries (ConfigApp/data/AzaharLink.3gx:
the file that has to appear on the SD card, so the person who tests can check it) in dist/LEGGIMI.txt and in PROVE_HARDWARE.md. After every build they change (makerom and the
log header's __TIME__ make every build different), and doing it by hand is how a number is forgotten: this script rewrites them. (The version itself is Tests/set_version.py's business.)

  python Tests/update_dist_numbers.py          rewrites the numbers (and says what changed)
  python Tests/update_dist_numbers.py --check  only says whether the documents hold the numbers of the files (exit 1 if not)
"""
import hashlib
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
# name in the documents -> where the file is (relative to AzaharLink/). The plugin is not a file of dist/: it is the resource the app is built with.
FILES = {
    'AzaharLink.3gx': 'ConfigApp/data/AzaharLink.3gx',
    '3DSLink.3dsx': 'dist/3DSLink.3dsx',
    '3DSLink.cia': 'dist/3DSLink.cia',
}


def dotted(n):
    return '{:,}'.format(n).replace(',', '.')


def facts(name):
    p = os.path.join(ROOT, FILES[name].replace('/', os.sep))
    if not os.path.isfile(p):
        return None
    data = open(p, 'rb').read()
    return dotted(len(data)), hashlib.md5(data).hexdigest()


# (document, file, regex with the size in group 2 and the md5 in group 4; groups 1 and 3 are kept as they are)
def patterns(name):
    n = re.escape(name)
    return [
        ('dist/LEGGIMI.txt', r'(?m)(^  %s\s+)([0-9.]+)( byte\s+md5 )([0-9a-f]{32})' % n),
        ('PROVE_HARDWARE.md', {
            'AzaharLink.3gx': r'(la risorsa `AzaharLink\.3gx`, )([0-9.]+)( byte, md5\s+`)([0-9a-f]{32})',
            '3DSLink.3dsx': r'(\), )([0-9.]+)( byte, md5 `)([0-9a-f]{32})(` \(contiene anche il modulo)',
            '3DSLink.cia': r'(dist/3DSLink\.cia` \()([0-9.]+)( byte, md5 `)([0-9a-f]{32})',
        }[name]),
    ]


def main():
    check = '--check' in sys.argv[1:]
    bad = 0
    docs = {}
    for name in FILES:
        f = facts(name)
        if f is None:
            if name in ('AzaharLink.3gx', '3DSLink.3dsx'):
                print('ERROR: %s is missing' % FILES[name])
                bad += 1
            continue
        size, md5 = f
        for doc, rx in patterns(name):
            text = docs.get(doc)
            if text is None:
                with open(os.path.join(ROOT, doc.replace('/', os.sep)), encoding='utf-8', newline='') as fh:
                    text = fh.read()
            found = re.findall(rx, text)
            if len(found) != 1:
                print('ERROR: %s: the numbers of %s were found %d times (expected 1)' % (doc, FILES[name], len(found)))
                bad += 1
                docs[doc] = text
                continue
            new, _ = re.subn(rx, lambda m: m.group(1) + size + m.group(3) + md5 + (m.group(5) if m.lastindex >= 5 else ''), text)
            if new != text:
                print('%s: %s -> %s byte, md5 %s' % (doc, FILES[name], size, md5))
                if check:
                    bad += 1
            docs[doc] = new
    if not check and not bad:
        for doc, text in docs.items():
            with open(os.path.join(ROOT, doc.replace('/', os.sep)), 'w', encoding='utf-8', newline='') as fh:
                fh.write(text)
    if check:
        print('the documents hold the numbers of the delivered files' if not bad else 'the documents do NOT hold the numbers of the delivered files')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
