#!/bin/bash
# Builds the host-side tests (meant to run inside a Linux container with g++) and runs the unit
# tests. With BUILD_ONLY=1 it only builds (Tests/integration.sh then runs the integration binary).
# test_main covers the core, the engine's lifecycle (every command's result, events and frames), the room link
# lost under a session and the players who leave the room, the
# scan (waits first, keeps the list, cancel event, wlancommid/id8 counted), stale nodes and the host's
# Deauthentication, the host forward rule, the beacon-sync shadow, the Azahar-peer rule checker, the
# Azahar host model, the debug-capture lines and the role lines, a Connect that is woken after the game began another,
# the repeated Start of a closed lobby, the spectator role, and the CPU/instrumentation items:
# the tick conversion, the bounded TX queue, the engine-lock and statistics counters, and the early
# drop of frames that are not for us (identical to dropping them after the decoder). ring_test covers the
# receive path of a bind: the byte ring against a model of nwm's, the bind table, binds that survive a session
# while their frames do not, the exit race and the load of two real FFE captures with a stalled game.
# dedupe_test covers the removal of the copies an Azahar host re-broadcasts (DEDUPE_TRACES="a.csv b.csv" adds
# the replay of real captures). link_policy_test covers the decisions between the room link and the game's session:
# when a lost link ends the session or waits for the room (our own pause of the network thread), the schedule and
# the rules of the attempts to join again, the screen messages and the sleep notification, and the log lines for the
# players that come into the room and leave it (azl/room_members.hpp). soc_plan_test covers
# the socket block placement and the SO_RCVBUF policy (hard limit, ring budget, ini key, retry ladder against a
# model of the soc service). soc_send_test covers the count of the datagrams handed to the room socket (azl/stats.hpp, soc_send_*:
# taken whole, refused with EWOULDBLOCK, failed, taken short) against the plugin's OWN ENet platform layer, Sources/enet/enet_3ds.c,
# with the socket call under it answered by the test: what enet_socket_send returns, the errno it leaves, what TakeStats() gives.
# log_files_test covers the log and what it costs when it is off: the ini values (debug 0
# or 1, menu_key off), Log() and the capture doing nothing and allocating nothing at debug 0, the log's files on a
# directory (none created at 0, 0 -> 1 -> 0 -> 1 with the writer running, rotation), the first line of every capture file
# (the console's clock), the bounded passes of the capture's drain and the diet's and the LOST lines in the file, the
# numbers of the status page and the wrap of the messages on the screen. settings_test builds the 3DS Link app's own ConfigApp/source/settings.cpp
# on the PC: debug and menu_key as they are read, carried and written back, the list of saved rooms (the rule of the keyboard, most
# recently used first, no copies, 10 at most, the file) and the version word of a 3gx header. games_test builds the app's
# ConfigApp/source/games.cpp and status.cpp the same way (plugin_install_test builds plugin_install.cpp: the plugin the app carries and puts on the card, byte for byte): the title id as Luma spells it and which titles are games, the SMDH (name,
# language, icon layout), the order of the list, a game's folder (off, on, another plugin), enabling and disabling a game (the copy
# of the plugin, never half a copy, the empty folder), a stale copy, the installation check (what to copy where, optional files never
# counted), the Plugin Loader question and the short texts of the main page, and the cache of the games (name and icon). ncch_icon_test
# builds ConfigApp/source/ncch_icon.cpp (the keyed reading of a game's icon file) with TitleShare's own aes, sdcrypt, sha256,
# contentpath and ncchname (the vendored copies) against a made-up title kept as a console keeps it (SD layer and NCCH layer). The
# tests that drive the IPC front-end (UdsHle) need a 32-bit build: the FFE golden replay (the 3DS as a client of an Azahar host
# with two Azahar client models around it), the replays of the real traces of the console (trace_replay_test: FFE and TFH as
# host, the lid episode with a second lobby of the game in the room, a client stretch; their data are Tests/*_golden.inc and
# Tests/lobby_trace.inc, cut out of the captures by Tests/make_goldens.py), the
# result-set closure, the command x state matrix and the ndm:u wiring are built by Tests/ipc_test/build.sh
# and run by Tests/hle_tests.sh (alone) or Tests/integration.sh (with the room server). test_main also covers
# NdmHle: the firmware's exclusive state, daemon and scheduler counts, sessions and result codes.
# sleep_crumbs_test covers the trail of a sleep (AzaharLink.sleep.log): the events of the sleep protocol as lines, the wake marker
# from the console's clock, the ring under overflow and two producers, and the close of a game (Luma3DS's plugin word at HOME and EXIT, after
# ReceiveParameter, the termination request, the lines after a close command). plg_guard_test covers the guard of the framework's link to
# Luma3DS's plugin loader (azl/plg_guard.hpp): the tries with a fake plgLdrInit, the arbiter, the fetch that answers 0 and never connects, the line.
# line_test covers the text of the code that runs on the game's threads: azl/line.hpp's pieces, LogLine / DebugLine, the
# raw records of the hook's two pass-through lines and the heads of the UDS/ndm capture against the printf formats they
# replaced, and the console clock against gmtime (no printf on a game thread: Tests/policy_check.sh, Tests/stack_usage.sh).
# capture_test covers the debug capture's ring: the two lanes (control records keep their room when a flood of frames fills the
# rest), the LOST markers, a producer that never waits for the ring's lock, the diet of the IPC lines, the raw records of the
# HLE proper's commands against the printf formats, the header-only frame records. line_test also holds the formatter of
# Log() / DebugEvent() (azl/printf_lite.hpp) against newlib's, and the formats of every Log() call in the sources.
# screen_text_test covers what the screen says about the game's memory (the reading, the mode, the limits, when a warning is told and
# told again) and about frames lost (too slow a reader or no reader at all, from the game's UDS calls).
# chat_core_test covers the chat of the room (azl/chat_core.hpp): the cleaning and the cut at 128 of what the room relays, a 5000-character
# message that costs what 128 do, malformed packets, the ring of 5 and its seqlock (two threads; built again with ThreadSanitizer), the rate
# limit (50 a second -> 4, 2 of one member: a flood leaves the others theirs), the outgoing box (one a second, only while joined) and no
# allocation (malloc wrapped by the link). chat_room_probe is the same chat through RoomClient against a real room (Tests/integration.sh).
# osk_test covers the plugin's one on-screen keyboard (azl/osk.hpp and azl/osk_draw.hpp): its table against the system keyboard's
# measures and the hit-test of every pixel, typing by touch and by the pad, the repeat, shift, caps lock and the pages, the limits, the
# password's mask, the scroll, no allocation, and its drawing in the five formats (what changes when a key is lit is inside that key).
# chat_host_test covers the chat over the running game (azl/chat_host.hpp): the notifications and events that close the keyboard (the low
# byte, answered only) and the rules of the reopening, the chord, the pace (30 jobs a second at most) and the latch of the keys between two
# jobs, the handshake that frees the keyboard's picture only when no copy reads it (two threads; built again with ThreadSanitizer), the copy
# onto the framebuffer (words only, the visible pixels only, in the five formats) and what the OSD callback draws, with no allocation.
# hid_mirror_test covers the input mirror page (azl/hid_mirror.hpp): the mirror made from the real HID page (the pad's and the touch's entries
# neutral, "released" for 2 samples, the headers, the accelerometer and the gyroscope live), the look that says nothing is held and the wait
# for the release (50 ms, at most 1 s), the search of the game's mappings by physical address, and the switch and the way back over a
# simulated kernel (the threads held across every change, the neighbours the kernel would fold the mirror into, the checks after the map and
# the unmap, the second try, the mirror again or the address left empty at the swap).
# dlp_lobby_test, dlp_ipc_test, dlp_lobby_sim_test, dlp_adv_lobby_test and dlp_adv_ipc_test cover the fake Download Play lobby (azl/dlp_types.hpp, dlp_lobby.hpp and
# dlp_ipc.hpp: pure headers, no engine, no kernel, never more than 4 players, no heap): the searcher's and the creator's logic over an in-memory virtual network
# (every command x state, the four messages lost, doubled and late, the timers on a clock that wraps), the IPC front-end (the words of every command, the records
# byte by byte, the game's buffers with canaries, ProbeOnly against DlpFkclHle), whole rooms of 2 to 4 consoles running the command sequences of Battle Royale and
# Triple Deluxe, and the same modules attacked (the ambiguities, a faulty engine, bad pointers, threads). They stop at the first undefined behaviour.
# dlp_net_adv_test is its adversarial half (the engine a game has, failures and stale lobbies, the ring, six nodes, a spectator, the beacons the engine files, another lobby of the game in the room, a peer
# that leaves or is kicked, the room's member lists, a service that stops, admissions closed and never accepted); both use Tests/dlp_net_rig.hpp.
# dlp_net_churn_test is the third (2026-10-29, the second review round: a console that is Connecting again when a fellow's departure refresh arrives, a leave / rejoin / leave inside 3 s,
# the guards of the refresh, a lobby closed to new players, and a churn fuzz of random joins, leaves, kicks and rejoins after which every roster and every message must agree).
# dlp_net_extra_test is the fourth: what the second mutation round showed missing (the table of lobbies heard, an engine no lobby used, the ring, who gets the departure's Logoff,
# a game's host and a lobby client staying silent, foreign Logoffs and node maps, the creator's goodbye, the game's own connect path, the debug capture, 24.8 days of silence, no memory).
# dlp_real_engine_test (1.1.3, phase F2) is the engine's half of the REAL Download Play: the beacons filed by kind (SetLobbyBeaconKind), the network the engine composes
# (BeginHostingDlp / ConnectBeginDlp), the spectators counted and never nodes, the channels 6 / 11, the rules of D16, the churn and extra scenarios redone in that network and
# the two dialects that do not see each other (about 3 minutes, real time).
# dlp_net_test covers the same lobby over the REAL UDS engine (phase 2: azl/dlp_lobby_net.hpp, the adapter, and the pieces the engine got for it): consoles that are engines joined by the in-memory room, the
# lobby heard and joined, the whole sequence of Triple Deluxe and then the game's own UDS on the same engines (clean), an admission closed or full, four players, a client that leaves and a creator that goes,
# the game that starts nwm::UDS under the lobby, two lobbies in one room, the messages of strangers and copies, the ring of 8, and the calls that must allocate nothing. dlp_room_probe is the same against a
# real room server (Tests/integration.sh). Tests/dlp_net_stack_check.sh builds the adapter and the engine for the plugin's target and holds the frames and the static storage.
# Tests/dlp_stack_check.sh (with Tests/dlp_stack_probe.cpp) builds the same three headers for the plugin's own target, with arm-none-eabi-g++ and the flags of the Makefile, and holds what the x86-64
# tests cannot see: the frame of every function (48 bytes, PollScan and Dispatch 64), the frames added up from each function down to its deepest call out of the module (160), no recursion, no dynamic
# frame, the sizes on ARM32 (the lobby 400 bytes, the service 40) and a wrapper for every public function. It looks for the compiler in /opt/devkitpro/devkitARM/bin too, and is skipped aloud where there is none.
# Tests/policy_check.sh (run first) reads the sources for the defaults and switches that must not change by accident:
# sleep_stop off, debug 0, rcvbuf_kb 64, the ini keys the plugin knows, the scan filter that only counts, the one
# place where a log file is opened and the level check in front of it.
set -e
cd "$(dirname "$0")/.."
bash Tests/policy_check.sh
OUT=${OUT:-/tmp/azl_tests}
mkdir -p "$OUT"
CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=address,undefined"
CFLAGS="-O1 -g -IIncludes -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_SOCKLEN_T=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -w"

for f in callbacks compress host list packet peer protocol; do
  gcc $CFLAGS -c Sources/enet/$f.c -o "$OUT/enet_$f.o"
done
gcc $CFLAGS -c third_party/enet/unix.c -o "$OUT/enet_unix.o"

CORE="Sources/azl/crypto.cpp Sources/azl/log.cpp Sources/azl/platform.cpp Sources/azl/room_proto.cpp
      Sources/azl/uds_frames.cpp Sources/azl/uds_engine.cpp Sources/azl/room_client.cpp
      Sources/azl/ndm_hle.cpp Sources/azl/log_files.cpp"

g++ $CXXFLAGS Tests/test_main.cpp $CORE "$OUT"/enet_*.o -o "$OUT/test_main"
g++ $CXXFLAGS Tests/ring_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/ring_test"
g++ $CXXFLAGS Tests/dedupe_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/dedupe_test"
g++ $CXXFLAGS Tests/link_policy_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/link_policy_test"
# beacon_cache_test (2026-11-10): the host's beacon is made again only when its network changed (UdsEngine::BeaconFrameLocked), byte for byte against
# GenerateBeaconFrame in a game's network, the fake lobby's and the real Download Play's; it counts the SHA-1 calls through the linker's --wrap
g++ $CXXFLAGS -DAZL_HAS_BEACON_CACHE -Wl,--wrap=_ZN3azl4Sha1EPKhmPh Tests/beacon_cache_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/beacon_cache_test"
# uds_eight_test (2026-11-13): the game's OWN nwm::UDS network with EIGHT nodes (Tetris; Mario Kart 7 after its lobby), which no console session has shown yet: a host and
# seven clients over the in-memory room - the tables of every node after every join (and the host's beacon, whose node list needs two tags), the ninth, every frame once and
# only where it must, who leaves and comes back, the host's end, seven joins in one round trip, the guests that say goodbye twice, and what 8 x 60 frames a second cost the
# room and a bind's ring. Its KNOWN_GAP_EQ lines hold the open points as the engine has them today (GB-01, a full host's answer, the table of who left: analysis/lobby-otto-2026-11-13/revisione-uds-normale-a-otto)
g++ $CXXFLAGS Tests/uds_eight_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/uds_eight_test"
# enet_path_test: the ENet client's data path (what a service call reads, when the acknowledgements go, the counters); the link wraps
# the platform layer's three socket calls so that the test can count them, and ENet's clock so that it can hold it
g++ $CXXFLAGS Tests/enet_path_test.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_socket_receive -Wl,--wrap=enet_socket_send -Wl,--wrap=enet_socket_wait -Wl,--wrap=enet_time_get -o "$OUT/enet_path_test"
# enet_cap_test and flow_relief_test (2026-11-14, Mario Party: Island Tour hosted by a console): ENet's limit of waiting data and its dead end - the state made by hand with
# datagrams written byte by byte, the way out, the bound, the counters, the lag of the stream - and the relief of a dense broadcast stream for the listed titles
# (azl/flow_relief.hpp: the list, the same-frame filter alone and in UdsEngine::SendTo, the hand-over burst of RoomClient::Run, the statistics line). The model's part
# is Tests/vsim/island_dice.sh (it needs ENet 1.3.18 for the room's side: not run from here).
# (2026-11-15) enet_cap_test links an ENet of its own built with the address sanitizer too: the C objects above are not, so a read of a placeholder of the
# drop at the limit after the dispatch freed it (enet_protocol_handle_send_fragment) passed unseen there; with these it stops the test (TestTheDropInOrder)
for f in callbacks compress host list packet peer protocol; do
  gcc $CFLAGS -fsanitize=address,undefined -c Sources/enet/$f.c -o "$OUT/asan_enet_$f.o"
done
gcc $CFLAGS -fsanitize=address,undefined -c third_party/enet/unix.c -o "$OUT/asan_enet_unix.o"
g++ $CXXFLAGS Tests/enet_cap_test.cpp $CORE "$OUT"/asan_enet_*.o -Wl,--wrap=enet_socket_send -Wl,--wrap=enet_time_get -o "$OUT/enet_cap_test"
g++ $CXXFLAGS Tests/flow_relief_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/flow_relief_test"
# soc_send_test (2026-11-11): the count of the datagrams handed to the room socket, against the plugin's own ENet platform layer (Sources/enet/enet_3ds.c with the shim of
# Tests/enet3ds for the one libctru call it makes; its object is NOT an enet_*.o: every other test here links the PC's layer). The link wraps the socket calls under the
# layer (the test decides what the socket answers) and the layer's two hooks (the test sees what they are told, and leaves another errno behind)
gcc $CFLAGS -ITests/enet3ds/shim -c Sources/enet/enet_3ds.c -o "$OUT/plat_3ds.o"
g++ $CXXFLAGS Tests/soc_send_test.cpp Sources/azl/log.cpp Sources/azl/platform.cpp "$OUT/plat_3ds.o" -Wl,--wrap=sendto -Wl,--wrap=send -Wl,--wrap=azl_c_soc_send -Wl,--wrap=azl_c_soc_call -o "$OUT/soc_send_test"
# hook_swap_test: the plugin's side of the svc hook's persistent gate around the swap of the plugin's memory on an Old 3DS (azl/hook_swap.hpp: the gate's close and open, with real
# threads through a model of gate.s, and no word of the game's text written after the installation); the same source with ThreadSanitizer (the gate's count and flag)
g++ $CXXFLAGS Tests/hook_swap_test.cpp -o "$OUT/hook_swap_test"
# traced_sessions_test: the table of the game's real sessions the hook follows (azl/traced_sessions.hpp), with the HOME loop of issue 5 (closes not seen); the same with ThreadSanitizer
g++ $CXXFLAGS Tests/traced_sessions_test.cpp -o "$OUT/traced_sessions_test"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=thread Tests/traced_sessions_test.cpp -o "$OUT/traced_sessions_test_tsan"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=thread Tests/hook_swap_test.cpp -o "$OUT/hook_swap_test_tsan"
# gate_plan_test: the plan of the persistent gate's page of the svc hook (azl/gate_plan.hpp): the words of the branches and the stubs and their 32 MB, where the
# page goes over the address spaces of the launches in our logs (it reads Tests/gate_maps.json) and made-up ones, and WriteGate (one word per call site, once)
g++ $CXXFLAGS Tests/gate_plan_test.cpp -o "$OUT/gate_plan_test"
# kernel_books_test: the arithmetic of the kernel's books of the game's process (azl/books_math.hpp: Charge, Raise, Settle) on a made-up KResourceLimit, the end of the process included
g++ $CXXFLAGS Tests/kernel_books_test.cpp -o "$OUT/kernel_books_test"
# swap_gate_test: the gate of the plugin's own threads around that swap (azl/swap_gate.hpp: no service call of theirs under way when the block goes); the same source with ThreadSanitizer
g++ $CXXFLAGS Tests/swap_gate_test.cpp -o "$OUT/swap_gate_test"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=thread Tests/swap_gate_test.cpp -o "$OUT/swap_gate_test_tsan"
g++ $CXXFLAGS Tests/gsp_wrapper_test.cpp -o "$OUT/gsp_wrapper_test"
g++ $CXXFLAGS Tests/soc_plan_test.cpp -o "$OUT/soc_plan_test"
g++ $CXXFLAGS Tests/text_search_guard_test.cpp Sources/ctr/text_search_guard.cpp -o "$OUT/text_search_guard_test"
g++ $CXXFLAGS Tests/plugin_compare_test.cpp -o "$OUT/plugin_compare_test"
g++ $CXXFLAGS Tests/screen_text_test.cpp -o "$OUT/screen_text_test"
g++ $CXXFLAGS Tests/chat_core_test.cpp -Wl,--wrap=malloc -o "$OUT/chat_core_test"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=thread Tests/chat_core_test.cpp -Wl,--wrap=malloc -o "$OUT/chat_core_test_tsan"
# chat_draw_test: the chat's lines on the top screen (azl/chat_draw.hpp) and the drawing under them (azl/gfx_surface.hpp, the font of
# azl/font_spleen.inc) in fake framebuffers of the five formats, with the 3D, every store counted and no allocation; it writes its pictures
# (BMP) into the folder it is given
g++ $CXXFLAGS Tests/chat_draw_test.cpp -Wl,--wrap=malloc -o "$OUT/chat_draw_test"
# osk_test: the on-screen keyboard (azl/osk.hpp, azl/osk_draw.hpp), its state and its drawing, no allocation once it is made; it writes
# its pictures (BMP) into the folder it is given
g++ $CXXFLAGS Tests/osk_test.cpp -Wl,--wrap=malloc -o "$OUT/osk_test"
# chat_host_test: the chat over the running game (azl/chat_host.hpp), no allocation on the game's thread; again with ThreadSanitizer
g++ $CXXFLAGS Tests/chat_host_test.cpp -Wl,--wrap=malloc -o "$OUT/chat_host_test"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=thread Tests/chat_host_test.cpp -Wl,--wrap=malloc -o "$OUT/chat_host_test_tsan"
# hid_mirror_test: the input mirror page (azl/hid_mirror.hpp): the page's layout, the wait for the release, the search and the switch over a
# simulated kernel
g++ $CXXFLAGS Tests/hid_mirror_test.cpp -o "$OUT/hid_mirror_test"
# zombie_probe_test: the pure logic of the zombie probe (ctr/zombie_probe.hpp): the scheduling-mask classification, the memory-map merge and
# walk (never loops, the cap marked), the IPC reply handle-transfer decode, the handle classes, the owned objects of the refcount line and the
# startup-occupancy decision (the console reads that need svcs are not built on a PC)
g++ $CXXFLAGS -IIncludes Tests/zombie_probe_test.cpp -o "$OUT/zombie_probe_test"
g++ $CXXFLAGS Tests/log_files_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/log_files_test"
g++ $CXXFLAGS Tests/line_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/line_test"
g++ $CXXFLAGS Tests/sleep_crumbs_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/sleep_crumbs_test"
g++ $CXXFLAGS Tests/plg_guard_test.cpp -o "$OUT/plg_guard_test"
# The exit guard (azl/exit_guard.hpp, REGISTRO phase Q): the decision table and its loop against a model of Luma3DS's EXIT (k11 ExitProcess.c,
# Rosalina's whole-word store and == on the events): cases A, B, SSF4, the exit during a wait, normal.
g++ $CXXFLAGS Tests/exit_guard_test.cpp -o "$OUT/exit_guard_test"
g++ $CXXFLAGS Tests/capture_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/capture_test"
# game_card_calls_test (2026-11-11): which IPC calls of the game the hook counts as calls to the file system (azl/game_card_calls.hpp, a pure
# header: every fs command, not the reads alone - Tekken 3D's open that waited 26 s behind the log's writer), over the fs headers of two real
# captures, the calls a game makes to the other services, the whole command space, and the hook's two lines around the writer's watch
g++ $CXXFLAGS Tests/game_card_calls_test.cpp -o "$OUT/game_card_calls_test"
# The fake Download Play lobby: header-only modules (Includes/azl/dlp_*.hpp) and the in-memory network of the tests (Tests/dlp_fakenet.hpp, found beside the
# tests), so there is nothing to link. ASan/UBSan like the rest: each of these tests defines __ubsan_default_options (halt_on_error=1) and so stops at the
# first undefined behaviour. dlp_lobby_sim_test takes about 35 s.
g++ $CXXFLAGS Tests/dlp_lobby_test.cpp -o "$OUT/dlp_lobby_test"
g++ $CXXFLAGS Tests/dlp_ipc_test.cpp -o "$OUT/dlp_ipc_test"
# dlp_ipc_test_joins (1.1.4, route B): the dlp:CLNT tests of the same file with G1's join path built (AZL_CLNT_SYSTEM_JOINS=1, azl/dlp_clnt_map.hpp: the plugin never has it)
g++ $CXXFLAGS -DAZL_CLNT_SYSTEM_JOINS=1 Tests/dlp_ipc_test.cpp -o "$OUT/dlp_ipc_test_joins"
g++ $CXXFLAGS Tests/dlp_lobby_sim_test.cpp -o "$OUT/dlp_lobby_sim_test"
g++ $CXXFLAGS Tests/dlp_adv_lobby_test.cpp -o "$OUT/dlp_adv_lobby_test"
g++ $CXXFLAGS Tests/dlp_adv_ipc_test.cpp -o "$OUT/dlp_adv_ipc_test"
# The fake lobby on the REAL engine (Tests/dlp_net_test.cpp: engines joined by the in-memory room, an EngineLobbyNet and a DlpLobby on each; real time, about 40 s):
# the adapter is one more source, like the core.
g++ $CXXFLAGS Tests/dlp_net_test.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -o "$OUT/dlp_net_test"
g++ $CXXFLAGS Tests/dlp_net_adv_test.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -o "$OUT/dlp_net_adv_test"
g++ $CXXFLAGS Tests/dlp_net_churn_test.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -o "$OUT/dlp_net_churn_test"
# (dlp_net_extra_test moves the clock by 2^31 ms and fails an allocation, both through the linker: --wrap of platform::NowMs and of malloc)
g++ $CXXFLAGS Tests/dlp_net_extra_test.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=_ZN3azl8platform5NowMsEv -Wl,--wrap=malloc -o "$OUT/dlp_net_extra_test"
g++ $CXXFLAGS Tests/dlp_real_engine_test.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_engine_test"
# The hub of the plugin (phase 3, Tests/dlp_hub_test.cpp: real threads, real engines, about 65 s). Its thread spy is the linker's: every UdsEngine member the adapter
# calls is wrapped (the mangled names; the test header says how they were got), and the same source is built a second time with ThreadSanitizer (no ASan: the two cannot be
# combined; the CORE sources are compiled with it too) because some synchronisation mutants are visible only there.
HUB_WRAP="-Wl,--wrap=_ZN3azl9UdsEngine18InitializeForLobbyEmPKt -Wl,--wrap=_ZN3azl9UdsEngine17BeginHostingLobbyEjhhhPKhj
  -Wl,--wrap=_ZN3azl9UdsEngine17ConnectBeginLobbyERKSt5arrayIhLm6EE -Wl,--wrap=_ZNK3azl9UdsEngine11ConnectPollEv
  -Wl,--wrap=_ZNK3azl9UdsEngine12HeardLobbyAtEjjPNS0_10HeardLobbyE -Wl,--wrap=_ZN3azl9UdsEngine12PrepareLobbyEv
  -Wl,--wrap=_ZNK3azl9UdsEngine12NodeSnapshotEPtS1_ -Wl,--wrap=_ZN3azl9UdsEngine18GetNodeInformationEtPNS_8NodeInfoE
  -Wl,--wrap=_ZN3azl9UdsEngine15PopLobbyMessageEPhS1_jPj -Wl,--wrap=_ZN3azl9UdsEngine6SendToEthjhPKhm
  -Wl,--wrap=_ZN3azl9UdsEngine11EjectClientEt -Wl,--wrap=_ZN3azl9UdsEngine8ShutdownEv
  -Wl,--wrap=_ZN3azl9UdsEngine18SetApplicationDataEPKhj -Wl,--wrap=_ZN3azl9UdsEngine22UpdateNetworkAttributeEth
  -Wl,--wrap=_ZNK3azl9UdsEngine11initializedEv"
g++ $CXXFLAGS Tests/dlp_hub_test.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_hub.cpp $CORE "$OUT"/enet_*.o $HUB_WRAP -o "$OUT/dlp_hub_test"
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -pthread -fsanitize=thread Tests/dlp_hub_test.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_hub.cpp $CORE "$OUT"/enet_*.o $HUB_WRAP -o "$OUT/dlp_hub_test_tsan"
# The Download Play wiring of plugin.cpp (the table of games, the seed, the resolver of the game's buffers, the hub's configuration) cannot be built on the PC (libctrpf): the
# test compiles that text, cut out of plugin.cpp unchanged (1.1.3: with the block of EarlyInit that picks the hub, and the adapter of the real Download Play it may
# make: the test is built below, after the library's dlph_wire.o).
python3 Tests/plugin_dlp_extract.py Sources/ctr/plugin.cpp "$OUT/plugin_dlp_slice.inc"
# dlp_real_wire_test: the wire of the REAL Download Play (1.1.3, azl/dlp_real_wire.hpp) against the golden bytes of the capture 1c (Tests/dlp_real_luigi_golden.inc):
# the checksum, the announcement, the host's packets, the fake client's answers and the parsers (W1..W10 of PROGETTO-DLP-VERO-1.1.3.md). It links the ONE file of the
# Download Play host library the plugin builds, ConfigApp/dlph/dlph_wire.c, compiled with the sanitizers.
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -c ConfigApp/dlph/dlph_wire.c -o "$OUT/dlph_wire.o"
g++ $CXXFLAGS Tests/dlp_real_wire_test.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" -o "$OUT/dlp_real_wire_test"
# dlp_title_block_test (2026-11-14, the TITLE in the announcement of the real Download Play: azl/dlp_title_block.hpp, for the games whose guests are the system's Download Play
# application - Mario Party: Island Tour): an SMDH into the block (language, byte order), the five frames with a block against the capture of two Azahar (every byte the capture
# kept, the ranges it does not carry, what Azahar's client reads back) and without one against the frames of before (Luigi's Mansion, Mario Kart 7), the child's package in a
# RomFS made here (its sizes, every lie refused, 20,000 damaged images), the adapter on the wire with and without a title, and the list of the games against the plugin's table.
g++ $CXXFLAGS -I"$OUT" Tests/dlp_title_block_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_title_block_test"
# dlp_children_test and dlp_clnt_test (1.1.4, piece BASE of the Download Play from the console: contract K1-K4 of analysis/piano-1.1.4-2026-11-15/specifiche/7-CONTRATTO.md),
# pure: the prepared child of every sold game of the catalogue (azl/title_rules.hpp PreparedOf / IsPreparedId), the index and the blocks the app writes and the
# plugin reads (azl/dlp_children.hpp: parser, round trips, sixteen rows, bad lines, the verdicts, the small line in ten units with the name shortened and never
# the mark, the writers into the records of dlp:CLNT), the app's paths of them (ConfigApp/source/dlp_guest_paths.cpp, built as it is), and the map of dlp:CLNT
# (azl/dlp_clnt_map.hpp).
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/dlp_children_test.cpp ConfigApp/source/dlp_guest_paths.cpp -o "$OUT/dlp_children_test"
g++ $CXXFLAGS Tests/dlp_clnt_test.cpp -o "$OUT/dlp_clnt_test"
# dlp_child_wait_test and dlp_content_notice_test (1.1.4, H5 and H3 of analysis/piano-1.1.4-2026-11-15/specifiche/3-G2.md), pure: the host of the real Download
# Play waits in its state 9 for the guests that went to start their child (azl/dlp_child_wait.hpp: the caps 15 s, 10 s with an Azahar, 6 s after the first
# one back, on a clock of the test), and names on its screen a guest it ejected for the content (azl/dlp_content_notice.hpp: once, never a MAC).
g++ $CXXFLAGS Tests/dlp_child_wait_test.cpp -o "$OUT/dlp_child_wait_test"
g++ $CXXFLAGS Tests/dlp_content_notice_test.cpp -o "$OUT/dlp_content_notice_test"
# dlp_real_net_test (1.1.3, phase F3): the HOST of the real Download Play (azl/dlp_real_net.hpp, RealDlpNet under the lobby) in front of a model of Azahar's
# Download Play client (Tests/azahar_dlp_peer.hpp): the announcement, the dialogue A1..A9, round trips up to 700 ms, losses, a game's session in the room,
# a quick rejoin, malformed packets, a full network, the lobby's resends and the game's BeginGame storm, the releases (R1, R1b, R4, R5, R6b, R7, R9, R12, R14;
# real time, about 2 minutes).
g++ $CXXFLAGS Tests/dlp_real_net_test.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_net_test"
# dlp_real_client_test (1.1.3, phase F4): the SEARCHER of the real Download Play (the same adapter) in front of a model of Azahar's Download Play host
# (Tests/azahar_dlp_host.hpp, with the capture's bytes): the probe as a spectator and its cache, the join and the answers A2/A4/A6/A9, the statuses 5-6-9,
# the host's silent end, losses, three hosts in a room, malformed / silent hosts, a full host, our own host, the releases on the game's thread (R2, R2c,
# R5b, R5c, R6, R6f, R9c, R12c, R3a; real time, about 2 minutes).
# (1.1.4, route B: dlp_real_client_test is built with G1's join path - RC1, RC3, RC4, RC8 join - and dlp_real_client_test_default as the plugin has dlp:CLNT: RC1b, RC2, RC5, RC6)
g++ $CXXFLAGS -DAZL_CLNT_SYSTEM_JOINS=1 Tests/dlp_real_client_test.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_client_test"
g++ $CXXFLAGS Tests/dlp_real_client_test.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_client_test_default"
g++ $CXXFLAGS -I"$OUT" Tests/plugin_dlp_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/plugin_dlp_test"
# dlp_real_hub_test (1.1.3, phase F5): the real Download Play as the plugin has it, behind the hub (DlpHub with the adapter RealDlpNet as the lobby's network), driven by
# the IPC words of the games on game threads with a network thread per console: 3DS - 3DS with 1 and 3 clients in both modes, the external states of the two compared
# (R3), the engine given back at the game's nwm::UDS with and without a network thread in every state (R8), the ends of every role in every state (R10), a quick rejoin and
# a spectator in the lobby (R11b), the two modes in one room (R13). Real time, about 2 minutes.
g++ $CXXFLAGS Tests/dlp_real_hub_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_hub_test"
# dlp_cap_guard_test (2026-11-13): the guard of the titles of four players, written BEFORE the lobby holds eight (what it holds and what it cannot: analysis/lobby-otto-2026-11-13/guardia).
# The 29 bytes of the announcement for what a game asks of dlp:SRVR Initialize, whole sessions of four driven through the IPC words as the hub runs them, recorded as lines (what the
# lobby asks of the network, every answer to the game) and held by their SHA-1 over the in-memory air and its simulated clock, the answers that depend on the number of players,
# Luigi's Mansion's golden bytes out of the codec, the host and the searcher of the real Download Play, and the dialect over the real engine (those parts in real time, about 15 s).
# Its literals are the output of the code of that day: they are not edited to make it pass. With a folder as its argument it writes the lines of the sessions there.
g++ $CXXFLAGS Tests/dlp_cap_guard_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_cap_guard_test"
# dlp_eight_test (2026-11-13): the lobby with EIGHT players - the tables hold host + 7 clients, the ceiling of a title is four unless its Download Play column of azl/title_rules.hpp says more (Mario Kart 7) - and
# the titles of four beside it: the ceiling as a value (0 is four), what a game gets for the clients it asks for, what a searcher lists, a whole evening of a title of four line by line
# as the code of before gave it (the fifth player, two nodes above the ceiling), the session of eight over the in-memory air with the words of the games, and over the real engine with
# the ring of the lobby's messages counted frame by frame, also with the creator or the seven clients stalled (those parts in real time, about 10 s). Since the reviews of that day
# also the count of its own lobby that every client in a session of eight reads, and the lobby and its service on a hostile network and under a hostile game for every ceiling of a
# title (a fixed seed and a fixed number of commands, about 20 s).
g++ $CXXFLAGS Tests/dlp_eight_test.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_eight_test"
# dlp_eight_hub_test (2026-11-13, the review of the lobby of eight): the same lobby as a console runs it - a DlpHub per console with the configuration of Mario Kart 7, a network thread
# each, real engines on the in-memory room - driven with the words Mario Kart 7 sent on a console, by the game threads of eight consoles at once: seven enter in the same instant, the
# ninth and the tenth, two leave while two enter and one comes back, START with a silent client and one whose answers come after the grace, the creator that goes, and after state 9
# the game's own nwm::UDS network of eight nodes (real time, about 25 s).
g++ $CXXFLAGS Tests/dlp_eight_hub_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_eight_hub_test"
# dlp_real_mk7_test (2026-11-13): Mario Kart 7 on the REAL Download Play with eight players (its Download Play column of azl/title_rules.hpp is RealDlp with a ceiling of eight; the adapter RealDlpNet holds
# host + 7 clients), held to the capture of eight Azahar that played it (Tests/dlp_real_mk7_golden.inc, made by Tests/make_dlp_real_mk7_goldens.py): the capture's seventy packets, beacon,
# node map and Logoff against the codec and the engine; a console that hosts with SEVEN models of Azahar's client, every frame it sends compared with the capture's form; a console that
# searches and joins a model of Azahar's host of 8 nodes (the unique id 0x307, the tick of GetServerInfo never older than 5 s) among Azahar clients; a client that leaves before the start
# (the host and the clients that remain know: the engine's refresh for the real Download Play's network); and Luigi's Mansion as before (ceiling four, one client, its own capture's bytes,
# not one frame more). Real time, about 60 s.
g++ $CXXFLAGS Tests/dlp_real_mk7_test.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_mk7_test"
# dlp_real_island_test (2026-11-14, Mario Party: Island Tour as the HOST of the real Download Play: its Download Play column of azl/title_rules.hpp is RealDlp, four players, the hash of the MAC): the hub as
# the plugin has it for that row - DlpHub on the adapter RealDlpNet, a network thread, the IPC words of the game as its code sends them (Initialize with no name for 3 clients,
# StartAccepting by hand, AcceptClient for every client and StartDistribution in the SAME frame, the passphrase once) - in front of models of Azahar's Download Play client, held to
# the capture of two Azahar that played it (Tests/dlp_real_island_golden.inc, made by Tests/make_dlp_real_island_goldens.py; the name, the icon, the passphrase and the nodes' names
# and addresses are not in it): the capture against the codec field by field, every frame the host sends compared with the capture's form, the game's own network from the same
# address with the three back in, a client that asks for the child's content (ejected: the child never travels), and the limits of the row held as they are (a command of the game
# the hub refuses with its network thread held up; the game's back-while-distributing). Real time, about 70 s.
g++ $CXXFLAGS Tests/dlp_real_island_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_island_test"
# dlp_real_lobby_games_test (1.1.4, D6): Kirby Triple Deluxe, Kirby Planet Robobot and Mario Tennis Open on the REAL Download Play, each held to the capture of
# Azahar emulators that played IT (Tests/dlp_real_<game>_golden.inc, made by Tests/make_dlp_real_lobby_games_goldens.py: no title, icon, passphrase, name or address in
# them): the capture against the codec (G), a console hosting the capture's Azahar clients (A), a console searching an Azahar host (B), four nodes and one that leaves
# before the start (C), Kirby's Finalize and StopSession with the network thread gone (F), the console's seed for the rows that have it (S). A game without its golden
# is SKIPPED, loudly, and its row must not be RealDlp (the rule: a row moves with its own capture). Real time.
g++ $CXXFLAGS Tests/dlp_real_lobby_games_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_lobby_games_test"
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/settings_test.cpp ConfigApp/source/settings.cpp ConfigApp/source/fileio.cpp -o "$OUT/settings_test"
# whole_file_test and whole_file_states_test (F7, 2026-11-21): AzaharLink.ini written whole or not at all by the plugin too (Includes/azl/whole_file.hpp,
# the order of the app's fileio.cpp): the model of the card (Tests/whole_file_model.hpp) cut at every step of a save, every prefix of its write and three
# ways of renaming; then the app's own fileio.cpp in front of every card those cuts leave (it reads what the plugin reads, and writes the same files).
g++ $CXXFLAGS Tests/whole_file_test.cpp -o "$OUT/whole_file_test"
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/whole_file_states_test.cpp ConfigApp/source/fileio.cpp -o "$OUT/whole_file_states_test"
# config_file_test (F7-rev, 2026-11-21): the plugin's own code of AzaharLink.ini (LoadConfig, SaveConfig, ReadDataFile and the card CtrWhole, cut out of
# plugin.cpp unchanged by Tests/config_file_extract.py) in front of a stand-in of CTRPF 0.8.0's File that answers as File.o does: the .bak of a cut save, a
# passing error of a look, an ini that cannot be read (never written over), the old place, every cut and every failing command of a save.
python3 Tests/config_file_extract.py Sources/ctr/plugin.cpp Includes/ctr/plugin.hpp "$OUT/config_file_slice.inc"
g++ $CXXFLAGS -I"$OUT" Tests/config_file_test.cpp -o "$OUT/config_file_test"
# TitleShare's own C files (the vendored copies): the app's keyed reading of a game's icon (ncch_icon_test) and its SHA-256 (paths.cpp) use them
for f in aes sdcrypt sha256 contentpath ncchname; do
  gcc -O1 -g -w -c ConfigApp/titleshare/common/$f.c -o "$OUT/ts_$f.o"
done
# the two values 3DS Link carries (the key generator's constant and the IV of the Download Play checksum): builtin_keys.c, which sdcrypt.c asks
gcc -O1 -g -w -c ConfigApp/titleshare_host/builtin_keys.c -o "$OUT/ts_builtin_keys.o"
g++ $CXXFLAGS -pthread Tests/settings_test/builtin_keys_test.cpp "$OUT"/ts_aes.o "$OUT"/ts_sdcrypt.o "$OUT"/ts_sha256.o "$OUT"/ts_builtin_keys.o -o "$OUT/builtin_keys_test"
# dlp_guest_test (1.1.4, piece APP-CORE of the Download Play child the app prepares with boot9.bin: analysis/piano-1.1.4-2026-11-15/specifiche/6-APP-boot9.md):
# the app's own aes_cbc.cpp and dlp_guest_compose.cpp (with dlp_guest_logic.hpp) on made-up children (Tests/settings_test/dlp_guest_synth.hpp) and, when the
# working tree has them, on the child of Island Tour against the PC's P2a and P2n by their md5. DLPGUEST_TEST_VISIBLE is defined on this line alone: only here may a child be made visible.
g++ $CXXFLAGS -DDLPGUEST_TEST_VISIBLE -IConfigApp/source Tests/settings_test/dlp_guest_test.cpp ConfigApp/source/aes_cbc.cpp ConfigApp/source/dlp_guest_compose.cpp "$OUT"/ts_aes.o "$OUT"/ts_sha256.o "$OUT"/ts_sdcrypt.o "$OUT"/ts_builtin_keys.o -o "$OUT/dlp_guest_test"
# dlp_guest_store_test (1.1.4, piece APP-STORE of the same child): the app's own dlp_guest_store.cpp (the .guest records, which titles are ours, children.cache, the
# index and the blocks the plugin reads, the leftovers, the start), dlp_guest_decide.hpp, dlp_guest_exclude.hpp and games.cpp's DisableGuestFolder, against a folder "sdmc:"
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/dlp_guest_store_test.cpp ConfigApp/source/dlp_guest_store.cpp ConfigApp/source/dlp_guest_paths.cpp ConfigApp/source/games.cpp ConfigApp/source/settings.cpp ConfigApp/source/fileio.cpp ConfigApp/source/paths.cpp "$OUT"/ts_sha256.o -o "$OUT/dlp_guest_store_test"
# dlp_guest_run_test (1.1.4, piece APP-WORKER of the same child): the app's own dlp_guest_run.cpp (the jobs: prepare, remove, disown, probe, check, adopt,
# repair, in the order that survives a cut), dlp_guest_route.hpp (route K: content 2 in the module's virtual CIA) and dlp_guest_reconcile.hpp, on made-up
# children and a made-up console, against a folder "sdmc:"
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/dlp_guest_run_test.cpp ConfigApp/source/dlp_guest_run.cpp ConfigApp/source/dlp_guest_ids.cpp ConfigApp/source/dlp_guest_compose.cpp ConfigApp/source/aes_cbc.cpp ConfigApp/source/dlp_guest_store.cpp ConfigApp/source/dlp_guest_paths.cpp ConfigApp/source/games.cpp ConfigApp/source/settings.cpp ConfigApp/source/fileio.cpp ConfigApp/source/paths.cpp "$OUT"/ts_aes.o "$OUT"/ts_sha256.o "$OUT"/ts_sdcrypt.o "$OUT"/ts_builtin_keys.o -o "$OUT/dlp_guest_run_test"
# dlp_guest_ids_test (1.1.4, the build 2h: the guard of the ids of the prepared children): the app's own dlp_guest_ids.cpp (the census of the console's ids,
# ids.cache with its seal, the refresh of the sources that changed, the judgment of an install and of a delete, R0-R8) on a made-up console, against a folder "sdmc:"
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/dlp_guest_ids_test.cpp ConfigApp/source/dlp_guest_ids.cpp ConfigApp/source/dlp_guest_store.cpp ConfigApp/source/dlp_guest_paths.cpp ConfigApp/source/games.cpp ConfigApp/source/settings.cpp ConfigApp/source/fileio.cpp ConfigApp/source/paths.cpp "$OUT"/ts_sha256.o -o "$OUT/dlp_guest_ids_test"
# dlp_join_test (1.1.4, route B, pieces B2/B3 of specifiche/8-INGRESSO-DA-3DSLINK.md): the app's Download Play page, its engine (ConfigApp/source/dlp_join.cpp:
# the Core on the adapter of the real Download Play, the polite leave of D-B10, the hand-over to ns:s) and its pure part (dlp_join_logic.hpp: JoinGate,
# BuildTable from the index file the plugin reads), in front of the model of Azahar's host and of our host with H5 (AwaitsChildOf from the slice of plugin.cpp)
g++ $CXXFLAGS -I"$OUT" -ITests/settings_test/shim -IConfigApp/source Tests/dlp_join_test.cpp ConfigApp/source/dlp_join.cpp ConfigApp/source/dlp_guest_store.cpp ConfigApp/source/dlp_guest_paths.cpp ConfigApp/source/games.cpp ConfigApp/source/settings.cpp ConfigApp/source/fileio.cpp ConfigApp/source/paths.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" "$OUT"/ts_sha256.o $CORE "$OUT"/enet_*.o -o "$OUT/dlp_join_test"
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/games_test.cpp ConfigApp/source/games.cpp ConfigApp/source/status.cpp ConfigApp/source/settings.cpp ConfigApp/source/fileio.cpp ConfigApp/source/paths.cpp "$OUT"/ts_sha256.o -o "$OUT/games_test"
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/paths_test.cpp ConfigApp/source/paths.cpp ConfigApp/source/appcfg.cpp ConfigApp/source/fileio.cpp ConfigApp/source/search.cpp ConfigApp/source/games.cpp "$OUT"/ts_sha256.o -o "$OUT/paths_test"
# plugin_install_test: the plugin that 3DS Link carries and puts on the card by itself (plugin_install.hpp with its .cpp, plain C++; paths.cpp makes the folders): what the file on the card
# is against the plugin inside the app (missing, damaged, a good header over other bytes, another version, the same), what is done about each, a write that is whole or leaves the
# old file, the words and the rule of the switch; on a temporary folder
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/plugin_install_test.cpp ConfigApp/source/plugin_install.cpp ConfigApp/source/paths.cpp "$OUT"/ts_sha256.o -o "$OUT/plugin_install_test"
# eta_test: the time a room of Titles Delivery still needs (delivery_eta.cpp, plain C++), against rooms worked out by hand
g++ $CXXFLAGS -IConfigApp/source Tests/settings_test/eta_test.cpp ConfigApp/source/delivery_eta.cpp -o "$OUT/eta_test"
# dlp_text_test: the words and the small decisions of 3DS Link Delivery (dlp_text.hpp, plain C++ header)
g++ $CXXFLAGS -IConfigApp/source Tests/settings_test/dlp_text_test.cpp -o "$OUT/dlp_text_test"
# dlp_prepare_test: what 3DS Link Delivery decides before its room opens (dlp_prepare_logic.hpp, plain C++ header, with the composer's dlpc.h for the numbers of its stages and results): is child.cia good, where the composer's input comes from, the time and the bar, the marker of a run that died
# (it also asks the real composer, ConfigApp/dlpc, what the app asks it - the size of the package, what each stage does - of a model that make_model.py makes from the finished child in
# DlpLink/out when the tree has one: without it that part says so and is skipped; the model is the one in ConfigApp/data when it is a real one)
gcc -O1 -g -Wall -c ConfigApp/dlpc/dlpc.c -o "$OUT/dlpc.o"
rm -f "$OUT/child_model.bin"
# the model that SHIPS (DlpLink/tools/build_all.sh made it without the app and the app carries it: ConfigApp/data/child_model.bin) when the tree has been built, else the model of the reference child
if [ -f ConfigApp/data/child_model.bin ] && python3 ../DlpLink/tools/make_model.py --check ConfigApp/data/child_model.bin > /dev/null 2>&1; then
  cp ConfigApp/data/child_model.bin "$OUT/child_model.bin"
else
  python3 ../DlpLink/tools/make_model.py ../DlpLink/out/3dslink-child.cia "$OUT/child_model.bin" > /dev/null 2>&1 || rm -f "$OUT/child_model.bin"
fi
g++ $CXXFLAGS -IConfigApp/source Tests/settings_test/dlp_prepare_test.cpp "$OUT/dlpc.o" "$OUT"/ts_aes.o "$OUT"/ts_sha256.o -o "$OUT/dlp_prepare_test"
# guide_test: the "How to get boot9.bin" guide (ConfigApp/guide/guide_logic.hpp, plain C++ header): the steps and their pictures (the files), the keys, the footer's taps, the look for boot9.bin
g++ $CXXFLAGS -IConfigApp/guide Tests/settings_test/guide_test.cpp -o "$OUT/guide_test"
# about_test: "About 3DS Link" and the credits (ConfigApp/source/about_text.hpp, plain C++ header): the name, the claim, the version, "Created by Gionko", how the app was started, the plugin row, the list of credits
# against CREDITS.md and against what the repository really holds (it reads files of the tree: run from AzaharLink/)
g++ $CXXFLAGS -IConfigApp/source Tests/settings_test/about_test.cpp -o "$OUT/about_test"
# update_test: the update from GitHub, what needs no console (ConfigApp/source/update_logic.hpp, plain C++ header): the versions, the redirect of GitHub, which file a run replaces, the CIA that came, the summary, the clock, when the question comes, the words; and the keys of 3DSLink.cfg. The requests themselves are Tests/update_http_test.sh
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/update_test.cpp ConfigApp/source/appcfg.cpp ConfigApp/source/fileio.cpp -o "$OUT/update_test"
# public_lobbies_test: the page "Public lobbies", what needs no console (ConfigApp/source/public_lobbies.hpp, plain C++ header): the list and the user's own file, what a new list keeps, the room in use that left it, the titles and their regions, the order, the ping's colours, the filters, the search, Quick join
g++ $CXXFLAGS -IConfigApp/source Tests/settings_test/public_lobbies_test.cpp -o "$OUT/public_lobbies_test"
# heap_plan_test: the memory the app asks for before main() (ConfigApp/source/heap_plan.hpp, plain C++ header: its own __system_allocateHeaps in heap_alloc.cpp), against a made-up kernel: the split of libctru, 8 KB and 5 MiB outside the books, a game still alive, the wait and the cuts, the New, the Old, the minimum, the log line
g++ $CXXFLAGS -IConfigApp/source Tests/settings_test/heap_plan_test.cpp -o "$OUT/heap_plan_test"
# zombie_plan_test: the pure part of 3DS Link's look at a closed game still in memory (ConfigApp/source/zombie_plan.hpp; zombie_census.cpp does the system calls)
g++ $CXXFLAGS -IConfigApp/source Tests/settings_test/zombie_plan_test.cpp -o "$OUT/zombie_plan_test"
# room_choices_test: the lists of the in-game menu (Includes/azl/room_choices.hpp): the saved rooms and the public lobbies with the user's own names and passwords, and the memory given back
g++ $CXXFLAGS Tests/room_choices_test.cpp -o "$OUT/room_choices_test"
# logrot_test: the title delivery module's log names and rotation and what the app tells it (titleshare_host/logrot.c and hostcfg.c, plain C), against the app's own paths
for f in logrot hostcfg; do
  gcc -O1 -g -Wall -Wextra -c ConfigApp/titleshare_host/$f.c -o "$OUT/th_$f.o"
done
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/logrot_test.cpp ConfigApp/source/paths.cpp ConfigApp/source/games.cpp "$OUT"/ts_sha256.o "$OUT"/th_logrot.o "$OUT"/th_hostcfg.o -o "$OUT/logrot_test"
# log_module_test: the module's log layer itself (ConfigApp/titleshare/source/log.c with its documented patch) on a PC, against a stand-in for libctru's threads and locks, with a 4 KB rotation limit
gcc -O1 -g -Wall -DLOGROT_MAX_BYTES=4096 -ITests/settings_test/logshim -IConfigApp/titleshare/source -IConfigApp/titleshare/common -IConfigApp/titleshare_host -c ConfigApp/titleshare/source/log.c -o "$OUT/lm_log.o"
gcc -O1 -g -Wall -DLOGROT_MAX_BYTES=4096 -c ConfigApp/titleshare_host/logrot.c -o "$OUT/lm_logrot.o"
g++ $CXXFLAGS -DLOGROT_MAX_BYTES=4096 -IConfigApp/titleshare/source -IConfigApp/titleshare_host Tests/settings_test/log_module_test.cpp "$OUT"/lm_log.o "$OUT"/lm_logrot.o "$OUT"/th_hostcfg.o -o "$OUT/log_module_test"
# ncch_icon_test: the app's keyed reading of a game's icon file against a made-up title
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/ncch_icon_test.cpp ConfigApp/source/ncch_icon.cpp ConfigApp/source/games.cpp "$OUT"/ts_*.o -o "$OUT/ncch_icon_test"
# (room_integration and room_ack_probe talk to a real room and read ENet's clock: a monotonic one, Tests/mono_clock.cpp - the wall clock of a VM steps by seconds)
g++ $CXXFLAGS Tests/room_integration.cpp Tests/mono_clock.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_time_get -o "$OUT/room_integration"
g++ $CXXFLAGS Tests/room_probe.cpp $CORE "$OUT"/enet_*.o -o "$OUT/room_probe"
# chat_room_probe: the chat through RoomClient against a real room (Tests/integration.sh runs it)
g++ $CXXFLAGS Tests/chat_room_probe.cpp $CORE "$OUT"/enet_*.o -o "$OUT/chat_room_probe"
# lobby_probe_test: the rooms of "Public lobbies" asked without joining (ConfigApp/source/lobby_probe.cpp), against real rooms: Tests/integration.sh runs it
g++ $CXXFLAGS -IConfigApp/source Tests/lobby_probe_test.cpp ConfigApp/source/lobby_probe.cpp $CORE "$OUT"/enet_*.o -o "$OUT/lobby_probe_test"
# dlp_room_probe: the fake lobby over a real room (Tests/integration.sh runs it; it needs the monotonic clock like room_integration)
g++ $CXXFLAGS Tests/dlp_room_probe.cpp Tests/mono_clock.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_time_get -o "$OUT/dlp_room_probe"
# dlp_real_room_probe (1.1.3, design 7.5, I1-I3): the REAL Download Play (RealDlpNet under the lobby) over a real room, our host with our searcher and with a model of
# Azahar's Download Play client behind its own ENet member, then 20 rounds in a row (Tests/integration.sh runs it; the monotonic clock like the other room probes);
# I4 the host's wait for the children (1.1.4, H5); I5 (1.1.4, route B) the 3DS Link app's page - its own Core, ConfigApp/source/dlp_join.cpp, built here as
# dlp_join_test builds it - and the prepared child's plugin (RejoinPolicy, HoldIdentity) in front of our host, H5 filtered by the plugin's AwaitsChildOf
g++ $CXXFLAGS -I"$OUT" -ITests/settings_test/shim -IConfigApp/source Tests/dlp_real_room_probe.cpp Tests/mono_clock.cpp ConfigApp/source/dlp_join.cpp ConfigApp/source/dlp_guest_store.cpp ConfigApp/source/dlp_guest_paths.cpp ConfigApp/source/games.cpp ConfigApp/source/settings.cpp ConfigApp/source/fileio.cpp ConfigApp/source/paths.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" "$OUT"/ts_sha256.o $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_time_get -o "$OUT/dlp_real_room_probe"
g++ $CXXFLAGS Tests/room_routing_probe.cpp $CORE "$OUT"/enet_*.o -o "$OUT/room_routing_probe"
g++ $CXXFLAGS Tests/room_observe.cpp $CORE "$OUT"/enet_*.o -o "$OUT/room_observe"
# room_ack_probe: the data path against the real room with a LAN-like wire (Tests/room_ack.sh runs it); the link wraps the send to classify the datagrams and the receive to count the empty ones
g++ $CXXFLAGS -DP5A_COUNTERS=1 Tests/room_ack_probe.cpp Tests/mono_clock.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_socket_send -Wl,--wrap=enet_socket_receive -Wl,--wrap=enet_time_get -o "$OUT/room_ack_probe"

if [ -z "$BUILD_ONLY" ]; then
  "$OUT/test_main"
  "$OUT/ring_test"
  "$OUT/dedupe_test" $DEDUPE_TRACES
  "$OUT/link_policy_test"
  "$OUT/beacon_cache_test"
  "$OUT/uds_eight_test"
  "$OUT/enet_path_test"
  "$OUT/enet_cap_test"
  "$OUT/flow_relief_test"
  "$OUT/soc_send_test"
  "$OUT/soc_plan_test"
  "$OUT/text_search_guard_test"
  "$OUT/plugin_compare_test"
  "$OUT/screen_text_test"
  "$OUT/chat_core_test"
  "$OUT/chat_core_test_tsan"
  mkdir -p "$OUT/chat-shots"
  "$OUT/chat_draw_test" "$OUT/chat-shots"
  mkdir -p "$OUT/osk-shots"
  "$OUT/osk_test" "$OUT/osk-shots"
  "$OUT/chat_host_test"
  "$OUT/chat_host_test_tsan"
  "$OUT/hid_mirror_test"
  "$OUT/zombie_probe_test"
  "$OUT/log_files_test"
  "$OUT/line_test"
  "$OUT/sleep_crumbs_test"
  "$OUT/plg_guard_test"
  "$OUT/exit_guard_test"
  "$OUT/capture_test"
  "$OUT/game_card_calls_test"
  timeout 1200 "$OUT/dlp_lobby_test"
  timeout 1200 "$OUT/dlp_ipc_test"
  timeout 1200 "$OUT/dlp_ipc_test_joins"
  timeout 1200 "$OUT/dlp_lobby_sim_test"
  timeout 1200 "$OUT/dlp_adv_lobby_test"
  timeout 1200 "$OUT/dlp_adv_ipc_test"
  timeout 1200 "$OUT/dlp_net_test"
  timeout 1200 "$OUT/dlp_net_adv_test"
  timeout 1200 "$OUT/dlp_net_churn_test"
  timeout 1200 "$OUT/dlp_net_extra_test"
  timeout 1200 "$OUT/dlp_real_engine_test"
  timeout 1200 "$OUT/dlp_hub_test"
  timeout 1200 "$OUT/dlp_hub_test_tsan"
  timeout 1200 "$OUT/plugin_dlp_test"
  timeout 1200 "$OUT/dlp_real_wire_test"
  timeout 1200 "$OUT/dlp_title_block_test"
  "$OUT/dlp_children_test"
  "$OUT/dlp_clnt_test"
  "$OUT/dlp_child_wait_test"
  "$OUT/dlp_content_notice_test"
  timeout 1200 "$OUT/dlp_real_net_test"
  timeout 1200 "$OUT/dlp_real_client_test"
  timeout 1200 "$OUT/dlp_real_client_test_default"
  timeout 1200 "$OUT/dlp_real_hub_test"
  timeout 1200 "$OUT/dlp_cap_guard_test"
  timeout 1200 "$OUT/dlp_eight_test"
  timeout 1200 "$OUT/dlp_eight_hub_test"
  timeout 1200 "$OUT/dlp_real_mk7_test"
  timeout 1200 "$OUT/dlp_real_island_test"
  timeout 1200 "$OUT/dlp_real_lobby_games_test"
  python3 Tests/make_dlp_real_lobby_games_goldens.py --selftest
  python3 Tests/dlp_chain_report.py --selftest
  bash Tests/dlp_stack_check.sh
  bash Tests/dlp_net_stack_check.sh
  "$OUT/settings_test"
  "$OUT/whole_file_test"
  "$OUT/whole_file_states_test"
  "$OUT/config_file_test"
  "$OUT/games_test"
  "$OUT/paths_test"
  "$OUT/plugin_install_test"
  "$OUT/eta_test"
  "$OUT/dlp_text_test"
  "$OUT/dlp_prepare_test" "$OUT/child_model.bin"
  "$OUT/guide_test"
  "$OUT/about_test"
  "$OUT/update_test"
  "$OUT/public_lobbies_test"
  "$OUT/heap_plan_test"
  "$OUT/zombie_plan_test"
  "$OUT/room_choices_test"
  "$OUT/logrot_test"
  "$OUT/log_module_test"
  "$OUT/ncch_icon_test"
  "$OUT/builtin_keys_test"
  "$OUT/dlp_guest_test"
  "$OUT/dlp_guest_store_test"
  "$OUT/dlp_guest_run_test"
  "$OUT/dlp_guest_ids_test"
  timeout 1200 "$OUT/dlp_join_test"
  "$OUT/hook_swap_test"
  "$OUT/hook_swap_test_tsan"
  "$OUT/gate_plan_test"
  "$OUT/kernel_books_test"
  "$OUT/traced_sessions_test"
  "$OUT/traced_sessions_test_tsan"
  "$OUT/swap_gate_test"
  "$OUT/swap_gate_test_tsan"
  "$OUT/gsp_wrapper_test" $GSP_GAME_CODE   # GSP_GAME_CODE="<decompressed .code> <text size> <expected bl address>" runs the search over a game
fi
