# (2026-11-15, N4) The table of Tests/vsim/party_grid.sh: one line per load, players and variant, over the seeds - the runs the room dropped the console
# in, the datagrams the ring lost, the room's resends, the numbers skipped while the thread was away, the thread's busy time, how long a frame of the game
# waited for ENet (p95), how late the room's frames reached the game (p95), the longest round, and for the dice the steps a second of the two games.
#   python3 Tests/vsim/party_grid.py <runs.txt>
import collections
import sys

rows = collections.OrderedDict()
for line in open(sys.argv[1]):
    kv = dict(p.split('=', 1) for p in line.split() if '=' in p)
    key = (kv['load'], int(kv['players']), kv['variant'])
    rows.setdefault(key, []).append(kv)


def num(runs, k, f=sum):
    vals = [float(r[k]) for r in runs if k in r]
    return f(vals) if vals else float('nan')


def mean(v):
    return sum(v) / len(v)


print('%-6s %2s %-6s | %5s %6s %7s %6s %6s %7s %7s %7s %8s | %s' % ('load', 'pl', 'var', 'drops', 'ring', 'resend', 'skipA', 'busy', 'txw95', 'add95', 'roundMx',
                                                                      'budget/drain', 'game steps con/azh (dead)'))
for (load, n, var), runs in rows.items():
    drops = sum(1 for r in runs if float(r.get('room_disconnects', 0)) > 0 or float(r.get('lost_at_s', -1)) >= 0)
    game = ''
    if 'game_con_steps_s' in runs[0]:
        dead = sum(1 for r in runs if float(r.get('game_con_dead_at_s', -1)) >= 0 or float(r.get('game_azh_dead_at_s', -1)) >= 0)
        game = '%.1f/%.1f (%d)' % (num(runs, 'game_con_steps_s', min), num(runs, 'game_azh_steps_s', min), dead)
    print('%-6s %2d %-6s | %5d %6.0f %7.0f %6.0f %6.0f %7.1f %7.1f %7.1f %4.0f/%-4.0f | %s' % (
        load, n, var, drops, num(runs, 'ring_drops'), num(runs, 'room_retransmits'), num(runs, 'enet_seq_skipped_away'), num(runs, 'busy_ms_s', mean),
        num(runs, 'txwait_p95', max), num(runs, 'added_ms_p95', max), num(runs, 'round_max_ms', max), num(runs, 'budget_rounds'), num(runs, 'drain_rounds'), game))
print('(%d runs; ring, resend and skipA are sums over the seeds; busy a mean; txw95, add95 and roundMx the worst seed; game steps the worst seed, 30 is the pace)'
      % sum(len(v) for v in rows.values()))
