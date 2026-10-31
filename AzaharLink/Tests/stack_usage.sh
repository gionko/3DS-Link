#!/bin/bash
# The stack a GAME thread pays for the plugin, which is the one resource the hook takes from threads it does not own.
# The hooked svc 0x32 runs the plugin's C++ on whatever stack the thread that made the IPC call has, and the SDK threads
# of some games have very little: Tri Force Heroes' srv-notification thread has 464 bytes below its call (a 512-byte
# stack inside its own object), and 0.1.15 at debug=1 ran a printf chain of 1.3 KB there - the game crashed on exit
# (hardware, 2026-10-10). Four checks:
#   1. the frames of the functions a game thread runs through (-fstack-usage on a COPY of the tree with the shipped
#      flags): fails when a frame is over its budget (the measured size plus a margin). This sees one frame at a time,
#      not the libc functions below it;
#   2. the call graph (Tests/callgraph_check.py): the built ELF is disassembled and every function a game thread can
#      reach from the entries of the hook - and from the callbacks it reaches through a pointer - is followed; the printf
#      family, Log() and DebugEvent() must not be among them (the HLE proper, past UdsHle::Run, is the one cut);
#   3. the chains of the Download Play hub (Tests/dlp_chain_report.py, phase 3): the frames and the call edges of every unit
#      of THIS build joined by symbol, the libraries' frames read from the machine code, and for every way a game thread
#      enters the hub - a command served inline, a command posted to the network thread, the wait for a join, OnGameUdsStarting,
#      OnSessionClosed - the deepest chain against the ceilings of the chain test; plus the proofs that an inline command reaches
#      no LobbyNet method but the clock, that the end of a session reaches nothing of the lobby, that no row reaches Log() with a
#      format and that a hub session does not go through RunTraced;
#   4. the WHOLE chain (Tests/chain_sim/chain_sim.py): the real code of the built plugin, libc included, run in an ARM
#      emulator on a 512-byte game stack, entered like the per-site trampoline does: the pure pass-through path must stay
#      within 300 bytes at debug 0 and at debug 1, the other paths of the hook within their budgets, nothing of the
#      game's thread object may change, the printf family must not run at all outside the HLE proper, one Log() or
#      DebugEvent() call must cost a few words at debug 0, and the log lines must read as they did. With the hub: the compiled
#      DlpHub, DlpService and DlpLobby run commands, waits and closes with a spy LobbyNet (no method but the clock on the game
#      thread) and over the real engine, the commands that succeed in every state with no network thread to serve them (Finalize,
#      StopScan, StopSession, EndAccepting: answered with success, the hub object compared word by word), and the deepest SP is read.
#   bash Tests/stack_usage.sh        (Git Bash, from anywhere; Docker running, pablomk7/libctrpf:0.8.0 available;
#                                      the chain test needs Python 3 with the `unicorn` package: pip install unicorn;
#                                      CHAIN_SIM=skip leaves it out, KEEP_WORK=1 keeps the build copy)
set -e
cd "$(dirname "$0")/.."
WORK=$(mktemp -d)
if [ -z "$KEEP_WORK" ]; then trap 'rm -rf "$WORK"' EXIT; else echo "stack usage: build copy kept in $WORK"; fi
# (only what the Makefile builds from: the project folder also holds gigabytes of console logs, captures and installers, and a copy of
#  all of it took minutes and 12 GB of the disk for a build that needs 2 MB)
tar -cf - Makefile 3gx.ld CTRPluginFramework.plgInfo Includes Sources third_party | tar -xf - -C "$WORK"
# (the Makefile's flags + -fstack-usage and the call graph of every unit, which Tests/dlp_chain_report.py joins; the build makes the ELF
#  that the checks below read)
sed -i 's/-fno-strict-aliasing/-fno-strict-aliasing -fstack-usage -fcallgraph-info=su/' "$WORK/Makefile"
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
# The Download Play hub (phase 3, DESIGN-FASE3.md sez. 1 and 4): what a game thread pays on a dlp:FKCL / dlp:SRVR call. RunDlp is the
# frame the hook's chain enters the hub with (Serve and the log lines are called from it; RunDlpHub is inlined in it); the functions
# of the hub are in their own unit. The chains they add up to are held below (Tests/dlp_chain_report.py), and the whole path in the
# emulator by Tests/chain_sim.
# (RunDlp is 32 bytes. It was 40 once, for one release of the code: the trace of the commands (debug only, "dlpc: " lines) tested the debug level
#  in RunDlp itself, which made the compiler keep the address of g_debug_level in a register across the function: 8 bytes more at EVERY debug
#  level, in the frame of every hub command. The two trace functions test the level themselves, first thing, and leave at once at debug 0.
#  So the budget is 32: the growth is a decision, and Tests/chain_sim holds the chains (296 / 412 for the tightest inline command).)
budget uds_hle "azl::ctr::UdsHle::RunDlp" 32
budget uds_hle "azl::ctr::UdsHle::EndInitializedSession" 16
# (NoteDlpFirst is 48 - it was 32: its line names the thread that made the first call of the service and its priority - the hub's threads
#  wait for each other's locks and a LightLock has no priority inheritance, so the game's dlp threads against the network thread's 0x2F is
#  what the console test reads from it - and the two svc calls (svcGetThreadPriority, svcGetThreadId) take two locals and the registers that
#  hold the service and the id across them. It runs once per service, at the first command, and it is not the tightest chain: RunDlp >
#  NoteDlpFirst is 192 / 340 bytes in Tests/dlp_chain_report.py against 320 / 520, and the tightest chain of the hub is untouched.)
budget uds_hle "azl::ctr::{anonymous}::NoteDlpFirst" 48
budget uds_hle "azl::ctr::{anonymous}::NoteDlpTrouble" 40
# The trace of the hub's commands: the request words and the clock (DlpTraceRequest), the line (DlpTraceReply: a LogLine and a handful of
# locals; its chain is a sibling of NoteCommand's and Tests/chain_sim reads it with a window). Both are called by RunDlp before it calls
# NoteCommand, which must stay its last call (a tail call: RunDlp's frame is gone when NoteCommand's Log() runs). DlpTraceReply remembers
# up to six replies per command and window (an array of u16 in the static table, not on the stack): its frame stayed at 64.
budget uds_hle "azl::ctr::{anonymous}::DlpTraceRequest" 24
budget uds_hle "azl::ctr::{anonymous}::DlpTraceReply" 72
budget uds_hle "azl::ctr::{anonymous}::WaitForJoin" 48
# (The answer to a request nobody could serve - success for the commands that cannot fail, the failure for the others - is AnswerUnserved, and the test
#  that a request is in flight before OnGameUdsStarting returns is RequestInFlight: both out of line, so that Serve (40) and OnGameUdsStarting (24) keep the
#  frames they had before those branches, which the tightest chain of the 512-byte thread pays: a growth is a decision, and the chain test pins what they cost)
budget dlp_hub "azl::DlpHub::Serve" 40
budget dlp_hub "azl::DlpHub::Post" 40
budget dlp_hub "azl::DlpHub::PollPending" 32
budget dlp_hub "azl::DlpHub::OnGameUdsStarting" 24
budget dlp_hub "azl::DlpHub::AnswerUnserved" 32
budget dlp_hub "azl::DlpHub::RequestInFlight" 24
budget dlp_hub "azl::DlpHub::OnSessionClosed" 40
budget dlp_hub "azl::DlpHub::RunHere" 32
# The small functions behind them. AlwaysSucceeds and EndsRole are the two tables of the Serve branch above (a game thread calls them), Stamp is the
# network thread's liveness stamp (Tick, at its start and at its end: a long Tick must not make the next requester find the thread dead), and
# SetPlayerName is the plugin's own call at start (the console's profile name for every list): none of the four may grow a frame.
budget dlp_hub "azl::DlpHub::AlwaysSucceeds" 8
budget dlp_hub "azl::DlpHub::EndsRole" 8
budget dlp_hub "azl::DlpHub::Stamp" 16
budget dlp_hub "azl::DlpHub::SetPlayerName" 24
# The IpcResolver of the lobby: the address of a buffer of the game is mapped by a range test with no local and no call (it runs inside
# every command that has a buffer, under Serve; DESIGN-FASE3 sez. 5.3: at most 76 bytes, 0 measured)
budget plugin "azl::ctr::{anonymous}::MapGameAddress" 76
# Every function of the hub's unit - the hub itself and the lobby's headers compiled into it - has a static frame of 64 bytes at the
# most (Tests/dlp_stack_check.sh holds 48 on the headers alone; PollScan, which holds the lobby heard, is the one 64)
if [ -f "$WORK/Build/dlp_hub.su" ]; then
  awk -F'\t' '($2 + 0 > 64) || ($3 !~ /static/) { print "stack usage FAILED: a function of the hub unit has a frame over 64 bytes or a dynamic one: " $1 " " $2 " " $3; bad = 1 } END { exit bad }' \
      "$WORK/Build/dlp_hub.su" || FAILED=1
else
  echo "stack usage FAILED: no stack listing for Sources/azl/dlp_hub.cpp"
  FAILED=1
fi
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

# ---- the chains of the Download Play hub: every way a game thread enters it, added up over the call graph of this build ---------
# (Tests/dlp_chain_report.py: the frames and the call edges of every unit - the .ci files of this build - joined by symbol, the
#  libraries' frames read from the machine code of the ELF, tail calls honoured, against the ceilings of Tests/chain_sim/chain_sim.py;
#  and the proofs that an inline command reaches nothing of LobbyNet but the clock and that no row reaches Log() with a format.
#  It runs in the build's own container: it needs the demangler of the toolchain.)
if [ ! -f "$WORK/cg.dis" ]; then
  echo "stack usage FAILED: no disassembly of the plugin for Tests/dlp_chain_report.py"
  FAILED=1
else
  REPO_WIN="$(pwd -W 2>/dev/null || pwd)"
  WORK_WIN="$(cd "$WORK" && (pwd -W 2>/dev/null || pwd))"
  MSYS_NO_PATHCONV=1 docker run --rm -v "$WORK_WIN:/src" -v "$REPO_WIN:/repo:ro" -w /src pablomk7/libctrpf:0.8.0 bash -c \
      'python3 /repo/Tests/dlp_chain_report.py --selftest && python3 /repo/Tests/dlp_chain_report.py /src/Build --src /src --repo /repo --path-map /src=/src --dis /src/cg.dis --gate' \
      || FAILED=1
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
