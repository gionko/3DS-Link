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
12:20:32.490 lat: n2 "Bob" 37 ms (30 s: min 31, p90 52; 290 samples, M)
                                                  round trip to node 2 (the same name as on the screen: the room's nickname of the player, else its UDS username in plain ASCII; no
                                                  address of any console is ever written). 37 = the value the screen shows: the median of the last 30 s. min / p90 = the lowest and
                                                  the 90th percentile of the same 30 s, samples = how many measures they are made of. The last letter says what the measures are:
                                                  M measured (the game's own echo), A acknowledgements with the other game's hold time removed (PRUDP), E estimated from the game
                                                  clock ("(2 x 18 ms one way, from the game clock, E)"), S an old value ("(old value, S)": the node is there but gives no measure now)
12:20:35.000 lat: room link 6+-4 ms, loss 5/1000  THIS console's own link to the room, as 3DS Link itself measures it (the same figures as "stats: link rtt" in AzaharLink.log): round
                                                  trip +- its variance, reliable packets lost per thousand. Written when it changes, and every 30 s
12:20:32.490 lat: protocol Pia,2009,0,0,lock=Pia,ack=0,tk=0,sf=0  protocol seen (Pia / PRUDP v1 / AckTag / Tekken / SSF4 / unknown), the counts (Pia, PRUDP, other), the lock and the packets of the three small layers; for an unknown kind also ",UR,<size>,<first bytes>"
12:21:07.120 lat: ring dropped 12 packets in all (the network thread paused: they gave no sample)     only if it happens: packets the module could not look at (never a wrong value)
(no CPU line: the module has no counter of its own - 3DS Link's own `stats:` line already measures the CPU of every thread, ours included: the network thread's "queue+tick",
the game threads' "uds" and "chat osd")
```

How the file is written: the network thread only *queues* each line (`latency::LatencyFile::Append`: no card access, no wait on a busy lock); the log writer
thread, which already holds the Old 3DS swap gate, writes the queue (`Flush`, one write and one flush) in the same pass as the main log. Nothing is created while
the log is off. At 8 MB the file becomes `latency.1.log` (the one before it goes) and a fresh `latency.log` begins. The clock is `osGetTime()`, the one the
main log's header already prints (the time the user set on the console; no time zone), so the lines can be set against the PC's clock.

### Reading the file: whose connection is it?

* **A player's value and p90 both high, the others' low, `room link` low and with no loss** -> that player's connection (or the path to him), not this console's.
* **Every player's value high and `room link` high or losing packets** -> this console's own connection to the room.
* **Value steady, p90 far above it** -> the connection jumps (the median hides single bad seconds on purpose: the p90 is where they show).
* **`ring dropped` lines, or few `samples` for a game that usually gives many** -> the plugin's network thread was held up on this console: a plugin / console matter, not the network.
* **`S` or `--` for a node while the game goes on** -> the game gives no measure for that node now (some PRUDP games only talk to the host); not a fault.

## How good the value is (checked against the real delay)

Three Azahar instances on one PC share one clock, so the real delay of every packet is known: the same bytes are seen leaving one instance and being pulled by another
(`truth = median there + median back`). The captures were replayed through this very file's code (`Sources/azl/latency.cpp`, linked unchanged, on the capture's clock) and what
`Snapshot()` would show was taken once a second: 36 pairs of players in Monster Hunter XX, Monster Hunter 4 Ultimate, Kirby Battle Royale, Fantasy Life, Tri Force Heroes and Mario
Party: Star Rush; 20 pairs in Final Fantasy Explorers and Pokemon X.

| | typical error | 90 % of the seconds within | worst undershoot | change from one second to the next (95th percentile) |
|---|---|---|---|---|
| Pia, before (median of the last 24 samples) | +0.9 ms | -4.8 .. +8.8 ms | -19 ms | 4.6 ms |
| Pia, now (median of 30 s of half-seconds) | +0.1 ms | -3.8 .. +5.7 ms | -10 ms | 1.9 ms |
| PRUDP v1, before | +6.8 ms | +2.8 .. +13.0 ms | -26 ms | 1.4 ms |
| PRUDP v1, now | +6.6 ms | +3.0 .. +12.9 ms | -26 ms | 1.5 ms |

Monster Hunter 4 Ultimate is where the old value moved most on a network that did not change (-10 .. +26 ms around the truth, 12 ms from one second to the next); now -7 .. +15 and 2 ms.
Known offsets that stay: **Kirby Battle Royale between two clients reads ~25 ms high** (their packets pass through the host's frame); **a PRUDP ping toward the host reads
15-27 ms high** (the host answers at its next frame); the PRUDP `A` values rest on an assumption about the other game's hold time. The tools and the outputs are kept with the
project's audit (`replay_real.cpp`, `compare_real.py`, `real_code_vs_truth*.txt`).

## On the screen, while you write

**Only while the chat's keyboard is open** (L+Left+Select opens it, B / Select / "Close" on the touch screen close it; nothing appears when a message comes in) the other
players' latency is drawn too, in the style of the chat's lines (a coloured bar and letters with a shadow, nothing behind them).

**Colours of the players.** Every player has the colour of his **node** in the match, from the moment he comes in, whatever his name: **1 light blue (the host), 2 green, 3 yellow,
4 orange, 5 red, 6 purple, 7 white, 8 fuchsia.** It is the colour of his messages in the chat (the bar and the name), of his latency line and of the little bar under his tile, so
the same colour is the same person everywhere. (The chat finds a message's node through the room: nickname -> member -> MAC address -> node of the engine; the address is only used
to find the player and is never written anywhere. A message whose writer is NOT a player of the match this console is in - somebody in the room who is not playing, or not yet -
is **grey**, never the colour of a node: the old colours of the nicks' hash are the same five as nodes 1, 2, 3, 4 and 6, and he would look like one of the players the latency lists.
While this console is in no match nobody has a node and the nicks keep the colours of their hash, as 3DS Link always did.)

**Up to four players in the lobby** (3 others: Monster Hunter, Final Fantasy Explorers, Mario Party ...): one line a player, with his name, in the style of a message of the chat (no black strip behind it:
the game shows through, and the letters have a 2-pixel black shadow):

```
| [H] Ana:28ms        the host (node 1) is marked "[H] " before his name; bar and name in the colour of the node, the value in the colour of the latency scale.
| Bob:64ms            "~" before an old value (no recent sample), "--" = no value yet (grey).   "Kamisama" (8 characters) is shown whole.
| Carlo Ma.:120ms     a name of more than 8 characters is cut: the first 8 and a "." (the room's nickname is up to 20; latency.log has the whole name).
```

The same rule - **8 characters of a nick at most, then a "."** - is applied to the messages of the chat (`Carlo Ma.: ciao`) and to the host's name (`[H] Carlo Ma.:28ms`: it was
cut to 5 before, so the host's name in the latency was not the name of his messages). No space after the colon in the latency lines, nor between the number and "ms".

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
  it and nothing has to be layered over anything. It turns with the chat's position setting. A notification of the plugin (5 s) is on the same rows in the corner across:
  a short one stands beside the latency; **one of ~40 characters or more reaches it and is drawn over part of it** for its 5 seconds (the notification wins: it is the one
  that says something happened). Measured with the plugin's own drawing code on 101,920 scenes (every corner, 1-7 other players, every mix of 0-5 messages of 1-3 lines, five
  kinds of notification, the framework's own notifications on and off): the chat never covers the latency; the latency takes a message away from the chat only with two of the
  framework's notifications on screen and a chat of 144 rows or more (445 scenes).
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

## When the connection gets worse for a moment (added 2026-11-13)

The value is the median of the window (30 s): steady by construction, which also means a connection that gets worse
for a few seconds would not move it. So the module also looks at **the last 2 s on their own** (the slots of the last 2 s, the open one included) and, while
they are **clearly worse than the window - at least one and a half times it and 30 ms above it -** the screen shows the value of those 2 s instead (50 ms
that become 500 for two seconds read 500 at once). When they are back near the window the window's value returns. A better moment does not move the shown
value: the window does, slowly. `NodeRtt` carries all three (`rtt_ms` = what is shown, `window_ms`, `recent_ms`, and `spike` when the first is the last 2 s),
and latency.log says when it starts and ends; on the chat's screen the value has a `!` after it while it lasts (`Gionko:480ms!`):

```
12:21:03.410 lat: n2 "Bob" 480 ms (30 s: min 31, p90 52; 290 samples, M)
12:21:03.410 lat: n2 spike: the last 2 s are 480 ms, the window 37 ms (shown: the last 2 s)
12:21:06.420 lat: n2 spike over: back to the window's 37 ms
```

The window is 30 s for every game (the games with few samples had 120 s until 2026-11-13: too long to follow). How fast the number can move is the game's: a new sample every half-second slot in the Pia games (30-60 echoes a second), one a second in Street Fighter,
one every 2 s in Tekken, a few a minute in Tetris. The screen is rebuilt 4 times a second while the keyboard is open.

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
With so few samples the first value appears about a second after the ring starts and a stable median takes some seconds.

**The window.** For every node the module keeps its last 60 half-seconds (30 s), each with one value: the median of the samples that came in it (the first 8). What is shown and
logged is the median of those values; the log also has the lowest and the 90th percentile. (PRUDP gives a sample every ~2 s: there the slots are 2 s long and the window 120 s.) It was
"the median of the last 24 samples": 2.4 s in a game that gives ten samples a second, which made the number move on a network that did not. A real change of the connection shows in
the p90 within seconds and in the value after about 15 s.

## Games that speak neither Pia nor PRUDP v1 (Tetris, Tekken, Street Fighter; added 2026-11-12/13)

The module recognises five kinds of packet and keeps a lock on the one a game uses (the "protocol" line of the log says which): Pia, PRUDP v1, and three small layers
of the games' own, found on captures of Azahar instances (Tekken and Street Fighter against a real console, Tetris between three instances). All are measured **from
what the game sends anyway**.

* **Tetris (EUR, "AckTag", flag A).** Every packet starts with a header whose bytes 29-31 are `ack`; byte 28 is the type (0 data, 1 heartbeat, 6 ACK); a reliable message
  has a number at byte 36 and the node that sent it at 38. A reliable data of mine is answered by an ACK from every other node; the round trip is "my data left" ->
  "the ACK of that node arrived", minus the time the other game held it, estimated with the hold of MY OWN ACKs (25 ms until three of them are measured). A message sent
  twice gives no sample (which send does the ACK answer?). Checked on three instances on one PC (the real delay ~33 ms): the pairs' typical errors were -15.6 .. +0.2 ms;
  few samples (2-3 a minute), so the value is slow to appear and, between two samples more than 30 s apart, is shown as an old one.
* **Tekken 3D Prime Edition (EUR, "Tekken", flag M).** Header `02 01 | src | dst | 01 00 | length | 8 zero bytes | 11 11 | type`, size = 16 + length. The 281-byte state
  frames (60/s) carry no echo of the other side's counter. Every 2 s each side sends a SHORT packet (30 bytes in a match, 34 in the lobby), type `0xC4` (`0xC0` in the
  lobby), and the other answers at once with `0x94` (`0x90`): that pair is the measure, one sample every 2 s. On the capture against the real console it
  reads 33 ms (min 16, p90 34). No ground truth exists for a real console: it includes the other game's frame wait.
* **Super Street Fighter IV 3D Edition (EUR, "SSF4", flag A).** Every message: `u16 size-2, u16 size-4, u8 type, u8 flags, u32 packet number, u32 checksum, content`.
  Type 0 is the game's data (60 a second, no echo: the equal values on the two sides come from both simulating the same frame), type 7 a KEEPALIVE once a second
  with the number it acknowledges at 20 and its own number at 22 (sent again every ~110 ms until acknowledged), type 3 the ACK (28 bytes) with the number at 20, which
  the other game sends when it next looks at its network (0-30 ms, a frame at 60 fps). Round trip = my keepalive leaving -> the ACK with its number arriving, minus
  the other game's hold, estimated with my own (16 ms until three are measured), as for Tetris; a keepalive sent again gives no sample. Checked on two instances on one
  PC (real round trip 33.4 ms): the plugin shows 32 ms from the second second on, 90 % of the seconds within -1.4 ms. On 120 s of a real match against a console through a
  public room it shows 66 ms (min 50, p90 83). In the lobby the other game may hold an ACK for long (one sample of 750 ms was seen while it loaded): the value settles
  with the match. One sample a second.

What the three have in common: a message with a number and an answer that carries the number back (Tetris, Street Fighter) or a request and its reply (Tekken); the
estimator, the hold and the window are the same code for all, only the bytes that say "this is the number" differ. What a game does not speak in a known way still
gives no value, and a game that only sends state (no number ever comes back) cannot be measured by listening. The next game is a new rule only if its packets show a
number coming back; the log's `protocol` line (the first bytes of an unknown packet, sent and received) is where to start.

## Where it hooks in

* `ctr/uds_hle.cpp`, `UdsHle::Dispatch`, after a successful PullPacket (0x14) with data and SendTo (0x17): `latency::OnRecv` / `OnSend`. On the game's
  thread: they copy up to 96 bytes into a ring of 128 records (`__sync_fetch_and_add`, no lock, no log, nothing on the stack).
* `ctr/plugin.cpp`, the room's tick callback (the network thread), after `engine_->Tick` and the hub's: `latency::Tick(now_ms, in_session)` creates
  the ring on the first call with `in_session` true, empties it into the estimator (single thread: no locks) and, with the log on, writes the lines.

## Cost

* **Memory** (arm-none-eabi-size of the object file, the plugin's own flags): **7,760 bytes of bss** (the estimator's tables: 8 nodes x 60 half-seconds, the PRUDP
  waiting lists) and a ring of 128 x 120 = **15,360 bytes on the heap**, made when the console joins a lobby: **23.1 KB in all**, plus 10.3 KB of code. Final Fantasy Explorers adds
  10,240 bytes on the heap for its game-clock samples, made the first time such a packet is seen; no other game pays for them. It was 46,608 bytes of bss (48 % of the whole
  plugin's 96,972) and a ring of 30,720: 77 KB. What went: a table of the last 1,024 sends (16 KB; the estimate is the same to the millisecond without it, replayed on seven games),
  the clock samples as static storage (23 KB, now on the heap for the one game that uses them and a third of the size), half of the ring (128 records hold half a second of a lobby of 8, so that the log of a full lobby is whole; the network thread empties it
  every few ms; what a long pause loses is counted and written, and gives no sample - never a wrong one).
* **CPU, measured in Docker on real captures (x86; only the ratios between the parts mean something):** the hook costs ~36 ns a packet, emptying the ring 7-14 ns
  a packet, the estimates and lines ~2-3 us once a second. At ~60 events a second (two players) that is a few microseconds of CPU a second; scaled to the ARM11 at
  268 MHz (a factor of 40-80, an estimate, not a measurement) and adding one clock read per packet, **about 0.03-0.06 % of a core**.
* **CPU, measured in Azahar with the AzaharPing plugin** (the same estimator on its own thread): 0.1 % for the hook and 0.5 % for the thread, 0.6-0.7 % in all. That figure is
  dominated by that plugin's own loop (15 wake-ups a second, clock calls, log writes), which does not exist here, and its hook figure counted every call to
  PullPacket, empty ones included (~150 a second); here only calls with data are handed over.
* **On a console** (New 3DS, Monster Hunter XX against an Azahar on the LAN, the build of 2026-11-11 11:48, from 3DS Link's own `stats:` lines): the hook 0.04 ms and the tick
  0.39 ms of CPU a second - **0.04 % of a core**; the network thread ran ~327 rounds a second with a longest pause of 9 ms; game latency 15-21 ms, room link 6-7 +- 4 ms with
  5 packets lost in 1000. The cost of the shadow text has not been read on a console yet (the `chat: cost per frame` line of the next log).

## Limits

* The value depends on the game's frame cadence; in a Monster Hunter 4 Ultimate mission it read 10-20 ms above the delivery delay.
* Between two clients in Final Fantasy Explorers the clock method can be off by up to ~15 ms (the two clocks are not exactly aligned).
* A protocol the game encrypts or does not speak in a known way gives no value.
* The Tetris, Tekken and Street Fighter rules were found on Azahar captures only (Tekken and Street Fighter against a real console, Tetris between three instances): not yet tried on two real consoles.
* The window, the room link's line and the smaller tables (this revision) have not been tried on a console yet. Tested in Docker: `Tests/latency_test.cpp` (not in a lobby, log off, Pia, PRUDP v1, unknown, a burst bigger than the ring, the window and the room link's line, the Tetris, Tekken and Street Fighter layers, the file),
  the whole `Tests/run.sh` (the sanitizer start-up problems of this PC are in TEST_ENVIRONMENT_REPORT.md) and `Tests/hle_tests.sh`.
