#!/usr/bin/env python3
"""Makes the golden data of the real Download Play of MARIO KART 7 (the RealDlp mode with eight players) from the capture of eight Azahar
emulators that played it through a room (2026-11-13: MK7-1 hosts, seven join as fake clients, the race starts).

    python -I Tests/make_dlp_real_mk7_goldens.py <dati.txt> <decodifica.py> <staffetta.cap> Tests/dlp_real_mk7_golden.inc

The capture (222 MB) and its decoding stay outside the repository (the kit of analysis/mk7-azahar-2026-11-13, folder
kit/registri/1-otto-azahar): `dati.txt` is its decoded data frames (decodificato/dati.txt), `decodifica.py` the kit's decoder and
`staffetta.cap` the capture itself, read through the decoder for the frames that are not data frames (the beacon, the node map).
What the tests need from it is small and is written as a C++ include file next to the tests (after Tests/dlp_real_luigi_golden.inc,
whose struct DlpRealGolden it uses):

  - the five frames of the announcement group of t=888.688 (data channel 1, host -> everyone): 768 + 3 x 1448 + 1464 bytes;
  - EVERY packet of data channel 2 of the good session, t=901.154 .. 1035.002 (65 packets: the Auth and its answer of the seven
    clients as each comes in, then for each of the seven StartDistribution, its answer, the Finish, its answer, the final with the
    passphrase);
  - as whole frames: the host's beacon alone (t=888.786) and with the eight nodes (t=1027.443), the node map and the EAPoL-Logoff the host
    broadcast when the seventh client came in (t=1027.394), the EAPoL-Start of that client.

What is Nintendo's stays out of the repository (the owner's rule of 2026-10-23): the title and the icon of the game, which the host puts in
frame 0 (+0x40..+0x2F7) and in frames 1-3 (+0x10 to the end), are REPLACED by zeros, except the first word of each such range, which holds
the 32-bit sum of the big-endian words that were there (the checksum is a function of that sum only: it still holds). This script finds
the seed of the capture's checksums (the candidates that fit three packets are tried on all seventy) and checks every packet with it.
The addresses in the node map are the ones the kit gave its eight emulators (costruisci-otto.py), nobody's console; the names are the
game's default ("Player"), the friend code seeds zero.
Nothing here is edited by hand: run it again on the same capture and diff.
"""
import os
import re
import sys
import tempfile

RE = re.compile(r'^\s*(\d+\.\d+) c\d+ (\S+) -> (\S+)\s+SecureData canale (\d+) seq \d+ nodo (\d+) -> (\d+) mgmt (\d) \[(\d+)\] ([0-9a-f]*)\s*$')

HOST = '34:af:2c:e1:71:11'
SEED_CANDIDATES = [0x162524a3, 0x36050483, 0x96a5a423, 0xb6858403]  # every seed that fits A1, A2 and A3 of the capture (a search of all 2^32)
SEED_ZERO = bytes(4)
T_GROUP = '888.688'
T_FIRST, T_LAST = 901.154, 1035.002
# the order the seven came in (node, the capture's name) and how many Auth / AuthAck each exchanged
WANT_NODES = [2, 3, 4, 5, 6, 7, 8]


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
        tail = b[i:] + bytes(4 - (len(b) - i))
        s += be32(tail, 0)
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


def redact(b, start, end):
    """Zeros [start, end) and writes there, at start, the 32-bit sum of its big-endian words (the checksum stays the same)."""
    assert start % 4 == 0 and end % 4 == 0 and start >= 0x10 and end <= len(b)
    s = 0
    for i in range(start, end, 4):
        s += be32(b, i)
    out = bytearray(b)
    out[start:end] = bytes(end - start)
    out[start:start + 4] = (s & 0xFFFFFFFF).to_bytes(4, 'big')
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


def cap_frames(decoder, cap):
    """The frames of the capture as the kit's decoder reads them: (t, client, direction, type, channel, tx, dst, data)."""
    src = open(decoder, encoding='utf-8').read()
    cut = src.index('# ---------------------------------------------------------------- uscite')
    saved = sys.argv
    sys.argv = [decoder, cap, tempfile.mkdtemp()]
    g = {}
    try:
        exec(compile(src[:cut], decoder, 'exec'), g)
    finally:
        sys.argv = saved
    return g['frames']


def c_array(out, name, b, what):
    out.append('// %s' % what)
    out.append('static const std::uint8_t %s[%d] = {' % (name, len(b)))
    for i in range(0, len(b), 24):
        out.append('    ' + ', '.join('0x%02x' % x for x in b[i:i + 24]) + ',')
    out.append('};')


def main():
    dati, decoder, cap, dest = sys.argv[1:5]
    rows = []
    for line in open(dati, encoding='utf-8', errors='replace'):
        m = RE.match(line)
        if not m:
            continue
        t, tx, dst_mac, ch, src, dst, mgmt, n, hx = m.groups()
        b = bytes.fromhex(hx)
        if len(b) != int(n):
            raise SystemExit('line %s: %d bytes for [%s]' % (t, len(b), n))
        rows.append((t, int(ch), int(src), int(dst), int(mgmt), b, tx))
    ch1 = [r for r in rows if r[1] == 1 and r[6].startswith(HOST)]
    ch2 = [r for r in rows if r[1] == 2 and T_FIRST <= float(r[0]) <= T_LAST]

    def group_from(i):
        g = ch1[i:i + 5]
        assert [len(r[5]) for r in g] == [768, 1448, 1448, 1448, 1464], [len(r[5]) for r in g]
        assert [r[5][12] for r in g] == [0, 1, 2, 3, 4]
        return g

    first = next(k for k, r in enumerate(ch1) if r[0] == T_GROUP and len(r[5]) == 768)
    group = group_from(first)
    # every group of the good session (from the first to the start of the distribution) is the same bytes, 0.5 s apart
    starts = [k for k, r in enumerate(ch1) if len(r[5]) == 768 and float(T_GROUP) <= float(r[0]) <= T_LAST]
    for k in starts:
        assert [r[5] for r in group_from(k)] == [r[5] for r in group], ch1[k][0]
    steps = [float(ch1[b][0]) - float(ch1[a][0]) for a, b in zip(starts, starts[1:])]
    assert len(starts) > 250 and min(steps) > 0.40 and max(steps) < 0.62, (len(starts), min(steps), max(steps))
    assert len(ch2) == 65, len(ch2)

    packets = []
    for k, r in enumerate(group):
        b = r[5]
        if k == 0:
            red = [(0x40, 0x2F8)]
        elif k <= 3:
            red = [(0x10, len(b))]
        else:
            red = []
        g = b
        for s, e in red:
            g = redact(g, s, e)
        packets.append(('F%d' % k, r, g, red))
    for r in ch2:
        packets.append((dialog_id(r[5], r[2]), r, r[5], []))

    # the dialogue's shape: nodes 2..8 in the order they came in; from each client an AuthAck per Auth, then one of each kind
    kinds = {}
    for pid, r, g, red in packets[5:]:
        node = r[3] if r[2] == 1 else r[2]
        assert (r[2] == 1) != (r[3] == 1), 'host <-> client only'
        kinds.setdefault(node, []).append(pid)
    assert sorted(kinds) == WANT_NODES, sorted(kinds)
    assert [min(k for k, p in enumerate(packets[5:]) if (p[1][3] if p[1][2] == 1 else p[1][2]) == n) for n in WANT_NODES] == \
        sorted(min(k for k, p in enumerate(packets[5:]) if (p[1][3] if p[1][2] == 1 else p[1][2]) == n) for n in WANT_NODES), 'one at a time'
    for node, seq in kinds.items():
        tail = [p for p in seq if p not in ('A1', 'A2')]
        assert sorted(tail) == ['A3', 'A4', 'A5', 'A6', 'A7'], (node, seq)  # (no notice, no A9: the clients left 16 ms after the final)
        assert seq.count('A1') == seq.count('A2') >= 2, (node, seq)

    seeds = []
    for cand in SEED_CANDIDATES:
        seed = cand.to_bytes(4, 'big')
        if all(checksum(seed, p[1][5]) == be32(p[1][5], 8) for p in packets):
            seeds.append(seed)
    assert len(seeds) == 1, [s.hex() for s in seeds]
    seed = seeds[0]

    for pid, r, g, red in packets:
        b = r[5]
        assert r[4] == 0, 'mgmt'
        assert int.from_bytes(b[4:6], 'big') == len(b), pid
        assert b[1] == 2 and b[2:4] == b'\0\0' and b[6:8] == b'\x02\x00', pid
        assert checksum(seed, g) == be32(b, 8), pid + ' (redacted)'
        if red:
            assert g[:red[0][0]] == b[:red[0][0]] and g[red[-1][1]:] == b[red[-1][1]:]

    # the whole frames, from the capture itself
    frames = cap_frames(decoder, cap)

    def whole(typ, t_lo, t_hi, tx=HOST, pred=None):
        for t, cid, d, ftyp, ch, ftx, dst, data in frames:
            if d == 0 and ftyp == typ and ftx == tx and t_lo <= t <= t_hi and (pred is None or pred(data)):
                return t, ch, dst, bytes(data)
        raise SystemExit('no frame of type %d from %s in %.3f..%.3f' % (typ, tx, t_lo, t_hi))

    is_logoff = lambda d: d[:8] == bytes.fromhex('aaaa03000000888e') and len(d) == 8 + 664
    is_start = lambda d: d[:8] == bytes.fromhex('aaaa03000000888e') and len(d) == 8 + 48
    raws = [
        ('Beacon1', 'the host alone: its beacon, 1 node of 8 (wlancommid 00002810, no application data)', whole(0, 888.75, 888.80)),
        ('Beacon8', 'the same beacon with the eight nodes', whole(0, 1027.40, 1027.50)),
        ('NodeMap', 'the node map the host broadcast when the seventh client came in (7 entries: MAC, node id)', whole(5, 1027.39, 1027.40)),
        ('Logoff8', 'the EAPoL-Logoff of that admission, to everybody: node 8 assigned, 8 nodes of 8', whole(1, 1027.39, 1027.40, pred=is_logoff)),
        ('Start', 'the EAPoL-Start of that client (a Client: connection type 1)', whole(1, 1027.39, 1027.40, tx='34:af:2c:3c:ef:12', pred=is_start)),
    ]
    b1 = raws[0][2][3]
    tag = b1.index(bytes.fromhex('dd34001f3215'))
    assert b1[tag + 6:tag + 10] == bytes.fromhex('00002810') and b1[tag + 18] == 1 and b1[tag + 19] == 8 and b1[tag + 53] == 0, 'the beacon'
    b8 = raws[1][2][3]
    assert b8[tag + 18] == 8 and b8[tag + 19] == 8
    # (the group read from the capture is the group of dati.txt)
    cap_group = [bytes(data) for t, cid, d, typ, ch, tx, dst, data in frames
                 if d == 0 and typ == 1 and tx == HOST and 888.68 <= t <= 888.70 and data[14:16] == b'\x00\x01']
    assert [f[22:] for f in cap_group] == [r[5] for r in group], 'the capture and dati.txt'

    out = []
    out.append('// Generated by Tests/make_dlp_real_mk7_goldens.py from the capture of 2026-11-13 (eight Azahar emulators, Mario Kart 7, host MK7-1 = node 1, '
               'seven fake clients = nodes 2..8) - do not edit by hand.')
    out.append('// The SecureData payloads of the real Download Play, as they went through the room: the announcement group of t=%s '
               '(data channel 1, node 1 -> 0xFFFF; the %d groups up to t=%.3f are the same bytes, %.2f..%.2f s apart)'
               % (T_GROUP, len(starts), T_LAST, min(steps), max(steps)))
    out.append('// and EVERY packet of data channel 2 from t=%.3f to t=%.3f (ids A1..A7 of PROGETTO-DLP-VERO-1.1.3.md, section 2.3: no notice A8 and '
               'no A9 in this capture, the clients left 16 ms after the final).' % (T_FIRST, T_LAST))
    out.append('// The game\'s title and icon are NOT here: each range listed in `redacted` is zeros except its first word, the 32-bit sum of '
               'the big-endian words')
    out.append('// the capture had there, so that the checksum at +8..11 (seed %s) still holds. `checksum_zero` is the checksum of the '
               'same bytes with the seed 00 00 00 00.' % ' '.join('%02x' % x for x in seed))
    out.append('// Then five WHOLE frames (what Azahar\'s nwm sent: the body of the room\'s WifiPacket), kDlpRealMk7Raw.')
    out.append('// Needs Tests/dlp_real_luigi_golden.inc before it (struct DlpRealGolden), <cstddef> and <cstdint>.')
    out.append('')
    out.append('static const std::uint8_t kDlpRealMk7Seed[4] = {%s};' % ', '.join('0x%02x' % x for x in seed))
    out.append('static const std::uint64_t kDlpRealMk7Child = 0x%016xULL;  // frame 0, +0x10' % int.from_bytes(group[0][5][0x10:0x18], 'big'))
    out.append('static const unsigned kDlpRealMk7MaxNodes = %d;  // frame 0, +0x20' % group[0][5][0x20])
    out.append('static const std::uint8_t kDlpRealMk7Pass[9] = {%s};  // the final, +0x14' % ', '.join('0x%02x' % x for x in ch2[-1][5][0x14:0x1D]))
    out.append('')
    names = []
    for k, (pid, r, g, red) in enumerate(packets):
        name = 'kDlpRealMk7_%02d_%s' % (k, pid)
        names.append(name)
        c_array(out, name, g, '%s  t=%s  channel %d  node %d -> %d  %d bytes' % (pid, r[0], r[1], r[2], r[3], len(g)))
    out.append('')

    def entry(k):
        pid, r, g, red = packets[k]
        rs, re_ = red[0] if red else (0, 0)
        return ('    {"%s", "%s", %d, %d, %d, %d, %s, 0x%08x, 0x%08x, {0x%x, 0x%x}},'
                % (pid, r[0], r[1], r[2], r[3], len(g), names[k], be32(g, 8), checksum(SEED_ZERO, g), rs, re_))

    out.append('static const DlpRealGolden kDlpRealMk7Announce[5] = {')
    for k in range(5):
        out.append(entry(k))
    out.append('};')
    out.append('static const DlpRealGolden kDlpRealMk7Dialog[%d] = {' % len(ch2))
    for k in range(5, len(packets)):
        out.append(entry(k))
    out.append('};')
    out.append('')
    out.append('struct DlpRealMk7Frame {')
    out.append('    const char* id;')
    out.append('    const char* time;        // the capture\'s clock, seconds')
    out.append('    unsigned type;           // the room\'s WifiPacket type: 0 beacon, 1 data, 5 node map')
    out.append('    unsigned channel;        // the Wi-Fi channel')
    out.append('    bool broadcast;          // to ff:ff:ff:ff:ff:ff (false: to the host)')
    out.append('    std::size_t size;')
    out.append('    const std::uint8_t* bytes;')
    out.append('};')
    raw_names = []
    for rid, what, (t, ch, dst, data) in raws:
        name = 'kDlpRealMk7Raw_%s' % rid
        raw_names.append(name)
        c_array(out, name, data, '%s  t=%.3f  %s  %d bytes' % (rid, t, what, len(data)))
    out.append('static const DlpRealMk7Frame kDlpRealMk7Raw[%d] = {' % len(raws))
    for (rid, what, (t, ch, dst, data)), name in zip(raws, raw_names):
        typ = 0 if rid.startswith('Beacon') else 5 if rid == 'NodeMap' else 1
        out.append('    {"%s", "%.3f", %d, %d, %s, %d, %s},' % (rid, t, typ, ch, 'true' if dst == 'ff:ff:ff:ff:ff:ff' else 'false', len(data), name))
    out.append('};')
    open(dest, 'w', newline='\n').write('\n'.join(out) + '\n')
    print('wrote', dest, ':', len(packets), 'packets,', sum(len(p[2]) for p in packets), 'bytes,', len(raws), 'whole frames; seed',
          seed.hex(), '; every checksum checked;', len(starts), 'groups, step', '%.2f..%.2f s' % (min(steps), max(steps)))
    print('the dialogue per node:', {n: ' '.join(v) for n, v in kinds.items()})


main()
