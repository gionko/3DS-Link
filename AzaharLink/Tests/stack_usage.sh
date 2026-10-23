#!/bin/bash
# The stack a GAME thread pays for the plugin, which is the one resource the hook takes from threads it does not own.
# The hooked svc 0x32 runs the plugin's C++ on whatever stack the thread that made the IPC call has, and the SDK threads
# of some games have very little: Tri Force Heroes' srv-notification thread has 464 bytes below its call (a 512-byte
# stack inside its own object), and 0.1.15 at debug=1 ran a printf chain of 1.3 KB there - the game crashed on exit
# (hardware, 2026-10-10). Three checks:
#   1. the frames of the functions a game thread runs through (-fstack-usage on a COPY of the tree with the shipped
#      flags): fails when a frame is over its budget (the measured size plus a margin). This sees one frame at a time,
#      not the libc functions below it;
#   2. the call graph (Tests/callgraph_check.py): the built ELF is disassembled and every function a game thread can
#      reach from the entries of the hook - and from the callbacks it reaches through a pointer - is followed; the printf
#      family, Log() and DebugEvent() must not be among them (the HLE proper, past UdsHle::Run, is the one cut);
#   3. the WHOLE chain (Tests/chain_sim/chain_sim.py): the real code of the built plugin, libc included, run in an ARM
#      emulator on a 512-byte game stack, entered like the per-site trampoline does: the pure pass-through path must stay
#      within 300 bytes at debug 0 and at debug 1, the other paths of the hook within their budgets, nothing of the
#      game's thread object may change, the printf family must not run at all outside the HLE proper, one Log() or
#      DebugEvent() call must cost a few words at debug 0, and the log lines must read as they did.
#   bash Tests/stack_usage.sh        (Git Bash, from anywhere; Docker running, pablomk7/libctrpf:0.8.0 available;
#                                      the chain test needs Python 3 with the `unicorn` package: pip install unicorn;
#                                      CHAIN_SIM=skip leaves it out, KEEP_WORK=1 keeps the build copy)
set -e
cd "$(dirname "$0")/.."
WORK=$(mktemp -d)
if [ -z "$KEEP_WORK" ]; then trap 'rm -rf "$WORK"' EXIT; else echo "stack usage: build copy kept in $WORK"; fi
tar --exclude=Build --exclude=analysis --exclude='*.elf' --exclude='*.3gx' --exclude=dist --exclude=Tests \
    --exclude=ConfigApp -cf - . | tar -xf - -C "$WORK"
# (the Makefile's flags + -fstack-usage; the elf and 3gx are not needed, only the objects)
sed -i 's/-fno-strict-aliasing/-fno-strict-aliasing -fstack-usage/' "$WORK/Makefile"
(cd "$WORK" && MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src \
    pablomk7/libctrpf:0.8.0 make >"$WORK/build.log" 2>&1) || { tail -20 "$WORK/build.log"; exit 1; }

FAILED=0
# $1 = file (without .su), $2 = function name as it appears in the listing, $3 = budget in bytes
budget() {
  local used
  used=$(grep -F -- "$2(" "$WORK/Build/$1.su" | head -1 | awk -F'\t' '{print $2}')
  if [ -z "$used" ]; then
    echo "stack usage: $2 not found in $1.su"
    FAILED=1
  elif [ "$used" -gt "$3" ]; then
    echo "stack usage FAILED: $2 uses $used bytes of stack, budget $3"
    FAILED=1
  else
    printf 'stack usage: %-52s %4s bytes (budget %s)\n' "$2" "$used" "$3"
  fi
}
# Measured 2026-10-10 (0.1.15.1): the 0.1.15 numbers of the HLE frames (16 / 48 / 112 / 40 / 232 / 48 / 80) did not change.
budget uds_hle "azl::ctr::UdsHle::HandleSendSyncRequest" 64
budget uds_hle "azl::ctr::UdsHle::Run" 32
budget uds_hle "azl::ctr::UdsHle::Dispatch" 128
budget uds_engine "azl::UdsEngine::RecvBeaconBroadcastData" 64
budget uds_engine "azl::UdsEngine::ScanBeacons" 256
budget uds_engine "azl::UdsEngine::BeginHostingNetwork" 64
budget uds_engine "azl::UdsEngine::Bind" 96
budget uds_engine "azl::UdsEngine::Initialize" 64
# The debug capture's frame is paid only with debug=1 (it is kept out of line for that reason): the copy of the request
# is as long as the words the header declares (it was a fixed 256 bytes until 0.1.16: the frame was 304); the capture
# takes the raw words and the writer thread makes the lines
budget uds_hle "azl::ctr::UdsHle::RunTraced" 80
# Every IPC call of every game thread that the HLE does not answer goes through the hook below. The common case (a call
# to a real service nobody traces) is PassThrough alone - 24 bytes, because the branches with locals are out of line
# (PassThroughGetService, PassThroughTraced) - plus the capture's DebugIpc at debug=1, which stores raw words.
budget hook "azl::ctr::PassThrough" 32
budget hook "azl::ctr::PassThroughApt" 72   # (0.1.16: +8 for the observer of the sleep protocol, which the replies call before their svc)
budget hook "azl::ctr::PassThroughGetService" 80
budget hook "azl::ctr::PassThroughTraced" 56
budget hook "azl_hook_SendSyncRequest" 24
budget hook "azl_hook_CloseHandle" 24
# (0.1.16: DebugIpc writes its record from registers, with no record or array on the stack - 64 bytes before, with the diet and
# the lanes in it 40 - so the pure pass-through path at debug 1 is 24 bytes shallower than the hotfix's; Tests/chain_sim pins that)
budget log "azl::DebugIpc" 48
budget log "azl::DebugSvcReal" 80
budget log "azl::DebugLine::~DebugLine" 40
budget log "azl::LogLine::~LogLine" 24
budget log_files "azl::LogFiles::Append" 80
# The fake sessions' open and close, on the game's thread that makes the call (no printf there any more: 5 lines of
# 0.1.15 used a 1 KB snprintf each)
budget uds_hle "azl::ctr::UdsHle::NoteHandleClosed" 80
budget uds_hle "azl::ctr::UdsHle::InterceptGetServiceHandle" 96
budget plugin "azl::ctr::Plugin::OnSleepEnter" 48
budget plugin "azl::ctr::Plugin::OnHomeButton" 16
# 0.1.16: the trail of a sleep (the observer the APT notifications and replies call), the role test of the sleep action, and the
# statistics / throttle leg that follows every served request of a fake session (out of line: HandleSendSyncRequest's own
# frame is the one the pure pass-through path pays, and it has not grown)
budget plugin "azl::ctr::Plugin::NoteSleepEvent" 32
budget plugin "azl::ctr::Plugin::SleepActs" 16
budget uds_hle "azl::ctr::UdsHle::AccountCall" 40
budget uds_hle "azl::ctr::UdsHle::ThrottleFailing" 40
# What the front reaches in the engine and the room client when a session ends on the game's thread (no printf there any
# more: the role line was a Log() + a DebugEvent(), a Deauthentication's capture line a DebugEvent())
budget uds_engine "azl::UdsEngine::TraceRoleLocked" 40
budget uds_engine "azl::UdsEngine::StopService" 56
budget uds_engine "azl::UdsEngine::Shutdown" 32
budget log "azl::DebugWifiLine" 56
budget room_client "azl::{anonymous}::DebugWifi" 40
# The 400 / 200-byte lines of Log() / DebugEvent() live in frames of their own (LogV / DebugEventV): the variadic entries,
# which every call pays at every debug level, are a few words (their .su names are garbled by the "...": the chain test
# measures them, 32 bytes at debug 0). The formatting frames are only paid with the log on.
budget log "azl::LogV" 48
budget log "azl::DebugEventV" 56
# 0.1.16: the formatter that Log() / DebugEvent() use (azl/printf_lite.hpp) instead of newlib's vsnprintf chain (1.4 KB)
budget log "azl::FormatLine" 48
budget log "azl::printf_lite::Format" 96
budget log "azl::printf_lite::detail::Integer" 72
# 0.1.16: the producers of the capture (the ring's lanes, the LOST markers, the diet, the raw command records)
budget log "azl::DebugFrame" 64
budget log "azl::DebugBeacon" 56
budget log "azl::DebugCommand" 48
budget log "azl::DebugServiceCommand" 64
budget log "azl::DebugPullPacket" 56
budget log "azl::{anonymous}::PushCommand" 48
budget log "azl::{anonymous}::PushLocked" 56
budget log "azl::{anonymous}::PushAggregateLocked" 72
# (the marker of a hole is written by whichever producer finds the hole, Tri Force Heroes' 464-byte thread among them: 8 bytes)
budget log "azl::{anonymous}::EmitLostLocked" 16
budget log "azl::{anonymous}::FoldMissesLocked" 24
[ "$FAILED" = 0 ] && echo "stack usage: all frames within budget"

# ---- which Python: the call graph check needs any Python 3, the chain test one with `unicorn` too ---------------------------
PY_ANY=""
PY=""
for candidate in python3 python py; do
  if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c "import sys; sys.exit(sys.version_info[0] < 3)" >/dev/null 2>&1; then
    [ -z "$PY_ANY" ] && PY_ANY="$candidate"
    if "$candidate" -c "import unicorn" >/dev/null 2>&1; then
      PY="$candidate"
      break
    fi
  fi
done

# ---- can a game's thread reach a formatter? answered from the machine code (Tests/callgraph_check.py) --------------------
if [ -z "$PY_ANY" ]; then
  echo "stack usage FAILED: no Python 3 for Tests/callgraph_check.py"
  FAILED=1
elif [ ! -f "$WORK/AzaharLink.elf" ]; then
  echo "stack usage FAILED: the build copy has no AzaharLink.elf"
  FAILED=1
else
  "$PY_ANY" Tests/callgraph_check.py --selftest || FAILED=1
  cp Tests/callgraph_dis.sh "$WORK/callgraph_dis.sh"
  (cd "$WORK" && MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src \
      pablomk7/libctrpf:0.8.0 bash /src/callgraph_dis.sh /src/AzaharLink.elf /src/cg) || { echo "stack usage FAILED: could not disassemble the ELF"; FAILED=1; }
  if [ -f "$WORK/cg.dis" ]; then
    "$PY_ANY" Tests/callgraph_check.py "$WORK/cg" || FAILED=1
  fi
fi

# ---- the whole chain, libc included ------------------------------------------------------------------------------
if [ "$CHAIN_SIM" = "skip" ]; then
  echo "stack usage: the chain test was skipped (CHAIN_SIM=skip)"
else
  if [ -z "$PY" ]; then
    echo "stack usage FAILED: no Python 3 with the unicorn package for Tests/chain_sim (pip install unicorn; or CHAIN_SIM=skip)"
    FAILED=1
  elif [ ! -f "$WORK/AzaharLink.elf" ]; then
    echo "stack usage FAILED: the build copy has no AzaharLink.elf"
    FAILED=1
  else
    "$PY" Tests/chain_sim/chain_sim.py "$WORK/AzaharLink.elf" || FAILED=1
  fi
fi
exit $FAILED
