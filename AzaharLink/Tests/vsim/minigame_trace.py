# (plugin-causa, 2026-11-14) A synthetic trace of the minigames of Mario Party: Island Tour hosted by a console, for the model (Tests/vsim): the
# room's stream to the console as the last windows of the session of 2026-11-14 22:50 show it (600-950 frames and 250-425 KB every 5 s: about
# 130-190 frames and 50-85 KB a second), with a share of large frames that ENet sends in 2-3 fragments (1400-2900 B), and the console's own
# frames (60-90 KB a second, up to a datagram of 1392 B). No byte of a game is in it: sizes and times only, drawn from a seeded generator.
#   python3 Tests/vsim/minigame_trace.py <out.trace> [seconds=70] [seed=1] [room_fps=160] [big_share=0.08] [con_fps=80]
# (the defaults: the windows 911-933 s of that session - the room's 120-190 frames a second of 425-450 B on average, the console's 46-95 of
# about 650 B, up to 1392)
import random
import sys

out = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 70.0
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
room_fps = float(sys.argv[4]) if len(sys.argv) > 4 else 160.0
big_share = float(sys.argv[5]) if len(sys.argv) > 5 else 0.08
con_fps = float(sys.argv[6]) if len(sys.argv) > 6 else 80.0
rnd = random.Random(seed)
lines = []
calm = 5.0  # the first seconds: the board, a light stream


def frames(fps, t0, t1):
    t = t0
    while t < t1:
        yield t
        t += rnd.expovariate(fps) * 0.5 + 0.5 / fps  # (half regular, half Poisson: a game sends on its own clock, the WiFi bunches them)


for t in frames(60.0, 0.0, calm):
    lines.append((t, "R %d %d" % (int(t * 1e6), rnd.randint(120, 260))))
for t in frames(room_fps, calm, seconds):
    size = rnd.randint(1400, 2900) if rnd.random() < big_share else rnd.randint(200, 500)
    lines.append((t, "R %d %d" % (int(t * 1e6), size)))
for t in frames(60.0, 0.0, calm):
    lines.append((t, "T %d %d 0" % (int(t * 1e6), rnd.randint(120, 260))))
for t in frames(con_fps, calm, seconds):
    size = rnd.randint(1000, 1392) if rnd.random() < 0.15 else rnd.randint(300, 900)
    lines.append((t, "T %d %d 0" % (int(t * 1e6), size)))
lines.sort(key=lambda x: x[0])
with open(out, "w") as f:
    f.write("\n".join(l for _, l in lines) + "\n")
