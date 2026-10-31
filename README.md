# 3DS Link

[![Release](https://img.shields.io/github/v/release/gionko/3DS-Link)](https://github.com/gionko/3DS-Link/releases/latest)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)
[![policy check](https://github.com/gionko/3DS-Link/actions/workflows/policy-check.yml/badge.svg)](https://github.com/gionko/3DS-Link/actions/workflows/policy-check.yml)

**Connect, share, play together.** 3DS Link is an application for the Nintendo 3DS family that carries local wireless games over the internet, and moves software between consoles without a PC.

Current version: **1.0.11**. Download it from the [Releases](../../releases/latest) page.

A player on a PC with Azahar, a player on a tablet with Azahar and a player on a real Nintendo 3DS can all be in the same match of the same game, over the internet.

## Contents

* [Overview](#overview) · [The three modes](#the-three-modes)
* [Requirements](#requirements) · [Installation](#installation)
* [Playing local wireless games online](#playing-local-wireless-games-online) · [Playing with Azahar](#playing-with-azahar)
* [Tested games](#tested-games) · [Known limits](#known-limits)
* [How it works](#how-it-works) · [Repository layout](#repository-layout)
* [Building from source](#building-from-source) · [Tests](#tests)
* [Reports and testing](#reports-and-testing) · [Contributing](#contributing)
* [License, credits and notices](#license-credits-and-notices)

## Overview

Almost every Nintendo 3DS multiplayer game offers local wireless play, few offer online play, and the console's internet connection was always optional. 3DS Link starts from there. It has two aims:

* to let local wireless games be played with anyone, anywhere, through an [Azahar](https://github.com/azahar-emu/azahar) multiplayer room, with real consoles and Azahar players (PC or Android) in the same match;
* to let consoles do among themselves what normally takes a PC, an internet connection or taking out the SD card: send installed titles to other consoles, and install 3DS Link itself on a console that does not have it, with nothing but another 3DS.

3DS Link has nothing to do with Pretendo or with Nintendo's online services: there are no official servers and no accounts, only the game's own local wireless carried over the internet.

It is made of three parts that ship as **one package** (`3DSLink.cia`, or `3DSLink.3dsx` for the Homebrew Launcher):

| Part | What it is | Where |
|---|---|---|
| **AzaharLink plugin** (`AzaharLink.3gx`) | A [Luma3DS](https://github.com/LumaTeam/Luma3DS) Plugin Loader plugin, built on [CTRPluginFramework](https://github.com/PabloMK7/CTRPluginFramework-BlankTemplate). It sits inside the game, answers the game's wireless calls itself (`nwm::UDS`, `ndm:u`, ...) and sends the frames to the room through the console's normal Wi-Fi. There is no code specific to any game. | `AzaharLink/Sources`, `AzaharLink/Includes` |
| **3DS Link app** | A citro2d application to switch the plugin on per game, choose the room, check the installation and run the two deliveries. The plugin is **inside** the app, which writes it to the SD card by itself. | `AzaharLink/ConfigApp` |
| **Deliveries** | *Titles Delivery* and *3DS Link Delivery*, described below. | `AzaharLink/ConfigApp/titleshare*`, `dlph`, `dlpc`; `DlpLink/` |

## The three modes

### Azahar Emulator Room: local wireless over the internet

Local wireless multiplayer, the kind that needs everybody in the same room, goes through an Azahar room on the internet. The game does not notice: it believes it sees nearby consoles, while the other players may be on the other side of the world, on a real console or on Azahar. You choose, game by game, which titles go through the room; the others keep using the real wireless.

### Titles Delivery: move titles between your consoles

Sends installed titles (games, updates, DLC, homebrew) from one console to the others nearby, with no PC, no internet and no SD card swapping. The transmission is one for everybody: up to seven receiving consoles take the same data at the same moment, so sending to seven consoles takes about as long as sending to one. As an example, a 680 MB title at an average of 1.6 MB/s takes about 7 minutes to one console and about 7 minutes to seven consoles together, against almost 50 minutes sending to seven one after another. The sending console writes no file: it builds the CIA on the fly while it transmits, and the receiving consoles install while they download.

### 3DS Link Delivery: install 3DS Link on another console with Download Play

A console that has 3DS Link offers it to the others through the system's Download Play: the other console installs it, updates it or, if you wish, goes back to an older version. No internet, no SD card to touch, no installer to find. The receiving console needs only Luma3DS.

## Requirements

* **A console with Luma3DS.** 3DS Link installs from Luma3DS 10 up, and Titles Delivery and 3DS Link Delivery already work there.
* **For the Azahar room, Luma3DS 13.3 or newer** with the **Plugin Loader enabled** (Rosalina menu: `L + Down + Select` → *Plugin Loader*; 3DS Link tells you, and enables it for you). The plugin was tested on 13.3.3; 13.0 to 13.2 should work but are untested.
* **For the Azahar room, a Wi-Fi connection with internet and an Azahar room.** Anyone can open one from Azahar (*Multiplayer → Create Room*) or with Azahar's room server. If the room runs at home and the other players connect from outside, forward UDP port 24872 on the router.
* **A good, genuine SD card.** The speed of Titles Delivery depends on it: the receiving console installs while it downloads, and a slow, worn-out or counterfeit card slows everything down or corrupts files. Test a new card first (H2testw on Windows, F3 on Linux).
* **`boot9.bin` from your own console, only for sending.** Titles Delivery needs it to send titles and 3DS Link Delivery to offer 3DS Link. Receiving needs nothing. Do not download it and do not share it: dump it once from your own console with GodMode9; the app has a step-by-step guide (*Settings → Installation check*, the `boot9.bin` row, `Y`).
* 3DS Link contains no game, no firmware image and no per-console key. It reads the `boot9.bin` of your own console, and only to *send* titles; it never uploads it. It is meant for titles you own and consoles you own.

## Installation

Download `3DSLink.cia` (or `3DSLink.3dsx`) from the [Releases](../../releases/latest) page, or build it yourself (see [Building from source](#building-from-source)).

1. Install `3DSLink.cia` with FBI or any other CIA installer (or put `3DSLink.3dsx` in `/3ds/`). A console that has no 3DS Link can also get it from one that has it, with Download Play alone (*3DS Link Delivery*).
2. Open **3DS Link** from the HOME Menu. The first start reads the installed titles and checks the installation; it writes `AzaharLink.3gx` to `sdmc:/3ds/3DSLink/` by itself. Anything that is missing gets its own row, and in most cases `A` fixes it.

## Playing local wireless games online

1. Open 3DS Link. In *Azahar Emulator Room*, add the room (name, address, port and password if it has one): the app tests it at once.
2. In *Enable Titles for Azahar Emulator Net*, switch on the games you want to play online. On the games left off, 3DS Link does nothing and the game starts as usual. Switch on only games that have local wireless play: for the others it brings nothing.
3. Exit with `START` and start the game. A notification tells you how it went with the room: `Joined room "..." (n players)` means you are in; `Room: Wrong password` and `Room: Could not connect to the room` say what to look at.
4. In the game, choose local wireless play as you would with people nearby: one player creates the match, the others join it. Azahar players join the same room (*Multiplayer* menu) and start the same game.

With the main switch *3DS Link* off, or for a game that is not switched on, the game uses the real local wireless.

## Playing with Azahar

* **Keep the emulation at 100%** (between 99 and 101%, the speed Azahar shows at the bottom of its window). If Azahar runs slower or faster than the console, the match suffers. Graphics enhancements such as a higher resolution are fine as long as the speed stays at 100%.
* **It is still emulation**, with its own bugs, and the network side of the games is not completely emulated. In *Tekken 3D Prime Edition*, for instance, two consoles work and a console can join a match created in Azahar, but Azahar cannot join a match created by a console: 100 ms after connecting the game asks the host for its application data (`GetApplicationData`, `nwm::UDS` command `0x11`) and Azahar, as a client, answers with zero bytes. When a game does not work between Azahar and a console, let the Azahar player create the match.
* **Kirby: Triple Deluxe and Planet Robobot** find their group through a lobby that 3DS Link makes between consoles inside the room: an Azahar player cannot take part in that lobby (Azahar's Download Play cannot open the child game), so for these two games all the players must be on real consoles with 3DS Link.
* Some games do not work in Azahar at all: in *Heroes of Ruin* Azahar can neither create a local wireless match nor see one, not even with another Azahar. Between two consoles, with 3DS Link, the game works well: the limit is in the emulator, not in the room.

## Tested games

The first two tables were tested by the project owner, on the owner's consoles and with consoles and Azahar instances of friends; the third lists what users reported or what was found not to work. **This is not a compatibility guarantee**: a game that is not in the tables has not been tried.

| Table | Tested on | Debug log | Rating |
|---|---|---|---|
| [Tested on 1.0.10 and 1.0.11](#tested-on-1010-and-1011) | 1.0.10 or 1.0.11 (the version is in each row) | **off**, no logs, except where the row says (a log is only written with the debug log on, which changes how the games behave: see [Known limits](#known-limits)) | score 1 to 10 |
| [Last tested on an older version](#last-tested-on-an-older-version) | the version written in the row | mostly on, as some rows say | none yet: the words used then |
| [Reported or known not to work](#reported-or-known-not-to-work) | — | — | — |

How to read the columns:

* **Score (1 to 10)**: the owner's own, subjective rating of how well the game plays through the room, in the setup written in the row. It is not a benchmark, and the steps of the scale are not defined.
* **3DS Link**: the version that was running. The build number is in the log header and on the screen (`AzaharLink X.Y.Z (built ...)`). The build that is today's 1.0.0 called itself 0.1.17 in its log. Versions 1.0.5 to 1.0.9 were test builds that were never released (see the [CHANGELOG](CHANGELOG.md)).
* **Modification that matters (since)**, in the first table, and **Modification since then that matters**, in the second: which change, and since which version, is relevant for that game. The full list is in the [CHANGELOG](CHANGELOG.md). The ENet data path change of 1.0.5 touches every game, so it is only written where it is the one that matters most.
* **Result then** (second table): the words used when the game was tried; no score has been given yet.

### Tested on 1.0.10 and 1.0.11

| Game | Players tested | Score | 3DS Link | Modification that matters (since) |
|---|---|---|---|---|
| Final Fantasy Explorers | 3 players: 2 consoles and Azahar (Android) | **10/10** | 1.0.10 | 1.0.5 frames sent to one node go to that node; 1.0.10 a room member that stopped playing is dropped from the game's session (not exercised in this test) |
| Tetris (Hudson Soft, 2011) | 4 players: 2 consoles, Azahar (Android) and Azahar (PC) | **10/10** | 1.0.10 | 1.0.5 ENet data path |
| Super Street Fighter IV 3D Edition | 3 consoles in the room, 2 playing at a time | **10/10** | 1.0.10 | 1.0.5 ENet data path (a fixed-step fighting game) |
| Monster Hunter 3 Ultimate | 2 consoles, 2 players | **10/10** | 1.0.10 | 1.0.4 the C-stick works again |
| Kirby: Planet Robobot | 2 consoles | **9/10** | 1.0.10 | 1.0.6 the game no longer closes at launch; 1.0.10 the Download Play lobby is provided inside the room |
| Tekken 3D Prime Edition | 2 consoles, 2 players | **8/10** | 1.0.10 | 1.0.5 ENet data path (expected to matter for a fast game; not measured) |
| Kirby: Triple Deluxe | 2 consoles | **9/10**; found a little slow | 1.0.10 | 1.0.6 the game no longer closes at launch; 1.0.10 the Download Play lobby is provided inside the room |
| LEGO Batman 2: DC Super Heroes | 2 New 3DS consoles connected through an Azahar room, with the debug log on (the logs are still to be read) | works; **no score given yet** | 1.0.11 | 1.0.11 the game no longer closes at launch (the plugin rewrites the game's GSP wrapper so that the framework finds it) |

Not tried yet with three or four players: the two Kirby games that use the lobby. Not tried with an Azahar player in the room: the same two (the lobby is made between 3DS Link consoles, Azahar cannot take part in it).

### Last tested on an older version

These were not re-tested on 1.0.10 or 1.0.11. The result is the one of the version written in the row, in the words used then; no score has been given to them yet.

| Game | Players tested | Result then | 3DS Link | Modification since then that matters |
|---|---|---|---|---|
| Heroes of Ruin | 2 consoles over the internet; console and Azahar | Perfect between consoles; does not work with Azahar (see above) | 1.0.3 | 1.0.5 frames sent to one node go to that node |
| The Legend of Zelda: Tri Force Heroes | 3 consoles (1.0.3); earlier, 2 consoles and a PC (0.1.17) | Great | 1.0.3 | 1.0.5 ENet data path |
| Dragon Ball Z: Extreme Butōden | 2 consoles | Great; with the debug log on it freezes for 5 to 10 s | 1.0.0 | 1.0.5 ENet data path; on Old 3DS see [Known limits](#known-limits) |
| New Super Mario Bros. 2 | 2 players | Playable, with several slowdowns (seen with the debug log on) | 1.0.0 | 1.0.5 ENet data path (expected to be the most sensitive to the held acknowledgements; not measured) |
| Dead or Alive Dimensions | 2 players | Works, with some stutter | 1.0.0 | 1.0.5 ENet data path |
| Mario Party: The Top 100 | 3 players: 2 consoles and Azahar (1.0.3); 4 players earlier (0.1.16) | Works; not tested thoroughly | 1.0.3 | 1.0.5 ENet data path (tuned on its recordings) |
| Mario Party: Star Rush | 3 consoles; 3 consoles and 1 Azahar (4 players); debug log on | With consoles only: good, not excellent (about 28 frames per second, the bottleneck being inside the consoles themselves). With an Azahar player in the match: not playable, matches fail to start with the game's communication error and go back to the lobby (more often when Azahar hosts the lobby: let a console create the match) | 1.0.3 | 1.0.5 ENet data path (tuned on its recordings) |
| SEGA 3D Classics Collection | 2 consoles | Works | 1.0.0 | 1.0.5 ENet data path |
| Monster Hunter 4 Ultimate | single player | The C-stick works (issue 1) | 1.0.4 | 1.0.4 the C-stick works again |
| Kirby Battle Royale | 2 consoles; a console and Azahar (debug log on) | Works: the lobby and a whole match; when an Azahar player is closed abruptly the console waited about 30 s | 1.0.6 | 1.0.6 the plugin answers the game's Download Play wireless check; 1.0.10 a room member that stopped playing is dropped after 3 s (not tested) |

### Reported or known not to work

| Game | What happens | Since / notes |
|---|---|---|
| Mario Tennis Open | After choosing the online option the game says the Wi-Fi switch is off (reported on an Old 3DS, not analysed) | Not analysed |
| Monster Hunter XX (Japan) | Closes at the start of a multiplayer match (Old 3DS, issue 3) | Not fixed |
| Balloon Fight (Virtual Console) | Closes at startup: the framework the plugin is built on aborts in Virtual Console games | Known limit |
| Pac-Man & Galaga Dimensions, Ridge Racer 3D, The Legend of Zelda: Ocarina of Time 3D | Close at launch on one of the test consoles with the plugin switched on (error `0xE0A01839`); with the game left off they run | See [Known limits](#known-limits) |

## Known limits

| Limit | In short | Status |
|---|---|---|
| **One plugin per game** | Another Luma plugin in the same game's folder: 3DS Link leaves that folder alone | By design |
| **Download Play games** | Only *Kirby: Triple Deluxe*, *Kirby: Planet Robobot* and *Kirby Battle Royale* work; other Download Play games, single-copy Download Play and Virtual Console games do not | Not supported, except those three |
| **Some 2011 titles** | *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) on one console | A remedy exists in the notes, **not** applied or tried |
| **Old 3DS and Old 2DS** | HOME closes the game; *Monster Hunter XX* closes in multiplayer (reported by users) | **Not fixed**: the cause of the HOME crash is understood, the fix is not written yet |
| **LEGO Batman 2 with 3DS Link switched off** | The fix of 1.0.11 is part of the hooks: with the main switch off the plugin stays loaded and the framework still stops the game at launch | To be fixed in the next version; meanwhile switch 3DS Link on, or take the game off in *Enable Titles* |
| **The debug log** | Makes everything stutter; with it on the Kirby games, *Dragon Ball Z* and *Tekken* are not playable | Not fixable here: keep it off (the default) |
| **Fast fighting games** | Can stutter on a slow connection; *Tekken 3D* lags between two consoles | Cause of Tekken's gaps not located |
| **The Download Play lobby of the two Kirby games** (*Triple Deluxe*, *Planet Robobot*) | At most four players; tried with two consoles; *Triple Deluxe* found "a little slow" | Partly tried |

### Details of the limits

* **One plugin per game.** A game switched on in 3DS Link cannot also have another Luma plugin (cheats, CTGP-7, ...): 3DS Link detects it and leaves that game's folder alone.
* **Download Play games.** The plugin carries the game's own local wireless (`nwm::UDS`). Three Kirby games use the Download Play service to find their group and work: *Triple Deluxe* and *Planet Robobot* because 3DS Link provides that part inside the room (since 1.0.10, between consoles only: Azahar cannot take part), and *Battle Royale* because the plugin answers its wireless check (since 1.0.6; last tried on 1.0.6). Other games that use Download Play to play together, single-copy Download Play (sending the game to a console that does not have it) and Virtual Console games do not work.
* **Some 2011 titles.** On a New 3DS XL, with the plugin enabled for the title, *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D* and *Ocarina of Time 3D* end within five seconds with the error code `0xE0A01839` (the result of the game's own `svcCreateThread`). With the title not enabled they run, and the same plugin works with newer games on the same console. The analysis points at a worker thread of the plugin framework that is started on the second core for these titles. A game without local wireless can simply stay off in *Enable Titles*. The analysis and a diagnostic build of the plugin are in [`AzaharLink/diag/README.md`](AzaharLink/diag/README.md).
* **Old 3DS and Old 2DS.** There is no console of this kind in the test set, and users report problems: with the plugin enabled, pressing HOME closes the game (the cause is understood: Luma frees the plugin's memory while a game thread is still inside the plugin's hook; the fix is not written yet), and *Monster Hunter XX* closes when a multiplayer match starts ([issue 2](https://github.com/gionko/3DS-Link/issues/2), [issue 3](https://github.com/gionko/3DS-Link/issues/3)). *LEGO Batman 2* also closed at launch on an Old 3DS, with the USA copy (issue 2): the cause found on New 3DS consoles is fixed in 1.0.11 (see the [CHANGELOG](CHANGELOG.md)), but that fix has **not** been tried on an Old 3DS or with the USA copy. Luma 13.0–13.2 are untested.
* **The debug log** (*Settings → Debug log*) writes a lot to the SD card. With it on, the Kirby games, *Dragon Ball Z* and *Tekken* are not playable (the capture probably saturates the card the game loads from: the game does not load or freezes, and the other consoles drop out), and some games with short network timeouts lose the room. Turn it on only to make a report. Writing the capture every 2 s instead of four times a second (1.0.8) did not solve it.
* **Fast fighting games** feel the network more and can stutter on a slow connection. *Tekken 3D* lags between two consoles: gaps of 80 ms and more rather than latency.
* **The Download Play lobby of the two Kirby games** is made of at most four players and was tried with two consoles only. *Triple Deluxe* was found "a little slow" without saying where (no log: the debug log cannot be used with these games).

## How it works

**The room.** A Luma plugin takes the place of the game's `nwm::UDS` service. Beacons, data frames and nodes become ENet packets to the Azahar room, in the same format Azahar uses (the logic is a port of Azahar's `nwm_uds` code). The app puts the plugin in the folder of every game you switch on.

**Titles Delivery.** The sending console reads the installed contents from the SD card, removes the SD encryption and builds the CIA piece by piece while it transmits (header, certificates, ticket, TMD, contents): a real CIA, which AM accepts like the ones from FBI and GodMode9. Each packet goes out as a UDS broadcast and every console that lacks it picks it up; lost packets are repaired the same way, one repeated packet for every console that lost it; late arrivals ride along for free if the title is still in the queue. The data goes straight to the receiving console's AM service as it arrives, so no CIA is left to delete. `boot9.bin` is needed because the SD card is encrypted with a key made of two halves: one is in `movable.sed`, which the app reads by itself, the other is in the ARM9 bootrom, which applications cannot read.

**3DS Link Delivery.** Download Play sends a small program, the *child*, to the other console, which installs it and runs it. The child of 3DS Link is an installer: it carries the 3DS Link CIA, the plugin and a manifest with the size and SHA-256 of every file, checks them, and installs 3DS Link through the AM service. Since the child contains the app and the app would have to contain the child, 3DS Link carries only a template of the child (about 250 KB) and a small compositor that builds the child on the console the first time the feature is opened.

**SD card only.** By design 3DS Link writes only to the SD card and only reads the NAND (the seed database, the certificates, `movable.sed`); titles are installed by the console's AM service, exactly as FBI does, so everything it installs can be managed or deleted with FBI and GodMode9. The two exceptions are the Download Play child, which by Download Play's design goes into its own NAND area, and `boot9.bin`, which you dump once with GodMode9.

## Repository layout

```
AzaharLink/           the plugin and the app
  Sources, Includes   the plugin (UDS engine, room client, hooks) and the code shared with the app
  ConfigApp/          the 3DS Link app (source/, guide/, banner/, the Download Play host dlph/, the composer dlpc/, the vendored delivery module titleshare/)
  Tests/              unit tests, replays of real captures, policy check and mutants
  diag/               a diagnostic build of the plugin for games that close at launch
  README.md, NOTE_*.md, PROVE_HARDWARE.md, CHECKLIST_DA_VERIFICARE.md   the project notes and hardware test plans (Italian)
DlpLink/              Download Play: the installer child, the build chain of the CIA, checks, research notes
tools_ctr/            how to get makerom and ctrtool (binaries are not in the repository)
```

The documentation under `AzaharLink/` and `DlpLink/docs/` is mostly in Italian and is a working log of the project rather than a manual. Comments in the code sometimes cite design notes and measurements kept under `analysis/` (for the Download Play lobby of the two Kirby games, `analysis/dlp-lobby-fake-2026-10-28/`): those folders are the author's working notes and are **not** in this repository.

## Building from source

You need **Docker** (no devkitPro on the host) and a shell. The scripts were written and tested with **Git Bash on Windows + Docker Desktop**; on Linux and macOS they should work as they are (`pwd -W` falls back to `pwd`) but that is untested.

```bash
# 1. makerom and ctrtool (Linux binaries, compiled once, kept out of the repository: see tools_ctr/README.md)
bash tools_ctr/build_tools.sh

# 2. everything: plugin, installer, the Download Play child model, the app, the CIA, and the checks
bash DlpLink/tools/build_all.sh
#    products: AzaharLink/AzaharLink.3gx, AzaharLink/ConfigApp/3DSLink.3dsx, DlpLink/out/3dslink.cia
#    add --dist to copy 3DSLink.cia / 3DSLink.3dsx to AzaharLink/dist and refresh the numbers in the documents
```

`build_all.sh` uses three Docker images: `devkitpro/devkitarm` (app, installer, tools), `pablomk7/libctrpf:0.8.0` (the plugin) and, for the PC tests, `azl-hle-build` (`docker build -t azl-hle-build -f AzaharLink/Tests/docker/hle-build.Dockerfile .`). The banner tool (`bannertool`) is not in the repository: `DlpLink/tools/bannertool/build.sh` fetches it (a fixed commit of a fork of Steveice10's MIT-licensed tool) and builds it in the container the first time, so that step needs `git` and network access.

**Nothing of Nintendo's is needed to build.** The Download Play child carries the access descriptor that `makerom` makes itself (`-desc dlpchild:1`, signed with its own key), with the programId turned into the wildcard `00040001FFFFFFFF` (`finalize_child.py --ad wild`, the default). This was tried on a console on 2026-10-23: a console without 3DS Link downloaded the child with Download Play and installed it. (Before 1.0.3 the child carried the descriptor that Nintendo signed; `DLP_AD=file` still takes such a 1024-byte file from `DlpLink/data/dlpchild_ad.bin`, which git ignores.) The two other values the app needs are in the repository, carried the way Azahar carries its default keys: the constant of the hardware's AES key generator and the IV of the Download Play checksum are the values Azahar ships as `generatorConstant` and `dlpChecksumModIv` (`src/core/hw/default_keys.h`), kept here as a small text obfuscated with AES and a key of zeros (`AzaharLink/ConfigApp/titleshare_host/builtin_keys_blob.h`, read by `builtin_keys.c`). The obfuscation is no protection and is not meant as one.

## Tests

On a PC, in Docker, from the repository root:

* `MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src/AzaharLink azl-hle-build bash Tests/run.sh`: unit tests of the engine, the settings and the app logic, and replays of real captured frames;
* `bash AzaharLink/Tests/policy_check.sh`: a plain-text check that the settings and rules that decide what the plugin does have not been changed by accident (on a tree that was never built it skips the checks of the build products and says so);
* `bash DlpLink/tests/run_checks.sh`: checks of the CIAs; they need a build, python3 and openssl, which the `devkitpro/devkitarm` container has: `docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src devkitpro/devkitarm bash DlpLink/tests/run_checks.sh`;
* `python AzaharLink/Tests/policy_mutants.py`: changes the sources one rule at a time and demands that the policy check notices (over a thousand cases, slow).

The names of other players and the addresses of consoles and rooms in the notes and in the test data were replaced by labels (`Friend-A`, `Player-G`, `Console-A`, documentation IP ranges). The Download Play tests open the host with a **placeholder console MAC** (`02:00:00:00:00:01`, marked `PLACEHOLDER MAC` in `DlpLink/host/tests/dlph_test.c`, `DlpLink/host/tests/3ds.h` and `DlpLink/tests/dlpc_glue_dlph.c`): to compare the library with a capture of a real console, set that console's real MAC there (the checksum seed of a Download Play host depends on it).

## Reports and testing

If a game does not work, open an [issue](https://github.com/gionko/3DS-Link/issues) with:

* your console model and Luma3DS version (*Settings → Installation check* shows it);
* the game, who you were playing with (console or Azahar, on PC or Android) and what happened;
* a photo of the error, if there is one, and the Luma dump if the game closed with an error (`sd:/luma/dumps/arm11/`);
* the logs in `sd:/3ds/3DSLink` (`AzaharLink.log` and, with *Settings → Debug log* on, `AzaharLink.debug.log`; for Titles Delivery, `3DSLink-TitleDelivery.log`). They contain the MAC address of the console and the names of the players in the room: read them before posting.

**Testers are welcome on every model**, especially Old 3DS, Old 3DS XL and the original 2DS, which the project could not test on. The logs of matches that go well are as useful as the ones of matches that fail. With the debug log on, expect everything to be slow: loading screens that normally take seconds can take minutes. The log records every network call the game makes, and it is the only way to see what happens on a console that the developer does not have. Turn it off when you are done.

## Contributing

Issues and pull requests are welcome: see [`CONTRIBUTING.md`](CONTRIBUTING.md). The changes of each release are in [`CHANGELOG.md`](CHANGELOG.md).

## License, credits and notices

3DS Link is free software, **GPL-2.0-or-later** (the UDS logic of the plugin is a port of code from Azahar, itself from Citra: "GPLv2 or any later version"); see [`LICENSE`](LICENSE). ENet (`AzaharLink/third_party/enet`) keeps its MIT license. The full list of what 3DS Link uses or copied, with authors, licenses and links, is [`AzaharLink/CREDITS.md`](AzaharLink/CREDITS.md) (it is also the *Credits and thanks* page of the app): Azahar and Citra, FBI, 3dbrew, devkitPro, libctru, citro2d/citro3d, Luma3DS, CTRPluginFramework, ENet, GodMode9, makerom/ctrtool, bannertool and Pillow. Thanks to everyone who tested the consoles and the emulator.

**Where the public values come from.** A few constants in the code are public, not secret. The key generator constant and the Download Play checksum IV are the defaults Azahar ships (see [Building from source](#building-from-source)). The Download Play passphrase (`dlph.c`) is the one in Azahar's own source (`src/core/hle/service/dlp/dlp_base.h`). The SHA-256 digests that recognise a `boot9.bin` (`AzaharLink/ConfigApp/source/paths.cpp`) are digests of the same kind that tools such as pyctr list: they identify the file, they are not its content. The ticket skeleton (`titleshare/common/ticket_template.h`) follows the public ticket layout (3dbrew, *Ticket*), with a fake signature, no title key and no console ID, and the certificate names are the public ones. The network bytes in `AzaharLink/Tests/*.inc` are frames captured from play sessions, kept as test vectors.

**Third-party code.** The linker script, `types.h`, `Unicode.h` and the plugin info file come from the CTRPluginFramework BlankTemplate by Nanquitas and PabloMK7; its repository carries no license file, so they are used and credited as its authors describe (see `CREDITS.md`) and will be replaced if its authors ask. The files ported from Azahar and Citra are under their GPL-2.0-or-later; ENet has its own license next to its source in `AzaharLink/third_party/enet/LICENSE`.

3DS Link is an independent project, not affiliated with or endorsed by Nintendo. "Nintendo 3DS" is a trademark of Nintendo.

Created by **Gionko**.
