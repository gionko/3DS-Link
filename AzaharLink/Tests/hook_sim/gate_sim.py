#!/usr/bin/env python3
"""Runs the REAL Sources/ctr/gate.s (assembled by build_gate.sh) in an ARM11 emulator, on real call sites of games, against the
swap of the plugin's block at every instruction boundary.

    python Tests/hook_sim/gate_sim.py <sim-dir> [--fail-fast] [--quick]      (sim-dir from build_gate.sh; unicorn on the host)

The sites (Tests/hook_sim/gate_sites.json, from extract_sites.py): the shared `svc 0x32; bx lr` of LEGO Batman 2 (its code from a
Luma dump), the one of Monster Hunter XX called from Thumb with `blx`, the one of Kirby Triple Deluxe and its `svc 0x23; bx lr`, and
sites inside their functions: `svc; ands; bmi` (Super Street Fighter IV, Tekken) and `svc; and; cmp; ldrge` (DBZ Extreme Butoden).
Each is installed the way the plugin will: the gate copied into its page (0x01E7E000), one three-word stub, ONE word `b stub` at the
site, all before the thread runs; the stand-in plugin (gate_plugin_stubs.s) at 0x07000000 answers with action 0, 1 or 2.

What is held, on every run:
  * the game ends where the same code without the hook ends (the caller after `bx lr`, or the end of the site's function), with the
    same registers, condition flags, sp, lr, the same data and the same stack at and above its SP; r0 is what the kernel (or the
    plugin, action 0) answered, plus 0x100 when post ran;
  * one svc of the site's number, made from the gate's page with the game's handle (none when the plugin answered); no other svc;
  * the plugin's code runs only while this thread is counted in in_flight, never once the block is unmapped (it is, for real), and
    in_flight is back to zero at the end; post runs only when asked (action >= 2) and only after a flag read of 1, and every post not
    run is counted in post_skipped;
  * order (the emulator has one memory order, so the barriers are checked on the trace): before every call into the plugin the
    thread's count went up, then a DMB, then the flag was read as 1; between the plugin's last instruction and the count coming
    down, a DMB;
  * pre gets (handle, stub+8, ctx = sp), post gets (result, stub+8, ctx = sp), sp on eight bytes at both, ctx[0..2] as pre left
    them and ctx[3] = the action at post; last_site / last_handle are the thread's when it went into the plugin (pre called),
    and untouched (zero) when it went straight to the kernel;
  * nothing is written in the game's text, the stubs or the gate's code; the gate's frame below the game's SP at the call into the
    plugin is at most GATE_FRAME_MAX bytes.

The swap: after k steps of the thread, flag = 0, the thread runs while in_flight != 0 (a thread asleep in a svc while counted is a
failure: the swap would wait for ever), the block is unmapped; after j more steps it is mapped again and flag = 1; then the thread
runs to its end. Every (k, j). A svc is a step of its own: the thread sleeps between the svc and the kernel's answer, so the swap
falls inside the sleep too. Two threads (the shared stubs): one asleep in the gate's svc, the swap closed, the other entering with
flag = 0; every point where the first wakes and where the block comes back.

The format of 1.1.2 (two words `ldr pc, [pc, #-4]` + the trampoline's address, the trampoline of BuildSvcTrampoline), rebuilt here
in Python: the sites put back during the HOME menu, the thread asleep in the game's own svc, the sites hooked again before it wakes.
It must FALL on every site: the word at site+4 is the trampoline's address, executed as an instruction (dump 1 of LEGO Batman 2).

The fake sessions (risk O-A, 2026-11-08): with the flag 0 and the game's handle in a slot of the table at +0x24, a svc 0x32 is
answered by the gate itself (no svc, r0 = 0, the thread's cmdbuf[0] = the command's id | 0x40, cmdbuf[1] = the slot's result,
fake_hits + 1) and the game goes on as with a kernel that answered 0; a svc 0x23 is made for real and the slot's bit is set in
fake_closed, the slot untouched (the plugin's alone). A handle in no slot, and the handle 0 (the free slots), go to the kernel as before, nothing written.
"""
import json
import os
import re
import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE,
                     UC_MODE_ARM, Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_C13_C0_3)

try:
    from unicorn.arm_const import UC_CPU_ARM_1176
except ImportError:
    UC_CPU_ARM_1176 = None

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = 0x01E7E000            # the gate's page (as assembled by build_gate.sh)
PAGE2 = 0x01E70000           # a second copy, to see that the gate is position independent
PLUGIN = 0x07000000          # the plugin's block (the stand-in), unmapped during the swap
TRAMP = 0x070D4000           # 1.1.2: the trampoline (the literal of dump 1 of LEGO Batman 2)
STACK = 0x0FF00000
STACK_SIZE = 0x4000
DATA = 0x0D000000            # r1-r12 point here (the inline sites load and store through them)
# the callers of a stub: the page below the site's (ARM `bl site` for thread A, +0x40 for B, +0x100 Thumb `blx site`: its range is 4 MB)
HANDLE = 0x00C40001
STRAY = 0xE0E01BFD           # the kernel's answer to any other svc (OutOfRange, as in the dump)
BELOW = 0xE5E5E5E5           # what the stack holds below the game's SP
GATE_FRAME_MAX = 48          # the gate's frame below the game's SP at the call into the plugin (stub word included)
UDF = 0xE7F000F0
TLS = 0x1FF82000             # the thread's TLS (TPIDRURO): its command buffer at +0x80
CMD_HEADER = 0x000D0082      # a command the game wrote (id 0xD, 2 normal words) before its svc 0x32
CMD_WORD1 = 0x11111111
FAKE_RESULT = 0xC8A113EA     # nwm::UDS's wrong state, what the plugin puts in a fake session's slot
DECOY = 0x00C40099           # a handle of another fake session
REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12]


def W(x):
    return struct.pack('<I', x & 0xFFFFFFFF)


def B(frm, to, link=False):
    return (0xEB000000 if link else 0xEA000000) | (((to - (frm + 8)) >> 2) & 0xFFFFFF)


def thumb_blx(frm, to):
    """Thumb BLX (immediate) at `frm` to the ARM address `to`, in the ARMv6 pair (prefix + suffix, 4 MB of range)."""
    off = to - (frm + 4)
    assert -0x400000 <= off < 0x400000 and not (to & 3)
    return struct.pack('<HH', 0xF000 | ((off >> 12) & 0x7FF), 0xE800 | ((off >> 1) & 0x7FF))


def is_dmb(w):
    return (w & 0xFFFF0FFF) == 0xEE070FBA


def syms(path):
    out = {}
    for line in open(path):
        f = line.split()
        if len(f) == 2:
            out[f[0]] = int(f[1], 16)
    return out


class Site:
    def __init__(self, d, index):
        self.name = d['name']
        self.index = index
        self.kind = d['kind']
        self.svc = int(d['svc'], 16)
        self.site = int(d['site'], 16)
        self.words = [int(w, 16) for w in d['words']]
        self.caller = d.get('caller', 'arm')
        self.end = int(d['end'], 16) if 'end' in d else None
        self.lo = self.site & ~0xFFF
        self.hi = (self.site + 4 * len(self.words) + 0xFFF) & ~0xFFF
        self.callers = self.lo - 0x1000

    def start(self, which=0):
        """(pc, thumb) where the thread starts, and where it stops."""
        if self.kind == 'inline':
            return self.site, False, None
        if self.caller == 'thumb':
            tc = self.callers + 0x100 + 0x20 * which
            return tc | 1, True, tc + 4
        c = self.callers + 0x40 * which
        return c, False, c + 4


class Thread:
    def __init__(self, name, site, which, sp, flags_in):
        self.name = name
        self.which = which
        self.site = site
        self.sp0 = sp
        self.flags_in = flags_in
        self.start, self.thumb, self.ret = site.start(which)
        self.ctx = None
        self.asleep = None
        self.done = False
        self.fault = None
        self.svcs = []
        self.events = []
        self.counted = 0
        self.pre_calls = []
        self.post_calls = []
        self.errors = []
        self.min_sp = sp
        self.frame_at_call = 0
        self.literal_run = False
        self.fresh = True           # (unicorn takes Thumb from the start address, not from a CPSR written before the first run)

    def state(self):
        return (self.ctx, self.asleep, self.done, self.fault, list(self.svcs), list(self.events), self.counted,
                list(self.pre_calls), list(self.post_calls), list(self.errors), self.min_sp, self.frame_at_call, self.literal_run,
                self.fresh)

    def load(self, s):
        (self.ctx, self.asleep, self.done, self.fault, svcs, events, self.counted, pre, post, errors, self.min_sp,
         self.frame_at_call, self.literal_run, self.fresh) = s
        self.svcs, self.events, self.pre_calls, self.post_calls, self.errors = list(svcs), list(events), list(pre), list(post), list(errors)


class Rig:
    """One emulator: the game's words, and either the gate (fmt 'gate', at `page`), or the hook of 1.1.2 (fmt '112'), or nothing
    (fmt 'plain': the reference)."""

    def __init__(self, sim, site, fmt, action=1, result=0, page=PAGE):
        self.sim = sim
        self.site = site
        self.fmt = fmt
        self.action = action
        self.result = result
        self.page = page
        self.G = {k: v - PAGE + page for k, v in sim.G.items()}
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        if UC_CPU_ARM_1176 is not None:
            uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
        uc.mem_map(site.lo, site.hi - site.lo)
        uc.mem_map(site.callers, 0x1000)
        uc.mem_map(STACK, STACK_SIZE)
        uc.mem_map(DATA, 0x1000)
        for i, w in enumerate(site.words):
            uc.mem_write(site.site + 4 * i, W(w))
        for which in (0, 1):
            c = site.callers + 0x40 * which
            uc.mem_write(c, W(B(c, site.site, True)) + W(0xEAFFFFFE))
            tc = site.callers + 0x100 + 0x20 * which
            uc.mem_write(tc, thumb_blx(tc, site.site) + struct.pack('<H', 0xE7FE))
        uc.mem_write(DATA, bytes((i * 7 + 3) & 0xFF for i in range(0x1000)))
        self.plugin_mapped = False
        self.stub = None
        if fmt == 'gate':
            uc.mem_map(page, 0x2000)
            uc.mem_write(page, sim.GATE)
            uc.mem_write(self.G['azl_gate_flag'], W(1))
            for slot, fn in (('azl_gate_pre32', 'plugin_pre'), ('azl_gate_post32', 'plugin_post'),
                             ('azl_gate_pre23', 'plugin_pre'), ('azl_gate_post23', 'plugin_post')):
                uc.mem_write(self.G[slot], W(sim.PL[fn]))
            stubs = page + ((len(sim.GATE) + 15) & ~15)
            self.stub = stub = stubs + 12 * site.index
            gate = self.G['azl_gate_32' if site.svc == 0x32 else 'azl_gate_23']
            uc.mem_write(stub, W(0xE52DE004) + W(B(stub + 4, gate, True)) + W(B(stub + 8, site.site + 4)))
            uc.mem_write(site.site, W(B(site.site, stub)))       # the ONE word, before any thread runs; never again
            self.map_plugin()
            self.code_lo = self.G['azl_gate_32']
            self.code_hi = stubs + 12 * len(sim.sites)
            uc.hook_add(UC_HOOK_MEM_WRITE, self.on_forbidden_write, begin=self.code_lo, end=self.code_hi - 1)
            uc.hook_add(UC_HOOK_MEM_WRITE, self.on_count_write, begin=self.G['azl_gate_in_flight'], end=self.G['azl_gate_in_flight'] + 3)
            uc.hook_add(UC_HOOK_MEM_READ, self.on_flag_read, begin=self.G['azl_gate_flag'], end=self.G['azl_gate_flag'] + 3)
            uc.hook_add(UC_HOOK_CODE, self.on_gate_code, begin=page, end=page + 0x1FFF)
            uc.hook_add(UC_HOOK_CODE, self.on_plugin_code, begin=PLUGIN, end=PLUGIN + 0xFFF)
        elif fmt == '112':
            self.tramp_page()
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_forbidden_write, begin=site.lo, end=site.hi - 1)
        uc.hook_add(UC_HOOK_CODE, self.on_game_code, begin=site.callers, end=site.hi - 1)
        uc.hook_add(UC_HOOK_INTR, self.on_svc)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.on_unmapped)
        self.threads = []
        self.cur = None
        self.finishing = False
        self.installed = True

    # ---- the plugin's block ----
    def map_plugin(self):
        if not self.plugin_mapped:
            self.uc.mem_map(PLUGIN, 0x1000)
            self.uc.mem_write(PLUGIN, self.sim.PLUG)
            self.uc.mem_write(self.sim.PL['plugin_action'], W(self.action))
            self.plugin_mapped = True

    def unmap_plugin(self):
        if self.plugin_mapped:
            self.uc.mem_unmap(PLUGIN, 0x1000)
            self.plugin_mapped = False

    def tramp_page(self):
        """1.1.2: the trampoline as BuildSvcTrampoline builds it, and an entry that makes the svc from the plugin's block."""
        uc, s = self.uc, self.site
        uc.mem_map(TRAMP, 0x1000)
        entry = TRAMP + 0x100
        nxt = s.words[1]
        assert nxt == 0xE12FFF1E or ((nxt & 0x0E000000) not in (0x0A000000,) and ((nxt >> 16) & 0xF) != 15), 'next insn not relocatable as is'
        uc.mem_write(TRAMP, W(0xE52DE004) + W(B(TRAMP + 4, entry, True)) + W(0xE49DE004) + W(nxt) + W(0xE51FF004) + W(s.site + 8))
        uc.mem_write(entry, W(0xEF000000 | s.svc) + W(0xE12FFF1E))   # (the 1.1.2 entry's own counting is not what this mode looks at)

    def write_code(self, addr, data):
        """A write in code that has run: the emulator's translated blocks of it go too (a real core has the I-cache flush)."""
        self.uc.mem_write(addr, data)
        self.uc.ctl_remove_cache(addr, addr + len(data))

    def set_flag(self, v):
        self.uc.mem_write(self.G['azl_gate_flag'], W(v))

    def word(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def in_flight(self):
        return self.word(self.G['azl_gate_in_flight'])

    # ---- hooks ----
    def on_unmapped(self, uc, access, addr, size, value, _):
        t = self.cur
        if t is not None and t.fault is None:
            t.fault = 'access %d to unmapped %08X at pc %08X' % (access, addr, uc.reg_read(UC_ARM_REG_PC))
        return False

    def on_forbidden_write(self, uc, access, addr, size, value, _):
        if self.installed and self.cur is not None and self.fmt != '112':
            self.cur.errors.append('code written at %08X (pc %08X)' % (addr, uc.reg_read(UC_ARM_REG_PC)))

    def on_count_write(self, uc, access, addr, size, value, _):
        t = self.cur
        old = self.in_flight()
        if value == (old + 1) & 0xFFFFFFFF:
            t.events.append('inc')
            t.counted += 1
        elif value == (old - 1) & 0xFFFFFFFF:
            # a DMB between the plugin's last instruction and the count coming down
            last_inc = max(i for i, e in enumerate(t.events) if e == 'inc') if 'inc' in t.events else -1
            plugin = [i for i, e in enumerate(t.events) if e == 'plugin' and i > last_inc]
            if plugin and 'dmb' not in t.events[plugin[-1] + 1:]:
                t.errors.append('the count comes down with no DMB after the plugin ran')
            t.events.append('dec')
            t.counted -= 1
            if t.counted < 0:
                t.errors.append('the count comes down below what this thread put up')
        else:
            t.errors.append('in_flight written %08X -> %08X' % (old, value))

    def on_flag_read(self, uc, access, addr, size, value, _):
        self.cur.events.append(('flag', self.word(self.G['azl_gate_flag'])))

    def on_gate_code(self, uc, addr, size, _):
        t = self.cur
        sp = uc.reg_read(UC_ARM_REG_SP)
        t.min_sp = min(t.min_sp, sp)
        if is_dmb(self.word(addr)):
            t.events.append('dmb')

    def ordered_call(self, t, what):
        if 'inc' not in t.events:
            t.errors.append('%s called with no count' % what)
            return
        last_inc = max(i for i, e in enumerate(t.events) if e == 'inc')
        tail = t.events[last_inc + 1:]
        if 'dec' in tail:
            t.errors.append('%s called after the count came down' % what)
        if 'dmb' not in tail:
            t.errors.append('%s called with no DMB after the count went up' % what)
            return
        after = tail[tail.index('dmb') + 1:]
        flags = [e for e in after if isinstance(e, tuple)]
        if not flags or flags[-1][1] != 1:
            t.errors.append('%s called without a flag read of 1 after the count and the DMB' % what)

    def on_plugin_code(self, uc, addr, size, _):
        t = self.cur
        if t.counted <= 0:
            t.errors.append('plugin code at %08X run while the thread is not counted' % addr)
        if not t.events or t.events[-1] != 'plugin':
            t.events.append('plugin')
        sp = uc.reg_read(UC_ARM_REG_SP)
        r = [uc.reg_read(x) for x in REGS[:3]]
        stub8 = self.stub + 8
        if addr == self.sim.PL['plugin_pre']:
            self.ordered_call(t, 'pre')
            t.pre_calls.append(self.action)
            t.frame_at_call = max(t.frame_at_call, t.sp0 - sp)
            if sp & 7:
                t.errors.append('pre: sp %08X not on eight bytes' % sp)
            if r != [HANDLE, stub8, sp]:
                t.errors.append('pre: arguments %s, wanted handle, stub+8 %08X, ctx = sp %08X' % (['%08X' % x for x in r], stub8, sp))
        elif addr == self.sim.PL['plugin_post']:
            self.ordered_call(t, 'post')
            t.post_calls.append(r[0])
            t.frame_at_call = max(t.frame_at_call, t.sp0 - sp)
            ctx = struct.unpack('<4I', uc.mem_read(sp, 16))
            if sp & 7:
                t.errors.append('post: sp %08X not on eight bytes' % sp)
            if r[1:] != [stub8, sp]:
                t.errors.append('post: arguments %s' % ['%08X' % x for x in r])
            if ctx != (self.action, HANDLE, stub8, self.action):
                t.errors.append('post: ctx %s, wanted (%d, handle, stub+8, action %d)' % (['%08X' % x for x in ctx], self.action, self.action))
            if r[0] != self.result:
                t.errors.append('post: result %08X, the kernel said %08X' % (r[0], self.result))

    def on_game_code(self, uc, addr, size, _):
        t = self.cur
        if self.fmt == '112' and addr == t.site.site + 4 and self.word(addr) == TRAMP:
            t.literal_run = True
        if self.finishing and self.at_end(t, addr):
            uc.emu_stop()

    def at_end(self, t, addr):
        s = t.site
        if t.ret is not None:
            return addr == t.ret
        return addr == s.end or (s.lo <= addr < s.hi and not (s.site <= addr < s.site + 4 * len(s.words)))

    def answer(self, n):
        if n in (0x32, 0x23):
            return self.result
        return STRAY

    def on_svc(self, uc, intno, _):
        t = self.cur
        pc = uc.reg_read(UC_ARM_REG_PC)
        n = self.word(pc - 4) & 0xFFFFFF
        t.svcs.append((n, uc.reg_read(UC_ARM_REG_R0), pc - 4))
        if self.finishing:
            uc.reg_write(UC_ARM_REG_R0, self.answer(n))
        else:
            t.asleep = n
            uc.emu_stop()

    # ---- threads ----
    def add_thread(self, name, which, sp, flags_in):
        uc = self.uc
        t = Thread(name, self.site, which, sp, flags_in)
        uc.mem_write(sp - 0x400, W(BELOW) * 0x100)
        uc.mem_write(sp, b''.join(W(0x5A000000 | (which << 16) | i) for i in range(0x40)))
        uc.reg_write(UC_ARM_REG_CPSR, 0x10 | flags_in | (0x20 if t.thumb else 0))
        uc.reg_write(UC_ARM_REG_R0, HANDLE)
        for i in range(1, 13):
            uc.reg_write(REGS[i], DATA + 0x100 * i + 0x10 * which)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, 0x0BADBAD0 + which)
        uc.reg_write(UC_ARM_REG_PC, t.start & ~1)
        t.ctx = uc.context_save()
        self.threads.append(t)
        return t

    def step(self, t):
        """One instruction of `t` (or the kernel's answer to its svc). False when it cannot go on."""
        if t.fault or t.done:
            return False
        uc = self.uc
        self.cur = t
        uc.context_restore(t.ctx)
        if t.asleep is not None:
            uc.reg_write(UC_ARM_REG_R0, self.answer(t.asleep))
            t.asleep = None
            t.ctx = uc.context_save()
            return True
        pc = uc.reg_read(UC_ARM_REG_PC)
        if self.at_end(t, pc):
            t.done = True
            return False
        thumb = (uc.reg_read(UC_ARM_REG_CPSR) & 0x20) or (t.fresh and t.thumb)
        t.fresh = False
        try:
            uc.emu_start(pc | (1 if thumb else 0), 0xFFFFFFFF, count=1)
        except UcError as e:
            if t.fault is None:
                t.fault = '%s at pc %08X' % (e, pc)
        t.ctx = uc.context_save()
        return t.fault is None

    def finish(self, t, limit=4000):
        """Runs `t` to its end at once (the kernel answers each svc right away)."""
        uc = self.uc
        self.cur = t
        if t.fault or t.done:
            return
        uc.context_restore(t.ctx)
        if t.asleep is not None:
            uc.reg_write(UC_ARM_REG_R0, self.answer(t.asleep))
            t.asleep = None
        self.finishing = True
        try:
            for _ in range(8):
                pc = uc.reg_read(UC_ARM_REG_PC)
                if self.at_end(t, pc):
                    t.done = True
                    break
                thumb = (uc.reg_read(UC_ARM_REG_CPSR) & 0x20) or (t.fresh and t.thumb)
                t.fresh = False
                try:
                    uc.emu_start(pc | (1 if thumb else 0), 0xFFFFFFFF, count=limit)
                except UcError as e:
                    if t.fault is None:
                        t.fault = '%s at pc %08X' % (e, uc.reg_read(UC_ARM_REG_PC))
                    break
                if t.fault:
                    break
            else:
                t.errors.append('does not end (pc %08X)' % uc.reg_read(UC_ARM_REG_PC))
        finally:
            self.finishing = False
            t.ctx = uc.context_save()

    # ---- snapshots ----
    def snapshot(self):
        uc = self.uc
        mem = [bytes(uc.mem_read(STACK, STACK_SIZE)), bytes(uc.mem_read(DATA, 0x1000))]
        if self.fmt == 'gate':
            mem.append(bytes(uc.mem_read(self.page, 0x64)))
        return mem, self.plugin_mapped, [t.state() for t in self.threads]

    def restore(self, snap):
        uc = self.uc
        mem, mapped, states = snap
        uc.mem_write(STACK, mem[0])
        uc.mem_write(DATA, mem[1])
        if self.fmt == 'gate':
            uc.mem_write(self.page, mem[2])
            if mapped:
                self.map_plugin()
            else:
                self.unmap_plugin()
        for t, s in zip(self.threads, states):
            t.load(s)

    def regs_of(self, t):
        self.uc.context_restore(t.ctx)
        r = [self.uc.reg_read(x) for x in REGS]
        return r, self.uc.reg_read(UC_ARM_REG_SP), self.uc.reg_read(UC_ARM_REG_LR), self.uc.reg_read(UC_ARM_REG_PC), \
            self.uc.reg_read(UC_ARM_REG_CPSR) >> 28

    def stack_above(self, t):
        return bytes(self.uc.mem_read(t.sp0, 0x100))


class Sim:
    def __init__(self, d, fail_fast, quick):
        self.G = syms(os.path.join(d, 'gate.sym'))
        self.PL = syms(os.path.join(d, 'plugin.sym'))
        self.GATE = open(os.path.join(d, 'gate.bin'), 'rb').read()
        self.PLUG = open(os.path.join(d, 'plugin.bin'), 'rb').read()
        assert self.G['azl_gate_begin'] == PAGE, 'build_gate.sh assembles the gate at %08X' % PAGE
        self.answer_word = struct.unpack_from('<I', self.PLUG, self.PL['plugin_answer'] - PLUGIN)[0]
        self.sites = [Site(s, i) for i, s in enumerate(json.load(open(os.path.join(HERE, 'gate_sites.json')))['sites'])]
        self.fail_fast = fail_fast
        self.quick = quick
        self.failures = []
        self.interleavings = 0
        self.refs = {}
        self.frames = {}
        self.paths = {'plugin answered': 0, 'svc, no post': 0, 'svc + post': 0, 'post skipped (flag 0 after the svc)': 0,
                      'straight to the kernel (flag 0)': 0}

    def fail(self, what):
        self.failures.append(what)
        if len(self.failures) <= 25:
            print('  FAIL: ' + what)
        if self.fail_fast:
            raise StopIteration

    # ---- the same code without the hook ----
    def reference(self, site, which, sp, flags_in, value):
        key = (site.name, which, sp, flags_in, value)
        if key not in self.refs:
            rig = Rig(self, site, 'plain', result=value)
            t = rig.add_thread('ref', which, sp, flags_in)
            rig.finish(t)
            assert t.done and not t.fault, '%s: the reference does not end (%s)' % (site.name, t.fault)
            self.refs[key] = (rig.regs_of(t), bytes(rig.uc.mem_read(DATA, 0x1000)), rig.stack_above(t), [s[0] for s in t.svcs])
        return self.refs[key]

    def judge(self, rig, t, label):
        """What a thread that went through the gate must look like at the end."""
        s = t.site
        errs = list(t.errors)
        if t.fault:
            errs.append('fault: %s' % t.fault)
        elif not t.done:
            errs.append('not at its end')
        answered = bool(t.pre_calls) and t.pre_calls[0] == 0
        if len(t.pre_calls) > 1 or len(t.post_calls) > 1:
            errs.append('pre called %d times, post %d' % (len(t.pre_calls), len(t.post_calls)))
        if t.post_calls and not (t.pre_calls and t.pre_calls[0] >= 2):
            errs.append('post called for action %s' % t.pre_calls)
        svcs = [(n, h, pc) for n, h, pc in t.svcs]
        if not t.fault:
            if answered:
                if svcs:
                    errs.append('the plugin answered and a svc was made %s' % svcs)
            elif [n for n, _, _ in svcs] != [s.svc]:
                errs.append('svcs %s, wanted one 0x%X' % ([hex(n) for n, _, _ in svcs], s.svc))
            elif svcs[0][1] != HANDLE:
                errs.append('the kernel saw handle %08X' % svcs[0][1])
            elif not (rig.page <= svcs[0][2] < rig.page + 0x1000):
                errs.append('the svc was made at %08X, not in the gate' % svcs[0][2])
        if t.fault or not t.done:
            return errs
        value = self.answer_word if answered else (rig.result + 0x100 if t.post_calls else rig.result)
        (regs, sp, lr, pc, fl), data, above, _ = self.reference(s, t.which, t.sp0, t.flags_in, value)
        g = rig.regs_of(t)
        for i in range(13):
            if g[0][i] != regs[i]:
                errs.append('r%d %08X, without the hook %08X' % (i, g[0][i], regs[i]))
        if g[1] != sp or g[2] != lr or g[3] != pc:
            errs.append('sp/lr/pc %08X %08X %08X, without the hook %08X %08X %08X' % (g[1], g[2], g[3], sp, lr, pc))
        if g[4] != fl:
            errs.append('flags %X, without the hook %X' % (g[4], fl))
        if bytes(rig.uc.mem_read(DATA, 0x1000)) != data:
            errs.append('the data differ from the run without the hook')
        if rig.stack_above(t) != above:
            errs.append('the stack at and above the game\'s SP differs')
        if t.frame_at_call > GATE_FRAME_MAX:
            errs.append('frame below the game\'s SP at the call into the plugin: %d bytes' % t.frame_at_call)
        return errs

    def judge_rig(self, rig, label):
        errs = []
        for t in rig.threads:
            errs += ['%s: %s' % (t.name, e) for e in self.judge(rig, t, label)]
            if rig.fmt == 'gate':
                if not t.pre_calls:
                    self.paths['straight to the kernel (flag 0)'] += 1
                elif t.pre_calls[0] == 0:
                    self.paths['plugin answered'] += 1
                elif t.pre_calls[0] == 1:
                    self.paths['svc, no post'] += 1
                else:
                    self.paths['svc + post' if t.post_calls else 'post skipped (flag 0 after the svc)'] += 1
        if rig.fmt == 'gate':
            if rig.in_flight() != 0:
                errs.append('in_flight left at %d' % rig.in_flight())
            want = sum(1 for t in rig.threads if t.pre_calls and t.pre_calls[0] >= 2 and t.svcs and not t.post_calls and t.done)
            got = rig.word(rig.G['azl_gate_post_skipped'])
            if got != want:
                errs.append('post_skipped %d, wanted %d' % (got, want))
            if len(rig.threads) == 1:
                t = rig.threads[0]
                ls, lh = rig.word(rig.G['azl_gate_last_site']), rig.word(rig.G['azl_gate_last_handle'])
                if t.pre_calls:
                    if (ls, lh) != (rig.stub + 8, HANDLE):
                        errs.append('last_site / last_handle %08X %08X' % (ls, lh))
                elif t.svcs and (ls, lh) != (0, 0):
                    errs.append('last_site / last_handle %08X %08X written by a thread that did not go into the plugin' % (ls, lh))
        for e in errs:
            self.fail('%s: %s' % (label, e))
        return not errs

    # ---- runs ----
    def combos(self, site):
        actions = (0, 1, 2)
        parities = (0, 4)
        for parity in parities:
            for action in actions:
                yield action, parity

    def new_rig(self, site, action, parity, page=PAGE):
        result = 0 if parity == 0 else 0xD8E007F7
        flags_in = 0x60000000 if parity == 0 else 0x90000000
        rig = Rig(self, site, 'gate', action=action, result=result, page=page)
        rig.add_thread('A', 0, STACK + 0x2000 + parity, flags_in)
        return rig

    def plain(self, site, action, parity, page=PAGE):
        rig = self.new_rig(site, action, parity, page)
        t = rig.threads[0]
        n = 0
        while rig.step(t):
            n += 1
        self.judge_rig(rig, '%s plain action %d sp%%8=%d page %08X' % (site.name, action, parity, page))
        self.frames[(site.name, action, parity)] = (t.frame_at_call, t.sp0 - t.min_sp)
        return n

    def close(self, rig):
        """SWAP_ENTER: flag = 0, then wait for in_flight == 0 while the thread runs; then the block goes."""
        t = rig.threads[0]
        rig.set_flag(0)
        guard = 0
        while rig.in_flight() != 0:
            if t.asleep is not None:
                t.errors.append('the count is held by a thread asleep in a svc: the swap would wait for ever')
                return
            if not rig.step(t) or guard > 2000:
                break
            guard += 1
        rig.unmap_plugin()

    def reopen(self, rig):
        rig.map_plugin()
        rig.set_flag(1)

    def exhaustive(self, site, action, parity, n):
        rig = self.new_rig(site, action, parity)
        t = rig.threads[0]
        s_k = rig.snapshot()
        count = 0
        for k in range(n + 2):
            rig.restore(s_k)
            self.close(rig)
            s_j = rig.snapshot()
            j = 0
            while True:
                rig.restore(s_j)
                self.reopen(rig)
                rig.finish(t)
                count += 1
                if not self.judge_rig(rig, '%s action %d sp%%8=%d swap after %d steps, back after %d more' % (site.name, action, parity, k, j)):
                    break
                rig.restore(s_j)
                if not rig.step(t):
                    if t.fault is None:
                        break
                j += 1
                s_j = rig.snapshot()
            rig.restore(s_k)
            if not rig.step(t) and t.done:
                break
            s_k = rig.snapshot()
        self.interleavings += count
        return count

    def fake(self, site):
        """The swap closed (flag 0 from the start) and the table of the fake sessions filled: the handle of the game in slot 0 or 6,
        in none (decoys only), or the handle 0 against the free slots."""
        runs = 0
        G = self.G
        for case in ('slot0', 'slot6', 'none', 'zero', 'closed6'):
            for parity in (0, 4):
                label = '%s fake %s sp%%8=%d' % (site.name, case, parity)
                result = 0 if parity == 0 else 0xD8E007F7
                flags_in = 0x60000000 if parity == 0 else 0x90000000
                rig = Rig(self, site, 'gate', action=1, result=result)
                uc = rig.uc
                uc.mem_map(TLS, 0x1000)
                uc.mem_write(TLS + 0x80, W(CMD_HEADER) + W(CMD_WORD1))
                uc.reg_write(UC_ARM_REG_C13_C0_3, TLS)
                th = rig.add_thread('A', 0, STACK + 0x2000 + parity, flags_in)
                handle = HANDLE
                if case == 'zero':
                    handle = 0
                    uc.context_restore(th.ctx)
                    uc.reg_write(UC_ARM_REG_R0, 0)
                    th.ctx = uc.context_save()
                slot = {'slot0': 0, 'slot6': 6, 'closed6': 6}.get(case)
                for i in range(7):
                    h = HANDLE if i == slot else (0 if case == 'zero' else DECOY + i)
                    uc.mem_write(G['azl_gate_fake_handles'] + 8 * i, W(h) + W(FAKE_RESULT + i))
                if case == 'closed6':
                    uc.mem_write(G['azl_gate_fake_closed'], W(1 << 6))
                rig.set_flag(0)
                rig.unmap_plugin() # (the swap: the plugin's block is gone, a read of it on the fake path faults)
                while rig.step(th):
                    pass
                runs += 1
                errs = list(th.errors)
                if th.fault or not th.done:
                    errs.append('fault %s, done %s' % (th.fault, th.done))
                    for e in errs:
                        self.fail('%s: %s' % (label, e))
                    continue
                hits = rig.word(G['azl_gate_fake_hits'])
                closed = rig.word(G['azl_gate_fake_closed'])
                tls = (rig.word(TLS + 0x80), rig.word(TLS + 0x84))
                slots = [(rig.word(G['azl_gate_fake_handles'] + 8 * i), rig.word(G['azl_gate_fake_handles'] + 8 * i + 4)) for i in range(7)]
                if th.pre_calls or th.post_calls:
                    errs.append('the plugin was called with the flag 0 (pre %s post %s)' % (th.pre_calls, th.post_calls))
                if rig.in_flight() != 0:
                    errs.append('in_flight left at %d' % rig.in_flight())
                ours = slot is not None and case != 'closed6'
                was_closed = 1 << 6 if case == 'closed6' else 0
                value = result
                if ours and site.svc == 0x32:
                    value = 0
                    if th.svcs:
                        errs.append('a fake session reached the kernel: %s' % th.svcs)
                    if tls != ((CMD_HEADER & 0xFFFF0000) | 0x40, FAKE_RESULT + slot):
                        errs.append('cmdbuf %08X %08X, wanted %08X %08X' % (tls[0], tls[1], (CMD_HEADER & 0xFFFF0000) | 0x40, FAKE_RESULT + slot))
                    if hits != 1 or closed != 0:
                        errs.append('fake_hits %d fake_closed %X, wanted 1 and 0' % (hits, closed))
                    if slots[slot] != (HANDLE, FAKE_RESULT + slot):
                        errs.append('slot %d changed: %08X %08X' % (slot, slots[slot][0], slots[slot][1]))
                else:
                    if [(n, h) for n, h, _ in th.svcs] != [(site.svc, handle)]:
                        errs.append('svcs %s, wanted one 0x%X with handle %08X' % (th.svcs, site.svc, handle))
                    elif not (rig.page <= th.svcs[0][2] < rig.page + 0x1000):
                        errs.append('the svc was made at %08X, not in the gate' % th.svcs[0][2])
                    if tls != (CMD_HEADER, CMD_WORD1):
                        errs.append('the cmdbuf was written: %08X %08X' % tls)
                    if hits != 0:
                        errs.append('fake_hits %d' % hits)
                    want_closed = ((1 << slot) if (slot is not None and site.svc == 0x23) else 0) | was_closed
                    if closed != want_closed:
                        errs.append('fake_closed %X, wanted %X' % (closed, want_closed))
                    for i in range(7):
                        want = (HANDLE if i == slot else (0 if case == 'zero' else DECOY + i), FAKE_RESULT + i)
                        if slots[i] != want:
                            errs.append('slot %d %08X %08X, wanted %08X %08X' % (i, slots[i][0], slots[i][1], want[0], want[1]))
                if not errs and handle == HANDLE:
                    (regs, sp, lr, pc, fl), data, above, _ = self.reference(site, 0, th.sp0, flags_in, value)
                    g = rig.regs_of(th)
                    for i in range(13):
                        if g[0][i] != regs[i]:
                            errs.append('r%d %08X, a kernel that answered %08X: %08X' % (i, g[0][i], value, regs[i]))
                    if g[1:4] != (sp, lr, pc) or g[4] != fl:
                        errs.append('sp/lr/pc/flags differ from a kernel that answered %08X' % value)
                    if bytes(rig.uc.mem_read(DATA, 0x1000)) != data or rig.stack_above(th) != above:
                        errs.append('data or stack differ from a kernel that answered %08X' % value)
                for e in errs:
                    self.fail('%s: %s' % (label, e))
        return runs

    def two_threads(self, site):
        """A asleep in the gate's svc (it asked for a post), the swap closed and the block gone; B comes in with flag = 0. A wakes
        after i steps of B, the block comes back after j steps of B (every i, j); then both run to their end."""
        rig = Rig(self, site, 'gate', action=2, result=0)
        a = rig.add_thread('A', 0, STACK + 0x1000, 0x60000000)
        b = rig.add_thread('B', 1, STACK + 0x3004, 0x90000000)
        while a.asleep is None and rig.step(a):
            pass
        if a.asleep is None:
            self.fail('%s two threads: A never sleeps in the svc' % site.name)
            return 0
        rig.set_flag(0)
        if rig.in_flight() != 0:
            self.fail('%s two threads: A asleep in the svc holds the count' % site.name)
            return 0
        rig.unmap_plugin()
        s0 = rig.snapshot()
        nb = 0
        while rig.step(b):
            nb += 1
        count = 0
        for i in range(nb + 2):
            for j in range(nb + 2):
                rig.restore(s0)
                for x in range(nb + 2):
                    if x == i:
                        rig.finish(a)
                    if x == j:
                        self.reopen(rig)
                    rig.step(b)
                self.reopen(rig)
                rig.finish(a)
                rig.finish(b)
                count += 1
                if not self.judge_rig(rig, '%s two threads: A wakes after %d steps of B, the block back after %d' % (site.name, i, j)):
                    break
        self.interleavings += count
        return count

    def format_112(self, site):
        """1.1.2: the sites are out for the HOME menu (the game's words), the thread goes; after k of its steps ResumeSites writes the
        two words. When the thread is asleep in the site's own svc at that moment it wakes at site+4 on the trampoline's address."""
        rig = Rig(self, site, '112', result=0)
        t = rig.add_thread('A', 0, STACK + 0x2000, 0x60000000)
        s0 = rig.snapshot()
        n = 0
        asleep_at = None
        while True:
            if t.asleep is not None and asleep_at is None and t.svcs and t.svcs[-1][2] == site.site:
                asleep_at = n
            if not rig.step(t):
                break
            n += 1
        falls, total = [], 0
        rig.restore(s0)
        for k in range(n + 1):
            snap = rig.snapshot()
            rig.write_code(site.site, W(0xE51FF004) + W(TRAMP))      # ResumeSites
            rig.finish(t, limit=200)
            total += 1
            if t.literal_run:
                if t.fault:
                    what = t.fault
                else:
                    regs = rig.regs_of(t)
                    ref = self.reference(site, 0, t.sp0, t.flags_in, rig.result)[0]
                    what = 'ended at %08X, %s' % (regs[3], 'state as without the hook' if regs == ref else 'state NOT as without the hook')
                falls.append((k, what))
            rig.restore(snap)
            rig.write_code(site.site, W(site.words[0]) + W(site.words[1]))
            if not rig.step(t):
                break
        ok = asleep_at is not None and any(k == asleep_at for k, _ in falls)
        print('  1.1.2 %-26s %3d resume points, %d run the literal at site+4 %s -> %s' % (
            site.name, total, len(falls), ('(asleep in the svc at step %s: %s)' % (asleep_at, dict(falls).get(asleep_at))),
            'FALLS, as in the dumps' if ok else 'DOES NOT FALL'))
        if not ok:
            self.fail('%s: the format of 1.1.2 does not fall at site+4' % site.name)
        return ok


# The plugin's side of the header (Includes/azl/gate_plan.hpp: WriteGate, GateClose, GateOpen) against the symbols of the assembled
# gate: every offset, the two entries and the size. A gate.s that moves a word without gate_plan.hpp (or the other way) fails here.
PLAN_OFFSETS = {
    'kGateFlag': 'azl_gate_flag', 'kGateInFlight': 'azl_gate_in_flight', 'kGatePre32': 'azl_gate_pre32',
    'kGatePost32': 'azl_gate_post32', 'kGatePre23': 'azl_gate_pre23', 'kGatePost23': 'azl_gate_post23',
    'kGateLastSite': 'azl_gate_last_site', 'kGateLastHandle': 'azl_gate_last_handle', 'kGatePostSkipped': 'azl_gate_post_skipped',
    'kGateFakeHandles': 'azl_gate_fake_handles', 'kGateFakeHits': 'azl_gate_fake_hits', 'kGateFakeClosed': 'azl_gate_fake_closed',
    'kGateCode': 'azl_gate_32', 'kGateEntry23': 'azl_gate_23', 'kGateBytes': 'azl_gate_end',
}


def check_plan_offsets(sim):
    text = open(os.path.join(HERE, '..', '..', 'Includes', 'azl', 'gate_plan.hpp'), encoding='utf-8').read()
    plan = {m.group(1): int(m.group(2), 0) for m in re.finditer(r'constexpr u32 (kGate\w+) = (0x[0-9A-Fa-f]+|\d+);', text)}
    bad = 0
    for const, sym in PLAN_OFFSETS.items():
        if const not in plan:
            sim.fail('gate_plan.hpp has no %s' % const)
            bad += 1
        elif sym not in sim.G:
            sim.fail('the gate has no symbol %s' % sym)
            bad += 1
        elif plan[const] != sim.G[sym] - PAGE:
            sim.fail('gate_plan.hpp %s = 0x%X, the gate has %s at +0x%X' % (const, plan[const], sym, sim.G[sym] - PAGE))
            bad += 1
    if len(sim.GATE) != plan.get('kGateBytes'):
        sim.fail('gate.bin is %d bytes, gate_plan.hpp kGateBytes %s' % (len(sim.GATE), plan.get('kGateBytes')))
        bad += 1
    print('offsets of gate_plan.hpp against the gate: %d checked, %d wrong' % (len(PLAN_OFFSETS) + 1, bad))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    sim = Sim(args[0] if args else os.path.join(HERE, '..', '..', 'analysis', 'gate_sim'), '--fail-fast' in sys.argv, '--quick' in sys.argv)
    print('gate: %d bytes (code %d), header %s' % (len(sim.GATE), sim.G['azl_gate_end'] - sim.G['azl_gate_32'],
          ', '.join('%s +0x%02X' % (k[9:], v - PAGE) for k, v in sorted(sim.G.items(), key=lambda x: x[1]) if v < sim.G['azl_gate_32'])))
    try:
        check_plan_offsets(sim)
        for site in sim.sites:
            sim.format_112(site)
        for site in sim.sites:
            line = []
            for action, parity in sim.combos(site):
                n = sim.plain(site, action, parity)
                sim.plain(site, action, parity, page=PAGE2)
                c = 0 if sim.quick else sim.exhaustive(site, action, parity, n)
                line.append('a%d/sp%d %d steps %d' % (action, parity, n, c))
            two = sim.two_threads(site) if site.kind == 'stub' and not sim.quick else 0
            fk = sim.fake(site)
            print('  gate  %-26s %s; two threads %d; fake sessions %d' % (site.name, ', '.join(line), two, fk))
    except StopIteration:
        pass
    fr = [v[0] for v in sim.frames.values()]
    deep = [v[1] for v in sim.frames.values()]
    if fr:
        print('frame below the game\'s SP at the call into the plugin: %d to %d bytes (budget %d); deepest in the gate %d' % (
            min(fr), max(fr), GATE_FRAME_MAX, max(deep)))
    print('paths judged: ' + ', '.join('%s %d' % kv for kv in sim.paths.items()))
    print('gate_sim: %d interleavings, %d failures' % (sim.interleavings, len(sim.failures)))
    return 1 if sim.failures else 0


if __name__ == '__main__':
    sys.exit(main())
