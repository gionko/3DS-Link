#!/bin/bash
# The stack and the size of the fake Download Play lobby on the plugin's own target (DESIGN-FASE1 sez. 0 and 1.3, rules 3 and 4
# of the user). The three headers (Includes/azl/dlp_types.hpp, dlp_lobby.hpp, dlp_ipc.hpp) run on the game thread that made the
# IPC call, and some of those threads have 464 bytes below it. The PC tests are x86-64 with the sanitizers: they see neither the
# stack of an ARM frame nor what -fno-rtti -fno-exceptions -std=gnu++17 for armv6k says about the code. This script does:
#   1. compiles Tests/dlp_stack_probe.cpp with arm-none-eabi-g++ and the flags of the Makefile, plus -Wextra -Wpedantic -Wshadow
#      -Werror (the headers must be clean for the plugin's compiler), -fstack-usage and -fcallgraph-info=su. The probe holds the
#      ARM32 sizes as assertions and one noinline wrapper for every public function of DlpLobby and DlpService. Two passes:
#        A, as the plugin would build it: the compiler inlines as it likes and the wrappers are the entries. Here the sum of the
#          frames from a function of the headers down to its deepest call out of the module (LobbyNet, the game's address
#          mapper) is held to CHAIN_MAX: "Dispatch down to the first call into LobbyNet <= 160". A wrapper that merely calls the
#          function it is named after (p_Dispatch calls Dispatch) is the probe's own shim and is not counted; one that has the
#          function inlined in it is counted whole;
#        B, with -fkeep-inline-functions: every function of the headers is in the listing, wrapper or not (a public function
#          that has no wrapper yet, a helper that pass A inlines everywhere). Here the sums are not held (a function the compiler
#          must keep is inlined less, so the sums are higher than the plugin's), only the frames and the circles;
#   2. fails when a function of the headers has a frame over FRAME_MAX bytes (DlpService::Dispatch FRAME_DISPATCH, DlpLobby::
#      PollScan FRAME_POLLSCAN: the one that holds the lobby heard), a dynamic frame (alloca, a variable length array), or when
#      the functions call each other in a circle (no recursion);
#   3. proves itself first: a made-up listing with a frame over its budget, a dynamic frame, a chain over its budget, a
#      recursion and a missing entry must each be refused, a good one accepted (a gate that cannot say no is not a gate).
# The plugin's own translation unit decides the real inlining: phase 3 measures the same functions again there
# (Tests/stack_usage.sh). Where the compiler is: in the PATH, in $DEVKITARM/bin, or in /opt/devkitpro/devkitARM/bin (the
# devkitpro/devkitarm container has it there and not in the PATH). Without it the check is SKIPPED and says so loudly;
# DLP_STACK_REQUIRED=1 makes that a failure. Needs Python 3.
#   bash Tests/dlp_stack_check.sh        (from anywhere; run by Tests/run.sh)
set -e
cd "$(dirname "$0")/.."

FRAME_MAX=48        # bytes: the largest frame of any function of the three headers (the rule of the user: locals up to about 64 bytes)
FRAME_DISPATCH=64   # bytes: DlpService::Dispatch, the function every command passes through
FRAME_POLLSCAN=64   # bytes: DlpLobby::PollScan, which holds the lobby heard (36 bytes) while it asks the network for the next one
CHAIN_MAX=160       # bytes: from a function of the headers down to its deepest call out of the module, every frame on the way counted

# the flags of the Makefile (ARCH, CFLAGS, CXXFLAGS; the defines of the 3DS and libctru are not needed by these headers), then the strict ones
ARM_FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -Os -mword-relocations -fomit-frame-pointer -ffunction-sections -fno-strict-aliasing -Wall -Wno-psabi -fno-rtti -fno-exceptions -std=gnu++17"
STRICT_FLAGS="-Wextra -Wpedantic -Wshadow -Werror -fstack-usage -fcallgraph-info=su"

ARM_GXX=""
for candidate in "$(command -v arm-none-eabi-g++ 2>/dev/null || true)" "${DEVKITARM:+$DEVKITARM/bin/arm-none-eabi-g++}" /opt/devkitpro/devkitARM/bin/arm-none-eabi-g++; do
  if [ -n "$candidate" ] && [ -x "$candidate" ]; then
    ARM_GXX="$candidate"
    break
  fi
done
ARM_SIZE="$(dirname "${ARM_GXX:-/nonexistent/x}")/arm-none-eabi-size"
if [ -z "$ARM_GXX" ]; then
  if [ -n "$DLP_STACK_REQUIRED" ]; then
    echo "dlp stack: FAILED - no arm-none-eabi-g++ (in the PATH, in \$DEVKITARM/bin or in /opt/devkitpro/devkitARM/bin) and DLP_STACK_REQUIRED is set"
    exit 1
  fi
  echo "dlp stack: SKIPPED - no arm-none-eabi-g++ (in the PATH, in \$DEVKITARM/bin or in /opt/devkitpro/devkitARM/bin): the frames of the fake lobby on the plugin's target were NOT measured"
  exit 0
fi
PY=""
for candidate in python3 python; do
  if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c "import sys; sys.exit(sys.version_info[0] < 3)" >/dev/null 2>&1; then
    PY="$candidate"
    break
  fi
done
if [ -z "$PY" ]; then
  echo "dlp stack: FAILED - no Python 3 to read the listings of the compiler"
  exit 1
fi

W="${OUT:-/tmp/azl_tests}/dlp_stack"
rm -rf "$W"
mkdir -p "$W"

cat > "$W/check.py" <<'PY'
import os
import re
import sys

HEADERS = ('dlp_types.hpp', 'dlp_lobby.hpp', 'dlp_ipc.hpp', 'common.hpp')
PROBE = 'dlp_stack_probe.cpp'
INDIRECT = '__indirect_call'
ENTRIES = ('Dispatch', 'PollPending', 'OnSessionClosed')   # the IPC front-end: each must have a chain that is measured


def in_headers(path):
    return os.path.basename(path) in HEADERS


def in_probe(path):
    return os.path.basename(path) == PROBE


def fname(label):
    """azl::DlpLobby::PollScan from 'void azl::DlpLobby::PollScan()' (the probe's own functions have no class)"""
    return label.split('(')[0].split(' ')[-1]


def parse_su(text):
    """[(file, line, function, bytes, qualifier)] of the .su listing (file:line:column:function <TAB> bytes <TAB> qualifier)"""
    rows = []
    for raw in text.splitlines():
        parts = raw.rsplit('\t', 2)
        if len(parts) != 3:
            continue
        head, size, qual = parts
        m = re.match(r'^(.*?):(\d+):(\d+):(.*)$', head)
        if m:
            rows.append((m.group(1), int(m.group(2)), m.group(4), int(size), qual.strip()))
    return rows


def parse_ci(text):
    """({title: (function, file, bytes)}, {title: [targets]}) of the -fcallgraph-info=su listing"""
    nodes = {}
    edges = {}
    for m in re.finditer(r'node: \{ title: "([^"]*)" label: "([^"]*)"', text):
        label = m.group(2).split('\\n')
        size = 0
        for line in label[2:]:
            q = re.match(r'^(\d+) bytes \(', line)
            if q:
                size = int(q.group(1))
        nodes[m.group(1)] = (label[0], re.sub(r':\d+:\d+$', '', label[1]) if len(label) > 1 else '', size)
    for m in re.finditer(r'edge: \{ sourcename: "([^"]*)" targetname: "([^"]*)"', text):
        edges.setdefault(m.group(1), []).append(m.group(2))
    return nodes, edges


def analyse(su_text, ci_text, frame_max, dispatch, pollscan, chain_max, wrappers, chains, required):
    """(errors, report). chains: hold the sums of the frames (pass A); wrappers: the number the probe has (None = do not count)"""
    errors = []
    report = []
    rows = [r for r in parse_su(su_text) if in_headers(r[0]) or in_probe(r[0])]
    if not rows:
        return ['the listing of the compiler has no function of the headers: nothing was measured'], report
    seen = sum(1 for r in rows if in_probe(r[0]) and fname(r[2]).startswith('p_'))
    if wrappers is not None and seen != wrappers:
        errors.append('the probe has %d wrappers and the listing shows %d of them (one was folded into another, or lost)' % (wrappers, seen))
    for path, line, name, size, qual in rows:
        limit = dispatch if 'azl::DlpService::Dispatch(' in name else (pollscan if 'azl::DlpLobby::PollScan(' in name else frame_max)
        if size > limit:
            errors.append('frame of %d bytes over the budget of %d: %s (%s:%d)' % (size, limit, name, os.path.basename(path), line))
        if 'dynamic' in qual:
            errors.append('dynamic frame (%s): %s (%s:%d)' % (qual, name, os.path.basename(path), line))
    worst = max(rows, key=lambda r: r[3])
    report.append('%d functions measured (%d wrappers of the probe); largest frame %d bytes (%s)' % (len(rows), seen, worst[3], fname(worst[2])))

    nodes, edges = parse_ci(ci_text)
    mine = [t for t, v in nodes.items() if t != INDIRECT and (in_headers(v[1]) or in_probe(v[1]))]
    best = {}      # title -> (bytes, path) down to a call out of the module, or None
    state = {}     # title -> 1 while on the way down, 2 when done
    cycles = []

    def visit(t, path):
        if t == INDIRECT:
            return (0, [])
        if t not in nodes:
            return None      # (memcpy, memset: not a call out of the module)
        if state.get(t) == 2:
            return best[t]
        if state.get(t) == 1:
            cycles.append(' > '.join(fname(nodes[p][0]) for p in path[path.index(t):] + [t]))
            return None
        state[t] = 1
        found = None
        for target in edges.get(t, []):
            sub = visit(target, path + [t])
            if sub is not None and (found is None or sub[0] > found[0]):
                found = sub
        best[t] = None if found is None else (nodes[t][2] + found[0], ['%s=%d' % (fname(nodes[t][0]), nodes[t][2])] + found[1])
        state[t] = 2
        return best[t]

    for t in mine:
        visit(t, [])
    for c in sorted(set(cycles)):
        errors.append('the functions call each other in a circle (no recursion): ' + c)
    if not chains:
        return errors, report
    # the functions whose sums are held: those of the headers, and a wrapper that has a function inlined in it (one that merely calls
    # the function it is named after is the probe's shim, 8 to 16 bytes of frame that the plugin's caller pays in its own way)
    roots = []
    for t in mine:
        label, path, _ = nodes[t]
        if in_headers(path):
            roots.append(t)
        elif in_probe(path):
            wrapped = fname(label)[2:]
            if not any(c in nodes and in_headers(nodes[c][1]) and fname(nodes[c][0]).split('::')[-1] == wrapped for c in edges.get(t, [])):
                roots.append(t)
    deep = sorted(((best[t][0], best[t][1]) for t in roots if best.get(t)), key=lambda c: -c[0])
    for depth, path in deep:
        if depth > chain_max:
            errors.append('chain of %d bytes over the budget of %d down to a call out of the module: %s' % (depth, chain_max, ' > '.join(path)))
    if deep:
        report.append('deepest chain down to a call out of the module: %d bytes (budget %d): %s' % (deep[0][0], chain_max, ' > '.join(deep[0][1])))
    else:
        errors.append('the call graph has no chain down to a call out of the module: nothing was measured')
    for name in required:
        sums = [best[t][0] for t in roots if best.get(t) and fname(nodes[t][0]).split('::')[-1] in (name, 'p_' + name)]
        if not sums:
            errors.append('no chain is measured for %s (it is neither a function of its own nor inlined in a wrapper that reaches the network)' % name)
        else:
            report.append('%s: %d bytes down to a call out of the module' % (name, max(sums)))
    return errors, report


def fake(entries, calls):
    """a made-up .su and .ci. entries = [(name, bytes, qualifier)]; a name that starts with p_ is a function of the probe, any other a
    member of DlpLobby. calls = [(from, to)] by name; 'NET' is a call out of the module."""
    su = []
    ci = ['graph: { title: "x.cpp"', 'node: { title: "%s" label: "Indirect Call Placeholder" shape : ellipse }' % INDIRECT]
    for i, (name, size, qual) in enumerate(entries):
        if name.startswith('p_'):
            where, func = 'Tests/dlp_stack_probe.cpp', 'void %s()' % name
        else:
            where, func = 'Includes/azl/dlp_lobby.hpp', 'void azl::DlpLobby::%s()' % name
        su.append('%s:%d:5:%s\t%d\t%s' % (where, 10 + i, func, size, qual))
        ci.append('node: { title: "x.cpp:%s" label: "%s\\n%s:%d:5\\n%d bytes (%s)\\n0 dynamic objects" }' % (name, func, where, 10 + i, size, qual))
    for a, b in calls:
        ci.append('edge: { sourcename: "x.cpp:%s" targetname: "%s" label: "x:1:1" }' % (a, INDIRECT if b == 'NET' else 'x.cpp:' + b))
    return '\n'.join(su) + '\n', '\n'.join(ci) + '\n}\n'


def selftest():
    good = [('A', 24, 'static'), ('B', 48, 'static'), ('C', 48, 'static'), ('PollScan', 64, 'static')]
    good_calls = [('A', 'B'), ('B', 'C'), ('C', 'NET'), ('PollScan', 'NET')]
    line = [('D', 40, 'static')]            # A 24 + B 48 + C 48 + D 40 = 160
    line_calls = [('A', 'B'), ('B', 'C'), ('C', 'D'), ('D', 'NET')]
    entry = [('Dispatch', 24, 'static'), ('Sv', 48, 'static'), ('Poll', 48, 'static'), ('Scan', 40, 'static')]   # 160
    entry_calls = [('Dispatch', 'Sv'), ('Sv', 'Poll'), ('Poll', 'Scan'), ('Scan', 'NET')]
    # (name, entries, calls, chains, required, must be refused)
    cases = [
        ('a good listing is accepted', good, good_calls, True, (), False),
        ('a frame of 56 bytes is refused', good + [('Big', 56, 'static')], good_calls + [('Big', 'NET')], True, (), True),
        ('a dynamic frame is refused', good + [('Dyn', 8, 'dynamic,bounded')], good_calls + [('Dyn', 'NET')], True, (), True),
        ('a chain of 168 bytes is refused', good + [('D', 48, 'static')], line_calls, True, (), True),
        ('a chain of 160 bytes is accepted', good + line, line_calls, True, (), False),
        ('a chain of 168 bytes is not held in pass B', good + [('D', 48, 'static')], line_calls, False, (), False),
        ('a recursion is refused', [('A', 16, 'static'), ('B', 16, 'static')], [('A', 'B'), ('B', 'A'), ('B', 'NET')], True, (), True),
        ('a recursion is refused in pass B too', [('A', 16, 'static'), ('B', 16, 'static')], [('A', 'B'), ('B', 'A'), ('B', 'NET')], False, (), True),
        ('a listing without a chain is refused', [('A', 16, 'static')], [], True, (), True),
        ('the entry Dispatch is measured', entry, entry_calls, True, ('Dispatch',), False),
        ('a missing entry is refused', good, good_calls, True, ('Dispatch',), True),
        ('the shim of a wrapper is not counted', entry + [('p_Dispatch', 48, 'static')], entry_calls + [('p_Dispatch', 'Dispatch')], True, ('Dispatch',), False),
        ('a wrapper with a function inlined in it is', entry[1:] + [('p_Inlined', 48, 'static')], entry_calls[1:] + [('p_Inlined', 'Sv')], True, (), True),
    ]
    bad = 0
    for name, entries, calls, chains, required, refused in cases:
        su, ci = fake(entries, calls)
        errors, _ = analyse(su, ci, 48, 64, 64, 160, None, chains, required)
        ok = bool(errors) == refused
        print('dlp stack self-test: %-46s %s' % (name, 'ok' if ok else 'WRONG: ' + str(errors)))
        bad += 0 if ok else 1
    # 64 bytes is the budget of PollScan and of nothing else
    su, ci = fake([('PollScan', 64, 'static')], [('PollScan', 'NET')])
    ok = not analyse(su, ci, 48, 64, 64, 160, None, True, ())[0]
    su, ci = fake([('Other', 64, 'static')], [('Other', 'NET')])
    ok = ok and bool(analyse(su, ci, 48, 64, 64, 160, None, True, ())[0])
    print('dlp stack self-test: %-46s %s' % ('64 bytes is for PollScan only', 'ok' if ok else 'WRONG'))
    return bad + (0 if ok else 1)


if __name__ == '__main__':
    if sys.argv[1] == '--selftest':
        sys.exit(1 if selftest() else 0)
    su_path, ci_path, mode = sys.argv[1], sys.argv[2], sys.argv[3]
    frame_max, dispatch, pollscan, chain_max, wrappers = (int(x) for x in sys.argv[4:9])
    pass_a = mode == 'A'
    errors, report = analyse(open(su_path, errors='replace').read(), open(ci_path, errors='replace').read(), frame_max, dispatch, pollscan, chain_max,
                             wrappers if pass_a else None, pass_a, ENTRIES if pass_a else ())
    for line in report:
        print('dlp stack [%s]: %s' % (mode, line))
    for e in errors:
        print('dlp stack [%s] FAILED: %s' % (mode, e))
    sys.exit(1 if errors else 0)
PY

"$PY" "$W/check.py" --selftest

# (the probe is compiled from the root of AzaharLink so that the listings name the files as the sources do)
echo "dlp stack: $("$ARM_GXX" --version | head -1), the flags of the Makefile; the probe holds the ARM32 sizes (DlpLobby <= 272, DlpService <= 40) as assertions"
WRAPPERS=$(grep -c '^PROBE_NOINLINE' Tests/dlp_stack_probe.cpp || true)
for pass in A B; do
  extra=""
  [ "$pass" = B ] && extra="-fkeep-inline-functions"
  if ! "$ARM_GXX" $ARM_FLAGS $STRICT_FLAGS $extra -IIncludes -c Tests/dlp_stack_probe.cpp -o "$W/probe_$pass.o"; then
    echo "dlp stack [$pass] FAILED: Tests/dlp_stack_probe.cpp does not compile for the plugin's target ($ARM_GXX $ARM_FLAGS $STRICT_FLAGS $extra)"
    exit 1
  fi
  [ -f "$W/probe_$pass.su" ] && [ -f "$W/probe_$pass.ci" ] || { echo "dlp stack [$pass] FAILED: the compiler wrote no stack listing ($W/probe_$pass.su, probe_$pass.ci)"; exit 1; }
  "$PY" "$W/check.py" "$W/probe_$pass.su" "$W/probe_$pass.ci" "$pass" "$FRAME_MAX" "$FRAME_DISPATCH" "$FRAME_POLLSCAN" "$CHAIN_MAX" "$WRAPPERS"
  # nothing may be kept in memory (rule 2 of the user): the headers hold no static storage at all. sizeof and the frames do not see
  # a `static u8 scratch[4096]` (a member or a namespace variable): the object file does (data and bss stay at 0, constants go to rodata).
  # The probe has no state of its own, so any byte here is the headers'. (If the size tool is missing the check is skipped aloud.)
  if [ -x "$ARM_SIZE" ]; then
    read -r _text data bss _rest < <("$ARM_SIZE" "$W/probe_$pass.o" | awk 'NR == 2')
    if [ "$data" != 0 ] || [ "$bss" != 0 ]; then
      echo "dlp stack [$pass] FAILED: the fake lobby keeps static storage (data $data, bss $bss bytes in the object file of the probe): nothing may be kept in memory, the state lives in DlpLobby and DlpService"
      exit 1
    fi
  else
    echo "dlp stack [$pass]: no arm-none-eabi-size beside the compiler: the static storage was NOT measured"
  fi
done

# ---- pass H: the hub (Includes/azl/dlp_hub.hpp; DESIGN-FASE3 sez. 2) ---------------------------------------------------------------------------
# The probe is compiled once more, with -DDLP_PROBE_HUB and the defines and the libctru headers of the plugin's own build (the hub's header includes
# the UDS engine's, and a Mutex is a 4-byte LightLock only with -D__3DS__): the sizes of the hub are held as assertions (a growth is a decision), one
# noinline wrapper lists every public function of DlpHub, the frame of what the header itself defines is held to FRAME_MAX, and the probe keeps no
# static storage. The functions of the hub proper are in Sources/azl/dlp_hub.cpp: Tests/dlp_net_stack_check.sh holds their frames, and
# Tests/stack_usage.sh the chains they add up to in the plugin's own unit.
HUB_FLAGS="-D__3DS__ -DAZL_USE_CTRPF -DDLP_PROBE_HUB"
CTRU=""
for candidate in "${DEVKITPRO:+$DEVKITPRO/libctru/include}" /opt/devkitpro/libctru/include; do
  if [ -n "$candidate" ] && [ -f "$candidate/3ds.h" ]; then
    CTRU="$candidate"
    break
  fi
done
if [ -z "$CTRU" ]; then
  if [ -n "$DLP_STACK_REQUIRED" ]; then
    echo "dlp stack [H]: FAILED - no libctru headers (\$DEVKITPRO/libctru/include or /opt/devkitpro/libctru/include) and DLP_STACK_REQUIRED is set"
    exit 1
  fi
  echo "dlp stack [H]: SKIPPED - no libctru headers: the hub's sizes and surface on the plugin's target were NOT held"
else
  HUB_WRAPPERS=$(grep -c '^HUB_NOINLINE' Tests/dlp_stack_probe.cpp || true)
  if ! "$ARM_GXX" $ARM_FLAGS $HUB_FLAGS $STRICT_FLAGS -isystem "$CTRU" -IIncludes -c Tests/dlp_stack_probe.cpp -o "$W/probe_H.o"; then
    echo "dlp stack [H] FAILED: Tests/dlp_stack_probe.cpp does not compile with the hub for the plugin's target ($ARM_GXX $ARM_FLAGS $HUB_FLAGS $STRICT_FLAGS)"
    exit 1
  fi
  [ -f "$W/probe_H.su" ] || { echo "dlp stack [H] FAILED: the compiler wrote no stack listing ($W/probe_H.su)"; exit 1; }
  seen=$(awk -F'\t' '$1 ~ /p_hub_/ { n++ } END { print n + 0 }' "$W/probe_H.su")
  if [ "$seen" != "$HUB_WRAPPERS" ]; then
    echo "dlp stack [H] FAILED: the probe has $HUB_WRAPPERS hub wrappers and the listing shows $seen of them (one was folded into another, or lost)"
    exit 1
  fi
  # (the header's own functions - the counters, EngineHeld - and the wrappers: none over FRAME_MAX, none dynamic)
  if ! awk -F'\t' -v max="$FRAME_MAX" '$1 ~ /dlp_hub[.]hpp|dlp_lobby_net[.]hpp|p_hub_/ && (($2 + 0 > max + 0) || ($3 !~ /static/)) { print "dlp stack [H] FAILED: " $1 " " $2 " " $3; bad = 1 } END { exit bad }' "$W/probe_H.su"; then
    exit 1
  fi
  if [ -x "$ARM_SIZE" ]; then
    read -r _text data bss _rest < <("$ARM_SIZE" "$W/probe_H.o" | awk 'NR == 2')
    if [ "$data" != 0 ] || [ "$bss" != 0 ]; then
      echo "dlp stack [H] FAILED: the hub's header keeps static storage (data $data, bss $bss bytes in the object file of the probe): nothing may be kept in memory but the hub object itself"
      exit 1
    fi
  else
    echo "dlp stack [H]: no arm-none-eabi-size beside the compiler: the static storage was NOT measured"
  fi
  echo "dlp stack [H]: the hub's sizes (DlpHub <= 408, EngineLobbyNet <= 12) are held, $HUB_WRAPPERS wrappers list its public functions, no frame over $FRAME_MAX, no static storage"
fi
echo "dlp stack: every frame of the fake lobby within its budget on ARM32 (frames <= $FRAME_MAX, Dispatch <= $FRAME_DISPATCH, PollScan <= $FRAME_POLLSCAN, chains <= $CHAIN_MAX)"
