#!/usr/bin/env python3
"""Makes the golden data of the real Download Play of MARIO PARTY: ISLAND TOUR (the RealDlp mode, the game as the HOST) from the capture
of two Azahar emulators that played it through a room (2026-11-13, "prova 2": the first hosts the game, the second is the system's
Download Play application with the child 00040001000F8200 already installed, so nothing is downloaded; two sessions, the second one is
the good one).

    python -I Tests/make_dlp_real_island_goldens.py <dati.txt> <frame.txt> Tests/dlp_real_island_golden.inc

The capture (50 MB) and its decoding stay outside the repository (the kit of analysis/island-tour-2026-11-13, folder
kit-azahar/registri/2-guest-con-figlio): `dati.txt` is its decoded data frames (decodificato/dati.txt), `frame.txt` every frame of it as
one line (decodificato/frame.txt), read for the beacons, the EAPoL-Start of the client and its goodbyes. What the tests need from it is
small and is written as a C++ include file next to the tests (after Tests/dlp_real_luigi_golden.inc, whose struct DlpRealGolden it uses):

  - the five frames of the announcement group of t=285.079 (data channel 1, host -> everyone): 768 + 3 x 1448 + 1464 bytes;
  - the nine packets of data channel 2 of the good session, t=402.106 .. 402.438: the Auth and StartDistribution in the same instant
    (the game accepts and starts in one step, after the client waited 105 s without a packet), their answers, the Finish and its answer,
    the final with the passphrase, ONE notice and its answer;
  - as numbers only (no whole frame is copied): the ids, the attributes and the nodes of the Download Play's network and of the game's own
    network that the host opens after the final, that both have the same transmitter, and the times between the steps.

What is not ours to publish stays out of the file:
  - the title and the icon of the game, which the host puts in frame 0 (+0x40..+0x2F7) and in frames 1-3 (+0x10 to the end), are REPLACED
    by zeros, except the first word of each such range, which holds the 32-bit sum of the big-endian words that were there (the checksum
    is a function of that sum only: it still holds);
  - the PASSPHRASE of the final (+0x14, eight characters) is REPLACED by the stand-in "1234567v" (the shape the game gives it: seven hex
    digits and a 'v') and the packet's checksum is made again with the capture's seed;
  - the NAMES of the two nodes and their ADDRESSES are in no byte of this file: no beacon, no EAPoL frame and no node map is copied.
This script finds nothing by itself about the seed of the capture's checksums: the candidate is the one seed of all 2^32 that fits the
Auth, its answer and StartDistribution (searched once, analysis/island-tour-2026-11-13/plugin/s1/island-h1/strumenti/cerca_seme.c), and
every packet is checked with it here. Nothing is edited by hand: run it again on the same capture and diff.
"""
import re
import sys

RE_DATA = re.compile(r'^\s*(\d+\.\d+) c\d+ (\S+) -> (\S+)\s+SecureData canale (\d+) seq \d+ nodo (\d+) -> (\d+) mgmt (\d) \[(\d+)\] ([0-9a-f]*)\s*$')
RE_BEACON = re.compile(r'^\s*(\d+\.\d+) c\d+ Beacon ch(\d+) (\S+) -> TUTTI\s.*net wlancommid ([0-9a-f]{8}) id8 ([0-9a-f]{2}) info ([0-9a-f]+) app\[(\d+)\]')
RE_EAPOL = re.compile(r'^\s*(\d+\.\d+) c\d+ Data ch\d+ (\S+) -> (\S+)\s+EAPoL \[(\d+)\] ([0-9a-f]+)')
RE_DEAUTH = re.compile(r'^\s*(\d+\.\d+) c\d+ Deauth ch\d+ (\S+) -> (\S+)')

SEED_CANDIDATES = [0xb8f46bff]  # every seed that fits A1, A2 and A3 of the capture (a search of all 2^32: one)
SEED_ZERO = bytes(4)
T_GROUP = '285.079'
T_GOOD = (402.106, 402.438)   # the dialogue of the good session
T_FIRST = (79.780, 80.126)    # the dialogue of the first session (the one that fell later, in the game: the same packets)
T_LOBBY = (259.5, 402.2)      # the host of the good session accepts: from its first group to StartDistribution
STAND_IN = b'1234567v\x00'    # what replaces the passphrase of the final (+0x14, nine bytes)
WANT_IDS = ['A1', 'A3', 'A2', 'A4', 'A5', 'A6', 'A7', 'A8', 'A9']


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


def sealed(seed, b):
    out = bytearray(b)
    out[8:12] = checksum(seed, b).to_bytes(4, 'big')
    return bytes(out)


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


def refuse(found, what):
    """Stops - also under python -O, where an assert is not run - when something of the capture that must stay out is about to be written."""
    if found:
        raise SystemExit('REFUSED, nothing written: ' + what)


def ms(a, b):
    return int(round((b - a) * 1000))


def main():
    dati, frame_txt, dest = sys.argv[1:4]
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
    ch1 = [r for r in rows if r[1] == 1 and r[2] == 1 and len(r[5]) in (768, 1448, 1464) and r[5][0] == 1 and float(r[0]) < T_GOOD[1]]
    ch2 = [r for r in rows if r[1] == 2]
    good = [r for r in ch2 if T_GOOD[0] <= float(r[0]) <= T_GOOD[1]]
    first = [r for r in ch2 if T_FIRST[0] <= float(r[0]) <= T_FIRST[1]]
    assert len(ch2) == 18 and len(good) == 9 and len(first) == 9, (len(ch2), len(good), len(first))
    host_tx = good[0][6]

    def group_from(i):
        g = ch1[i:i + 5]
        assert [len(r[5]) for r in g] == [768, 1448, 1448, 1448, 1464], [len(r[5]) for r in g]
        assert [r[5][12] for r in g] == [0, 1, 2, 3, 4]
        return g

    at = next(k for k, r in enumerate(ch1) if r[0] == T_GROUP and len(r[5]) == 768)
    group = group_from(at)
    # every group of the good session's lobby is the same bytes, 0.5 s apart, and the last one is before StartDistribution
    starts = [k for k, r in enumerate(ch1) if len(r[5]) == 768 and T_LOBBY[0] <= float(r[0]) <= T_LOBBY[1]]
    for k in starts:
        assert [r[5] for r in group_from(k)] == [r[5] for r in group], ch1[k][0]
    steps = [float(ch1[b][0]) - float(ch1[a][0]) for a, b in zip(starts, starts[1:])]
    assert len(starts) > 250 and min(steps) > 0.40 and max(steps) < 0.62, (len(starts), min(steps), max(steps))
    t_last_group = float(ch1[starts[-1]][0])
    assert t_last_group < T_GOOD[0] and T_GOOD[0] - t_last_group < 0.6
    assert not [r for r in ch1 if T_GOOD[0] <= float(r[0])], 'no announcement after StartDistribution'

    seeds = [c.to_bytes(4, 'big') for c in SEED_CANDIDATES
             if all(checksum(c.to_bytes(4, 'big'), r[5]) == be32(r[5], 8) for r in group + good + first)]
    assert len(seeds) == 1, [s.hex() for s in seeds]
    seed = seeds[0]

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
    passphrases = []
    for r in good:
        pid = dialog_id(r[5], r[2])
        g = r[5]
        if pid == 'A7':
            p = g[0x14:0x1D]
            # the shape the game gives it: seven hex digits, a 'v', a zero (and 09 after it)
            assert re.fullmatch(rb'[0-9a-f]{7}v\x00', p) and g[0x1D] == 9 and g[0x1E:0x20] == b'\0\0', 'the passphrase'
            passphrases.append(p)
            g = sealed(seed, g[:0x14] + STAND_IN + g[0x1D:])
        packets.append((pid, r, g, []))
    assert [p[0] for p in packets[5:]] == WANT_IDS, [p[0] for p in packets[5:]]
    for pid, r, g, red in packets[5:]:
        assert (r[2], r[3]) == ((1, 2) if pid in ('A1', 'A3', 'A5', 'A7', 'A8') else (2, 1)), pid

    # the first session is the same dialogue: only the passphrase and the two bytes Azahar's client never initialises (A6, +0x18..0x19) differ
    assert [dialog_id(r[5], r[2]) for r in first] == ['A1', 'A3', 'A2', 'A4', 'A5', 'A6', 'A7', 'A8', 'A9']
    by_id = lambda rs: {dialog_id(r[5], r[2]): r[5] for r in rs}
    f, g2 = by_id(first), by_id(good)
    for pid in WANT_IDS:
        a, b = bytearray(f[pid]), bytearray(g2[pid])
        a[8:12] = b[8:12] = bytes(4)
        if pid == 'A7':
            assert re.fullmatch(rb'[0-9a-f]{7}v\x00', bytes(a[0x14:0x1D])) and a[0x14:0x1D] != b[0x14:0x1D], 'a new passphrase each session'
            a[0x14:0x1D] = b[0x14:0x1D] = bytes(9)
        if pid == 'A6':
            a[0x18:0x1A] = b[0x18:0x1A] = bytes(2)
        assert a == b, pid + ' of the two sessions'

    for pid, r, g, red in packets:
        b = r[5]
        assert r[4] == 0, 'mgmt'
        assert int.from_bytes(b[4:6], 'big') == len(b), pid
        assert b[1] == 2 and b[2:4] == b'\0\0' and b[6:8] == b'\x02\x00', pid
        assert checksum(seed, b) == be32(b, 8), pid
        assert checksum(seed, g) == be32(g, 8), pid + ' (as written)'
        if red:
            assert g[:red[0][0]] == b[:red[0][0]] and g[red[-1][1]:] == b[red[-1][1]:]
            assert checksum(seed, g) == be32(b, 8), pid + ' (redacted: the capture\'s checksum still holds)'
    # nothing of the capture's passphrases is left in what is written
    refuse(any(p[:8] in g for pid, r, g, red in packets for p in passphrases), 'a passphrase of the capture in a packet')

    # the frames that are not copied: what they say, as numbers
    beacons, starts_eapol, deauths = [], [], []
    for line in open(frame_txt, encoding='utf-8', errors='replace'):
        m = RE_BEACON.match(line)
        if m:
            t, ch, tx, wlan, id8, info, app = m.groups()
            i = bytes.fromhex(info)
            assert i[:4].hex() == wlan and '%02x' % i[4] == id8
            beacons.append({'t': float(t), 'ch': int(ch), 'tx': tx, 'wlan': int(wlan, 16), 'id': i[4], 'attr': int.from_bytes(i[6:8], 'big'),
                            'net': be32(i, 8), 'total': i[12], 'max': i[13], 'app': int(app)})
            continue
        m = RE_EAPOL.match(line)
        if m and int(m.group(4)) == 48:
            b = bytes.fromhex(m.group(5))
            starts_eapol.append((float(m.group(1)), m.group(2), m.group(3), b))
            continue
        m = RE_DEAUTH.match(line)
        if m:
            deauths.append((float(m.group(1)), m.group(2), m.group(3)))
    dlp = [b for b in beacons if b['wlan'] == 0x00002810 and b['t'] > 250]
    game = [b for b in beacons if b['wlan'] != 0x00002810 and b['t'] > 250]
    assert [b['total'] for b in dlp] == [1, 2] and [b['total'] for b in game] == [1, 2], (dlp, game)
    for b in dlp:
        assert (b['id'], b['attr'], b['max'], b['app'], b['tx']) == (0, 0, 4, 0, host_tx), b
    for b in game:
        assert (b['wlan'], b['id'], b['attr'], b['net'], b['max'], b['app'], b['tx']) == (0x000F8210, 1, 0x8000, 0, 4, 0, host_tx), b
    assert dlp[0]['ch'] == game[0]['ch']
    # the client: a spectator for each look at the announcement, then ONE join as a client, with the friend code seed zero; in the game's own
    # network the child comes in as a client with a seed of its own (not written here)
    client_tx = good[2][6]
    mine = [s for s in starts_eapol if s[1] == client_tx and s[0] > 285]
    assert [s[3][4] for s in mine] == [2, 1, 1], [s[3][:8].hex() for s in mine]   # spectator, client (the lobby), client (the game's network)
    for s in mine:
        assert s[3][:4] == bytes.fromhex('02010001') and s[2] == host_tx
    t_look, t_join, t_child = (s[0] for s in mine)
    assert mine[0][3][8:16] == bytes(8) and mine[1][3][8:16] == bytes(8), 'the seed in the Download Play\'s network is zero'
    assert mine[2][3][8:16] != bytes(8), 'the seed in the game\'s network is the emulator\'s own'
    assert not [r for r in ch2 if t_join <= float(r[0]) < T_GOOD[0]], 'no packet of channel 2 while the client waits to be accepted'
    t_a1 = float(good[0][0])
    t_final = float(next(r[0] for r in good if dialog_id(r[5], r[2]) == 'A7'))
    t_bye = min(d[0] for d in deauths if d[1] == client_tx and d[0] > t_final)
    assert good[0][0] == good[1][0], 'the Auth and StartDistribution in the same instant'
    assert game[0]['t'] > t_final and t_bye > t_final and t_child > game[0]['t']

    out = []
    out.append('// Generated by Tests/make_dlp_real_island_goldens.py from the capture of 2026-11-13 (two Azahar emulators, Mario Party: Island Tour, '
               'the host = node 1, the Download Play application with the child installed = node 2) - do not edit by hand.')
    out.append('// The SecureData payloads of the real Download Play, as they went through the room: the announcement group of t=%s '
               '(data channel 1, node 1 -> 0xFFFF; the %d groups from t=%.3f to t=%.3f are the same bytes, %.2f..%.2f s apart, the last one before StartDistribution)'
               % (T_GROUP, len(starts), float(ch1[starts[0]][0]), t_last_group, min(steps), max(steps)))
    out.append('// and the NINE packets of data channel 2 of the good session, t=%.3f to t=%.3f (ids A1..A9 of PROGETTO-DLP-VERO-1.1.3.md, section 2.3), '
               'in the capture\'s order: the game accepts by hand and' % T_GOOD)
    out.append('// starts in the same step, so the Auth (A1) and StartDistribution (A3) leave in the same instant, %d ms after the client came in (no packet of channel 2 '
               'meanwhile), and the client\'s answer to the Auth comes when' % ms(t_join, t_a1))
    out.append('// Azahar\'s host no longer waits for it: every later packet of the host keeps the sequence it had and the two bytes of an id nobody set (%s). The dialogue of '
               'the first session (t=%.3f..%.3f) is the same'
               % (' '.join('%02x' % x for x in g2['A3'][0x0E:0x10]), T_FIRST[0], T_FIRST[1]))
    out.append('// packets but for its passphrase and +0x18..0x19 of A6 (two bytes Azahar\'s client never initialises).')
    out.append('// What is NOT here: the game\'s title and icon (each range listed in `redacted` is zeros except its first word, the 32-bit sum of the big-endian '
               'words the capture had there, so that the')
    out.append('// checksum at +8..11, seed %s, still holds); the PASSPHRASE of the final, replaced by the stand-in "%s" with the checksum made again; the names and the '
               'addresses of the two nodes, which are in no byte of this file'
               % (' '.join('%02x' % x for x in seed), STAND_IN[:8].decode()))
    out.append('// (no beacon, no EAPoL frame, no node map is copied: what they say is the numbers below). `checksum_zero` is the checksum of the same bytes with the seed 00 00 00 00.')
    out.append('// Needs Tests/dlp_real_luigi_golden.inc before it (struct DlpRealGolden), <cstddef> and <cstdint>.')
    out.append('')
    out.append('static const std::uint8_t kDlpRealIslandSeed[4] = {%s};' % ', '.join('0x%02x' % x for x in seed))
    out.append('static const std::uint64_t kDlpRealIslandChild = 0x%016xULL;  // frame 0, +0x10' % int.from_bytes(group[0][5][0x10:0x18], 'big'))
    out.append('static const unsigned kDlpRealIslandMaxNodes = %d;  // frame 0, +0x20' % group[0][5][0x20])
    out.append('static const std::uint32_t kDlpRealIslandTransferSize = 0x%08x;  // frame 0, +0x38: the child\'s CIA up to its meta block (zero in our announcement)' % be32(group[0][5], 0x38))
    out.append('static const std::uint32_t kDlpRealIslandRequiredSize = 0x%08x;  // frame 0, +0x3C: the whole CIA (zero in ours)' % be32(group[0][5], 0x3C))
    out.append('static const std::uint8_t kDlpRealIslandPass[9] = {%s};  // the final, +0x14: the STAND-IN "%s" (seven hex digits and a \'v\', as the game makes it)'
               % (', '.join('0x%02x' % x for x in STAND_IN), STAND_IN[:8].decode()))
    out.append('// The two networks of the host, from their beacons (same transmitter, same Wi-Fi channel): the Download Play\'s while it accepts, the game\'s own after the final.')
    out.append('static const std::uint32_t kDlpRealIslandLobbyNet = 0x%08x;   // wlancommid; id %d, attributes %04x, %d nodes at most, no application data' % (dlp[0]['wlan'], dlp[0]['id'], dlp[0]['attr'], dlp[0]['max']))
    out.append('static const std::uint32_t kDlpRealIslandGameNet = 0x%08x;    // wlancommid; id %d, attributes %04x, network id %d, %d nodes at most, no application data'
               % (game[0]['wlan'], game[0]['id'], game[0]['attr'], game[0]['net'], game[0]['max']))
    out.append('static const unsigned kDlpRealIslandGameNetId = %d;' % game[0]['id'])
    out.append('static const unsigned kDlpRealIslandGameNetNodes = %d;' % game[0]['max'])
    out.append('static const bool kDlpRealIslandGameNetSameHost = true;  // the beacons of the two networks have the same transmitter: the address the client is given with the passphrase')
    out.append('static const bool kDlpRealIslandLobbySeedsZero = true;   // the client\'s friend code seed in the Download Play\'s network is zero (its own in the game\'s network: not here)')
    out.append('// Times of the good session, milliseconds (the capture\'s clock): the client reads the announcement as a spectator, comes in as a client, waits; then')
    out.append('static const unsigned kDlpRealIslandWaitMs = %d;        // from the client\'s admission to the Auth: nothing on channel 2 meanwhile' % ms(t_join, t_a1))
    out.append('static const unsigned kDlpRealIslandDialogueMs = %d;      // from the Auth to the answer to the notice' % ms(t_a1, float(good[-1][0])))
    out.append('static const unsigned kDlpRealIslandFinalToNetMs = %d;    // from the final to the first beacon of the game\'s own network' % ms(t_final, game[0]['t']))
    out.append('static const unsigned kDlpRealIslandFinalToByeMs = %d;   // from the final to the client\'s Deauthentication' % ms(t_final, t_bye))
    out.append('static const unsigned kDlpRealIslandFinalToChildMs = %d; // from the final to the child\'s admission to the game\'s network' % ms(t_final, t_child))
    out.append('')
    names = []
    for k, (pid, r, g, red) in enumerate(packets):
        name = 'kDlpRealIsland_%02d_%s' % (k, pid)
        names.append(name)
        c_array(out, name, g, '%s  t=%s  channel %d  node %d -> %d  %d bytes' % (pid, r[0], r[1], r[2], r[3], len(g)))
    out.append('')

    def entry(k):
        pid, r, g, red = packets[k]
        rs, re_ = red[0] if red else (0, 0)
        return ('    {"%s", "%s", %d, %d, %d, %d, %s, 0x%08x, 0x%08x, {0x%x, 0x%x}},'
                % (pid, r[0], r[1], r[2], r[3], len(g), names[k], be32(g, 8), checksum(SEED_ZERO, g), rs, re_))

    out.append('static const DlpRealGolden kDlpRealIslandAnnounce[5] = {')
    for k in range(5):
        out.append(entry(k))
    out.append('};')
    out.append('static const DlpRealGolden kDlpRealIslandDialog[%d] = {' % len(good))
    for k in range(5, len(packets)):
        out.append(entry(k))
    out.append('};')
    text = '\n'.join(out) + '\n'
    # (the last look, on the BYTES that are written - the text holds them as "0xaa, 0xbb", where a search for "aa:bb" finds nothing: no address
    # of a node of the capture, straight or reversed, and no name of a node, as UTF-8 or as the UTF-16 the wire carries, in either byte order)
    nicks = [tx[18:].rstrip(')') for tx in (host_tx, client_tx)]   # ("aa:bb:cc:dd:ee:ff(name)": the name the emulator had in the room)
    macs = [bytes.fromhex(tx[:17].replace(':', '')) for tx in (host_tx, client_tx)]
    refuse(not all(nicks) or any(len(mac) != 6 for mac in macs) or macs[0] == macs[1], 'the two nodes of the capture are not read')
    refuse(any(mac in p[2] or mac[::-1] in p[2] for mac in macs for p in packets), 'an address of a node of the capture in a packet')
    refuse(any(n.encode(enc) in p[2] for n in nicks for enc in ('utf-8', 'utf-16-be', 'utf-16-le') for p in packets), 'a name of a node of the capture in a packet')
    # (... and on the text around the bytes, the comments and the numbers: no address as it is printed, no name)
    refuse(any(bad in text.lower() for bad in [tx[:17].lower() for tx in (host_tx, client_tx)] + [mac.hex() for mac in macs] + [n.lower() for n in nicks]),
           'an address or a name of a node of the capture in the text')
    open(dest, 'w', newline='\n').write(text)
    print('wrote', dest, ':', len(packets), 'packets,', sum(len(p[2]) for p in packets), 'bytes; seed', seed.hex(), '; every checksum checked;',
          len(starts), 'groups, step', '%.2f..%.2f s' % (min(steps), max(steps)))
    print('the dialogue:', ' '.join(p[0] for p in packets[5:]), '; wait', ms(t_join, t_a1), 'ms; final ->', ms(t_final, game[0]['t']), 'ms the game\'s network,',
          ms(t_final, t_bye), 'ms the goodbye,', ms(t_final, t_child), 'ms the child in')


main()
