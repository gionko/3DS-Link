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

What is a root: the two assembly entries of the hook (azl_svc32_entry: SendSyncRequest, azl_svc23_entry: CloseHandle) and the
functions the hook calls through a pointer or a vtable, which a listing of direct calls cannot follow: the observers of the
game's APT session, the sink of the log, the engine's transport and the kernel operations of the engine; and, for the games that have
the Download Play hub, the hub's functions a game thread runs past the cut and the adapter methods it reaches through the lobby's vtable.

What is cut: UdsHle::Run. Past it is the HLE proper - the dispatch of a fake session's command, the engine, NdmHle - which
keeps its Log() formats, legitimately needs more stack than a small thread has, and runs on the game's UDS thread (Tests/chain_sim
measures it; moving it to a stack of the plugin's own is the 0.1.16 item). The set of what is reachable but not followed is printed.

What it cannot see: a call through a pointer that is not in the roots (the list below is the plugin's own: add a new
callback there), and a library function that formats without calling the printf family (none is known).
"""
import collections
import re
import sys

ENTRY_ROOTS = ['azl_svc32_entry', 'azl_svc23_entry']

# Demangled-name patterns of the callbacks the hook reaches through a pointer / a vtable / a std::function
CALLBACK_ROOTS = [
    r'^azl::ctr::Plugin::OnSleepEnter\(\)$',
    r'^azl::ctr::Plugin::OnHomeButton\(\)$',
    r'^azl::ctr::Plugin::NoteSleepEvent\(unsigned (?:int|long), unsigned (?:int|long)\)$',   # the APT observer (the trail of a sleep)
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
]

# ... that the compiler may fold into their caller (then they are not in the listing and nothing is lost)
OPTIONAL_CALLBACK_ROOTS = [
    r'^azl::ctr::Plugin::OnWake\(\)$',
]

# Not followed: the HLE proper
CUT = [r'^azl::ctr::UdsHle::Run\(unsigned char, int, unsigned (?:int|long), unsigned (?:int|long)\*\)$']

# What must not be reachable: the printf family of newlib, and the plugin's functions that take a format
SINK_LIBC = re.compile(r'^(?:_?v?s?n?i?printf(?:_r)?|_?svf?i?printf_r|_?vf?i?printf_r|__sbprintf|v?f?i?printf|_?s?s?v?f?i?scanf(?:_r)?|sscanf|siscanf)$')
SINK_PLUGIN = re.compile(r'^(?:_ZN3azl3LogEPKcz|_ZN3azl10DebugEventEPKcz|azl_c_log)$')

HDR = re.compile(r'^([0-9a-f]{8}) <(.+)>:$')
INS = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
TARGET = re.compile(r'^([0-9a-f]+) <([^>+]+)(?:\+0x[0-9a-f]+)?>')
BRANCH = re.compile(r'^(?:bl|b)(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?$')


class Function:
    def __init__(self, name, addr):
        self.name = name
        self.addr = addr
        self.calls = collections.OrderedDict()   # name -> 'call' | 'ptr'
        self.indirect = 0                        # calls through a register or a table: not followed
        self.pool = []                           # words of the literal pool(s)


def parse_listing(lines):
    funcs = collections.OrderedDict()
    by_addr = {}
    cur = None
    for line in lines:
        line = line.rstrip('\n')
        m = HDR.match(line)
        if m:
            cur = Function(m.group(2), int(m.group(1), 16))
            funcs[cur.name] = cur
            by_addr.setdefault(cur.addr, cur.name)
            continue
        if cur is None:
            continue
        m = INS.match(line)
        if not m:
            continue
        op, rest = m.group(2), m.group(3)
        rest = rest.split('@')[0].strip()
        if BRANCH.match(op):
            t = TARGET.match(rest)
            if t and t.group(2) != cur.name:
                cur.calls.setdefault(t.group(2), 'call')
        elif op == 'blx' or (op.startswith('bx') and rest != 'lr'):
            t = TARGET.match(rest)
            if t:
                if t.group(2) != cur.name:
                    cur.calls.setdefault(t.group(2), 'call')
            else:
                cur.indirect += 1
        elif op in ('ldr', 'mov', 'add') and rest.startswith('pc,'):
            cur.indirect += 1
        elif op == '.word':
            try:
                cur.pool.append(int(rest.split()[0], 0))
            except ValueError:
                pass
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
07000000 <azl_svc32_entry>:
 7000000:	bl	7000100 <hook>
 7000004:	bx	lr
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
07000700 <azl_svc23_entry>:
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
SELFTEST_NM = """07000000 T azl_svc32_entry
07000100 T hook
07000200 T ok_leaf
07000300 T tail_helper
07000400 T HleRun
07000500 T HleFormats
07000600 T vsnprintf
07000700 T azl_svc23_entry
07000800 T closer
07000900 T pointer_taken
07000a00 T snprintf
"""


def selftest():
    global CALLBACK_ROOTS, CUT
    saved = (CALLBACK_ROOTS, CUT)
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
        expect(paths and paths[0] == ['azl_svc32_entry', 'hook', 'tail_helper', 'HleRun', 'HleFormats', 'vsnprintf'],
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
    finally:
        CALLBACK_ROOTS, CUT = saved
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
