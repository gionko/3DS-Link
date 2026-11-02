# Changelog

## 1.0.15 - 2026-11-02

**On an Old 3DS the power button should no longer close the game: a fix, not tried on one yet. Nothing changes on a New 3DS.**

> A user with an Old 3DS confirms that, with 1.0.14, HOME no longer closes the game: the first confirmation on a real Old 3DS of the fix in 1.0.12. The same user reports that the power button still did, and sent a Luma dump. This fix was tested on a PC only, on the compiled code: **not tried on any Old 3DS or Old 2DS**. A console with 1.0.14 is offered this version by the app; an older one installs it by hand.

### At a glance

| Area | Before | Now | Since |
|---|---|---|---|
| Old 3DS: the power button with the plugin enabled | closed the game | a fix, **not tried on an Old 3DS** | 1.0.15 |
| Old 3DS: HOME with the plugin enabled | closed the game | a user confirms it no longer does (on 1.0.14) | 1.0.12 |
| New 3DS family | | the games behave as in 1.0.14 | |

### Not fixed or not tried

| What | Notes |
|---|---|
| The fix for the power button on an Old 3DS or Old 2DS | **not tried**: this path does not run on a New 3DS |
| The update on an Old 3DS or Old 2DS | **not tried**: the secure connection takes longer there, and how long is not known |
| *Monster Hunter XX* on an Old 3DS | the fix **has not been tried** there: [issue 3](https://github.com/gionko/3DS-Link/issues/3), [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| *Mario Tennis Open*, USA copy on an Old 3DS: "the Wi-Fi switch is off" after the online option | [issue 2](https://github.com/gionko/3DS-Link/issues/2): not confirmed fixed, not tried on an Old 3DS (the Europe copy plays between two New 2DS XL) |
| *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) | on one console (a New 3DS XL): [Known limits](README.md#known-limits) |
| *Balloon Fight* (Virtual Console) closes at startup | the plugin framework aborts in Virtual Console games: a known limit |

### Technical notes

<details>
<summary>The power button: why the fix for HOME did not cover it</summary>

**The dump.** The game's main thread wakes up at the return of `svcSendSyncRequest` in the plugin's memory, which is no longer mapped, with the game's `APT:ReceiveParameter` on its stack. It is the case of HOME: on an Old 3DS Luma swaps the plugin's memory out at the end of that call, on the thread that made it, so a call made from the plugin's code wakes up in nothing.

**The cause.** Since 1.0.12 that call goes back to the game's own call site, but only in the seconds after a HOME button was seen: the notifications 1 and 2. The power button sends 8 and then 9, about 0.6 s later, as the logs of real consoles show. Its dialog belongs to the HOME menu, so Luma swaps in the same way, and the plugin was not ready for it.

**The fix.** `AptSignalPrecedesSwap` (`Includes/azl/apt_sleep.hpp`) holds 1, 2, 8 and 9. On a New 3DS the call is never handed back, so nothing changes for the game. A new scenario of `Tests/chain_sim/chain_sim.py` runs the compiled code with the reply word of a real console: it fails on 1.0.14 and passes on 1.0.15.

**What is not known.** Reading Luma's source, the swap happens at every `COMMAND_RESPONSE`, so library applets on an Old 3DS (the software keyboard, the Mii selector) may fall in the same way: nobody has reported it, and they were not touched.

</details>

## 1.0.14 - 2026-11-01

**3DS Link now updates itself: it tells you when a new version is out, installs it and starts again.**

> Tried on one of the owner's consoles, with a test build of this code that called itself 1.0.1 and updated itself to the public 1.0.13: the whole way went well, from the question to the restart and the plugin brought up to date in the games. **Not tried on any Old 3DS or Old 2DS.** This is the first version that has the update, so **it has to be installed by hand, one last time**. The plugin's code is unchanged: the games behave as in 1.0.13. Requested in [issue 4](https://github.com/gionko/3DS-Link/issues/4).

### At a glance

| Area | Before | Now | Since |
|---|---|---|---|
| A new version is out | you had to find out | at every start the app asks GitHub, in the background, and a popup offers the update | 1.0.14 |
| Installing it | by hand, with FBI | one button: download, install, restart | 1.0.14 |
| The plugin in your games | brought up to date at the next start | the same, and the update tells you to wait for its message | 1.0.14 |
| No Wi-Fi, no internet, GitHub not answering | - | nothing is shown at start | 1.0.14 |
| The console's clock is wrong | - | one popup: it is not serious, but updates cannot work until the date is right | 1.0.14 |
| *Settings* | - | `Check for updates` and the switch `Check at start` | 1.0.14 |

### How it behaves

* **The question.** The bottom screen shows the new version and yours, the top screen what is new. `A Update` goes on. `B Not now` is remembered for that version: the start does not ask about it again, a newer version does, and *Settings → Check for updates* always does.
* **The update.** The file is downloaded into memory and checked (it must be 3DS Link, at the version that was announced) before anything on the console changes. If anything fails you keep the version you have. Then 3DS Link starts again by itself; if it does not, open it from the HOME Menu.
* **The plugin.** After the restart 3DS Link brings the plugin up to date in all your games by itself and says when it is done. Wait for that message before closing it.
* **`3DSLink.3dsx`.** The file is replaced, and you open it again from the Homebrew Launcher.
* **What is sent.** One ordinary HTTPS request to GitHub at every start, and nothing about your console. `Check at start` turns it off.

### Not fixed or not tried

| What | Notes |
|---|---|
| The update on an Old 3DS or Old 2DS | **not tried**: the secure connection takes longer there, and how long is not known |
| *Monster Hunter XX* on an Old 3DS | the fix **has not been tried** there: [issue 3](https://github.com/gionko/3DS-Link/issues/3), [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| Old 3DS: pressing HOME with the plugin enabled | a fix since 1.0.12, **not tried on an Old 3DS**: [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| *Mario Tennis Open*, USA copy on an Old 3DS: "the Wi-Fi switch is off" after the online option | [issue 2](https://github.com/gionko/3DS-Link/issues/2): not confirmed fixed, not tried on an Old 3DS (the Europe copy plays between two New 2DS XL) |
| The power button closes the game on an Old 3DS | reported in [issue 2](https://github.com/gionko/3DS-Link/issues/2); it is not known whether the fix for HOME covers it |
| *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) | on one console (a New 3DS XL): [Known limits](README.md#known-limits) |
| *Balloon Fight* (Virtual Console) closes at startup | the plugin framework aborts in Virtual Console games: a known limit |

### Technical notes

<details>
<summary>How the update is made</summary>

**The check.** A single `HEAD` of `/releases/latest`: GitHub answers with a redirect to the tag, and the tag is the version. No API, so no limit of requests and no JSON. A prerelease or a tag with a suffix is never offered.

**The connection.** libcurl 8.4.0 on Mbed TLS 2.28 (devkitPro's portlibs), plain HTTPS: certificate, dates and host name are checked against Mozilla's public list of roots, which the app carries (`ConfigApp/certs/cacert.pem`). It does not depend on the console's own SSL module, which is asked only for random numbers.

**The clock.** Certificates cannot be checked with a clock that is years off. A year before 2026, or a certificate outside its dates, gives the popup once; it can come back only after a check has gone through.

**The install.** The CIA goes to AM without deleting the title first, because that title is running. A failure at any step leaves the old one in place.

**The size.** These libraries are in the app only: the CIA goes from 1.75 to 2.40 MB and the `.3dsx` from 2.43 to 3.40 MB. The Download Play child, which carries the CIA, goes from 3.10 to 3.76 MB.

**The tests.** `AzaharLink/Tests/settings_test/update_test.cpp` for the decisions and the words, `AzaharLink/Tests/update_http_test.sh` for the requests: the same libcurl on a PC against a server of its own (redirects, a file cut short, a 404, a file too big, a stop, silences, certificates that are out of date, of another host or of another authority) and against GitHub, also with the clock in 2011 and in 2040.

</details>

### Tested after this release (1 November 2026)

| Game | Setup | Score |
|---|---|---|
| *Luigi's Mansion 2* (Dark Moon), tower mode | 2 consoles over the internet (New 3DS XL and New 2DS XL) | 9/10 |
| *Monster Hunter XX* (Japanese copy, English patch v5) | the same | 10/10 |
| *New Super Mario Bros. 2* (Special Edition) | the same | 7/10: it was "playable, with several slowdowns" on 1.0.0 and it improved a lot with the recent versions, with no change aimed at it |
| *Metroid Prime: Federation Force* | 2 consoles over the internet (New 3DS and New 2DS) | 10/10 |

The debug log setting of these tests was not written down. Nothing in the code changed for them: see [Tested games](README.md#tested-games).

## 1.0.13 - 2026-11-01

**Monster Hunter XX no longer closes at launch. A list for any other game with the same defect.**

> Tried on the owner's **New 2DS XL**, with the Japanese copy of *Monster Hunter XX* and the English patch v5: it works very well: **10/10**. **Not tried on any Old 3DS or Old 2DS**: the two users who reported the crash have one, and there the fix takes another road. All the other games are unchanged.

### At a glance

| Game or area | Before | Now | Since |
|---|---|---|---|
| *Monster Hunter XX*, New 3DS family, plugin enabled | closed at launch, before its first frame | starts and plays: 10/10 | 1.0.13 |
| *Monster Hunter XX*, Old 3DS family, plugin enabled | closed at launch, before its first frame | a fix, **not tried on an Old 3DS** | 1.0.13 |
| A game with the same defect | no remedy | its title id goes in a list (`socplan::kLateBlockTitles`) | 1.0.13 |

### Not fixed or not tried

| What | Notes |
|---|---|
| *Monster Hunter XX* on an Old 3DS | the fix **has not been tried** there: [issue 3](https://github.com/gionko/3DS-Link/issues/3), [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| Old 3DS: pressing HOME with the plugin enabled | a fix since 1.0.12, **not tried on an Old 3DS**: [issue 2](https://github.com/gionko/3DS-Link/issues/2) |
| *Mario Tennis Open*, USA copy on an Old 3DS: "the Wi-Fi switch is off" after the online option | [issue 2](https://github.com/gionko/3DS-Link/issues/2): not confirmed fixed, not tried on an Old 3DS (the Europe copy plays between two New 2DS XL) |
| The power button closes the game on an Old 3DS | reported in [issue 2](https://github.com/gionko/3DS-Link/issues/2); it is not known whether the fix for HOME covers it |
| *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) | on one console (a New 3DS XL): [Known limits](README.md#known-limits) |
| *Balloon Fight* (Virtual Console) closes at startup | the plugin framework aborts in Virtual Console games: a known limit |

### Technical notes

<details>
<summary>Monster Hunter XX: the cause, the fix and the correction of what was written before</summary>

**The cause.** The Luma dumps of two Old 3DS and of a New 2DS XL show the same crash in the same place. At launch the game works out its heap once, down to the last page, from what is left: the smaller of the memory it is told it has and 80 MiB, minus what is already charged to it, minus 59 MiB of device memory. That is 8,240 KB, and the game fills them. The plugin took its 128 KB socket buffer before the game's first instruction, and that buffer is charged to the game: the heap became 8,112 KB, one of the game's allocations returned nothing, the game did not check and wrote through a null pointer. With the plugin off the game plays.

**The fix.** The titles in the list `socplan::kLateBlockTitles` (today only this game) get the buffer at the first connection to the room, when the game has already done its sums; so the game sees what it sees with the plugin off. For a game with the same defect, its title id in the list is enough. It is not a rule for every game: a game that fills a budget as large as the whole memory region would leave no room for a late buffer, while today it can spare 128 KB.

* **New 3DS family:** about 44 MiB of the game's budget are still free: a normal allocation.
* **Old 3DS family:** the game holds all of its 80 MiB, so the buffer comes from the SYSTEM region, like the plugin's own block on that console, through Luma's `svcControlMemoryUnsafe`. Such a block is charged to nobody, but when a process ends the kernel adds up its private blocks and gives the sum back to the budget, and if the sum is more than what was charged it gives back nothing: the next game would find no budget until a reboot. Freeing the block at the end would not be enough: after HOME on an Old 3DS the plugin is swapped out and, if the game is closed from there, Luma does not tell it. So the 128 KB are written on the process's own resource limit when the block is taken, after checking on the live object that its limits and its current value are where they are expected: if they are not, the buffer is not taken and the game runs without the room. The end then balances by itself, however the game ends.
* **Trying the Old 3DS road on a New 3DS:** an empty file `sd:/3ds/3DSLink/soc-from-system`, read only for the titles in the list.

**Corrections.** This changelog and the README said that the game closes "when a multiplayer match starts": it closes at launch. A reply in [issue 3](https://github.com/gionko/3DS-Link/issues/3) said that the dump shows a fault on the address `0x100b`: that value is another register of the dump, and the faulting address is `0xC000`.

</details>

## 1.0.12 - 2026-11-01

**Mario Tennis Open is played between consoles. A fix for HOME on the Old 3DS (not tried on one). A change so that LEGO Batman 2 also starts with 3DS Link switched off (not tried).**

> Tried on the owner's consoles: *Mario Tennis Open* (Europe copy, **two New Nintendo 2DS XL**, several modes and several matches, no problems: 9/10) and *SEGA 3D Classics Collection* (*Altered Beast*, 2 consoles, 10/10). **Not tried on any Old 3DS or Old 2DS**: the fix for HOME was written from Luma3DS's source and tested on a PC only. The change for *LEGO Batman 2* with the main switch off was not tried either. Scores and setups: [Tested games](README.md#tested-games).

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
| *Mario Tennis Open*, USA copy on an Old 3DS: "the Wi-Fi switch is off" after the online option | [issue 2](https://github.com/gionko/3DS-Link/issues/2): not confirmed fixed, not tried on an Old 3DS (the Europe copy plays between two New 2DS XL) |
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
| *Mario Tennis Open*: after the online option the game says the Wi-Fi switch is off (Old 3DS) | reported by a user, not analysed at the time: [issue 2](https://github.com/gionko/3DS-Link/issues/2). Local play between two consoles works since 1.0.12 (Europe copy, two New 2DS XL); whether this report was the same message is not confirmed (the "online option" may also be the game's internet mode, which 3DS Link does not carry) |
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
