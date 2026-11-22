#!/usr/bin/env python3
"""Makes the golden data of the SYSTEM's Download Play application as the client of the real Download Play (1.1.4, G1: dlp:CLNT served by the
plugin, specifiche/1-G1.md sections 4.4 and 4.6): the four packets the application's side answers with - the AuthAck (A2), the answer to
StartDistribution without content (A4), the answer to the Finish (A6) and the answer to the notice (A9) - as two Azahar emulators exchanged
them through a room on 2026-11-13 (Mario Party: Island Tour, "prova 2": the first hosts the game, the second is the Download Play application
with the child 00040001000F8200 already installed, so nothing is downloaded; the good session, t=402.106 .. 402.438).

    python -I Tests/make_dlp_real_clnt_goldens.py <scambio-dlp-senza-scaricare.txt> Tests/dlp_real_clnt_golden.inc

The recording stays outside the repository (the kit of analysis/island-tour-2026-11-13, folder kit-azahar/registri/2-guest-con-figlio):
`scambio-dlp-senza-scaricare.txt` is the exchange of the good session as text, one line per distinct frame. Read here: the nine SecureData
packets of data channel 2 between t=402.106 and t=402.438 (A1 .. A9 of PROGETTO-DLP-VERO-1.1.3.md section 2.3), every one checked against
the seed of the capture's checksums (the one seed of all 2^32 that fits, as Tests/make_dlp_real_island_goldens.py found it).
Written (after Tests/dlp_real_luigi_golden.inc, whose struct DlpRealGolden it uses): the four packets of node 2, and as NUMBERS only the
sequence and the id (+0x0D, +0x0E..0F) of the host's packet each one answers - no byte of a packet of the host is copied.

What is not ours to publish stays out of the file, and the script REFUSES to write a file that has any of it:
  - the PASSPHRASE of the session (the host's final, +0x14, eight characters): it is read only to look for it. Wherever a packet that is
    written carries it, it is REPLACED by the stand-in "1234567v" and the checksum is made again with the capture's seed; after that no
    four bytes of it in a row may be left in a packet, nor in the text as characters or as hex;
  - the NAMES of the two emulators and their ADDRESSES (as the room printed them): in no byte of a packet, in no line of the text.
Nothing is edited by hand: run it again on the same recording and diff.
"""
import re
import sys

RE_DATA = re.compile(r'^\s*(\d+\.\d+) x(\d+)\s+\(ultimo\s+(\d+\.\d+)\)\s+c\d+ Data ch(\d+) (\S+) -> (\S+)\s+SecureData canale (\d+) seq N '
                     r'nodo (\d+) -> (\d+) mgmt (\d) \[(\d+)\] ([0-9a-f]+)\s*$')
SEED_CANDIDATES = [0xb8f46bff]   # the seed of the capture's checksums (Tests/make_dlp_real_island_goldens.py: a search of all 2^32, one)
SEED_ZERO = bytes(4)
T_GOOD = (402.106, 402.438)      # the dialogue of the good session
STAND_IN = b'1234567v'           # what replaces the passphrase wherever a written packet would carry it
WANT_IDS = ['A1', 'A3', 'A2', 'A4', 'A5', 'A6', 'A7', 'A8', 'A9']
ANSWERS = [('A2', 'A1'), ('A4', 'A3'), ('A6', 'A5'), ('A9', 'A8')]   # the application's packet, and the host's packet it answers


def be32(b, off):
    return int.from_bytes(b[off:off + 4], 'big')


def frame_sum(b):
    s = 0
    i = 0
    while i + 4 <= len(b):
        if i != 8:
            s += be32(b, i)
        i += 4
    if i < len(b):
        s += be32(b[i:] + bytes(4 - (len(b) - i)), 0)
    return s & 0xFFFFFFFF


def checksum(seed, b):
    # dlph_checksum: the step is (h >> s) | (h << s) with the SAME s both ways - not a rotation (design 2.5)
    h = frame_sum(b)
    key = int.from_bytes(seed, 'big')
    sh = (seed[2] & 0xF) + 4
    cnt = (seed[3] & 7) + 2
    for _ in range(cnt):
        h = (((h >> sh) | (h << sh)) & 0xFFFFFFFF) ^ key
    return h


def sealed(seed, b):
    out = bytearray(b)
    out[8:12] = checksum(seed, b).to_bytes(4, 'big')
    return bytes(out)


def dialog_id(pk, src):
    t = pk[0]
    from_host = src == 1
    if t == 2:
        return 'A1' if from_host else 'A2'
    if t == 3:
        return 'A3' if from_host else 'A4'
    if t == 5:
        return 'A5' if from_host else 'A6'
    if t == 6:
        if not from_host:
            return 'A9'
        return 'A7' if len(pk) == 0x20 else 'A8'
    raise SystemExit('unknown packet type %d' % t)


def refuse(found, what):
    """Stops - also under python -O, where an assert is not run - when something of the recording that must stay out is about to be written."""
    if found:
        raise SystemExit('REFUSED, nothing written: ' + what)


def windows(b, n=4):
    """every n bytes in a row of b"""
    return [b[i:i + n] for i in range(len(b) - n + 1)]


def c_array(out, name, b, what):
    out.append('// %s' % what)
    out.append('static const std::uint8_t %s[%d] = {' % (name, len(b)))
    for i in range(0, len(b), 24):
        out.append('    ' + ', '.join('0x%02x' % x for x in b[i:i + 24]) + ',')
    out.append('};')


def main():
    src, dest = sys.argv[1:3]
    rows = []
    for line in open(src, encoding='utf-8', errors='replace'):
        m = RE_DATA.match(line)
        if not m:
            continue
        t, times, _last, _wch, tx, rx, ch, s, d, mgmt, n, hx = m.groups()
        if int(ch) != 2:
            continue
        b = bytes.fromhex(hx)
        refuse(len(b) != int(n), 'a packet of channel 2 at t=%s is not whole in the recording (%d bytes for [%s])' % (t, len(b), n))
        rows.append({'t': t, 'times': int(times), 'tx': tx, 'rx': rx, 'src': int(s), 'dst': int(d), 'mgmt': int(mgmt), 'b': b})
    good = [r for r in rows if T_GOOD[0] <= float(r['t']) <= T_GOOD[1]]
    assert len(good) == 9 and all(r['times'] == 1 and r['mgmt'] == 0 for r in good), [(r['t'], r['times']) for r in good]
    ids = [dialog_id(r['b'], r['src']) for r in good]
    assert ids == WANT_IDS, ids
    by = {pid: r for pid, r in zip(ids, good)}
    for pid, r in by.items():
        assert (r['src'], r['dst']) == ((1, 2) if pid in ('A1', 'A3', 'A5', 'A7', 'A8') else (2, 1)), pid
        b = r['b']
        assert int.from_bytes(b[4:6], 'big') == len(b) and b[1] == 2 and b[2:4] == b'\0\0' and b[6:8] == b'\x02\x00', pid
    seeds = [c.to_bytes(4, 'big') for c in SEED_CANDIDATES if all(checksum(c.to_bytes(4, 'big'), r['b']) == be32(r['b'], 8) for r in good)]
    assert len(seeds) == 1, [s.hex() for s in seeds]
    seed = seeds[0]
    # the session's passphrase (read to be looked for, never written): the shape the game gives it, seven hex digits and a 'v'
    final = by['A7']['b']
    passphrase = final[0x14:0x1C]
    refuse(not re.fullmatch(rb'[0-9a-f]{7}v', passphrase) or final[0x1C] != 0, 'the final of the recording does not hold a passphrase where it should')
    host_tx, app_tx = by['A1']['tx'], by['A2']['tx']
    refuse(by['A1']['rx'] != app_tx or by['A2']['rx'] != host_tx or host_tx == app_tx, 'the two nodes of the recording are not read')

    packets = []
    replaced = 0
    for mine, asked in ANSWERS:
        r = by[mine]
        g = r['b']
        while passphrase in g:   # (none of the four answers carries it: the count says so in the file)
            k = g.index(passphrase)
            g = sealed(seed, g[:k] + STAND_IN + g[k + len(STAND_IN):])
            replaced += 1
        assert checksum(seed, g) == be32(g, 8), mine + ' (as written)'
        h = by[asked]['b']
        packets.append((mine, asked, r, g, h[0x0D], h[0x0E], h[0x0F]))

    # nothing of the passphrase, the names or the addresses in the bytes that are written
    nicks = [tx[18:].rstrip(')') for tx in (host_tx, app_tx)]   # ("aa:bb:cc:dd:ee:ff(name)": the name the emulator had in the room)
    macs = [bytes.fromhex(tx[:17].replace(':', '')) for tx in (host_tx, app_tx)]
    refuse(not all(nicks) or any(len(mac) != 6 for mac in macs) or macs[0] == macs[1], 'the two nodes of the recording are not read')
    refuse(any(w in p[3] for w in windows(passphrase) for p in packets), 'four bytes of the passphrase of the recording in a packet')
    refuse(any(mac in p[3] or mac[::-1] in p[3] for mac in macs for p in packets), 'an address of a node of the recording in a packet')
    refuse(any(n.encode(enc) in p[3] for n in nicks for enc in ('utf-8', 'utf-16-be', 'utf-16-le') for p in packets), 'a name of a node of the recording in a packet')

    out = []
    out.append('// Generated by Tests/make_dlp_real_clnt_goldens.py from the recording of 2026-11-13 (two Azahar emulators, Mario Party: Island Tour, '
               'the host = node 1, the Download Play application with the child installed = node 2) - do not edit by hand.')
    out.append('// The four packets the Download Play APPLICATION answers the host with, data channel 2 of the good session (t=%.3f to t=%.3f; ids of '
               'PROGETTO-DLP-VERO-1.1.3.md section 2.3):' % T_GOOD)
    out.append('// the AuthAck (A2), the answer to StartDistribution without content (A4), to the Finish (A6) and to the notice (A9). They are what '
               'dlp:CLNT served by the plugin must send')
    out.append('// (Tests/dlp_real_client_test.cpp, RC8), byte for byte but for what the HOST put in its own packet: its sequence (+0x0D) and its id of '
               'the client (+0x0E..0F) - here as numbers,')
    out.append('// `kDlpRealClntAsked` - and +0x16..0x19 of A6, a member Azahar\'s client never initialises. No byte of a packet of the host is in this '
               'file; the checksums (+8..11) are the')
    out.append('// capture\'s, seed %s; `checksum_zero` is the checksum of the same bytes with the seed 00 00 00 00 (what the plugin seals with).'
               % ' '.join('%02x' % x for x in seed))
    out.append('// What is NOT here: the session\'s passphrase (none of these four packets carries it: %d replaced by the stand-in "%s"; the script '
               'refuses four bytes of it in a row), the names and the' % (replaced, STAND_IN.decode()))
    out.append('// addresses of the two nodes (in no byte and no line of this file).')
    out.append('// Needs Tests/dlp_real_luigi_golden.inc before it (struct DlpRealGolden), <cstddef> and <cstdint>.')
    out.append('')
    out.append('static const std::uint8_t kDlpRealClntSeed[4] = {%s};' % ', '.join('0x%02x' % x for x in seed))
    out.append('static const unsigned kDlpRealClntPassReplaced = %d;  // the passphrase of the session found in these packets and replaced by the stand-in' % replaced)
    out.append('')
    names = []
    for k, (mine, asked, r, g, seq, r0, r1) in enumerate(packets):
        name = 'kDlpRealClnt_%d_%s' % (k, mine)
        names.append(name)
        c_array(out, name, g, '%s  t=%s  channel 2  node %d -> %d  %d bytes  (the answer to %s)' % (mine, r['t'], r['src'], r['dst'], len(g), asked))
    out.append('')
    out.append('static const DlpRealGolden kDlpRealClntAnswers[4] = {')
    for k, (mine, asked, r, g, seq, r0, r1) in enumerate(packets):
        out.append('    {"%s", "%s", 2, %d, %d, %d, %s, 0x%08x, 0x%08x, {0x0, 0x0}},'
                   % (mine, r['t'], r['src'], r['dst'], len(g), names[k], be32(g, 8), checksum(SEED_ZERO, g)))
    out.append('};')
    out.append('// The host\'s packet each one answers (A1, A3, A5, A8): its sequence (+0x0D) and its id of the client (+0x0E, +0x0F), as the recording has them.')
    out.append('static const std::uint8_t kDlpRealClntAsked[4][3] = {%s};'
               % ', '.join('{0x%02x, 0x%02x, 0x%02x}' % (seq, r0, r1) for (_m, _a, _r, _g, seq, r0, r1) in packets))
    text = '\n'.join(out) + '\n'
    # (... and on the text around the bytes: no passphrase as characters or as hex, no address as it is printed or as hex, no name)
    low = text.lower()
    bad = [w.decode('latin-1').lower() for w in windows(passphrase)] + [w.hex() for w in windows(passphrase)]
    bad += [tx[:17].lower() for tx in (host_tx, app_tx)] + [mac.hex() for mac in macs] + [n.lower() for n in nicks]
    refuse(any(x in low for x in bad), 'the passphrase, an address or a name of a node of the recording in the text')
    open(dest, 'w', newline='\n').write(text)
    print('wrote', dest, ':', ' '.join(p[0] for p in packets), '-', sum(len(p[3]) for p in packets), 'bytes; seed', seed.hex(),
          '; every checksum checked; the passphrase replaced', replaced, 'times')


main()
