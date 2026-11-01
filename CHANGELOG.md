# Changelog

## 1.0.12 - 2026-11-01

**Mario Tennis Open is played between consoles. A fix for HOME on the Old 3DS (not tried on one). A change so that LEGO Batman 2 also starts with 3DS Link switched off (not tried).**

> Tried on **two New Nintendo 3DS consoles** (the owner's): *Mario Tennis Open* (Europe copy, several modes: it works) and *SEGA 3D Classics Collection* (*Altered Beast*, 10/10). **Not tried on any Old 3DS or Old 2DS**: the fix for HOME was written from Luma3DS's source and tested on a PC only. The change for *LEGO Batman 2* with the main switch off was not tried either. Scores and setups: [Tested games](README.md#tested-games).

### At a glance

| Game or area | Before | Now | Since |
|---|---|---|---|
| *Mario Tennis Open*, local wireless | the game said the Wi-Fi switch was off | the plugin provides the Download Play lobby inside the room, as for the Kirby games, with the console's own friend code seed in it; played between two consoles | 1.0.12 |
| Old 3DS: pressing HOME with the plugin enabled | closed the game | a fix, **not tried on an Old 3DS** | 1.0.12 |
| *LEGO Batman 2* with the main switch off | by reading the code, the framework still stopped the game at launch (never seen on a console) | the plugin rewrites the stub whatever the switch | 1.0.12 (**not tried**) |

### Not fixed

| What | Notes |
|---|---|
| *Monster Hunter XX* closes when a multiplayer match starts (Old 3DS) | reported by users: [issue 3](https://github.com/gionko/3DS-Link/issues/3), [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| *Mario Tennis Open*, USA copy on an Old 3DS: "the Wi-Fi switch is off" after the online option | [issue 2](https://github.com/gionko/3DS-Link/issues/2): not confirmed fixed, not tried on an Old 3DS (the Europe copy plays between two New 3DS) |
| The power button closes the game on an Old 3DS | reported in [issue 2](https://github.com/gionko/3DS-Link/issues/2); it is not known whether the fix for HOME covers it |
| *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) | on one console (a New 3DS XL): [Known limits](README.md#known-limits) |
| *Balloon Fight* (Virtual Console) closes at startup | the plugin framework aborts in Virtual Console games: a known limit |

### Technical notes

<details>
<summary>Mario Tennis Open: why it needed the lobby, and the friend code seed</summary>

**The lobby.** The game's local multiplayer scene starts the Download Play client at once (`Initialize`, `GetMyStatus` every 5 ms, `GetChannels`, a scan, a join) and, for the player who hosts, the Download Play server. 3DS Link replaces the wireless services, so the real service answered "wireless off" and the game showed its message about the Wi-Fi switch. The game is now one of the titles of `kDlpGames` that get the Download Play lobby inside the room (the one made for the Kirby games).

**What the first try showed.** With the lobby alone, the match was found and the match type could be chosen, but at the character selection nothing loaded on either console. Reading the game's memory with Luma's debugger showed both consoles inside the game's own synchronisation: the host waits until the number of players with a valid id, minus one, equals the number of "ready" messages it received. Its table of players held the guest with the right name and **id 0**. The game keeps the players of the lobby in a table and, when its own network starts, zeroes their ids and gives each one back by comparing the friend code seed of the lobby with the seed of the player's `nwm::UDS` node. The lobby used a seed made by the plugin (a hash of the console's MAC), while the game gives `nwm::UDS` the console's real seed (it reads it from config block `0x00090000`, 8 bytes, just before `Initialize`), so the comparison never matched.

**What the plugin does now.** For this game only, it reads that config block at start and announces it as the lobby's seed. The table is `kDlpGames` in `AzaharLink/Sources/ctr/plugin.cpp`: the Europe copy `000400000007C800` was tried; the USA `000400000007C700`, Japan `0004000000064D00` and Taiwan `00040000000B9100` ids come from catalogues and were never seen on a console. The Kirby games, which work with the hash, are unchanged. With the debug log on, the line `dlp: the lobby's friend code seed is the console's own` tells that it happened (**it prints the console's friend code seed: do not post it**); `WARNING: cannot read the console's friend code seed` says the block could not be read.

</details>

<details>
<summary>Old 3DS: the fix for HOME (not tried on an Old 3DS)</summary>

**Why the game fell.** On an Old 3DS, Luma saves the plugin's memory to a file and frees its 5 MiB when the game goes to the HOME menu. The kernel raises that event on the **game's own thread**, at the end of its `APT:ReceiveParameter`, and that thread sleeps in the kernel until the swap is finished. If the system call was made by the plugin's hook, the thread wakes up in memory that is no longer there (the crash of the report: PC in `svcSendSyncRequest`, LR in the plugin's hook).

**What the plugin does.** On an Old 3DS only, and right after a HOME seen (10 s window), that one call returns to its call site and the game makes the system call from its own code. At the start of the swap all the call sites go back to the game and the threads already inside the hook are waited for (up to 1.5 s; Luma gives 10). At its end the sites re-enter the hook. On a New 3DS nothing of this happens (there is no swap event). The cost for every call of the game is a counter and a comparison.

**What was checked, and what was not.** The logic of the sites, the real entry assembly in an ARM11 emulator and the compiled plugin were tested on a PC, with ThreadSanitizer and mutants. **Not checked**: that Luma behaves on a real console as its source says (read, not run), what the other threads of the game do in the short time between the start of the swap and the suspension, and that the game's APT session is always the one the plugin tracks. If it does not hold, the behaviour stays the one of before (the game closes). With the debug log on, a successful HOME shows `hook swap: ... call sites given back`, then `SWAP_EXIT`.

</details>

<details>
<summary>LEGO Batman 2 with the main switch off, and the bound test</summary>

With the main switch off, Luma still loads the plugin, the framework ran and stopped the game. The rewrite of the stub (see 1.0.11) now runs before the hooks whatever the switch; it reads the game's `.text` and writes one word. In the search, `stub + 8 > end` could overflow for a `bl` aimed at `0xFFFFFFFC` (never seen in a game); the test is now `stub > end - 8` and `AzaharLink/Tests/gsp_wrapper_test.cpp` tries six targets around the top of the address space.

</details>

<details>
<summary>About the files of this release</summary>

`3DSLink.cia` is the very file that was tried on the consoles (same bytes); `3DSLink.3dsx` was built in the same run, from the same plugin, and was not installed on its own. *SEGA 3D Classics Collection* was tried on an earlier build of 1.0.12, before the *Mario Tennis Open* change, which only touches that game's four title ids.

</details>

## 1.0.11 - 2026-10-31

**LEGO Batman 2: DC Super Heroes no longer closes at launch.** Nothing else changed since 1.0.10.

> Tried on **two New Nintendo 3DS consoles** connected through an Azahar room, with the plugin switched on: the game starts and plays. It was **not tried on an Old 3DS, nor with the USA copy**: the report of the same symptom ([issue 2](https://github.com/gionko/3DS-Link/issues/2)) comes from an Old 3DS with the USA copy, and it is not known whether it has the same cause. The logs of that test are still to be read.

> The fix is part of the plugin's hooks, so it works **when 3DS Link is switched on** (main switch on). With the main switch off the plugin file stays in the game's folder and the framework still stops this game: to play it without 3DS Link, take the game off in *Enable Titles*. This is changed in 1.0.12 (not tried yet).

### At a glance

| Game | Before | Now | Since |
|---|---|---|---|
| LEGO Batman 2: DC Super Heroes | a "Fatal plugin error" screen (`abort()` called, code `0x0705C108`), then a Luma exception dump | the game starts and plays between two consoles | 1.0.11 |

### Not fixed

| What | Notes |
|---|---|
| Old 3DS: pressing HOME with the plugin enabled closes the game | reported by users, no Old 3DS in the test set; the cause is understood, the fix was not written yet (it is in 1.0.12, not tried on an Old 3DS): [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| *Monster Hunter XX* closes when a multiplayer match starts (Old 3DS) | reported by users: [issue 3](https://github.com/gionko/3DS-Link/issues/3) |
| *Mario Tennis Open*: after the online option the game says the Wi-Fi switch is off (Old 3DS) | reported by a user, not analysed at the time: [issue 2](https://github.com/gionko/3DS-Link/issues/2). Local play between two consoles works since 1.0.12 (Europe copy, two New 3DS); whether this report was the same message is not confirmed (the "online option" may also be the game's internet mode, which 3DS Link does not carry) |
| *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) | on one console (a New 3DS XL): [Known limits](README.md#known-limits) |
| *Balloon Fight* (Virtual Console) closes at startup | the plugin framework aborts in Virtual Console games: a known limit |

### Technical notes

<details>
<summary>Why LEGO Batman 2 closed, and what the plugin does now</summary>

**Why.** The plugin framework (CTRPluginFramework) hooks the game's GSP function `GSPGPU_RegisterInterruptRelayQueue`, which it finds in the game's code by instruction pattern. If it does not find it, it calls `abort()` before the game starts: a "Fatal plugin error" screen, then a Luma exception dump (no log is written, because the plugin's log opens later). In LEGO Batman 2 the function is there and equals the pattern except for one word: its `svc 0x32` is not inline but behind a `bl` to a stub of the game, which is exactly `svc 0x32 ; bx lr`.

**What the plugin does.** At start, before the framework searches, the plugin looks in the game's code for that function with a `bl` to such a stub (the other 20 words equal to the pattern, the condition "always", the target inside the game's code) and writes the `svc 0x32` in place of the `bl`. For the CPU the two are the same (the function leaves with `pop {..., pc}`, so the `lr` that `bl` changes is not used, and `svc` does not touch the flags). The framework then finds a function it knows and hooks it as in any other game. The code is `AzaharLink/Includes/azl/gsp_wrapper.hpp` and `InstallSvcHooks` in `AzaharLink/Sources/ctr/hook.cpp`; the test is `AzaharLink/Tests/gsp_wrapper_test.cpp`.

**Other games.** The rewrite only happens when the other 20 words equal the pattern and the 12th is a `bl` to exactly `svc 0x32 ; bx lr`. It was looked for in the code of *Final Fantasy Explorers*, *Kirby Battle Royale*, *Kirby: Planet Robobot* and *Kirby: Triple Deluxe* and found nothing there, so those games are not touched. With the debug log on, the log line `hook: the GSP wrapper at 0x... calls its svc through a stub` tells that it happened.

</details>

<details>
<summary>About the files of this release</summary>

`3DSLink.cia` is the very file that was tried on the consoles (same bytes); `3DSLink.3dsx` was built in the same run, from the same plugin, and was not installed on its own.

</details>

## 1.0.10 - 2026-10-31

The first release after 1.0.4.

- Versions **1.0.5 to 1.0.9** (28 October 2026) were test builds for the owner's consoles and **were never released**. There is no tag for them: their sources were not kept one by one.
- Everything they brought is in 1.0.10. The tables below say **since which version** each change is there.
- The build that was tried on the consoles is of 30 October.

> What was tried on 1.0.10 was tried only on the owner's consoles and Azahar instances, with the debug log **off** and without logs. Items marked **not tried** were not tried on a console. Scores and setups: [Tested games](README.md#tested-games).

### At a glance

| Area | What changed |
|---|---|
| **Kirby: Triple Deluxe, Planet Robobot** | no longer close at launch (1.0.6); played in the room through a Download Play lobby that the plugin provides (1.0.10): **between consoles only**, tried with 2 consoles |
| **Kirby Battle Royale** | no longer asks for the wireless to be turned on (1.0.6; last tried on 1.0.6) |
| **All games** | room data path (ENet) changed in five places, gain worked out on recorded traces and **not measured** (1.0.5); frames sent to one node go to that node (1.0.5); plugin +41,668 bytes (+4.1%) compared with 1.0.4 |
| **Not fixed** | Old 3DS (HOME crash, *Monster Hunter XX*), *LEGO Batman 2*, three 2011 titles (`0xE0A01839`) |

### Games

| Game, and what changed | Before | Now | Since | Tried on 1.0.10 |
|---|---|---|---|---|
| Kirby: Triple Deluxe and Planet Robobot, **launch** | closed at launch | start normally | 1.0.6 | yes |
| Kirby: Planet Robobot, **wireless play** | none (the game looks for its group with Download Play) | in the room, up to 4 players, consoles only (Azahar cannot take part) | 1.0.10 | 2 consoles: 9/10. 3–4 players, Azahar player: **not tried** |
| Kirby: Triple Deluxe, **wireless play** | none (as above) | in the room, up to 4 players, consoles only (Azahar cannot take part) | 1.0.10 | 2 consoles: 9/10, found a little slow (cause not known). 3–4 players, Azahar player: **not tried** |
| Kirby Battle Royale, **wireless check** | asked for the wireless to be turned on | the plugin answers the game's wireless check; the game goes on to the normal wireless | 1.0.6 | not again; tried on 1.0.6 (debug log on) |
| Kirby Battle Royale, **Azahar player closed abruptly** | the console waited about 30 s | *intended*: dropped after 3 s (see "Room members that stopped playing" below) | 1.0.10 | **not tried** |

### All games

| Change | What it does | Since | On a console |
|---|---|---|---|
| Room data path (ENet), five changes | skip an empty read, read passes of 32 datagrams, acknowledgement after delivery, earlier `CONNECT` resend, ping after a silence (open the list below) | 1.0.5 | gain worked out on recorded traces, **not measured** |
| Frames with the broadcast flag to one node | go to that node's MAC (still unreliable, never doubled); matters for *Heroes of Ruin*, *Final Fantasy Explorers*, *Battle Royale* | 1.0.5 | *Final Fantasy Explorers* tried |
| Room members that stopped playing | a member that stays in the room but says it plays nothing, or whose game information is empty (no title name, id 0), is dropped from the game's session after 3 s, as if it had left | 1.0.10 | **not tried** on a console |
| UDS engine split into shared functions | `Initialize`, `BeginHostingNetwork`, `ConnectToNetwork` also serve the lobby; nothing should change for the games that do not use the lobby | 1.0.10 | *Final Fantasy Explorers*, *Tetris*, *Super Street Fighter IV*, *Monster Hunter 3 Ultimate*, *Tekken* tried |
| Plugin size | 1.0.4 → 1.0.10: 1,011,465 → 1,053,133 bytes (+41,668: about 31,700 with the lobby of 1.0.10, the rest from 1.0.5 to 1.0.8) | 1.0.5–1.0.10 | — |

<details>
<summary>The five changes of the room data path (1.0.5)</summary>

- The read skips a socket that was just found empty.
- A read pass stops after 32 datagrams and delivers them.
- The acknowledgement is sent after the packet reaches the game (held for at most 8 packets or 8 ms).
- A lost `CONNECT` is resent after 100, 300, 700 ms (not 500, 1500, 3500).
- A ping is sent when the room speaks again after a silence of at least 0.5 s.

</details>

### What you see

| Part | Item | Since |
|---|---|---|
| Plugin | A second notification after joining the room: `AzaharLink <version> (built <stamp>)`, at every start, with the debug log off too | 1.0.8 |
| Plugin | 25 s after the start it compares its copy with the one in `/3ds/3DSLink/` (it reads up to 2 MB from the card, debug log off too). If that one is a newer version: "3DS Link has a newer plugin. Open 3DS Link to update your games." | 1.0.8 |
| Plugin | The same comparison, same version but other bytes: a warning that this game's plugin is not the one in 3DS Link | 1.0.8 |
| App | Checks the plugin copies in the games' folders as soon as it starts (without waiting for the title scan) and recognises the plugin by its content (size and hash) instead of size and date | 1.0.6 |
| App | Says "Updating the plugin in your games: N of M folders done. Please wait." then "You can close 3DS Link."; START during the pass asks "Leave anyway?" (*Leave* leaves the games not yet reached with the old plugin) | 1.0.8 |

### Debug log

| Item | Since |
|---|---|
| The capture is written to the card every 2 s, or at 160 KB, instead of four times a second. **It did not solve the problem**: with the debug log on, the Kirby games do not load or freeze, and the other consoles drop out. Keep it off | 1.0.8 |
| New line `dlp:` (which Download Play service the game uses) | 1.0.6 |
| New lines `dlpc:` (every Download Play command) | 1.0.10 |
| New line `hid:` (whether the plugin keeps `ir:rst` open), ENet counters, and the build time in the header is now the real one | 1.0.5 |
| `probe:` lines: 25 s after the start the plugin asks AM for the installed 3DS Link version and tries to read its icon (**never tried on a console**) | 1.0.8 |

### Not fixed

| What | Notes |
|---|---|
| Old 3DS: pressing HOME with the plugin enabled closes the game | reported by users, no Old 3DS in the test set; the cause is understood, the fix is not written yet: [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| *Monster Hunter XX* closes when a multiplayer match starts (Old 3DS) | reported by users: [issue 3](https://github.com/gionko/3DS-Link/issues/3) |
| *LEGO Batman 2* closes at launch | an abort of the plugin framework; not fixed in 1.0.10, fixed in 1.0.11 (see above) |
| *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) | on one console (a New 3DS XL): [Known limits](README.md#known-limits) |
| *Balloon Fight* (Virtual Console) closes at startup | the plugin framework aborts in Virtual Console games: a known limit |

### Technical notes

<details>
<summary>The Download Play lobby of the two Kirby games</summary>

**How it works.** These games find and create their group with the Download Play service (`dlp:FKCL` to look for it, `dlp:SRVR` to create it), which runs in the system with the real radio, out of the plugin's reach. The plugin now answers both services: the announcement of the group and the *Start* messages travel in the room like the game's own frames, and when the game opens its UDS network the lobby gives the engine back. It is a virtual UDS network between two plugins, so **Azahar cannot take part in it**; at most four players.

**Which titles.** Only the titles in the table `kDlpGames`: Triple Deluxe `000400000010C000`, Planet Robobot `0004000000183600` (`000400000010BF00` comes from a catalogue and has never been seen).

**Where is the code.** In `AzaharLink/Includes/azl/dlp_*.hpp` and `AzaharLink/Sources/azl/dlp_*.cpp`. The design notes are the author's working notes and are not in this repository.

</details>

<details>
<summary>How the launch crash of the two Kirby games and the Battle Royale check were fixed (1.0.6, 1.0.7)</summary>

**Startup guard (1.0.6).** The framework searches the game's code at startup, and in Triple Deluxe and Planet Robobot the repeated search ran past the end of the code (`.text`). A linker `--wrap` of `Process::GetTextSize`, at that call site only, cuts the search to the real code; it does nothing in other games.

**Battle Royale (1.0.6).** The game calls `dlp:FKCL` even for a local match; the plugin answers the "wireless on" check. Only for `00040000001C2100` (`00040000001C2000`, another region, is in the table and has never been seen).

**Correction (1.0.7).** In 1.0.6, Triple Deluxe and Planet Robobot were in Battle Royale's list and showed the error `0xE0A0AC01`; 1.0.7 took them out.

</details>

<details>
<summary>About the files of this release</summary>

`3DSLink.cia` and `3DSLink.3dsx` are built from this tree. The plugin inside them is the very file that was tried on the consoles (same bytes); the app and the CIA were built again from the same sources, so they are not byte for byte the ones that were tried: the `.3dsx` differs from the tried one in 570 of its 2,423,468 bytes (random-looking bytes, which the CIA tool draws at each run for tickets and signatures, and a build date), and the `.cia`, whose contents are compressed and signed, in about 30% of its bytes (same size).

</details>

## 1.0.4 - 2026-10-25
- Fixes the C-stick not working in a game while the plugin is in its folder (issue #1, Monster Hunter 4 Ultimate; also seen in Monster Hunter 3 Ultimate). On a New 3DS the plugin framework opened the `ir:rst` service (C-stick, ZL, ZR), which has only two sessions. The plugin no longer opens it.
- ZL and ZR can no longer be part of the in-game menu key combination (they cannot be read any more).
- The in-game menu is now off by default (`menu_key=off`); an existing ini keeps its value, and the app's *In-game menu* switch puts `L+R+Select` back.
- Tested: Monster Hunter 3 Ultimate in a two-console room, Monster Hunter 4 Ultimate in single player.

## 1.0.3 - 2026-10-23
First public release.
