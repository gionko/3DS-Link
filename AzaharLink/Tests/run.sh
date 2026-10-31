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
# model of the soc service). log_files_test covers the log and what it costs when it is off: the ini values (debug 0
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
# from the console's clock, the ring under overflow and two producers.
# line_test covers the text of the code that runs on the game's threads: azl/line.hpp's pieces, LogLine / DebugLine, the
# raw records of the hook's two pass-through lines and the heads of the UDS/ndm capture against the printf formats they
# replaced, and the console clock against gmtime (no printf on a game thread: Tests/policy_check.sh, Tests/stack_usage.sh).
# capture_test covers the debug capture's ring: the two lanes (control records keep their room when a flood of frames fills the
# rest), the LOST markers, a producer that never waits for the ring's lock, the diet of the IPC lines, the raw records of the
# HLE proper's commands against the printf formats, the header-only frame records. line_test also holds the formatter of
# Log() / DebugEvent() (azl/printf_lite.hpp) against newlib's, and the formats of every Log() call in the sources.
# screen_text_test covers what the screen says about the game's memory (the reading, the mode, the limits, when a warning is told and
# told again) and about frames lost (too slow a reader or no reader at all, from the game's UDS calls).
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
# dlp_net_test covers the same lobby over the REAL UDS engine (phase 2: azl/dlp_lobby_net.hpp, the adapter, and the pieces the engine got for it): consoles that are engines joined by the in-memory room, the
# lobby heard and joined, the whole sequence of Triple Deluxe and then the game's own UDS on the same engines (clean), an admission closed or full, four players, a client that leaves and a creator that goes,
# the game that starts nwm::UDS under the lobby, two lobbies in one room, the messages of strangers and copies, the ring of 8, and the calls that must allocate nothing. dlp_room_probe is the same against a
# real room server (Tests/integration.sh). Tests/dlp_net_stack_check.sh builds the adapter and the engine for the plugin's target and holds the frames and the static storage.
# Tests/dlp_stack_check.sh (with Tests/dlp_stack_probe.cpp) builds the same three headers for the plugin's own target, with arm-none-eabi-g++ and the flags of the Makefile, and holds what the x86-64
# tests cannot see: the frame of every function (48 bytes, PollScan and Dispatch 64), the frames added up from each function down to its deepest call out of the module (160), no recursion, no dynamic
# frame, the sizes on ARM32 (the lobby 272 bytes, the service 40) and a wrapper for every public function. It looks for the compiler in /opt/devkitpro/devkitARM/bin too, and is skipped aloud where there is none.
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
      Sources/azl/ndm_hle.cpp Sources/azl/arm_reloc.cpp Sources/azl/svc_patch.cpp Sources/azl/log_files.cpp"

g++ $CXXFLAGS Tests/test_main.cpp $CORE "$OUT"/enet_*.o -o "$OUT/test_main"
g++ $CXXFLAGS Tests/ring_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/ring_test"
g++ $CXXFLAGS Tests/dedupe_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/dedupe_test"
g++ $CXXFLAGS Tests/link_policy_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/link_policy_test"
# enet_path_test: the ENet client's data path (what a service call reads, when the acknowledgements go, the counters); the link wraps
# the platform layer's three socket calls so that the test can count them, and ENet's clock so that it can hold it
g++ $CXXFLAGS Tests/enet_path_test.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_socket_receive -Wl,--wrap=enet_socket_send -Wl,--wrap=enet_socket_wait -Wl,--wrap=enet_time_get -o "$OUT/enet_path_test"
g++ $CXXFLAGS Tests/reloc_test.cpp Sources/azl/arm_reloc.cpp -o "$OUT/reloc_test"
g++ $CXXFLAGS Tests/soc_plan_test.cpp -o "$OUT/soc_plan_test"
g++ $CXXFLAGS Tests/text_search_guard_test.cpp Sources/ctr/text_search_guard.cpp -o "$OUT/text_search_guard_test"
g++ $CXXFLAGS Tests/plugin_compare_test.cpp -o "$OUT/plugin_compare_test"
g++ $CXXFLAGS Tests/screen_text_test.cpp -o "$OUT/screen_text_test"
g++ $CXXFLAGS Tests/log_files_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/log_files_test"
g++ $CXXFLAGS Tests/line_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/line_test"
g++ $CXXFLAGS Tests/sleep_crumbs_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/sleep_crumbs_test"
g++ $CXXFLAGS Tests/capture_test.cpp $CORE "$OUT"/enet_*.o -o "$OUT/capture_test"
# The fake Download Play lobby: header-only modules (Includes/azl/dlp_*.hpp) and the in-memory network of the tests (Tests/dlp_fakenet.hpp, found beside the
# tests), so there is nothing to link. ASan/UBSan like the rest: each of these tests defines __ubsan_default_options (halt_on_error=1) and so stops at the
# first undefined behaviour. dlp_lobby_sim_test takes about 35 s.
g++ $CXXFLAGS Tests/dlp_lobby_test.cpp -o "$OUT/dlp_lobby_test"
g++ $CXXFLAGS Tests/dlp_ipc_test.cpp -o "$OUT/dlp_ipc_test"
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
# test compiles that text, cut out of plugin.cpp unchanged.
python3 Tests/plugin_dlp_extract.py Sources/ctr/plugin.cpp "$OUT/plugin_dlp_slice.inc"
g++ $CXXFLAGS -I"$OUT" Tests/plugin_dlp_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -o "$OUT/plugin_dlp_test"
g++ $CXXFLAGS -ITests/settings_test/shim -IConfigApp/source Tests/settings_test/settings_test.cpp ConfigApp/source/settings.cpp ConfigApp/source/fileio.cpp -o "$OUT/settings_test"
# TitleShare's own C files (the vendored copies): the app's keyed reading of a game's icon (ncch_icon_test) and its SHA-256 (paths.cpp) use them
for f in aes sdcrypt sha256 contentpath ncchname; do
  gcc -O1 -g -w -c ConfigApp/titleshare/common/$f.c -o "$OUT/ts_$f.o"
done
# the two values 3DS Link carries (the key generator's constant and the IV of the Download Play checksum): builtin_keys.c, which sdcrypt.c asks
gcc -O1 -g -w -c ConfigApp/titleshare_host/builtin_keys.c -o "$OUT/ts_builtin_keys.o"
g++ $CXXFLAGS -pthread Tests/settings_test/builtin_keys_test.cpp "$OUT"/ts_aes.o "$OUT"/ts_sdcrypt.o "$OUT"/ts_sha256.o "$OUT"/ts_builtin_keys.o -o "$OUT/builtin_keys_test"
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
# dlp_room_probe: the fake lobby over a real room (Tests/integration.sh runs it; it needs the monotonic clock like room_integration)
g++ $CXXFLAGS Tests/dlp_room_probe.cpp Tests/mono_clock.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_time_get -o "$OUT/dlp_room_probe"
g++ $CXXFLAGS Tests/room_routing_probe.cpp $CORE "$OUT"/enet_*.o -o "$OUT/room_routing_probe"
g++ $CXXFLAGS Tests/room_observe.cpp $CORE "$OUT"/enet_*.o -o "$OUT/room_observe"
# room_ack_probe: the data path against the real room with a LAN-like wire (Tests/room_ack.sh runs it); the link wraps the send to classify the datagrams and the receive to count the empty ones
g++ $CXXFLAGS -DP5A_COUNTERS=1 Tests/room_ack_probe.cpp Tests/mono_clock.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_socket_send -Wl,--wrap=enet_socket_receive -Wl,--wrap=enet_time_get -o "$OUT/room_ack_probe"

if [ -z "$BUILD_ONLY" ]; then
  "$OUT/test_main"
  "$OUT/ring_test"
  "$OUT/dedupe_test" $DEDUPE_TRACES
  "$OUT/link_policy_test"
  "$OUT/enet_path_test"
  "$OUT/soc_plan_test"
  "$OUT/text_search_guard_test"
  "$OUT/plugin_compare_test"
  "$OUT/screen_text_test"
  "$OUT/log_files_test"
  "$OUT/line_test"
  "$OUT/sleep_crumbs_test"
  "$OUT/capture_test"
  timeout 1200 "$OUT/dlp_lobby_test"
  timeout 1200 "$OUT/dlp_ipc_test"
  timeout 1200 "$OUT/dlp_lobby_sim_test"
  timeout 1200 "$OUT/dlp_adv_lobby_test"
  timeout 1200 "$OUT/dlp_adv_ipc_test"
  timeout 1200 "$OUT/dlp_net_test"
  timeout 1200 "$OUT/dlp_net_adv_test"
  timeout 1200 "$OUT/dlp_net_churn_test"
  timeout 1200 "$OUT/dlp_net_extra_test"
  timeout 1200 "$OUT/dlp_hub_test"
  timeout 1200 "$OUT/dlp_hub_test_tsan"
  timeout 1200 "$OUT/plugin_dlp_test"
  python3 Tests/dlp_chain_report.py --selftest
  bash Tests/dlp_stack_check.sh
  bash Tests/dlp_net_stack_check.sh
  "$OUT/settings_test"
  "$OUT/games_test"
  "$OUT/paths_test"
  "$OUT/plugin_install_test"
  "$OUT/eta_test"
  "$OUT/dlp_text_test"
  "$OUT/dlp_prepare_test" "$OUT/child_model.bin"
  "$OUT/guide_test"
  "$OUT/about_test"
  "$OUT/logrot_test"
  "$OUT/log_module_test"
  "$OUT/ncch_icon_test"
  "$OUT/builtin_keys_test"
  "$OUT/reloc_test" $RELOC_GAME_CODE   # RELOC_GAME_CODE=<decompressed .code> adds the game sweep
fi
