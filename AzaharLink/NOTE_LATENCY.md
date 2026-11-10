# The latency of the other players

What it is: `Sources/azl/latency.cpp` (`azl::latency`). It reads the packets the game itself sends and receives over UDS and keeps, for every
other node, the round trip. It sends nothing and needs nothing on the other consoles. The value is what the player feels: it includes the wait
for the other game's next frame (about one frame at 30 fps), so on a LAN it is ~30 ms, not 1 ms.

## When it runs

**Always, from the moment the console joins a lobby** (hosts, connects or watches a network; the engine's status is asked at most 4 times a second).
Before that there is no ring and a game thread pays one load and one branch per command. The log (`debug=1`) only decides what is *written*, not
whether the estimate is kept: `latency::Snapshot()` gives the values to whoever needs them (the chat, later).

With the log on, `AzaharLink.log` gets, about once a second and only when something changed:

```
lat: nodes sending 6                 nodes 1 and 2 are sending packets (mask)
lat: n2 37 ms (24 samples, M)        round trip to node 2: median of the recent samples; M measured, E estimated from the game clock, S old
lat: protocol Pia,2009,0,0,lock=Pia  protocol seen (Pia / PRUDP v1 / unknown), the counts, the lock; for an unknown kind also ",UR,<size>,<first bytes>"
lat: cpu hook 0.05 ms/s (61 packets), tick 0.12 ms/s, total 0.17 ms/s = 0.02% of a core | ring 29 KB, 0 lost     (every 10 s)
```

The `lat: cpu` line is the module's own counter: ARM11 ticks used on the game's threads (the hook: OnSend / OnRecv) and on the network thread
(Tick), per second of wall time. The two clock reads that measure it are inside the figure. It is only counted with the log on.

## How the round trip is found

* **Pia** (magic `32 AB 98 64`, up to version 5.10): every packet carries the sender's timer (bytes 8-9, ms, big endian) and the last
  timer it received from the receiver (bytes 10-11). The time since this console sent that timer is a full round trip with that node
  (NintendoClients wiki, "Pia Protocol"). Games tried: Kirby Battle Royale, Monster Hunter XX and 4 Ultimate, Mario Party: Star Rush,
  Tri Force Heroes, Fantasy Life, Animal Crossing: New Leaf.
* **PRUDP v1** (`EA D0 01` in the first 16 bytes, over UDS): the unreliable DATA "ping" with a stamp (request 79 bytes, reply 91 bytes,
  payload byte 20 = 2 / 3, byte 21 = 9, u16 LE stamp at byte 22; every 1-2 s toward the host), the individual ACKs of PING packets, and
  in Final Fantasy Explorers the game clock of the state packets (one-way delay, also between two clients). Games tried: Final
  Fantasy Explorers, Pokemon X.
* **Anything else** (for example the encrypted PRUDP of Mario Kart 7): nothing is measured; the packets are counted and the first bytes
  of one are logged once, so that a new game can be looked at.

Once the game is known to speak one protocol only that one is checked (32 packets of one kind and fewer than a quarter of the other);
one packet in 64 is classified again, and 8 of the other kind in a row unlock.

## How many packets there are (from our captures, per console, between Azahar instances on one PC)

| Game | sent / s | received / s per node | received with a usable echo / s per node |
|---|---|---|---|
| Fantasy Life, Tri Force Heroes | ~32 | 25-31 | 1-3 |
| Kirby Battle Royale (and Kirby runs) | 21-31 | 20-30 | 1 |
| Monster Hunter 4 Ultimate | 21-24 | 11-12 | 11-12 |
| Monster Hunter XX | ~20 | 10-11 | 9-10 |
| Mario Party: Star Rush | ~12 | 8-10 | 2-3 |

In most Pia games only 1-3 packets a second per node carry a usable echo (the field is 0 or 0xFFFF in the others), so the module looks at
**every** packet: a test that used one in two lost up to 11 ms against the full estimate (Kirby), because it threw away half of the few useful ones.
With so few samples the first value appears about a second after the ring starts and a stable median takes some seconds (the median is over the last 24 samples).

## Where it hooks in

* `ctr/uds_hle.cpp`, `UdsHle::Dispatch`, after a successful PullPacket (0x14) with data and SendTo (0x17): `latency::OnRecv` / `OnSend`. On the game's
  thread: they copy up to 96 bytes into a ring of 256 records (`__sync_fetch_and_add`, no lock, no log, nothing on the stack).
* `ctr/plugin.cpp`, the room's tick callback (the network thread), after `engine_->Tick` and the hub's: `latency::Tick(now_ms, in_session)` creates
  the ring on the first call with `in_session` true, empties it into the estimator (single thread: no locks) and, with the log on, writes the lines.

## Cost

* **Memory:** the ring is 256 x 116 = 29,696 bytes on the heap, made when the console joins a lobby. The estimator's tables are static: **46,376 bytes of bss**
  (the object file's own figure), part of the image whether the module is used or not. The code is ~6.7 KB (the 3gx grew by ~10 KB). Total ~76 KB: ~2 % of the
  plugin's heap (3.7 MB) and ~1.5 % of its 5 MiB block, which is fixed (`MemorySize: 5MiB`): the 76 KB come out of it.
* **CPU, measured in Docker on real captures (x86; only the ratios between the parts mean something):** the hook costs ~36 ns a packet, emptying the ring 7-14 ns
  a packet, the estimates and lines ~2-3 us once a second. At ~60 events a second (two players) that is a few microseconds of CPU a second; scaled to the ARM11 at
  268 MHz (a factor of 40-80, an estimate, not a measurement) and adding one clock read per packet, **about 0.03-0.06 % of a core**.
* **CPU, measured in Azahar with the AzaharPing plugin** (the same estimator on its own thread): 0.1 % for the hook and 0.5 % for the thread, 0.6-0.7 % in all. That figure is
  dominated by that plugin's own loop (15 wake-ups a second, clock calls, log writes), which does not exist here, and its hook figure counted every call to
  PullPacket, empty ones included (~150 a second); here only calls with data are handed over.
* **On a console:** not measured. The `lat: cpu` line is there to measure it with `debug=1`.

## Limits

* The value depends on the game's frame cadence; in a Monster Hunter 4 Ultimate mission it read 10-20 ms above the delivery delay.
* Between two clients in Final Fantasy Explorers the clock method can be off by up to ~15 ms (the two clocks are not exactly aligned).
* A protocol the game encrypts or does not speak in a known way gives no value.
* Not tried on a console. Tested in Docker: `Tests/latency_test.cpp` (not in a lobby, log off, `lat: cpu`, Pia, PRUDP v1, unknown, a burst bigger than the ring),
  the whole `Tests/run.sh` (the sanitizer start-up problems of this PC are in TEST_ENVIRONMENT_REPORT.md) and `Tests/hle_tests.sh`.
