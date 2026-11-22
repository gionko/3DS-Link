#!/usr/bin/env python3
"""Makes the golden data of the real Download Play of a game of D6 - KIRBY TRIPLE DELUXE (td), KIRBY PLANET ROBOBOT (pr) or MARIO TENNIS
OPEN (mto): the games whose players all have the game (dlp:SRVR on the host, the "fake client" dlp:FKCL on each guest, nothing downloaded)
and that moved, or will move, from the plugin's dialect (Lobby) to the real Download Play (RealDlp) ONE ROW AT A TIME, each with the capture
of two (or three) Azahar emulators that played it through a room (D6 of analysis/piano-1.1.4-2026-11-15/specifiche/5-L4.md, section 2).

    python Tests/make_dlp_real_lobby_games_goldens.py <td|pr|mto> <dati.txt> <frame.txt> Tests/<the game's golden>.inc   (numpy: no -I)

The capture and its decoding stay outside the repository (the kit of analysis/piano-1.1.4-2026-11-15/d6-cattura, folder registri/<game>):
`dati.txt` is its decoded data frames (decodificato/dati.txt), `frame.txt` every frame of it as one line (decodificato/frame.txt), read for
the beacons, the EAPoL-Start of the clients and their goodbyes. What the tests need from it is small and is written as a C++ include file
next to the tests (after Tests/dlp_real_luigi_golden.inc, whose struct DlpRealGolden it uses, and after the struct DlpRealLobbyGolden of
Tests/dlp_real_lobby_games_test.cpp, which describes the game to the test):

  - the five frames of the announcement group that the host repeated, unchanged, from its first group to the StartDistribution of the
    session (data channel 1, host -> everyone): 768 + 3 x 1448 + 1464 bytes (every 500 ms on Azahar's clock: 0.48..1.05 s on the room's in
    the capture of Planet Robobot, an emulator that ran late);
  - EVERY packet of data channel 2 of the last session that reached the final (the Auth and its answer as often as they went, then for each
    client StartDistribution, its answer WITHOUT content, the Finish, its answer, the notices and their answers, the final);
  - as numbers only (no whole frame is copied): the two networks of the host (the Download Play's while it accepts, the game's own after the
    final), the clients' friend code seed in the Download Play's network, and the times between the steps.

What is not ours to publish stays out of the file:
  - the title and the icon of the game, which the host puts in frame 0 (+0x40..+0x2F7) and in frames 1-3 (+0x10 to the end), are REPLACED by
    zeros, except the first word of each such range, which holds the 32-bit sum of the big-endian words that were there (the checksum is a
    function of that sum only: it still holds);
  - the PASSPHRASE of every final (+0x14, eight characters) is REPLACED by a stand-in of the shape the game gives it (eight hex digits in a
    row of 0123456789abcdef, "12345678" first; seven and a 'v') and the packet's checksum is made again with the capture's seed. The script
    REFUSES to write anything if a byte of a passphrase of the capture is left in its place (the 'v' of the shape aside), if four bytes of one
    in a row are anywhere in what it writes, or if the stand-in is not the shape the game gave;
  - the NAMES of the nodes and their ADDRESSES are in no byte of the file: no beacon, no EAPoL frame and no node map is copied.
The seed of the capture's checksums is searched here, among all 2^32 (numpy, about a minute), on the first Auth, its answer and the first
StartDistribution, and every packet is checked with it. Nothing is edited by hand: run it again on the same capture and diff.
The session written is the last one that reached a final (a session begins at an Auth after a final; it has one final per client that
played); a node that came in and left before the start (its Auth and its answer only, D6-3 of the procedure) stays in the dialogue and is
not counted as a client; a later session that never reached a final is left out.
Refusals (nothing written): no session reached the final (the game never got going between the emulators: the row stays Lobby); a client
asked for the content (+0x14 of its answer to StartDistribution = 0x0020: a download, not this mode); the announcement changed during the
lobby; a packet whose checksum no seed explains; a node that stopped half-way; something of the capture in the text.
    python3 Tests/make_dlp_real_lobby_games_goldens.py --selftest   (the suite: the session, the nodes and the text guard on made-up data)
"""
import re
import sys

RE_DATA = re.compile(r'^\s*(\d+\.\d+) c\d+ (\S+) -> (\S+)\s+SecureData canale (\d+) seq \d+ nodo (\d+) -> (\d+) mgmt (\d) \[(\d+)\] ([0-9a-f]*)\s*$')
RE_BEACON = re.compile(r'^\s*(\d+\.\d+) c\d+ Beacon ch(\d+) (\S+) -> TUTTI\s.*net wlancommid ([0-9a-f]{8}) id8 ([0-9a-f]{2}) info ([0-9a-f]+) app\[(\d+)\]')
RE_EAPOL = re.compile(r'^\s*(\d+\.\d+) c\d+ Data ch\d+ (\S+) -> (\S+)\s+EAPoL \[(\d+)\] ([0-9a-f]+)')
RE_DEAUTH = re.compile(r'^\s*(\d+\.\d+) c\d+ Deauth ch\d+ (\S+) -> (\S+)')

# The games of D6: the application's id (Europe, the copy that was captured), the child its host announces, the names the file and its
# symbols take. Every region of a game moves with its row (azl/title_rules.hpp): the capture is of one of them.
GAMES = {
    'td': {'name': 'Kirby Triple Deluxe', 'sym': 'KirbyTd', 'file': 'dlp_real_kirby_td_golden.inc', 'low': 0x0010C000},
    'pr': {'name': 'Kirby Planet Robobot', 'sym': 'Robobot', 'file': 'dlp_real_robobot_golden.inc', 'low': 0x00183600},
    'mto': {'name': 'Mario Tennis Open', 'sym': 'Tennis', 'file': 'dlp_real_tennis_golden.inc', 'low': 0x0007C800},
}
SEED_ZERO = bytes(4)
DLP_WLAN = 0x00002810       # the Download Play's own network (every game's)
PASS_AT, PASS_LEN = 0x14, 9  # the final: eight characters and a zero
CONTENT = 0x0020             # +0x14 of the answer to StartDistribution of a client that wants the child


def refuse(found, what):
    """Stops - also under python -O, where an assert is not run - and writes nothing."""
    if found:
        raise SystemExit('REFUSED, nothing written: ' + what)


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


def mix(h, key):
    # dlph_checksum: the step is (h >> s) | (h << s) with the SAME s both ways - not a rotation (design 2.5)
    sh = ((key >> 8) & 0xF) + 4
    for _ in range((key & 7) + 2):
        h = (((h >> sh) | (h << sh)) & 0xFFFFFFFF) ^ key
    return h


def checksum(seed, b):
    return mix(frame_sum(b), int.from_bytes(seed, 'big'))


def sealed(seed, b):
    out = bytearray(b)
    out[8:12] = checksum(seed, b).to_bytes(4, 'big')
    return bytes(out)


def search_seeds(packets):
    """Every seed of all 2^32 whose checksum of each packet is the one at its +8..11 (numpy: the shift and the number of rounds are bits of
    the seed itself, so the search runs once for each of their 128 values over the 2^25 seeds that have them)."""
    import numpy as np
    sums = [np.uint32(frame_sum(p)) for p in packets]
    want = [np.uint32(be32(p, 8)) for p in packets]
    free = np.arange(1 << 25, dtype=np.uint32)
    # the 25 free bits: 3..7 and 12..31 (bits 0..2 are the rounds, 8..11 the shift)
    spread = ((free & np.uint32(0x1F)) << np.uint32(3)) | ((free >> np.uint32(5)) << np.uint32(12))
    found = []
    chunk = 1 << 22
    for nib in range(16):
        sh = np.uint32(nib + 4)
        for cnt in range(8):
            base = np.uint32((nib << 8) | cnt)
            for at in range(0, len(spread), chunk):
                keys = spread[at:at + chunk] | base
                for s, w in zip(sums, want):
                    h = np.full(keys.shape, s, dtype=np.uint32)
                    for _ in range(cnt + 2):
                        h = ((h >> sh) | (h << sh)) ^ keys
                    keys = keys[h == w]  # (the next packet only for the seeds that fit this one)
                    if keys.size == 0:
                        break
                found.extend(int(k) for k in keys)
    return sorted(set(found))


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


def c_array(out, name, b, what):
    out.append('// %s' % what)
    out.append('static const std::uint8_t %s[%d] = {' % (name, len(b)))
    for i in range(0, len(b), 24):
        out.append('    ' + ', '.join('0x%02x' % x for x in b[i:i + 24]) + ',')
    out.append('};')


def ms(a, b):
    return int(round((b - a) * 1000))


def stand_in(p):
    """The stand-in of the shape the game gave its passphrase (eight characters and a zero) that has NO byte of it in its place: the first of
    "12345678", "23456789", ... (eight hex digits taken in turn from 0123456789abcdef, the first one starting at 1; for seven digits and a 'v',
    the seven and the 'v'). Each position of the passphrase rules out one of the sixteen, so one is always left. None for a shape this script
    does not know."""
    digits = b'0123456789abcdef'
    if re.fullmatch(rb'[0-9a-f]{8}\x00', p):
        n, tail = 8, b''
    elif re.fullmatch(rb'[0-9a-f]{7}v\x00', p):
        n, tail = 7, b'v'
    else:
        return None
    for k in range(1, 17):
        s = bytes(digits[(k + i) % 16] for i in range(n))
        if all(a != b for a, b in zip(s, p[:n])):
            return s + tail + b'\x00'
    return None


def sessions(ids):
    """The sessions of data channel 2 as [begin, end) of `ids` (A1..A9): one begins at the first Auth of the capture and at the first Auth
    after a final, and ends where the next one begins. A session has ONE final per client that played (the host sends each its own), so
    the finals of a session are not its borders: an Auth after them is (2026-11-16, review: two clients at the final ran past the list)."""
    starts, after_final = [], True
    for k, i in enumerate(ids):
        if i == 'A1' and after_final:
            starts.append(k)
            after_final = False
        elif i == 'A7':
            after_final = True
    return list(zip(starts, starts[1:] + [len(ids)]))


def last_session(ids):
    """[begin, end) of the last session that reached a final; None when none did. A session after it that never reached one is left out."""
    done = [s for s in sessions(ids) if 'A7' in ids[s[0]:s[1]]]
    return done[-1] if done else None


def nodes_of(per_node):
    """(the nodes that played, the nodes that left before the start) of a session, from each client node's ids. A node that played went
    Auth, its answer, StartDistribution, its answer, the Finish, its answer, the final (notices and their answers aside); a node that came
    in and left before the START had its Auth and its answer only (D6-3 of PROCEDURA.md: its packets stay in the dialogue, it is not a
    client of the session). Anything else, or a session where nobody played, is refused."""
    played, left = [], []
    for n, seq in sorted(per_node.items()):
        refuse(seq.count('A1') < 1 or seq.count('A2') < 1 or seq.index('A2') < seq.index('A1'), 'node %d: no Auth and its answer first' % n)
        tail = [p for p in seq if p not in ('A1', 'A2')]
        if not tail:
            left.append(n)
            continue
        core = [p for p in tail if p not in ('A8', 'A9')]
        refuse(core != ['A3', 'A4', 'A5', 'A6', 'A7'], 'node %d: the session is not StartDistribution, its answer, the Finish, its answer, '
               'the final: %s' % (n, ' '.join(seq)))
        played.append(n)
    refuse(not played, 'no client of the session played: every node left before the start')
    return played, left


def leaks_in_text(text, txs, macs, nicks, passphrases):
    """What of the capture the text of the golden file would carry, as kinds (never the value: the refusal must not print it either)."""
    low = text.lower()
    found = []
    if any(bad in low for bad in [tx[:17].lower() for tx in txs] + [mac.hex() for mac in macs] + [n.lower() for n in nicks]):
        found.append('an address or a name of a node of the capture')
    if any(p[:8].decode('latin-1').lower() in low or ', '.join('0x%02x' % x for x in p[i:i + 4]) in low for p in passphrases for i in range(5)):
        found.append('a passphrase of the capture')
    return found


def selftest():
    """The three functions above on made-up sessions and text (no capture, no numpy): python3 Tests/make_dlp_real_lobby_games_goldens.py --selftest"""
    bad = []

    def case(name, run, want):
        try:
            got = run()
        except SystemExit as e:
            got = 'refused (%s)' % e
        except Exception as e:  # (an IndexError of a walk that ran past the list: a failure of the case, not of the self-test)
            got = '%s: %s' % (type(e).__name__, e)
        print('  %s %s' % ('ok  ' if got == want else 'FAIL', name) + ('' if got == want else ': got %r, want %r' % (got, want)))
        if got != want:
            bad.append(name)

    one = ['A1', 'A2', 'A3', 'A4', 'A5', 'A6', 'A7', 'A8', 'A9']
    two = ['A1', 'A2', 'A1', 'A2', 'A3', 'A4', 'A3', 'A4', 'A5', 'A6', 'A5', 'A6', 'A7', 'A8', 'A7', 'A8', 'A9', 'A9']
    print('make_dlp_real_lobby_games_goldens.py --selftest')
    case('one client: the whole session', lambda: last_session(one), (0, 9))
    case('two clients at the final, one session (each its own final)', lambda: last_session(two), (0, 18))
    case('two clients at the final after an earlier session', lambda: last_session(one + two), (9, 27))
    case('the last of two sessions with a final', lambda: last_session(one + one[:7]), (9, 16))
    case('a session that never reached a final after the good one is left out', lambda: last_session(one + ['A1', 'A2', 'A3', 'A4']), (0, 9))
    case('an aborted try before the final is part of the session', lambda: last_session(['A1', 'A2', 'A1', 'A2'] + one[2:]), (0, 11))
    case('no final: none', lambda: last_session(['A1', 'A2', 'A3', 'A4', 'A5', 'A6']), None)
    case('nodes: one played', lambda: nodes_of({2: one}), ([2], []))
    case('nodes: D6-3 came in and left before the start (PROCEDURA.md)', lambda: nodes_of({2: one, 3: ['A1', 'A2']}), ([2], [3]))
    case('nodes: two played', lambda: nodes_of({2: one, 3: one[:7]}), ([2, 3], []))
    case('nodes: a node stopped after StartDistribution is refused', lambda: nodes_of({2: one, 3: ['A1', 'A2', 'A3', 'A4']}),
         'refused (REFUSED, nothing written: node 3: the session is not StartDistribution, its answer, the Finish, its answer, the final: A1 A2 A3 A4)')
    case('nodes: nobody played is refused', lambda: nodes_of({3: ['A1', 'A2']}),
         'refused (REFUSED, nothing written: no client of the session played: every node left before the start)')
    case('nodes: an answer before its Auth is refused', lambda: nodes_of({2: ['A2', 'A1'] + one[2:]}),
         'refused (REFUSED, nothing written: node 2: no Auth and its answer first)')
    pw = b'1a2b3c4d\x00'  # (made up: a passphrase of the shape the games give)
    mac = bytes([0x02, 0, 0, 0, 0, 0x01])
    tx = '02:00:00:00:00:01(Prova)'
    clean = '// Generated ...\nstatic const std::uint8_t kSeed[4] = {0x12, 0x34, 0x56, 0x78};\n'

    def leak(text):
        return leaks_in_text(text, [tx], [mac], ['Prova'], [pw])
    case('text: clean', lambda: leak(clean), [])
    case('text: the passphrase in a comment', lambda: leak(clean + '// stand-in for 1A2B3C4D\n'), ['a passphrase of the capture'])
    case('text: four bytes of the passphrase as numbers', lambda: leak(clean + '{0x32, 0x62, 0x33, 0x63}'), ['a passphrase of the capture'])
    case('text: an address as printed', lambda: leak(clean + '// from 02:00:00:00:00:01'), ['an address or a name of a node of the capture'])
    case('text: an address as hex', lambda: leak(clean + '// 020000000001'), ['an address or a name of a node of the capture'])
    case('text: a name', lambda: leak(clean + '// prova'), ['an address or a name of a node of the capture'])
    print('make_dlp_real_lobby_games_goldens.py --selftest: %d case(s) failed%s' % (len(bad), (': ' + ', '.join(bad)) if bad else ''))
    return 1 if bad else 0


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in GAMES:
        raise SystemExit('usage: make_dlp_real_lobby_games_goldens.py <%s> <dati.txt> <frame.txt> <out.inc>' % '|'.join(GAMES))
    game = GAMES[sys.argv[1]]
    dati, frame_txt, dest = sys.argv[2:5]
    child = 0x0004000100000000 | game['low']
    sym = 'kDlpReal' + game['sym']
    rows = []
    for line in open(dati, encoding='utf-8', errors='replace'):
        m = RE_DATA.match(line)
        if not m:
            continue
        t, tx, dst_mac, ch, src, dst, mgmt, n, hx = m.groups()
        b = bytes.fromhex(hx)
        if len(b) != int(n):
            raise SystemExit('line %s: %d bytes for [%s]' % (t, len(b), n))
        rows.append((t, int(ch), int(src), int(dst), int(mgmt), b, tx))
    ch2 = [r for r in rows if r[1] == 2]
    ids = [dialog_id(r[5], r[2]) for r in ch2]
    # the sessions: each begins at an Auth that follows a final (or the start); the last one that has a final is the one written
    session = last_session(ids)
    refuse(session is None, 'no session of the capture reached the final (%d packets on channel 2): the game did not get going between the '
           'emulators, its row stays Lobby' % len(ch2))
    begin, end = session
    good = ch2[begin:end]
    good_ids = ids[begin:end]
    # (a later session that never reached a final bounds what follows: the last packet of this one, the first of that one)
    t_last = float(good[-1][0]) if end < len(ch2) else float('inf')
    t_next = float(ch2[end][0]) if end < len(ch2) else float('inf')
    refuse(any(i == 'A1' for i in good_ids[good_ids.index('A3'):]), 'an Auth after the StartDistribution of the last session')
    host_tx = good[0][6]
    refuse(any((r[6] == host_tx) != (r[2] == 1) for r in good), 'a packet of the host that is not node 1, or of a client that is')
    nodes = sorted({r[3] if r[2] == 1 else r[2] for r in good})
    refuse(not nodes or any(n < 2 or n > 4 for n in nodes), 'the clients of the session are not nodes 2..4: %s' % nodes)
    per_node = {n: [i for r, i in zip(good, good_ids) if (r[3] if r[2] == 1 else r[2]) == n] for n in nodes}
    clients, left = nodes_of(per_node)
    for r, i in zip(good, good_ids):
        if i == 'A4':
            refuse(len(r[5]) != 0x18 or int.from_bytes(r[5][0x14:0x16], 'big') == CONTENT,
                   'node %d asked for the content (a download): not the fake client of this mode' % r[2])

    # the announcement: every group of the lobby, from its first to the StartDistribution, is the same five frames
    ch1 = [r for r in rows if r[1] == 1 and r[2] == 1 and r[6] == host_tx and len(r[5]) in (768, 1448, 1464) and r[5][0] == 1]
    t_a3 = float(good[good_ids.index('A3')][0])
    t_a1 = float(good[0][0])
    starts = [k for k, r in enumerate(ch1) if len(r[5]) == 768 and float(r[0]) < t_a3]
    refuse(len(starts) < 4, 'fewer than four groups of the announcement before StartDistribution')

    def group_from(i):
        g = ch1[i:i + 5]
        refuse([len(r[5]) for r in g] != [768, 1448, 1448, 1448, 1464] or [r[5][12] for r in g] != [0, 1, 2, 3, 4],
               'a group of the announcement that is not frames 0..4 (t=%s)' % ch1[i][0])
        return g

    # (the lobby of the good session: the groups after the last final before it; a group can be cut by the end of a session)
    t_begin = float(ch2[begin - 1][0]) if begin else 0.0
    lobby = [k for k in starts if float(ch1[k][0]) > t_begin and k + 5 <= len(ch1) and float(ch1[k + 4][0]) < t_a3]
    group = group_from(lobby[0])
    for k in lobby:
        refuse([r[5] for r in group_from(k)] != [r[5] for r in group], 'the announcement changed during the lobby (t=%s)' % ch1[k][0])
    steps = [float(ch1[b][0]) - float(ch1[a][0]) for a, b in zip(lobby, lobby[1:])]
    refuse(not steps or min(steps) < 0.40 or max(steps) > 1.5, "the groups are not about 0.5 s apart: %.3f..%.3f s" % (min(steps or [0]), max(steps or [0])))
    refuse(any(t_a3 < float(r[0]) <= t_last for r in ch1), 'an announcement after StartDistribution')
    f0 = group[0][5]
    refuse(int.from_bytes(f0[0x10:0x18], 'big') != child, 'frame 0 announces %016x, not the child %016x' % (int.from_bytes(f0[0x10:0x18], 'big'), child))
    refuse(f0[0x20] != 4, 'frame 0 says %d nodes, not 4' % f0[0x20])

    # the seed of the capture's checksums
    first = {}
    for r, i in zip(good, good_ids):
        first.setdefault(i, r[5])
    print('searching the seed of the checksums among 2^32 ...', flush=True)
    seeds = search_seeds([first['A1'], first['A2'], first['A3']])
    seeds = [s for s in seeds if all(checksum(s.to_bytes(4, 'big'), r[5]) == be32(r[5], 8) for r in group + good)]
    refuse(len(seeds) != 1, '%d seeds fit every packet of the session' % len(seeds))
    seed = seeds[0].to_bytes(4, 'big')

    packets = []
    for k, r in enumerate(group):
        b = r[5]
        red = [(0x40, 0x2F8)] if k == 0 else [(0x10, len(b))] if k <= 3 else []
        g = b
        for s, e in red:
            g = redact(g, s, e)
        packets.append(('F%d' % k, r, g, red))
    passphrases = []
    stand = None
    for r, pid in zip(good, good_ids):
        g = r[5]
        if pid == 'A7':
            p = g[PASS_AT:PASS_AT + PASS_LEN]
            s = stand_in(p)
            refuse(s is None, 'a passphrase of a shape this script does not know')
            refuse(stand is not None and s != stand, 'two shapes of passphrase in one session')
            refuse(any(a == b for a, b in zip(p[:8], s[:8]) if b != ord('v')), 'the stand-in keeps a byte of the passphrase in its place')
            stand = s
            passphrases.append(p)
            g = sealed(seed, g[:PASS_AT] + s + g[PASS_AT + PASS_LEN:])
        packets.append((pid, r, g, []))
    for pid, r, g, red in packets:
        b = r[5]
        refuse(r[4] != 0 or int.from_bytes(b[4:6], 'big') != len(b) or b[1] != 2 or b[2:4] != b'\0\0' or b[6:8] != b'\x02\x00',
               pid + ': not the header of the real Download Play')
        refuse(checksum(seed, g) != be32(g, 8), pid + ': the checksum of what is written')
        if red:
            refuse(g[:red[0][0]] != b[:red[0][0]] or g[red[-1][1]:] != b[red[-1][1]:] or be32(g, 8) != be32(b, 8), pid + ': the redaction')
    # nothing of the capture's passphrases is left: not in its place, not four bytes of it in a row anywhere
    for p in passphrases:
        for pid, r, g, red in packets:
            refuse(any(p[i:i + 4] in g for i in range(0, 5)), 'four bytes of a passphrase of the capture in ' + pid)
            if pid == 'A7':
                refuse(any(g[PASS_AT + i] == p[i] and p[i] != ord('v') for i in range(8)), 'a byte of a passphrase of the capture in its place in the final')

    # the frames that are not copied: what they say, as numbers
    beacons, starts_eapol, deauths = [], [], []
    for line in open(frame_txt, encoding='utf-8', errors='replace'):
        m = RE_BEACON.match(line)
        if m:
            t, ch, tx, wlan, id8, info, app = m.groups()
            i = bytes.fromhex(info)
            beacons.append({'t': float(t), 'tx': tx, 'wlan': int(wlan, 16), 'max': i[13], 'total': i[12], 'app': int(app)})
            continue
        m = RE_EAPOL.match(line)
        if m and int(m.group(4)) == 48:
            starts_eapol.append((float(m.group(1)), m.group(2), m.group(3), bytes.fromhex(m.group(5))))
            continue
        m = RE_DEAUTH.match(line)
        if m:
            deauths.append((float(m.group(1)), m.group(2), m.group(3)))
    t_final = float(good[good_ids.index('A7')][0])
    dlp = [b for b in beacons if b['wlan'] == DLP_WLAN and b['tx'] == host_tx and b['t'] <= t_final]
    net = [b for b in beacons if b['wlan'] != DLP_WLAN and b['tx'] == host_tx and b['t'] > t_final]
    refuse(not dlp or any(b['max'] != 4 or b['app'] != 0 for b in dlp), 'the Download Play\'s network of the host: 4 nodes, no application data')
    refuse(not net, 'no beacon of the game\'s own network after the final')
    client_txs = sorted({r[6] for r in good if r[2] != 1})
    joins = []
    for tx in client_txs:
        mine = [s for s in starts_eapol if s[1] == tx and s[2] == host_tx and t_begin < s[0] <= t_final]
        refuse(not mine or mine[-1][3][4] != 1, 'the client %d did not come in as a client' % len(joins))
        refuse(any(s[3][8:16] != bytes(8) for s in mine), 'a friend code seed that is not zero in the Download Play\'s network')
        joins.append(mine[-1][0])
    t_join = min(joins)
    byes = [d[0] for d in deauths if d[1] in client_txs and t_final < d[0] < t_next]
    auto = ms(t_join, t_a1) < 1000
    t_bye = min(byes) if byes else None

    out = []
    out.append('// Generated by Tests/make_dlp_real_lobby_games_goldens.py from the capture of %s (D6: %d Azahar emulators through a room, the host = '
               'node 1, %d fake client(s) = node(s) %s) - do not edit by hand.' % (game['name'], 1 + len(nodes), len(clients), ', '.join(map(str, clients))))
    if left:
        out.append('// Node(s) %s came in and left before the start: their Auth and its answer are in the dialogue; they are not clients of the session.'
                   % ', '.join(map(str, left)))
    out.append('// The SecureData payloads of the real Download Play, as they went through the room: the announcement group of t=%s (data channel 1, '
               'node 1 -> 0xFFFF; the %d groups of the lobby are the same bytes, %.2f..%.2f s apart)' % (group[0][0], len(lobby), min(steps), max(steps)))
    out.append('// and EVERY packet of data channel 2 of the last session that reached the final, t=%s to t=%s (ids A1..A9 of PROGETTO-DLP-VERO-1.1.3.md, '
               'section 2.3), in the capture\'s order.' % (good[0][0], good[-1][0]))
    out.append('// What is NOT here: the game\'s title and icon (each range listed in `redacted` is zeros except its first word, the 32-bit sum of the big-endian '
               'words the capture had there, so that')
    out.append('// the checksum at +8..11 still holds); the PASSPHRASE of the final, replaced by the stand-in "%s" with the checksum made again; the names and '
               'the addresses of the nodes, which are in no byte of this file' % stand[:8].decode())
    out.append('// (no beacon, no EAPoL frame, no node map is copied: what they say is the numbers below). `checksum_zero` is the checksum of the same bytes with '
               'the seed 00 00 00 00.')
    out.append('// Needs Tests/dlp_real_luigi_golden.inc (struct DlpRealGolden) and the struct DlpRealLobbyGolden of Tests/dlp_real_lobby_games_test.cpp before it.')
    out.append('')
    out.append('static const std::uint8_t %sSeed[4] = {%s};' % (sym, ', '.join('0x%02x' % x for x in seed)))
    out.append('static const std::uint8_t %sPass[9] = {%s};  // the final, +0x14: the STAND-IN "%s" (the shape the game gives it)'
               % (sym, ', '.join('0x%02x' % x for x in stand), stand[:8].decode()))
    names = []
    for k, (pid, r, g, red) in enumerate(packets):
        name = '%s_%02d_%s' % (sym, k, pid)
        names.append(name)
        c_array(out, name, g, '%s  t=%s  channel %d  node %d -> %d  %d bytes' % (pid, r[0], r[1], r[2], r[3], len(g)))
    out.append('')

    def entry(k):
        pid, r, g, red = packets[k]
        rs, re_ = red[0] if red else (0, 0)
        return ('    {"%s", "%s", %d, %d, %d, %d, %s, 0x%08x, 0x%08x, {0x%x, 0x%x}},'
                % (pid, r[0], r[1], r[2], r[3], len(g), names[k], be32(g, 8), checksum(SEED_ZERO, g), rs, re_))

    out.append('static const DlpRealGolden %sAnnounce[5] = {' % sym)
    for k in range(5):
        out.append(entry(k))
    out.append('};')
    out.append('static const DlpRealGolden %sDialog[%d] = {' % (sym, len(good)))
    for k in range(5, len(packets)):
        out.append(entry(k))
    out.append('};')
    out.append('// The game as the capture shows it (struct DlpRealLobbyGolden of Tests/dlp_real_lobby_games_test.cpp).')
    out.append('static const DlpRealLobbyGolden %sGame = {' % sym)
    out.append('    "%s", 0x%08xu, 0x%016xULL, %d,  // the name, the application\'s low word, the child of frame 0 (+0x10), its nodes (+0x20)' % (game['name'], game['low'], child, f0[0x20]))
    out.append('    0x%08xu, 0x%08xu,  // frame 0, +0x38 / +0x3C: the child\'s sizes (zero in our announcement)' % (be32(f0, 0x38), be32(f0, 0x3C)))
    out.append('    %sSeed, %sPass, %sAnnounce, %sDialog, %d,' % (sym, sym, sym, sym, len(good)))
    out.append('    %d, %s,  // the clients of the session; the acceptance automatic (the Auth %d ms after the first came in)' % (len(clients), 'true' if auto else 'false', ms(t_join, t_a1)))
    out.append('    0x%08xu, %d,  // the game\'s own network after the final: its wlancommid (the same transmitter), %d ms after the final; its nodes'
               % (net[0]['wlan'], net[0]['max'], ms(t_final, net[0]['t'])))
    out.append('    %d,  // ms from the final to the first client\'s goodbye (Deauthentication; -1: none in the capture)' % (ms(t_final, t_bye) if t_bye is not None else -1))
    out.append('};')
    text = '\n'.join(out) + '\n'
    # (the last look, on the BYTES that are written - the text holds them as "0xaa, 0xbb", where a search for "aa:bb" finds nothing: no address
    # of a node of the capture, straight or reversed, and no name of a node, as UTF-8 or as the UTF-16 the wire carries, in either byte order)
    txs = [host_tx] + client_txs
    nicks = [tx[18:].rstrip(')') for tx in txs]   # ("aa:bb:cc:dd:ee:ff(name)": the name the emulator had in the room)
    macs = [bytes.fromhex(tx[:17].replace(':', '')) for tx in txs]
    refuse(not all(nicks) or any(len(mac) != 6 for mac in macs) or len(set(macs)) != len(macs), 'the nodes of the capture are not read')
    refuse(any(mac in p[2] or mac[::-1] in p[2] for mac in macs for p in packets), 'an address of a node of the capture in a packet')
    refuse(any(n.encode(enc) in p[2] for n in nicks for enc in ('utf-8', 'utf-16-be', 'utf-16-le') for p in packets), 'a name of a node of the capture in a packet')
    # (... and on the text around the bytes, the comments and the numbers: no address as it is printed, no name, no passphrase)
    leaks = leaks_in_text(text, txs, macs, nicks, passphrases)
    refuse(leaks, ' and '.join(leaks) + ' in the text')
    open(dest, 'w', newline='\n').write(text)
    print('wrote', dest, ':', len(packets), 'packets,', sum(len(p[2]) for p in packets), 'bytes; every checksum checked;', len(lobby), 'groups, step',
          '%.2f..%.2f s' % (min(steps), max(steps)))
    print('the dialogue per node:', {n: ' '.join(v) for n, v in per_node.items()}, '; left before the start:', left or 'none', '; acceptance',
          'automatic' if auto else 'by hand', '; final ->', ms(t_final, net[0]['t']), 'ms the game\'s network')


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        sys.exit(selftest())
    main()
