# 3DS Link

**Connect, share, play together.** 3DS Link takes the *local wireless* games of a Nintendo 3DS / 2DS (the ones that use `nwm::UDS`: Final Fantasy Explorers, Tri Force Heroes, Mario Party: The Top 100, Tekken 3D, ...) and plays them **over the internet** through a multiplayer room of the [Azahar](https://github.com/azahar-emu/azahar) emulator, with Azahar players and with other 3DS consoles. It also moves installed titles and the app itself from one console to another without a PC.

It is made of three parts that ship as **one package** (`3DSLink.cia`, or `3DSLink.3dsx` for the Homebrew Launcher):

| Part | What it is | Where |
|---|---|---|
| **AzaharLink plugin** (`AzaharLink.3gx`) | A [Luma3DS](https://github.com/LumaTeam/Luma3DS) Plugin Loader plugin, built on [CTRPluginFramework](https://github.com/PabloMK7/CTRPluginFramework-BlankTemplate). It sits inside the game, answers the game's wireless calls itself (`nwm::UDS`, `ndm:u`, ...) and sends the frames to the room through the console's normal Wi-Fi. There is no code specific to any game. | `AzaharLink/Sources`, `AzaharLink/Includes` |
| **3DS Link app** | A citro2d app to switch the plugin on per title, choose the room, check the installation, and run the two delivery features below. The plugin is **inside** the app, which writes it to the SD card by itself. | `AzaharLink/ConfigApp` |
| **Deliveries** | *Titles Delivery*: send installed titles to other consoles over UDS broadcast. *3DS Link Delivery*: hand the app itself to a console that does not have it, with nothing but Download Play (the console needs only Luma3DS). | `AzaharLink/ConfigApp/titleshare*`, `dlph`, `dlpc`; `DlpLink/` |

Current version: **1.0.2** (one version number for all of it: the plugin, the app, the CIA, the installer and the documents say the same `X.Y.Z`, kept in `AzaharLink/VERSION`).

> **Status, honestly.** The plugin has been played on real hardware with Final Fantasy Explorers, Tri Force Heroes, Mario Party: The Top 100, Tekken 3D, Street Fighter IV, Dragon Ball Z Extreme Butōden, Tetris and others, on two New 2DS XL (Luma3DS 13.3.3) and one New 3DS XL (Luma3DS 13.0 or newer, the exact version was not recorded). **Never tried on an Old 3DS / Old 2DS.** The latest app/CIA packaging (1.0.1 and 1.0.2: plugin inside the package, new About page, banner) is built and checked on a PC but **had not been through a console test when this repository was published**. See [Known issues](#known-issues).

## Requirements

* A 3DS / 2DS with **Luma3DS 13.0 or newer** and the **Plugin Loader enabled** (Rosalina menu: `L + Down + Select` → *Plugin Loader*). Luma 13.3.3 is the version it was tested on. Below 13.0 there is no Plugin Loader and the Azahar side of the app switches itself off (the deliveries still work).
* An Azahar multiplayer room you can reach (address and port, 24872 by default).
* `boot9.bin` from **your own** console, only for *sending* titles (the app has a step-by-step guide, "How to get boot9.bin", with GodMode9). Receiving needs only Luma with the signature patches.
* 3DS Link contains no game, no firmware image and no per-console key. It reads the `boot9.bin` of your own console, and only to *send* titles; it never uploads it. What the build needs from your own console is listed under [Build from source](#build-from-source).
* It is meant for titles you own and consoles you own. Nintendo, the games and the console's firmware belong to their makers.

## Install

There is no pre-built package, and none will be attached to a release: the build needs items from your own console (see [Build from source](#build-from-source)) and the package it makes carries some of them, so everybody builds their own and keeps it for themselves. Once you have `3DSLink.cia` (or `3DSLink.3dsx`):

1. Install `3DSLink.cia` with FBI (or put `3DSLink.3dsx` in `/3ds/`).
2. Open **3DS Link**. The first start scans the SD card and checks the installation. It writes `AzaharLink.3gx` to `sdmc:/3ds/3DSLink/` by itself.
3. *Azahar Emulator Room*: add your room. *Enable Titles for Azahar Emulator Net*: switch on the titles you want (it copies the plugin to `/luma/plugins/<TitleID>/`). *3DS Link*: the main switch.
4. Start the game. A "Joined room" notification means it works. With the main switch OFF (or a title not enabled) the game uses the real local wireless.

A short guide in Italian, with the checks to make on a console, is in [`AzaharLink/dist/LEGGIMI.txt`](AzaharLink/dist/LEGGIMI.txt).

## Build from source

You need **Docker** (no devkitPro on the host) and a shell. The scripts were written and tested with **Git Bash on Windows + Docker Desktop**; on Linux/macOS they should work as they are (`pwd -W` falls back to `pwd`) but that is untested.

```bash
# 1. makerom and ctrtool (Linux binaries, compiled once, kept out of the repository: see tools_ctr/README.md)
bash tools_ctr/build_tools.sh

# 2. everything: plugin, installer, the Download Play child model, the app, the CIA, and the checks
bash DlpLink/tools/build_all.sh
#    products: AzaharLink/AzaharLink.3gx, AzaharLink/ConfigApp/3DSLink.3dsx, DlpLink/out/3dslink.cia
#    add --dist to copy 3DSLink.cia / 3DSLink.3dsx to AzaharLink/dist and refresh the numbers in the documents
```

**Three things are not in this repository, and the project keeps them out on purpose: they belong to Nintendo or to the hardware, and whoever builds brings them from their own console** (Azahar keeps the same kind of values out of its source and reads them from the user's own key file):

| What | Where it goes (git ignores all three) | Without it |
|---|---|---|
| The access descriptor of a Download Play child (1024 bytes, signed by Nintendo) | `DlpLink/data/dlpchild_ad.bin` | `build_all.sh` stops at the model of the Download Play child ("MANCA ... dlpchild_ad.bin") |
| The 16-byte IV of the Download Play checksum | `DlpLink/host/dlph_iv.local.h`: `static const u8 DLP_IV[16] = { ... };` | the library builds with an all-zero placeholder and a `#warning`; 3DS Link Delivery does not work |
| The 128-bit constant of the hardware's AES key generator | `AzaharLink/ConfigApp/titleshare/common/keygen_const.local.h`: `#define KEYGEN_CONSTANT {0x...ULL, 0x...ULL}` (high word first) | the app builds with a zero placeholder and a `#warning`; the keys of *Titles Delivery* come out wrong |

The plugin alone (`make` in `AzaharLink`, with the `pablomk7/libctrpf` image) needs none of them. **The package a full build makes carries these values (the first two are compiled into the app), so keep it for yourself: do not publish or attach it anywhere.**

`build_all.sh` uses three Docker images: `devkitpro/devkitarm` (app, installer, tools), `pablomk7/libctrpf:0.8.0` (the plugin) and, for the PC tests, `azl-hle-build` (`docker build -t azl-hle-build -f AzaharLink/Tests/docker/hle-build.Dockerfile .`). The banner tool (`bannertool`) is not in the repository: `DlpLink/tools/bannertool/build.sh` fetches it (a fixed commit of a fork of Steveice10's MIT-licensed tool) and builds it in the container the first time, so that step needs `git` and network access.

**Tests (on a PC, in Docker, from the repository root):** `MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src/AzaharLink azl-hle-build bash Tests/run.sh` (unit tests of the engine, the settings, the app logic, replays of real captured frames), `bash AzaharLink/Tests/policy_check.sh` (a plain-text check that the settings and rules that decide what the plugin does have not been changed by accident), `bash DlpLink/tests/run_checks.sh` (CIA checks: they need a build, python3 and openssl; the `devkitpro/devkitarm` container has them: `docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src devkitpro/devkitarm bash DlpLink/tests/run_checks.sh`), `python AzaharLink/Tests/policy_mutants.py` (changes the sources one rule at a time and demands that the check notices; about 1,100 cases, slow). On a tree that was never built, `policy_check.sh` skips the checks of the build products (`dist/`, `ConfigApp/data/`) and says so; after `build_all.sh --dist` it runs them too.

**Placeholders in the tests and the notes.** The names of other players and the addresses of consoles and rooms in the notes and in the test data were replaced by labels (`Friend-A`, `Player-G`, `Console-A`, documentation IP ranges). The Download Play tests open the host with a **placeholder console MAC** (`02:00:00:00:00:01`, marked `PLACEHOLDER MAC` in `DlpLink/host/tests/dlph_test.c`, `DlpLink/host/tests/3ds.h` and `DlpLink/tests/dlpc_glue_dlph.c`): to compare the library with a capture of a real console, set **that console's real MAC** there (the checksum seed of a Download Play host depends on it).

Notes on the build chain, in Italian: [`AzaharLink/README.md`](AzaharLink/README.md) ("Compilare", "Test") and `DlpLink/docs/`.

## Repository layout

```
AzaharLink/           the plugin and the app
  Sources, Includes   the plugin (UDS engine, room client, hooks) and the code shared with the app
  ConfigApp/          the 3DS Link app (source/, guide/, banner/, the Download Play host dlph/, the composer dlpc/, the vendored delivery module titleshare/)
  Tests/              unit tests, replays of real captures, policy check and mutants
  diag/               a diagnostic build of the plugin for games that close at launch (see Known issues)
  README.md, NOTE_*.md, PROVE_HARDWARE.md, CHECKLIST_DA_VERIFICARE.md   the project notes and hardware test plans (Italian)
DlpLink/              Download Play: the installer child, the build chain of the CIA, checks, research notes
tools_ctr/            how to get makerom and ctrtool (binaries are not in the repository)
```

## Known issues

* **Some 2011 titles close at launch on one console.** On a New 3DS XL, with the plugin enabled for the title, *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D* and *The Legend of Zelda: Ocarina of Time 3D* end within five seconds with the error code `0xE0A01839` (the result of the game's own `svcCreateThread`). With the title **not** enabled they run, and the same plugin works with other (newer) games on the same console. The cause is **not established**: it may be Luma/the framework on that console rather than 3DS Link (a one-minute test with the main switch OFF, a title enabled, tells them apart). Analysis, a hypothesis from reading Luma's source and a diagnostic plugin build are in [`AzaharLink/diag/README.md`](AzaharLink/diag/README.md) and [`AzaharLink/NOTE_ANALISI_2026-10-21_BUTODEN_PACMAN.md`](AzaharLink/NOTE_ANALISI_2026-10-21_BUTODEN_PACMAN.md).
* Tekken 3D lags between two DS consoles (gaps of 80 ms and more rather than latency; not located yet). With `debug=1` some games with short network timeouts (Dragon Ball Z Extreme Butōden) lose the room: the capture stops the game for seconds. Play with `debug=0`.
* Old 3DS / Old 2DS: untested. Luma 13.0–13.2: untested (the plugin uses shared memory, which is all those versions have).
* Documentation is mostly in Italian and is a working log of the project rather than a manual.

## Contributing and reports

Issues and pull requests are welcome: see [`CONTRIBUTING.md`](CONTRIBUTING.md) and the [`CHANGELOG`](CHANGELOG.md). For a bug in a game, the useful files are `AzaharLink.log` and (with `debug=1`) `AzaharLink.debug.log` from `sd:/3ds/3DSLink/`; note that they contain the MAC address of the console and the names of the players in the room, so read them before posting.

## License and credits

3DS Link is free software, **GPL-2.0-or-later** (the UDS logic of the plugin is a port of code from Azahar, itself from Citra: "GPLv2 or any later version"); see [`LICENSE`](LICENSE). ENet (`AzaharLink/third_party/enet`) keeps its MIT license. The full list of what it uses or copied, with authors, licenses and links, is [`AzaharLink/CREDITS.md`](AzaharLink/CREDITS.md) (it is also the *Credits and thanks* page of the app): Azahar and Citra, FBI, 3dbrew, devkitPro, libctru, citro2d/citro3d, Luma3DS, CTRPluginFramework, ENet, GodMode9, makerom/ctrtool, bannertool and Pillow.

**A note on third-party code.** The linker script, `types.h`, `Unicode.h` and the plugin info file come from the CTRPluginFramework BlankTemplate by Nanquitas and PabloMK7; its repository carries no license file, so they are used and credited as its authors describe (see `CREDITS.md`) and will be replaced if its authors ask. The files ported from Azahar/Citra are under their GPL-2.0-or-later; ENet has its own license next to its source in `AzaharLink/third_party/enet/LICENSE`.

3DS Link is an independent project, not affiliated with or endorsed by Nintendo. "Nintendo 3DS" is a trademark of Nintendo.

Created by **Gionko**.
