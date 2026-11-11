# The latency of the other players

What it is: `Sources/azl/latency.cpp` (`azl::latency`). It reads the packets the game itself sends and receives over UDS and keeps, for every
other node, the round trip. It sends nothing and needs nothing on the other consoles. The value is what the player feels: it includes the wait
for the other game's next frame (about one frame at 30 fps), so on a LAN it is ~30 ms, not 1 ms.

## When it runs

**Always, from the moment the console joins a lobby** (hosts, connects or watches a network; the engine's status is asked at most 4 times a second).
Before that there is no ring and a game thread pays one load and one branch per command. The log (`debug=1`) only decides what is *written*, not
whether the estimate is kept: `latency::Snapshot()` gives the values to whoever needs them (the chat shows them, see below).

With the log on (`debug=1`) the module writes **its own file, `/3ds/3DSLink/latency.log`** (not `AzaharLink.log`), about once a second and only when
something changed. **Every line begins with the time of day of the console's own clock**, `hh:mm:ss.mmm` (hours:minutes:seconds.milliseconds), then the text:

```
12:20:31.482 lat: ---- the console joined a lobby, console clock 2026-11-11 12:20:31.482 ----     (once per lobby: the only line with the date)
12:20:32.490 lat: nodes sending 6     nodes 1 and 2 are sending packets (mask)
12:20:32.490 lat: n2 "Bob" 37 ms (24 samples, M)  round trip to node 2 (the same name as on the screen: the room's nickname of the player, else its UDS username in plain ASCII; no address of any console is ever written): median of the recent samples; M measured, E estimated from the game clock, S old
12:20:32.490 lat: protocol Pia,2009,0,0,lock=Pia  protocol seen (Pia / PRUDP v1 / unknown), the counts, the lock; for an unknown kind also ",UR,<size>,<first bytes>"
(no CPU line: the module has no counter of its own - 3DS Link's own `stats:` line already measures the CPU of every thread, ours included: the network thread's "queue+tick",
the game threads' "uds" and "chat osd")
```

How the file is written: the network thread only *queues* each line (`latency::LatencyFile::Append`: no card access, no wait on a busy lock); the log writer
thread, which already holds the Old 3DS swap gate, writes the queue (`Flush`, one write and one flush) in the same pass as the main log. Nothing is created while
the log is off. At 8 MB the file becomes `latency.1.log` (the one before it goes) and a fresh `latency.log` begins. The clock is `osGetTime()`, the one the
main log's header already prints (the time the user set on the console; no time zone), so the lines can be set against the PC's clock.

## On the screen, while you write

**Only while the chat's keyboard is open** (L+Left+Select opens it, B / Select / "Close" on the touch screen close it; nothing appears when a message comes in) the other
players' latency is drawn too, in the same black blocks as the chat's and the plugin's notifications.

**Colours of the players.** Every player has the colour of his **node** in the match, from the moment he comes in, whatever his name: **1 light blue (the host), 2 green, 3 yellow,
4 orange, 5 red, 6 purple, 7 white, 8 fuchsia.** It is the colour of his messages in the chat (the bar and the name), of his latency line and of the little bar under his tile, so
the same colour is the same person everywhere. (The chat finds a message's node through the room: nickname -> member -> MAC address -> node of the engine; the address is only used
to find the player and is never written anywhere. A message whose player cannot be placed - the match has not given him a node yet - keeps the old colour, from the hash of his nick.)

**Up to four players in the lobby** (3 others: Monster Hunter, Final Fantasy Explorers, Mario Party ...): one line a player, with his name, in the style of a message of the chat (no black strip behind it:
the game shows through, and the letters have a 2-pixel black shadow):

```
| [H] Ana:28ms        the host (node 1) is marked "[H] " before his name (cut to 5 characters); bar and name in the colour of the node, the value in the colour of the latency scale.
| Bob:64ms            "~" before an old value (no recent sample), "--" = no value yet (grey).   "Kamisama" (8 characters) is shown whole.
| Carlo Ma.:120ms     a name of more than 8 characters is cut: the first 8 and a "." (the room's nickname is up to 20; latency.log has the whole name).
```

The same rule - **8 characters of a nick at most, then a "."** - is applied to the messages of the chat (`Carlo Ma.: ciao`). No space after the colon in the latency lines, nor between the number and "ms".

Name = the nickname the room lists for that player - the very name the chat writes - found through the node's MAC; if the room has no such player, the console's UDS username when it
is plain ASCII, else P<node>.
**More than four** (a Tetris-like game takes up to 8 in the lobby): the names are dropped and the others become small **tiles** side by side with no black between them, in **three rows: 3, 2 and 2**
(4 players: 3 and 1; 5: 3 and 2; 6: 3, 2 and 1), as high as the three blocks of names and as narrow as they are - never including yourself: each tile has the colour of that player's **latency**, with an **H** in black on it for the host (node 1) and no number on the others, and **under it a small bar in the colour of
his node** (the colour of his messages in the chat): that colour tells who is who. Grey tile = no value yet. With 5 in the lobby there are 4 tiles, with 8 there are 7. `latency.log` always has every player, with his
name and value, whatever the screen shows.

* **Latency colours:** blue 0-30 ms, green 31-60, yellow 61-100, orange 101-150, red over 150, grey with no value.
* **Where:** in one of the two corners that are free (the chat has one, the plugin's notifications the one across): **the one beside the notifications, on the same row**
  (`chat_pos ^ 2`: chat bottom-left -> notifications top-right -> latency top-left; chat top-right -> notifications bottom-left -> latency bottom-right ...). That is the half of the
  screen the chat does not use, and three blocks (48 rows) are all it takes: even five messages of three lines (180 rows) cannot reach it, so nothing of the chat ever covers
  it and nothing has to be layered over anything. It turns with the chat's position setting. When a notification of the plugin is on screen (5 s) the stack starts under (or above)
  it, never over it.
* **Live:** the drawing tells the network thread it wants the lines (`Overlay::lat_wanted`); the network thread builds them 4 times a second from
  `latency::Snapshot` and the names (`azl/chat_latency.hpp`) and replaces the whole picture under one sequence number (`ChatRing::Replace`: a reader
  never sees a mix). When the keyboard closes nothing is built any more and the picture is emptied; a picture older than 2 s is not drawn.
* A game's thread pays one atomic store a frame while the keyboard is open, and nothing otherwise.
* **No black behind the chat's lines - a shadow instead, and no gap between messages** (the user's decision, 2026-11-11): the black strip behind the chat's lines and the latency's blocks is gone, so the
  game shows through everywhere; the letters get a black shadow (the pixel to the right of each pixel of a letter, the one below it and the one right-and-below) and the coloured bar at the left of a
  line is kept. It is done by WRITING only - the framebuffer is never read, as before - and costs far less than the opaque black (the test counts a third of the bytes or less: only the letters,
  their shadows and the bars are written). The messages touch each other (no row between two blocks: they take less of the screen). The plugin's own notifications keep the opaque black strip.
  Limit: on a very bright scene (snow, a white room) white letters with a shadow read less well than on the black strip.
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
* **On a console:** not measured by us: the figures are in 3DS Link's own `stats:` line (see above), which already counts this module's CPU inside the network thread and the game threads.

## Limits

* The value depends on the game's frame cadence; in a Monster Hunter 4 Ultimate mission it read 10-20 ms above the delivery delay.
* Between two clients in Final Fantasy Explorers the clock method can be off by up to ~15 ms (the two clocks are not exactly aligned).
* A protocol the game encrypts or does not speak in a known way gives no value.
* Not tried on a console. Tested in Docker: `Tests/latency_test.cpp` (not in a lobby, log off, Pia, PRUDP v1, unknown, a burst bigger than the ring),
  the whole `Tests/run.sh` (the sanitizer start-up problems of this PC are in TEST_ENVIRONMENT_REPORT.md) and `Tests/hle_tests.sh`.
