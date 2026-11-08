#!/usr/bin/env python3
"""Can a game's thread reach a text formatter? Answered from the machine code of the built plugin, not from the sources.

    python Tests/callgraph_check.py <prefix>        <prefix>.dis / .nm / .nmc: objdump -d, nm -n and nm -n -C of the ELF
                                                    (Tests/callgraph_dis.sh makes them; Tests/stack_usage.sh runs both)
    python Tests/callgraph_check.py --selftest      the checker against a made-up listing (no ELF needed)

Why. The svc hook runs the plugin's C++ on the stack of whatever thread of the game made an IPC call, and some of those
stacks are tiny (Tri Force Heroes' srv-notification thread: 464 bytes below its call). 0.1.15 at debug=1 ran DebugEvent ->
vsnprintf (~1.3 KB of stack) there and the game crashed on exit (hardware, 2026-10-10). Tests/policy_check.sh reads the
sources between markers; this reads the ELF: every function that can be reached from the entries of the hook (and from the
callbacks the hook reaches through a pointer), following direct calls, tail calls and function addresses kept in literal
pools, and fails if the printf family (or Log() / DebugEvent(), which end in it) is among them. A mistake the markers do not
see - a snprintf in a helper that nobody marked, a Log() in a header, a library function that grew a printf - is seen here.

What is a root: what the gate of the hook calls in the plugin (Sources/ctr/gate.s: azl_pre32 / azl_post32 around a SendSyncRequest,
azl_pre23 / azl_post23 around a CloseHandle; the gate itself is assembly in its own page, Tests/hook_sim/gate_sim.py holds it) and the
functions the hook calls through a pointer or a vtable, which a listing of direct calls cannot follow: the observers of the
game's APT session, the sink of the log, the engine's transport and the kernel operations of the engine; and, for the games that have
the Download Play hub, the hub's functions a game thread runs past the cut and the adapter methods it reaches through the lobby's vtable.

What is cut: UdsHle::Run. Past it is the HLE proper - the dispatch of a fake session's command, the engine, NdmHle - which
keeps its Log() formats, legitimately needs more stack than a small thread has, and runs on the game's UDS thread (Tests/chain_sim
measures it; moving it to a stack of the plugin's own is the 0.1.16 item). The set of what is reachable but not followed is printed.

What it cannot see: a call through a pointer that is not in the roots (the list below is the plugin's own: add a new
callback there), and a library function that formats without calling the printf family (none is known).

The chat's OSD callback (OnChatOsd, registered with OSD::Run) is a root too, and a root of another kind: the framework calls it on the
game's thread that presents a frame, once per screen and per present, and nothing of the hook reaches it (its address is only in
ChatFrame, on the plugin's thread). Besides the formatters, its DEEPEST CHAIN of frames is added up here from the machine code (each
function's prologue: push, vpush, sub sp; a tail call does not stack on its caller) and held to OSD_CHAIN_CEILING; a call through a
register or a cycle under it is a failure too (the chain could not be bounded). Tests/stack_usage.sh budgets the frames one at a time:
a helper of the callback with a buffer of its own, or a chain that grows a frame at a time, is seen here.
"""
import collections
import re
import sys

ENTRY_ROOTS = ['azl_pre32', 'azl_post32', 'azl_pre23', 'azl_post23']

# Demangled-name patterns of the callbacks the hook reaches through a pointer / a vtable / a std::function
CALLBACK_ROOTS = [
    r'^azl::ctr::Plugin::OnSleepEnter\(\)$',
    r'^azl::ctr::Plugin::OnHomeButton\(\)$',
    r'^azl::ctr::Plugin::NoteSleepEvent\(unsigned (?:int|long), unsigned (?:int|long)\)$',   # the APT observer (the trail of a sleep)
    r'^azl::ctr::Plugin::WaitForCloseTrail\(\)$',   # the hook's TrailWaiter (after the close command 12, the log on, a New 3DS only)
    r'^azl::ctr::Plugin::EarlyInit\(\)::\{lambda\(bool\)#1\}::_FUN\(bool\)$',      # the sleep observer
    r'^azl::ctr::Plugin::EarlyInit\(\)::\{lambda\(\)#2\}::_FUN\(\)$',               # the HOME observer
    r'^std::_Function_handler<void \(char const\*\), azl::ctr::Plugin::EarlyInit\(\)::\{lambda\(char const\*\)#1\}>::_M_invoke',
    r'^std::_Function_handler<void \(azl::WifiPacket&\), azl::ctr::Plugin::EarlyInit\(\)::\{lambda\(azl::WifiPacket&\)#1\}>::_M_invoke',
    r'^std::_Function_handler<std::array<unsigned char, 6u> \(\), azl::ctr::Plugin::EarlyInit\(\)::\{lambda\(\)#1\}>::_M_invoke',
    r'^azl::ctr::CtrKernelOps::(?!~)',
    r'^azl::LogFiles::Append\(char const\*\)$',
    # The Download Play hub (DESIGN-FASE3): what a dlp:FKCL / dlp:SRVR call runs is past the cut (UdsHle::Run > RunDlp), so the hub's game-thread
    # functions are roots of their own, and the LobbyNet methods a game thread can reach through the lobby's `net_->` calls - which are calls
    # through a vtable that a listing of direct calls cannot follow - are roots too: the clock of the inline commands, and what the give-back
    # of the engine on the calling thread (DlpHub::RunHere, from OnGameUdsStarting) asks of the adapter. (The other methods of the adapter are
    # the network thread's; Tests/dlp_chain_report.py proves from the call graph of the sources which ones a game thread reaches.)
    r'^azl::DlpHub::(?:Serve|PollPending|OnGameUdsStarting|OnSessionClosed)\(',
    r'^azl::EngineLobbyNet::(?:NowMs|NowTicks|HostClose|Disconnect|ScanClose)\(',
    # The chat's OSD callback (Sources/ctr/chat_overlay.cpp, OSD::Run): the framework calls it on the game's thread that presents a frame
    r'^azl::ctr::\(anonymous namespace\)::OnChatOsd\(CTRPluginFramework::Screen const&\)$',
]

# The roots that run on a game thread at every frame, with the ceiling of their deepest chain of frames (bytes of the game's stack below the
# call). OnChatOsd, measured 2026-11-05: 392 = OnChatOsd 48 > DrawOverlayTop 32 > DrawChatLines 112 > DrawBlock 136 > RunColumnBoth 64
# (> PutPixel 0); the ceiling is that and about 20 % more: a growth past it is a decision, written here. 2026-11-08 (the notifications in
# the corner opposite the chat, three lines of the whole width): 448 = OnChatOsd 56 > DrawOverlayTop 56 > DrawChatLines 120 > DrawBlock 152 >
# RunColumnBoth 64, under the ceiling.
OSD_CHAIN_CEILING = 480
FRAME_ROOTS = [
    (r'^azl::ctr::\(anonymous namespace\)::OnChatOsd\(CTRPluginFramework::Screen const&\)$', OSD_CHAIN_CEILING),
]

# ... that the compiler may fold into their caller (then they are not in the listing and nothing is lost)
OPTIONAL_CALLBACK_ROOTS = [
    r'^azl::ctr::Plugin::OnWake\(\)$',
]

# Not followed: the HLE proper
CUT = [r'^azl::ctr::UdsHle::Run\(unsigned char, int, unsigned (?:int|long), unsigned (?:int|long)\*\)$']

# ---- The waits (the persistent gate, analysis/fix-gancio-swap-old-2026-11-05, step 6) ------------------------------------------------
# On an Old 3DS the swap of the plugin's block (HOME, power, an applet) closes the svc hook's gate and waits 1.5 s for the game's threads
# counted in it, then the block goes: a game's thread still in the plugin's code then wakes up in memory that is gone. So every wait a game
# thread can make between pre and post - in pre, in the HLE (here NOT cut at UdsHle::Run: the engine and the hub are followed too) and in
# post - must end when the gate closes (GateClosing) or be shorter than 20 ms, and a real svc 0x32 / 0x23 of the game is the gate's to make
# (Sources/ctr/gate.s), not the plugin's. Every function the roots reach that calls one of these is in WAITS_ALLOWED, with what it may call
# and why; a new caller, or a listed one that calls another of them, fails, and so does an entry that matches nothing any more (the list
# must not rot). The arguments of a call (how long) are not in the call graph: the texts that decide them are pinned by Tests/policy_check.sh
# (section "The swap of the plugin's memory on an Old 3DS", 5) and the waits that end at the close are run by Tests/chain_sim and the PC tests.
WAIT_PRIMITIVES = ('svcSleepThread', 'svcWaitSynchronization', 'svcWaitSynchronizationN', 'LightLock_Lock', 'RecursiveLock_Lock', 'LightEvent_Wait',
                   'LightSemaphore_Acquire', 'syncArbitrateAddress', 'syncArbitrateAddressWithTimeout', 'svcArbitrateAddress',
                   '_ZN3azl8platform7SleepMsEm')
GAME_SVCS = ('svcSendSyncRequest', 'svcCloseHandle')
SHORT_LOCK = ('held by its other users (the network thread, the writer, the plugin\'s main thread) for the length of a copy or a queue '
              'operation; the gate closes before the plugin\'s own threads stop (CloseGate before CloseSwapGate), so the holder runs to the unlock')
WAITS_ALLOWED = [
    # (demangled-name pattern of the caller, what it may call, why)
    (r'^azl::ctr::\(anonymous namespace\)::WaitForJoin\(', ('svcSleepThread',),
     'the join of StartFakeSession: 10 ms slices (kDlpJoinSleepNs), GateClosing() at every slice ends it at once'),
    (r'^azl::ctr::UdsHle::ThrottleFailing\(', ('svcSleepThread',), '0.5 ms (kFailPauseNs), and not taken at all while the gate closes'),
    (r'^azl::ctr::NoteGameFailure\(', ('svcSleepThread',), 'a fatal error only (err:f), 50 ms for the trail, not taken while the gate closes'),
    (r'^azl::ctr::Plugin::WaitForCloseTrail\(', ('svcSleepThread',),
     'the close command (12) only, with the log on and on a New 3DS only: 2 ms slices up to 200 ms (kCloseTrailWaitMs) for the trail, GateClosing() at each'),
    (r'^azl::ctr::\(anonymous namespace\)::CtrOps::Settle\(', ('svcSleepThread',),
     'the input mirror\'s way back: 0.1 ms (kSettleNs; a game thread spins instead)'),
    (r'^azl::ctr::CtrKernelOps::SleepNs\(', ('svcSleepThread',), 'the engine\'s scan: 20 ms slices with GateClosing() at each (kScanSliceMs)'),
    (r'^azl::ctr::CtrKernelOps::WaitEvent\(', ('svcWaitSynchronization',),
     'the engine\'s connect (WaitEventGuarded, 10 ms slices with GateClosing() at each), the scan\'s cancel (0 ms), the hub\'s queue '
     '(100 ms slices, woken by DlpHub::WakeForSwap at CloseGate\'s first look, GateClosing() at each)'),
    (r'^azl::ctr::CtrKernelOps::CloseEvent\(', ('svcCloseHandle',), 'an event of the engine\'s or the hub\'s own, not a session of the game; it does not wait'),
    (r'^azl::DlpHub::Post\(', ('_ZN3azl8platform7SleepMsEm',), 'the slot behind another requester: try_lock every 1 ms, GateClosing() at each'),
    (r'^azl::DlpHub::(?:PollPending|RunHere|Serve)\(', ('LightLock_Lock',), 'the hub\'s lock, ' + SHORT_LOCK),
    (r'^azl::ctr::UdsHle::(?:ForgetFakeHandle|MarkInitialized|RunDlpProbe|RunNdm)\(', ('LightLock_Lock',), 'the table of the fake sessions, ' + SHORT_LOCK),
    (r'^azl::ctr::UdsHle::InterceptGetServiceHandle\(', ('LightLock_Lock', 'svcCloseHandle'),
     'the table of the fake sessions, ' + SHORT_LOCK + '; the close is of the fake event it made when the table is full, not a session of the game'),
    (r'^azl::platform::ContendedMutex::lock\(', ('LightLock_Lock',), 'the engine\'s lock (UdsEngine::Guard), ' + SHORT_LOCK),
    (r'^azl::RoomClient::(?:SendWifiPacket|mac)\(', ('LightLock_Lock',), 'the room client\'s queue, ' + SHORT_LOCK),
    (r'^azl::ctr::Plugin::OnSleepEnter\(', ('LightLock_Lock',), 'the sleep notification (APT 5), not the swap: the locks of the link policy, ' + SHORT_LOCK),
    (r'^azl::\(anonymous namespace\)::LockLogBriefly\(', ('LightLock_Lock',), 'only on the log\'s writer thread: a game thread tries 256 times and gives the line up'),
    (r'^azl::LogFiles::Append\(', ('LightLock_Lock',), 'only on the log\'s writer thread: a game thread tries 256 times and gives the line up'),
    (r'^(?:abort|PLGLDR__DisplayErrMessage)$', ('svcSleepThread', 'svcSendSyncRequest'),
     'the end of the process (std::terminate behind an exception that cannot be thrown): it never comes back to the gate'),
    (r'^(?:__syscall_lock_acquire|__syscall_lock_acquire_recursive|__ctrpf_getThreadVars)$', ('LightLock_Lock', 'RecursiveLock_Lock'),
     'newlib\'s own locks (the allocator behind operator new, stdio\'s first use behind vsnprintf), ' + SHORT_LOCK),
]

# What must not be reachable: the printf family of newlib, and the plugin's functions that take a format
SINK_LIBC = re.compile(r'^(?:_?v?s?n?i?printf(?:_r)?|_?svf?i?printf_r|_?vf?i?printf_r|__sbprintf|v?f?i?printf|_?s?s?v?f?i?scanf(?:_r)?|sscanf|siscanf)$')
SINK_PLUGIN = re.compile(r'^(?:_ZN3azl3LogEPKcz|_ZN3azl10DebugEventEPKcz|azl_c_log)$')

HDR = re.compile(r'^([0-9a-f]{8}) <(.+)>:$')
INS = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
TARGET = re.compile(r'^([0-9a-f]+) <([^>+]+)(?:\+0x[0-9a-f]+)?>')
BRANCH = re.compile(r'^(?:bl|b)(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?$')
PLAIN_COND = ('ble', 'blt', 'bls', 'blo')   # `b` with a condition, not `bl`


class Function:
    def __init__(self, name, addr):
        self.name = name
        self.addr = addr
        self.calls = collections.OrderedDict()   # name -> 'call' | 'ptr'
        self.indirect = 0                        # calls through a register or a table: not followed
        self.pool = []                           # words of the literal pool(s)
        self.frame = 0                           # bytes of its prologue: push / stmdb sp! / vpush / sub sp, sp, #n / str rX, [sp, #-4]!
        self.linked = set()                      # callees reached by a `bl` (the callee's frame sits on this one's)
        self.reg_calls = 0                       # blx rN / bx rN: calls (or tail calls) through a register (not a switch's jump table)


def prologue_bytes(op, rest):
    if op == 'push' or (op == 'stmdb' and rest.startswith('sp!')):
        return 4 * len([r for r in rest[rest.index('{') + 1:rest.index('}')].split(',') if r.strip()])
    if op.startswith('vpush'):
        regs = rest[rest.index('{') + 1:rest.index('}')].strip()
        if '-' in regs and ',' not in regs:
            a, b = regs.split('-')
            n = int(re.sub(r'\D', '', b)) - int(re.sub(r'\D', '', a)) + 1
        else:
            n = len(regs.split(','))
        return n * (8 if regs.startswith('d') else 4)
    m = re.match(r'^sp,\s*sp,\s*#(\d+)', rest)
    if op == 'sub' and m:
        return int(m.group(1))
    if op == 'str' and re.match(r'^\w+,\s*\[sp,\s*#-4\]!', rest):
        return 4
    return 0


def parse_listing(lines):
    funcs = collections.OrderedDict()
    by_addr = {}
    cur = None
    idx, called = 0, False
    for line in lines:
        line = line.rstrip('\n')
        m = HDR.match(line)
        if m:
            cur = Function(m.group(2), int(m.group(1), 16))
            funcs[cur.name] = cur
            by_addr.setdefault(cur.addr, cur.name)
            idx, called = 0, False
            continue
        if cur is None:
            continue
        m = INS.match(line)
        if not m:
            continue
        op, rest = m.group(2), m.group(3)
        rest = rest.split('@')[0].strip()
        idx += 1
        if BRANCH.match(op):
            t = TARGET.match(rest)
            links = op.startswith('bl') and op not in PLAIN_COND
            if t and t.group(2) != cur.name:
                cur.calls.setdefault(t.group(2), 'call')
                if links:
                    cur.linked.add(t.group(2))
            called = called or links
        elif op == 'blx' or (op.startswith('bx') and rest != 'lr'):
            t = TARGET.match(rest)
            if t:
                if t.group(2) != cur.name:
                    cur.calls.setdefault(t.group(2), 'call')
                    cur.linked.add(t.group(2))
            else:
                cur.indirect += 1
                cur.reg_calls += 1
            called = True
        elif op in ('ldr', 'mov', 'add') and rest.startswith('pc,'):
            cur.indirect += 1
        elif op == '.word':
            try:
                cur.pool.append(int(rest.split()[0], 0))
            except ValueError:
                pass
        elif idx <= 14 and not called:
            cur.frame += prologue_bytes(op, rest)
    # the address of a function kept in a literal pool is a call that goes through a register (or a pointer that is
    # stored for later): an edge too, so that a function whose address is only passed around is not missed
    for f in funcs.values():
        for w in f.pool:
            target = by_addr.get(w)
            if target and target != f.name:
                f.calls.setdefault(target, 'ptr')
    return funcs


def demangled_names(nm_lines, nmc_lines):
    dem = {}
    for l, lc in zip(nm_lines, nmc_lines):
        p = l.split()
        pc = lc.split(None, 2)
        if len(p) >= 3 and len(pc) >= 3:
            dem[p[2]] = pc[2].strip()
    return dem


def analyse(funcs, dem, quiet=False):
    """Returns (violations, reached, cut_reached, indirect_in_reached)."""
    roots = [r for r in ENTRY_ROOTS if r in funcs]
    missing = [r for r in ENTRY_ROOTS if r not in funcs]
    callback_re = [re.compile(p) for p in CALLBACK_ROOTS + OPTIONAL_CALLBACK_ROOTS]
    cut_re = [re.compile(p) for p in CUT]
    callbacks = [n for n in funcs if any(r.search(dem.get(n, n)) for r in callback_re)]
    cuts = {n for n in funcs if any(r.search(dem.get(n, n)) for r in cut_re)}
    problems = []
    if missing:
        problems.append('no entry named %s in the listing' % ', '.join(missing))
    for pat in CALLBACK_ROOTS:
        if not any(re.search(pat, dem.get(n, n)) for n in funcs):
            problems.append('no function matches the callback root %s (renamed? update CALLBACK_ROOTS)' % pat)
    for pat in CUT:
        if not any(re.search(pat, dem.get(n, n)) for n in funcs):
            problems.append('no function matches the cut %s (renamed? update CUT)' % pat)
    all_roots = roots + [n for n in callbacks if n not in roots]
    parent = {}
    queue = collections.deque()
    for r in all_roots:
        parent[r] = None
        queue.append(r)
    cut_reached = set()
    while queue:
        n = queue.popleft()
        if n in cuts:
            cut_reached.add(n)
            continue
        f = funcs.get(n)
        if not f:
            continue
        for c in f.calls:
            if c not in parent:
                parent[c] = n
                queue.append(c)

    def is_sink(name):
        return bool(SINK_LIBC.match(name) or SINK_PLUGIN.match(name))

    violations = []
    for n in parent:
        if is_sink(n):
            path = [n]
            while parent[path[-1]] is not None:
                path.append(parent[path[-1]])
            path = path[::-1]
            if any(is_sink(p) for p in path[:-1]):
                continue # (it is behind another formatter: the first one is the one to report)
            violations.append(path)
    # the shortest path to each distinct first sink
    violations.sort(key=len)
    indirect = [(n, funcs[n].indirect) for n in parent if n in funcs and funcs[n].indirect and n not in cuts]
    return problems, violations, parent, cut_reached, indirect


def chain_depth(funcs, root):
    """(bytes, path, trouble) of the deepest chain of frames from `root`, read from the machine code: a function costs its prologue plus the
    deepest of its callees, a tail call (a plain `b` to another function: the caller's frame is popped first) costs the callee alone. A call
    through a register or a cycle makes the chain unbounded: `trouble` names them (the bytes are then the deepest of what could be followed)."""
    memo, active, trouble = {}, set(), []

    def depth(n):
        if n in memo:
            return memo[n]
        f = funcs.get(n)
        if f is None:
            trouble.append('%s is not in the listing' % n)
            return (0, [n])
        if n in active:
            trouble.append('a cycle through %s' % n)
            return (0, [n])
        if f.reg_calls:
            trouble.append('%s calls through a register (%d)' % (n, f.reg_calls))
        active.add(n)
        best = (f.frame, [n])
        for c, kind in f.calls.items():
            if kind != 'call':
                continue   # (an address in the literal pool: a pointer that is passed or stored, not a call made here)
            below, path = depth(c)
            val = f.frame + below if c in f.linked else below
            if val > best[0]:
                best = (val, [n] + path)
        active.discard(n)
        memo[n] = best
        return best

    total, path = depth(root)
    return total, path, trouble


def check_frame_roots(funcs, dem, quiet=False):
    """The roots that run at every frame of a game thread (FRAME_ROOTS): their deepest chain against its ceiling. Returns the failures."""
    bad = []
    for pat, ceiling in FRAME_ROOTS:
        r = re.compile(pat)
        roots = [n for n in funcs if r.search(dem.get(n, n))]
        if not roots:
            bad.append('no function matches the per-frame root %s (renamed? update FRAME_ROOTS)' % pat)
        for n in roots:
            total, path, trouble = chain_depth(funcs, n)
            chain = ' > '.join('%s %d' % (short(dem, p, 48), funcs[p].frame if p in funcs else 0) for p in path)
            if trouble:
                bad.append('the chain under %s cannot be bounded: %s' % (short(dem, n), '; '.join(sorted(set(trouble)))))
            elif total > ceiling:
                bad.append("the deepest chain under %s is %d bytes of the game's stack, ceiling %d: %s" % (short(dem, n), total, ceiling, chain))
            elif not quiet:
                print('callgraph: the deepest chain under %s is %d bytes (ceiling %d): %s' % (short(dem, n, 56), total, ceiling, chain))
    return bad


def reachable_uncut(funcs, dem):
    """Every function the roots reach, the HLE proper included (no cut), not followed into the wait primitives and the game's svc wrappers.
    Returns {name: parent}."""
    callback_re = [re.compile(p) for p in CALLBACK_ROOTS + OPTIONAL_CALLBACK_ROOTS]
    roots = [r for r in ENTRY_ROOTS if r in funcs]
    roots += [n for n in funcs if n not in roots and any(r.search(dem.get(n, n)) for r in callback_re)]
    stop = set(WAIT_PRIMITIVES) | set(GAME_SVCS)
    parent = {r: None for r in roots}
    queue = collections.deque(roots)
    while queue:
        n = queue.popleft()
        if n in stop:
            continue
        f = funcs.get(n)
        if not f:
            continue
        for c in f.calls:
            if c not in parent:
                parent[c] = n
                queue.append(c)
    return parent


def check_waits(funcs, dem, quiet=False):
    """The waits and the game's svc a game thread can reach from the gate's pre/post and the callbacks, against WAITS_ALLOWED. Returns the
    number of failures (printed)."""
    parent = reachable_uncut(funcs, dem)
    stop = set(WAIT_PRIMITIVES) | set(GAME_SVCS)
    allowed = [(re.compile(p), set(calls), why) for p, calls, why in WAITS_ALLOWED]
    used = [0] * len(allowed)
    bad = 0
    callers = 0
    for n in parent:
        f = funcs.get(n)
        if f is None or n in stop:
            continue
        waits = sorted(c for c in f.calls if c in stop)
        if not waits:
            continue
        callers += 1
        name = dem.get(n, n)
        hits = [i for i, (r, _c, _w) in enumerate(allowed) if r.search(name)]
        for i in hits:
            used[i] += 1
        may = set().union(*(allowed[i][1] for i in hits)) if hits else set()
        extra = [w for w in waits if w not in may]
        if extra:
            path = [n]
            while parent[path[-1]] is not None:
                path.append(parent[path[-1]])
            print('callgraph check FAILED: a game thread can reach %s in %s, which is not in WAITS_ALLOWED (a wait must end when the gate '
                  'closes or be under 20 ms; the game\'s svc is the gate\'s):' % (', '.join(dem.get(w, w) for w in extra), short(dem, n)))
            print('    ' + '\n <- '.join(short(dem, p) for p in path))
            bad += 1
    for i, (p, _calls, _why) in enumerate(WAITS_ALLOWED):
        if not used[i]:
            print('callgraph check FAILED: the entry %s of WAITS_ALLOWED matches no caller of a wait any more (renamed? take it out)' % p)
            bad += 1
    if not quiet and not bad:
        print('callgraph: %d functions a game thread reaches call a wait or a svc of the game (the HLE followed, no cut): all in the list '
              'of the guarded or short ones (%d entries)' % (callers, len(WAITS_ALLOWED)))
    return bad


def short(dem, name, width=96):
    return dem.get(name, name)[:width]


def report(funcs, dem, quiet=False):
    problems, violations, parent, cut_reached, indirect = analyse(funcs, dem)
    for p in problems:
        print('callgraph check FAILED: ' + p)
    seen_first = set()
    bad = 0
    for path in violations:
        key = path[-2] if len(path) > 1 else path[-1]
        if key in seen_first:
            continue
        seen_first.add(key)
        bad += 1
        print('callgraph check FAILED: a game thread can reach %s:' % short(dem, path[-1]))
        print('    ' + '\n -> '.join(short(dem, p) for p in path))
    for p in check_frame_roots(funcs, dem, quiet):
        print('callgraph check FAILED: ' + p)
        bad += 1
    bad += check_waits(funcs, dem, quiet)
    if not quiet:
        print('callgraph: %d functions, %d reachable from the hook entries and its callbacks (not followed past: %s)' % (
            len(funcs), len(parent), ', '.join(sorted(short(dem, c) for c in cut_reached)) or 'nothing reached'))
        # (of the plugin's own and the C++ library's: the unwinder and libc dispatch through tables by design)
        own = [(n, k) for n, k in indirect if dem.get(n, n).startswith(('azl', 'std::', 'enet_', 'ctr'))]
        if own:
            names = sorted('%s x%d' % (short(dem, n, 56), k) for n, k in own)
            print('callgraph: calls through a register / table (not followed; the roots cover the plugin\'s callbacks), %d of '
                  'the plugin\'s functions: %s' % (len(own), '; '.join(names[:12]) + (' ...' if len(names) > 12 else '')))
    return 1 if (problems or bad) else 0


# ---------------------------------------------------------------------------------------------------------------------
SELFTEST_LISTING = """
07000000 <azl_pre32>:
 7000000:	bl	7000100 <hook>
 7000004:	bx	lr
07000010 <azl_post32>:
 7000010:	bx	lr
07000020 <azl_post23>:
 7000020:	bx	lr
07000100 <hook>:
 7000100:	bl	7000200 <ok_leaf>
 7000104:	beq	7000300 <tail_helper>
 7000108:	bx	lr
07000200 <ok_leaf>:
 7000200:	bx	lr
07000300 <tail_helper>:
 7000300:	bl	7000400 <HleRun>
 7000304:	bx	lr
07000400 <HleRun>:
 7000400:	bl	7000500 <HleFormats>
 7000404:	bx	lr
07000500 <HleFormats>:
 7000500:	bl	7000600 <vsnprintf>
 7000504:	bx	lr
07000600 <vsnprintf>:
 7000600:	bx	lr
07000700 <azl_pre23>:
 7000700:	bl	7000800 <closer>
 7000704:	bx	lr
07000800 <closer>:
 7000800:	ldr	r3, [pc, #8]	@ 7000810 <closer+0x10>
 7000804:	blx	r3
 7000808:	bx	lr
 700080c:	nop
 7000810:	.word	0x07000900
07000900 <pointer_taken>:
 7000900:	bl	7000a00 <snprintf>
 7000904:	bx	lr
07000a00 <snprintf>:
 7000a00:	bx	lr
"""
SELFTEST_NM = """07000000 T azl_pre32
07000010 T azl_post32
07000020 T azl_post23
07000100 T hook
07000200 T ok_leaf
07000300 T tail_helper
07000400 T HleRun
07000500 T HleFormats
07000600 T vsnprintf
07000700 T azl_pre23
07000800 T closer
07000900 T pointer_taken
07000a00 T snprintf
"""


SELFTEST_CHAIN = """
08000000 <osd>:
 8000000:	push	{r4, r5, r6, lr}
 8000004:	sub	sp, sp, #8
 8000008:	bl	8000100 <draw>
 800000c:	bl	8000300 <leaf>
 8000010:	pop	{r4, r5, r6, pc}
08000100 <draw>:
 8000100:	push	{r4, lr}
 8000104:	vpush	{d8-d9}
 8000108:	sub	sp, sp, #16
 800010c:	bl	8000200 <big>
 8000110:	sub	sp, sp, #64
 8000114:	pop	{r4, pc}
08000200 <big>:
 8000200:	push	{r4, lr}
 8000204:	sub	sp, sp, #1536
 8000208:	pop	{r4, pc}
08000300 <leaf>:
 8000300:	bx	lr
08000400 <tailer>:
 8000400:	push	{r4, lr}
 8000404:	pop	{r4, lr}
 8000408:	b	8000200 <big>
08000500 <byreg>:
 8000500:	push	{r4, lr}
 8000504:	blx	r3
 8000508:	pop	{r4, pc}
08000600 <loop_a>:
 8000600:	push	{r4, lr}
 8000604:	bl	8000700 <loop_b>
 8000608:	pop	{r4, pc}
08000700 <loop_b>:
 8000700:	push	{r4, lr}
 8000704:	bl	8000600 <loop_a>
 8000708:	pop	{r4, pc}
"""


SELFTEST_WAITS = """
09000000 <azl_pre32>:
 9000000:	bl	9000100 <guarded_wait>
 9000004:	b	9000200 <HleRun>
09000010 <azl_post32>:
 9000010:	bx	lr
09000020 <azl_post23>:
 9000020:	bx	lr
09000030 <azl_pre23>:
 9000030:	bl	9000500 <svcCloseHandle>
 9000034:	bx	lr
09000100 <guarded_wait>:
 9000100:	bl	9000400 <svcSleepThread>
 9000104:	bx	lr
09000200 <HleRun>:
 9000200:	bl	9000300 <engine_lock>
 9000204:	bx	lr
09000300 <engine_lock>:
 9000300:	bl	9000600 <LightLock_Lock>
 9000304:	bx	lr
09000400 <svcSleepThread>:
 9000400:	svc	0x0000000a
 9000404:	bx	lr
09000500 <svcCloseHandle>:
 9000500:	svc	0x00000023
 9000504:	bx	lr
09000600 <LightLock_Lock>:
 9000600:	bl	9000400 <svcSleepThread>
 9000604:	bx	lr
"""


def selftest():
    global CALLBACK_ROOTS, CUT, FRAME_ROOTS, WAITS_ALLOWED
    saved = (CALLBACK_ROOTS, CUT, FRAME_ROOTS, WAITS_ALLOWED)
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)
            print('  FAIL: ' + what)

    try:
        CALLBACK_ROOTS = []
        CUT = [r'^HleRun$']
        funcs = parse_listing(SELFTEST_LISTING.split('\n'))
        dem = {n: n for n in funcs}
        problems, violations, parent, cut_reached, indirect = analyse(funcs, dem)
        expect(not problems, 'the made-up listing is understood (%r)' % problems)
        expect('ok_leaf' in parent and 'tail_helper' in parent, 'a call and a conditional tail call are followed')
        expect('HleRun' in cut_reached and 'HleFormats' not in parent,
               'the cut function is reached and not followed (the formatter behind it is the HLE proper)')
        expect(not any(p[-1] == 'vsnprintf' for p in violations), 'vsnprintf behind the cut is not a violation')
        expect(any(p[-1] == 'snprintf' for p in violations),
               'a function whose address sits in a literal pool is reached: its snprintf is a violation')
        expect(any(n == 'closer' for n, k in indirect), 'a call through a register is counted')
        # without the cut the formatter behind it is found, with the path
        CUT = []
        problems2, violations2, parent2, _, _ = analyse(funcs, dem)
        paths = [p for p in violations2 if p[-1] == 'vsnprintf']
        expect(paths and paths[0] == ['azl_pre32', 'hook', 'tail_helper', 'HleRun', 'HleFormats', 'vsnprintf'],
               'the path to a formatter is reported (%r)' % (paths,))
        # a function that is a root is followed from there even though the cut hides it from the entries (the Download Play hub's functions are
        # roots for that reason: UdsHle::Run is the cut and everything a dlp command does is behind it)
        CALLBACK_ROOTS = [r'^HleFormats$']
        CUT = [r'^HleRun$']
        _, violations4, _, _, _ = analyse(funcs, dem)
        expect(any(p[0] == 'HleFormats' and p[-1] == 'vsnprintf' for p in violations4),
               'a root behind the cut is followed from its own start: its vsnprintf is a violation (%r)' % (violations4,))
        # a renamed root is an error, not a silent pass
        CALLBACK_ROOTS = [r'^NoSuchCallback$']
        problems3, _, _, _, _ = analyse(funcs, dem)
        expect(any('NoSuchCallback' in p for p in problems3), 'a callback root that matches nothing is reported')
        # the deepest chain of a per-frame root: prologues added up, a tail call not stacked, a buffer in a helper over the ceiling
        osd = parse_listing(SELFTEST_CHAIN.split('\n'))
        odem = {n: n for n in osd}
        expect(osd['osd'].frame == 24 and osd['draw'].frame == 40 and osd['big'].frame == 1544,
               'the prologues are read (push, vpush, sub sp; not past the first call) (%r)' % ({n: f.frame for n, f in osd.items()},))
        total, path, trouble = chain_depth(osd, 'osd')
        expect(not trouble and total == 24 + 40 + 1544 and path == ['osd', 'draw', 'big'],
               'the deepest chain is the sum of its frames (%r %r %r)' % (total, path, trouble))
        expect(chain_depth(osd, 'tailer')[0] == 1544, 'a tail call does not stack on its caller (%r)' % (chain_depth(osd, 'tailer'),))
        expect(any('register' in t for t in chain_depth(osd, 'byreg')[2]), 'a call through a register leaves the chain unbounded')
        expect(any('cycle' in t for t in chain_depth(osd, 'loop_a')[2]), 'a cycle leaves the chain unbounded')
        FRAME_ROOTS = [(r'^osd$', 1700)]
        expect(not check_frame_roots(osd, odem, quiet=True), 'a chain within its ceiling passes')
        FRAME_ROOTS = [(r'^osd$', 1600)]
        expect(any('1608 bytes' in p for p in check_frame_roots(osd, odem, quiet=True)), 'a chain over its ceiling fails')
        FRAME_ROOTS = [(r'^byreg$', 1600)]
        expect(any('cannot be bounded' in p for p in check_frame_roots(osd, odem, quiet=True)), 'an unbounded chain fails')
        FRAME_ROOTS = [(r'^NoSuchFrameRoot$', 100)]
        expect(any('NoSuchFrameRoot' in p for p in check_frame_roots(osd, odem, quiet=True)), 'a per-frame root that matches nothing fails')
        # the waits: followed past the cut, every caller of a wait or of the game's svc in the list, nothing more, no entry left over
        import contextlib
        import io
        wl = parse_listing(SELFTEST_WAITS.split('\n'))
        wdem = {n: n for n in wl}
        CALLBACK_ROOTS = []
        CUT = [r'^HleRun$']
        everything = [(r'^guarded_wait$', ('svcSleepThread',), 'x'), (r'^engine_lock$', ('LightLock_Lock',), 'x'),
                      (r'^azl_pre23$', ('svcCloseHandle',), 'x')]

        def waits(allowed):
            global WAITS_ALLOWED
            WAITS_ALLOWED = allowed
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                n = check_waits(wl, wdem, quiet=True)
            return n, out.getvalue()

        n, _ = waits(everything)
        expect(n == 0, 'every wait in the list: no failure (%d)' % n)
        n, out = waits(everything[:1] + everything[2:])
        expect(n == 1 and 'LightLock_Lock in engine_lock' in out and 'HleRun' in out,
               'a lock behind the cut of the formatters (the HLE proper) is followed and fails when it is not in the list (%r)' % out)
        n, out = waits(everything[:2])
        expect(n == 1 and 'svcCloseHandle in azl_pre23' in out, 'a svc of the game made by the plugin fails when it is not in the list (%r)' % out)
        n, out = waits([(r'^guarded_wait$', ('LightLock_Lock',), 'x')] + everything[1:])
        expect(n == 1 and 'svcSleepThread in guarded_wait' in out, 'a listed caller that makes another wait than its own fails (%r)' % out)
        n, out = waits(everything + [(r'^NoSuchWaiter$', ('svcSleepThread',), 'x')])
        expect(n == 1 and 'NoSuchWaiter' in out, 'an entry that matches no caller any more fails (%r)' % out)
        expect('LightLock_Lock' in reachable_uncut(wl, wdem) and 'svcSleepThread' in reachable_uncut(wl, wdem),
               'the primitives are reached, and not followed (LightLock_Lock calls svcSleepThread: not its own failure)')
    finally:
        CALLBACK_ROOTS, CUT, FRAME_ROOTS, WAITS_ALLOWED = saved
    print('callgraph selftest: %s' % ('%d failure(s)' % len(failures) if failures else 'ok'))
    return 1 if failures else 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == '--selftest':
        return selftest()
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    prefix = sys.argv[1]
    funcs = parse_listing(open(prefix + '.dis', errors='replace'))
    dem = demangled_names(open(prefix + '.nm', errors='replace').read().split('\n'),
                          open(prefix + '.nmc', errors='replace').read().split('\n'))
    rc = report(funcs, dem)
    if rc == 0:
        print('callgraph: no formatter (printf family, Log(), DebugEvent()) can be reached from a game thread outside the HLE proper')
    return rc


if __name__ == '__main__':
    sys.exit(main())
