#!/usr/bin/env python3
"""Runs the REAL Sources/ctr/svc_entry.s (assembled by build_entry.sh) in an ARM11 emulator and checks what the swap needs of it.

    python entry_sim.py <sim-dir>        (sim-dir from build_entry.sh: entry.bin, symbols.txt)

The two entries are entered the way the per-site trampoline enters them: `push {lr}` done, lr = the trampoline's return address, r0 = the
handle, every other register and the flags the game's. What is held:

  * the counter of the threads inside an entry (azl_hooks_in_flight) is one more inside the C handler, and back where it was after, whatever the
    counter was before (an entry that does not count is a swap that frees the block under a thread);
  * the registers and flags the game had come back untouched (r0 aside), the C code being free to clobber what AAPCS lets it, with the stack at the
    eight bytes AAPCS wants whenever C is called;
  * APT:ReceiveParameter(application) (command buffer 0x000D0080, 0x300) asks azl_handback_site first, with the handle and the trampoline's return
    address; no other call does, and azl_svc23_entry never does;
  * when it answers with a call site the C handler is NOT called, the counter is back, and the thread is at that site with the registers, the flags,
    the stack pointer and lr the game had when it reached the site (the trampoline's push undone), the stack above intact;
  * when it answers 0 the call goes the normal way.
"""
import random
import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM, Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_C13_C0_3, UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC,
                               UC_ARM_REG_R0, UC_ARM_REG_SP)

try:
    from unicorn.arm_const import UC_CPU_ARM_1176
except ImportError:                      # older bindings: the default core
    UC_CPU_ARM_1176 = None

ENTRY_BASE = 0x07000100
TRAMP_RET = 0x07100008        # the `bl` of the trampoline returns here (the sim stops when the entry gets back)
SITE = 0x00123450             # the call site the hand-back goes to
TLS = 0x0D000000
STACK_TOP = 0x0E0FF000
RECEIVE = 0x000D0080
APPLICATION = 0x300

SYMS = {}
failures = 0
checks = 0


def check(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print('  FAIL: ' + what)


class Run:
    def __init__(self, entry, header, arg, handback_answer, counter0, seed, sp_misaligned):
        self.rnd = random.Random(seed)
        self.entry = entry
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        if UC_CPU_ARM_1176 is not None:
            uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
        uc.mem_map(0x07000000, 0x200000)
        uc.mem_write(ENTRY_BASE, ENTRY_CODE)
        uc.mem_map(0x00100000, 0x100000)
        uc.mem_map(STACK_TOP - 0xF000, 0x10000)
        uc.mem_map(TLS, 0x1000)
        self.handback_answer = handback_answer
        self.log = []           # what C was called with
        self.counter_in_c = []  # the counter, as C saw it
        self.sp_in_c = []
        self.done_at = None
        uc.mem_write(SYMS['azl_hooks_in_flight'], struct.pack('<I', counter0))
        uc.mem_write(TLS + 0x80, struct.pack('<II', header, arg))
        uc.reg_write(UC_ARM_REG_C13_C0_3, TLS)
        # the game's state when its code reached the call site
        rnd = self.rnd
        self.flags = rnd.getrandbits(4) << 28
        uc.reg_write(UC_ARM_REG_CPSR, 0x10 | self.flags)      # user mode first: the banked sp and lr
        self.regs = [rnd.getrandbits(32) for _ in range(13)]
        self.handle = self.regs[0] = rnd.getrandbits(32) | 1
        for i, v in enumerate(self.regs):
            uc.reg_write(UC_ARM_REG_R0 + i, v)
        self.game_sp = STACK_TOP - 0x2000 - 8 * rnd.randrange(0, 64) - (4 if sp_misaligned else 0)
        self.game_lr = rnd.getrandbits(32) & ~3
        # `push {lr}` of the trampoline, then `bl entry`
        self.entry_sp = self.game_sp - 4
        uc.mem_write(self.entry_sp, struct.pack('<I', self.game_lr))
        self.above = bytes(uc.mem_read(self.game_sp, 64))        # the game's frame, untouched by anyone
        uc.reg_write(UC_ARM_REG_SP, self.entry_sp)
        uc.reg_write(UC_ARM_REG_LR, TRAMP_RET)
        uc.hook_add(UC_HOOK_CODE, self.on_c, begin=SYMS['azl_hook_SendSyncRequest'], end=SYMS['azl_hook_SendSyncRequest'])
        uc.hook_add(UC_HOOK_CODE, self.on_c, begin=SYMS['azl_hook_CloseHandle'], end=SYMS['azl_hook_CloseHandle'])
        uc.hook_add(UC_HOOK_CODE, self.on_c, begin=SYMS['azl_handback_site'], end=SYMS['azl_handback_site'])
        uc.hook_add(UC_HOOK_CODE, self.on_done, begin=TRAMP_RET, end=TRAMP_RET)
        uc.hook_add(UC_HOOK_CODE, self.on_done, begin=SITE, end=SITE)
        self.count = 0
        uc.hook_add(UC_HOOK_CODE, self.on_count)

    def counter(self):
        return struct.unpack('<I', bytes(self.uc.mem_read(SYMS['azl_hooks_in_flight'], 4)))[0]

    def on_count(self, uc, address, size, user):
        self.count += 1
        if self.count > 400:
            uc.emu_stop()

    def on_c(self, uc, address, size, user):
        name = [n for n in ('azl_hook_SendSyncRequest', 'azl_hook_CloseHandle', 'azl_handback_site') if SYMS[n] == address][0]
        r0 = uc.reg_read(UC_ARM_REG_R0)
        r1 = uc.reg_read(UC_ARM_REG_R0 + 1)
        self.log.append((name, r0, r1))
        self.counter_in_c.append(self.counter())
        self.sp_in_c.append(uc.reg_read(UC_ARM_REG_SP))
        # compiled C clobbers what AAPCS lets it, flags included
        for i, reg in enumerate((1, 2, 3, 12)):
            uc.reg_write(UC_ARM_REG_R0 + reg, 0xBAD00000 + i)
        uc.reg_write(UC_ARM_REG_CPSR, (uc.reg_read(UC_ARM_REG_CPSR) & 0x0FFFFFFF) | (self.flags ^ 0xF0000000))
        uc.reg_write(UC_ARM_REG_R0, self.handback_answer if name == 'azl_handback_site' else 0x600D0000 | (self.handle & 0xFFFF))
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def on_done(self, uc, address, size, user):
        self.done_at = address
        uc.emu_stop()

    def go(self):
        try:
            self.uc.emu_start(self.entry, 0)
            self.err = None
        except UcError as e:
            self.err = str(e)
        return self

    def reg(self, i):
        return self.uc.reg_read(UC_ARM_REG_R0 + i)

    def sp(self):
        return self.uc.reg_read(UC_ARM_REG_SP)

    def lr(self):
        return self.uc.reg_read(UC_ARM_REG_LR)

    def cpsr_flags(self):
        return self.uc.reg_read(UC_ARM_REG_CPSR) & 0xF0000000


def game_state_restored(r, what, lr_expected, sp_expected, r0_expected):
    check(r.err is None, '%s: the emulator stopped on an error: %s' % (what, r.err))
    for i in range(1, 13):
        check(r.reg(i) == r.regs[i], '%s: r%d is 0x%08x, the game had 0x%08x' % (what, i, r.reg(i), r.regs[i]))
    check(r.reg(0) == r0_expected, '%s: r0 is 0x%08x, expected 0x%08x' % (what, r.reg(0), r0_expected))
    check(r.cpsr_flags() == r.flags, '%s: the flags are 0x%08x, the game had 0x%08x' % (what, r.cpsr_flags(), r.flags))
    check(r.lr() == lr_expected, '%s: lr is 0x%08x, expected 0x%08x' % (what, r.lr(), lr_expected))
    check(r.sp() == sp_expected, '%s: sp is 0x%08x, expected 0x%08x' % (what, r.sp(), sp_expected))
    check(bytes(r.uc.mem_read(r.game_sp, 64)) == r.above, '%s: the game\'s frame above the call changed' % what)


def scenario_normal(entry_name, handler, header, arg, seed, counter0, mis):
    r = Run(SYMS[entry_name], header, arg, 0, counter0, seed, mis).go()
    what = '%s header 0x%08x arg 0x%x seed %d counter0 %d' % (entry_name, header, arg, seed, counter0)
    check(r.done_at == TRAMP_RET, '%s: it did not get back to the trampoline (%s)' % (what, r.done_at))
    check([l[0] for l in r.log] == [handler], '%s: C was called as %s, expected only %s' % (what, [l[0] for l in r.log], handler))
    check(r.counter_in_c == [counter0 + 1], '%s: the counter in C was %s, expected %d' % (what, r.counter_in_c, counter0 + 1))
    check(all(sp & 7 == 0 for sp in r.sp_in_c), '%s: the stack is not aligned to eight in C' % what)
    check(r.counter() == counter0, '%s: the counter is %d after the call, expected %d' % (what, r.counter(), counter0))
    # the trampoline's return: lr is the trampoline's, sp is below the trampoline's push (it pops it itself)
    game_state_restored(r, what, TRAMP_RET, r.entry_sp, 0x600D0000 | (r.handle & 0xFFFF))
    return r


def scenario_receive(answer, seed, counter0, mis):
    r = Run(SYMS['azl_svc32_entry'], RECEIVE, APPLICATION, answer, counter0, seed, mis).go()
    what = 'ReceiveParameter answer 0x%x seed %d counter0 %d' % (answer, seed, counter0)
    check(r.log and r.log[0][0] == 'azl_handback_site', '%s: azl_handback_site was not asked first (%s)' % (what, r.log))
    check(r.log and r.log[0][1] == r.handle and r.log[0][2] == TRAMP_RET,
          '%s: azl_handback_site got (0x%x, 0x%x), expected (0x%x, 0x%x)' % (what, r.log[0][1], r.log[0][2], r.handle, TRAMP_RET))
    check(r.counter_in_c[0] == counter0 + 1, '%s: the counter in azl_handback_site was %d, expected %d' % (what, r.counter_in_c[0], counter0 + 1))
    check(all(sp & 7 == 0 for sp in r.sp_in_c), '%s: the stack is not aligned to eight in C' % what)
    check(r.counter() == counter0, '%s: the counter is %d after the call, expected %d' % (what, r.counter(), counter0))
    if answer:
        check(r.done_at == SITE, '%s: the thread did not end up at the call site (%s)' % (what, r.done_at))
        check([l[0] for l in r.log] == ['azl_handback_site'], '%s: C was called as %s, expected only the hand-back question' % (what, [l[0] for l in r.log]))
        # at the site: every register, the flags, sp and lr as the game had them when its code reached the site
        game_state_restored(r, what, r.game_lr, r.game_sp, r.handle)
    else:
        check(r.done_at == TRAMP_RET, '%s: it did not get back to the trampoline (%s)' % (what, r.done_at))
        check([l[0] for l in r.log] == ['azl_handback_site', 'azl_hook_SendSyncRequest'],
              '%s: C was called as %s' % (what, [l[0] for l in r.log]))
        check(r.counter_in_c == [counter0 + 1, counter0 + 1], '%s: the counter in C was %s' % (what, r.counter_in_c))
        game_state_restored(r, what, TRAMP_RET, r.entry_sp, 0x600D0000 | (r.handle & 0xFFFF))
    return r


def main():
    global ENTRY_CODE
    sim = sys.argv[1]
    ENTRY_CODE = open(sim + '/entry.bin', 'rb').read()
    for line in open(sim + '/symbols.txt'):
        name, addr = line.split()
        SYMS[name] = int(addr, 16)
    seeds = range(40)
    for seed in seeds:
        for counter0 in (0, 1, 7):
            mis = bool(seed & 1)
            # an ordinary call, and the ones that look alike: other commands, the right command for another application id
            scenario_normal('azl_svc32_entry', 'azl_hook_SendSyncRequest', 0x00050100, 0, seed, counter0, mis)
            scenario_normal('azl_svc32_entry', 'azl_hook_SendSyncRequest', RECEIVE, 0x301, seed, counter0, mis)
            scenario_normal('azl_svc32_entry', 'azl_hook_SendSyncRequest', 0x000E0080, APPLICATION, seed, counter0, mis)   # GlanceParameter: Luma3DS does not raise the swap on it
            scenario_normal('azl_svc32_entry', 'azl_hook_SendSyncRequest', RECEIVE ^ 0x10000, APPLICATION, seed, counter0, mis)
            # CloseHandle never asks, whatever is in the command buffer
            scenario_normal('azl_svc23_entry', 'azl_hook_CloseHandle', RECEIVE, APPLICATION, seed, counter0, mis)
            # the call that raises the swap: handed back to its call site, or not
            scenario_receive(0, seed, counter0, mis)
            scenario_receive(0x00123450, seed, counter0, mis)
    print('entry_sim: %d checks, %d failures' % (checks, failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
