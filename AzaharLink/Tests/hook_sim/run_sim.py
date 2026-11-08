#!/usr/bin/env python3
"""Runs the game's real functions around its svc call sites in an ARM emulator, unhooked and hooked through the REAL gate, and compares them.

    python Tests/hook_sim/run_sim.py <code.bin> <sim-dir> [--seeds N]
        (sim-dir from patch_image: patched.bin, page.bin, layout.txt, sites.txt; unicorn on the host)

The hooked image is what the plugin leaves in the game: ONE word `b stub` at every `svc 0x32` / `svc 0x23`, the stubs and the gate
(Sources/ctr/gate.s, assembled by build_gate.sh) in a page below 0x01E80000, all written by the plugin's own WriteGate (azl/gate_plan.hpp,
through patch_image.cpp). For every function that holds a call site, from random register state, four hooked runs are compared with the
unhooked one:
  * gate open, pre answers the call itself (action 0): the stand-in plugin records the call as the kernel would and answers it;
  * gate open, pre asks for the svc (action 1), or for the svc and then post (action 2): the gate makes the svc from its page;
  * gate closed (flag = 0, as during the swap): the gate makes the svc and never calls the plugin.
They must be the same as the unhooked run: the calls seen (svc number, r0, command buffer) and their answers, the final registers and
condition flags, the live stack (from sp up), the TLS and the argument memory. Also held, on every hooked run: the plugin is called only
with the gate open, with sp on eight bytes, pre with (handle, stub+8 of a stub of the page, ctx = sp), post with the stub of its pre and
ctx[3] = the action; the plugin may clobber r1-r3, r12 and the flags (it does, on purpose); the gate's count is back to zero at the end;
nothing is written in the game's text, nor in the page's code and stubs (only the header's words move).
"""
import random
import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_C13_C0_3, UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12, UC_ARM_REG_SP)

try:
    from unicorn.arm_const import UC_CPU_ARM_1176
except ImportError:
    UC_CPU_ARM_1176 = None

BASE = 0x00100000
PLUGIN = 0x07000000
TLS = 0x0D000000
STACK_TOP = 0x0E0FF000
TRAP = 0x0C000000
ARG_BASE = 0x08000000
ARG_SIZE = 0x10000
MAX_INSN = 20000          # the gate adds a few dozen instructions per call: the same limit for every run
GATE_CODE = 0x64          # azl/gate_plan.hpp kGateCode: the header is below it
GATE_IN_FLIGHT = 0x04
BX_LR = 0xE12FFF1E
MODES = (('open', 0), ('open', 1), ('open', 2), ('closed', 1))


def result_for(n):
    return (0, 0xE0E01BF9, 0x12345678, 0x80000000, 0xD8E007F7)[n % 5]


def page_up(n):
    return (n + 0xFFF) & ~0xFFF


class Layout:
    def __init__(self, sim):
        v = {}
        for line in open(sim + '/layout.txt'):
            k, x = line.split()
            v[k] = int(x, 16) if k not in ('pages', 'sites') else int(x)
        self.page = v['page']
        self.page_bytes = v['pages'] * 0x1000
        self.stubs = v['stubs']
        self.sites = v['sites']
        self.text = v['text']
        self.plugin = {v['pre32']: ('pre', 0x32), v['post32']: ('post', 0x32), v['pre23']: ('pre', 0x23), v['post23']: ('post', 0x23)}


class Run:
    def __init__(self, image, lay=None, page=None, mode=None, action=1):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        if UC_CPU_ARM_1176 is not None:
            uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
        uc.mem_map(BASE, page_up(len(image)))
        uc.mem_write(BASE, image)
        uc.mem_map(STACK_TOP - 0xF000, 0x10000)
        uc.mem_map(TLS, 0x1000)
        uc.mem_map(ARG_BASE, ARG_SIZE)
        uc.mem_map(TRAP, 0x1000)
        uc.reg_write(UC_ARM_REG_C13_C0_3, TLS)
        self.lay = lay
        self.mode = mode
        self.action = action
        self.trace = []
        self.done = False
        self.errors = []
        self.pres = self.posts = 0
        self.pre_stub = None
        self.code_writes = 0
        uc.hook_add(UC_HOOK_INTR, self.on_svc)
        uc.hook_add(UC_HOOK_CODE, self.on_trap, begin=TRAP, end=TRAP)
        if lay is not None:
            uc.mem_map(lay.page, lay.page_bytes)
            uc.mem_write(lay.page, page)
            if mode == 'closed':
                uc.mem_write(lay.page, struct.pack('<I', 0))   # the flag: the swap closed the gate
            uc.mem_map(PLUGIN, 0x1000)
            uc.mem_write(PLUGIN, struct.pack('<I', BX_LR) * 0x400)
            for addr in lay.plugin:
                uc.hook_add(UC_HOOK_CODE, self.on_plugin, begin=addr, end=addr)
            # nothing but the gate's header may be written: not the game's text, not the gate's code, not the stubs
            uc.hook_add(UC_HOOK_MEM_WRITE, self.on_code_write, begin=BASE, end=BASE + lay.text - 1)
            uc.hook_add(UC_HOOK_MEM_WRITE, self.on_code_write, begin=lay.page + GATE_CODE, end=lay.page + lay.page_bytes - 1)

    def record(self, svc):
        uc = self.uc
        r0 = uc.reg_read(UC_ARM_REG_R0)
        cmd = struct.unpack('<16I', bytes(uc.mem_read(TLS + 0x80, 64)))
        self.trace.append((svc, r0, cmd))
        uc.reg_write(UC_ARM_REG_R0, result_for(len(self.trace) - 1))

    def on_svc(self, uc, intno, user):
        pc = uc.reg_read(UC_ARM_REG_PC)
        svc = struct.unpack('<I', bytes(uc.mem_read(pc - 4, 4)))[0] & 0xFFFFFF
        self.record(svc)

    def fail(self, what):
        if len(self.errors) < 4:
            self.errors.append(what)

    def on_plugin(self, uc, address, size, user):
        kind, svc = self.lay.plugin[address]
        sp = uc.reg_read(UC_ARM_REG_SP)
        r1, r2 = uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2)
        if self.mode != 'open':
            self.fail('%s called with the gate closed' % kind)
        if sp & 7:
            self.fail('%s called with sp 0x%08x (not on eight bytes)' % (kind, sp))
        if r2 != sp:
            self.fail('%s: ctx 0x%08x is not sp 0x%08x' % (kind, r2, sp))
        if kind == 'pre':
            self.pres += 1
            off = r1 - 8 - self.lay.stubs
            if off < 0 or off % 12 or off // 12 >= self.lay.sites:
                self.fail('pre: stub+8 0x%08x is none of the stubs' % r1)
            self.pre_stub = r1
            if self.action == 0:
                self.record(svc)                       # the plugin answers: the call as the kernel would see it, and its answer in r0
            uc.reg_write(UC_ARM_REG_R1, self.action)
        else:
            self.posts += 1
            if self.action < 2:
                self.fail('post called for action %d' % self.action)
            if r1 != self.pre_stub:
                self.fail('post: stub+8 0x%08x, its pre had 0x%08x' % (r1, self.pre_stub))
            ctx3 = struct.unpack('<I', bytes(uc.mem_read(r2 + 12, 4)))[0]
            if ctx3 != self.action:
                self.fail('post: ctx[3] = %d, not the action %d' % (ctx3, self.action))
            uc.reg_write(UC_ARM_REG_R1, 0xBAD00001)
        # what compiled C may clobber: the gate must give the game its own back
        if kind == 'pre':
            uc.reg_write(UC_ARM_REG_R2, 0xBAD00002)
        else:
            uc.reg_write(UC_ARM_REG_R2, 0xBAD00012)
        uc.reg_write(UC_ARM_REG_R3, 0xBAD00003)
        uc.reg_write(UC_ARM_REG_R12, 0xBAD0000C)
        uc.reg_write(UC_ARM_REG_CPSR, (uc.reg_read(UC_ARM_REG_CPSR) & 0x0FFFFFFF) | 0xA0000000)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))  # bx lr

    def on_code_write(self, uc, access, address, size, value, user):
        self.code_writes += 1

    def on_trap(self, uc, address, size, user):
        self.done = True
        uc.emu_stop()

    def in_flight(self):
        return struct.unpack('<I', bytes(self.uc.mem_read(self.lay.page + GATE_IN_FLIGHT, 4)))[0]

    def state(self):
        uc = self.uc
        regs = [uc.reg_read(r) for r in range(UC_ARM_REG_R0, UC_ARM_REG_R0 + 13)]
        regs += [uc.reg_read(UC_ARM_REG_SP), uc.reg_read(UC_ARM_REG_LR), uc.reg_read(UC_ARM_REG_CPSR) & 0xF0000000]
        # only the live part of the stack (from sp up): the gate's frame below sp is invisible to the game
        sp = uc.reg_read(UC_ARM_REG_SP)
        live = bytes(uc.mem_read(sp, STACK_TOP - sp)) if STACK_TOP - 0x2000 <= sp <= STACK_TOP else b'?'
        mem = (live, bytes(uc.mem_read(TLS, 0x1000)), bytes(uc.mem_read(ARG_BASE, ARG_SIZE)))
        return regs, mem


def find_function_start(code, site):
    off = site - BASE
    for k in range(1, 300):
        if off - 4 * k < 0:
            return None
        w = struct.unpack_from('<I', code, off - 4 * k)[0]
        if (w & 0xFFFF0000) == 0xE92D0000 and (w & 0x4000):
            return site - 4 * k
    return None


def run_once(image, start, seed, lay=None, page=None, mode=None, action=1):
    rnd = random.Random(seed)
    r = Run(image, lay, page, mode, action)
    uc = r.uc
    # argument memory with pseudo-random contents (pointers into itself where derefs are likely)
    blob = bytearray()
    for i in range(ARG_SIZE // 4):
        blob += struct.pack('<I', ARG_BASE + (rnd.randrange(0, ARG_SIZE // 4) * 4) if rnd.random() < 0.5 else rnd.getrandbits(32))
    uc.mem_write(ARG_BASE, bytes(blob))
    # user mode first: writing CPSR swaps the banked SP/LR, so it must precede them
    flags = rnd.getrandbits(4) << 28
    uc.reg_write(UC_ARM_REG_CPSR, 0x10 | flags)
    for i in range(13):
        uc.reg_write(UC_ARM_REG_R0 + i, ARG_BASE + rnd.randrange(0, ARG_SIZE // 4 - 64) * 4)
    uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
    uc.reg_write(UC_ARM_REG_LR, TRAP)
    try:
        uc.emu_start(start, TRAP + 0x100, count=MAX_INSN)
        err = None
    except UcError as e:
        err = str(e)
    return r, err


def main():
    args = sys.argv[1:]
    seeds = 3
    if '--seeds' in args:
        i = args.index('--seeds')
        seeds = int(args[i + 1])
        del args[i:i + 2]
    code = open(args[0], 'rb').read()
    sim = args[1]
    lay = Layout(sim)
    patched = open(sim + '/patched.bin', 'rb').read()
    page = open(sim + '/page.bin', 'rb').read()
    if struct.unpack_from('<I', page, 0)[0] != 1:
        print('the page from patch_image does not have the gate open (flag != 1)')
        return 1
    sites = [(int(a, 16), k) for a, k in (l.split() for l in open(sim + '/sites.txt'))]

    ok = mismatch = incomplete = faulted = broken = 0
    pres = posts = 0
    starts = set()
    for site, kind in sites:
        start = find_function_start(code, site)
        if start is None or start in starts:
            continue
        starts.add(start)
        for seed in range(seeds):
            s = seed * 1000 + (start >> 2)
            a, ea = run_once(code, start, s)
            for mode, action in MODES:
                b, eb = run_once(patched, start, s, lay, page, mode, action)
                tag = '%s/%d' % (mode, action)
                if b.errors or b.code_writes:
                    broken += 1
                    print('BROKEN %s at function 0x%08x site 0x%08x: %s%s' % (tag, start, site, '; '.join(b.errors),
                                                                              ' | %d write(s) into code' % b.code_writes if b.code_writes else ''))
                    continue
                if ea or eb:
                    faulted += 1
                    if bool(ea) != bool(eb):
                        mismatch += 1
                        print('MISMATCH %s (only one run faulted) at function 0x%08x site 0x%08x: %s | %s' % (tag, start, site, ea, eb))
                    continue
                if not (a.done and b.done):
                    incomplete += 1
                    continue
                if b.in_flight() != 0:
                    broken += 1
                    print('BROKEN %s at function 0x%08x: the gate\'s count is %d at the end' % (tag, start, b.in_flight()))
                    continue
                pres += b.pres
                posts += b.posts
                sa, sb = a.state(), b.state()
                if a.trace != b.trace or sa != sb:
                    mismatch += 1
                    print('MISMATCH %s at function 0x%08x site 0x%08x (%s)' % (tag, start, site, kind))
                    if a.trace != b.trace:
                        print('   calls differ:', [(hex(t[0]), hex(t[1])) for t in a.trace[:3]], [(hex(t[0]), hex(t[1])) for t in b.trace[:3]])
                    if sa[0] != sb[0]:
                        print('   regs  orig:', [hex(x) for x in sa[0]])
                        print('   regs hook :', [hex(x) for x in sb[0]])
                    if sa[1] != sb[1]:
                        print('   memory differs')
                else:
                    ok += 1
    print('sites: %d, functions: %d, hooked runs identical to the unhooked one: %d (gate open: pre %d, post %d; gate closed too), '
          'mismatches: %d, broken: %d, faulted-in-both: %d, did-not-finish: %d'
          % (len(sites), len(starts), ok, pres, posts, mismatch, broken, faulted, incomplete))
    return 1 if (mismatch or broken or ok == 0 or pres == 0 or posts == 0) else 0


if __name__ == '__main__':
    sys.exit(main())
