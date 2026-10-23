#!/usr/bin/env python3
"""Makes the golden data the replay tests include from the real hardware captures of the user's console.

    python -I Tests/make_goldens.py <PROVA 2 AzaharLink.debug.log> <TFH AzaharLink.debug.log> <PROVA 1 AzaharLink.debug.log>

The captures are 20-65 MB and stay outside the repository; what the tests need from them is small and is written as
C++ include files next to the tests:

  Tests/ffe_host_golden.inc  the command sequence of FINAL FANTASY EXPLORERS as the HOST of a lobby (PROVA 2, 2026-10-10,
                             plugin 0.1.15 interim build, 4 players): request words and the reply words our HLE gave,
                             plus real payloads (FRAME rx / tx lines) paired with the PullPacket / SendTo they belong to
  Tests/tfh_host_golden.inc  the same for ZELDA TRI FORCE HEROES (first TFH trace, 2026-10-10, 3 players)
  Tests/lobby_trace.inc      the frames of the lid episode of PROVA 2 as {kind, src node, dest node, size}: the lobby
                             before the lid (4 of 4), the second lobby of the game that the three PCs made in the same
                             room while the 3DS was dark (501 of its frames overflowed the game's ring), and the PCs
                             coming back; and a stretch of PROVA 1 (the 3DS as a client of a PC) for the client role

Only lines the capture has whole are used: the script refuses a pick that sits next to a "records were dropped" marker.
Nothing here is edited by hand: run it again with newer captures and diff.
"""
import re
import sys

RE_LINE = re.compile(r'^\s*(\d+\.\d+) (\S+)(.*)$')
RE_CMD = re.compile(r'^ cmd 0x([0-9a-f]+) req ?(.*?) -> rep ?(.*)$')
RE_PULL = re.compile(r'^ PullPacket bind (\d+): (\d+) bytes from node (\d+) req (.*?) -> rep (.*)$')
RE_FRAME = re.compile(r'^ (rx|tx|other)\s+(\S+)>(\S+) ch(\d+) (\d+)B: ([0-9a-f]*)$')
RE_BEACON = re.compile(r'^ (rx|tx)\s+([0-9a-f:]+) ch(\d+) (\d+)B: ([0-9a-f]*)$')


def words(s):
    return [int(w, 16) for w in s.split()] if s.strip() else []


def node(s):
    return 0xFFFF if s == 'all' else int(s)


class Capture:
    def __init__(self, path):
        self.recs = []  # dicts: ln, t, k ('uds'|'ndm'|'pull'|'frame'|'beacon'|'drop'|'other'), ...
        with open(path, 'r', encoding='latin-1') as f:
            for ln, line in enumerate(f, 1):
                line = line.rstrip('\n')
                if line.startswith('debug:'):
                    self.recs.append(dict(ln=ln, t=None, k='drop'))
                    continue
                m = RE_LINE.match(line)
                if not m:
                    continue
                t, kind, rest = float(m.group(1)), m.group(2), m.group(3)
                if kind in ('uds', 'ndm'):
                    mm = RE_CMD.match(rest)
                    if mm:
                        self.recs.append(dict(ln=ln, t=t, k=kind, id=int(mm.group(1), 16), req=words(mm.group(2)),
                                              rep=words(mm.group(3))))
                        continue
                    mm = RE_PULL.match(rest)
                    if mm:
                        self.recs.append(dict(ln=ln, t=t, k='pull', bind=int(mm.group(1)), n=int(mm.group(2)),
                                              src=int(mm.group(3)), req=words(mm.group(4)), rep=words(mm.group(5))))
                elif kind == 'FRAME':
                    mm = RE_FRAME.match(rest)
                    if mm:
                        self.recs.append(dict(ln=ln, t=t, k='frame', dir=mm.group(1), src=node(mm.group(2)),
                                              dst=node(mm.group(3)), ch=int(mm.group(4)), n=int(mm.group(5)),
                                              hex=mm.group(6)))
                elif kind == 'BEACON':
                    mm = RE_BEACON.match(rest)
                    if mm:
                        self.recs.append(dict(ln=ln, t=t, k='beacon', dir=mm.group(1), mac=mm.group(2), n=int(mm.group(4)),
                                              hex=mm.group(5)))
        self.dropped_at = [r['ln'] for r in self.recs if r['k'] == 'drop']

    def whole(self, ln, span=3):
        """True when no 'records were dropped' marker is within `span` lines of `ln` (a hole in the capture)."""
        return not any(abs(d - ln) <= span for d in self.dropped_at)

    def pick(self, kind, cmd_id, nth=0, after=0.0, before=1e12, pred=None):
        n = 0
        for r in self.recs:
            if r['k'] != kind or r.get('id') != cmd_id or r['t'] is None or not (after <= r['t'] < before):
                continue
            if pred and not pred(r):
                continue
            if n == nth:
                if not self.whole(r['ln']):
                    raise SystemExit('pick %s 0x%02x #%d sits next to a hole in the capture (line %d)' % (kind, cmd_id, nth, r['ln']))
                return r
            n += 1
        raise SystemExit('pick %s 0x%02x #%d (after %.1f) not found' % (kind, cmd_id, nth, after))


def hexw(ws):
    return ', '.join('0x%08x' % w for w in ws)


def cmd_entry(tag, r):
    ws = r['req']
    rp = r['rep']
    return '    {"%s", %d, %d, %d, {%s}, {%s}}, // %.3f' % (tag, r['id'], len(ws), len(rp), hexw(ws), hexw(rp), r['t'])


def emit_cmds(out, name, picks):
    out.append('static const GoldenCmd %s[] = {' % name)
    for tag, r in picks:
        out.append(cmd_entry(tag, r))
    out.append('};')
    out.append('')


def pair_frames(cap, first, last, bind_line_kind, max_pairs, direction):
    """Pairs the k-th PullPacket (or SendTo) of the stretch with the k-th FRAME rx (or tx) line: the queue of a bind is
    first in, first out, and a SendTo has one FRAME tx line. The sizes must agree or the stretch is refused."""
    frames = [r for r in cap.recs if r['k'] == 'frame' and r['dir'] == direction and first <= r['t'] < last]
    if direction == 'rx':
        calls = [r for r in cap.recs if r['k'] == 'pull' and first <= r['t'] < last and r['n'] > 0]
    else:
        calls = [r for r in cap.recs if r['k'] == 'uds' and r['id'] == 0x17 and first <= r['t'] < last]
    pairs = []
    for f, c in zip(frames, calls):
        size = c['n'] if direction == 'rx' else c['req'][4]
        if f['n'] != size or len(f['hex']) != 2 * f['n']:
            raise SystemExit('frame/call mismatch at %.3f: %d vs %d' % (f['t'], f['n'], size))
        if not (cap.whole(f['ln']) and cap.whole(c['ln'])):
            raise SystemExit('hole inside the paired stretch at %.3f' % f['t'])
        pairs.append((f, c))
        if len(pairs) >= max_pairs:
            break
    return pairs


def ffe_host(cap, out):
    out.append('// ---- FFE as the host of a lobby (PROVA 2): contiguous stretches of the capture, every reply as our HLE gave it ----')
    P = []

    def add(tag, r):
        P.append((tag, r))

    add('ndm-override', cap.pick('ndm', 0x14, 0))
    add('ndm-suspend', cap.pick('ndm', 0x06, 0))
    add('ndm-enter', cap.pick('ndm', 0x01, 0))
    add('ndm-leave', cap.pick('ndm', 0x02, 0))
    add('initialize', cap.pick('uds', 0x1B, 0))
    add('status-off', cap.pick('uds', 0x0B, 0, pred=lambda r: r['rep'][1] == 3))
    add('bind-2', cap.pick('uds', 0x12, 0, pred=lambda r: r['req'][0] == 2))
    add('set-0x21', cap.pick('uds', 0x21, 0))
    add('begin-hosting', cap.pick('uds', 0x1D, 0))
    add('status-host-1', cap.pick('uds', 0x0B, 0, pred=lambda r: r['rep'][-1] == 0x00010401))
    add('get-channel', cap.pick('uds', 0x1A, 0))
    # SetApplicationData: after each join FFE publishes the list of its players in the application data of the lobby
    add('set-app-data', cap.pick('uds', 0x10, 0, after=200.0))
    # (the status read of the fourth player, 222 s, is not in the capture: the writer dropped it)
    for total, last in ((2, 0x00030402), (3, 0x00070403)):
        r = cap.pick('uds', 0x0B, 0, pred=lambda r, last=last: r['rep'][-1] == last and r['rep'][0] == 0)
        add('status-host-%d' % total, r)
        add('node-info-%d' % total, cap.pick('uds', 0x0D, 0, after=r['t'], pred=lambda r2, total=total: r2['req'][0] == total))
    # the teardown of the game (t > 2000): the pull of the bind that is gone, the unbinds, Bind(4), Destroy, status, Shutdown
    t0 = 2085.0
    add('pull-no-bind', cap.pick('uds', 0x14, 0, after=t0, pred=lambda r: r['rep'][0] == 0xE10113EA))
    add('unbind-2', cap.pick('uds', 0x13, 0, after=t0, pred=lambda r: r['req'][0] == 2))
    add('unbind-1', cap.pick('uds', 0x13, 0, after=t0, pred=lambda r: r['req'][0] == 1))
    add('bind-4', cap.pick('uds', 0x12, 0, after=t0, pred=lambda r: r['req'][0] == 4))
    add('status-after-destroy', cap.pick('uds', 0x0B, 0, after=t0, pred=lambda r: r['rep'][1] == 3))
    add('destroy', cap.pick('uds', 0x08, 0, after=t0))
    add('unbind-4', cap.pick('uds', 0x13, 0, after=t0, pred=lambda r: r['req'][0] == 4))
    add('unbind-3', cap.pick('uds', 0x13, 0, after=t0, pred=lambda r: r['req'][0] == 3))
    add('shutdown', cap.pick('uds', 0x03, 0, after=t0))
    emit_cmds(out, 'kFfeHostCmds', P)
    return P


def tfh_host(cap, out):
    out.append('// ---- Tri Force Heroes as the host of a lobby (first TFH trace) ----')
    P = []

    def add(tag, r):
        P.append((tag, r))

    add('ndm-override', cap.pick('ndm', 0x14, 0))
    add('ndm-suspend', cap.pick('ndm', 0x06, 0))
    add('ndm-enter', cap.pick('ndm', 0x01, 0))
    add('ndm-leave', cap.pick('ndm', 0x02, 0))
    add('initialize', cap.pick('uds', 0x1B, 0))
    add('bind-2', cap.pick('uds', 0x12, 0))
    add('set-0x21', cap.pick('uds', 0x21, 0))
    add('begin-hosting', cap.pick('uds', 0x1D, 0))
    add('status-host-1', cap.pick('uds', 0x0B, 0, pred=lambda r: r['rep'][-1] == 0x00010301))
    add('status-host-2', cap.pick('uds', 0x0B, 0, pred=lambda r: r['rep'][-1] == 0x00030302))
    add('status-host-3', cap.pick('uds', 0x0B, 0, pred=lambda r: r['rep'][-1] == 0x00070303))
    add('unbind-2', cap.pick('uds', 0x13, 0, pred=lambda r: r['req'][0] == 2))
    add('unbind-1', cap.pick('uds', 0x13, 0, pred=lambda r: r['req'][0] == 1))
    add('shutdown', cap.pick('uds', 0x03, 0))
    emit_cmds(out, 'kTfhHostCmds', P)
    return P


def network_info(cap, who, out, name):
    """The NetworkInfo the game gave BeginHostingNetwork, read back from the first beacon the plugin sent: wlancommid,
    id8, attribute word, network id, node counts and the application data."""
    tx = next(r for r in cap.recs if r['k'] == 'beacon' and r['dir'] == 'tx')
    body = bytes.fromhex(tx['hex'])
    pos = 12
    info = None
    while pos + 2 <= len(body):
        tid, ln = body[pos], body[pos + 1]
        pos += 2
        d = body[pos:pos + ln]
        pos += ln
        if tid == 221 and d[:3] == b'\x00\x1f\x32' and d[3] == 0x15:
            info = d
    assert info is not None
    app_size = info[0x33]
    app = info[0x34:0x34 + app_size]
    out.append('// %s: the NetworkInfo the game gave BeginHostingNetwork (read back from the first beacon sent at %.3f)' % (who, tx['t']))
    out.append('static const struct GoldenNetwork %s = {0x%08x, 0x%02x, 0x%04x, 0x%08x, %d, %d, "%s"};'
               % (name, int.from_bytes(info[4:8], 'big'), info[8], int.from_bytes(info[10:12], 'big'),
                  int.from_bytes(info[12:16], 'big'), info[17], app_size, app.hex()))
    out.append('')


def app_data_after(cap, t, out, name):
    "The application data of the first beacon the plugin sent after time t: what the SetApplicationData call at t set."
    tx = next(r for r in cap.recs if r['k'] == 'beacon' and r['dir'] == 'tx' and r['t'] > t)
    body = bytes.fromhex(tx['hex'])
    pos = 12
    info = None
    while pos + 2 <= len(body):
        tid, ln = body[pos], body[pos + 1]
        pos += 2
        d = body[pos:pos + ln]
        pos += ln
        if tid == 221 and d[:3] == b'\x00\x1f\x32' and d[3] == 0x15:
            info = d
    assert info is not None
    app = info[0x34:0x34 + info[0x33]]
    out.append('// %s: the application data (%d bytes) of the first beacon sent after the SetApplicationData call at %.3f (the beacon is at %.3f; %d nodes of %d)'
               % (name, len(app), t, tx['t'], info[16], info[17]))
    out.append('static const char %s[] = "%s";' % (name, app.hex()))
    out.append('')


def payload_pairs(cap, out, name, first, last, direction, max_pairs):
    pairs = pair_frames(cap, first, last, None, max_pairs, direction)
    out.append('// %s: %d real payloads (%s) with the %s they belong to' % (
        name, len(pairs), 'FRAME rx lines' if direction == 'rx' else 'FRAME tx lines',
        'PullPacket reply' if direction == 'rx' else 'SendTo request'))
    out.append('static const GoldenFrame %s[] = {' % name)
    for f, c in pairs:
        out.append('    {%d, %d, %d, %s, {%s}, "%s"}, // %.3f' % (
            f['src'], f['dst'], f['n'], 'true' if direction == 'rx' else 'false', hexw(c['rep'] if direction == 'rx' else c['req']),
            f['hex'], f['t']))
    out.append('};')
    out.append('')


def trace_rows(cap, first, last, kinds, limit=None):
    rows = [r for r in cap.recs if r['k'] == 'frame' and first <= r['t'] < last and r['dir'] in kinds]
    return rows[:limit] if limit else rows


def emit_trace(out, name, rows, comment):
    out.append('// %s' % comment)
    out.append('static const TraceFrame %s[] = {' % name)
    line = '   '
    for i, r in enumerate(rows):
        kind = {'rx': 'R', 'other': 'O', 'tx': 'T'}[r['dir']]
        item = " {'%s',%d,%d,%d}," % (kind, r['src'], r['dst'], r['n'])
        if len(line) + len(item) > 128:
            out.append(line)
            line = '   '
        line += item
    out.append(line)
    out.append('};')
    out.append('')


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    p2 = Capture(sys.argv[1])
    tfh = Capture(sys.argv[2])
    p1 = Capture(sys.argv[3])
    hdr = ['// Generated by Tests/make_goldens.py from the real captures of the user\'s console - do not edit by hand.']

    # ---- FFE host ------------------------------------------------------------------------------
    out = list(hdr)
    out.append('// PROVA 2 (2026-10-10): FINAL FANTASY EXPLORERS, 3DS = host of a lobby with three PCs, plugin 0.1.15 interim build.')
    out.append('')
    picks = ffe_host(p2, out)
    network_info(p2, 'FFE', out, 'kFfeHostNetwork')
    app_data_after(p2, dict(picks)['set-app-data']['t'], out, 'kFfeHostAppData')
    payload_pairs(p2, out, 'kFfeHostPulled', 200.9, 215.0, 'rx', 24)
    payload_pairs(p2, out, 'kFfeHostSent', 200.9, 215.0, 'tx', 24)
    open('Tests/ffe_host_golden.inc', 'w', newline='\n').write('\n'.join(out) + '\n')

    # ---- TFH host ------------------------------------------------------------------------------
    out = list(hdr)
    out.append('// First trace of ZELDA TRI FORCE HEROES (2026-10-10): 3DS = host of a lobby of 3, plugin 0.1.15 interim build.')
    out.append('')
    tfh_host(tfh, out)
    network_info(tfh, 'TFH', out, 'kTfhHostNetwork')
    payload_pairs(tfh, out, 'kTfhHostPulled', 83.2, 90.0, 'rx', 24)
    payload_pairs(tfh, out, 'kTfhHostSent', 83.2, 90.0, 'tx', 24)
    open('Tests/tfh_host_golden.inc', 'w', newline='\n').write('\n'.join(out) + '\n')

    # ---- the lid episode of PROVA 2 and a client stretch of PROVA 1 --------------------------------
    out = list(hdr)
    out.append("// Frames as the 3DS logged them: {kind (R = for this node and delivered, O = for another node, T = sent by the 3DS),")
    out.append('// source node, destination node (0xffff = everybody), payload bytes}. Channel 243 all along.')
    out.append('')
    emit_trace(out, 'kLobbyBeforeLid', trace_rows(p2, 1000.0, 1100.0, ('rx', 'other'), 500),
               'PROVA 2, 4 of 4 players, 500 frames from tick 1000 s (the 3DS hosts)')
    emit_trace(out, 'kOtherLobby', trace_rows(p2, 1985.4, 2027.69, ('rx', 'other')),
               'PROVA 2, tick 1985.4 - 2027.69 s: the 3DS is host of a lobby of 1 and the three PCs have made a lobby of their own '
               'in the room; the frames it logged as received are theirs (their node ids are ours)')
    emit_trace(out, 'kLobbyAfterLid', trace_rows(p2, 2027.7, 2090.0, ('rx', 'other'), 600),
               'PROVA 2, from tick 2027.7 s: the PCs are back in the 3DS\'s lobby (one at 2027.7, a second at 2041.1)')
    emit_trace(out, 'kClientStretch', trace_rows(p1, 300.0, 400.0, ('rx', 'tx'), 600),
               'PROVA 1, the 3DS as client (node 2) of a PC host: 600 frames from tick 300 s')
    open('Tests/lobby_trace.inc', 'w', newline='\n').write('\n'.join(out) + '\n')
    print('written Tests/ffe_host_golden.inc, tfh_host_golden.inc, lobby_trace.inc')


if __name__ == '__main__':
    main()
