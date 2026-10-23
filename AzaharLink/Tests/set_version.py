#!/usr/bin/env python3
"""set_version.py X.Y.Z   -- the ONE place where the version of 3DS Link changes.

3DS Link is ONE product with ONE version (the user's rule, 2026-10-21): the app, the plugin, the CIAs, the Download Play child and the documents all say the
same X.Y.Z. A patch changes this number and nothing else by hand: this script rewrites every point of the SOURCES that carries it, and then lists what has to be
rebuilt and which numbers of the documents follow (Tests/update_dist_numbers.py does those). Tests/policy_check.sh refuses a tree where one of the points says
another number, so a forgotten point is a failed check, not a surprise on a console.

  python Tests/set_version.py 1.0.1      writes the points below
  python Tests/set_version.py --check    only checks that every point says VERSION (exit 1 if not)
  python Tests/set_version.py --show     prints VERSION and its TMD number

The points (all relative to AzaharLink/):
  VERSION                                   the number itself (DlpLink/tools read it for the TMD of the CIAs, the manifest and the child's description)
  Includes/ctr/plugin.hpp                   kVersion (the plugin's log header and in-game menu text)
  Sources/main.cpp                          PluginMenu("AzaharLink", MAJOR, MINOR, REVISION, ...)
  CTRPluginFramework.plgInfo                Major / Minor / Revision: the header of the .3gx (the app compares it with its own kVersion)
  ConfigApp/source/settings.hpp             kVersion (the app's title bar, About, the Installation check)
  README.md                                 "Stato attuale: X.Y.Z." and the heading "## Novità X.Y.Z ("
  PROVE_HARDWARE.md                         the title, "la X.Y.Z" and the 3gx header word
  dist/LEGGIMI.txt                          the title, "la X.Y.Z" and the two table rows that say the version
The TMD of the CIA (major << 10 | minor << 4 | micro, so micro is 0..15 and minor/major 0..63), the model of the Download Play child that the app carries (and the child the app
composes from it: 3DS Link is ONE CIA) and the descriptions of the SMDH ("Powered by Gionko · vX.Y.Z", "Connect, share, play together · vX.Y.Z") come from VERSION when the
CIA and the apps are built (DlpLink/tools/build_all.sh); the composer refuses a model of another version than the app's.
Windows Python or Docker; no dependencies.
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def path(rel):
    return os.path.join(ROOT, rel.replace('/', os.sep))


def read(rel):
    with open(path(rel), encoding='utf-8', newline='') as f:
        return f.read()


def write(rel, text):
    with open(path(rel), 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def parse(v):
    m = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)', v)
    if not m:
        sys.exit('ERROR: "%s" is not X.Y.Z' % v)
    major, minor, micro = (int(x) for x in m.groups())
    if v != '%d.%d.%d' % (major, minor, micro):
        sys.exit('ERROR: "%s" is not canonical X.Y.Z (no leading zeros, no spaces: the app compares the version as TEXT with the one in the plugin)' % v)
    if not (0 <= major < 64 and 0 <= minor < 64 and 0 <= micro < 16):
        sys.exit('ERROR: %s does not fit the TMD (major 0-63, minor 0-63, micro 0-15: a version is major << 10 | minor << 4 | micro)' % v)
    return major, minor, micro


def tmd(v):
    major, minor, micro = parse(v)
    return (major << 10) | (minor << 4) | micro


def word3gx(v):
    major, minor, micro = parse(v)
    return '0x%02X%02X%02X00' % (major, minor, micro)       # the header's version word: major, minor, revision, 0 (0.1.17 is 0x00011100)


def current():
    return read('VERSION').strip()


# Each point: (file, regex, replacement template with {v} {major} {minor} {micro} {w3gx} {tmd}, how many times it must be found)
def points(v):
    major, minor, micro = parse(v)
    fmt = dict(v=v, major=major, minor=minor, micro=micro, w3gx=word3gx(v), tmd=tmd(v))
    table = [
        ('Includes/ctr/plugin.hpp', r'constexpr const char\* kVersion = "[0-9.]+";[^\n]*',
         'constexpr const char* kVersion = "{v}"; // (the log header and the menu text; the menu\'s ints and plgInfo say {v}: Tests/set_version.py writes all of them)', 1),
        ('Sources/main.cpp', r'PluginMenu\("AzaharLink", \d+, \d+, \d+,', 'PluginMenu("AzaharLink", {major}, {minor}, {micro},', 1),
        ('CTRPluginFramework.plgInfo', r'(?m)^(    )Major: \d+', r'\1Major: {major}', 1),
        ('CTRPluginFramework.plgInfo', r'(?m)^(    )Minor: \d+', r'\1Minor: {minor}', 1),
        ('CTRPluginFramework.plgInfo', r'(?m)^(    )Revision: \d+', r'\1Revision: {micro}', 1),
        ('ConfigApp/source/settings.hpp', r'constexpr const char\* kVersion = "[0-9.]+";', 'constexpr const char* kVersion = "{v}";', 1),
        ('README.md', r'(> \*\*Stato attuale: )\d+\.\d+\.\d+(\.\*\*)', r'\g<1>{v}\g<2>', 1),
        ('README.md', r"(: \*\*)\d+\.\d+\.\d+(\*\* è quella dell'app)", r'\g<1>{v}\g<2>', 1),
        ('README.md', r'(Powered by Gionko · v)\d+\.\d+\.\d+(» sotto il nome)', r'\g<1>{v}\g<2>', 1),
        ('README.md', r'(==== AzaharLink )\d+\.\d+\.\d+( \(built)', r'\g<1>{v}\g<2>', 2),
        ('README.md', r'(la parola di versione `)0x[0-9A-Fa-f]{8}(` per la )\d+\.\d+\.\d+', r'\g<1>{w3gx}\g<2>{v}', 1),
        ('PROVE_HARDWARE.md', r'(?m)^(# Prove su hardware - AzaharLink )\d+\.\d+\.\d+', r'\g<1>{v}', 1),
        ('PROVE_HARDWARE.md', r'(una sola versione da provare: la )\d+\.\d+\.\d+(\*\*)', r'\g<1>{v}\g<2>', 1),
        ('PROVE_HARDWARE.md', r'(==== AzaharLink )\d+\.\d+\.\d+( \(built)', r'\g<1>{v}\g<2>', 1),
        ('PROVE_HARDWARE.md', r'(dimensione 0, versione `)0x[0-9A-Fa-f]{8}(`)', r'\g<1>{w3gx}\g<2>', 1),
        ('dist/LEGGIMI.txt', r'(?m)^AzaharLink \d+\.\d+\.\d+ - LEGGIMI', 'AzaharLink {v} - LEGGIMI', 1),
        ('dist/LEGGIMI.txt', r'(da installare e da provare: la )\d+\.\d+\.\d+(\.)', r'\g<1>{v}\g<2>', 1),
        ('dist/LEGGIMI.txt', r'(intestazione 3gx: versione )\d+\.\d+\.\d+(,)', r'\g<1>{v}\g<2>', 1),
        ('dist/LEGGIMI.txt', r"(l'app 3DS Link \(mostra v)\d+\.\d+\.\d+(;)", r'\g<1>{v}\g<2>', 1),
        ('dist/LEGGIMI.txt', r'(e mostra v)\d+\.\d+\.\d+(;)', r'\g<1>{v}\g<2>', 1),
        ('dist/LEGGIMI.txt', r'(==== AzaharLink )\d+\.\d+\.\d+( \(built)', r'\g<1>{v}\g<2>', 1),
        ('dist/LEGGIMI.txt', r'(Title ID 000400000F83D100, versione )\d+\.\d+\.\d+(,)', r'\g<1>{v}\g<2>', 1),
        ('../DlpLink/test/PROVA-INSTALLATORE.md', r'(Powered by Gionko · v)\d+\.\d+\.\d+(»)', r'\g<1>{v}\g<2>', 1),
    ]
    return fmt, table


STUB = 'TODO-SCRIVERE'


def readme_sections(text, v):
    """The README keeps one "## Novità X.Y.Z (" section per version and the old ones keep their number. A version that has no section yet gets a STUB above
    the newest one (the build and Tests/policy_check.sh refuse a README that still has the stub: somebody writes what changed). Stubs of other versions go."""
    text = re.sub(r'(?m)^## Novità (?!%s )\d+\.\d+\.\d+ \(%s[^\n]*\n\n?' % (re.escape(v), STUB), '', text)
    if re.search(r'(?m)^## Novità %s ' % re.escape(v), text):
        return text
    first = re.search(r'(?m)^## Novità \d+\.\d+\.\d+ \(', text)
    if not first:
        return text
    stub = '## Novità %s (%s: cosa cambia in questa versione; la sezione della versione precedente resta sotto)\n\n' % (v, STUB)
    return text[:first.start()] + stub + text[first.start():]


def apply(v, write_files):
    fmt, table = points(v)
    problems = []
    cache = {}
    for rel, rx, repl, count in table:
        if rel.startswith('../') and not os.path.exists(path(rel)):
            continue   # (a document of the sibling project DlpLink, when the tree is a copy of AzaharLink alone)
        text = cache.get(rel)
        if text is None:
            text = read(rel)
        new, n = re.subn(rx, lambda m, repl=repl: m.expand(repl.format(**fmt)) if '\\' in repl else repl.format(**fmt), text)
        if n != count:
            problems.append('%s: the pattern %r was found %d times (expected %d)' % (rel, rx[:50], n, count))
        cache[rel] = new
    if not write_files:
        cache['README.md'] = readme_sections(cache['README.md'], v)
        return problems, cache
    cache['README.md'] = readme_sections(cache['README.md'] if 'README.md' in cache else read('README.md'), v)
    # the underline of the title of LEGGIMI.txt follows the length of the title
    t = cache.get('dist/LEGGIMI.txt')
    if t is not None:
        t = re.sub(r'(?m)^(AzaharLink [0-9.]+ - LEGGIMI)\n=+\n', lambda m: m.group(1) + '\n' + '=' * len(m.group(1)) + '\n', t, count=1)
        cache['dist/LEGGIMI.txt'] = t
    if problems:
        return problems, cache
    for rel, text in cache.items():
        write(rel, text)
    write('VERSION', v + '\n')
    return problems, cache


def check():
    v = current()
    parse(v)
    problems, cache = apply(v, False)
    # a point is right when rewriting it with VERSION changes nothing
    for rel, text in cache.items():
        if read(rel) != text:
            problems.append('%s does not say %s' % (rel, v))
    readme = read('README.md')
    if STUB in readme:
        problems.append('README.md still has the stub "%s": write the section "## Novità %s (" (what changed in this version)' % (STUB, v))
    if not re.search(r'(?m)^\| (\*\*)?%s(\*\*)? \|' % re.escape(v), readme.replace(chr(13), '')):
        problems.append('the history table of the README ("Storia delle versioni") has no row for %s' % v)
    return problems


def main():
    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        sys.exit(__doc__)
    if args[0] == '--show':
        v = current()
        print('%s (TMD %d)' % (v, tmd(v)))
        return
    if args[0] == '--check':
        problems = check()
        for p in problems:
            print('ERROR:', p)
        print('every point says %s' % current() if not problems else '%d point(s) wrong' % len(problems))
        sys.exit(1 if problems else 0)
    v = args[0]
    parse(v)
    old = current()
    problems, _ = apply(v, True)
    for p in problems:
        print('ERROR:', p)
    if problems:
        sys.exit(1)
    print('version %s -> %s (TMD %d, 3gx word %s)' % (old, v, tmd(v), word3gx(v)))
    print('Written: VERSION, plugin.hpp, Sources/main.cpp, plgInfo, settings.hpp, README, PROVE_HARDWARE, dist/LEGGIMI.txt.')
    print('Now REBUILD (DlpLink/tools/build_all.sh does all of it: plugin, installer, the model of the Download Play child, app, the CIA, checks, dist) and then Tests/update_dist_numbers.py.')
    print('Then, in the README: write the section "## Novità %s (" (a stub is there if it was new) and add the row of %s to the table "Storia delle versioni"' % (v, v))
    print('(--check, build_all.sh and Tests/policy_check.sh refuse a README with the stub or without the row).')


if __name__ == '__main__':
    main()
