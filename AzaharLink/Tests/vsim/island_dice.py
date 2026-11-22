#!/usr/bin/env python3
"""island_dice.py <vsim of this tree> [<vsim of a tree without the remedy>]: the dice phase of Mario Party: Island Tour as a load (Tests/vsim/island_dice.sh
builds the models and calls this). The game is the closed loop of vsim_client.c (game=1), the costs are those of the console in that phase
(sample/island_dice.cfg), run over send costs from 4 to 12 ms, with and without a lossy WiFi, six seeds each.

What is held (a failed line ends the run with 1):
  before      at the send costs measured (6.25 and 8.2 ms) the console's game loses the other player in at least 5 runs of 6, frames are never handed over,
              and every run ends with the waiting data at ENet's limit or with the room having given the console up: the dead end of 2026-11-14
  the relief  with the same-frame filter and the hand-over burst of azl/flow_relief.hpp (same_ms=30 burst=16): in EVERY run the game makes at least 28 of
              its 30 steps a second, every frame of the room is handed over, none later than 500 ms after the room had it (1000 ms at a send cost of
              12 ms, half as much again as the worst measured), the socket loses nothing and ENet's limit is never reached
  other games the sample trace of a game that works gives the same output from the two models, line for line (the keys both print)
What is only shown: this tree WITHOUT the relief (the loop every other game has): the stream no longer stops for good, and the game does not play;
and a party of three and four players (game_players), where the relief is NOT enough in the model.
"""
import os
import statistics
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
CFG = os.path.join(here, 'sample', 'island_dice.cfg')
TRACE = os.path.join(here, 'sample', 'island_dice.trace')
SAMPLE = (os.path.join(here, 'sample', 'TFH_friend_b1.cfg'), os.path.join(here, 'sample', 'TFH_friend_b1.trace'))
RELIEF = ['same_ms=30', 'burst=16']   # azl/title_rules.hpp: the row of Island Tour (same_frame_ms, hand_over_burst)
SEEDS = [1, 2, 3, 4, 5, 6]
LIMIT = 192 * 1024
BOUND_MS = 500.0        # the latest a frame of the room may reach the game with the relief, at send costs up to 10 ms ...
BOUND_SLOW_MS = 1000.0  # ... and beyond
STEPS_MIN = 28.0

new = sys.argv[1]
old = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else None
failures = 0


def run(model, keys, files=(CFG, TRACE)):
    out = subprocess.run([model, files[0], files[1]] + keys, capture_output=True, text=True).stdout
    values = {}
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            try:
                values[k] = float(v)
            except ValueError:
                pass
    return values, out


def hold(ok, what):
    global failures
    if not ok:
        failures += 1
        print('  FAIL: ' + what)


def rows_of(model, keys):
    return [run(model, keys + ['seed=%d' % s])[0] for s in SEEDS]


def line(name, rows):
    g = lambda k: [r.get(k, 0.0) for r in rows]
    lost = sum(1 for r in rows if r.get('game_con_dead_at_s', -1) >= 0 or r.get('game_azh_dead_at_s', -1) >= 0)
    print('  %-26s steps/s %5.1f (least %5.1f) | room frame to the game: median %6.0f ms, worst %6.0f | never handed over %5.0f | lost the other %d/6 | socket lost %4.0f |'
          ' refused at the limit %4.0f, taken over it %3.0f | waiting at the end %3.0f KB | the room gave the console up %d/6 | soc send+recv %4.0f ms/s' % (
              name, statistics.median(g('game_con_steps_s')), min(g('game_con_steps_s')), statistics.median(g('e2e_ms_p50')), max(g('e2e_ms_max')),
              statistics.median(g('frames_never_delivered')), lost, statistics.median(g('ring_drops')), statistics.median(g('enet_cap_refused')),
              statistics.median(g('enet_cap_admitted')), statistics.median(g('waiting_now')) / 1024.0, sum(1 for r in rows if r.get('room_disconnects', 0) > 0),
              (statistics.median(g('soc_send_ms_s')) + statistics.median(g('soc_recv_ms_s'))) * 34.0 / 30.0))
    return lost


for send in (4000, 6250, 8200, 10000, 12000):
    for loss in (0.0, 0.005):
        keys = ['send_us=%d' % send, 'loss=%g' % loss]
        print('a send to the soc service %.2f ms, the WiFi loses %.1f %% of the datagrams' % (send / 1000.0, loss * 100))
        if old:
            rows = rows_of(old, keys)
            lost = line('before', rows)
            if send in (6250, 8200):
                hold(lost >= 5, 'before the remedy the game was expected to lose the other player in at least 5 runs of 6 (the dead end), it did in %d' % lost)
                hold(statistics.median([r.get('frames_never_delivered', 0) for r in rows]) > 1000, 'before the remedy frames were expected to be never handed over')
                hold(all(r.get('waiting_now', 0) >= LIMIT or r.get('room_disconnects', 0) > 0 for r in rows),
                     'before the remedy every run was expected to end with the waiting data at the limit, or with the room having given the console up')
        line('this tree, no relief', rows_of(new, keys))
        rows = rows_of(new, keys + RELIEF)
        lost = line('this tree, the relief', rows)
        hold(lost == 0, 'with the relief no side loses the other')
        hold(min(r.get('game_con_steps_s', 0) for r in rows) >= STEPS_MIN and min(r.get('game_azh_steps_s', 0) for r in rows) >= STEPS_MIN,
             'with the relief the game makes at least %.0f steps a second on both sides' % STEPS_MIN)
        bound = BOUND_MS if send <= 10000 else BOUND_SLOW_MS
        hold(max(r.get('e2e_ms_max', 1e9) for r in rows) <= bound, 'with the relief no frame of the room reaches the game later than %.0f ms' % bound)
        hold(all(r.get('frames_never_delivered', 1) == 0 and r.get('ring_drops', 1) == 0 and r.get('enet_cap_refused', 1) == 0 for r in rows),
             'with the relief every frame is handed over, the socket loses nothing and the limit is never reached')
        hold(all(r.get('same_dropped', 0) > 0 and r.get('burst_extra', 0) > 0 for r in rows), 'the two reliefs were at work')

# Shown, not held: a party of more than two. The game asks for 3 clients (four players); the relief works on what the console SENDS, and the room's
# stream to the console grows by 180 frames a second for every other player (game_players: each one sends what the modelled one does, a stand-in).
# Everything above is a party of two; what this says is where that stops holding, in the model.
print('a party of more than two players (shown, not held: every other player is a copy of the modelled one, never seen on a console)')
for players in (3, 4):
    for send in (4000, 6250, 8200):
        keys = ['send_us=%d' % send, 'game_players=%d' % players]
        print('%d players, a send to the soc service %.2f ms' % (players, send / 1000.0))
        line('this tree, no relief', rows_of(new, keys))
        line('this tree, the relief', rows_of(new, keys + RELIEF))

if old:
    a, text_a = run(old, [], SAMPLE)
    b, text_b = run(new, [], SAMPLE)
    common = [l for l in text_b.splitlines() if l.split('=', 1)[0] in a or not l.startswith(('enet_cap_', 'enet_waiting_', 'enet_stream_'))]
    same = text_a.splitlines() == common
    print('a game that works (the sample trace): %s' % ('the two models give the same %d lines' % len(common) if same else 'the two models DIFFER'))
    hold(same, 'the sample trace gives the same output before and after')
    hold(b.get('enet_cap_refused', 1) == 0 and b.get('enet_cap_admitted', 1) == 0, 'the sample trace never reaches the limit')

print('%d failures' % failures)
sys.exit(1 if failures else 0)
