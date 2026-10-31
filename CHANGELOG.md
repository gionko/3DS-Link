# Changelog

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
| Kirby: Triple Deluxe, **wireless play** | none (as above) | in the room, up to 4 players, consoles only (Azahar cannot take part) | 1.0.10 | 2 consoles: works, a little slow (cause not known). 3–4 players, Azahar player: **not tried** |
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
| Old 3DS: pressing HOME with the plugin enabled closes the game | reported by users, no Old 3DS in the test set: [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| *Monster Hunter XX* closes when a multiplayer match starts (Old 3DS) | reported by users: [issue 3](https://github.com/gionko/3DS-Link/issues/3) |
| *LEGO Batman 2* closes at launch | an abort of the plugin framework; cause identified, fix not in 1.0.10: [Known limits](README.md#known-limits) |
| *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) | on one console (a New 3DS XL): [Known limits](README.md#known-limits) |

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
