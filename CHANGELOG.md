# Changelog

## 1.1.2 - 2026-11-08

**The lobby update: the public Azahar rooms in the app, with their ping, how many players are in and what they play, your favourite rooms, Quick join, and a chat inside the games with a rebuilt in-game menu. Also: a new way of hooking the game, written once before it starts, so that a thread asleep in a system call can no longer wake up on a rewritten instruction. The Old 3DS fixes of this version, one more game with the Download Play lobby (*Luigi's Mansion*, 2018), and guards for a game that is closed and the next one that starts. Tried on consoles of the New 3DS family only: nothing of it was tried on an Old 3DS yet.**

> Where the reports came from. [Issue 5](https://github.com/gionko/3DS-Link/issues/5) tried 1.1.1 on an Old 3DS with Pokémon Alpha Sapphire: going to HOME and back crashed at the 19th try (the 15th with the debug log off). A user on the GBAtemp thread sent dumps of *LEGO Batman 2* on an Old 3DS. A tester sent the logs of *Mario Party: Island Tour* and *Luigi's Mansion* (2018), whose local multiplayer said the wireless was off, and asked to see in the chat who comes into the room and who leaves.
>
> What was tried. Two consoles of the New 3DS family (a New 2DS XL and a New 3DS XL), on 7 and 8 November 2026, with the debug log on: *Monster Hunter XX* with both consoles in the same room and the chat used many times (up to nine openings of the keyboard in a match), then closed from HOME, several times: the game no longer stayed in memory; eight to ten games in a row with the chat; with the build that has the exit guard, 29 game closes on the two consoles with no crash and no "Couldn't set memblock size", and a match of *Super Street Fighter IV 3D Edition* between the two; then a first try of *Luigi's Mansion* (2018) with its new lobby, between two New 3DS (3DS to 3DS): it played; and the chat's lines for who joins and who leaves, seen on the same two consoles. **Not tried on any Old 3DS or Old 2DS**, where most of the hook and swap changes run. A console with 1.0.14 or later is offered this version by the app; an older one installs it by hand.

### At a glance

| Area | Before | Now | Since |
|---|---|---|---|
| **Public lobbies** | only the rooms you typed in | *Azahar Emulator Room → Public lobbies*: the public Azahar rooms, each asked without joining it, with its **ping**, **how many players are in out of how many seats** and **what they play**, the password lock and *full*. Rooms with a free seat and the lowest ping come first | 1.1.2 |
| **Quick join** | - | one button: the room with a free seat where somebody plays one of your games (your edition first), with the lowest ping | 1.1.2 |
| **Favourite rooms** | - | `X` keeps a public room among your rooms (the Server list) and takes it away again; it stays there even when it leaves the public list | 1.1.2 |
| Filters and search | - | `R` the region, `L` all rooms / your games / free seats, `Y` searches by name, `SELECT` asks the rooms again. Tried on a New 2DS XL | 1.1.2 |
| In-game chat | - | the room's chat over the game, in the corner you choose; **`L + Left + Select`** opens a keyboard on the bottom screen. On by default; *Settings → In-game chat* turns it off | 1.1.2 |
| A chat message | - | up to **128 characters**; a longer one is cut to 125 and `...`. The last five messages are kept | 1.1.2 |
| The chat's keyboard | - | it **stays open after Send**, for the next message; **Close** shuts it and keeps the draft. HOME, the power button and the lid close it too, and the draft stays | 1.1.2 |
| The plugin's notices | - | in the corner **opposite** the chat, so they do not cover it | 1.1.2 |
| Who joins and who leaves | nothing told | a line in the chat: "*name* joined the room", "*name* left the room". Tried on two New 3DS | 1.1.2 |
| **The in-game menu, rebuilt** | the framework's menu | 3DS Link's own menu: the room you are in, another room (yours or a public lobby, with its ping), leave or join, the chat's switch and corner, the debug log. Still **off by default**; the standard keys are now **`L + Right + Select`** for the menu (a file that still holds `L+R+Select` takes them) and **`L + Left + Select`** for the chat's keyboard; `B` closes | 1.1.2 |
| How the game is hooked (every console) | on an Old 3DS, two words of the game rewritten at every trip to HOME | one word per call site, written once before the game starts; the real system call is made in a small page that HOME does not take away. On an Old 3DS: the sequence of the *LEGO Batman 2* dumps needs a hook that is written again, and it no longer is (checked on a PC); **not tried on an Old 3DS**. On a New 3DS: it ran in every test | 1.1.2 |
| Old 3DS: HOME and back many times in the same game | from the 13th time the plugin no longer saw the HOME button; a later return could wake the game up in memory that is gone | the plugin keeps seeing it; **not tried on an Old 3DS** | 1.1.2 |
| Old 3DS: a game that opens a system window of its own (an error message, for example), in a room | it could crash with no button pressed ([issue 5](https://github.com/gionko/3DS-Link/issues/5)) | handled like HOME; **not tried on an Old 3DS** | 1.1.2 |
| Old 3DS: a game call on one of the plugin's own sessions while the game is at HOME | it reached the kernel, which answers `0xD8E007F7`: the game could stop with its error screen | the hook's page answers at once with an error of that service, not the kernel's; **not tried on an Old 3DS** | 1.1.2 |
| Old 3DS: the plugin's answer to Luma at HOME | each wait had its own limit, and together they could come close to the 10 s Luma waits | one deadline for all of them; Luma's wait is raised to 15 s in the plugin's own header (no setting of Luma is touched); **not tried on an Old 3DS** | 1.1.2 |
| Closing a game after using the chat | in the test builds, the game stayed in memory and its next launch crashed | fixed: tried on two New 3DS with *Monster Hunter XX* | 1.1.2 |
| A game started right after another one was closed | once, on a test build, it crashed at start in the plugin framework, which had not got its link to Luma's plugin loader | the link is tried again for about 2 s; without it the plugin stays out of the game and says *"restart the game"*. On the consoles the link always came at the first try: the retries ran on a PC only | 1.1.2 |
| "Couldn't set memblock size" on a New 3DS | seen on one New 3DS XL with test builds of 1.1.2, and once the console then stopped | an exit guard puts Luma's state right when the game exits, so that Luma learns that the game ended. Tried in 29 closes on two New 3DS: no crash, no message; **the guard never had to act** | 1.1.2 |
| 3DS Link opened right after a game was closed | it took the free memory to the byte, and in a test build it stopped at start when a little was still held | it waits up to 3 s and starts with the memory there is (`3DSLink-heap.log`); if a closed game is still in memory, a window says to restart the console. The wait was seen on two New 3DS | 1.1.2 |
| *Luigi's Mansion* (2018) | the local multiplayer said the wireless was off | the Download Play lobby is provided inside the room, as for the Kirby games and *Mario Tennis Open*. Tried once between two New 3DS (3DS to 3DS): it played. **With an Azahar player: not tried**. No score yet | 1.1.2 |
| The debug log | | `AzaharLink.luma.log`, the sleep trail kept in `AzaharLink.sleep.1.log` instead of deleted, new lines for the swap, the hook's page and the exit (see below) | 1.1.2 |

### Not fixed or not tried

| What | Notes |
|---|---|
| Everything of this version on an Old 3DS or Old 2DS | **not tried**: the hooks, HOME and the power button, the public lobbies, the chat |
| *Mario Party: Island Tour* | its local multiplayer asks for Download Play too, and still says the wireless is off. The same lobby was tried on two New 3DS and the game did not play with it, so it is not in this version: **for 1.1.3** |
| *Luigi's Mansion* (2018) | **tried once**, 3DS to 3DS with two players; no score yet, not with three or four. **Not tried with an Azahar player.** The Europe copy was seen in logs; the other regions come from a catalogue and were never seen |
| The exit guard | it has **never had to act** on a console: the failure did not come back during the test. It cannot help when the system kills the game without letting it exit, or when Luma's loader has already lost its link to the game (one of the failures seen). It runs on the New 3DS family only |
| C-stick, ZL and ZR while the chat's keyboard is open | the game still receives them in the games that read them through the Circle Pad Pro service (*Monster Hunter*, for example). A known limit |
| Characters in the chat | printable ASCII only: any other character shows as `?` |
| The in-game menu pauses the game | a game with a short network timeout can lose its match while the menu is open |
| Public lobbies: how a room is asked | it relies on how an Azahar room answers someone who has not joined; a future Azahar may change it, and then the players show `-` and the ping stays. Each check makes the room send its information again to everyone in it, the players too: what that costs a game in play was not measured |
| *Fantasy Life* | still no room: the game opens the console's network service before the plugin, and the plugin then has no network. A fix is planned for 1.1.3 |
| `0xD8E007F7` from `csnd`, once, at the 22nd HOME without a room, on an Old 3DS | no dump; the plugin was already blind to HOME then. **Not known if the plugin has anything to do with it** |
| The power button during a game on an Old 3DS, then opening the game again | the game did not come back, and opening it again stopped Luma's loader with `0xD86007F3` (no memory). **Open** |

### Technical notes

<details>
<summary>Public lobbies</summary>

**The list.** A text file in a public repository of the project, `public-lobbies.txt`, made twice a day from the public list of Azahar rooms and a list kept by hand. The app downloads it when the page opens, and keeps a copy on the card; a failed download shows no error. The names and passwords you give to the rooms are in a file of your own on the card (`public-lobbies-local.txt`), which the download never touches.

**Asking a room without joining it.** An Azahar room sends its information to every peer connected to it when one of them sends a game info packet, and answers a join request with a version no room speaks with *full*, *wrong password* or *another version* without letting anyone in. So the app reads the name, the seats, the players and their games, the password and the ping of a room without entering it, with no nickname and no MAC of the console. Eight rooms are asked at a time; a room that does not take the connection in 2.5 s is offline. The rooms are in order: those with a free seat first (the lowest ping first, then the fuller room), then those not asked yet, then the full ones; offline ones at the bottom in red.

**The games.** `titles.txt` maps title ids to games and regions, so the page can show who plays a game you have (in another region, a warning). The last pass is written to `public-lobbies-ping.txt`, which the in-game menu reads for its own list: the plugin never asks a room itself.

**A public room among yours.** `X` keeps it in the Server list (one of the ten places); there only its name can change, and it stays even when it leaves the public list.

</details>

<details>
<summary>The chat</summary>

**What it is.** The room's own chat, the one Azahar has in its room window. A message goes out on the room's reliable channel; the room does not send it back, so the plugin adds your own line by itself.

**Limits.** At most four messages a second are read, two of them from one member: a member who floods the room costs a few compares per packet. Text is cleaned to printable ASCII (the font has nothing else), spaces are compressed, an empty message is dropped. The font is Spleen by Frederic Cambus (BSD-2), reduced to 6x10 so that a message and its name fit in three lines across the whole width of the top screen.

**While you type.** The game reads a neutral copy of its own input page, so the keys you press for the keyboard do not reach it; the copy is given back when the keyboard closes. HOME, the power button, the lid and the end of the game close the keyboard first and give the input back. The C-stick, ZL and ZR come from another service and are not covered.

**Who joins and who leaves.** The plugin compares the room's list of members each time it changes and writes a line with no name before it, drawn like a notice. Never yourself.

</details>

<details>
<summary>The hook's page: one word, written once</summary>

**The defect.** Up to 1.1.1 a hook was two words of the game's code: a jump and its address. On an Old 3DS the plugin put the game's own instructions back before HOME and wrote the hook again after it. A thread of the game asleep in its `svc` at that site woke up on the second word, ran an address as an instruction and lost the real one. In the *LEGO Batman 2* dumps the site is a shared function `svc; bx lr`, so its thread is always asleep there at HOME; the "handle 0" crashes (`0xD8E007F7`) of 1.0.15 and 1.1.0 come from the same kind of window (see 1.1.1). 1.1.1 hooked the sites again earlier, at a higher priority, before the game's thread ran: on a shared site like this one, where a thread is always asleep at HOME, that made the crash almost certain. It was a regression of 1.1.1 on an Old 3DS; 1.1.1 itself said that a hook of one word would make it impossible.

**The fix.** Each call site becomes one word, `b stub`, written once before the game starts and never touched again. The stub and a small gate in assembly (`Sources/ctr/gate.s`) are in a page of the game's process below the plugin's memory, which Luma does not take away at HOME. Every real system call is made inside the gate. At HOME only a flag changes: with it off, the gate makes the game's call itself without the plugin. A sleeping thread always wakes up on the same instructions it went in through.

**Where the page comes from.** With Luma3DS 13.0 or newer, on an Old and on a New 3DS, it is taken from the SYSTEM region with Luma's `svcControlMemoryUnsafe` and put on the kernel's books, so that the game keeps all the memory it counts on. If that fails, the game runs with no hooks for that launch and a notice says so. A test build used an alias of the plugin's memory on a New 3DS: on a console the alias outlived the game, and reopening it gave "Cannot map plugin memory" until a reboot, so the alias is used only on Azahar and on a New 3DS below Luma 13.0. An Old 3DS below Luma 13.0 gets no hooks (the app already says "Needs Luma3DS 13.0 or newer").

**Monster Hunter XX on an Old 3DS.** The game fills its whole region, so the socket buffer taken at the first connection cannot always be reserved through the kernel there; then the plugin writes the books itself, as 1.0.13 to 1.1.1 did. **Not tried on an Old 3DS.**

**How it was tested.** `gate_sim` runs the compiled gate under an ARM emulator through 97,922 interleavings of the game's threads and the swap, with no failure (and, since the fake sessions, with the plugin's memory gone), `lb2_dump_sim` reproduces the dumps on the old hooks, and the mutants of the gate are all caught. Luma and the kernel are not in any of them.

</details>

<details>
<summary>Old 3DS: HOME many times, system windows, fake sessions, one deadline</summary>

**HOME many times.** The plugin follows the game's sessions with a few system services, among them `APT:A`, the one that carries the HOME button. The table had 16 places. The game opens an `APT:A` session for every APT call, about 13 at each trip to HOME, and closes it at once; around the swap of an Old 3DS some of those closes happen out of the plugin's sight, one place lost per trip. After twelve trips the table was full. Now a new session whose handle has the same index in the kernel's table frees the old entry, which must be closed, and when the table is full an `APT:A` session takes the place of the one used least recently.

**System windows.** Luma swaps the plugin's memory out at every `APT:ReceiveParameter` that carries a command response, also when the game opened a library applet or an error window itself (a dump of issue 5). The plugin hands that call back to the game's own site in every case, not only after a button.

**Fake sessions.** The plugin answers some services in place of the system (`nwm::UDS`, `ndm:u`, the Download Play ones). While the game is at HOME the plugin's memory is gone; a call on one of those sessions went to the kernel, which does not know them and answers `0xD8E007F7`, and the game's SDK stops on its error screen. The gate now holds up to seven of those sessions with the error of the real service (`0xC8A113EA`, `0xC8A0680E`, `0xC940AC04`) and answers by itself, with no wait; a session the game closes meanwhile is only marked, and the plugin forgets it when it is back. For `ndm:u` that error also answers a few commands that the real service would accept: better than `0xD8E007F7`, and to be looked at if a game stops there.

**One deadline.** At HOME the plugin must answer Luma before Luma gives up, 10 s after the event. The chat's input, the trail, the threads inside the hooks and the plugin's own threads each had their own limit. Now they share one deadline, counted from the game's call that started the swap: 8 s. On an Old 3DS linked to the plugin loader, the plugin raises the wait in its own copy of the plugin header from 10 to 15 s (only when it is exactly the 10 s Luma writes), and the deadline is then 12.5 s. No setting of Luma or Rosalina is changed.

All of this was tested on a PC and in the gate simulator: **not tried on an Old 3DS or Old 2DS.**

</details>

<details>
<summary>The game left in memory after the chat</summary>

In the test builds, a game in which the chat's keyboard had been opened stayed in memory after HOME → Close: 4 times out of 4 with the keyboard used, 0 out of 5 without. The next launch of a game that sizes its heap on the free memory, *Monster Hunter XX*, found none and stopped at once. The cause: to count the game's threads the input copy called `svcGetThreadList`, and the kernel takes a reference to the process there and never gives it back. The process ended but was never freed. The threads are now counted with `svcGetProcessInfo` (type 7), which gives the reference back, and a rule of the policy check forbids `svcGetThreadList` in the plugin and in the app. Tried on two New 3DS with *Monster Hunter XX* in the same room and the chat used as before: the game never stayed, and the next launch found all its memory.

3DS Link also checks at start whether a closed game is still in memory, writes what it sees in `3DSLink-census.log` and shows the window *A game is still in memory*.

</details>

<details>
<summary>The link to Luma's plugin loader</summary>

On a New 2DS XL, a game started right after another one was closed crashed at start inside the plugin framework: its main thread had not got a session to Luma's plugin loader (`plg:ldr`), ignored the failure, and read address 0 the first time it asked for an event. Why the loader's port was busy is not known. The plugin now wraps that link: up to 100 tries 20 ms apart, and a try counts only with a session and its arbiter. Asking for an event with no link answers "no event". If the link never comes, the plugin stays out of the game (no capture, no socket buffer, no hooks) and says once on the screen: *"3DS Link: the plugin could not talk to Luma's plugin loader: restart the game"*. A line goes to `AzaharLink.log` at once, at every debug level, when it took more than one try or failed.

</details>

<details>
<summary>The exit guard ("Couldn't set memblock size")</summary>

**The failure.** Luma frees the plugin's memory of a game only when that game ends with `svcExitProcess`, and the kernel tells Rosalina only if Luma's state word for the plugin says "running" with no event pending. On one New 3DS XL, with test builds, Rosalina never learned that a game closed from HOME had ended: the next game got "Couldn't set memblock size" (`0xD8A077F9`), and once Luma's loader then ran out of memory and the console stopped until it was switched off. What the word held then is not known: the debug log of those builds read it through the cache, and those reads turned out to be stale. The plugin does not write that word, and nothing showed that it caused the failure.

**The guard.** On a New 3DS with Luma, each `svc 0x03` of the game's code (ExitProcess) becomes a jump to a small trampoline in the hook's page, written once before the game starts. The first thread that exits runs the guard on a stack of the plugin: it waits up to 1 s for Rosalina to be ready, answers in place of the framework if Rosalina is still waiting for an answer about HOME, writes its lines through the trail's thread (at most 300 ms), and at the last look sets the word back to "running" if it reads 0 ("no plugin"), 2, or carries an old event. It never touches an exit that is already raised or a value that is not Luma's. Then the game's exit goes on. Never on an Old 3DS or an emulator.

**Tried.** 29 exits on two New 3DS: each time the word was already clean, Rosalina sent the exit by itself, and the guard had nothing to do. So its action is tried on a PC only: `Tests/exit_guard_test.cpp` runs the same `Guard()` against a model of Luma, where without it the exit is lost in six cases of the field and with it reaches Rosalina in all six.

</details>

<details>
<summary>Luigi's Mansion (2018) and Mario Party: Island Tour</summary>

Both open the Download Play service as soon as local multiplayer is chosen: *Luigi's Mansion* `dlp:SRVR` and `dlp:FKCL` InitializeWithName, *Island Tour* `dlp:SRVR` Initialize, all answered `0xC940AC04` (wireless off) in the logs. It is the pattern of the Kirby games and *Mario Tennis Open*, so *Luigi's Mansion* is now in the plugin's table of lobby games: Europe (seen), USA, Japan and Korea (from 3DS Link's catalogue, not seen). The friend code seed is the lobby's, as for the Kirby games; *Mario Tennis Open* needed the console's own, so if a console stops at the synchronisation, that is the first thing to look at. With the lobby it played between two New 3DS (3DS to 3DS; not tried with an Azahar player). *Island Tour* was given the same lobby in a test build and did not play with it: it was taken out of the table again and waits for 1.1.3. *Luigi's Mansion 2* (Dark Moon) is another game, which plays without the lobby (see [Tested games](README.md#tested-games)).

</details>

<details>
<summary>New lines in the debug log</summary>

* `AzaharLink.luma.log`: written to the card at once, with the debug log off too: a line when the exit guard is installed, and one the first time Luma's state word reads "none" while the game runs (title, process, seconds since the start).
* `AzaharLink.log`: `startup: a closed game is still in memory`, at every debug level; the line about the plugin loader's link (above).
* The sleep trail (`AzaharLink.sleep.log`): kept in `AzaharLink.sleep.1.log` instead of deleted, written in batches, with a header (title, process, start time) and a count of the lines lost to a full queue. New entries for HOME, the return and the exit with Luma's state word, read from memory that bypasses the cache (the first reads came from the cache and could be stale), and the five lines of the exit guard.
* `swap: our part answered N ms after ...`, with the deadline and the waits it cut; `swap: gate open` with the count of fake sessions answered; `gate: page ... SYSTEM` at every start, or why there are no hooks; the `hook swap:` line counts the sessions the plugin had to forget or replace.
* With the debug log on, at the events only: `res`, `ref`, `threads`, `handles at EXIT`, `vm`.
* 3DS Link: `3DSLink-heap.log` (the memory it took at start, and whether it had to wait), `3DSLink-census.log`.

</details>

<details>
<summary>Found in the test builds of 1.1.2 and fixed before this release</summary>

* With the in-game menu open, HOME made *Super Street Fighter IV* stop with `svcBreak` on two consoles: the menu took the HOME notification away, and the game's notification thread stops on an empty one. The plugin never changes a notification now, and a rule of the policy check holds it.
* With the chat's keyboard open, HOME, the power button or the lid stopped the game (found on the PC, before any console).
* The hook's page as an alias on a New 3DS: "Cannot map plugin memory" when the game was opened again, until a reboot (above).
* The game left in memory after the chat (above).
* 3DS Link stopping at start after a game was closed (the memory it takes at start, above).

</details>

## 1.1.1 - 2026-11-03

**On an Old 3DS: two crashes when coming back from HOME or the power button, and a debug log that says much more. Not tried on an Old 3DS yet. Games on a New 3DS behave as in 1.1.0.**

> Two users tried 1.1.0 on an Old 3DS ([issue 5](https://github.com/gionko/3DS-Link/issues/5) and the GBAtemp thread) and sent logs, dumps and photos. Two crashes came out of them, in Pokémon Alpha Sapphire and in My Hero Academia. This package was tried on two New 2DS XL in a room with Super Street Fighter IV (HOME, the power button, the lid, with the debug log on): nothing broke.

### At a glance

| Area | Before | Now | Since |
|---|---|---|---|
| Old 3DS: the power button during a game, when the system's answer carries other bits (`0x608`) | not recognized: the game could wake up in memory that is gone | recognized; a fix, **not tried on an Old 3DS** | 1.1.1 |
| Old 3DS: `0xD8E007F7` when the game comes back from HOME or the power menu, or when it is closed from HOME | seen on 1.0.15 and on 1.1.0, now and then | a fix, **not tried on an Old 3DS** (see below) | 1.1.1 |
| A failed call to ask for the notification | its old answer could look like a button | ignored | 1.1.1 |
| The debug log on an Old 3DS | little about the swap, and the lines of a crash could be lost | the swap and the memory at each one, the calls around it, the calls that fail for good, written to the card at once | 1.1.1 |
| New 3DS family | | with the debug log off, the games run as in 1.1.0; with it on, more lines. The lid's sleep is recognized with those bits too, which only matters with *End session on sleep* on | |

### Not fixed or not tried

| What | Notes |
|---|---|
| These fixes on an Old 3DS or Old 2DS | **not tried**: these paths do not run on a New 3DS |
| `0xD8C3FBF3` from the HOME menu, once, at the tenth HOME | seen once on 1.1.0. **Not fixed, and not known if the plugin has anything to do with it** |
| The `kernel panic` at HOME | not reported on 1.1.0 so far, but the attempts of those reports left no log: **not settled** |

### Technical notes

<details>
<summary>0xD8E007F7 on handle 0: an instruction lost when the plugin hooks the game again</summary>

On an Old 3DS the plugin puts the game's own `svc` instructions back before Luma takes its memory away, and hooks them again when the memory is back. Hooking a call site writes two words. Luma lets the plugin's threads run before the game's thread that went to HOME, but the plugin only hooked the game again later, when the framework's callback ran. By then the game's thread was opening one session after another. If it was asleep in its own `svc` at a call site while that site was rewritten, it woke up and ran the second word in place of the instruction that checked the result: the session it had just opened was never saved, and the next call went out on handle 0.

The sites now go back in the hook as soon as the memory is back, at a higher priority, before the game's thread runs. The sequence was reproduced on the PC with the SDK code of five recent games, not on a console. A hook of a single word, which would make this impossible, is for a later version.

</details>

## 1.1.0 - 2026-11-02

**Three fixes for the HOME button and the power button on an Old 3DS, not tried on one yet. Nothing changes for the games on a New 3DS.**

> Two users with an Old 3DS sent the same crash: a `kernel panic` with `socket` as the current process, on 1.0.4 ([issue 5](https://github.com/gionko/3DS-Link/issues/5)) and on 1.0.15 ([issue 3](https://github.com/gionko/3DS-Link/issues/3)), with two different games. It is not the crash of HOME fixed in 1.0.12. These fixes were tested on a PC, on the compiled code and against a real room: **not tried on any Old 3DS or Old 2DS**, where they run. Tried on two New 2DS XL, debug log off, to see that nothing changes there: *Super Street Fighter IV 3D Edition*, a match over the internet, HOME and back, HOME and close, HOME and power off, no problems. A console with 1.0.14 or 1.0.15 is offered this version by the app; an older one installs it by hand.

The number is 1.1.0 and not 1.0.16 because the third number of a 3DS title stops at 15.

### At a glance

| Area | Before | Now | Since |
|---|---|---|---|
| Old 3DS: HOME while in a room | `kernel panic`, the console stops | a fix, **not tried on an Old 3DS** | 1.1.0 |
| Old 3DS: the log's writer and the sleep's trail at HOME | the same risk, never seen in a dump | they wait at the same gate | 1.1.0 |
| Old 3DS: the game fails right after coming back from HOME, or when powering off from the HOME menu (`0xD8E007F7`, [issue 5](https://github.com/gionko/3DS-Link/issues/5)) | seen on 1.0.15 | a real risk removed; **it is not proven to be the cause** and it was not tried on an Old 3DS | 1.1.0 |
| *Monster Hunter XX* on an Old 3DS | closed at launch | a user reports a whole mission on 1.0.15 (copy with a Spanish patch) | 1.0.13 |
| *Terraria* (Europe, update 1.0.5), *Super Smash Bros. for Nintendo 3DS* (Europe) | | new in [Tested games](README.md#tested-games): **10/10**, two consoles of the New 3DS family over the internet, on 1.0.15 | |
| Old 3DS: the framework's worker thread beginning in memory that is gone (prefetch abort at `_thread_begin`, core 1, [issue 5](https://github.com/gionko/3DS-Link/issues/5)) | seen on an Old 3DS | a fix, **not tried on an Old 3DS** | 1.1.0 |
| New 3DS family | | the games behave as in 1.0.15: the gate stays off and the worker does not move | |

### Not fixed or not tried

| What | Notes |
|---|---|
| These fixes on an Old 3DS or Old 2DS | **not tried**: these paths do not run on a New 3DS |
| Back from HOME on an Old 3DS, in a room | the room may have dropped the console meanwhile, and the plugin should join again: **never seen** in this situation |
| A network call that lasts more than 7 s at that moment (a room's name with no DNS) | the swap then goes on as before this fix, so that crash remains possible there; the debug log says who was left |
| The power button on an Old 3DS (1.0.15) | a log from an Old 3DS shows it going through; another user got a dump once, and not after a full reboot: not settled |
| A game whose only `svc 0x32` is one shared function, on an Old 3DS | that site cannot wait out of the hook, so the risk of the swap back stays there |
| The update on an Old 3DS or Old 2DS | **not tried**: the secure connection takes longer there, and how long is not known |
| *Mario Tennis Open*, USA copy on an Old 3DS: "the Wi-Fi switch is off" after the online option | [issue 2](https://github.com/gionko/3DS-Link/issues/2): not confirmed fixed, not tried on an Old 3DS (the Europe copy plays between two New 2DS XL) |
| *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D*, *Ocarina of Time 3D* close at launch (`0xE0A01839`) | on one console (a New 3DS XL): [Known limits](README.md#known-limits) |
| *Balloon Fight* (Virtual Console) closes at startup | the plugin framework aborts in Virtual Console games: a known limit |

### Technical notes

<details>
<summary>The kernel panic at HOME: stopping a thread does not take back its service call</summary>

**The crash.** On an Old 3DS, when a game goes to the HOME menu, Luma stops the plugin's threads and unmaps the plugin's memory. A stopped thread can still have a service call under way. The room's network thread sits in `poll()` about 91% of the time (133 windows of the log of issue 3). `socket` then answers, the kernel writes the answer into the buffer of `poll()`, which is in the plugin's heap, finds the page gone, and stops the console. So the current process is `socket`, though it did nothing wrong.

**What the dump says.** The arguments on the stack are a reply with 2 normal words and 2 translated ones, to a client's static buffer of 12 bytes: one `struct pollfd`. The destination is inside the plugin's heap, and the check that failed is the first one, on the client's memory. The photo in issue 5 has the same layout.

**Why 1.0.12 did not cover it.** That fix takes the game's threads out of the hook. The network thread never was in the hook, and nothing stopped it.

**The fix.** A gate (`Includes/azl/swap_gate.hpp`, `Sources/ctr/swap_gate.cpp`). At `SWAP_ENTER`, after the hooks are out and before the framework answers Luma, it closes: who is inside a call is waited for, for 7 s at most (Luma waits for 10), and who comes later stops on an event until the gate opens. It covers the twelve socket calls of the network thread that move bytes through the plugin's memory (`sendto` too: the kernel reads a request's buffer when the service picks it up), a pass of the log's writer, which also leaves the card after the write in hand, and the lines of the sleep's trail. It opens at `SWAP_EXIT` and at the plugin's shutdown. On a New 3DS it is one test of a flag, since Luma swaps on an Old 3DS only.

**How it was tested.** `Tests/swap_gate_test.cpp` (real threads, ThreadSanitizer) with `Tests/swap_gate_mutants.py`, a new scenario of `Tests/chain_sim/chain_sim.py` on the compiled ARM code, and `Tests/integration.sh` against a real room. Luma and the kernel are not in any of them.

**The swap back.** A hook is two words: a jump and the address it jumps to. The thread that brings the block back sleeps inside the game's own `svc`, with the hooks out, and wakes up at the word after it. The framework, woken at the same moment, puts the hooks back: if it comes first, that word is the address, run as an instruction, and the game's next instruction is skipped. So the plugin notes where the game makes its `APT:ReceiveParameter`, leaves those sites to the game when the hooks go back, and hooks them 2 s later. On issue 5 a log shows an APT call with handle 0 right after the swap back, which fails with `0xD8E007F7`: this may be the cause, and it is not proven (another launch went through the same path unharmed, and the game's code was not read). A site that is only `svc; bx lr` is every call of the game, so it goes back at once: it stays exposed.

**The worker that had not begun.** The plugin framework starts one idle worker on core 1. A thread there runs only when the game leaves that core some time, so it may not have begun when the game goes to HOME. Luma knows a thread of the plugin by a mark that the thread itself writes when it begins, so a thread that has not begun is not stopped, and it then begins in memory that is gone. On an Old 3DS the worker now goes to core 0, where it begins at once and is stopped with the others (`Includes/ctr/thread_core.hpp`). The move runs only on the console where Luma swaps.

**In the debug log.** Back from HOME, the line `swap gate: in a service call at the start: ...` says who was inside, who was left and how long it waited.

</details>

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
