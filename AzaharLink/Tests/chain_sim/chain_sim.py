#!/usr/bin/env python3
"""The whole call chain of the svc hook, on a 512-byte game stack, with the REAL code of the plugin.

    python chain_sim.py <AzaharLink.elf>

The 0.1.15 plugin ran a printf chain of ~1.3 KB on the stack of whatever game thread made an IPC call, and the
srv-notification thread of Tri Force Heroes has 464 bytes below its call: the game crashed on exit
(hardware, 2026-10-10). Tests/stack_usage.sh budgets single frames and cannot see the libc functions below them;
this test runs the compiled plugin (the ELF the 3gx is made of) in the Unicorn ARM emulator:

  * the static initialisers of the plugin run (so every mutex, std::function and table has its real state), the
    capture ring is the real DebugInit() one, the sink of the log is the real LogFiles::Append behind a real
    std::function, the HLE front-end is a real UdsHle (its engine is not built: only the commands that do not reach it
    are run) and the game's Nintendo-SDK thread object is laid out like the smallest thread seen (a 512-byte stack inside a
    0x228-byte object, SP of the dispatcher at obj+0x1F8, a 16-byte wrapper frame and the 4-byte trampoline push);
  * every scenario enters azl_svc32_entry / azl_svc23_entry exactly like the per-site trampoline does and the test
    measures how deep the plugin went below the entry SP (the stack under the entry is canary-filled; the object
    header and the memory below the object are checked for changes), that the game's registers and flags came back,
    and what the plugin logged (compared with the printf formats of 0.1.15);
  * budgets: the pure pass-through path (any IPC call to a service nobody traces: srv:ReceiveNotification above all)
    must stay within 300 bytes below the entry SP at debug 0 AND debug 1; the other paths that run on a game thread
    (GetServiceHandle, APT, traced services, the fake sessions' open / close) must fit the 464 bytes of the smallest
    thread seen (they are rarer: a handful of lines per launch). The HLE proper (the dispatch of a fake session's
    command and what it calls) legitimately needs more: its numbers are printed and pinned to a ceiling, so a growth is
    noticed, and Tests/stack_usage.sh budgets its frames;
  * the printf family (vsnprintf, _vsnprintf_r, _svfprintf_r, snprintf, ...) must not run AT ALL on any path outside the
    HLE proper, at either level: a snprintf added to a helper stays inside the byte budget today and kills a game on a
    thread with less stack tomorrow;
  * one call of Log() / DebugEvent() with a format - which the HLE proper makes in every command that logs, on the
    game's UDS thread, at every debug level - must cost a few words at debug 0 (they cost 440 / 256 bytes in 0.1.15 and
    in the first cut of 0.1.15.1, the line buffer being in their own frame ahead of the test of the level).
The Download Play hub (phase 3, DESIGN-FASE3.md): the COMPILED DlpHub, DlpService and DlpLobby are built by the plugin's own constructor
in the emulator and driven through the hook with the headers the games send (scenario_hub_*). The emulator has one thread, so when the
game thread waits on a kernel event or sleeps, the other thread - the network thread, DlpHub::Tick - runs in its place (Sim.on_yield).
Two kinds of rig: a SPY LobbyNet (the adapter's vtable replaced by thunks the sim answers and writes down: the inline commands must
call nothing of it but the clock, and the engine is an unmapped address, so a touch fails loudly) for the commands, the waits, the
join, the closes and the commands that succeed in every state when nobody can serve them (scenario_hub_always_succeeds: Finalize,
StopScan, StopSession, EndAccepting answered with success, the hub object compared word by word before and after); and the REAL engine under the lobby, with a transport that drops the frames or queues them in a RoomClient, for
what a game thread pays when it gives the engine back itself (OnGameUdsStarting with no network thread). The depth of one function is
read with a window (Sim.window_begin). `--hub-only` runs these scenarios alone.
What this does not run: the observers' own code (Plugin::OnSleepEnter / OnHomeButton / NoteSleepEvent: stubbed here, the
last one with a check of what it is told and when) and an engine with a game's session up (the only engine the sim builds is the hub scenarios' one, under the lobby); Tests/callgraph_check.py
follows both statically. 0.1.16 adds: the APT observer of the sleep protocol (what it is told, and that the replies are
told BEFORE their call), and 60 failing replies in a row on a fake session (the throttle of a polling game thread: the
sleeps happen, the replies do not change).
"""
import os
import re
import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc, UcError)
from unicorn.arm_const import *

# ---- the game's thread: a 512-byte stack with 464 bytes free below the srv call (as measured on a console) -----------------
OBJ = 0x00800800                 # the SDK object: header 0x28 bytes, then the 512-byte stack
STACK_LO = OBJ + 0x28
STACK_TOP = OBJ + 0x228
DISPATCHER_SP = OBJ + 0x1F8      # SP in the watcher function when it calls the srv wrapper
WRAPPER_FRAME = 16               # the game's srv wrapper pushes four registers (r4-r6, lr)
TRAMPOLINE_PUSH = 4              # the per-site trampoline: push {lr}
ENTRY_SP = DISPATCHER_SP - WRAPPER_FRAME - TRAMPOLINE_PUSH   # SP at azl_svc32_entry
FREE_BELOW_DISPATCHER = DISPATCHER_SP - STACK_LO             # 464
HEADER = {off: 0xAAAA0000 + off for off in range(0, 0x28, 4)}   # ten distinct marks: the test only needs a header that must come back unchanged
ROOMY_LO = 0x00802000            # a thread with a big stack, for the HLE proper
ROOMY_TOP = 0x00804000
ROOMY_ENTRY_SP = ROOMY_TOP - 0x100
STUB_RET = 0x07F00010            # where the trampoline's `bl` returns to
PLUGIN_STACK = 0x07E00000        # a big stack for the setup calls (constructors, DebugInit)
PLUGIN_STACK_TOP = 0x07E40000
TLS = 0x08100000
HEAP = 0x09000000
HEAP_SIZE = 0x400000
REENT = 0x08280000
CANARY = 0xA5
BELOW = 0xEE
# What the stubbed kernel answers to the two questions the first-call lines ask (NoteCommand's "uds: command 0x.. is first called by thread N
# at priority 0x..", and the first command of each dlp service, "... carried by the room (thread N, priority 0x..)"): the thread id is printed in
# decimal, the priority in hex. Distinct numbers, so that a line which swaps them (or prints the handle the call was made with, as the
# unstubbed svc did: 4294934528 = 0xFFFF8000) cannot read as right.
SIM_THREAD_ID = 42
SIM_THREAD_PRIORITY = 0x30

# What the pure path WRITES below the entry SP (the canary of the emulator measures that): the hotfix's 108 / 176 less the
# 20 bytes of the game's wrapper and the trampoline
PURE_WRITTEN = {0: 88, 1: 156}
BUDGET_PURE = 300                # bytes below the entry SP, pure pass-through path, debug 0 and 1
# The HLE proper (what Run() reaches: Dispatch, NdmHle, NoteCommand's Log) is not held to a small thread's budget -
# Tests/stack_usage.sh budgets its frames - but its depth is pinned to a ceiling a little above today's (debug 0: 176 / 224,
# debug 1: 464). At debug 1 it was 1896 until 0.1.16: ~1.4 KB of that was newlib's vsnprintf under Log(), and the lines of the
# capture were built on this stack too; Log() / DebugEvent() now write the conversions of the plugin's formats themselves
# (azl/printf_lite.hpp) and the capture takes the raw words of a command, so the printf family does not run in the HLE
# proper at all (checked below: `hle_formatter_hits`).
HLE_CEILING = {0: 320, 1: 520}
# What the game's nwm::UDS threads pay today for a command that reaches the engine (DESIGN-FASE2.md sez. 7: ~1196 / 1260 bytes in the
# chain, the room client's queue included): the room a lobby give-back that runs on such a thread (DlpHub::OnGameUdsStarting with no
# network thread alive) has. The ceilings above are for the paths that have no engine.
UDS_ENGINE_CALL = {0: 1196, 1: 1260}
# What ONE call of Log() / DebugEvent() with a format costs the thread that makes it (the HLE proper calls them in every
# command that logs, at every debug level): at debug 0 they return at once and must cost a few words (they cost 440 / 256
# bytes in 0.1.15, the line buffer being in their own frame ahead of the test of the level); at debug 1 they were 1456 / 1272
# (newlib's printf chain) and are 264 / 280 since 0.1.16 (the plugin's own formatter, the frames of the sink included).
LOG_CALL_CEILING = {0: 48, 1: 320}
DEBUGEVENT_CALL_CEILING = {0: 48, 1: 320}
# The printf family of newlib: executed by none of the hook's own paths (see Sim.enter)
FORMATTERS = ('vsnprintf', '_vsnprintf_r', '_svfprintf_r', 'snprintf', 'sprintf', '_svfiprintf_r', '_vfiprintf_r', 'sniprintf')
BUDGET_THREAD = FREE_BELOW_DISPATCHER - WRAPPER_FRAME - TRAMPOLINE_PUSH   # 444: what is left under the entry SP
BUDGET_LOG = 340                 # the paths that write a line of the main log (through the real LogFiles::Append): 292 now

ARM_REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
            UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12]

failures = []
checks = [0]


def check(cond, what):
    checks[0] += 1
    if not cond:
        failures.append(what)
        print('  FAIL: ' + what)


class Elf:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.data = d
        assert d[:4] == b'\x7fELF' and d[4] == 1, 'a 32-bit ELF is expected'
        e_phoff, = struct.unpack_from('<I', d, 0x1C)
        e_shoff, = struct.unpack_from('<I', d, 0x20)
        e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x2A)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2E)
        self.segments = []
        for i in range(e_phnum):
            p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(
                '<8I', d, e_phoff + i * e_phentsize)
            if p_type == 1:
                self.segments.append((p_vaddr, p_off, p_filesz, p_memsz))
        secs = []
        for i in range(e_shnum):
            secs.append(struct.unpack_from('<10I', d, e_shoff + i * e_shentsize))
        self.symbols = {}
        for sh in secs:
            if sh[1] == 2:  # SHT_SYMTAB
                strtab = secs[sh[6]]
                for j in range(sh[5] // 16):
                    st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                        '<IIIBBH', d, sh[4] + j * 16)
                    end = d.index(b'\0', strtab[4] + st_name)
                    name = d[strtab[4] + st_name:end].decode('latin1')
                    if name and st_shndx != 0:
                        self.symbols.setdefault(name, st_value)
        # .init_array
        self.init_array = []
        for sh in secs:
            nm_end = d.index(b'\0', secs[e_shstrndx][4] + sh[0])
            nm = d[secs[e_shstrndx][4] + sh[0]:nm_end].decode()
            if nm == '.init_array':
                for k in range(sh[5] // 4):
                    self.init_array.append(struct.unpack_from('<I', d, sh[4] + 4 * k)[0])

    def sym(self, name):
        if name in self.symbols:
            return self.symbols[name]
        raise KeyError(name)

    def find(self, needle):
        hits = [n for n in self.symbols if needle in n]
        if len(hits) != 1:
            raise KeyError('%s: %d matches %s' % (needle, len(hits), hits[:5]))
        return self.symbols[hits[0]]


def p32(uc, addr, value):
    uc.mem_write(addr, struct.pack('<I', value & 0xFFFFFFFF))


def r32(uc, addr):
    return struct.unpack('<I', uc.mem_read(addr, 4))[0]


def cstr(uc, addr, limit=512):
    out = bytearray()
    while len(out) < limit:
        b = uc.mem_read(addr + len(out), 1)[0]
        if b == 0:
            break
        out.append(b)
    return out.decode('latin1')


class Sim:
    """One run of the plugin: a fresh emulator, the static initialisers run, the log on at `level`."""

    def __init__(self, elf, level, hle=False):
        self.elf = elf
        self.level = level
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        lo = min(s[0] for s in elf.segments) & ~0xFFF
        hi = (max(s[0] + s[3] for s in elf.segments) + 0xFFF) & ~0xFFF
        uc.mem_map(lo, hi - lo)
        for vaddr, off, filesz, memsz in elf.segments:
            uc.mem_write(vaddr, elf.data[off:off + filesz])
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        uc.mem_map(0x00800000, 0x4000)                  # the game's thread object and what is around it
        uc.mem_map(PLUGIN_STACK, PLUGIN_STACK_TOP - PLUGIN_STACK)
        uc.mem_map(TLS, 0x1000)
        uc.mem_map(HEAP, HEAP_SIZE)
        uc.mem_map(REENT, 0x1000)
        uc.mem_map(STUB_RET & ~0xFFF, 0x1000)
        uc.reg_write(UC_ARM_REG_C13_C0_3, TLS)
        self.heap_next = HEAP
        self.ticks = 0
        self.tick_step = 268112     # system ticks per read of the clock (1 ms); a scenario that needs finer time sets it
        self.apt_calls = []         # (kind, value, svc 0x32 calls made so far) told to the APT observer
        self.svc_log = []
        self.kernel = None          # callable(sim, number) -> sets the reply, returns r0
        self.min_sp = 0xFFFFFFFF
        self.min_pc = 0
        self.measuring = False
        self.log_lines = []         # every line handed to the sink (LogLine and Log())
        self.calls = []             # observer calls
        self.lines_at_call = []     # (observer, number of main-log lines already written when it was called)
        self.pc = 0
        self.next_handle = 0x00200000
        self.hooks = {}
        self.sink_thunk = None
        # The other thread (the Download Play hub scenarios): the emulator runs ONE thread, so when the game thread blocks - a wait on a
        # kernel event (svc 0x24) or a sleep (svc 0x0A) - and `on_yield` is set, the emulation stops there, `on_yield(sim)` runs (the
        # network thread's work: DlpHub::Tick, on the plugin's own stack, with the game thread's registers saved) and the game thread
        # goes on at the instruction after the svc.
        self.on_yield = None
        self.clear_event_fails = False
        self.yields = 0
        self.yield_pending = False
        self.win_entry = None       # the depth of ONE function (see window_begin)
        self.win_entry_sp = None
        self.win = None
        self.spy_calls = []         # LobbyNet methods the spy vtable saw on the GAME thread (a name per call)
        self.observed = []          # ... and the real ones that were watched (the clock; the adapter's when the engine is real)
        self.net_calls = []         # the same two lists for the calls the network thread (DlpHub::Tick) made
        self.spy_group = None       # the HeardGroup the spy's ScanRead reports (36 bytes)
        self.join_polls = 0         # ConnectPoll calls so far (the spy answers "joining" twice, then "failed")
        uc.hook_add(UC_HOOK_CODE, self.on_code)
        uc.hook_add(UC_HOOK_INTR, self.on_svc)
        # malloc & friends: a bump allocator (the allocator's own stack use is not what is being measured here)
        for name in ('malloc', '_malloc_r', 'calloc', '_calloc_r'):
            if name in elf.symbols:
                self.hooks[elf.sym(name) & ~1] = ('malloc', name)
        for name in ('free', '_free_r', 'cfree'):
            if name in elf.symbols:
                self.hooks[elf.sym(name) & ~1] = ('free', name)
        self.hooks[elf.sym('__getreent') & ~1] = ('getreent', None)
        # the printf family: any run of it on a small thread's stack is the defect of 0.1.15 (counted, see enter())
        self.formatter_hits = []
        self.hle_formatter_hits = []   # ... and in the HLE proper (since 0.1.16 it does not run there either: see scenario_hle_front)
        self.scenario = ''
        self.roomy = False
        for name in FORMATTERS:
            if name in elf.symbols:
                self.hooks.setdefault(elf.sym(name) & ~1, ('formatter', name))
        # the static initialisers of the plugin's own sources (the mutexes and the sink of the log are built there), like
        # __libc_init_array does; the framework's (threads, fonts, the OSD) are not the plugin's business
        inv = {}
        for n, a in elf.symbols.items():
            inv.setdefault(a & ~1, n)
        ours = [fn for fn in elf.init_array if '3azl' in inv.get(fn & ~1, '')]
        check(len(ours) >= 1, 'the plugin has a static initialiser (log.cpp: the mutexes)')
        for fn in ours:
            self.call(fn, [])
        p32(uc, elf.find('g_debug_level'), level)
        if level > 0:
            self.setup_sink()
            check((self.call(elf.sym('_ZN3azl9DebugInitEv'), []) & 0xFF) == 1, 'DebugInit() gave the capture a ring')
        if hle:
            self.setup_hle()
        self.log_lines.clear()          # (what the setup logged is not the scenario's)
        if level > 0:
            self.clear_capture()

    # ---- helpers ----
    def alloc(self, size, align=8):
        self.heap_next = (self.heap_next + align - 1) & ~(align - 1)
        p = self.heap_next
        self.heap_next += (size + 7) & ~7
        assert self.heap_next < HEAP + HEAP_SIZE
        return p

    def call(self, addr, args, stack_args=(), sp=None):
        """Runs a function of the plugin on the plugin's own big stack; returns r0."""
        uc = self.uc
        sp = (PLUGIN_STACK_TOP - 0x100) if sp is None else sp
        sp -= 4 * len(stack_args)
        sp &= ~7
        for i, v in enumerate(stack_args):
            p32(uc, sp + 4 * i, v)
        for i, v in enumerate(args[:4]):
            uc.reg_write(ARM_REGS[i], v & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, STUB_RET)
        was = self.measuring
        self.measuring = False
        try:
            uc.emu_start(addr, STUB_RET, count=5_000_000)
        except UcError as e:
            raise RuntimeError('emulation of %s failed: %s at pc=%08x (%s), lr=%08x, sp=%08x' % (
                self.where(addr), e, self.pc, self.where(self.pc), uc.reg_read(UC_ARM_REG_LR), uc.reg_read(UC_ARM_REG_SP)))
        finally:
            self.measuring = was
        return uc.reg_read(UC_ARM_REG_R0)

    def where(self, addr):
        best = None
        for n, a in self.elf.symbols.items():
            a &= ~1
            if a <= addr and (best is None or a > best[0]):
                best = (a, n)
        return '%s+0x%x' % (best[1], addr - best[0]) if best else '?'

    def setup_sink(self):
        """The log's sink as the plugin has it: a std::function around a real LogFiles."""
        uc, elf = self.uc, self.elf
        ctor = elf.find('_ZN3azl8LogFilesC1EPNS_9FileLayerENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEyRKNS_13LogFileLimitsE')
        obj = self.alloc(512)
        dirstr = self.alloc(32)
        p32(uc, dirstr, dirstr + 8)
        p32(uc, dirstr + 4, 1)
        uc.mem_write(dirstr + 8, b'/\0')
        limits = self.alloc(32)
        # (LogFileLimits on ARM32, 40 bytes: u64 log_rotate_bytes, u64 debug_rotate_bytes, int keep, u32 max_pending, u32 exit_capture_ms,
        #  u32 capture_every_ms, u32 capture_ring_high, 4 bytes of padding for the u64 alignment)
        uc.mem_write(limits, struct.pack('<QQiIIII4x', 1 << 20, 64 << 20, 7, 64 * 1024, 500, 2000, 160 * 1024))
        self.call(ctor, [obj, 0, dirstr, 0], stack_args=[0, 0, limits])   # (the u64 start_ms is the first stack argument, limits the next)
        # pending_ is the std::string at +92 (offsetof on ARM32 with -D__3DS__, where a Mutex is a 4-byte LightLock: +84 until 0.1.16, +76
        # before; LogFileLimits grew with exit_capture_ms, capture_every_ms and capture_ring_high); give it the
        # capacity Flush() leaves it with (reserve(16 KB)), so that the lines of the scenarios do not allocate, as in the steady
        # state of the plugin. The check below reads the std::string it finds there (a short string: its pointer is its own buffer):
        # a layout change that moves pending_ again is a failure here, not a test that quietly measures an allocation.
        pend = obj + 92
        check(r32(uc, pend) == pend + 8 and r32(uc, pend + 4) == 0, 'LogFiles::pending_ is where Append() expects it')
        buf = self.alloc(16 * 1024 + 1)
        p32(uc, pend, buf)
        p32(uc, pend + 4, 0)
        p32(uc, pend + 8, 16 * 1024)
        uc.mem_write(buf, b'\0')
        self.logfiles = obj
        self.pending = pend
        # the std::function: functor[0] = the LogFiles, manager != 0, invoker = a thunk (functor, const char*&&) ->
        # Append(logfiles, line)
        thunk = self.alloc(32, 4)
        append = elf.sym('_ZN3azl8LogFiles6AppendEPKc')
        code = struct.pack('<IIII', 0xE5900000, 0xE5911000, 0xE51FF004, append)  # ldr r0,[r0]; ldr r1,[r1]; ldr pc,[pc,#-4]
        uc.mem_write(thunk, code)
        self.sink_thunk = thunk
        sink = elf.find('g_sink')
        uc.mem_write(sink, b'\0' * 16)      # (libstdc++ on 32-bit ARM: _Any_data is 8 bytes, then the manager and the invoker)
        p32(uc, sink + 0, obj)
        p32(uc, sink + 8, 0x07000004)       # _M_manager (never called)
        p32(uc, sink + 12, thunk)           # _M_invoker
        self.hooks[thunk] = ('sink', None)

    def setup_hle(self):
        uc, elf = self.uc, self.elf
        size = 64 * 1024
        obj = self.alloc(size)
        self.call(elf.sym('_ZN3azl3ctr6UdsHleC1EPNS_9UdsEngineE'), [obj, 0])
        p32(uc, elf.find('g_hle'), obj)
        self.hle = obj

    # ---- hooks ----
    def on_code(self, uc, address, size, user):
        h = self.hooks.get(address)
        if h:
            kind = h[0]
            if kind == 'malloc':
                name = h[1]
                n = uc.reg_read(UC_ARM_REG_R1 if name.startswith('_') else UC_ARM_REG_R0)
                if name in ('calloc', '_calloc_r'):
                    n = uc.reg_read(UC_ARM_REG_R1 if name == 'calloc' else UC_ARM_REG_R2) * \
                        uc.reg_read(UC_ARM_REG_R0 if name == 'calloc' else UC_ARM_REG_R1)
                uc.reg_write(UC_ARM_REG_R0, self.alloc(n))
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                return
            if kind == 'free':
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                return
            if kind == 'getreent':
                uc.reg_write(UC_ARM_REG_R0, REENT)
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                return
            if kind == 'formatter':
                if self.measuring and not self.roomy:
                    self.formatter_hits.append((h[1], self.scenario))
                elif self.measuring:
                    self.hle_formatter_hits.append((h[1], self.scenario))
            if kind == 'sink':
                line_ptr = r32(uc, uc.reg_read(UC_ARM_REG_R1))
                self.log_lines.append(cstr(uc, line_ptr))
            if kind == 'aptobs':
                self.apt_calls.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1), self.svc_log.count(0x32)))
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                return
            if kind == 'observer':
                self.calls.append(h[1])
                self.lines_at_call.append((h[1], len(self.log_lines)))   # what the main log held when the observer ran
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                return
            if kind == 'spy':          # a LobbyNet method of the spy vtable: the answer is made here, the method does not run
                self.spy_calls.append(h[1])
                uc.reg_write(UC_ARM_REG_R0, self.spy_answer(h[1]) & 0xFFFFFFFF)
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                return
            if kind == 'watch':        # a real LobbyNet method that only gets written down (it runs as it is)
                self.observed.append(h[1])
            if kind == 'mac':          # the transport's get_mac: a std::array<u8, 6> through the hidden result pointer in r0
                uc.mem_write(uc.reg_read(UC_ARM_REG_R0), OWN_MAC)
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                return
        if self.measuring:
            sp = uc.reg_read(UC_ARM_REG_SP)
            if sp < self.min_sp:
                self.min_sp = sp
                self.min_pc = address
            if self.win is not None:
                self.window_step(address, sp)
            elif self.win_entry is not None and address == self.win_entry:
                self.win = [uc.reg_read(UC_ARM_REG_LR) & ~1, sp, sp]     # [where it returns to, SP at its entry, lowest SP so far]
        self.pc = address

    # ---- the depth of ONE function (the part of a long call that belongs to the hub) ----
    def window_begin(self, address):
        """From the next entry to the function at `address` until it returns, the lowest SP is kept apart: `window_depth(entry_sp)`."""
        self.win_entry = address & ~1
        self.win = None
        self.win_low = None

    def window_step(self, address, sp):
        ret, sp_entry, low = self.win
        if sp < low:
            self.win[2] = low = sp
        if address == ret and sp >= sp_entry:
            self.win_low = low
            self.win_entry_sp = sp_entry      # (a function that pushed nothing has win_low == win_entry_sp)
            self.win = None
            self.win_entry = None

    def window_depth(self, entry_sp):
        return None if self.win_low is None else entry_sp - self.win_low

    def spy_answer(self, name):
        """What the spy LobbyNet says. `this` is r0, the arguments r1.."""
        uc = self.uc
        r1, r2 = uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2)
        if name == 'ScanRead':
            if self.spy_group is not None and r1 == 0:
                uc.mem_write(r2, self.spy_group)
                return 1
            return 0
        if name in ('HostOpen', 'ScanOpen', 'ConnectBegin', 'Send'):
            return 1
        if name == 'ConnectPoll':
            self.join_polls += 1
            return 0 if self.join_polls <= 2 else 0xFFFFFFFF      # joining, joining, failed
        return 0

    def on_svc(self, uc, intno, user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        insn, = struct.unpack('<I', uc.mem_read(pc - 4, 4))
        num = insn & 0xFFFFFF
        self.svc_log.append(num)
        if num == 0x28:       # GetSystemTick
            self.ticks += self.tick_step
            uc.reg_write(UC_ARM_REG_R0, self.ticks & 0xFFFFFFFF)
            uc.reg_write(UC_ARM_REG_R1, self.ticks >> 32)
        elif num == 0x32 and self.kernel:   # SendSyncRequest
            uc.reg_write(UC_ARM_REG_R0, self.kernel(self, num) & 0xFFFFFFFF)
        elif num == 0x17:     # CreateEvent: result in r0, the handle in r1
            uc.reg_write(UC_ARM_REG_R1, self.next_handle)
            self.next_handle += 0x1000
            uc.reg_write(UC_ARM_REG_R0, 0)
        elif num == 0x19 and self.clear_event_fails:    # ClearEvent on a handle that now names something else: the fake session is found dead
            uc.reg_write(UC_ARM_REG_R0, 0xD8E007F7)
        elif num in (0x24, 0x0A) and self.on_yield is not None and self.measuring:    # the game thread waits or sleeps
            uc.reg_write(UC_ARM_REG_R0, 0)     # (signalled / woken)
            self.yield_pending = True
            uc.emu_stop()
        elif num == 0x35:     # GetProcessId: result in r0, the id in r1
            uc.reg_write(UC_ARM_REG_R1, 0x1D)
            uc.reg_write(UC_ARM_REG_R0, 0)
        elif num == 0x0B:     # GetThreadPriority: result in r0, the priority in r1
            uc.reg_write(UC_ARM_REG_R1, SIM_THREAD_PRIORITY)
            uc.reg_write(UC_ARM_REG_R0, 0)
        elif num == 0x37:     # GetThreadId: result in r0, the id in r1 (it was left to the default branch, which leaves r1 as the call put it: the handle)
            uc.reg_write(UC_ARM_REG_R1, SIM_THREAD_ID)
            uc.reg_write(UC_ARM_REG_R0, 0)
        else:
            uc.reg_write(UC_ARM_REG_R0, 0)

    # ---- the game's side ----
    def cmd(self):
        return TLS + 0x80

    def set_request(self, words):
        self.uc.mem_write(self.cmd(), b'\0' * 256)
        for i, w in enumerate(words):
            p32(self.uc, self.cmd() + 4 * i, w)

    def reset_game_stack(self, roomy):
        uc = self.uc
        if roomy:
            uc.mem_write(ROOMY_LO, bytes([CANARY]) * (ROOMY_TOP - ROOMY_LO))
            return
        uc.mem_write(OBJ - 0x400, bytes([BELOW]) * 0x400)
        uc.mem_write(STACK_LO, bytes([CANARY]) * (STACK_TOP - STACK_LO))
        for off, v in HEADER.items():
            p32(uc, OBJ + off, v)

    def run_game(self, begin):
        """Runs the game thread from `begin` until it returns to the trampoline. When it waits or sleeps and `on_yield` is set, the other
        thread runs in its place (the registers of the game thread are saved and put back)."""
        uc = self.uc
        pc = begin
        while True:
            uc.emu_start(pc, STUB_RET, count=5_000_000)
            if not self.yield_pending:
                return
            self.yield_pending = False
            self.yields += 1
            ctx = uc.context_save()
            was = self.measuring
            self.measuring = False
            try:
                self.on_yield(self)
            finally:
                self.measuring = was
            uc.context_restore(ctx)
            pc = uc.reg_read(UC_ARM_REG_PC)
            if uc.reg_read(UC_ARM_REG_CPSR) & 0x20:     # (Thumb)
                pc |= 1

    def enter(self, entry_symbol, handle, name, roomy=False):
        """The game's thread calls the hooked svc through the per-site trampoline. Returns (r0, depth, ok).
        `roomy`: a thread with a big stack (the HLE proper is run there: it needs more than the small threads have)."""
        uc = self.uc
        stack_lo, stack_top = (ROOMY_LO, ROOMY_TOP) if roomy else (STACK_LO, STACK_TOP)
        entry_sp = ROOMY_ENTRY_SP if roomy else ENTRY_SP
        self.reset_game_stack(roomy)
        self.scenario = name
        self.roomy = roomy
        hits_before = len(self.formatter_hits)
        hle_hits_before = len(self.hle_formatter_hits)
        marks = {UC_ARM_REG_R1: 0x11111111, UC_ARM_REG_R2: 0x22222222, UC_ARM_REG_R3: 0x33333333,
                 UC_ARM_REG_R4: 0x44444444, UC_ARM_REG_R5: 0x55555555, UC_ARM_REG_R6: 0x66666666,
                 UC_ARM_REG_R7: 0x77777777, UC_ARM_REG_R8: 0x88888888, UC_ARM_REG_R9: 0x99999999,
                 UC_ARM_REG_R10: 0xAAAAAAAA, UC_ARM_REG_R11: 0xBBBBBBBB, UC_ARM_REG_R12: 0xCCCCCCCC}
        for r, v in marks.items():
            uc.reg_write(r, v)
        uc.reg_write(UC_ARM_REG_R0, handle)
        uc.reg_write(UC_ARM_REG_SP, entry_sp)
        uc.reg_write(UC_ARM_REG_LR, STUB_RET)
        cpsr = uc.reg_read(UC_ARM_REG_CPSR)
        flags_in = (cpsr & 0x0FFFFFFF) | 0xA0000000   # N and C set
        uc.reg_write(UC_ARM_REG_CPSR, flags_in)
        self.min_sp = entry_sp
        self.measuring = True
        self.entry_sp = entry_sp
        try:
            self.run_game(self.elf.sym(entry_symbol))
        except UcError as e:
            self.measuring = False
            check(False, '%s: emulation error %s at pc=%08x (%s)' % (name, e, self.pc, self.where(self.pc)))
            return 0, 0, False
        self.measuring = False
        ok = True
        # nothing the hook runs outside the HLE proper may format: the printf family is not executed at all, at any level
        # (a path that stays inside the stack budget today and grows a snprintf tomorrow is caught here by what it runs)
        if len(self.formatter_hits) > hits_before:
            ok = False
            check(False, '%s: the printf family ran on a game thread (%s)' % (
                name, ', '.join(sorted({h[0] for h in self.formatter_hits[hits_before:]}))))
        # ... nor in the HLE proper since 0.1.16: the lines of a command are raw words for the writer thread and Log() writes the
        # conversions of the plugin's formats itself, so what a fake session's command costs does not include newlib's chain
        if len(self.hle_formatter_hits) > hle_hits_before:
            ok = False
            check(False, '%s: the printf family ran in the HLE proper (%s)' % (
                name, ', '.join(sorted({h[0] for h in self.hle_formatter_hits[hle_hits_before:]}))))
        for r, v in marks.items():
            if r in (UC_ARM_REG_R0,):
                continue
            if uc.reg_read(r) != v:
                ok = False
                check(False, '%s: register %d not preserved (%08x)' % (name, r - UC_ARM_REG_R0, uc.reg_read(r)))
        if uc.reg_read(UC_ARM_REG_SP) != entry_sp:
            ok = False
            check(False, '%s: SP not restored' % name)
        if (uc.reg_read(UC_ARM_REG_CPSR) & 0xF0000000) != (flags_in & 0xF0000000):
            ok = False
            check(False, '%s: condition flags not preserved' % name)
        # memory: the object header and what is below the object are untouched, the canary tells the deepest write
        if not roomy:
            for off, v in HEADER.items():
                if r32(uc, OBJ + off) != v:
                    ok = False
                    check(False, '%s: object word +0x%02x was changed (%08x)' % (name, off, r32(uc, OBJ + off)))
            if bytes(uc.mem_read(OBJ - 0x400, 0x400)) != bytes([BELOW]) * 0x400:
                ok = False
                check(False, '%s: memory below the game thread object was written' % name)
        stack = bytes(uc.mem_read(stack_lo, stack_top - stack_lo))
        low = next((i for i, b in enumerate(stack) if b != CANARY), None)
        depth_sp = entry_sp - self.min_sp
        depth_written = entry_sp - (stack_lo + low) if low is not None else 0
        # nothing written above the entry SP (the game's own frames): the bytes from entry_sp up are the canary too
        above = stack[entry_sp - stack_lo:]
        # (the sim does not build the game's frames: the area is canary, and the plugin must not touch it)
        if above != bytes([CANARY]) * len(above):
            ok = False
            check(False, '%s: the plugin wrote above its entry SP (into the game thread frames)' % name)
        depth = max(depth_sp, depth_written)
        self.last_sp_depth = depth_sp
        self.last_written = depth_written
        self.last_deepest = self.where(self.min_pc)
        return uc.reg_read(UC_ARM_REG_R0), depth, ok

    # ---- what the capture holds ----
    def ring_records(self):
        """The records in the capture ring (tail..head) as (kind, ms, payload bytes)."""
        uc, elf = self.uc, self.elf
        ring = r32(uc, elf.find('g_dbg_ring'))
        head = r32(uc, elf.find('g_dbg_head'))
        tail = r32(uc, elf.find('g_dbg_tail'))
        size = 512 * 1024
        out = []
        pos = tail
        while pos != head:
            def take(n):
                nonlocal pos
                b = bytearray()
                for _ in range(n):
                    b.append(uc.mem_read(ring + pos, 1)[0])
                    pos = (pos + 1) % size
                return bytes(b)
            hdr = take(16)
            stored, kind, direction, ms, src, dest, rsize, channel, pad = struct.unpack('<HBBIHHHBB', hdr)
            out.append((kind, ms, take(stored)))
        return out

    def capture_lines(self):
        """The text of every record, as DebugFormat() writes it (without the time column)."""
        lines = []
        for kind, ms, payload in self.ring_records():
            if kind == 1:
                lines.append(payload.decode('latin1'))
            elif kind == 3:
                h, c, a, r = struct.unpack('<4I', payload)
                lines.append('ipc h=0x%x cmd=0x%08x arg=0x%x -> 0x%08x' % (h, c, a, r))
            elif kind == 4:
                c, a, r = struct.unpack_from('<3I', payload)
                name = payload[12:].split(b'\0')[0].decode('latin1')
                lines.append('svc REAL %s cmd 0x%04x arg 0x%x -> 0x%08x' % (name, c, a, r))
            elif kind == 5:      # the diet: the count of a kind of IPC call (handle, command, last arg, result, count, first ms, last ms)
                h, c, a, r, n, first, last = struct.unpack('<7I', payload)
                lines.append('ipc h=0x%x cmd=0x%08x arg=0x%x -> 0x%08x x%d' % (h, c, a, r, n))
            elif kind == 6:      # the raw words of a UDS / ndm command of the HLE proper: the writer thread makes the line
                tag, rw, pw, text_len, a, b, c = struct.unpack_from('<4B3I', payload)
                at = 16
                text = payload[at:at + text_len].decode('latin1')
                at += text_len
                req = struct.unpack_from('<%dI' % rw, payload, at)
                rep = struct.unpack_from('<%dI' % pw, payload, at + 4 * rw)
                head = {0: 'uds cmd 0x%02x' % a, 1: 'ndm cmd 0x%02x' % a,
                        2: 'uds PullPacket bind %d: %d bytes from node %d' % (a, b, c), 3: text}[tag]
                lines.append(head + ' req' + ''.join(' %08x' % w for w in req) + ' -> rep' + ''.join(' %08x' % w for w in rep))
            elif kind == 7:      # a hole in a lane (ring full, lock busy, first ms, last ms)
                full, busy, first, last = struct.unpack('<4I', payload)
                lines.append('LOST %d records (ring full %d, lock busy %d)' % (full + busy, full, busy))
            else:
                lines.append('<record kind %d>' % kind)
        return lines

    def clear_capture(self):
        uc, elf = self.uc, self.elf
        p32(uc, elf.find('g_dbg_tail'), r32(uc, elf.find('g_dbg_head')))
        p32(uc, elf.find('g_dbg_used'), 0)


def srv_reply(words):
    """A kernel that answers every SendSyncRequest with these command buffer words."""
    def kernel(sim, num):
        for i, w in enumerate(words):
            p32(sim.uc, sim.cmd() + 4 * i, w)
        return 0
    return kernel


results = []   # (scenario, level, depth, budget, kind)


def record(scenario, level, depth, budget, kind):
    results.append((scenario, level, depth, budget, kind))
    check(depth <= budget, '%s at debug %d: used %d bytes below the entry SP, budget %d' % (scenario, level, depth, budget))
    if kind == 'hub':      # a dlp command of the hub: run on the 512-byte thread, so it must also fit what is left of the smallest thread seen
        check(depth <= BUDGET_THREAD, '%s at debug %d: %d bytes do not fit the %d below the entry SP of the smallest thread seen' % (
            scenario, level, depth, BUDGET_THREAD))


def name12(s):
    return s.encode().ljust(8, b'\0')[:8]


# =================================================================================================================
# scenarios
# =================================================================================================================
def scenario_pure(elf, level):
    """srv:ReceiveNotification on the dispatcher thread: the call that crashed Tri Force Heroes."""
    sim = Sim(elf, level, hle=True)
    sim.kernel = srv_reply([0x000B0040, 0, 0x100])           # the reply: result 0, notification id 0x100
    sim.set_request([0x000B0000])
    r0, depth, ok = sim.enter('azl_svc32_entry', 0x14801F, 'pure')
    check(r0 == 0, 'pure pass-through: the result of the svc comes back in r0')
    check(r32(sim.uc, sim.cmd() + 8) == 0x100, 'pure pass-through: the reply words are the kernel\'s')
    check(sim.svc_log.count(0x32) == 1, 'pure pass-through: exactly one svc 0x32')
    record('pass-through (srv ReceiveNotification)', level, depth, BUDGET_PURE, 'pure')
    # The same call as the emulator's canary sees it (the lowest byte of the thread's stack that was WRITTEN, which is what
    # Tests of the TFH object measure on Azahar): a frame that is reserved but not written does not show there, and a 64-bit
    # argument passed on the stack through the hot frame (AccountCall, 0.1.16 first cut) added 12 bytes to it at debug 0. The
    # numbers are the hotfix's canary (108 / 176 with the game's wrapper and the trampoline, 20 bytes) less those 20.
    check(sim.last_written <= PURE_WRITTEN[level],
          'pass-through at debug %d: %d bytes written below the entry SP, the hotfix wrote %d' % (level, sim.last_written, PURE_WRITTEN[level]))
    print('  pass-through (srv ReceiveNotification) at debug %d: %d bytes written, SP down to %d' % (level, sim.last_written, sim.last_sp_depth))
    if level > 0:
        lines = sim.capture_lines()
        check(lines == ['ipc h=0x14801f cmd=0x000b0000 arg=0x0 -> 0x00000000'],
              'pure pass-through at debug 1: one ipc line, the 0.1.15 text (%r)' % (lines,))
        check(sim.log_lines == [], 'pure pass-through leaves the main log alone')
    else:
        check(r32(sim.uc, elf.find('g_dbg_ring')) == 0, 'debug 0: no capture ring')
    # other results: an error from the kernel and a request with arguments
    sim.kernel = lambda s, n: 0xD8E007F7
    sim.set_request([0x00170042, 0xDEADBEEF])
    sim.clear_capture()
    r0, depth2, ok = sim.enter('azl_svc32_entry', 0x1B002A, 'pure error')
    check(r0 == 0xD8E007F7, 'pure pass-through: an error result comes back')
    record('pass-through (kernel error)', level, depth2, BUDGET_PURE, 'pure')
    if level > 0:
        check(sim.capture_lines() == ['ipc h=0x1b002a cmd=0x00170042 arg=0xdeadbeef -> 0xd8e007f7'],
              'pure pass-through at debug 1: an error result is logged as the result (%r)' % (sim.capture_lines(),))
    # CloseHandle through azl_svc23_entry
    sim.kernel = None
    r0, depth3, ok = sim.enter('azl_svc23_entry', 0x1B002A, 'close')
    record('CloseHandle (azl_hook_CloseHandle)', level, depth3, BUDGET_PURE, 'pure')


def scenario_diet(elf, level):
    """The diet and the lanes in the compiled code, on the dispatcher thread of Tri Force Heroes: the calls that are only counted,
    the call that closes a count, and the call that writes the marker of a hole must all fit what the plain pass-through fits."""
    if level == 0:
        return
    sim = Sim(elf, level, hle=True)
    sim.kernel = srv_reply([0x000B0040, 0, 0x100])
    worst = 0
    for i in range(6):
        sim.set_request([0x000B0000, 0x1000 + i])
        r0, depth, ok = sim.enter('azl_svc32_entry', 0x14801F, 'diet call %d' % i)
        worst = max(worst, depth)
        check(r0 == 0, 'diet: the result of the svc is not touched by the capture')
    whole = ['ipc h=0x14801f cmd=0x000b0000 arg=0x%x -> 0x00000000' % (0x1000 + i) for i in range(3)]
    check(sim.capture_lines() == whole, 'diet: six calls of one kind in a second: the first three whole, the rest only counted (%r)' %
          (sim.capture_lines(),))
    record('pass-through, the calls the diet only counts', level, worst, BUDGET_PURE, 'pure')
    # the second is over: the next call only hands the count of the others over (it pushes no record for it: the call that
    # does that must not be deeper than any other), the writer thread writes the line
    sim.ticks += 1200 * 268112
    sim.set_request([0x000B0000, 0x2000])
    r0, depth, ok = sim.enter('azl_svc32_entry', 0x14801F, 'diet call that finds its second over')
    record('pass-through, the call that finds the second over', level, depth, BUDGET_PURE, 'pure')
    check(depth <= worst + 8, 'diet: a call that hands a count over is not deeper than the others (%d against %d)' % (depth, worst))
    lines = sim.capture_lines()
    check(lines == whole + ['ipc h=0x14801f cmd=0x000b0000 arg=0x2000 -> 0x00000000'],
          'diet: the call of the new second is written, the count of the old one is not (yet) (%r)' % (lines,))
    sim.call(elf.find('DebugFlushAggregates'), [0])      # the writer's turn
    lines = sim.capture_lines()
    check(lines == whole + ['ipc h=0x14801f cmd=0x000b0000 arg=0x2000 -> 0x00000000',
                            'ipc h=0x14801f cmd=0x000b0000 arg=0x1005 -> 0x00000000 x3'],
          'diet: the writer writes the count (%r)' % (lines,))
    # a kernel error is never counted
    sim.kernel = lambda s, n: 0xD8E007F7
    sim.clear_capture()
    for i in range(5):
        sim.set_request([0x00170042, i])
        sim.enter('azl_svc32_entry', 0x14801F, 'failing call %d' % i)
    check(len(sim.capture_lines()) == 5 and all(l.endswith('-> 0xd8e007f7') for l in sim.capture_lines()),
          'diet: five failing calls of one kind are five whole lines (%r)' % (sim.capture_lines(),))
    # a hole: the heavy lane is full (the ring's used count is set to the limit), the call is dropped and counted; when there
    # is room again the next call is preceded by the marker
    uc = sim.uc
    sim.kernel = srv_reply([0x000B0040, 0, 0x100])
    sim.clear_capture()
    p32(uc, elf.find('g_dbg_used'), 480 * 1024 - 8)
    sim.set_request([0x000C0000, 7])
    sim.enter('azl_svc32_entry', 0x14801F, 'call into a full lane')
    check(r32(uc, elf.find('g_dbg_used')) == 480 * 1024 - 8, 'lanes: a record the heavy lane has no room for is not written')
    p32(uc, elf.find('g_dbg_tail'), r32(uc, elf.find('g_dbg_head')))
    p32(uc, elf.find('g_dbg_used'), 0)
    sim.set_request([0x000C0000, 8])
    r0, depth, ok = sim.enter('azl_svc32_entry', 0x14801F, 'call that writes the marker of a hole')
    record('pass-through, the call that writes the marker of a hole', level, depth, BUDGET_PURE, 'pure')
    # (the emulator's canary of Tri Force Heroes' thread once met this call and read 184 bytes against the hotfix's 176: the
    # marker is written word by word from registers now, and the path is no deeper than a plain call)
    check(depth <= worst, 'lanes: the call that writes the marker of a hole is not deeper than the others (%d against %d)' % (depth, worst))
    check(sim.last_written <= PURE_WRITTEN[level],
          'lanes: the marker path writes %d bytes below the entry SP, the hotfix wrote %d' % (sim.last_written, PURE_WRITTEN[level]))
    lines = sim.capture_lines()
    check(lines == ['LOST 1 records (ring full 1, lock busy 0)', 'ipc h=0x14801f cmd=0x000c0000 arg=0x8 -> 0x00000000'],
          'lanes: the marker of the hole, then the call (%r)' % (lines,))
    # records that other threads gave up because the ring's lock was busy leave a count behind (no lock was held to write it); the
    # next call that gets the lock folds it in - FoldMissesLocked is one more frame on this path - and writes the marker
    p32(uc, elf.find('g_dbg_missE') + 4, 3)            # lane 1 = heavy
    p32(uc, elf.find('g_dbg_miss_firstE') + 4, 100)
    p32(uc, elf.find('g_dbg_miss_lastE') + 4, 120)
    p32(uc, elf.find('g_dbg_miss_anyE'), 1)
    sim.clear_capture()
    sim.set_request([0x000C0000, 9])
    r0, depth, ok = sim.enter('azl_svc32_entry', 0x14801F, 'call that folds in the records given up for the lock')
    record('pass-through, the call that folds in the records given up for the lock', level, depth, BUDGET_PURE, 'pure')
    check(depth <= worst + 16, 'lanes: the call that folds in the given-up records is one small frame deeper at the most (%d against %d)' %
          (depth, worst))
    check(sim.last_written <= PURE_WRITTEN[level],
          'lanes: the fold path writes %d bytes below the entry SP, the hotfix wrote %d' % (sim.last_written, PURE_WRITTEN[level]))
    lines = sim.capture_lines()
    check(lines == ['LOST 3 records (ring full 0, lock busy 3)', 'ipc h=0x14801f cmd=0x000c0000 arg=0x9 -> 0x00000000'],
          'lanes: the marker of the records given up for the lock, then the call (%r)' % (lines,))


def scenario_getservice(elf, level):
    sim = Sim(elf, level, hle=True)
    # srv:GetServiceHandle 'fs:USER' -> 0, handle 0x1b002a: a real service nobody traces
    sim.kernel = srv_reply([0x00050100 | 0x40, 0, 0, 0x1B002A])
    sim.set_request([0x00050100] + list(struct.unpack('<2I', name12('fs:USER'))) + [7, 0])
    r0, depth, ok = sim.enter('azl_svc32_entry', 0x14801F, 'getservice')
    record('srv GetServiceHandle (a real service)', level, depth, BUDGET_LOG, 'log')
    if level > 0:
        check(sim.log_lines == ["srv: the game opened the REAL service 'fs:USER' -> 0x00000000 (handle 0x1b002a)"],
              'GetServiceHandle: main log line (%r)' % (sim.log_lines,))
        check(sim.capture_lines() == ["srv GetServiceHandle 'fs:USER' -> 0x00000000 (handle 0x1b002a)"],
              'GetServiceHandle: capture line (%r)' % (sim.capture_lines(),))
    else:
        check(sim.log_lines == [], 'debug 0: GetServiceHandle logs nothing')
    return sim


def scenario_traced(elf, level):
    sim = Sim(elf, level, hle=True)
    sim.kernel = srv_reply([0x00050100 | 0x40, 0, 0, 0x1C0031])
    sim.set_request([0x00050100] + list(struct.unpack('<2I', name12('ac:u'))) + [4, 0])
    sim.enter('azl_svc32_entry', 0x14801F, 'getservice ac:u')
    sim.log_lines.clear()
    sim.clear_capture() if level > 0 else None
    # a command of the traced session: ac:u 0x0029 with arg 0x1234 -> result 0
    sim.kernel = srv_reply([0x00290040, 0xC8A01234])
    sim.set_request([0x00290000 | 0x42, 0x1234])
    r0, depth, ok = sim.enter('azl_svc32_entry', 0x1C0031, 'traced command')
    record('a command of a traced real service (ac:u)', level, depth, BUDGET_LOG, 'log')
    if level > 0:
        check(sim.log_lines == ['svc: REAL ac:u command 0x0029 (arg 0x1234) -> 0xc8a01234'],
              'traced command: main log line (%r)' % (sim.log_lines,))
        check(sim.capture_lines() == ['svc REAL ac:u cmd 0x0029 arg 0x1234 -> 0xc8a01234'],
              'traced command: capture line (%r)' % (sim.capture_lines(),))
    else:
        check(sim.log_lines == [], 'debug 0: a traced command logs nothing')
        check(sim.svc_log.count(0x32) == 2, 'debug 0: the traced command is still passed on')


def scenario_apt(elf, level):
    sim = Sim(elf, level, hle=True)
    uc = sim.uc
    # the observers: two stub functions that only record that they were called (bx lr is the hook's `kind` handler)
    stub = sim.alloc(64, 4)
    for i, n in enumerate(('sleep', 'home')):
        addr = stub + 16 * i
        uc.mem_write(addr, struct.pack('<I', 0xE12FFF1E))   # bx lr
        sim.hooks[addr] = ('observer', n)
    uc.mem_write(stub + 32, struct.pack('<I', 0xE12FFF1E))
    sim.hooks[stub + 32] = ('aptobs', 'apt')                 # the observer of the whole sleep protocol (told kind, value)
    p32(uc, elf.find('g_sleep_observer'), stub)
    p32(uc, elf.find('g_home_observer'), stub + 16)
    p32(uc, elf.find('g_apt_observer'), stub + 32)
    sim.kernel = srv_reply([0x00050100 | 0x40, 0, 0, 0x1D0041])
    sim.set_request([0x00050100] + list(struct.unpack('<2I', name12('APT:U'))) + [5, 0])
    sim.enter('azl_svc32_entry', 0x14801F, 'getservice APT:U')
    sim.log_lines.clear()
    if level > 0:
        sim.clear_capture()
    apt = 0x1D0041
    worst = 0
    expect_log = []
    expect_cap = []
    # InquireNotification: id 0 (nothing: not logged), 1 (home button), 5 (sleep begins), 6 (wake-up)
    sim.apt_calls.clear()
    told = []
    for ident, observers in ((0, []), (1, ['home']), (5, ['sleep']), (6, ['sleep']), (3, [])):
        sim.calls.clear()
        sim.kernel = srv_reply([0x000B0080, 0, ident])
        sim.set_request([0x000B0040, 0x300])
        n32 = sim.svc_log.count(0x32)
        r0, depth, ok = sim.enter('azl_svc32_entry', apt, 'apt inquire %d' % ident)
        worst = max(worst, depth)
        if ident != 0:
            told.append((1, ident, n32 + 1))                   # a notification is told after the call returned
        if ident == 1 and level > 0:
            check(sim.calls == ['home'], 'APT: the HOME notification reaches the home observer at debug 1 (%r)' % (sim.calls,))
            # the flush it asks for must have the notification's own line: the observer runs after the lines are written
            at = [n for who, n in sim.lines_at_call if who == 'home']
            check(len(at) == 1 and at[0] >= 1 and sim.log_lines[at[0] - 1] == 'apt: notification 1 (home button) for app 0x300 -> 0x00000000',
                  'APT: the HOME observer runs after the line of the HOME notification (%r, %r)' % (at, sim.log_lines[-2:]))
        elif ident == 1:
            check(sim.calls == [], 'APT: at debug 0 the HOME observer is not called')
        elif ident in (5, 6):
            check(sim.calls == ['sleep'], 'APT: notification %d reaches the sleep observer at every level (%r)' % (ident, sim.calls))
        if level > 0 and ident != 0:
            names = {1: 'home button', 3: 'sleep query', 5: 'sleep begins', 6: 'wake-up'}
            expect_log.append('apt: notification %d (%s) for app 0x300 -> 0x00000000' % (ident, names[ident]))
            expect_cap.append('apt InquireNotification app 0x300 -> %d (%s)' % (ident, names[ident]))
    # ReplySleepQuery (answer 1) and ReplySleepNotificationComplete
    sim.kernel = srv_reply([0x003E0040, 0])
    sim.set_request([0x003E0080, 0x300, 1])
    n32 = sim.svc_log.count(0x32)
    r0, depth, ok = sim.enter('azl_svc32_entry', apt, 'apt reply sleep query')
    worst = max(worst, depth)
    told.append((2, 1, n32))                                   # the game's answer is told BEFORE its call
    sim.kernel = srv_reply([0x003F0040, 0])
    sim.set_request([0x003F0040, 0x300])
    n32 = sim.svc_log.count(0x32)
    r0, depth, ok = sim.enter('azl_svc32_entry', apt, 'apt sleep complete')
    worst = max(worst, depth)
    told.append((3, 0, n32))                                   # ... and so is its acknowledgement of the sleep (it may not return)
    if level > 0:
        check(sim.apt_calls == told, 'APT: the trail observer is told %r (it was told %r)' % (told, sim.apt_calls))
    else:
        check(sim.apt_calls == [], 'APT: at debug 0 the trail observer is not called (%r)' % (sim.apt_calls,))
    if level > 0:
        expect_log += ['apt: ReplySleepQuery app 0x300: answer 1 (accept) -> 0x00000000',
                       'apt: ReplySleepNotificationComplete app 0x300 -> 0x00000000']
        expect_cap += ['apt ReplySleepQuery app 0x300 answer 1', 'apt ReplySleepNotificationComplete app 0x300 answer 0']
        check(sim.log_lines == expect_log, 'APT: main log lines (%r vs %r)' % (sim.log_lines, expect_log))
        check(sim.capture_lines() == expect_cap, 'APT: capture lines (%r vs %r)' % (sim.capture_lines(), expect_cap))
    record('APT notifications and the sleep replies', level, worst, BUDGET_LOG, 'log')


def scenario_hle_front(elf, level):
    """A game opens and closes the fake sessions: the lines and the stack."""
    sim = Sim(elf, level, hle=True)
    sim.log_lines.clear()
    if level > 0:
        sim.clear_capture()
    # GetServiceHandle 'ndm:u' / 'nwm::UDS': answered by UdsHle (no svc 0x32 at all)
    handles = {}
    worst = 0
    for svc in ('ndm:u', 'nwm::UDS'):
        sim.kernel = lambda s, n: (_ for _ in ()).throw(AssertionError('the real kernel was called for ' + svc))
        words = struct.unpack('<2I', name12(svc))
        sim.set_request([0x00050100, words[0], words[1], len(svc), 0])
        before = sim.svc_log.count(0x32)
        r0, depth, ok = sim.enter('azl_svc32_entry', 0x14801F, 'open ' + svc)
        worst = max(worst, depth)
        check(sim.svc_log.count(0x32) == before, 'the fake %s session is answered without the kernel' % svc)
        handles[svc] = r32(sim.uc, sim.cmd() + 12)
    record('opening a fake session (UdsHle::InterceptGetServiceHandle)', level, worst, BUDGET_LOG, 'log')
    if level > 0:
        opened = [l for l in sim.log_lines if l.startswith('uds: game opened')]
        check(opened == ['uds: game opened ndm:u -> fake session 0x%x (ndm session 0)' % handles['ndm:u'],
                         'uds: game opened nwm::UDS -> fake session 0x%x' % handles['nwm::UDS']],
              'fake session open lines (%r)' % (opened,))
        cap = [l for l in sim.capture_lines() if l.startswith('uds game opened')]
        check(cap == ['uds game opened ndm:u -> fake session 0x%x (ndm session 0)' % handles['ndm:u'],
                      'uds game opened nwm::UDS -> fake session 0x%x' % handles['nwm::UDS']],
              'fake session open capture lines (%r)' % (cap,))
    # a command of the fake ndm:u session (RunTraced -> Run -> RunNdm -> NdmHle::Dispatch -> NoteCommand): the HLE proper
    sim.log_lines.clear()
    if level > 0:
        sim.clear_capture()
    sim.kernel = lambda s, n: (_ for _ in ()).throw(AssertionError('the real kernel was called'))
    sim.set_request([0x00140040, 0x0F])    # ndm command 0x14 with one word (as the tests of the ndm front-end do)
    r0, depth, ok = sim.enter('azl_svc32_entry', handles['ndm:u'], 'ndm command', roomy=True)
    record('HLE: a command of the fake ndm:u (RunTraced, NdmHle, NoteCommand)', level, depth, HLE_CEILING[level], 'hle')
    if level > 0:
        check(sim.capture_lines() == ['ndm cmd 0x14 req 0000000f -> rep 00000000'],
              'HLE: the capture line of an ndm command is the raw words made into the 0.1.15 text (%r)' % (sim.capture_lines(),))
        check(sim.log_lines == ['ndm: command 0x14 -> 0x00000000 (first call)'],
              'HLE: the log line of the first call of a command, written without newlib (%r)' % (sim.log_lines,))
    # a command of the fake nwm::UDS session that needs no engine (0x23 allowed-channel mask)
    sim.set_request([0x00230040, 0])
    sim.log_lines.clear()
    if level > 0:
        sim.clear_capture()
    r0, depth, ok = sim.enter('azl_svc32_entry', handles['nwm::UDS'], 'uds command 0x23', roomy=True)
    check(r32(sim.uc, sim.cmd() + 8) == 0x07FF, 'the fake nwm::UDS answers command 0x23 with the channel mask')
    if level > 0:
        check(sim.capture_lines() == ['uds cmd 0x23 req 00000000 -> rep 00000000 000007ff'],
              'HLE: the capture line of a nwm::UDS command (%r)' % (sim.capture_lines(),))
        # (the first call of a command also says which thread of the game made it, at what priority: the thread id and the priority
        #  are what the simulator's stubbed kernel answers)
        check(len(sim.log_lines) == 2 and sim.log_lines[0].startswith('uds: command 0x23 is first called by thread ') and
              ' at priority 0x' in sim.log_lines[0] and sim.log_lines[1] == 'uds: command 0x23 -> 0x00000000 (first call)',
              'HLE: the log lines (%r)' % (sim.log_lines,))
        # ... and the numbers are the kernel's (the stub answers SIM_THREAD_ID and SIM_THREAD_PRIORITY: GetThreadId was not stubbed once,
        # and the line printed the handle the call was made with, 4294934528)
        check(sim.log_lines[0] == 'uds: command 0x23 is first called by thread %d at priority 0x%02x' % (SIM_THREAD_ID, SIM_THREAD_PRIORITY),
              'HLE: the first-caller line carries the thread id and the priority the kernel answered (%r)' % (sim.log_lines[:1],))
    record('HLE: UdsHle::Dispatch of a command that needs no engine', level, depth, HLE_CEILING[level], 'hle')
    # closing them (azl_svc23_entry -> azl_hook_CloseHandle -> NoteHandleClosed)
    sim.log_lines.clear()
    if level > 0:
        sim.clear_capture()
    worst = 0
    for svc in ('ndm:u', 'nwm::UDS'):
        r0, depth, ok = sim.enter('azl_svc23_entry', handles[svc], 'close ' + svc)
        worst = max(worst, depth)
    record('closing a fake session (UdsHle::NoteHandleClosed)', level, worst, BUDGET_LOG, 'log')
    if level > 0:
        check(sim.log_lines == ['uds: game closed its fake ndm:u session 0x%x (ndm session 0)' % handles['ndm:u'],
                                'uds: game closed its fake nwm::UDS session 0x%x' % handles['nwm::UDS']],
              'fake session close lines (%r)' % (sim.log_lines,))
        check(sim.capture_lines() == ['uds game closed its fake ndm:u session 0x%x (ndm session 0)' % handles['ndm:u'],
                                      'uds game closed its fake nwm::UDS session 0x%x' % handles['nwm::UDS']],
              'fake session close capture lines (%r)' % (sim.capture_lines(),))


def scenario_throttle(elf, level):
    """A game thread that keeps calling a command that fails (here: an unknown id, 0xD900182F, which needs no engine): the
    first 50 replies in 100 ms are answered at once, each further one is preceded by svcSleepThread(0.5 ms); the reply is
    the same word; a pause in the polling (more than 100 ms) starts a new window."""
    sim = Sim(elf, level, hle=True)
    sim.tick_step = 268          # 1 us per read of the clock: 60 calls are a fraction of a window
    sim.kernel = lambda s, n: (_ for _ in ()).throw(AssertionError('the real kernel was called'))
    words = struct.unpack('<2I', name12('nwm::UDS'))
    sim.set_request([0x00050100, words[0], words[1], 8, 0])
    sim.enter('azl_svc32_entry', 0x14801F, 'open nwm::UDS', roomy=True)
    uds = r32(sim.uc, sim.cmd() + 12)
    sim.log_lines.clear()
    if level > 0:
        sim.clear_capture()
    sleeps_before = sim.svc_log.count(0x0A)
    worst = 0
    for i in range(60):
        sim.set_request([0x00FF0040, 0])
        r0, depth, ok = sim.enter('azl_svc32_entry', uds, 'failing poll %d' % i, roomy=True)
        worst = max(worst, depth)
        check(r32(sim.uc, sim.cmd() + 4) == 0xD900182F and r32(sim.uc, sim.cmd()) == 0x00000040,
              'throttle: the reply of call %d is the unchanged unknown-command answer' % i)
    sleeps = sim.svc_log.count(0x0A) - sleeps_before
    check(sleeps == 10, 'throttle: 60 failing replies in a row: the 10 past the 50th waited (%d)' % sleeps)
    record('HLE: 60 failing replies in a row (the throttle past the 50th)', level, worst, HLE_CEILING[level], 'hle')
    said = [l for l in sim.log_lines if l.startswith('uds: a game thread keeps calling commands that fail')]
    if level > 0:
        check(len(said) == 1 and said[0].endswith('(thread priority 0x30)'),
              'throttle: said once, with the thread priority (%r)' % (said,))
    else:
        check(said == [], 'throttle: nothing is logged at debug 0')
    # a successful reply never waits: GetConnectionStatus on a service that was never initialised answers an error, so use the
    # command that always succeeds without an engine, the allowed-channel mask (0x23)
    before = sim.svc_log.count(0x0A)
    for i in range(80):
        sim.set_request([0x00230040, 0])
        sim.enter('azl_svc32_entry', uds, 'successful call %d' % i, roomy=True)
        check(r32(sim.uc, sim.cmd() + 4) == 0, 'throttle: command 0x23 succeeds')
    check(sim.svc_log.count(0x0A) == before, 'throttle: successful replies never wait')
    # the window: 150 ms later the failing replies are free again
    sim.ticks += 150 * 268112
    before = sim.svc_log.count(0x0A)
    for i in range(40):
        sim.set_request([0x00FF0040, 0])
        sim.enter('azl_svc32_entry', uds, 'failing poll, new window %d' % i, roomy=True)
    check(sim.svc_log.count(0x0A) == before, 'throttle: a new window of 100 ms starts free (40 failing replies, no wait)')


def scenario_log_cost(elf, level):
    """One call of Log() / DebugEvent() with a format, as the HLE proper makes them on the game's threads: the cost at
    debug 0 (which the HLE pays on every command that logs) must be a few words, not the line buffer."""
    for fn_name, sym, ceiling in (('Log()', '_ZN3azl3LogEPKcz', LOG_CALL_CEILING),
                                  ('DebugEvent()', '_ZN3azl10DebugEventEPKcz', DEBUGEVENT_CALL_CEILING)):
        sim = Sim(elf, level, hle=True)
        uc = sim.uc
        fmt = sim.alloc(64)
        uc.mem_write(fmt, b"uds: role %s -> %s (node %u)\0")
        s1 = sim.alloc(16)
        uc.mem_write(s1, b"idle\0")
        s2 = sim.alloc(16)
        uc.mem_write(s2, b"host\0")
        entry_sp = ROOMY_ENTRY_SP
        sim.reset_game_stack(True)
        for reg, value in ((UC_ARM_REG_R0, fmt), (UC_ARM_REG_R1, s1), (UC_ARM_REG_R2, s2), (UC_ARM_REG_R3, 1)):
            uc.reg_write(reg, value)
        uc.reg_write(UC_ARM_REG_SP, entry_sp)
        uc.reg_write(UC_ARM_REG_LR, STUB_RET)
        sim.min_sp = entry_sp
        sim.measuring = True
        try:
            uc.emu_start(elf.sym(sym), STUB_RET, count=2_000_000)
        except UcError as e:
            sim.measuring = False
            check(False, '%s: emulation error %s at pc=%08x' % (fn_name, e, sim.pc))
            continue
        sim.measuring = False
        depth = entry_sp - sim.min_sp
        record('%s with a format (one call from the HLE proper)' % fn_name, level, depth, ceiling[level], 'call')
        if level > 0 and fn_name == 'Log()':
            check(sim.log_lines == ['uds: role idle -> host (node 1)'], 'Log(): the line reaches the sink (%r)' % (sim.log_lines,))


# =================================================================================================================
# the Download Play hub (phase 3): the COMPILED DlpHub, DlpService and DlpLobby, on the game's thread
# =================================================================================================================
# The plugin's hub is built by its own constructor in the emulator. Two kinds of rig:
#   spy    the hub's LobbyNet (the adapter on the UDS engine) is replaced by a vtable of the emulator's: every method but the two
#          clocks is a thunk that the sim answers (and writes down), so a command that reaches the network is SEEN doing it, and the
#          engine is not there at all (its pointer is an unmapped address: a touch is a loud failure). Used for the commands and the
#          waits, where the game thread is the object of the measure.
#   engine the real UdsEngine and the real adapter, a transport that drops the frames (a plugin with no room) or hands them to the room
#          client. Used where the game thread itself runs the lobby's network side (DlpHub::RunHere: no network thread alive).
# The network thread is the sim's other thread: whenever the game thread waits or sleeps, DlpHub::Tick runs in its place.
LOBBYNET_SLOTS = ('NowMs', 'NowTicks', 'HostOpen', 'HostUpdate', 'HostClose', 'Kick', 'ScanOpen', 'ScanClose', 'ScanRead',
                  'ConnectBegin', 'ConnectPoll', 'Disconnect', 'MyNode', 'Members', 'ReadNode', 'Send', 'Receive')
CLOCK_SLOTS = ('NowMs', 'NowTicks')
TD_TID = 0x000400000010C000          # Kirby Triple Deluxe
HOST_MAC = bytes([0x02, 0x00, 0x00, 0x00, 0xAB, 0xCD])
OWN_MAC = bytes([0x02, 0x00, 0x00, 0x00, 0x12, 0x34])
DLP_FAILURE = 0xC940AC04             # DlpHub::kResultWirelessOff
NOT_FOUND = 0xC880AC05
WRONG_STATE = 0xC8A113EA
# The request headers of the services as the games send them (SPEC-DLP.md sez. 2 and 3): id << 16 | normal words << 6 | translate words
FK_INIT_NAME, FK_FINALIZE, FK_GET_EVENT, FK_CHANNELS = 0x00110203, 0x00020000, 0x00030000, 0x00040000
FK_START_SCAN, FK_STOP_SCAN, FK_SERVER_INFO, FK_TITLE_INFO = 0x00050180, 0x00060000, 0x00070080, 0x00080100
FK_IN_ORDER, FK_START_FAKE, FK_MY_STATUS, FK_NODES = 0x00090040, 0x000B0100, 0x000C0000, 0x000D0040
SV_INIT, SV_INIT_NAME, SV_START_ACCEPT = 0x00010183, 0x000F0303, 0x00050080
FK_STOP_SESSION, SV_FINALIZE, SV_END_ACCEPT = 0x00100000, 0x00020000, 0x00060000     # (the commands that succeed in every state, with FK_FINALIZE / FK_STOP_SCAN)
HUB_BYTES = 416                      # sizeof(DlpHub) on ARM32 is at most 408 (Tests/dlp_stack_probe.cpp holds that)


def hdr(cmd_id, normal, translate=0):
    return (cmd_id << 16) | (normal << 6) | translate


class HubRig:
    """A DlpHub built by the plugin's constructor, with the other thread (Tick) and, optionally, the spy LobbyNet or a real engine."""

    def __init__(self, sim, elf, engine='spy', room=False):
        self.sim, self.elf, self.kind = sim, elf, engine
        uc = sim.uc
        self.kernel = sim.alloc(16)
        p32(uc, self.kernel, elf.sym('_ZTVN3azl3ctr12CtrKernelOpsE') + 8)
        self.engine = 0x0F000000 if engine == 'spy' else self.make_engine(room)
        cfg = sim.alloc(48)
        uc.mem_write(cfg, struct.pack('<QQ20s', TD_TID, 0x1122334455667788, 'Link'.encode('utf-16-le').ljust(20, b'\0')))
        res = sim.alloc(8)
        p32(uc, res, 0)
        p32(uc, res + 4, elf.sym('_ZN3azl3ctr12_GLOBAL__N_1L14MapGameAddressEPvmm'))
        self.hub = sim.alloc(512)
        sim.call(elf.sym('_ZN3azl6DlpHubC1EPNS_9UdsEngineEPNS_9KernelOpsERKNS_11LobbyConfigERKNS_11IpcResolverE'),
                 [self.hub, self.engine, self.kernel, cfg], stack_args=[res])
        vt = elf.sym('_ZTVN3azl14EngineLobbyNetE')
        check(r32(uc, self.hub) == vt + 8, 'the lobby adapter is the first member of the hub (its vtable pointer is at +0)')
        if engine == 'spy':
            self.install_spy(vt)
        else:
            self.install_watch(vt)
        sim.on_yield = self.net_thread
        self.dead = False       # True: the other thread does not run (the network thread is gone)

    # ---- the engine ----
    def make_engine(self, room):
        sim, uc, elf = self.sim, self.sim.uc, self.elf
        eng = sim.alloc(0x40000, 8)
        tr = sim.alloc(32, 4)
        send = sim.alloc(16, 4)
        if room:
            self.room = self.make_room()
            uc.mem_write(send, struct.pack('<IIII', 0xE5900000, 0xE51FF004, elf.sym('_ZN3azl10RoomClient14SendWifiPacketERKNS_10WifiPacketE'), 0))
            p32(uc, tr + 0, self.room)            # (the "lambda" is a pointer to the room client: ldr r0, [r0]; b SendWifiPacket)
        else:
            uc.mem_write(send, struct.pack('<I', 0xE12FFF1E))     # bx lr: the plugin's lambda drops the frame when no room is joined
        mac = sim.alloc(16, 4)
        uc.mem_write(mac, struct.pack('<I', 0xE12FFF1E))
        sim.hooks[mac] = ('mac', None)
        p32(uc, tr + 8, 0x07000004)
        p32(uc, tr + 12, send)
        p32(uc, tr + 16 + 8, 0x07000004)
        p32(uc, tr + 16 + 12, mac)
        sim.call(elf.sym('_ZN3azl9UdsEngineC1EPNS_9KernelOpsENS0_9TransportE'), [eng, self.kernel, tr])
        return eng

    def make_room(self):
        """A RoomClient built by its own constructor and put in the Joined state (state_ is its first member: state() is `ldr r0, [r0]`), so that
        the plugin's transport lambda - here a jump to SendWifiPacket - queues the frames as it does in a room."""
        sim, uc, elf = self.sim, self.sim.uc, self.elf
        room = sim.alloc(16 * 1024, 8)
        sim.call(elf.sym('_ZN3azl10RoomClientC1Ev'), [room])
        p32(uc, room, 3)           # State::Joined
        return room

    # ---- the network seen by the hub ----
    def install_spy(self, vt):
        sim, uc = self.sim, self.sim.uc
        new_vt = sim.alloc(4 * (2 + len(LOBBYNET_SLOTS)), 4)
        p32(uc, new_vt, 0)
        p32(uc, new_vt + 4, 0)
        for i, name in enumerate(LOBBYNET_SLOTS):
            orig = r32(uc, vt + 8 + 4 * i)
            if name in CLOCK_SLOTS:
                sim.hooks[orig & ~1] = ('watch', name)       # the real clock, written down
                p32(uc, new_vt + 8 + 4 * i, orig)
            else:
                thunk = sim.alloc(16, 4)
                uc.mem_write(thunk, struct.pack('<I', 0xE12FFF1E))
                sim.hooks[thunk] = ('spy', name)
                p32(uc, new_vt + 8 + 4 * i, thunk)
        p32(uc, self.hub, new_vt + 8)

    def install_watch(self, vt):
        sim, uc = self.sim, self.sim.uc
        for i, name in enumerate(LOBBYNET_SLOTS):
            sim.hooks[r32(uc, vt + 8 + 4 * i) & ~1] = ('watch', name)

    def net_thread(self, sim):
        """The network thread's turn: the game thread is waiting or sleeping (a sleep of the join wait is 10 ms of the clock)."""
        if self.dead:
            return
        if self.pending_sleep:
            sim.ticks += 10 * 268112
        self.run_tick()

    pending_sleep = True

    def run_tick(self):
        """DlpHub::Tick on the plugin's own stack; what it calls of the adapter is written down apart from the game thread's calls."""
        sim = self.sim
        n_spy, n_obs = len(sim.spy_calls), len(sim.observed)
        sim.call(self.elf.sym('_ZN3azl6DlpHub4TickEv'), [self.hub])
        sim.net_calls += sim.spy_calls[n_spy:] + sim.observed[n_obs:]
        del sim.spy_calls[n_spy:]
        del sim.observed[n_obs:]

    def tick(self, ms=25):
        """A round of the network thread outside any wait (the clock moves on first: the lobby's pump runs once per 20 ms)."""
        self.sim.ticks += ms * 268112
        self.run_tick()


def hle_for_hub(sim, elf, rig):
    """A UdsHle whose hub is the rig's (UdsHle::SetDlpHub is inline: the member is found where RunDlp loads it)."""
    uc = sim.uc
    obj = sim.alloc(64 * 1024)
    sim.call(elf.sym('_ZN3azl3ctr6UdsHleC1EPNS_9UdsEngineE'), [obj, rig.engine])
    run = elf.sym('_ZN3azl3ctr6UdsHle6RunDlpEhmPm') & ~1
    off = None
    for i in range(8):
        ins = r32(uc, run + 4 * i)
        if (ins & 0xFFF00000) == 0xE5900000:      # ldr rd, [r0, #imm12]  (cond always, load word, immediate offset up, base r0)
            off = ins & 0xFFF
            break
    check(off is not None, 'UdsHle::RunDlp loads the hub from a member of UdsHle (the first ldr [r0, #n] of its prologue)')
    p32(uc, obj + off, rig.hub)
    p32(uc, elf.find('g_hle'), obj)
    sim.hle = obj
    return obj


def open_session(sim, name):
    words = struct.unpack('<2I', name12(name))
    sim.kernel = lambda s, n: (_ for _ in ()).throw(AssertionError('the real kernel was called'))
    sim.set_request([0x00050100, words[0], words[1], len(name), 0])
    sim.enter('azl_svc32_entry', 0x14801F, 'open ' + name, roomy=True)
    return r32(sim.uc, sim.cmd() + 12)


def static_buffer(sim, size):
    """Static buffer 0 of the game thread: [descriptor, address], the address a block of the emulator's heap."""
    buf = sim.alloc(size + 16)
    p32(sim.uc, TLS + 0x180, (size << 14) | 2)
    p32(sim.uc, TLS + 0x184, buf)
    return buf


def reply(sim, n=4):
    return [r32(sim.uc, sim.cmd() + 4 * i) for i in range(n)]


def mac_words(mac):
    return struct.unpack('<IH', mac)


def dlp_call(sim, handle, header, words, name, roomy=False):
    sim.set_request([header] + list(words))
    return sim.enter('azl_svc32_entry', handle, name, roomy=roomy)


def hub_words(rig):
    """The words of the DlpHub object (the emulator's memory): a game thread's call is checked by what it changed in it."""
    return struct.unpack('<%dI' % (HUB_BYTES // 4), bytes(rig.sim.uc.mem_read(rig.hub, HUB_BYTES)))


def hub_diff(before, after):
    """{byte offset: (before, after)} of the words that differ"""
    return {4 * i: (b, a) for i, (b, a) in enumerate(zip(before, after)) if b != a}


def scenario_hub_commands(elf, level):
    """dlp:FKCL as the game sends it: Initialize, StartScan (posted: the network thread runs them), the pump finds a host, then the commands
    a game thread serves ITSELF (GetServerInfo and GetTitleInfo with a record written into the game's static buffer, GetTitleInfoInOrder,
    GetMyStatus ...) - and on those the spy sees no LobbyNet method but the clock -, then StartFakeSession and the wait for the join."""
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    sim.hle_rig = rig
    ceiling = HLE_CEILING[level]
    h = open_session(sim, 'dlp:FKCL')
    check(h != 0, 'the hub opens a fake dlp:FKCL session')
    rig.tick()                                         # (the network thread is alive: a Tick has been seen)
    # InitializeWithName (posted)
    name_words = list(struct.unpack('<5I', 'Kirby'.encode('utf-16-le').ljust(20, b'\0')))
    y0 = sim.yields
    r0, depth, ok = dlp_call(sim, h, FK_INIT_NAME, [0x1000, 1] + name_words + [0, 0x04000000, 0, 0], 'fkcl InitializeWithName')
    check(reply(sim, 2) == [hdr(0x11, 1), 0], 'FKCL InitializeWithName is answered by the network thread: %r' % (reply(sim, 2),))
    check(sim.yields > y0, 'FKCL InitializeWithName waited for the network thread (it is a posted command)')
    record('hub: posted command (FKCL InitializeWithName), the network thread serves it', level, depth, ceiling, 'hub')
    # StartScan (posted); the group is then heard by the network thread's pump
    sim.spy_group = struct.pack('<6sBBI20sBB2x', HOST_MAC, 1, 3, (TD_TID & 0xFFFFFF00) | 0, 'Host'.encode('utf-16-le').ljust(20, b'\0'), 4, 1)
    r0, depth, ok = dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan')
    check(reply(sim, 2) == [hdr(0x05, 1), 0], 'FKCL StartScan is answered: %r' % (reply(sim, 2),))
    check('ScanOpen' in sim.net_calls, 'StartScan opened the scan on the network thread (the spy saw ScanOpen there)')
    check('ScanOpen' not in sim.spy_calls, 'StartScan did not open the scan on the game thread')
    rig.tick()
    check('ScanRead' in sim.net_calls, 'the pump of the network thread read the scan (the spy saw ScanRead there)')
    # the inline commands: the spy must see the clock and nothing else
    mlo, mhi = mac_words(HOST_MAC)
    worst = {}
    for header, size, label, words, mac_at in (
            (FK_SERVER_INFO, 0x298, 'GetServerInfo (the 0x298-byte record)', [mlo, mhi], 0),
            (FK_TITLE_INFO, 0x13A8, 'GetTitleInfo (the 0x13A8-byte record)', [mlo, mhi, 0, 0], 8),
            (FK_IN_ORDER, 0x13A8, 'GetTitleInfoInOrder (the next record)', [1], 8),
            (FK_MY_STATUS, 0, 'GetMyStatus', [], 0),
            (FK_NODES, 0, 'GetConnectingNodes', [4], 0),
            (FK_GET_EVENT, 0, 'GetEventDesc', [], 0),
            (FK_CHANNELS, 0, 'GetChannels', [], 0)):
        cmd_id = header >> 16
        sim.spy_calls.clear()
        sim.observed.clear()
        buf = static_buffer(sim, size) if size else 0
        y0 = sim.yields
        # The trace of the commands (debug only, DESIGN-FASE3 sez. 6; the 'dlpc: ' lines): two of the commands are read with a window on the
        # trace function they enter - GetEventDesc on DlpTraceRequest (every command), GetChannels on DlpTraceReply (the first call of a
        # command is always a line). Both test the debug level first thing (a test in RunDlp itself cost its frame 8 bytes at every level): at
        # debug 0 they are entered and leave at once, pushing nothing (the window's lowest SP is the SP it was entered with) and writing nothing;
        # at debug 1 the window is the depth of the trace alone (the hook's frames, RunDlp, the function and the whole of the line's path down
        # to the sink), a sibling of NoteCommand's chain, which must be no deeper than it.
        trace_fn = {'GetEventDesc': 'DlpTraceRequest', 'GetChannels': 'DlpTraceReply'}.get(label)
        if trace_fn is not None:
            sim.window_begin(elf.find(trace_fn))
        sim.log_lines.clear()
        r0, depth, ok = dlp_call(sim, h, header, words, 'fkcl ' + label)
        rep = reply(sim, 4)
        if trace_fn is not None:
            w = sim.window_depth(ENTRY_SP)
            trace_lines = [l for l in sim.log_lines if l.startswith('dlpc: ')]
            if level == 0:
                check(w is not None and sim.win_low == sim.win_entry_sp and trace_lines == [],
                      'debug 0: %s leaves at once, pushes nothing and writes no trace line (window %r, entered at %r, %r)' % (
                          trace_fn, w, None if sim.win_entry_sp is None else ENTRY_SP - sim.win_entry_sp, trace_lines))
            else:
                check(w is not None, 'debug 1: %s was entered by FKCL %s' % (trace_fn, label))
                if trace_fn == 'DlpTraceReply':
                    check(len(trace_lines) == 1 and re.fullmatch(r'dlpc: dlp:FKCL 0x04 req( 00000000){6} -> 0x00000000 rep 00000421 \(\d+ ms\)', trace_lines[0]) is not None,
                          'debug 1: the trace line of GetChannels (%r)' % (trace_lines,))
                if w is not None:
                    record('hub: the trace of a command (window: %s down)' % trace_fn, level, w, ceiling, 'hub')
        check(sim.yields == y0, 'inline FKCL %s did not wait for the network thread' % label)
        check(sim.spy_calls == [], 'inline FKCL %s called no LobbyNet method of the adapter on the game thread (%r)' % (label, sim.spy_calls))
        check(all(o in CLOCK_SLOTS for o in sim.observed), 'inline FKCL %s called only the clock (%r)' % (label, sim.observed))
        if size:
            check(rep[0] >> 16 == cmd_id and rep[1] == 0, 'inline FKCL %s: the record was written (%r)' % (label, [hex(x) for x in rep]))
            check(bytes(sim.uc.mem_read(buf + mac_at, 6)) == HOST_MAC, 'inline FKCL %s: the record holds the host\'s MAC (at +%d)' % (label, mac_at))
        worst[label] = depth
        record('hub: inline FKCL ' + label, level, depth, ceiling, 'hub')
    # the posted commands that leave a request and finish (Finalize), and the join
    r0, depth, ok = dlp_call(sim, h, FK_FINALIZE, [], 'fkcl Finalize')
    check(reply(sim, 2) == [hdr(0x02, 1), 0], 'FKCL Finalize is answered')
    record('hub: posted command (FKCL Finalize)', level, depth, ceiling, 'hub')
    # a join: Initialize, StartScan, the host is heard, StartFakeSession (posted: JoinPending), the wait loop sleeps 10 ms at a time and
    # the network thread's pump polls the (spy) engine, which says "joining" twice and then "failed"
    dlp_call(sim, h, FK_INIT_NAME, [0x1000, 1] + name_words + [0, 0x04000000, 0, 0], 'fkcl InitializeWithName')
    dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan')
    rig.tick()
    dlp_call(sim, h, FK_STOP_SCAN, [], 'fkcl StopScan')
    sim.spy_calls.clear()
    sim.join_polls = 0
    y0 = sim.yields
    sleeps0 = sim.svc_log.count(0x0A)
    r0, depth, ok = dlp_call(sim, h, FK_START_FAKE, [mlo, mhi, 0, 0], 'fkcl StartFakeSession')
    check('ConnectBegin' in sim.net_calls and 'ConnectBegin' not in sim.spy_calls,
          'StartFakeSession began the join on the network thread (the spy saw ConnectBegin there, not on the game thread)')
    check(sim.svc_log.count(0x0A) > sleeps0, 'the game thread slept in the join wait (WaitForJoin)')
    check(reply(sim, 2)[0] == hdr(0x0B, 1) and reply(sim, 2)[1] == NOT_FOUND,
          'the join the spy engine failed ends the wait with "server not found": %r' % ([hex(x) for x in reply(sim, 2)],))
    record('hub: FKCL StartFakeSession (the request, then the join wait with the network thread polling)', level, depth, ceiling, 'hub')
    return sim


def scenario_hub_trace_dedupe(elf, level):
    """The trace of the commands (DlpTraceReply) as the target's compiler built it: a reply the command has already given in the last 2 s is
    counted and not written, a NEW reply is a line at once, one command remembers up to 6 replies per window, a seventh distinct one is
    written every time (loud, never silent), and after the window the next call is written again with the count of the calls it left out.
    The same rules as L15 of Tests/ipc_test/dlp_lobby_hle_test.cpp, but over the machine code of the plugin (the 16-bit fold of the reply, the
    array of six, the 2 s window with the clock of the simulated kernel). GetTitleInfoInOrder is the command: its reply echoes the address
    of the game's buffer, so a call into another buffer is another reply, and (1) then (0) is a group and then "no data", as Triple
    Deluxe polls it. At debug 0 the same calls write no line at all."""
    sim = Sim(elf, level, hle=False)
    sim.tick_step = 268                    # 1 us per read of the clock: the calls below are a fraction of the 2 s window
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    sim.hle_rig = rig
    h = open_session(sim, 'dlp:FKCL')
    rig.tick()
    name_words = list(struct.unpack('<5I', 'Kirby'.encode('utf-16-le').ljust(20, b'\0')))
    dlp_call(sim, h, FK_INIT_NAME, [0x1000, 1] + name_words + [0, 0x04000000, 0, 0], 'fkcl InitializeWithName')
    sim.spy_group = struct.pack('<6sBBI20sBB2x', HOST_MAC, 1, 3, (TD_TID & 0xFFFFFF00) | 0, 'Host'.encode('utf-16-le').ljust(20, b'\0'), 4, 1)
    dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan')
    rig.tick()
    rig.tick()
    bufs = [sim.alloc(0x13A8 + 16) for _ in range(8)]         # eight buffers of the game: eight different replies

    def poll(buf, restart):
        p32(sim.uc, TLS + 0x180, (0x13A8 << 14) | 2)
        p32(sim.uc, TLS + 0x184, buf)
        dlp_call(sim, h, FK_IN_ORDER, [restart], 'fkcl GetTitleInfoInOrder(%d)' % restart)
        return reply(sim, 4)

    def lines():
        return [l for l in sim.log_lines if l.startswith('dlpc: dlp:FKCL 0x09 ')]

    group = poll(bufs[0], 1)
    nodata = poll(bufs[0], 0)
    if level == 0:
        for i in range(20):
            poll(bufs[0], 1 if i % 2 == 0 else 0)
        check(lines() == [] and not [l for l in sim.log_lines if l.startswith('dlpc: ')],
              'debug 0: the trace writes no line for a poll that alternates between two replies (%r)' % (sim.log_lines[-3:],))
        return sim
    check(group[0] >> 16 == 0x09 and group[1] == 0 and group[3] == bufs[0], 'GetTitleInfoInOrder(1) gives the group and echoes the game\'s buffer: %r' % ([hex(x) for x in group],))
    check(nodata == [hdr(0x09, 1), 0xC880AFEF, nodata[2], nodata[3]], 'GetTitleInfoInOrder(0) after the group is "no data": %r' % ([hex(x) for x in nodata],))
    check(len(lines()) == 2, 'the first call of each of the two replies is a line (%r)' % (lines(),))
    # (a) 18 more calls that alternate between the same two replies: not one line, 18 counted
    for i in range(2, 20):
        poll(bufs[0], 1 if i % 2 == 0 else 0)
    check(len(lines()) == 2, 'a command that alternates between two replies writes two lines, not one per call (%d lines)' % len(lines()))
    # (b) a NEW reply inside the window is written at once, with nothing counted before it that it should not have (the last line was at the nodata)
    poll(bufs[1], 1)
    check(len(lines()) == 3 and lines()[2].endswith(' ms, 18 more like it since the last line)'),
          'a new reply in the window is a line at its call, with the 18 calls left out before it (%r)' % (lines()[2:],))
    # ... and the replies already seen are still counted, not written, after it
    poll(bufs[0], 1)
    poll(bufs[0], 0)
    poll(bufs[1], 1)
    check(len(lines()) == 3, 'the replies the window already holds stay silent (%d lines)' % len(lines()))
    # (c) 1.5 s into the window the same reply is still only counted; after the 2 s the next call is written again, with the calls it left out:
    # 4 (the last three polls and the one at 1.5 s)
    sim.ticks += 1500 * 268112
    poll(bufs[0], 1)
    check(len(lines()) == 3, 'inside the 2 s of the window (1.5 s) a reply the window holds is still only counted (%d lines)' % len(lines()))
    sim.ticks += 600 * 268112
    poll(bufs[0], 1)
    check(len(lines()) == 4 and lines()[3].endswith(' ms, 4 more like it since the last line)'),
          'after the window the same reply is written again, with the count of the calls left out (%r)' % (lines()[3:],))
    poll(bufs[0], 0)
    check(len(lines()) == 5 and 'more like it' not in lines()[4], 'the new window starts empty: the "no data" is news again, and nothing was left out (%r)' % (lines()[4:],))
    # (d) six replies are remembered in a window (the group in bufs[0], "no data", and the groups in bufs[1..4]); a seventh is written EVERY time
    for k in (1, 2, 3, 4):
        poll(bufs[k], 1)
    check(len(lines()) == 9, 'four more replies: four more lines, the sixth reply of the window is the last the command remembers (%d lines)' % len(lines()))
    poll(bufs[5], 1)
    poll(bufs[5], 1)
    poll(bufs[5], 1)
    check(len(lines()) == 12, 'a seventh distinct reply in one window is written at every call, never silent (%d lines)' % len(lines()))
    poll(bufs[0], 1)
    poll(bufs[4], 1)
    poll(bufs[1], 1)
    check(len(lines()) == 12, 'the six that were remembered are still counted, not written (%d lines)' % len(lines()))
    poll(bufs[6], 1)
    check(len(lines()) == 13 and lines()[12].endswith(' ms, 3 more like it since the last line)'),
          'another seventh reply is a line too, with the calls the remembered ones left out (%r)' % (lines()[12:],))
    # (e) replies that differ in the RESULT alone: the same header, no reply word. A posted command (StartScan) with no network thread to take it is
    # refused at once ("wireless off"); once the network thread has been seen it is served, and a console that has not initialised the client
    # role answers "invalid state" - another result, the same words. A fold that left the result out would call them one reply.
    sim = Sim(elf, level, hle=False)
    sim.tick_step = 268
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    h = open_session(sim, 'dlp:FKCL')
    dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan, no network thread')
    check(reply(sim, 2) == [hdr(0x05, 1), DLP_FAILURE], 'StartScan with no network thread is "wireless off": %r' % ([hex(x) for x in reply(sim, 2)],))
    dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan, no network thread (again)')
    rig.tick()
    dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan, served')
    served = reply(sim, 2)
    check(served[0] == hdr(0x05, 1) and served[1] != DLP_FAILURE and served[1] != 0, 'StartScan served by the network thread before Initialize is an error of its own: %r' % ([hex(x) for x in served],))
    scan_lines = [l for l in sim.log_lines if l.startswith('dlpc: dlp:FKCL 0x05 ')]
    check(len(scan_lines) == 2 and '-> 0x%08x rep (' % DLP_FAILURE in scan_lines[0] and '-> 0x%08x rep (' % served[1] in scan_lines[1] and
          scan_lines[1].endswith(' ms, 1 more like it since the last line)'),
          'a reply that differs in the result alone is a line of its own, with the call the first reply left out (%r)' % (scan_lines,))
    return sim


def scenario_hub_no_network_thread(elf, level):
    """The network thread is not there. A posted command is answered at once with the module's "wireless off" (nobody takes the request),
    and one that is taken too late is cancelled after 2 s and answered the same way; the 8 s cap of the join wait ends the join."""
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    ceiling = HLE_CEILING[level]
    h = open_session(sim, 'dlp:FKCL')
    name_words = list(struct.unpack('<5I', 'Kirby'.encode('utf-16-le').ljust(20, b'\0')))
    # (1) no Tick has ever been seen: refused at once
    y0 = sim.yields
    r0, depth, ok = dlp_call(sim, h, FK_INIT_NAME, [0x1000, 1] + name_words + [0, 0x04000000, 0, 0], 'fkcl InitializeWithName, no network thread')
    check(reply(sim, 2) == [(0x11 << 16) | 0x40, DLP_FAILURE],
          'no network thread: the command is answered "wireless off" at once: %r' % ([hex(x) for x in reply(sim, 2)],))
    check(sim.yields == y0, 'no network thread: the game thread did not wait')
    if level > 0:
        check(any('answered "wireless off"' in l for l in sim.log_lines), 'no network thread: the log says so (%r)' % (sim.log_lines[-2:],))
    record('hub: posted command with no network thread (answered "wireless off" at once)', level, depth, ceiling, 'hub')
    # (2) a Tick has been seen but nobody serves the request: it is cancelled after 2 s (the other thread is alive and does nothing)
    rig.tick()
    rig.dead = True
    sim.log_lines.clear()
    r0, depth, ok = dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan, request not served')
    check(reply(sim, 2) == [(0x05 << 16) | 0x40, DLP_FAILURE], 'a request nobody serves is cancelled and answered "wireless off": %r' % ([hex(x) for x in reply(sim, 2)],))
    check(sim.yields > 0, 'the game thread waited for the network thread (in slices) before giving up')
    record('hub: posted command nobody serves (cancelled after 2 s)', level, depth, ceiling, 'hub')
    return sim


def scenario_hub_always_succeeds(elf, level):
    """The commands that succeed in every state - dlp:FKCL Finalize / StopScan / StopSession and dlp:SRVR Finalize / EndAccepting - are
    answered with SUCCESS when no network thread can serve them (Triple Deluxe and Planet Robobot treat a negative Finalize as fatal):
    the new branch of DlpHub::Serve, run here in the compiled plugin on the 512-byte thread. Five flavours:
      A  no Tick was ever seen: answered at once (no wait), the role-ending ones leave their bit (closing_) and count (deferred_)
         for the next Tick, nothing else is touched; a command that can fail (StartScan) is still answered "wireless off"
      B  the network thread was there and is gone (its last Tick is older than kAliveMs) with a scan open: the same, and the next Tick
         of the returning network thread closes the scan (the end of the role the Finalize had left for it)
      C  a Tick was seen a moment ago but nobody takes the request: it waits kQueuedMs, is cancelled and answered with success
      D  the same for dlp:SRVR (Finalize ends the role, EndAccepting does not), over a host that was opened
      E  the plugin is going away: DlpHub::Stop() before the call, and Stop() while the call waits for a request that was queued
    The depth is read on the 512-byte thread, the log lines (debug 1) must not say "wireless off" for a success, and the hub object
    is compared word by word before and after the call: the counters and the closing bit are the ONLY words the game thread changed."""
    ceiling = HLE_CEILING[level]
    name_words = list(struct.unpack('<5I', 'Kirby'.encode('utf-16-le').ljust(20, b'\0')))
    srv_init = [0x1000, 3, 0x1D, 0, 0x1000, 1] + list(struct.unpack('<6I', 'Link'.encode('utf-16-le').ljust(24, b'\0'))) + [0x04000000, 0x1000, 0]
    success = lambda cmd_id: [(cmd_id << 16) | 0x40, 0]
    wireless_off = lambda cmd_id: [(cmd_id << 16) | 0x40, DLP_FAILURE]

    def call(sim, rig, h, header, words, what, want, waits=False):
        """One call on a dlp session whose network thread cannot serve it. -> (depth, {offset: (before, after)} of the hub object)"""
        before = hub_words(rig)
        sim.spy_calls.clear()
        sim.observed.clear()
        sim.log_lines.clear()
        y0 = sim.yields
        t0 = sim.ticks
        r0, depth, ok = dlp_call(sim, h, header, words, what)
        rep = reply(sim, 2)
        check(rep == want, '%s: answered %s, expected %s' % (what, [hex(x) for x in rep], [hex(x) for x in want]))
        check((sim.yields > y0) == waits, '%s: the game thread %s for the network thread' % (what, 'did not wait' if waits else 'waited'))
        check(sim.spy_calls == [] and sim.observed == [],
              '%s: the game thread called no method of the adapter (%r, %r)' % (what, sim.spy_calls, sim.observed))
        if level > 0:
            said = any('answered "wireless off"' in l for l in sim.log_lines)
            check(said == (want[1] == DLP_FAILURE), '%s: the log %s "wireless off" (%r)' % (
                what, 'says' if want[1] == DLP_FAILURE else 'does not say', sim.log_lines))
        else:
            check(sim.log_lines == [], '%s: nothing is logged at debug 0 (%r)' % (what, sim.log_lines))
        call.waited_ms = (sim.ticks - t0) // 268112
        return depth, hub_diff(before, hub_words(rig))

    # ---- A: no Tick was ever seen -----------------------------------------------------------------------------------------------
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    h = open_session(sim, 'dlp:FKCL')
    depth, d = call(sim, rig, h, FK_FINALIZE, [], 'fkcl Finalize, no network thread', success(0x02))
    worst = depth
    offs = sorted(d)
    check(len(offs) == 3 and all(d[o] == (0, 1) for o in offs),
          'FKCL Finalize, no network thread: failed_fast_, closing_ and deferred_ went 0 -> 1 and nothing else changed (%r)' % (d,))
    failed = offs[0]                      # failed_fast_; the atomics are declared in a row: ..., cancelled_, failed_fast_, missed_, ran_here_, closing_, deferred_
    closing, deferred, cancelled = failed + 12, failed + 16, failed - 4
    check(offs == [failed, closing, deferred], 'the hub object keeps failed_fast_, closing_ and deferred_ at +0, +12, +16 (%r)' % (offs,))
    if level > 0:
        # the lines of a command that succeeded: the first command of the service, the trace of the command (DlpTraceReply, before NoteCommand: a
        # sibling call keeps the chain of the first-call line as short as it was) and the first-call line of the pair. The trace line has the six
        # words of the request (all zero for Finalize), the result and no reply word, and the duration of the simulated clock. The first line
        # names the thread that made the call (decimal) and its priority (hex): the hub's threads wait for each other's locks, and the console
        # test reads from this line how the game's dlp threads stand against the network thread's 0x2F.
        check(len(sim.log_lines) == 3 and sim.log_lines[0] == 'dlp: the first command of dlp:FKCL is 0x02: the Download Play lobby is carried by the room'
              ' (thread %d, priority 0x%x)' % (SIM_THREAD_ID, SIM_THREAD_PRIORITY)
              and re.fullmatch(r'dlpc: dlp:FKCL 0x02 req( 00000000){6} -> 0x00000000 rep \(\d+ ms\)', sim.log_lines[1]) is not None
              and sim.log_lines[2] == 'dlp:FKCL: command 0x02 -> 0x00000000 (first call)',
              'FKCL Finalize, no network thread: the lines of a command that succeeded (%r)' % (sim.log_lines,))
    depth, d = call(sim, rig, h, FK_STOP_SCAN, [], 'fkcl StopScan, no network thread', success(0x06))
    worst = max(worst, depth)
    check(d == {failed: (1, 2)}, 'FKCL StopScan, no network thread: only the count of refused requests moved (%r)' % (d,))
    depth, d = call(sim, rig, h, FK_STOP_SESSION, [], 'fkcl StopSession, no network thread', success(0x10))
    worst = max(worst, depth)
    check(d == {failed: (2, 3), deferred: (1, 2)}, 'FKCL StopSession, no network thread: it ends the role too (the bit was set already) (%r)' % (d,))
    depth, d = call(sim, rig, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan, no network thread', wireless_off(0x05))
    check(d == {failed: (3, 4)}, 'FKCL StartScan (a command that can fail), no network thread: "wireless off", no bit left (%r)' % (d,))
    record('hub: FKCL Finalize / StopScan / StopSession with no network thread (answered with success)', level, worst, ceiling, 'hub')
    before = hub_words(rig)
    rig.tick()
    check(hub_diff(before, hub_words(rig)).get(closing) == (1, 0), 'the next Tick of the network thread took the bit the closes had left')
    check(hub_words(rig)[deferred // 4] == 2, 'the count of deferred closes is the Finalize and the StopSession')

    # ---- B: the network thread was there and is gone, a scan is open --------------------------------------------------------------
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    h = open_session(sim, 'dlp:FKCL')
    rig.tick()
    dlp_call(sim, h, FK_INIT_NAME, [0x1000, 1] + name_words + [0, 0x04000000, 0, 0], 'fkcl InitializeWithName')
    dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan')
    rig.tick()
    check('ScanOpen' in sim.net_calls, 'the scan is open (the network thread ran StartScan)')
    sim.ticks += 1200 * 268112         # (more than kAliveMs since the last Tick)
    rig.dead = True
    depth, d = call(sim, rig, h, FK_FINALIZE, [], 'fkcl Finalize, network thread gone', success(0x02))
    check(set(d) == {failed, closing, deferred} and d[closing] == (0, 1) and d[deferred] == (0, 1),
          'FKCL Finalize, network thread gone: a refused request, the bit and the count (%r)' % (d,))
    record('hub: FKCL Finalize, network thread gone with a scan open (answered with success)', level, depth, ceiling, 'hub')
    sim.net_calls.clear()
    rig.dead = False
    rig.tick()
    check('ScanClose' in sim.net_calls, 'the returning network thread closed the scan the Finalize had left open (the adapter saw %r)' % (sim.net_calls,))
    r0, depth, ok = dlp_call(sim, h, FK_INIT_NAME, [0x1000, 1] + name_words + [0, 0x04000000, 0, 0], 'fkcl InitializeWithName after the return')
    check(reply(sim, 2) == success(0x11), 'the hub serves again once the network thread is back: %r' % ([hex(x) for x in reply(sim, 2)],))

    # ---- C: a Tick was seen a moment ago and nobody takes the request: cancelled after kQueuedMs --------------------------------
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    h = open_session(sim, 'dlp:FKCL')
    rig.tick()
    rig.dead = True
    depth, d = call(sim, rig, h, FK_FINALIZE, [], 'fkcl Finalize, request not served', success(0x02), waits=True)
    check(2000 <= call.waited_ms <= 2400, 'the request waited about kQueuedMs (2 s) of the clock before it was given up (%d ms)' % call.waited_ms)
    check(d.get(cancelled) == (0, 1) and d.get(closing) == (0, 1) and d.get(deferred) == (0, 1) and failed not in d,
          'FKCL Finalize, not served: counted as cancelled (not refused at once), the bit and the count are left (%r)' % (d,))
    record('hub: FKCL Finalize nobody serves (cancelled after 2 s, answered with success)', level, depth, ceiling, 'hub')
    rig.dead = False
    before = hub_words(rig)
    rig.tick()
    check(hub_diff(before, hub_words(rig)).get(closing) == (1, 0), 'the next Tick took the bit (and did not run the request that was given up)')

    # ---- D: dlp:SRVR over an opened host ----------------------------------------------------------------------------------------
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    h = open_session(sim, 'dlp:SRVR')
    rig.tick()
    dlp_call(sim, h, SV_INIT_NAME, srv_init, 'srvr InitializeWithName')
    check(reply(sim, 2) == success(0x0F), 'SRVR InitializeWithName was served by the network thread: %r' % ([hex(x) for x in reply(sim, 2)],))
    if level > 0:
        # the first command of dlp:SRVR: its own line, once, with the thread and the priority the kernel answered (dlp:FKCL's is checked in A)
        first = [l for l in sim.log_lines if l.startswith('dlp: the first command of ')]
        check(first == ['dlp: the first command of dlp:SRVR is 0x0f: the Download Play lobby is carried by the room (thread %d, priority 0x%x)' % (
            SIM_THREAD_ID, SIM_THREAD_PRIORITY)], 'dlp:SRVR: the first-command line names the thread and its priority (%r)' % (first,))
    dlp_call(sim, h, SV_START_ACCEPT, [0, 0], 'srvr StartAccepting')
    check(reply(sim, 2) == success(0x05), 'SRVR StartAccepting was served by the network thread: %r' % ([hex(x) for x in reply(sim, 2)],))
    check('HostOpen' in sim.net_calls, 'the host is open (the network thread ran StartAccepting)')
    sim.ticks += 1200 * 268112
    rig.dead = True
    depth, d = call(sim, rig, h, SV_END_ACCEPT, [], 'srvr EndAccepting, network thread gone', success(0x06))
    worst = depth
    check(d == {failed: (0, 1)}, 'SRVR EndAccepting, network thread gone: it does not end the role (%r)' % (d,))
    depth, d = call(sim, rig, h, SV_FINALIZE, [], 'srvr Finalize, network thread gone', success(0x02))
    worst = max(worst, depth)
    check(d == {failed: (1, 2), closing: (0, 2), deferred: (0, 1)}, 'SRVR Finalize, network thread gone: it ends the creator\'s role (bit 2) (%r)' % (d,))
    depth, d = call(sim, rig, h, SV_INIT, [0x1000, 3, 0x1D, 0, 0x1000, 1, 0x04000000, 0, 0], 'srvr Initialize, network thread gone', wireless_off(0x01))
    check(d == {failed: (2, 3)}, 'SRVR Initialize (a command that can fail), network thread gone: "wireless off", no bit left (%r)' % (d,))
    record('hub: SRVR Finalize / EndAccepting, network thread gone (answered with success)', level, worst, ceiling, 'hub')
    sim.net_calls.clear()
    rig.dead = False
    rig.tick()
    check('HostClose' in sim.net_calls, 'the returning network thread closed the host the Finalize had left open (the adapter saw %r)' % (sim.net_calls,))

    # ---- E: the plugin is going away (DlpHub::Stop): before the call, and while the call waits ------------------------------------
    stop = elf.sym('_ZN3azl6DlpHub4StopEv')
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    h = open_session(sim, 'dlp:FKCL')
    rig.tick()
    sim.call(stop, [rig.hub])
    depth, d = call(sim, rig, h, FK_FINALIZE, [], 'fkcl Finalize after Stop()', success(0x02))
    check(d == {failed: (0, 1), closing: (0, 1), deferred: (0, 1)}, 'FKCL Finalize after Stop(): refused at once, the bit and the count are left (%r)' % (d,))
    record('hub: FKCL Finalize after Stop() (answered with success)', level, depth, ceiling, 'hub')
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    h = open_session(sim, 'dlp:FKCL')
    rig.tick()
    stopped = []

    def stop_while_waiting(s):         # the plugin ends while the game thread waits for a request that was queued: Stop() takes it to Done with ok == false
        if not stopped:
            stopped.append(1)
            s.call(stop, [rig.hub])
    sim.on_yield = stop_while_waiting
    depth, d = call(sim, rig, h, FK_FINALIZE, [], 'fkcl Finalize, Stop() while it waits', success(0x02), waits=True)
    check(d.get(closing) == (0, 1) and d.get(deferred) == (0, 1) and failed not in d and cancelled not in d,
          'FKCL Finalize, Stop() while it waits: neither refused at once nor cancelled, the bit and the count are left (%r)' % (d,))
    check(call.waited_ms < 100, 'Stop() released the wait at once (%d ms of the clock)' % call.waited_ms)
    record('hub: FKCL Finalize, Stop() while it waits (answered with success)', level, depth, ceiling, 'hub')
    return sim


def scenario_hub_join_cap(elf, level):
    """The join wait ends by itself after 8 s when the network thread is gone: the reply is "server not found", and the end of the client
    role is left for the network thread's next Tick (nothing of the lobby's network side runs on the waiting thread)."""
    sim = Sim(elf, level, hle=False)
    rig = HubRig(sim, elf)
    hle_for_hub(sim, elf, rig)
    ceiling = HLE_CEILING[level]
    h = open_session(sim, 'dlp:FKCL')
    name_words = list(struct.unpack('<5I', 'Kirby'.encode('utf-16-le').ljust(20, b'\0')))
    sim.spy_group = struct.pack('<6sBBI20sBB2x', HOST_MAC, 1, 3, (TD_TID & 0xFFFFFF00) | 0, 'Host'.encode('utf-16-le').ljust(20, b'\0'), 4, 1)
    rig.tick()
    dlp_call(sim, h, FK_INIT_NAME, [0x1000, 1] + name_words + [0, 0x04000000, 0, 0], 'fkcl InitializeWithName')
    dlp_call(sim, h, FK_START_SCAN, [0x0421, 0, 0, 0, 0, 0], 'fkcl StartScan')
    rig.tick()
    dlp_call(sim, h, FK_STOP_SCAN, [], 'fkcl StopScan')
    mlo, mhi = mac_words(HOST_MAC)
    # the network thread serves StartFakeSession and is gone from then on: the join is never polled
    orig_net_thread = rig.net_thread

    def one_round_then_gone(s):
        if getattr(rig, 'gone', False):
            s.ticks += 10 * 268112
            return
        orig_net_thread(s)
        rig.gone = True
    sim.on_yield = one_round_then_gone
    sim.log_lines.clear()
    sim.spy_calls.clear()
    t0 = sim.ticks
    r0, depth, ok = dlp_call(sim, h, FK_START_FAKE, [mlo, mhi, 0, 0], 'fkcl StartFakeSession, the network thread gone')
    waited_ms = (sim.ticks - t0) // 268112
    check(reply(sim, 2) == [(0x0B << 16) | 0x40, NOT_FOUND], 'the cap: "server not found": %r' % ([hex(x) for x in reply(sim, 2)],))
    check(8000 <= waited_ms <= 8400, 'the cap came after about 8 s of the clock (%d ms)' % waited_ms)
    if level > 0:
        check(any('the join did not end in 8 s' in l for l in sim.log_lines), 'the cap is logged (%r)' % (sim.log_lines[-1:],))
    check(sim.spy_calls == [], 'the cap: the waiting thread called no method of the adapter (%r)' % (sim.spy_calls,))
    sim.net_calls.clear()
    rig.gone = False
    rig.tick()                                           # the network thread is back: the end of the client role is its Tick's
    check('Disconnect' in sim.net_calls or 'HostClose' in sim.net_calls, 'the next Tick of the network thread ended the join (the adapter saw %r)' % (sim.net_calls,))
    record('hub: StartFakeSession, the 8 s cap of the join wait (the end of the role is left for the next Tick)', level, depth, ceiling, 'hub')
    return sim


def scenario_hub_close(elf, level):
    """The game closes its dlp:SRVR session without Finalize: the role ends (a posted request when the network thread is alive; on the
    calling thread when it is not), through the CloseHandle hook. Twice: over the spy network (the lobby's own part) and over the REAL
    engine with the lobby holding it (SRVR StartAccepting): there the game thread that runs the end itself pays for the engine's
    Shutdown as well."""
    for engine, dead, room in (('spy', False, False), ('spy', True, False), ('engine', False, False), ('engine', True, False),
                               ('engine', True, True)):
        if True:
            sim = Sim(elf, level, hle=False)
            rig = HubRig(sim, elf, engine=engine, room=room)
            hle_for_hub(sim, elf, rig)
            h = open_session(sim, 'dlp:SRVR')
            rig.tick()
            # (SRVR InitializeWithName is the biggest request of the service: 15 words declared by its header)
            r0, depth, ok = dlp_call(sim, h, SV_INIT_NAME, [0x1000, 3, 0x1D, 0, 0x1000, 1] + list(struct.unpack('<6I', 'Link'.encode('utf-16-le').ljust(24, b'\0')))
                                     + [0x04000000, 0x1000, 0], 'srvr InitializeWithName')
            check(reply(sim, 2) == [hdr(0x0F, 1), 0], 'SRVR InitializeWithName is answered by the network thread: %r' % ([hex(x) for x in reply(sim, 2)],))
            record('hub: posted command (SRVR InitializeWithName, 15 words declared), the network thread serves it', level, depth,
                   HLE_CEILING[level], 'hub')
            if engine == 'engine':
                dlp_call(sim, h, SV_START_ACCEPT, [0, 0], 'srvr StartAccepting')
                check(reply(sim, 2) == [hdr(0x05, 1), 0], 'SRVR StartAccepting opened the lobby on the real engine')
            sim.log_lines.clear()
            sim.spy_calls.clear()
            sim.observed.clear()
            if dead:
                sim.ticks += 1200 * 268112     # (more than kAliveMs without a Tick)
                rig.dead = True
            y0 = sim.yields
            r0, depth, ok = sim.enter('azl_svc23_entry', h, 'close dlp:SRVR' + (' (network thread gone)' if dead else ''))
            if level > 0:
                check(any('had called Initialize' in l for l in sim.log_lines), 'the close says the role ends with the session (%r)' % (sim.log_lines,))
            if dead:
                # nothing of the lobby's network side runs on the closing thread: the close is left for the network thread's next Tick
                check(sim.yields == y0, 'network thread gone: the game thread did not wait')
                check(sim.observed == [] and sim.spy_calls == [],
                      'network thread gone: the game thread called no method of the adapter (%r, %r)' % (sim.observed, sim.spy_calls))
                if engine == 'engine':
                    sim.net_calls.clear()
                    rig.dead = False                     # the network thread is back: its next Tick ends the role
                    rig.tick()
                    check(any(m in sim.net_calls for m in ('HostClose', 'Disconnect')),
                          'the next Tick of the network thread ended the role and gave the engine back (the adapter saw %r)' % (sim.net_calls,))
            else:
                check(sim.yields > y0, 'network thread alive: the close left a request and waited')
                check(sim.observed == [], 'network thread alive: the game thread called no method of the adapter (%r)' % (sim.observed,))
            what = '%s%s' % ('real engine' if engine == 'engine' else 'spy network',
                             (', network thread gone, ' + ('room joined' if room else 'no room') + ': the close is left for the next Tick') if dead
                             else ', network thread alive')
            record('hub: closing a dlp:SRVR session that called Initialize (%s)' % what, level, depth, BUDGET_LOG, 'log')
    return sim


def scenario_hub_found_dead(elf, level):
    """The game's handle value was reused by another object (a close the hook did not see): the next call on it fails svcClearEvent, the fake
    session is forgotten, and the role it had begun ends - a posted request, or, with no network thread, a bit for the next Tick. The call
    then goes on to the real kernel (the sim's kernel answers an error)."""
    for dead in (False, True):
        sim = Sim(elf, level, hle=False)
        rig = HubRig(sim, elf, engine='engine')
        hle_for_hub(sim, elf, rig)
        h = open_session(sim, 'dlp:SRVR')
        rig.tick()
        dlp_call(sim, h, SV_INIT, [0x1000, 3, 0x1D, 0, 0x1000, 1, 0x04000000, 0, 0], 'srvr Initialize')
        dlp_call(sim, h, SV_START_ACCEPT, [0, 0], 'srvr StartAccepting')
        sim.observed.clear()
        sim.net_calls.clear()
        if dead:
            sim.ticks += 1200 * 268112
            rig.dead = True
        sim.kernel = lambda s, n: 0xD8E007F7
        sim.clear_event_fails = True
        y0 = sim.yields
        sim.set_request([0x000C0000])
        r0, depth, ok = sim.enter('azl_svc32_entry', h, 'call on a dlp:SRVR session found dead' + (' (network thread gone)' if dead else ''))
        sim.clear_event_fails = False
        check(r0 == 0xD8E007F7, 'the call on a session found dead goes on to the real kernel (its result comes back)')
        if dead:
            check(sim.yields == y0 and sim.observed == [], 'network thread gone: the game thread neither waited nor called the adapter (%r)' % (sim.observed,))
            rig.dead = False
            rig.tick()
        check(any(m in sim.net_calls for m in ('HostClose', 'Disconnect')), 'the network thread ended the role of the forgotten session (%r)' % (sim.net_calls,))
        record('hub: a call on a dlp:SRVR session found dead%s' % (', network thread gone' if dead else ''), level, depth, BUDGET_LOG, 'log')
    return sim


def scenario_hub_engine(elf, level):
    """The real engine under the lobby: the lobby holds it (SRVR Initialize + StartAccepting), then the game starts its own nwm::UDS
    (InitializeWithVersion, 0x1B): OnGameUdsStarting gives the engine back first. With the network thread alive the request is posted;
    with it gone the game thread does the lobby's network side itself (RunHere), with no room (the transport drops the frames) or in a
    room (the frames are queued by the room client). The depth of DlpHub::OnGameUdsStarting alone is read with a window (everything
    below that call, from the entry SP). The same Initialize with NOTHING held is the base: the UDS thread pays for it anyway."""
    base = None
    for flavour, hold, dead, room in (('nothing held (the base)', False, False, False), ('alive', True, False, False),
                                      ('gone, no room', True, True, False), ('gone, room joined', True, True, True)):
        sim = Sim(elf, level, hle=False)
        rig = HubRig(sim, elf, engine='engine', room=room)
        hle_for_hub(sim, elf, rig)
        words = struct.unpack('<2I', name12('nwm::UDS'))
        sim.set_request([0x00050100, words[0], words[1], 8, 0])
        sim.enter('azl_svc32_entry', 0x14801F, 'open nwm::UDS', roomy=True)
        uds = r32(sim.uc, sim.cmd() + 12)
        rig.tick()
        if hold:
            srvr = open_session(sim, 'dlp:SRVR')
            dlp_call(sim, srvr, SV_INIT, [0x1000, 3, 0x1D, 0, 0x1000, 1, 0x04000000, 0, 0], 'srvr Initialize')
            dlp_call(sim, srvr, SV_START_ACCEPT, [0, 0], 'srvr StartAccepting')
            check(reply(sim, 2) == [hdr(0x05, 1), 0], 'SRVR StartAccepting opened the lobby on the real engine: %r' % ([hex(x) for x in reply(sim, 2)],))
            # while the lobby holds the engine the game's own UDS commands are "wrong state" (UdsHle::Dispatch's gate)
            sim.set_request([0x00020000])
            sim.enter('azl_svc32_entry', uds, 'uds command 0x02 while the lobby holds the engine', roomy=True)
            check(reply(sim, 2)[1] == WRONG_STATE, 'while the lobby holds the engine a nwm::UDS command answers "wrong state": %r' % (reply(sim, 2),))
        if dead:
            sim.ticks += 1200 * 268112
            rig.dead = True
        sim.observed.clear()
        sim.net_calls.clear()
        sim.window_begin(elf.sym('_ZN3azl6DlpHub17OnGameUdsStartingEv'))
        # InitializeWithVersion as libctru sends it: 12 normal words (the shared memory size, the NodeInfo, the version), 2 translate
        sim.set_request([(0x1B << 16) | (12 << 6) | 2, 0x1000] + [0] * 10 + [0x0400, 0, 0])
        r0, depth, ok = sim.enter('azl_svc32_entry', uds, 'nwm::UDS Initialize, %s' % flavour, roomy=True)
        w = sim.window_depth(ROOMY_ENTRY_SP)
        if not hold:
            base = depth
            record('hub: nwm::UDS Initialize with nothing held (the UDS thread pays this whole call anyway)', level, depth, 4096, 'dead')
            continue
        check(w is not None, 'DlpHub::OnGameUdsStarting was entered and left (%s)' % flavour)
        if flavour == 'alive':
            check(sim.observed == [], 'network thread alive: the game thread called no method of the adapter (%r)' % (sim.observed,))
            check('HostClose' in sim.net_calls or 'Disconnect' in sim.net_calls,
                  'network thread alive: the network thread gave the engine back (%r)' % (sim.net_calls,))
        else:
            check(any(m in sim.observed for m in ('HostClose', 'Disconnect')),
                  'network thread gone: the game thread gave the engine back itself (RunHere; the adapter saw %r)' % (sim.observed,))
        if w is not None:
            if flavour == 'alive':
                record('hub: OnGameUdsStarting, network thread alive (window: DlpHub::OnGameUdsStarting down)', level, w, HLE_CEILING[level], 'hle')
            else:
                # the thread is the game's nwm::UDS thread: it runs the engine's Initialize right after, and its engine calls need UDS_ENGINE_CALL
                record('hub: OnGameUdsStarting, network thread %s (window: DlpHub::OnGameUdsStarting down; Initialize alone is %s)' % (
                    flavour, base), level, w, UDS_ENGINE_CALL[level], 'dead')
    return sim


def main():
    # --pass-through-only: the scenarios that exist in every version of the plugin (to run an older ELF and see what
    # the gate says about it: the 0.1.15 build must FAIL it). The two older ELFs are in analysis/baseline/ (outside git, like
    # the rest of analysis/; see the README.txt there): AzaharLink-0.1.15.elf fails (116 / 1388 bytes, 21 of 42 checks) and
    # AzaharLink-0.1.15.1.elf passes (100 / 148 bytes, 24 checks).
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    only_pass_through = '--pass-through-only' in sys.argv
    only_hub = '--hub-only' in sys.argv       # (while working on the hub scenarios: they alone)
    elf = Elf(args[0])
    for level in (0, 1):
        print('== debug %d' % level)
        if only_hub:
            scenario_hub_commands(elf, level)
            scenario_hub_trace_dedupe(elf, level)
            scenario_hub_no_network_thread(elf, level)
            scenario_hub_always_succeeds(elf, level)
            scenario_hub_join_cap(elf, level)
            scenario_hub_close(elf, level)
            scenario_hub_found_dead(elf, level)
            scenario_hub_engine(elf, level)
            continue
        scenario_pure(elf, level)
        if only_pass_through:
            continue
        scenario_diet(elf, level)
        scenario_getservice(elf, level)
        scenario_traced(elf, level)
        scenario_apt(elf, level)
        scenario_hle_front(elf, level)
        scenario_throttle(elf, level)
        scenario_log_cost(elf, level)
        if only_hub or not only_pass_through:
            scenario_hub_commands(elf, level)
            scenario_hub_trace_dedupe(elf, level)
            scenario_hub_no_network_thread(elf, level)
            scenario_hub_always_succeeds(elf, level)
            scenario_hub_join_cap(elf, level)
            scenario_hub_close(elf, level)
            scenario_hub_found_dead(elf, level)
            scenario_hub_engine(elf, level)
    print()
    print('%-72s %5s %7s %7s' % ('path (bytes below the SP at azl_svc32_entry)', 'debug', 'used', 'budget'))
    for scenario, level, depth, budget, kind in results:
        print('%-72s %5d %7d %7d  %s' % (scenario, level, depth, budget, kind))
    print()
    print('(the game wrapper frame and the trampoline add %d bytes to every number: the srv thread of Tri Force Heroes'
          ' has %d bytes below its call,' % (WRAPPER_FRAME + TRAMPOLINE_PUSH, FREE_BELOW_DISPATCHER))
    print(' so a path that uses N here uses N + %d of them)' % (WRAPPER_FRAME + TRAMPOLINE_PUSH))
    print('chain_sim: %d checks, %d failures' % (checks[0], len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
