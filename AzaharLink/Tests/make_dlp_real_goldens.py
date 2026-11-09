#!/usr/bin/env python3
"""Makes the golden data of the real Download Play wire (the RealDlp mode, 3DS Link 1.1.3) from the capture of two Azahar emulators that
played Luigi's Mansion (2018) through a room (capture 1c, 2026-11-09: Prova-B hosts, Prova-A joins as a fake client).

    python -I Tests/make_dlp_real_goldens.py <dati.txt of the decoded capture 1c> Tests/dlp_real_luigi_golden.inc

The capture (2 MB) and its decoding stay outside the repository (the kit of analysis/luigi-azahar-2026-11-09, folder
registri/1c-decodificato); what the tests need from it is small and is written as a C++ include file next to the tests:

  - the five frames of the announcement group of t=770.168 (data channel 1, host -> everyone): 768 + 3 x 1448 + 1464 bytes;
  - the thirteen packets of data channel 2 from t=773.427 to t=774.003 (the dialogue A1..A9 of the design, repeats included);
  - the checksums of the announcement group of t=739.896, which must be the same as those of t=770.168.

What is Nintendo's stays out of the repository (the owner's rule of 2026-10-23): the titles and the icon of the game, which the host puts in
frame 0 (+0x40..+0x2F7) and in frames 1-3 (+0x10 to the end), are REPLACED by zeros, except the first word of each such range, which holds
the 32-bit sum of the big-endian words that were there. The Download Play checksum is a function of that sum only (see dlph_checksum in
DlpLink/host/dlph_wire.c), so the checksum at +8..11 of every golden is still the one of the capture; this script checks it, with the seed of
the capture (ff a4 72 ff) and with the seed 00 00 00 00 of our transmitter (design 2.5), against the values the design lists.
No MAC address, IP or seed of a console is written out: only the payloads and the node numbers.
Nothing here is edited by hand: run it again on the same capture and diff.
"""
import re
import sys

RE = re.compile(r'^\s*(\d+\.\d+) c\d+ .*?SecureData canale (\d+) seq \d+ nodo (\d+) -> (\d+) mgmt (\d) \[(\d+)\] ([0-9a-f]*)\s*$')

SEED_CAPTURE = bytes([0xFF, 0xA4, 0x72, 0xFF])
SEED_ZERO = bytes(4)

# the checksums the design lists (its appendix A and test W1/W1b), in the order of the packets below
WANT_CK_CAPTURE_ANNOUNCE = [0xa85b0d4c, 0xac4b0c08, 0xe85f2c0a, 0xad435d0d, 0xe8470ccf]
WANT_CK_ZERO_ANNOUNCE = [0xbdfff7f2, 0xafbfffd7, 0xbebfffef, 0x1195bfbf, 0xbfbfbf02]
WANT_CK_ZERO_DIALOG = {'A1': 0x17171602, 'A2': 0x1b1b1b03, 'A3': 0x17171703, 'A4': 0x1f1f1b03, 'A5': 0x1f1f1f03, 'A6': 0x27272303,
                       'A7': 0xb7b7bfb9, 'A8': 0x1d1f1f03, 'A9': 0x1d1f1f03}
WANT_DIALOG = ['A1', 'A1', 'A2', 'A1', 'A3', 'A2', 'A2', 'A4', 'A5', 'A6', 'A7', 'A8', 'A9']


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


def main():
    rows = []
    for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
        m = RE.match(line)
        if not m:
            continue
        t, ch, src, dst, mgmt, n, hx = m.groups()
        b = bytes.fromhex(hx)
        if len(b) != int(n):
            raise SystemExit('line %s: %d bytes for [%s]' % (t, len(b), n))
        rows.append((t, int(ch), int(src), int(dst), int(mgmt), b))
    ch1 = [r for r in rows if r[1] == 1]
    ch2 = [r for r in rows if r[1] == 2]

    def group_at(t0):
        i = next(k for k, r in enumerate(ch1) if r[0] == t0 and len(r[5]) == 768)
        g = ch1[i:i + 5]
        assert [len(r[5]) for r in g] == [768, 1448, 1448, 1448, 1464], [len(r[5]) for r in g]
        assert [r[5][12] for r in g] == [0, 1, 2, 3, 4]
        return g

    group = group_at('770.168')
    first = group_at('739.896')
    # the identity: the group of t=739.896 is the group of t=770.168, byte for byte (so its checksums are the same)
    assert [r[5] for r in first] == [r[5] for r in group]
    dialog = [r for r in ch2 if 773.427 <= float(r[0]) <= 774.003]
    assert len(dialog) == 13, len(dialog)

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
    for r in dialog:
        packets.append((dialog_id(r[5], r[2]), r, r[5], []))
    assert [p[0] for p in packets[5:]] == WANT_DIALOG, [p[0] for p in packets[5:]]

    for k, (pid, r, g, red) in enumerate(packets):
        b = r[5]
        assert r[4] == 0, 'mgmt'
        assert int.from_bytes(b[4:6], 'big') == len(b), pid
        assert b[1] == 2 and b[2:4] == b'\0\0' and b[6:8] == b'\x02\x00', pid
        ck = be32(b, 8)
        assert checksum(SEED_CAPTURE, b) == ck, pid
        assert checksum(SEED_CAPTURE, g) == ck, pid + ' (redacted)'
        if k < 5:
            assert ck == WANT_CK_CAPTURE_ANNOUNCE[k], pid
            assert checksum(SEED_ZERO, g) == WANT_CK_ZERO_ANNOUNCE[k], pid
        else:
            assert checksum(SEED_ZERO, g) == WANT_CK_ZERO_DIALOG[pid], pid
        if red:
            assert g[:red[0][0]] == b[:red[0][0]] and g[red[-1][1]:] == b[red[-1][1]:]

    out = []
    out.append('// Generated by Tests/make_dlp_real_goldens.py from the capture 1c (2026-11-09, two Azahar emulators, Luigi\'s Mansion 2018, '
               'host Prova-B = node 1, fake client Prova-A = node 2) - do not edit by hand.')
    out.append('// The SecureData payloads of the real Download Play, as they went through the room: the announcement group of t=770.168 '
               '(data channel 1, node 1 -> 0xFFFF)')
    out.append('// and the dialogue of data channel 2 from t=773.427 to t=774.003 (ids A1..A9 of PROGETTO-DLP-VERO-1.1.3.md, section 2.3).')
    out.append('// The game\'s titles and icon are NOT here: each range listed in `redacted` is zeros except its first word, the 32-bit sum of '
               'the big-endian words')
    out.append('// the capture had there, so that the checksum at +8..11 (seed ff a4 72 ff) still holds. `checksum_zero` is the checksum of the '
               'same bytes with the seed 00 00 00 00.')
    out.append('// Needs <cstddef> and <cstdint>.')
    out.append('')
    out.append('struct DlpRealGolden {')
    out.append('    const char* id;          // F0..F4 (announcement frames) or A1..A9 (dialogue)')
    out.append('    const char* time;        // the capture\'s clock, seconds')
    out.append('    unsigned channel;        // SecureData data channel')
    out.append('    unsigned src_node, dst_node;')
    out.append('    std::size_t size;')
    out.append('    const std::uint8_t* bytes;')
    out.append('    std::uint32_t checksum_capture;  // +8..11 as captured (seed ff a4 72 ff)')
    out.append('    std::uint32_t checksum_zero;     // the checksum of these bytes with the seed 00 00 00 00 (design 2.5)')
    out.append('    std::size_t redacted[2];         // [start, end) of the range that carries only its sum (0, 0: none)')
    out.append('};')
    out.append('')
    out.append('static const std::uint8_t kDlpRealLuigiSeed[4] = {0xff, 0xa4, 0x72, 0xff};')
    out.append('')
    names = []
    for k, (pid, r, g, red) in enumerate(packets):
        name = 'kDlpRealLuigi_%02d_%s' % (k, pid)
        names.append(name)
        out.append('// %s  t=%s  channel %d  node %d -> %d  %d bytes' % (pid, r[0], r[1], r[2], r[3], len(g)))
        out.append('static const std::uint8_t %s[%d] = {' % (name, len(g)))
        for i in range(0, len(g), 24):
            out.append('    ' + ', '.join('0x%02x' % x for x in g[i:i + 24]) + ',')
        out.append('};')
    out.append('')

    def entry(k):
        pid, r, g, red = packets[k]
        rs, re_ = red[0] if red else (0, 0)
        return ('    {"%s", "%s", %d, %d, %d, %d, %s, 0x%08x, 0x%08x, {0x%x, 0x%x}},'
                % (pid, r[0], r[1], r[2], r[3], len(g), names[k], be32(g, 8), checksum(SEED_ZERO, g), rs, re_))

    out.append('static const DlpRealGolden kDlpRealLuigiAnnounce[5] = {')
    for k in range(5):
        out.append(entry(k))
    out.append('};')
    out.append('static const DlpRealGolden kDlpRealLuigiDialog[13] = {')
    for k in range(5, 18):
        out.append(entry(k))
    out.append('};')
    out.append('// the checksums of the announcement group of t=739.896 (the first one Prova-A heard): the same bytes as the group of t=770.168')
    out.append('static const std::uint32_t kDlpRealLuigiAnnounceChecksumsAt739[5] = {%s};'
               % ', '.join('0x%08x' % be32(r[5], 8) for r in first))
    open(sys.argv[2], 'w', newline='\n').write('\n'.join(out) + '\n')
    print('wrote', sys.argv[2], ':', len(packets), 'packets,', sum(len(p[2]) for p in packets), 'bytes; every checksum checked')


main()
