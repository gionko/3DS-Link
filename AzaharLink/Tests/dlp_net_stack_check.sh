#!/bin/bash
# The adapter of the fake Download Play lobby (Includes/azl/dlp_lobby_net.hpp, Sources/azl/dlp_lobby_net.cpp) and the pieces it
# added to the UDS engine (Sources/azl/uds_engine.cpp: BeginHostingLobby, HeardLobbyAt, ConnectBegin / ConnectPoll, the lobby's
# message ring, NodeSnapshot, InitializeForLobby) on the plugin's own target (DESIGN-FASE2.md, rules 3 and 4 of the user: little
# stack, little memory). The adapter's methods run on the game thread that made the IPC call (some of them have 464 bytes below it),
# so the PC tests, which are x86-64 with the sanitizers, do not see what matters here. This script:
#   1. compiles the adapter with arm-none-eabi-g++ and the flags of the Makefile plus -Wextra -Wpedantic -Wshadow -Werror (it must
#      be clean for the plugin's compiler), and the engine with the flags of the Makefile (the plugin builds it so), both with
#      -fstack-usage; and Sources/azl/dlp_lobby_net.cpp must compile with the 3DS defines (-D__3DS__ -DAZL_USE_CTRPF), as in the plugin;
#   2. fails when a method of the adapter has a frame over FRAME_ADAPTER bytes, or a function of the engine that a game thread
#      reaches through the adapter has one over FRAME_ENGINE (the ones the network thread alone runs are not held), or a dynamic frame;
#   3. fails when the adapter's object keeps any static storage (data or bss over 0: nothing is kept in memory by it, the lobby's
#      own object is the whole of its state, and a `static` buffer in a method would hide from every other check);
#   4. proves itself first: a made-up listing with a frame over its budget and one with a dynamic frame must each be refused, a good
#      one accepted (a gate that cannot say no is not a gate).
# The plugin's own translation unit decides the real inlining: phase 3 measures the same functions again there
# (Tests/stack_usage.sh). Where the compiler is: in the PATH, in $DEVKITARM/bin, or in /opt/devkitpro/devkitARM/bin; libctru's headers
# in $DEVKITPRO/libctru/include or /opt/devkitpro/libctru/include. Without them the check is SKIPPED and says so loudly;
# DLP_STACK_REQUIRED=1 makes that a failure.
#   bash Tests/dlp_net_stack_check.sh        (from anywhere; run by Tests/run.sh)
set -e
cd "$(dirname "$0")/.."

FRAME_ADAPTER=80   # bytes: a method of EngineLobbyNet (ScanRead, which holds the lobby heard, is the largest)
FRAME_ENGINE=64    # bytes: a function of the engine that a lobby method calls on the game's thread
FRAME_HUB=40       # bytes: a function of DlpHub (Serve and AnswerUnserved, 40 and 24 since the always-succeed branch has a frame of its own): the code a game thread runs to serve a dlp:FKCL / dlp:SRVR call
FRAME_HUB_UNIT=64  # bytes: any function of the hub's translation unit (the hub, and the lobby and the service compiled into it: PollScan holds the lobby heard)
# (Phase 3: the hub, Sources/azl/dlp_hub.cpp, is compiled strictly like the adapter and its frames and static storage are held the same way)
ENGINE_FUNCTIONS='BeginHostingLobby|HeardLobbyAt|ConnectBegin|ConnectBeginLobby|ConnectPoll|InitializeForLobby|SetLobbyInbox|PopLobbyMessage|NodeSnapshot|StartHostingLocked|StartConnectLocked|BeginServiceLocked|PrepareLobby|EnsureLobbyLocked'

ARM_FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -Os -mword-relocations -fomit-frame-pointer -ffunction-sections -fno-strict-aliasing -Wall -Wno-psabi -fno-rtti -fno-exceptions -std=gnu++17 -D__3DS__ -DAZL_USE_CTRPF"

ARM_GXX=""
for candidate in "$(command -v arm-none-eabi-g++ 2>/dev/null || true)" "${DEVKITARM:+$DEVKITARM/bin/arm-none-eabi-g++}" /opt/devkitpro/devkitARM/bin/arm-none-eabi-g++; do
  if [ -n "$candidate" ] && [ -x "$candidate" ]; then
    ARM_GXX="$candidate"
    break
  fi
done
CTRU=""
for candidate in "${DEVKITPRO:+$DEVKITPRO/libctru/include}" /opt/devkitpro/libctru/include; do
  if [ -n "$candidate" ] && [ -f "$candidate/3ds.h" ]; then
    CTRU="$candidate"
    break
  fi
done
if [ -z "$ARM_GXX" ] || [ -z "$CTRU" ]; then
  if [ -n "$DLP_STACK_REQUIRED" ]; then
    echo "dlp net stack: FAILED - no arm-none-eabi-g++ or no libctru headers and DLP_STACK_REQUIRED is set"
    exit 1
  fi
  echo "dlp net stack: SKIPPED - no arm-none-eabi-g++ or no libctru headers: the frames of the lobby's adapter on the plugin's target were NOT measured"
  exit 0
fi
ARM_SIZE="$(dirname "$ARM_GXX")/arm-none-eabi-size"
ARM_FLAGS="$ARM_FLAGS -IIncludes -isystem $CTRU"

W="${OUT:-/tmp/azl_tests}/dlp_net_stack"
rm -rf "$W"
mkdir -p "$W"

# check_su <listing> <name-regex> <budget> <what>: every function whose signature matches the regex has a static frame within the budget
check_su() {
  awk -F'\t' -v re="$2" -v budget="$3" -v what="$4" '
      $1 ~ re {
        seen++
        if ($3 != "static") { print "  " what ": a frame that is not static (alloca or a variable length array): " $1; bad = 1 }
        else if ($2 + 0 > budget + 0) { print "  " what ": " $2 " bytes over the " budget " of the budget: " $1; bad = 1 }
      }
      END { if (!seen) { print "  " what ": no function matches " re " (a rename that the check does not follow?)"; bad = 1 } exit bad }' "$1"
}

# the gate on itself
printf 'a.cpp:1:1:azl::EngineLobbyNet::ScanRead(int)\t96\tstatic\n' > "$W/bad_frame.su"
printf 'a.cpp:1:1:azl::EngineLobbyNet::ScanRead(int)\t40\tdynamic\n' > "$W/bad_dynamic.su"
printf 'a.cpp:1:1:azl::EngineLobbyNet::ScanRead(int)\t80\tstatic\n' > "$W/good.su"
if check_su "$W/bad_frame.su" 'EngineLobbyNet::' 80 selftest >/dev/null; then echo "dlp net stack: FAILED - the gate accepted a frame over its budget"; exit 1; fi
if check_su "$W/bad_dynamic.su" 'EngineLobbyNet::' 80 selftest >/dev/null; then echo "dlp net stack: FAILED - the gate accepted a dynamic frame"; exit 1; fi
check_su "$W/good.su" 'EngineLobbyNet::' 80 selftest >/dev/null || { echo "dlp net stack: FAILED - the gate refused a good listing"; exit 1; }

"$ARM_GXX" $ARM_FLAGS -Wextra -Wpedantic -Wshadow -Werror -fstack-usage -c Sources/azl/dlp_lobby_net.cpp -o "$W/adapter.o"
"$ARM_GXX" $ARM_FLAGS -fstack-usage -c Sources/azl/uds_engine.cpp -o "$W/engine.o"

"$ARM_GXX" $ARM_FLAGS -Wextra -Wpedantic -Wshadow -Werror -fstack-usage -c Sources/azl/dlp_hub.cpp -o "$W/hub.o"

rc=0
check_su "$W/adapter.su" 'EngineLobbyNet::' $FRAME_ADAPTER "the adapter" || rc=1
check_su "$W/hub.su" 'DlpHub::' $FRAME_HUB "the hub" || rc=1
check_su "$W/hub.su" '.' $FRAME_HUB_UNIT "the hub's unit" || rc=1
check_su "$W/engine.su" "UdsEngine::($ENGINE_FUNCTIONS)\\(" $FRAME_ENGINE "the engine's lobby functions" || rc=1
read -r _text data bss _rest < <("$ARM_SIZE" "$W/adapter.o" | awk 'NR == 2')
if [ "$data" != 0 ] || [ "$bss" != 0 ]; then
  echo "  the adapter keeps static storage (data $data, bss $bss)"
  rc=1
fi
# the hub keeps no static storage either: its whole state is the object the plugin makes (a block of the heap, only for a game in Lobby mode)
read -r _text data bss _rest < <("$ARM_SIZE" "$W/hub.o" | awk 'NR == 2')
if [ "$data" != 0 ] || [ "$bss" != 0 ]; then
  echo "  the hub keeps static storage (data $data, bss $bss)"
  rc=1
fi
if [ "$rc" != 0 ]; then
  echo "dlp net stack: FAILED"
  exit 1
fi
big=$(awk -F'\t' '$1 ~ /EngineLobbyNet::/ { if ($2 + 0 > m) m = $2 + 0 } END { print m + 0 }' "$W/adapter.su")
hubbig=$(awk -F'	' '$1 ~ /DlpHub::/ { if ($2 + 0 > m) m = $2 + 0 } END { print m + 0 }' "$W/hub.su")
echo "dlp net stack: ok (the largest frame of the adapter is $big bytes, budget $FRAME_ADAPTER; the engine's lobby functions are within $FRAME_ENGINE; the largest frame of the hub is $hubbig bytes, budget $FRAME_HUB; no static storage)"
