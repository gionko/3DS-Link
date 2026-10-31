# Changelog

## 1.0.10 - 2026-10-31
(the build that was tried on the consoles is of 30 October)
The first release after 1.0.4. The versions **1.0.5 to 1.0.9** (28 October 2026) were test builds for the owner's consoles and **were never released**: there is no tag for them (their sources were not kept one by one), and everything they brought is in 1.0.10. Each item says since which version it is there.

**Games that did not work**
- **Kirby: Triple Deluxe and Planet Robobot no longer close at launch** (since 1.0.6). The plugin framework searches the game's code at startup; in these two games the repeated search ran past the end of the code (`.text`). A guard (a linker `--wrap` of `Process::GetTextSize`, at that call site only) cuts the search to the real code and does nothing in other games.
- **Kirby: Triple Deluxe and Planet Robobot are played in the room, with a Download Play lobby that the plugin provides** (1.0.10). These games find and create their group with the Download Play service (`dlp:FKCL` to look for it, `dlp:SRVR` to create it), which runs in the system with the real radio, out of the plugin's reach. The plugin now answers both services: the announcement of the group and the *Start* messages travel in the room like the game's own frames, and when the game opens its UDS network the lobby gives the engine back. It is a virtual UDS network between two plugins: **Azahar cannot take part in it**. At most four players; only for the titles in the table `kDlpGames` (Triple Deluxe `000400000010C000`, Planet Robobot `0004000000183600`; `000400000010BF00` comes from a catalogue and has never been seen). The design notes (`analysis/dlp-lobby-fake-2026-10-28/`, cited in the code comments) are the author's working notes and are not in this repository; the code is in `AzaharLink/Includes/azl/dlp_*.hpp` and `AzaharLink/Sources/azl/dlp_*.cpp`. Tested with two consoles (see the README); three or four players and an Azahar player waiting in the room have not been tried.
- **Kirby Battle Royale no longer asks for the wireless to be turned on** (since 1.0.6). The game calls `dlp:FKCL` even for a local match; the plugin answers the "wireless on" check and the game goes on to the normal wireless. Only for `00040000001C2100` (`00040000001C2000`, another region, is in the table and has never been seen). Tested with 1.0.6 (console against console and Azahar against console, debug log on), not again with 1.0.10. 1.0.7 fixed a mistake of 1.0.6 in which Triple Deluxe and Planet Robobot were in the same list and showed the error `0xE0A0AC01`.
- **A room member that stays in the room but says it plays nothing is dropped from the game's session after 3 s** (1.0.10), as if it had left. This is meant for the 30 s that a console waited in Battle Royale when an Azahar player was closed abruptly. Not tried on a console; a member whose game information is empty (no title name, id 0) no longer counts as a player.

**All games**
- **The room data path (ENet) changed in five places** (1.0.5): the read skips a socket that was just found empty, a read pass stops after 32 datagrams and delivers them, the acknowledgement is sent after the packet reaches the game (held for at most 8 packets or 8 ms), a lost `CONNECT` is resent after 100, 300, 700 ms (not 500, 1500, 3500), and a ping is sent when the room speaks again after a silence of at least 0.5 s. The gain was worked out on recorded traces and **not measured on a console**.
- **Frames sent with the broadcast flag to one node go to that node's MAC** (1.0.5): the room delivers them to that node only. They are still unreliable and never doubled. Relevant for Heroes of Ruin, Final Fantasy Explorers and Battle Royale.
- **The UDS engine is split into shared functions** (1.0.10): `Initialize`, `BeginHostingNetwork` and `ConnectToNetwork` also serve the lobby. For the other games nothing should change; Final Fantasy Explorers, Tetris, Super Street Fighter IV, Monster Hunter 3 Ultimate and Tekken were tried with 1.0.10.
- **The plugin is 41,668 bytes bigger** (1,011,465 to 1,053,133, +4.1%) for every game, lobby or not.

**What you see**
- A second notification after joining the room says `AzaharLink <version> (built <stamp>)` (since 1.0.8), at every start, with the debug log off too.
- 25 s after the start the plugin compares its copy with the one in `/3ds/3DSLink/` and, if that one is a newer version, warns "3DS Link has a newer plugin. Open 3DS Link to update your games." (or, for the same version with other bytes, that this game's plugin is not the one in 3DS Link) (since 1.0.8; it reads up to 2 MB from the card, debug log off too).
- **The app** checks the plugin copies in the games' folders as soon as it starts, without waiting for the title scan, and recognises the plugin by its content (size and hash) instead of size and date (since 1.0.6). It says "Updating the plugin in your games: N of M folders done. Please wait." and then "You can close 3DS Link." (since 1.0.8); START during the pass asks "Leave anyway?" (*Leave* leaves the games not yet reached with the old plugin).

**Debug log**
- The capture is written to the card every 2 s, or at 160 KB, instead of four times a second (since 1.0.8). **This did not solve the problem**: with the debug log on, the Kirby games do not load or freeze, and the other consoles drop out. Keep it off.
- New lines: `dlp:` (which service the game uses), `dlpc:` (every Download Play command, 1.0.10), `hid:` (whether the plugin keeps `ir:rst` open, 1.0.5) and ENet counters (1.0.5). The build time in the log header is now the real one (1.0.5). Since 1.0.8 there are also `probe:` lines: 25 s after the start the plugin asks AM for the installed 3DS Link version and tries to read its icon; this probe has never been tried on a console.

**Tested** (1.0.10, debug log off, no logs, on the owner's consoles and with Azahar instances; scores and setups are in the README): Final Fantasy Explorers, Tetris, Super Street Fighter IV, Monster Hunter 3 Ultimate, Kirby: Planet Robobot, Tekken, Kirby: Triple Deluxe.
**Not fixed**: pressing HOME with the plugin enabled closes the game on an Old 3DS (issue 2), *Monster Hunter XX* closes when a multiplayer match starts on an Old 3DS (issue 3), *LEGO Batman 2* closes at launch, and *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D* and *Ocarina of Time 3D* close at launch (`0xE0A01839`).

**About the files of this release**: `3DSLink.cia` and `3DSLink.3dsx` are built from this tree. The plugin inside them is the very file that was tried on the consoles (same bytes); the app and the CIA were built again from the same sources, so they are not byte for byte the ones that were tried: the `.3dsx` differs from the tried one in 570 of its 2,423,468 bytes (random-looking bytes, which the CIA tool draws at each run for tickets and signatures, and a build date), and the `.cia`, whose contents are compressed and signed, in about 30% of its bytes (same size).

## 1.0.4 - 2026-10-25
- Fixes the C-stick not working in a game while the plugin is in its folder (issue #1, Monster Hunter 4 Ultimate; also seen in Monster Hunter 3 Ultimate). On a New 3DS the plugin framework opened the `ir:rst` service (C-stick, ZL, ZR), which has only two sessions. The plugin no longer opens it.
- ZL and ZR can no longer be part of the in-game menu key combination (they cannot be read any more).
- The in-game menu is now off by default (`menu_key=off`); an existing ini keeps its value, and the app's *In-game menu* switch puts `L+R+Select` back.
- Tested: Monster Hunter 3 Ultimate in a two-console room, Monster Hunter 4 Ultimate in single player.

## 1.0.3 - 2026-10-23
First public release.
