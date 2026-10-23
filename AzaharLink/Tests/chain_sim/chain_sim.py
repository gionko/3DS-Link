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
    are run) and the game's Nintendo-SDK thread object is laid out like Tri Force Heroes' (a 512-byte stack inside a
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
What this does not run: the observers' own code (Plugin::OnSleepEnter / OnHomeButton / NoteSleepEvent: stubbed here, the
last one with a check of what it is told and when) and an engine with a session up (the sim has none); Tests/callgraph_check.py
follows both statically. 0.1.16 adds: the APT observer of the sleep protocol (what it is told, and that the replies are
told BEFORE their call), and 60 failing replies in a row on a fake session (the throttle of a polling game thread: the
sleeps happen, the replies do not change).
"""
import os
import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc, UcError)
from unicorn.arm_const import *

# ---- the game's thread, laid out like Tri Force Heroes' srv-notification thread ------------------------------------------
OBJ = 0x00800800                 # the SDK object: header 0x28 bytes, then the 512-byte stack
STACK_LO = OBJ + 0x28
STACK_TOP = OBJ + 0x228
DISPATCHER_SP = OBJ + 0x1F8      # SP in the watcher function when it calls the srv wrapper
WRAPPER_FRAME = 16               # `bl 0x108a24`: push {r4, r5, r6, lr}
TRAMPOLINE_PUSH = 4              # the per-site trampoline: push {lr}
ENTRY_SP = DISPATCHER_SP - WRAPPER_FRAME - TRAMPOLINE_PUSH   # SP at azl_svc32_entry
FREE_BELOW_DISPATCHER = DISPATCHER_SP - STACK_LO             # 464
HEADER = {0x00: 0xAAAA0000, 0x04: 0xAAAA0004, 0x08: 0xAAAA0008, 0x0C: 0x00140021, 0x10: 0x00140025,
          0x14: 0x00000000, 0x18: 0x0080B900, 0x1C: 0x00000001, 0x20: 0x00000000, 0x24: 0x00000000}
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
        self.measuring = False
        self.log_lines = []         # every line handed to the sink (LogLine and Log())
        self.calls = []             # observer calls
        self.lines_at_call = []     # (observer, number of main-log lines already written when it was called)
        self.pc = 0
        self.next_handle = 0x00200000
        self.hooks = {}
        self.sink_thunk = None
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
        uc.mem_write(limits, struct.pack('<QQiII4x', 1 << 20, 64 << 20, 7, 64 * 1024, 500))   # (LogFileLimits: 32 bytes since 0.1.17, exit_capture_ms)
        self.call(ctor, [obj, 0, dirstr, 0], stack_args=[0, 0, limits])   # (the u64 start_ms is the first stack argument, limits the next)
        # pending_ is the std::string at +84 (+76 until 0.1.16: LogFileLimits grew by 8 bytes with exit_capture_ms); give it the
        # capacity Flush() leaves it with (reserve(16 KB)), so that the lines of the scenarios do not allocate, as in the steady
        # state of the plugin
        pend = obj + 84
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
        if self.measuring:
            sp = uc.reg_read(UC_ARM_REG_SP)
            if sp < self.min_sp:
                self.min_sp = sp
        self.pc = address

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
        elif num == 0x35:     # GetProcessId: result in r0, the id in r1
            uc.reg_write(UC_ARM_REG_R1, 0x1D)
            uc.reg_write(UC_ARM_REG_R0, 0)
        elif num == 0x0B:     # GetThreadPriority: result in r0, the priority in r1
            uc.reg_write(UC_ARM_REG_R1, 0x30)
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
        try:
            uc.emu_start(self.elf.sym(entry_symbol), STUB_RET, count=5_000_000)
        except UcError as e:
            self.measuring = False
            check(False, '%s: emulation error %s at pc=%08x' % (name, e, self.pc))
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


def main():
    # --pass-through-only: the scenarios that exist in every version of the plugin (to run an older ELF and see what
    # the gate says about it: the 0.1.15 build must FAIL it). The two older ELFs are in analysis/baseline/ (outside git, like
    # the rest of analysis/; see the README.txt there): AzaharLink-0.1.15.elf fails (116 / 1388 bytes, 21 of 42 checks) and
    # AzaharLink-0.1.15.1.elf passes (100 / 148 bytes, 24 checks).
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    only_pass_through = '--pass-through-only' in sys.argv
    elf = Elf(args[0])
    for level in (0, 1):
        print('== debug %d' % level)
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
