#!/usr/bin/env python3
"""The deepest CHAIN OF STACK FRAMES of the fake Download Play lobby on the game's threads, measured on the plugin's target.

    python3 Tests/dlp_chain_report.py <dir with the .ci files> --src <tree> [--dis plugin.dis] [--json FILE] [--kind NAME]
                                       [--path-map FROM=TO] [--repo DIR] [--gate]
    python3 Tests/dlp_chain_report.py --selftest

Two users: analysis/dlp-lobby-fake-2026-10-28/misura-stack/dlp_chain_report.sh (compiles every unit of the plugin with
-fstack-usage -fcallgraph-info=su in a container and calls this: the MEASUREMENT, with a report of every chain), and
Tests/stack_usage.sh (builds the plugin once with the same two flags, disassembles the ELF and calls this with --gate: the GATE).

WHY. Stack is the plugin's scarcest resource: the hook runs the plugin's C++ on the stack of whatever game thread made the IPC call,
and some of those threads have 464 bytes below the call. Tests/stack_usage.sh budgets ONE frame at a time and Tests/chain_sim runs
the real code in an emulator, but only the scenarios it can build; the chain that matters goes on through the whole call graph. This
tool is that graph: for every ENTRY a game thread can make into the Download Play hub (DESIGN-FASE3.md sez. 1.1: a command served
inline, a command posted to the network thread, the join wait, OnGameUdsStarting, OnSessionClosed), the deepest chain of frames.

HOW. arm-none-eabi-g++ -fstack-usage -fcallgraph-info=su gives, for every unit, the static frame of each function and its call
edges with the file:line:col of every call site. The plugin has no LTO, so the units are joined here the way the linker joins
them: by symbol name (a global function is a node named by its mangled name, a static one by "file:name"; a function defined in
several units, an inline one, keeps its LARGEST frame and the union of its edges). The depth of a function is its own frame plus
the deepest of its callees; a chain is that sum along the worst path (the frames already include the saved registers and lr).

  * INDIRECT CALLS are resolved, never skipped; the ones that cannot be are listed with their source line and an assumption:
      net_->X(...)            DlpLobby's LobbyNet: X of EngineLobbyNet            (the adapter is the only implementation)
      kernel_->X(...)         the engine's / the hub's KernelOps: X of azl::ctr::CtrKernelOps (the only implementation in the plugin)
      r.map(...)              the IpcResolver of the lobby: plugin.cpp's MapGameAddress (the only one in the plugin)
      std::function calls     the compiler inlines operator() and leaves "_M_invoker(...)": STD_FUNCTION_CALLERS says which
                              signature the caller calls and every _Function_handler<SIGNATURE, F>::_M_invoke of the program is
                              a target (the transport lambda, the log sink, ...)
  * calls into libc / libstdc++ / libctru have no .su: their frame and calls are read from the machine code of the linked plugin
    (--dis, objdump -d of AzaharLink.elf: prologue push / sub sp, bl and tail branches). An ESTIMATE, marked "~"; without --dis
    such a callee counts 0 and is listed as UNKNOWN.
  * debug level: at debug 0 a Log() / DebugEvent() / LogLine / DebugLine / Debug*() call returns after its own prologue (the
    level test is its first act), so it costs its frame and nothing below it. At debug 1 it costs everything below it: the
    formatter and the sink. Nothing else differs between the levels.
  * EXCLUDED, and said so in the report: the fallback of FormatLine to newlib's vsnprintf (a format printf_lite does not write:
    none of the plugin's, Tests/log_files_test.cpp TestSourceFormats), and the noreturn __throw_* calls (an allocation failure).

THE ENTRIES AND WHAT IS STATIC, WHAT IS DYNAMIC. A call graph cannot know which branch of a function runs. Each row of the report
is a chain with CUTS, and every cut is a statement that something else proves:
  * "inline" rows (the commands a game thread serves itself, DlpHub::IsInline): the edge Serve > Post is cut (a different row), the
    handlers of the commands that are NOT inline are cut (they run on the network thread), and the edges of DlpLobby::Poll to
    PollClient / PollServer are cut: DYNAMIC, Poll returns at once when the lobby is in the external-poll mode and the caller is
    not the network thread (the first line of Poll, dlp_lobby.hpp; proved by Tests/dlp_hub_test.cpp with a spy LobbyNet and by
    Tests/chain_sim on the compiled code). EVERYTHING ELSE is STATIC: the proof section lists what the cut graph still reaches and
    FAILS if that is a LobbyNet method other than NowMs / NowTicks, a function of the engine, or a Log() / DebugEvent() with a format.
  * "posted" rows: the handlers are cut (the network thread runs them), the chain is the wait for the network thread.
  * the OnGameUdsStarting rows come in three flavours, because DlpHub runs it on the CALLING thread when no network thread is alive
    (RunHere): the normal one (the request is posted), the dead-network one with a room joined (the whole LobbyNet side down to the
    transport and the room client: the worst a stalled network thread can make a game thread pay) and the dead-network one with no
    room (the transport lambda drops the frame: `if (!room_->joined()) return;`). Which flavour runs is DYNAMIC (last Tick older than
    kAliveMs; the room's state). That thread is the game's nwm::UDS thread, which pays far more for its own engine calls (the
    dead-network rows are not gated; Tests/chain_sim holds them to UDS_ENGINE_CALL). The end of a session (OnSessionClosed) has no
    such flavour: it posts a request or leaves a bit for the next Tick, and proof 3 holds the graph to that.

THE BUDGET (Tests/chain_sim/chain_sim.py, read from the file when --repo is given): every number there is "bytes below the SP at
azl_svc32_entry". A thread of 464 bytes has 20 of them above that point (the game's wrapper frame 16, the trampoline's push 4);
azl_svc32_entry costs up to 36 (push {r1-r4, r12, lr} 24, push {r4, r12} 8, alignment up to 4), then the hook's frames. The HLE
proper (what UdsHle::Run reaches) is held to HLE_CEILING (320 at debug 0, 520 at debug 1), a path that writes a log line to
BUDGET_LOG (340). In the real build the entry of a dlp command is azl_svc32_entry > azl_hook_SendSyncRequest >
HandleSendSyncRequest > [RunTraced at debug 1] > Run > RunDlp, then the hub.
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys

sys.setrecursionlimit(50000)

# ======================================================================================================================
# 0. What the tool knows about the plugin (everything plugin-specific is in this section)
# ======================================================================================================================
HLE_CEILING = {0: 320, 1: 520}   # Tests/chain_sim/chain_sim.py: the HLE proper, bytes below the SP at azl_svc32_entry
BUDGET_LOG = 340                  # ... the paths that write a log line (the close path of a fake session)
THREAD_FREE = 464                 # the smallest game thread seen (Tri Force Heroes' srv thread): bytes below its call
ABOVE_ENTRY = 20                  # of those 464: the game's wrapper frame (16) and the trampoline's push (4), above the entry SP
BUDGET_THREAD = THREAD_FREE - ABOVE_ENTRY   # 444
ASM_ENTRY = 36                    # azl_svc32_entry / azl_svc23_entry: 24 + 8 + up to 4 of alignment (Sources/ctr/svc_entry.s)
ASSUMED_RESOLVER_MAP = 76         # the game's address mapper when it cannot be resolved to MapGameAddress (DESIGN-FASE3 sez. 5.3: 76)
CLOCK_METHODS = ('NowMs', 'NowTicks')   # the only LobbyNet methods a game thread may call

# Who makes the std::function call that the compiler inlined (the call edge says only "std_function.h:581 _M_invoker"):
# a regex on the demangled name of the CALLER -> the signature of the std::function it calls, as the demangler prints it.
STD_FUNCTION_CALLERS = [
    (r'^azl::UdsEngine::SendPacket\(', 'void (azl::WifiPacket&)'),                 # transport_.send
    (r'^azl::UdsEngine::GetMacAddress\(', 'std::array<unsigned char, 6u> ()'),     # transport_.get_mac
    (r'^azl::LogV\(|^azl::DebugEventV\(|^azl::LogLine::|^azl::DebugLine::', 'void (char const*)'),   # g_sink
]
# The implementations of the abstract classes whose virtual methods are called through a member:
VIRTUAL_RECEIVERS = {
    'net_': 'azl::EngineLobbyNet::',        # DlpLobby -> LobbyNet
    'kernel_': 'azl::ctr::CtrKernelOps::',  # UdsEngine / DlpHub -> KernelOps
}
# Function pointers: the identifier before the '(' of the call -> the demangled name of the only function that is ever stored there
FUNCTION_POINTERS = {
    'map': r'^azl::ctr::\(anonymous namespace\)::MapGameAddress\(',     # IpcResolver::map (Sources/ctr/plugin.cpp)
}
# Two virtual methods with the same body are one function after -fipa-icf (only one has a .su line): X is served by Y
FOLDED = {'Disconnect': 'HostClose'}
# Functions whose first act is the test of the debug level (Log, DebugEvent, LogLine, DebugLine, the capture's producers)
GATED = re.compile(r'^azl::(?:Log|LogV|DebugEvent|DebugEventV|Debug[A-Z]\w*)\(|^azl::(?:LogLine|DebugLine)::')
FORMATTED_LOG = re.compile(r'^azl::(?:Log|DebugEvent)\(char const\*, \.\.\.\)')   # Log() / DebugEvent() with a format
PIECEWISE_LOG = re.compile(r'^azl::(?:LogLine|DebugLine)::(?:LogLine|DebugLine)\(')  # LogLine / DebugLine (no formatter)
# Edges the chains do not follow (caller regex, callee symbol regex, why)
EXCLUDED = [
    (r'^azl::FormatLine\(', r'^vsnprintf$', 'newlib fallback for a format printf_lite does not write (none of the plugin\'s)'),
    (r'.', r'^_ZSt\d+__throw_', 'noreturn error path (allocation failure, bad function call)'),
]
# What the walk through the machine code of the libraries does not follow: the exception machinery that operator new reaches when
# malloc fails, abort / assert, newlib's printf (the plugin does not call it: see EXCLUDED), the unwinder. The SUCCESS path only.
EXCLUDED_ELF = re.compile(r'^(?:__cxa_|_Unwind_|__gnu_Unwind|unwind_|get_eit_entry|search_EIT_table|__assert|abort$|raise$|_Exit$|_exit$|'
                          r'__sbprintf|__sfvwrite_r|_svf|_vf|fiprintf|iprintf|printf$|_dtoa|__pow5|__multiply|_fflush|__sflush|'
                          r'_ZSt\d+__throw_|_ZSt9terminate|__gnu_cxx::__verbose|_ZN9__gnu_cxx\d+__verbose|_ZN9__gnu_cxx\d+__throw_)')
# Allocation: operator new is not followed from LogFiles::Append down (the pending buffer is reserved: no growth in the steady state)
STEADY_ENTER = r'^azl::LogFiles::Append\('
ALLOC_SYMS = ('_Znwj', '_Znaj', '_ZnwjRKSt9nothrow_t', '_ZnajRKSt9nothrow_t')
GROWTH = re.compile(r'_M_mutate|_M_create|_M_replace_aux')   # std::string growth: the reallocation path of an append that fits does not run
def no_alloc(sym):
    return sym in ALLOC_SYMS or bool(GROWTH.search(sym))
# Libc-ish leaves with no frame at all, used when --dis is not given
FRAMELESS = re.compile(r'^(?:__aeabi_\w+|mem\w+|str\w+|svc\w+)$')


# ======================================================================================================================
# 1. The call graph of the whole program, from the .ci files
# ======================================================================================================================
class Node:
    def __init__(self, key):
        self.key = key
        self.name = key           # demangled, with the parameters
        self.short = key          # without them
        self.frames = []          # every definition's frame (bytes)
        self.units = set()
        self.defined = False

    @property
    def frame(self):
        return max(self.frames) if self.frames else 0


class Edge:
    __slots__ = ('src', 'kind', 'targets', 'loc', 'method', 'text')

    def __init__(self, src, kind, targets, loc, method='', text=''):
        self.src = src
        self.kind = kind          # 'call' | 'virtual' | 'stdfn' | 'unresolved'
        self.targets = targets    # [node key]
        self.loc = loc
        self.method = method
        self.text = text


LABEL_SIZE = re.compile(r'^(\d+) bytes \(([\w,]+)\)$')
NODE_RE = re.compile(r'^node: \{ title: "(.*?)" label: "(.*)"(?: shape : ellipse)? \}$')
EDGE_RE = re.compile(r'^edge: \{ sourcename: "(.*?)" targetname: "(.*?)"(?: label: "(.*?)")? \}$')


def parse_ci(path, nodes, raw_edges, unit):
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('node:'):
                m = NODE_RE.match(line)
                if not m or m.group(1) == '__indirect_call':
                    continue
                key, label = m.group(1), m.group(2).split('\\n')
                n = nodes.setdefault(key, Node(key))
                sizes = [s for s in (LABEL_SIZE.match(x) for x in label) if s]
                if sizes:
                    n.defined = True
                    n.units.add(unit)
                    n.frames.append(int(sizes[0].group(1)))
                    if 'dynamic' in sizes[0].group(2):
                        n.frames[-1] = max(n.frames[-1], 1)   # (a dynamic frame is reported by the gates; not expected here)
            elif line.startswith('edge:'):
                m = EDGE_RE.match(line)
                if m:
                    raw_edges.append((m.group(1), m.group(2), m.group(3) or ''))


def symbol_of(key):
    """'Sources/azl/uds_engine.cpp:_ZN3azl...' -> '_ZN3azl...' ; a global function's title is the symbol itself"""
    return key.rsplit(':', 1)[1] if ':' in key else key


def demangle(symbols, filt):
    if not filt:
        return {}
    try:
        out = subprocess.run([filt], input='\n'.join(symbols) + '\n', stdout=subprocess.PIPE, universal_newlines=True,
                             check=True).stdout.split('\n')
    except Exception:
        return {}
    return dict(zip(symbols, out))


def strip_params(name):
    """'azl::DlpLobby::Poll(bool)' -> 'azl::DlpLobby::Poll' ; 'f()::{lambda(int)#1}::operator()(int) const' -> '...::operator()'"""
    n = re.sub(r' \[clone [^\]]*\]$', '', name)
    n = re.sub(r' (?:const|volatile)$', '', n)
    if n.endswith(')'):
        depth = 0
        for i in range(len(n) - 1, -1, -1):
            if n[i] == ')':
                depth += 1
            elif n[i] == '(':
                depth -= 1
                if depth == 0:
                    n = n[:i]
                    break
    return n


def collapse_templates(s):
    """std::deque<azl::TxQueue::Item, std::allocator<...> >::erase -> std::deque<>::erase"""
    out, depth = [], 0
    for ch in s:
        if ch == '<':
            if depth == 0:
                out.append('<')
            depth += 1
        elif ch == '>':
            depth -= 1
            if depth == 0:
                out.append('>')
        elif depth == 0:
            out.append(ch)
    return ''.join(out)


class Source:
    """The text of a call site: a call edge's label is file:line:col, col being the '(' of the call."""

    def __init__(self, root, path_map=()):
        self.root = root
        self.path_map = list(path_map)    # [(prefix in the .ci files, prefix on this machine)]: a build in a container names /src/...
        self.cache = {}

    def lines(self, path):
        if path not in self.cache:
            for old, new in self.path_map:
                if path.startswith(old):
                    path = new + path[len(old):]
                    break
            full = path if os.path.isabs(path) else os.path.join(self.root, path)
            try:
                with open(full, errors='replace') as f:
                    self.cache[path] = f.read().split('\n')
            except OSError:
                self.cache[path] = None
        return self.cache[path]

    def at(self, loc):
        """(text of the line, the identifier before the '(' at col, the receiver expression before '->' or '.')"""
        m = re.match(r'^(.*):(\d+):(\d+)$', loc)
        if not m:
            return '', '', ''
        lines = self.lines(m.group(1))
        line, col = int(m.group(2)), int(m.group(3))
        if not lines or line > len(lines) or line < 1:
            return '', '', ''
        text = lines[line - 1]
        head = text[:max(col - 1, 0)]
        mm = re.search(r'(\w+)\s*$', head)
        ident = mm.group(1) if mm else ''
        rm = re.search(r'(\w+)\s*(?:->|\.)\s*\w+\s*$', head)
        return text.strip(), ident, (rm.group(1) if rm else '')


class Graph:
    def __init__(self, ci_dir, src_root, demangler, path_map=()):
        self.nodes = collections.OrderedDict()
        raw = []
        for f in sorted(os.listdir(ci_dir)):
            if f.endswith('.ci'):
                parse_ci(os.path.join(ci_dir, f), self.nodes, raw, f[:-3])
        dem = demangle(sorted({symbol_of(k) for k in self.nodes}), demangler)
        for k, n in self.nodes.items():
            d = dem.get(symbol_of(k), '')
            n.name = d if d else symbol_of(k)       # (a C function, or no demangler: the symbol itself)
            n.short = strip_params(n.name)
        self.src = Source(src_root, path_map)
        mapped = tuple(old for old, _ in path_map)
        self.out = collections.defaultdict(list)
        self.unresolved = []
        self.notes = []
        self.by_symbol = {}
        for k, n in self.nodes.items():
            if n.defined:
                self.by_symbol.setdefault(symbol_of(k), k)
        seen = set()
        for s, t, loc in raw:
            if (s, t, loc) in seen or s not in self.nodes:
                continue
            seen.add((s, t, loc))
            if t == '__indirect_call':
                self.out[s].append(self.resolve_indirect(s, loc))
                continue
            t = self.canon(t)
            if t not in self.nodes:
                self.nodes[t] = Node(t)
                self.nodes[t].name = self.nodes[t].short = symbol_of(t)
            text = '' if (loc.startswith('/') and not loc.startswith(mapped)) else self.src.at(loc)[0]
            self.out[s].append(Edge(s, 'call', [t], loc, text=text))

    def canon(self, key):
        """A call to the complete-object constructor / destructor (C1, D1) is a call to the base one (C2, D2) the compiler defined."""
        if key in self.nodes and self.nodes[key].defined:
            return key
        sym = symbol_of(key)
        for a, b in (('C1E', 'C2E'), ('D1E', 'D2E')):
            if a in sym:
                alt = sym.replace(a, b)
                if alt in self.by_symbol:
                    return self.by_symbol[alt]
        return key

    def find(self, regex, defined_only=True):
        r = re.compile(regex)
        return [k for k, n in self.nodes.items() if (n.defined or not defined_only) and r.search(n.name)]

    def method_nodes(self, prefix, ident):
        keys = [k for k, n in self.nodes.items() if n.defined and n.name.startswith(prefix + ident + '(')]
        if not keys and ident in FOLDED:
            keys = [k for k, n in self.nodes.items() if n.defined and n.name.startswith(prefix + FOLDED[ident] + '(')]
            if keys:
                note = '%s%s is folded into %s%s (identical body, -fipa-icf)' % (prefix, ident, prefix, FOLDED[ident])
                if note not in self.notes:
                    self.notes.append(note)
        return keys

    def resolve_indirect(self, caller, loc):
        text, ident, receiver = self.src.at(loc)
        cname = self.nodes[caller].name
        if receiver in VIRTUAL_RECEIVERS and ident:
            targets = self.method_nodes(VIRTUAL_RECEIVERS[receiver], ident)
            if targets:
                return Edge(caller, 'virtual', targets, loc, ident, text)
        if ident in FUNCTION_POINTERS and not text.lstrip().startswith(('//', '*')):
            targets = [k for k, n in self.nodes.items() if n.defined and re.search(FUNCTION_POINTERS[ident], n.name)]
            if targets:
                return Edge(caller, 'fnptr', targets, loc, ident, text)
        if ident == '_M_invoker' or '_M_invoker' in text or 'std_function.h' in loc:
            m = re.match(r'^std::function<(.*)>::operator\(\)\(', cname)
            sigs = [m.group(1)] if m else [sig for pat, sig in STD_FUNCTION_CALLERS if re.search(pat, cname)]
            for sig in sigs:
                prefix = 'std::_Function_handler<%s, ' % sig
                targets = [k for k, n in self.nodes.items() if n.defined and n.name.startswith(prefix) and '::_M_invoke(' in n.name]
                if targets:
                    return Edge(caller, 'stdfn', targets, loc, sig, text)
        self.unresolved.append((caller, loc, text))
        return Edge(caller, 'unresolved', [], loc, ident, text)


# ======================================================================================================================
# 2. The machine code of the linked plugin: frames and calls of the functions the compiler did not report (libc ...)
# ======================================================================================================================
class ElfModel:
    HDR = re.compile(r'^([0-9a-f]{8}) <(.+)>:$')
    INS = re.compile(r'^\s*([0-9a-f]+):\s+([a-z][\w.]*)\s*(.*)$')
    TARGET = re.compile(r'^([0-9a-f]+) <([^>+]+)(?:\+0x[0-9a-f]+)?>')
    BRANCH = re.compile(r'^(?:bl|b|blx)(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(?:\.n|\.w)?$')
    # "ble", "blt", "bls", "blo" are `b` with a condition, not `bl`
    PLAIN_COND = ('ble', 'blt', 'bls', 'blo')

    @classmethod
    def links(cls, mn):
        """does this branch set lr (a call that comes back), as opposed to a jump (a tail call, a loop)?"""
        return mn.startswith('bl') and mn not in cls.PLAIN_COND

    def __init__(self, text):
        self.frame = {}
        self.depth_memo = {}      # (symbol, steady) -> depth, shared by every Chain
        self.scc = None
        self.calls = collections.defaultdict(set)
        self.linked = collections.defaultdict(set)    # callee symbols reached by `bl` from this function
        self.tailed = collections.defaultdict(set)    # ... and by a plain `b` to ANOTHER function (a tail call: the caller's frame is gone)
        self.indirect = collections.Counter()
        cur, idx, called = None, 0, False
        for line in text.split('\n'):
            m = self.HDR.match(line)
            if m:
                cur, idx, called = m.group(2), 0, False
                self.frame.setdefault(cur, 0)
                continue
            m = self.INS.match(line)
            if not m or cur is None:
                continue
            mn, ops = m.group(2), m.group(3).strip()
            idx += 1
            if self.BRANCH.match(mn):
                t = self.TARGET.match(ops)
                if t:
                    if t.group(2) != cur:
                        self.calls[cur].add(t.group(2))
                        (self.linked if self.links(mn) else self.tailed)[cur].add(t.group(2))
                    if self.links(mn):
                        called = True
                elif mn.startswith('blx'):
                    self.indirect[cur] += 1
                    called = True
                continue
            if mn == 'bx' and ops != 'lr':
                self.indirect[cur] += 1
                continue
            if idx <= 14 and not called:
                self.frame[cur] += self.prologue_bytes(mn, ops)

    def is_tail(self, caller, callee):
        """True when `caller` reaches `callee` only by jumping to it (after popping its frame): the callee's stack does not sit on the caller's"""
        return callee in self.tailed.get(caller, ()) and callee not in self.linked.get(caller, ())

    def succ(self, sym):
        """the callees of a library function that are followed: in the image, not an error / exception path"""
        if not hasattr(self, '_succ'):
            self._succ = {}
        r = self._succ.get(sym)
        if r is None:
            r = self._succ[sym] = sorted(c for c in self.calls.get(sym, ())
                                         if c in self.frame and not EXCLUDED_ELF.search(c) and not any(re.search(x, c) for _, x, _ in EXCLUDED))
        return r

    def scc_sizes(self, roots):
        """Tarjan over what `roots` reach: {symbol: size of its strongly connected component} (1 = not in a cycle, unless self-called)"""
        index, low, onstack, stack, sizes, counter = {}, {}, set(), [], {}, [0]
        for root in roots:
            if root in index or root not in self.frame:
                continue
            work = [(root, iter(self.succ(root)))]
            index[root] = low[root] = counter[0]
            counter[0] += 1
            stack.append(root)
            onstack.add(root)
            while work:
                v, it = work[-1]
                advanced = False
                for w in it:
                    if w not in index:
                        index[w] = low[w] = counter[0]
                        counter[0] += 1
                        stack.append(w)
                        onstack.add(w)
                        work.append((w, iter(self.succ(w))))
                        advanced = True
                        break
                    if w in onstack:
                        low[v] = min(low[v], index[w])
                if advanced:
                    continue
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[v])
                if low[v] == index[v]:
                    comp = []
                    while True:
                        w = stack.pop()
                        onstack.discard(w)
                        comp.append(w)
                        if w == v:
                            break
                    for w in comp:
                        sizes[w] = len(comp) + (1 if len(comp) == 1 and w in self.succ(w) else 0)
        return sizes

    @staticmethod
    def prologue_bytes(mn, ops):
        if mn in ('push', 'stmdb') and (mn == 'push' or ops.startswith('sp!')):
            regs = ops[ops.index('{') + 1:ops.index('}')]
            return 4 * len([r for r in regs.split(',') if r.strip()])
        if mn.startswith('vpush'):
            regs = ops[ops.index('{') + 1:ops.index('}')].strip()
            if '-' in regs and ',' not in regs:
                a, b = regs.split('-')
                n = int(re.sub(r'\D', '', b)) - int(re.sub(r'\D', '', a)) + 1
            else:
                n = len(regs.split(','))
            return n * (8 if regs.startswith('d') else 4)
        m = re.match(r'^sp,\s*sp,\s*#(\d+)', ops)
        if mn == 'sub' and m:
            return int(m.group(1))
        if mn == 'str' and re.match(r'^\w+,\s*\[sp,\s*#-4\]!', ops):
            return 4
        return 0


# ======================================================================================================================
# 3. Depth of a function: its frame plus the deepest of its callees
# ======================================================================================================================
class Chain:
    """Depth at a debug level; optionally some nodes / edges are cut (what a row of the report proves by some other means).

    A node's depth is looked at in two contexts: the ordinary one and the STEADY one, entered at LogFiles::Append, where the
    operator new calls are not followed: the pending buffer of the log files is reserved (16 KB) at start, so a line does not
    allocate in the steady state (Tests/chain_sim/chain_sim.py does the same for the same reason)."""

    def __init__(self, graph, debug, elf=None, cut_node=None, cut_edge=None):
        self.g, self.debug, self.elf = graph, debug, elf
        self.cut_node, self.cut_edge = cut_node or (lambda n: False), cut_edge or (lambda e: False)
        self.memo, self.active, self.cycles, self.unknown_externals = {}, set(), set(), set()
        # (the depth of a library function does not depend on the graph's cuts: the ElfModel keeps one table for all the chains)
        self.elf_memo = elf.depth_memo if elf is not None else {}

    def gated(self, key):
        return self.debug == 0 and bool(GATED.search(self.g.nodes[key].name))

    def excluded(self, e, t):
        sym = symbol_of(t)
        cname = self.g.nodes[e.src].name
        return any(re.search(c, cname) and re.search(s, sym) for c, s, _ in EXCLUDED)

    def skip(self, e, t, steady=False):
        return self.cut_edge(e) or self.cut_node(t) or self.excluded(e, t) or (steady and no_alloc(symbol_of(t)))

    # --- machine-code side -------------------------------------------------------------------------------------------
    def elf_depth(self, sym, steady=False):
        """(bytes, sym) of a library function: the longest simple path over its calls (success paths), or None"""
        if self.elf is None or sym not in self.elf.frame:
            return None
        if self.elf.scc is None:
            self.elf.scc = self.elf.scc_sizes(sorted(self.elf.frame))
        return (self._walk(sym, (), steady), sym)

    def _walk(self, sym, stack, steady):
        key = (sym, steady)
        if key in self.elf_memo:
            return self.elf_memo[key]
        best = 0
        for c in self.elf.succ(sym):
            if c == sym or c in stack or (steady and no_alloc(c)):
                continue
            below = self._walk(c, stack + (sym,), steady)
            best = max(best, below - self.elf.frame[sym] if self.elf.is_tail(sym, c) else below)   # (a jump: the frame is popped first)
        value = self.elf.frame[sym] + best
        if self.elf.scc.get(sym, 1) == 1:      # not in a cycle of the library: its depth does not depend on the way here
            self.elf_memo[key] = value
        return value

    # --- the graph side ----------------------------------------------------------------------------------------------
    def tail(self, caller, callee):
        """does the machine code reach `callee` from `caller` by a jump after popping the frame? (-foptimize-sibling-calls is on at -Os)"""
        return self.elf is not None and self.elf.is_tail(symbol_of(caller), symbol_of(callee))

    def frame_of(self, key):
        n = self.g.nodes[key]
        if n.defined:
            return n.frame
        return self.elf.frame.get(symbol_of(key), 0) if self.elf else 0

    def depth(self, key, steady=False):
        """(bytes, next key | None, approx, steady context of the next)"""
        mk = (key, steady)
        if mk in self.memo:
            return self.memo[mk]
        n = self.g.nodes[key]
        if not n.defined:
            d = self.elf_depth(symbol_of(key), steady)
            if d is None and not FRAMELESS.match(symbol_of(key)):
                self.unknown_externals.add(symbol_of(key))
            r = (0 if d is None else d[0], None, True, steady)
            self.memo[mk] = r
            return r
        if mk in self.active:
            self.cycles.add(n.short)
            return (0, None, False, steady)
        if self.gated(key):
            self.memo[mk] = (n.frame, None, False, steady)
            return self.memo[mk]
        steady = steady or bool(re.search(STEADY_ENTER, n.name))
        self.active.add(mk)
        best, nxt, approx = n.frame, None, False       # (what the function costs with the deepest callee: a leaf costs its frame)
        for e in self.g.out.get(key, ()):
            targets = e.targets if e.kind != 'unresolved' else []
            for t in targets:
                if self.skip(e, t, steady):
                    continue
                d = self.depth(t, steady)
                val = d[0] if self.tail(key, t) else n.frame + d[0]
                if val > best or (nxt is None and val >= best):
                    best, nxt, approx = val, t, d[2]
        self.active.discard(mk)
        extra = ASSUMED_RESOLVER_MAP if any(e.kind == 'unresolved' and e.method == 'map' for e in self.g.out.get(key, ())) else 0
        r = (best + extra, nxt, approx, steady)
        self.memo[mk] = r
        return r

    def path(self, key):
        """[(node, frame, approx, depth)] of the deepest path from `key` (depth = of that node, in the context it is reached in)"""
        out, seen, steady = [], set(), False
        while key is not None and key not in seen:
            seen.add(key)
            d = self.depth(key, steady)
            out.append((key, self.frame_of(key), d[2] and not self.g.nodes[key].defined, d[0]))
            key, steady = d[1], d[3]
        return out

    def prefix_to(self, root, pred, deepest=True):
        """The deepest (or the shallowest) prefix in bytes down to a call site for which pred(edge, target) holds: the frames of
        every function from `root` to the one that makes the call, the callee's own frame NOT included. -> (bytes, [keys], edge)"""
        memo, active = {}, set()

        def go(key):
            if key in memo:
                return memo[key]
            n = self.g.nodes[key]
            if not n.defined or key in active or self.gated(key):
                return None
            active.add(key)
            best = None
            for e in self.g.out.get(key, ()):
                if e.kind == 'unresolved':
                    if pred(e, None):
                        cand = (n.frame, [key], e)
                        if best is None or (cand[0] > best[0] if deepest else cand[0] < best[0]):
                            best = cand
                    continue
                for t in e.targets:
                    if self.cut_edge(e) or self.cut_node(t) or self.excluded(e, t):
                        continue
                    cand = None
                    if pred(e, t):
                        cand = (0 if self.tail(key, t) else n.frame, [key], e)
                    else:
                        sub = go(t)
                        if sub is not None:
                            cand = (sub[0] if self.tail(key, t) else n.frame + sub[0], [key] + sub[1], sub[2])
                    if cand is not None and (best is None or (cand[0] > best[0] if deepest else cand[0] < best[0])):
                        best = cand
            active.discard(key)
            memo[key] = best
            return best
        return go(root)


# ======================================================================================================================
# 4. The report
# ======================================================================================================================
def display(g, key, bound=44):
    n = g.nodes[key]
    s = n.short
    m = re.match(r'^std::_Function_handler<(.*?), (?:azl|\{|std::)?.*>::_M_invoke$', s)
    if s.startswith('std::_Function_handler<'):
        sig = re.match(r'^std::_Function_handler<(.*?), ', s)
        s = 'std::function[%s]::invoke' % (sig.group(1) if sig else '?')
    s = re.sub(r'^azl::(?:ctr::)?', '', s)
    s = re.sub(r'\{lambda\([^)]*\)#\d+\}', '{lambda}', s)
    s = collapse_templates(s) if len(s) > bound else s
    return s if len(s) <= bound else s[:bound - 1] + '~'


def fmt_path(g, chain, key, limit=60):
    parts = []
    for k, fr, approx, below in chain.path(key)[:limit]:
        if g.nodes[k].defined:
            parts.append('%s=%d' % (display(g, k), fr))
        else:                                   # a library function: its whole chain, read from the machine code
            parts.append('%s=~%d' % (display(g, k), below))
    return ' > '.join(parts)


def dispatch_groups(src):
    """[(service, [case names], first line, last line)] of the switches of DlpService::Fkcl and ::Srvr in dlp_ipc.hpp"""
    lines = src.lines('Includes/azl/dlp_ipc.hpp') or []
    groups, service, cur = [], None, None
    for i, text in enumerate(lines, 1):
        if re.search(r'DlpOutcome Fkcl\(', text):
            service = 'FKCL'
        elif re.search(r'void Srvr\(', text):
            service = 'SRVR'
        elif re.search(r'^\s*// ---- dlp:FKCL handlers', text):
            service = None
        if service is None:
            continue
        m = re.match(r'^\s*(?:case\s+(k\w+)|default)\s*:', text)
        if m:
            name = m.group(1) or 'default'
            if cur is not None and cur['empty'] and cur['end'] == i - 1:
                cur['names'].append(name)        # case A: case B: share a body
            else:
                cur = {'service': service, 'names': [name], 'start': i, 'end': i, 'empty': True}
                groups.append(cur)
        elif cur is not None:
            cur['end'] = i
            if text.strip():
                cur['empty'] = False
    return groups


def case_label(gr):
    names = [re.sub(r'^k(?:Fk|Sv)', '', n) for n in gr['names']]
    return '%s %s' % (gr['service'], '/'.join(names))




# ======================================================================================================================
# 4. The report: every way a game thread enters the Download Play hub
# ======================================================================================================================
# The functions the rows are made of, by demangled name. A name that matches nothing is an ERROR ("renamed? update SPEC"): a gate that
# silently measures nothing is not a gate.
SPEC = collections.OrderedDict([
    ('hook', r'^azl_hook_SendSyncRequest$'),
    ('close_hook', r'^azl_hook_CloseHandle$'),
    ('handle', r'^azl::ctr::UdsHle::HandleSendSyncRequest\('),
    ('rundlp', r'^azl::ctr::UdsHle::RunDlp\('),
    ('runndm', r'^azl::ctr::UdsHle::RunNdm\('),
    ('uds_dispatch', r'^azl::ctr::UdsHle::Dispatch\('),
    ('note_closed', r'^azl::ctr::UdsHle::NoteHandleClosed\('),
    ('end_init', r'^azl::ctr::UdsHle::EndInitializedSession\('),
    ('waitjoin', r'^azl::ctr::\(anonymous namespace\)::WaitForJoin\('),
    ('notecommand', r'^azl::ctr::\(anonymous namespace\)::NoteCommand\('),
    ('notefirst', r'^azl::ctr::\(anonymous namespace\)::NoteDlpFirst\('),
    ('notetrouble', r'^azl::ctr::\(anonymous namespace\)::NoteDlpTrouble\('),
    ('tracereq', r'^azl::ctr::\(anonymous namespace\)::DlpTraceRequest\('),
    ('tracerep', r'^azl::ctr::\(anonymous namespace\)::DlpTraceReply\('),
    ('serve', r'^azl::DlpHub::Serve\('),
    ('post', r'^azl::DlpHub::Post\('),
    ('pollpending', r'^azl::DlpHub::PollPending\('),
    ('ongame', r'^azl::DlpHub::OnGameUdsStarting\('),
    ('onclosed', r'^azl::DlpHub::OnSessionClosed\('),
    ('runhere', r'^azl::DlpHub::RunHere\('),
    ('dispatch', r'^azl::DlpService::Dispatch\('),
    ('pollclient', r'^azl::DlpLobby::PollClient\('),
    ('pollserver', r'^azl::DlpLobby::PollServer\('),
    ('sendwifi', r'^azl::RoomClient::SendWifiPacket\('),
])
# The prefixes: where the chain of a row starts in the hook, and the function it enters (the row's own chain begins there)
PREFIXES = {
    'dlp': ('hook', 'rundlp'),            # an IPC call on a dlp:FKCL / dlp:SRVR session: hook > HandleSendSyncRequest > [RunTraced] > Run > RunDlp
    'ndm': ('hook', 'runndm'),            # the same on an ndm:u session (the calibration row: Tests/chain_sim measures it)
    'uds': ('hook', 'uds_dispatch'),      # a nwm::UDS command: ... > Run > UdsHle::Dispatch (OnGameUdsStarting is in its cases 0x01 / 0x1B)
    'close': ('close_hook', 'note_closed'),   # CloseHandle of a fake session
    'hsr': ('hook', 'handle'),            # the call that finds a fake session dead (HandleSendSyncRequest itself)
}
LIMIT_NAMES = {'hle': 'HLE proper', 'log': 'path that logs'}
# nwm::UDS InitializeWithVersion (libctru: header 0x001B0302 = 12 normal words + 2 translate): RunTraced keeps a copy of the 14 words of the
# request in a frame that is dynamic (a variable length array); the compiler's 64 bytes are the fixed part
UDS_INIT_REQUEST_WORDS = 14


def parse_loc(loc):
    m = re.match(r'^(.*):(\d+):\d+$', loc)
    return (m.group(1), int(m.group(2))) if m else ('', 0)


def inline_command_names(src):
    """The ids IsInline (Sources/azl/dlp_hub.cpp) says a game thread serves itself, as the names of DlpService's constants (kFkGetServerInfo ...)"""
    lines = src.lines('Sources/azl/dlp_hub.cpp') or []
    names, inside = set(), False
    for text in lines:
        if re.search(r'bool DlpHub::IsInline\(', text):
            inside = True
        elif inside and re.match(r'^}', text):
            break
        elif inside:
            m = re.search(r'case DlpService::(k(?:Fk|Sv)\w+):', text)
            if m:
                names.add(m.group(1))
    return names


class Row:
    def __init__(self, label, prefix, start, cut_edge, limit, gated, condition, group='', req_words=0):
        self.label, self.prefix, self.start, self.cut_edge = label, prefix, start, cut_edge
        self.limit, self.gated, self.condition, self.group = limit, gated, condition, group
        self.req_words = req_words    # the words the request declares, for the copy RunTraced keeps on the stack at debug 1
        self.chain, self.prefix_bytes, self.total, self.room, self.path, self.prefix_path = {}, {}, {}, {}, {}, ''
        self.problem = ''


class TreeReport:
    def __init__(self, g, elf, kind, repo=None):
        self.g, self.elf, self.kind = g, elf, kind
        self.problems = []
        self.n = {}
        for name, regex in SPEC.items():
            keys = g.find(regex)
            self.n[name] = set(keys)
            if not keys:
                self.problems.append('SPEC: no function matches %s (%s): renamed? update SPEC' % (name, regex))
        self.lognodes = set(g.find(FORMATTED_LOG.pattern))
        self.groups = dispatch_groups(g.src)
        self.inline_names = inline_command_names(g.src)
        if not self.groups:
            self.problems.append('no switch of DlpService::Fkcl / Srvr found in Includes/azl/dlp_ipc.hpp: nothing to split the commands by')
        if not self.inline_names:
            self.problems.append('no inline command found in DlpHub::IsInline (Sources/azl/dlp_hub.cpp)')
        self.rows = []
        self.chains = {}      # (level, id of the cut set) -> Chain, kept so that the memo of a row is reused by its twin
        self.limits = ceilings_from(repo)

    # --- small builders for the cuts ----------------------------------------------------------------------------------------
    def keys(self, *names):
        out = set()
        for nm in names:
            out |= self.n.get(nm, set())
        return out

    def only_via(self, src, dst):
        """cut every edge out of the functions `src` (names of SPEC) that does not go to one of `dst`"""
        srcs, dsts = self.keys(*src), self.keys(*dst)
        return lambda e: e.src in srcs and not (set(e.targets) & dsts)

    def to(self, *names):
        """an edge that goes to one of these functions"""
        dsts = self.keys(*names)
        return lambda e: bool(set(e.targets) & dsts)

    def in_group(self, gr):
        def pred(e):
            f, line = parse_loc(e.loc)
            return f.endswith('Includes/azl/dlp_ipc.hpp') and gr['start'] <= line <= gr['end']
        return pred

    @staticmethod
    def any_of(*preds):
        return lambda e: any(p(e) for p in preds)

    def is_inline(self, gr):
        return all(nm in self.inline_names for nm in gr['names'])

    # --- the rows -------------------------------------------------------------------------------------------------------------
    def build_rows(self):
        R = self.rows
        to_serve = self.only_via(['rundlp'], ['serve'])
        no_poll = self.to('pollclient', 'pollserver')    # DYNAMIC: Poll returns before them in the external-poll mode on a game thread
        cond_inline = ('inline: the lobby\'s tables only; Poll > PollClient / PollServer cut (external-poll flag, dynamic); '
                       'Serve > Post cut (another row)')
        cmds = [gr for gr in self.groups if gr['names'] != ['default']]
        inline = [gr for gr in cmds if self.is_inline(gr)]
        posted = [gr for gr in cmds if not self.is_inline(gr)]
        for gr in inline:
            others = [self.in_group(h) for h in self.groups if h is not gr]
            cut = self.any_of(to_serve, self.to('post'), no_poll, *others)
            R.append(Row(case_label(gr), 'dlp', 'rundlp', cut, 'hle', True, cond_inline, 'inline'))
        if posted:
            R.append(Row('posted command (' + ', '.join(case_label(gr) for gr in posted[:3]) + ' ... %d in all)' % len(posted),
                         'dlp', 'rundlp', self.any_of(to_serve, self.to('dispatch')), 'hle', True,
                         'posted: the game thread leaves the request and waits on the kernel event (the handler runs on the network thread)',
                         'posted'))
        to_wait = self.only_via(['rundlp'], ['waitjoin'])
        R.append(Row('FKCL StartFakeSession: the wait for the join (WaitForJoin > PollPending)', 'dlp', 'rundlp',
                     self.any_of(to_wait, self.only_via(['waitjoin'], ['pollpending']), no_poll), 'hle', True,
                     'the join loop: PollPending reads flags (ClientJoinPoll); Poll cut as above', 'join'))
        # WaitForJoin's cap: OnSessionClosed(Fkcl). The end of a session is a posted request, or, when no network thread takes it, a bit that
        # the next Tick acts on (DlpHub::OnSessionClosed): nothing of the lobby runs on this thread (proof 4 holds the graph to that)
        cap = self.only_via(['waitjoin'], ['onclosed'])
        R.append(Row('FKCL StartFakeSession: the 8 s cap (WaitForJoin > OnSessionClosed)', 'dlp', 'rundlp',
                     self.any_of(to_wait, cap), 'hle', True, 'the request is posted, or the close is left for the next Tick', 'join'))
        # OnGameUdsStarting in the cases 0x01 / 0x1B of UdsHle::Dispatch
        via_game = self.only_via(['uds_dispatch'], ['ongame'])
        R.append(Row('nwm::UDS Initialize > OnGameUdsStarting, network thread alive', 'uds', 'uds_dispatch',
                     self.any_of(via_game, self.to('runhere')), 'hle', True, 'the request is posted (or nothing is held: it returns at once)', 'uds',
                     req_words=UDS_INIT_REQUEST_WORDS))
        for flavour, extra, cond in self.dead_flavours():
            R.append(Row('nwm::UDS Initialize > OnGameUdsStarting, ' + flavour, 'uds', 'uds_dispatch',
                         self.any_of(via_game, self.to('post'), *extra), 'hle', False, cond, 'dead', req_words=UDS_INIT_REQUEST_WORDS))
        # OnSessionClosed from CloseHandle, and from the call that finds the session dead
        for pname, start, limit, where in (('close', 'note_closed', 'log', 'CloseHandle of a dlp session without Finalize'),
                                           ('hsr', 'handle', 'log', 'a call on a dlp session found dead')):
            via = self.any_of(self.only_via([start], ['end_init']), self.only_via(['end_init'], ['onclosed']))
            R.append(Row(where + ' > OnSessionClosed', pname, start, via, limit, True,
                         'the request is posted, or the close is left for the next Tick', 'closed'))
        # the log lines of RunDlp
        for nm, what in (('notecommand', 'NoteCommand (the first call of a command: Log() with a format)'),
                         ('notefirst', 'NoteDlpFirst (the first command of a service: LogLine)'),
                         ('notetrouble', 'NoteDlpTrouble (a refused request: LogLine)'),
                         ('tracereq', 'DlpTraceRequest (the trace of a request: the words and the clock; at debug 0 it leaves at once)'),
                         ('tracerep', 'DlpTraceReply (the trace line of a command: LogLine; at debug 0 it leaves at once)')):
            R.append(Row('RunDlp > ' + what, 'dlp', 'rundlp', self.only_via(['rundlp'], [nm]), 'hle', True,
                         'the HLE proper writes its lines; the hub is not in this chain', 'log'))

        # CALIBRATION: the path Tests/chain_sim measures in the emulator ("HLE: a command of the fake ndm:u"): the numbers of this row
        # and the emulator's differ by what the static sum over-counts (the entry's alignment, a libc branch that is not taken)
        R.append(Row('calibration: ndm:u command (RunNdm, every branch); chain_sim measures 176 / 480', 'ndm', 'runndm',
                     lambda e: False, 'hle', False, 'the path of chain_sim\'s "HLE: a command of the fake ndm:u" scenario', 'calib'))

    def dead_flavours(self):
        """(label, extra cuts, condition) of the rows that run the lobby on the CALLING thread (DlpHub::RunHere: no network thread alive)"""
        no_room = self.to('sendwifi')
        return [('network thread dead, room joined (RunHere: the whole LobbyNet side down to the room client)', [],
                 'DYNAMIC: no Tick for 1000 ms (kAliveMs) while the room is joined; the transport lambda sends'),
                ('network thread dead, no room (RunHere; the transport lambda drops the frame)', [no_room],
                 'DYNAMIC: no Tick for 1000 ms and `!room_->joined()`: SendPacket reaches the lambda and the lambda returns')]

    # --- measuring ------------------------------------------------------------------------------------------------------------
    def chain_for(self, level, row):
        ck = (level, id(row))
        c = Chain(self.g, level, self.elf, cut_edge=row.cut_edge)
        self.chains[ck] = c
        return c

    def prefix_to(self, pname, level, start_key):
        root_name, _ = PREFIXES[pname]
        roots = sorted(self.n[root_name])
        if not roots:
            return None
        # RunTraced (the capture of a request, debug 1 only) is not in the way at debug 0, and not in the way of a dlp session of the hub at
        # any level: HandleSendSyncRequest sends those to Run directly (proof 3 holds that sentence to the source)
        runtraced = set(self.g.find(r'^azl::ctr::UdsHle::RunTraced\('))
        ch = Chain(self.g, level, self.elf, cut_node=(lambda k: k in runtraced) if (level == 0 or pname == 'dlp') else None)
        best = ch.prefix_to(roots[0], lambda e, t: t == start_key)
        return None if best is None else (best[0], best[1])

    def measure(self):
        for row in self.rows:
            starts = sorted(self.n[row.start])
            if not starts:
                row.problem = 'start function not found'
                continue
            start = starts[0]
            for level in (0, 1):
                ch = self.chain_for(level, row)
                pre = self.prefix_to(row.prefix, level, start)
                if pre is None:
                    row.problem = 'no path from the hook to %s' % row.start
                    break
                d = ch.depth(start)[0]
                row.chain[level] = d
                extra = ((4 * row.req_words + 7) & ~7) if (level == 1 and row.req_words and row.prefix in ('uds', 'ndm')) else 0
                row.prefix_bytes[level] = ASM_ENTRY + pre[0] + extra
                row.total[level] = ASM_ENTRY + pre[0] + extra + d
                limit = self.limits['hle'][level] if row.limit == 'hle' else self.limits['log']
                row.room[level] = limit - row.total[level]
                row.path[level] = fmt_path(self.g, ch, start)
                row.prefix_path = ' > '.join('%s=%d' % (display(self.g, k), self.g.nodes[k].frame) for k in pre[1])
            if row.gated and not row.problem:
                for level in (0, 1):
                    if row.room[level] < 0:
                        self.problems.append('%s: %d bytes at debug %d, over the %d of the %s (by %d)' % (
                            row.label, row.total[level], level,
                            self.limits['hle'][level] if row.limit == 'hle' else self.limits['log'],
                            LIMIT_NAMES[row.limit], -row.room[level]))
            if row.problem:
                self.problems.append('%s: %s' % (row.label, row.problem))

    # --- proofs ---------------------------------------------------------------------------------------------------------------
    def reach(self, start_keys, cut_edge):
        """({node keys reached}, [edges followed]) from the start keys, honouring the cut"""
        seen, edges, stack = set(), [], list(start_keys)
        while stack:
            k = stack.pop()
            if k in seen:
                continue
            seen.add(k)
            for e in self.g.out.get(k, ()):
                if cut_edge(e):
                    continue
                edges.append(e)
                stack.extend(t for t in e.targets if t not in seen)
        return seen, edges

    def prove(self, out):
        """What the game thread can reach in the inline rows. Static, except for the one dynamic cut the report names."""
        g = self.g
        p = lambda *a: out.append(' '.join(str(x) for x in a))
        inline_rows = [r for r in self.rows if r.group == 'inline']
        p()
        p('PROOF 1, STATIC: what a game thread reaches when it serves a command INLINE (Poll > PollClient / PollServer cut: the external-poll flag)')
        if not inline_rows:
            self.problems.append('proof 1: no inline row')
            p('  no inline row')
            return
        union_nodes, union_edges = set(), []
        for r in inline_rows:
            nodes, edges = self.reach(self.n[r.start], r.cut_edge)
            union_nodes |= nodes
            union_edges += edges
        net = sorted({g.nodes[k].short for k in union_nodes if g.nodes[k].short.startswith('azl::EngineLobbyNet::')})
        eng = sorted({g.nodes[k].short for k in union_nodes if g.nodes[k].short.startswith('azl::UdsEngine::')})
        clock = sorted({e.method for e in union_edges if e.kind == 'virtual' and e.method in CLOCK_METHODS})
        virt = sorted({'%s (from %s)' % (e.method, display(g, e.src)) for e in union_edges
                       if e.kind == 'virtual' and e.method not in CLOCK_METHODS})
        logs = sorted({g.nodes[k].short for k in union_nodes if k in self.lognodes})
        p('  functions reached: %d; LobbyNet methods reached: %s; virtual calls made: the clock (%s)%s; UdsEngine functions: %s; '
          'Log() / DebugEvent() with a format: %s' % (
              len(union_nodes), ', '.join(display(g, k, 60) for k in sorted(union_nodes) if g.nodes[k].short.startswith('azl::EngineLobbyNet::')) or 'none',
              ', '.join(clock) or 'none', (' and ' + ', '.join(virt)) if virt else ' and nothing else',
              ', '.join(eng) or 'none', ', '.join(logs) or 'none'))
        bad_net = [m for m in net if m.rsplit('::', 1)[1] not in CLOCK_METHODS]
        if bad_net:
            self.problems.append('proof 1: an inline command reaches LobbyNet method(s) %s (only %s are allowed on a game thread)' % (
                ', '.join(bad_net), '/'.join(CLOCK_METHODS)))
        if eng:
            self.problems.append('proof 1: an inline command reaches the engine: %s' % ', '.join(eng))
        if virt:
            self.problems.append('proof 1: an inline command makes a virtual call: %s' % '; '.join(virt))
        if logs:
            self.problems.append('proof 1: an inline command reaches Log() / DebugEvent() with a format: %s' % ', '.join(logs))
        unres = sorted({'%s %s' % (display(g, e.src), e.loc) for e in union_edges if e.kind == 'unresolved'})
        if unres:
            self.problems.append('proof 1: an inline command makes a call the tool cannot resolve: %s' % '; '.join(unres))
        # the dynamic part: what the cut Poll would reach
        poll_targets = self.keys('pollclient', 'pollserver')
        no_cut = lambda e: False
        _, edges2 = self.reach(poll_targets, no_cut)
        held = sorted({e.method for e in edges2 if e.kind == 'virtual' and e.method not in CLOCK_METHODS})
        p('  DYNAMIC part, not proved here: DlpLobby::Poll holds back PollClient / PollServer, which call %s; Poll returns before them when '
          '(flags2_ & (kF2External | kF2OnNet)) == kF2External (the hub sets the first, only Tick sets the second); proved by '
          'Tests/dlp_hub_test.cpp (a spy LobbyNet) and Tests/chain_sim (the compiled code, a spy vtable)' % (', '.join(held) or 'nothing'))
        if not held:
            self.problems.append('proof 1: the tool sees nothing behind Poll > PollClient / PollServer (the cut would be meaningless: renamed?)')
        # Poll must test the flag BEFORE anything else: check the source text of its first statement
        lobby = g.src.lines('Includes/azl/dlp_lobby.hpp') or []
        poll_ok = False
        for i, text in enumerate(lobby):
            if re.search(r'^\s*void Poll\(bool force\) \{', text):
                body = ' '.join(lobby[i + 1:i + 3])
                poll_ok = bool(re.search(r'if \(use_ == kUseNone \|\| \(flags2_ & \(kF2External \| kF2OnNet\)\) == kF2External \|\| !Due\(force\)\)', body))
                break
        p('  Poll\'s first statement tests the external-poll flag before Due() and every call out: %s' % ('yes' if poll_ok else 'NO'))
        if not poll_ok:
            self.problems.append('proof 1: DlpLobby::Poll no longer starts with the external-poll test (Includes/azl/dlp_lobby.hpp)')

        # PROOF 2: no formatter below the hub, whatever the row (the rows' cuts are the rows' own statements)
        p()
        p('PROOF 2, STATIC: Log() / DebugEvent() with a format reachable from the rows (the HLE proper\'s own NoteCommand row apart)')
        found = False
        for r in self.rows:
            if r.group in ('log', 'calib') or r.problem or not self.n[r.start]:
                continue
            nodes, _ = self.reach(self.n[r.start], r.cut_edge)
            hit = sorted({g.nodes[k].short for k in nodes if k in self.lognodes})
            if hit:
                found = True
                self.problems.append('proof 2: %s reaches Log() / DebugEvent() with a format: %s' % (r.label, ', '.join(hit)))
        p('  %s' % ('FOUND (see the problems)' if found else 'none: no row reaches Log() / DebugEvent() with a format'))

        # PROOF 3: the end of a session touches nothing of the lobby's network side on the calling thread
        p()
        p('PROOF 3, STATIC: DlpHub::OnSessionClosed (the end of a session: the CloseHandle hook of any thread, the join wait\'s cap) reaches nothing of '
          'the lobby, the service, the adapter or the engine - no cut at all')
        bad4 = []
        for k in sorted(self.n['onclosed']):
            nodes, _ = self.reach([k], lambda e: False)
            bad4 += sorted({g.nodes[x].short for x in nodes if re.match(r'^azl::(?:DlpLobby|DlpService|EngineLobbyNet|UdsEngine)::', g.nodes[x].short)
                            or x in self.n['runhere']})
        p('  %s' % (', '.join(bad4) if bad4 else 'nothing: it posts a request or leaves a bit for the next Tick'))
        if bad4:
            self.problems.append('proof 3: DlpHub::OnSessionClosed reaches the lobby\'s network side on the calling thread: %s' % ', '.join(bad4[:6]))

        # PROOF 4: the hub's sessions do not carry RunTraced's copy of the request (up to 63 words on the stack, debug 1)
        p()
        p('PROOF 4, SOURCE: a dlp session of the hub goes to Run directly, not through RunTraced (whose copy of the request is as long as the '
          'header declares: 15 words for SRVR InitializeWithName, 252 bytes at the most, under the hub\'s whole chain)')
        text = g.src.lines('Sources/ctr/uds_hle.cpp') or []
        guarded = False
        for i, line in enumerate(text):
            if re.search(r'^\s*RunTraced\(kind, ndm_session, id, cmd\);', line):
                prev = [x for x in text[max(0, i - 3):i] if x.strip() and not x.strip().startswith('//')]
                guarded = bool(prev) and bool(re.search(r'if \(g_debug_level > 0 && \(kind < kKindDlp \|\| dlp_hub_ == nullptr\)\) \{', prev[-1]))
                break
        p('  the call of RunTraced is guarded by "kind < kKindDlp || dlp_hub_ == nullptr": %s' % ('yes' if guarded else 'NO'))
        if not guarded:
            self.problems.append('proof 4: HandleSendSyncRequest sends a hub session through RunTraced (Sources/ctr/uds_hle.cpp): its copy of the '
                                 'request puts up to 252 bytes under the hub\'s chain at debug 1')

    # --- the report -------------------------------------------------------------------------------------------------------------
    def run(self, out, gate=False):
        g = self.g
        p = lambda *a: out.append(' '.join(str(x) for x in a))
        p('== %s' % (self.kind or 'chain'))
        p('   %d functions with a frame (the .ci of %d units), %d without (calls out of the compiled code), %d call sites, '
          '%d indirect calls the tool could not resolve' % (
              sum(1 for n in g.nodes.values() if n.defined), len({u for n in g.nodes.values() for u in n.units}),
              sum(1 for n in g.nodes.values() if not n.defined), sum(len(v) for v in g.out.values()), len(g.unresolved)))
        for note in g.notes:
            p('   note: ' + note)
        if self.elf is None:
            p('   NOTE: no --dis: calls into libc / libstdc++ / libctru count 0 bytes (the totals are LOWER BOUNDS)')
        else:
            both = [(n.frame, self.elf.frame[symbol_of(k)]) for k, n in g.nodes.items()
                    if n.defined and symbol_of(k) in self.elf.frame and n.frame > 0]
            exact = sum(1 for a, b in both if a == b)
            p('   machine-code estimator checked against the compiler on %d functions of the plugin: %d equal, mean absolute error %.1f '
              'bytes, worst %d' % (len(both), exact, sum(abs(a - b) for a, b in both) / max(len(both), 1), max([abs(a - b) for a, b in both] or [0])))
        p('   ceilings (Tests/chain_sim/chain_sim.py): HLE proper %d / %d, a path that logs %d, anything on the smallest thread %d; '
          'the entry azl_svc32_entry costs up to %d' % (self.limits['hle'][0], self.limits['hle'][1], self.limits['log'], BUDGET_THREAD, ASM_ENTRY))
        if self.problems and any(pr.startswith('SPEC') for pr in self.problems):
            for pr in self.problems:
                p('  PROBLEM: ' + pr)
            return
        self.build_rows()
        self.measure()

        # --- the prefixes ---------------------------------------------------------------------------------------------------------
        p()
        p('THE FRAMES THE HOOK PAYS BEFORE THE HUB (bytes below the SP of azl_svc32_entry; the entry itself: %d worst case)' % ASM_ENTRY)
        shown = set()
        for r in self.rows:
            if r.prefix in shown or r.problem:
                continue
            shown.add(r.prefix)
            p('  %-5s d0 %3d   d1 %3d   %s' % (r.prefix, r.prefix_bytes[0], r.prefix_bytes[1], r.prefix_path))

        # --- the table ----------------------------------------------------------------------------------------------------------
        p()
        p('THE ENTRIES (chain = frames from the function the row starts in, its own included; total = entry %d + the hook\'s frames + chain; '
          'room = ceiling - total; * = not gated, a DYNAMIC condition)' % ASM_ENTRY)
        p('  %-92s %5s %5s | %6s %6s | %6s %6s | %s' % ('entry', 'ch d0', 'ch d1', 'tot d0', 'tot d1', 'room0', 'room1', 'limit'))
        groups = collections.OrderedDict()
        for r in self.rows:
            if r.problem:
                continue
            groups.setdefault((r.label if r.group != 'inline' else '#inline', r.chain[0], r.chain[1], r.total[0], r.total[1], r.limit,
                               r.path[1] if r.group == 'inline' else ''), []).append(r)
        for key, rs in groups.items():
            r = rs[0]
            label = r.label if r.group != 'inline' else ('inline: ' + rs[0].label + (' (+%d like it)' % (len(rs) - 1) if len(rs) > 1 else ''))
            p('  %-92s %5d %5d | %6d %6d | %6d %6d | %s%s' % (label[:92], r.chain[0], r.chain[1], r.total[0], r.total[1], r.room[0], r.room[1],
                                                         '%d / %d' % tuple(self.limits['hle']) if r.limit == 'hle' else str(self.limits['log']),
                                                         '' if r.gated else ' *'))
            if len(rs) > 1 and not gate:
                p('        like it: ' + ', '.join(x.label for x in rs[1:]))
        inline_rows = [r for r in self.rows if r.group == 'inline' and not r.problem]
        if inline_rows:
            w0 = max(inline_rows, key=lambda r: r.chain[0])
            w1 = max(inline_rows, key=lambda r: r.chain[1])
            p('  tightest inline command: d0 %d (%s), d1 %d (%s)' % (w0.chain[0], w0.label, w1.chain[1], w1.label))
        gated = [r for r in self.rows if r.gated and not r.problem]
        if gated:
            t0 = min(gated, key=lambda r: r.room[0])
            t1 = min(gated, key=lambda r: r.room[1])
            p('  least room among the gated entries: d0 %d (%s), d1 %d (%s)' % (t0.room[0], t0.label, t1.room[1], t1.label))
        if not gate:
            p()
            p('THE DEEPEST PATHS')
            for r in self.rows:
                if r.problem or r.group == 'inline' and r is not max(inline_rows, key=lambda x: x.chain[1]):
                    continue
                p('  %s' % r.label)
                p('    condition: ' + r.condition)
                p('    d0: ' + r.path[0])
                if r.path[1] != r.path[0]:
                    p('    d1: ' + r.path[1])

        # --- the proofs ----------------------------------------------------------------------------------------------------------
        self.prove(out)

        # --- assumptions ----------------------------------------------------------------------------------------------------------
        if not gate:
            reach_nodes = set()
            for r in self.rows:
                if not r.problem:
                    nodes, _ = self.reach(self.n[r.start], r.cut_edge)
                    reach_nodes |= nodes
            p()
            p('UNRESOLVED INDIRECT CALLS reachable from the rows:')
            shown = set()
            for caller, loc, text in g.unresolved:
                if caller in reach_nodes and (caller, loc) not in shown:
                    shown.add((caller, loc))
                    p('  %s  %s  | %s' % (display(g, caller, 36), loc, text[:80]))
            if not shown:
                p('  none')
            c1 = Chain(g, 1, self.elf)
            ext = sorted({symbol_of(k) for k in reach_nodes if not g.nodes[k].defined})
            p()
            p('CALLS INTO LIBRARIES reachable from the rows (frame / chain read from the machine code, success paths):')
            for sym in ext:
                d = c1.elf_depth(sym)
                p('  %-48s %s' % (sym, ('frame %d, chain %d' % (self.elf.frame[sym], d[0])) if d else ('UNKNOWN (counted 0)' if not FRAMELESS.match(sym) else 'frameless leaf')))
            p('EXCLUDED edges: ' + '; '.join(w for _, _, w in EXCLUDED) + '; in the libraries: the exception machinery, abort, assert, printf')
        # an unknown library function reachable from a gated row is a hole in the numbers
        unknown = set()
        for r in gated:
            for level in (0, 1):
                ch = self.chains.get((level, id(r)))
                if ch is not None:
                    unknown |= ch.unknown_externals
        if unknown and self.elf is not None:
            p('  NOTE: library functions the machine code could not size (counted 0): ' + ', '.join(sorted(unknown)))

        p()
        if self.problems:
            for pr in self.problems:
                p('PROBLEM: ' + pr)
            p('chain report: %d problem(s)' % len(self.problems))
        else:
            p('chain report: every gated entry is within its ceiling, and the proofs hold')

    def json(self):
        return {'rows': [{'label': r.label, 'group': r.group, 'gated': r.gated, 'chain': r.chain, 'total': r.total, 'room': r.room,
                          'limit': r.limit, 'path0': r.path.get(0), 'path1': r.path.get(1), 'problem': r.problem} for r in self.rows],
                'problems': self.problems}


def ceilings_from(repo):
    """HLE_CEILING and BUDGET_LOG of Tests/chain_sim/chain_sim.py when the repository is given (the numbers live in ONE place)"""
    limits = {'hle': [HLE_CEILING[0], HLE_CEILING[1]], 'log': BUDGET_LOG}
    if repo:
        path = os.path.join(repo, 'Tests', 'chain_sim', 'chain_sim.py')
        try:
            with open(path, errors='replace') as f:
                text = f.read()
        except OSError:
            return limits
        m = re.search(r'^HLE_CEILING = \{0: (\d+), 1: (\d+)\}', text, re.M)
        if m:
            limits['hle'] = [int(m.group(1)), int(m.group(2))]
        m = re.search(r'^BUDGET_LOG = (\d+)', text, re.M)
        if m:
            limits['log'] = int(m.group(1))
    return limits

def selftest_core():
    """The graph machinery against a made-up program (a gate that cannot say no is not a gate): sums, the deepest branch, the debug level,
    a cut, a virtual call resolved from its source line, the steady context, a recursion, the machine-code frames."""
    import tempfile
    d = tempfile.mkdtemp()
    src = os.path.join(d, 'src')
    os.makedirs(os.path.join(src, 'Includes'))
    with open(os.path.join(src, 'Includes', 'x.hpp'), 'w') as f:
        f.write('void a() {\n    net_->Send(1);\n}\n')
    names = {'f1': 'f1()', 'f2': 'f2()', 'f3': 'f3()', 'g': 'g()', 'LOG': 'azl::Log(char const*, ...)', 'FMT': 'formatter()',
             'LOBBY': 'azl::DlpLobby::A()', 'SEND': 'azl::EngineLobbyNet::Send(unsigned char)', 'R1': 'rec1()', 'R2': 'rec2()',
             'APP': 'azl::LogFiles::Append(char const*)', 'OTHER': 'other()'}
    ci = ['graph: { title: "t.cpp"', 'node: { title: "__indirect_call" label: "Indirect Call Placeholder" shape : ellipse }']

    def fn(name, size):
        ci.append('node: { title: "%s" label: "%s\\nIncludes/x.hpp:1:1\\n%d bytes (static)" }' % (name, name, size))

    def call(a, b, loc='Includes/x.hpp:1:1'):
        ci.append('edge: { sourcename: "%s" targetname: "%s" label: "%s" }' % (a, b, loc))
    for name, size in (('f1', 16), ('f2', 32), ('f3', 8), ('g', 100), ('LOG', 16), ('FMT', 200), ('LOBBY', 8), ('SEND', 40),
                       ('R1', 8), ('R2', 8), ('APP', 24), ('OTHER', 16)):
        fn(name, size)
    call('f1', 'f2')
    call('f2', 'f3')
    call('f1', 'g')
    call('g', 'LOG')
    call('LOG', 'FMT')
    call('LOBBY', '__indirect_call', 'Includes/x.hpp:2:15')
    call('R1', 'R2')
    call('R2', 'R1')
    call('APP', '_Znwj')
    call('OTHER', '_Znwj')
    with open(os.path.join(d, 't.ci'), 'w') as f:
        f.write('\n'.join(ci) + '\n}\n')
    real_demangle = globals()['demangle']
    globals()['demangle'] = lambda symbols, filt: {x: names.get(x, x) for x in symbols}
    try:
        g = Graph(d, src, 'stub')
    finally:
        globals()['demangle'] = real_demangle
    bad, checks = [], [0]

    def expect(what, got, want):
        checks[0] += 1
        if got != want:
            bad.append('%s: %r instead of %r' % (what, got, want))
    elf = ElfModel('0000a000 <_Znwj>:\n    a000:\tpush\t{r4, lr}\n    a004:\tbl\t0000a100 <malloc>\n0000a100 <malloc>:\n'
                   '    a100:\tpush\t{r4, r5, r6, lr}\n    a104:\tsub\tsp, sp, #16\n    a108:\tbx\tlr\n')
    c0, c1 = Chain(g, 0), Chain(g, 1)
    expect('the deepest branch: 16 + 100 + Log + formatter', c1.depth('f1')[0], 16 + 100 + 16 + 200)
    expect('at debug 0 a call of Log costs its own frame', c0.depth('f1')[0], 16 + 100 + 16)
    expect('a cut node is not followed', Chain(g, 1, cut_node=lambda n: n == 'g').depth('f1')[0], 16 + 32 + 8)
    expect('a virtual call is resolved from its source line', g.out['LOBBY'][0].kind, 'virtual')
    expect("... to the adapter's method", Chain(g, 1).depth('LOBBY')[0], 8 + 40)
    cr = Chain(g, 1)
    expect('a recursion ends', cr.depth('R1')[0], 16)
    expect('... and is reported', sorted(cr.cycles), ['rec1'])
    expect('the machine-code frame: push of two registers', elf.frame.get('_Znwj'), 8)
    expect('... push of four registers and sub sp, #16', elf.frame.get('malloc'), 32)
    expect('a library chain is the sum of its frames', Chain(g, 1, elf).depth('OTHER')[0], 16 + 8 + 32)
    expect("operator new is not followed from the log's append (steady)", Chain(g, 1, elf).depth('APP')[0], 24)
    expect('no machine code, no library frame', Chain(g, 1).depth('OTHER')[0], 16)
    # a jump to another function (a tail call) leaves the caller's frame behind; a `bl` does not; "ble" is a conditional `b`
    elf2 = ElfModel('0000b000 <A>:\n    b000:\tpush\t{r4, r5, lr}\n    b004:\tbl\t0000b100 <B>\n    b008:\tpop\t{r4, r5, lr}\n'
                    '    b00c:\tb\t0000b200 <C>\n0000b100 <B>:\n    b100:\tpush\t{r4}\n    b104:\tbx\tlr\n'
                    '0000b200 <C>:\n    b200:\tpush\t{r4, r5, r6, r7, r8, lr}\n    b204:\tble\t0000b300 <D>\n    b208:\tbx\tlr\n'
                    '0000b300 <D>:\n    b300:\tpush\t{r4}\n    b304:\tbx\tlr\n')
    expect('a plain b to another function is a tail call', elf2.is_tail('A', 'C'), True)
    expect('a bl is not', elf2.is_tail('A', 'B'), False)
    expect('"ble" is a conditional jump (a tail call), not a call', elf2.is_tail('C', 'D'), True)
    expect('bl, bleq and blx link; ble, blt, bls, blo and b do not',
           [ElfModel.links(m) for m in ('bl', 'bleq', 'blx', 'ble', 'blt', 'bls', 'blo', 'b', 'beq')], [True, True, True, False, False, False, False, False, False])
    expect('the chain of a function that tail-calls does not carry its own frame into the callee: max(12 + 4 through the bl, 24 through the jump to C, '
           'whose own jump to D leaves its frame too)', Chain(g, 1, elf2).elf_depth('A')[0], max(12 + 4, 24))
    shutil.rmtree(d)
    for line in bad:
        print('chain self-test FAILED: ' + line)
    print('chain self-test: %s' % ('ok (%d checks)' % checks[0] if not bad else '%d failures' % len(bad)))
    return 1 if bad else 0




def selftest_tree():
    """TreeReport against a made-up plugin: the inline / posted split by IsInline, the cuts, the prefixes, the ceilings, and each of the
    proofs REFUSING a defect (a gate that cannot say no is not a gate)."""
    import tempfile
    d = tempfile.mkdtemp()
    src = os.path.join(d, 'src')
    ipc = ['// fake',
           '    DlpOutcome Fkcl(u32 id) {',                                  # 2
           '        switch (id) {',
           '        case kFkGetServerInfo:',                                 # 4  (inline)
           '            FkServerInfo(cmd);',                                 # 5
           '            break;',
           '        case kFkInitialize:',                                    # 7  (posted)
           '            FkInitialize(cmd);',                                 # 8
           '            break;',
           '        default:',
           '            Unknown(cmd);',                                      # 11
           '            break;',
           '        }',
           '    }',
           '    void Srvr(u32 id) {',                                        # 15
           '        switch (id) {',
           '        case kSvGetServerState:',                                # 17 (inline)
           '            state = lobby_->ServerState();',                     # 18
           '            break;',
           '        default:',
           '            break;',
           '        }',
           '    }',
           '    // ---- dlp:FKCL handlers ----']
    hub = ['bool DlpHub::IsInline(DlpKind kind, u32 id) {', '    switch (id) {', '    case DlpService::kFkGetServerInfo:',
           '    case DlpService::kSvGetServerState:', '        return true;', '    }', '    return false;', '}']
    lobby = ['// fake', '    void Poll(bool force) {',
             '        if (use_ == kUseNone || (flags2_ & (kF2External | kF2OnNet)) == kF2External || !Due(force)) {',
             '            return;', '        }', '        net_->ScanRead(1, nullptr);', '        net_->HostOpen(1);', '        net_->NowMs();',
             '        kernel_->WaitEvent(1, 2);']
    for rel, lines in (('Includes/azl/dlp_ipc.hpp', ipc), ('Sources/azl/dlp_hub.cpp', hub), ('Includes/azl/dlp_lobby.hpp', lobby)):
        os.makedirs(os.path.dirname(os.path.join(src, rel)), exist_ok=True)
        with open(os.path.join(src, rel), 'w') as f:
            f.write('\n'.join(lines) + '\n')

    def col(lines, n, token):
        return lines[n - 1].index(token) + 1
    LOBBY, IPC = 'Includes/azl/dlp_lobby.hpp', 'Includes/azl/dlp_ipc.hpp'
    names = {
        'hook': 'azl_hook_SendSyncRequest', 'close_hook': 'azl_hook_CloseHandle', 'hsr': 'azl::ctr::UdsHle::HandleSendSyncRequest(unsigned long, long*)',
        'run': 'azl::ctr::UdsHle::RunDlp(unsigned char, unsigned long, unsigned long*)',
        'rn': 'azl::ctr::UdsHle::RunNdm(int, unsigned long, unsigned long*)',
        'udsd': 'azl::ctr::UdsHle::Dispatch(unsigned long*)', 'nhc': 'azl::ctr::UdsHle::NoteHandleClosed(unsigned long)',
        'endi': 'azl::ctr::UdsHle::EndInitializedSession(unsigned char)',
        'wj': 'azl::ctr::(anonymous namespace)::WaitForJoin(azl::DlpHub&, unsigned long*)',
        'nc': 'azl::ctr::(anonymous namespace)::NoteCommand(char const*, unsigned long, unsigned long)',
        'nf': 'azl::ctr::(anonymous namespace)::NoteDlpFirst(bool, unsigned long)',
        'nt': 'azl::ctr::(anonymous namespace)::NoteDlpTrouble(azl::DlpHub const&, bool, unsigned long)',
        'trq': 'azl::ctr::(anonymous namespace)::DlpTraceRequest(bool, unsigned long const*)',
        'trp': 'azl::ctr::(anonymous namespace)::DlpTraceReply(bool, unsigned long, unsigned long const*)',
        'serve': 'azl::DlpHub::Serve(azl::DlpKind, unsigned long*, unsigned long const*)', 'post': 'azl::DlpHub::Post(azl::DlpHub::Op, azl::DlpKind)',
        'pp': 'azl::DlpHub::PollPending(unsigned long*)', 'og': 'azl::DlpHub::OnGameUdsStarting()', 'oc': 'azl::DlpHub::OnSessionClosed(azl::DlpKind)',
        'rh': 'azl::DlpHub::RunHere(azl::DlpHub::Op, azl::DlpKind)', 'disp': 'azl::DlpService::Dispatch(azl::DlpKind, unsigned long*)',
        'pc': 'azl::DlpLobby::PollClient()', 'ps': 'azl::DlpLobby::PollServer()', 'sw': 'azl::RoomClient::SendWifiPacket(azl::WifiPacket const&)',
        'hinfo': 'azl::DlpService::FkServerInfo(unsigned long*)', 'hinit': 'azl::DlpService::FkInitialize(unsigned long*)',
        'poll': 'azl::DlpLobby::Poll(bool)', 'lob': 'azl::DlpLobby::ClientServerInfo()', 'now': 'azl::EngineLobbyNet::NowMs()',
        'scan': 'azl::EngineLobbyNet::ScanRead(unsigned long, azl::HeardGroup*)', 'open': 'azl::EngineLobbyNet::HostOpen(unsigned long)',
        'wait': 'azl::ctr::CtrKernelOps::WaitEvent(unsigned long, long long)', 'clr': 'azl::ctr::CtrKernelOps::ClearEvent(unsigned long)',
        'eng': 'azl::UdsEngine::Shutdown()',
    }
    frames = {'hook': 16, 'hsr': 48, 'run': 32, 'serve': 40, 'post': 32, 'disp': 24, 'hinfo': 48, 'hinit': 16, 'poll': 8, 'pc': 24, 'lob': 16,
              'now': 8, 'scan': 80, 'open': 40, 'wait': 8, 'clr': 8, 'wj': 40, 'oc': 32, 'rh': 24, 'eng': 56, 'sw': 96, 'nhc': 56, 'endi': 0,
              'trq': 16, 'trp': 64}

    def program(defect=None):
        ci = ['graph: { title: "t.cpp"', 'node: { title: "__indirect_call" label: "Indirect Call Placeholder" shape : ellipse }']
        for key, name in names.items():
            ci.append('node: { title: "%s" label: "%s\\nIncludes/x.hpp:1:1\\n%d bytes (static)" }' % (key, name, frames.get(key, 8)))

        def call(a, b, loc='Sources/azl/dlp_hub.cpp:1:1'):
            ci.append('edge: { sourcename: "%s" targetname: "%s" label: "%s" }' % (a, b, loc))

        def via_ptr(a, n, token):       # an indirect call at line n of the fake lobby header
            ci.append('edge: { sourcename: "%s" targetname: "__indirect_call" label: "%s:%d:%d" }' % (a, LOBBY, n, col(lobby, n, token)))
        call('hook', 'hsr'); call('hsr', 'run'); call('hsr', 'rn'); call('rn', 'nc'); call('hook', 'close_hook'); call('close_hook', 'nhc')
        call('run', 'nf'); call('run', 'serve'); call('run', 'wj'); call('run', 'nt'); call('run', 'nc'); call('run', 'trq'); call('run', 'trp')
        call('serve', 'post'); call('serve', 'disp')
        via_ptr('post', 9, '(')            # kernel_->WaitEvent
        call('disp', 'poll', LOBBY + ':3:1')
        call('poll', 'pc', LOBBY + ':3:1')
        via_ptr('pc', 6, '(')              # net_->ScanRead (held back by the external-poll flag)
        call('disp', 'hinfo', IPC + ':5:13'); call('disp', 'hinit', IPC + ':8:13')
        call('hinfo', 'lob', IPC + ':5:13')
        via_ptr('lob', 8, '(')             # net_->NowMs (the clock)
        via_ptr('hinit', 7, '(')           # net_->HostOpen (a posted command: the network thread's)
        call('wj', 'pp'); call('wj', 'oc'); call('oc', 'post'); call('rh', 'eng'); call('eng', 'sw')
        call('hsr', 'udsd'); call('udsd', 'og'); call('og', 'post'); call('og', 'rh')
        call('nhc', 'endi'); call('endi', 'oc'); call('hsr', 'endi')
        if defect == 'closed_inline':     # the end of a session runs the lobby's network side on the calling thread again
            call('oc', 'rh')
        if defect == 'virtual':       # an inline command asks the network
            via_ptr('hinfo', 6, '(')
        if defect == 'engine':        # an inline command reaches the engine
            call('hinfo', 'eng')
        if defect == 'log':
            names['lg'] = 'azl::Log(char const*, ...)'
            ci.insert(2, 'node: { title: "lg" label: "azl::Log(char const*, ...)\\nIncludes/x.hpp:1:1\\n16 bytes (static)" }')
            call('hinfo', 'lg')
        return '\n'.join(ci) + '\n}\n'

    bad, checks = [], [0]

    def expect(what, got, want):
        checks[0] += 1
        if got != want:
            bad.append('%s: %r instead of %r' % (what, got, want))

    def run(defect=None, with_ceiling=None):
        with open(os.path.join(d, 't.ci'), 'w') as f:
            f.write(program(defect))
        uds = ['void UdsHle::HandleSendSyncRequest() {',
               '    // a comment',
               '    if (g_debug_level > 0 && %s) {' % ('true' if defect == 'runtraced' else '(kind < kKindDlp || dlp_hub_ == nullptr)'),
               '        RunTraced(kind, ndm_session, id, cmd);',
               '    } else {', '        Run(kind, ndm_session, id, cmd);', '    }', '}']
        os.makedirs(os.path.join(src, 'Sources', 'ctr'), exist_ok=True)
        with open(os.path.join(src, 'Sources', 'ctr', 'uds_hle.cpp'), 'w') as f:
            f.write('\n'.join(uds) + '\n')
        with open(os.path.join(src, 'Includes', 'azl', 'dlp_lobby.hpp'), 'w') as f:       # (Poll without its first test, for 'poll_flag')
            f.write('\n'.join(lobby if defect != 'poll_flag' else
                              [l.replace('(flags2_ & (kF2External | kF2OnNet)) == kF2External || ', '') for l in lobby]) + '\n')
        real = globals()['demangle']
        globals()['demangle'] = lambda symbols, filt: {x: names.get(x, x) for x in symbols}
        try:
            g = Graph(d, src, 'stub')
        finally:
            globals()['demangle'] = real
        rep = TreeReport(g, None, 'selftest')
        rep.limits = {'hle': list(with_ceiling or HLE_CEILING.values()), 'log': BUDGET_LOG}
        out = []
        rep.run(out)
        return rep, out

    rep, out = run()
    by = {r.label: r for r in rep.rows}
    info = by['FKCL GetServerInfo']
    expect('the commands are split by IsInline: an inline row for GetServerInfo', info.group, 'inline')
    expect('... and one for the SRVR GetServerState', by['SRVR GetServerState'].group, 'inline')
    expect('... and the others are posted', [r.group for r in rep.rows if r.group == 'posted'], ['posted'])
    # chain of an inline command: RunDlp 32 + Serve 40 + Dispatch 24 + FkServerInfo 48 + ClientServerInfo 16 + NowMs 8 = 168
    expect('an inline chain is the sum of the frames down to the clock', info.chain[0], 168)
    expect('... and its total is the entry + the hook\'s frames + the chain', info.total[0], ASM_ENTRY + 16 + 48 + 168)
    expect('the Poll cut keeps PollClient and the virtual behind it out of an inline chain', 'ScanRead' in info.path[1], False)
    posted = [r for r in rep.rows if r.group == 'posted'][0]
    expect('a posted chain is RunDlp + Serve + Post + the kernel event call', posted.chain[0], 32 + 40 + 32 + 8)
    expect('the join wait: RunDlp + WaitForJoin + PollPending', by['FKCL StartFakeSession: the wait for the join (WaitForJoin > PollPending)'].chain[0], 32 + 40 + 8)
    alive = by['FKCL StartFakeSession: the 8 s cap (WaitForJoin > OnSessionClosed)']
    expect('the cap goes through Post: RunDlp + WaitForJoin + OnSessionClosed + Post + the kernel call (the lobby is not reached)',
           alive.chain[0], 32 + 40 + 32 + 32 + 8)
    dead = [r for r in rep.rows if r.group == 'dead']
    expect('dead-network rows exist for OnGameUdsStarting only (the end of a session is deferred): joined and no room', len(dead), 2)
    expect('the dead-network row goes through RunHere into the engine and the room client: 8 + 8 + 24 + 56 + 96',
           [r for r in dead if r.label.endswith('room joined (RunHere: the whole LobbyNet side down to the room client)')][0].chain[0], 8 + 8 + 24 + 56 + 96)
    nr = [r for r in dead if 'no room' in r.label][0]
    expect('... and with no room the room client is not reached: 8 + 8 + 24 + 56', nr.chain[0], 8 + 8 + 24 + 56)
    expect('dead-network rows are not gated', {r.gated for r in rep.rows if r.group == 'dead'}, {False})
    expect('the close paths are gated rows with a posted request', all(r.gated for r in rep.rows if r.group == 'closed'), True)
    expect('a healthy tree has no problem', rep.problems, [])
    for defect, needle in (('virtual', 'ScanRead'), ('engine', 'engine'), ('log', 'Log()'), ('runtraced', 'RunTraced'), ('closed_inline', 'OnSessionClosed'),
                           ('poll_flag', 'external-poll')):
        rep2, _ = run(defect)
        expect('proof 1 refuses a defect (%s)' % defect, any(needle in pr for pr in rep2.problems), True)
    # a ceiling that is too low refuses the gated rows and not the others
    rep3, _ = run(None, (100, 100))
    expect('a ceiling the chains do not fit is a problem for the gated rows', any('over the 100' in pr for pr in rep3.problems), True)
    expect('... and the dead-network rows (not gated) are not named in it', any('network thread dead' in pr for pr in rep3.problems), False)
    # a renamed function is an error, not a silent pass
    names['serve'] = 'azl::DlpHub::Renamed(azl::DlpKind)'
    rep4, _ = run()
    expect('a function that is gone is a problem (SPEC)', any(pr.startswith('SPEC') for pr in rep4.problems), True)
    shutil.rmtree(d)
    for line in bad:
        print('chain self-test (tree) FAILED: ' + line)
    print('chain self-test (tree): %s' % ('ok (%d checks)' % checks[0] if not bad else '%d failures' % len(bad)))
    return 1 if bad else 0


def selftest():
    return selftest_core() + selftest_tree()


def main():
    if '--selftest' in sys.argv:
        sys.exit(selftest())
    ap = argparse.ArgumentParser()
    ap.add_argument('ci_dir')
    ap.add_argument('--src', required=True, help='the tree the sources are read from (call-site text, the switch of DlpService)')
    ap.add_argument('--repo', default=None, help='the tree whose Tests/chain_sim/chain_sim.py gives the ceilings (default: --src)')
    ap.add_argument('--out', default=None)
    ap.add_argument('--kind', default='')
    ap.add_argument('--dis', default=None, help='objdump -d of the linked plugin (frames of the libc / libctru callees)')
    ap.add_argument('--json', default=None)
    ap.add_argument('--path-map', action='append', default=[], metavar='FROM=TO',
                    help='the .ci files name /src/Includes/..., the sources are in TO (a build in a container)')
    ap.add_argument('--gate', action='store_true', help='the short report of Tests/stack_usage.sh; needs --dis')
    args = ap.parse_args()
    path_map = [tuple(x.split('=', 1)) for x in args.path_map]

    filt = None
    for c in ('arm-none-eabi-c++filt', '/opt/devkitpro/devkitARM/bin/arm-none-eabi-c++filt', 'c++filt'):
        if shutil.which(c) or os.path.exists(c):
            filt = c
            break
    if args.gate and not args.dis:
        print('chain report FAILED: --gate needs --dis (the library frames are the machine code\'s)')
        sys.exit(1)
    g = Graph(args.ci_dir, args.src, filt, path_map)
    elf = None
    if args.dis and os.path.exists(args.dis):
        with open(args.dis, errors='replace') as f:
            elf = ElfModel(f.read())
    rep = TreeReport(g, elf, args.kind, args.repo or args.src)
    out = []
    rep.run(out, gate=args.gate)
    text = '\n'.join(out)
    print(text)
    if args.out and os.path.isdir(args.out):
        with open(os.path.join(args.out, 'report.txt'), 'w') as f:
            f.write(text + '\n')
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(rep.json(), f, indent=1)
    sys.exit(1 if rep.problems else 0)


if __name__ == '__main__':
    main()
