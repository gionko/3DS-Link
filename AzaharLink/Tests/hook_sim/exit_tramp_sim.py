#!/usr/bin/env python3
"""Runs the REAL Sources/ctr/exit_tramp.s (assembled by build_gate.sh at 0x01E7F000) in an ARM11 emulator, as the plugin installs it: a copy
placed elsewhere (it must be position independent), a game's `svc 0x03` word turned into `b tramp`, the guard a stand-in at 0x07000000.

    python Tests/hook_sim/exit_tramp_sim.py <sim-dir>      (sim-dir from build_gate.sh; unicorn on the host)

What is held (REGISTRO phase Q):
  * one thread: the guard is called once, with the state at 1 (a second thread waits), on the plugin's own stack (sp 8-byte aligned, inside it), with lr back into the copy; then the ONLY
    other svc is 0x03, made with the game's own sp (4-aligned or 8-aligned: both), and the state word is 2;
  * a second thread that comes after the first (state 1: the guard runs; state 2: done): no guard call, svcSleepThread(1 ms) (r0:r1 =
    1000000:0) again and again, and NEVER a svc 0x03 of its own (the first thread's ends the process);
  * no guard set (fn 0 or stack 0): no call, svc 0x03 at once, state 2;
  * nothing is written outside the copy's state word and the guard's stack (the game's stack is untouched).
"""
import os
import struct
import sys

from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc, UcError
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP

ASSEMBLED_AT = 0x01E7F000
COPY = 0x01E7EC48        # where the plugin puts it (after the stubs): 8-byte aligned, not page aligned
SITE = 0x00104000
GUARD = 0x07000000
GSTACK = 0x07100000      # the guard's stack: [GSTACK, GSTACK + 0x2000), top 8-byte aligned
GSTACK_TOP = GSTACK + 0x2000
GAME_STACK = 0x0FF00000
FAILS = []


def W(x):
    return struct.pack('<I', x & 0xFFFFFFFF)


def B(frm, to):
    off = (to - frm - 8) >> 2
    return 0xEA000000 | (off & 0xFFFFFF)


def syms(path):
    out = {}
    for line in open(path):
        p = line.split()
        if len(p) == 2:
            out[p[0]] = int(p[1], 16)
    return out


def fail(what):
    FAILS.append(what)
    print('  FAIL: ' + what)


def run(label, tramp, S, state0, fn, stack, game_sp, flip_after_sleeps=None, expect_exit=True):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(0x01E7E000, 0x2000)
    uc.mem_map(SITE & ~0xFFF, 0x1000)
    uc.mem_map(GUARD, 0x1000)
    uc.mem_map(GSTACK, 0x2000)
    uc.mem_map(GAME_STACK - 0x1000, 0x2000)
    uc.mem_write(COPY, tramp)
    off = lambda name: COPY + S[name] - S['azl_exit_tramp_begin']
    uc.mem_write(off('azl_exit_tramp_state'), W(state0))
    uc.mem_write(off('azl_exit_tramp_fn'), W(fn))
    uc.mem_write(off('azl_exit_tramp_stack'), W(stack))
    uc.mem_write(SITE, W(B(SITE, COPY)))
    # the guard: records sp and lr through its first instruction, then `bx lr`
    uc.mem_write(GUARD, W(0xE12FFF1E))
    game_stack_before = bytes(uc.mem_read(GAME_STACK - 0x100, 0x200))
    seen = {'guard': [], 'svcs': [], 'sleeps': 0, 'writes': []}

    def on_code(uc_, addr, size, _):
        if addr == GUARD:
            seen['guard'].append((uc_.reg_read(UC_ARM_REG_SP), uc_.reg_read(UC_ARM_REG_LR),
                                  struct.unpack('<I', uc_.mem_read(off('azl_exit_tramp_state'), 4))[0]))

    def on_write(uc_, access, addr, size, value, _):
        seen['writes'].append(addr)

    def on_svc(uc_, intno, _):
        pc = uc_.reg_read(UC_ARM_REG_PC)
        n = struct.unpack('<I', uc_.mem_read(pc - 4, 4))[0] & 0xFFFFFF
        seen['svcs'].append((n, uc_.reg_read(UC_ARM_REG_SP), uc_.reg_read(UC_ARM_REG_R0), uc_.reg_read(UC_ARM_REG_R1)))
        if n == 0x03:
            uc_.emu_stop()
        elif n == 0x0A:
            seen['sleeps'] += 1
            if flip_after_sleeps is not None and seen['sleeps'] == flip_after_sleeps:
                uc_.mem_write(off('azl_exit_tramp_state'), W(2)) # (the other thread's guard is done)
            if seen['sleeps'] >= 20:
                uc_.emu_stop() # (enough: a waiting thread sleeps until the process ends)
        else:
            uc_.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code, begin=GUARD, end=GUARD + 3)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    uc.hook_add(UC_HOOK_INTR, on_svc)
    uc.reg_write(UC_ARM_REG_SP, game_sp)
    uc.reg_write(UC_ARM_REG_LR, 0x0BADBAD0)
    try:
        uc.emu_start(SITE, 0xFFFFFFFF, count=20000)
    except UcError as e:
        fail('%s: fault %s at pc %08X' % (label, e, uc.reg_read(UC_ARM_REG_PC)))
        return
    state = struct.unpack('<I', uc.mem_read(off('azl_exit_tramp_state'), 4))[0]
    svcs = seen['svcs']
    if not expect_exit:
        if [s for s in svcs if s[0] != 0x0A]:
            fail('%s: a waiting thread made %s' % (label, [hex(s[0]) for s in svcs if s[0] != 0x0A]))
        for s in svcs:
            if (s[2], s[3]) != (1000000, 0):
                fail('%s: svcSleepThread(%d, %d), wanted 1 ms' % (label, s[2], s[3]))
                break
        return seen
    if not svcs or svcs[-1][0] != 0x03:
        fail('%s: did not end in svc 0x03 (%s)' % (label, svcs))
        return
    if svcs[-1][1] != game_sp:
        fail('%s: svc 0x03 made with sp %08X, the game had %08X' % (label, svcs[-1][1], game_sp))
    others = [s for s in svcs[:-1] if s[0] != 0x0A]
    if others:
        fail('%s: other svcs %s' % (label, others))
    for s in svcs[:-1]:
        if s[0] == 0x0A and (s[2], s[3]) != (1000000, 0):
            fail('%s: svcSleepThread(%d, %d), wanted 1 ms' % (label, s[2], s[3]))
    if state != 2:
        fail('%s: state %d at the svc, wanted 2' % (label, state))
    allowed = [off('azl_exit_tramp_state') + i for i in range(4)]
    for a in seen['writes']:
        if not (a in allowed or GSTACK <= a < GSTACK_TOP):
            fail('%s: write at %08X' % (label, a))
            break
    if bytes(uc.mem_read(GAME_STACK - 0x100, 0x200)) != game_stack_before:
        fail('%s: the game\'s stack was written' % label)
    return seen


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'analysis', 'gate_sim')
    S = syms(os.path.join(d, 'tramp.sym'))
    tramp = open(os.path.join(d, 'tramp.bin'), 'rb').read()
    assert S['azl_exit_tramp_begin'] == ASSEMBLED_AT, 'build_gate.sh assembles the trampoline at %08X' % ASSEMBLED_AT
    size = S['azl_exit_tramp_end'] - S['azl_exit_tramp_begin']
    tramp = tramp[:size]
    print('exit trampoline: %d bytes (the gate\'s page keeps 192)' % size)
    if size > 16 * 12 - 8:
        fail('the trampoline is %d bytes: ctr/hook.cpp kExitTrampStubs keeps %d' % (size, 16 * 12 - 8))
    for game_sp in (GAME_STACK, GAME_STACK - 4):
        seen = run('one thread sp%%8=%d' % (game_sp % 8), tramp, S, 0, GUARD, GSTACK_TOP, game_sp)
        if seen is not None:
            g = seen['guard']
            if len(g) != 1:
                fail('one thread: the guard called %d times' % len(g))
            else:
                sp, lr, st = g[0]
                if st != 1:
                    fail('one thread: the state is %d while the guard runs (a second thread would run it too)' % st)
                if sp % 8 != 0 or not (GSTACK <= sp < GSTACK_TOP):
                    fail('one thread: the guard runs with sp %08X (its stack %08X-%08X, 8-byte aligned)' % (sp, GSTACK, GSTACK_TOP))
                if not (COPY <= lr < COPY + size):
                    fail('one thread: the guard returns to %08X, not into the copy' % lr)
            if seen['sleeps']:
                fail('one thread: %d sleeps' % seen['sleeps'])
    for state0, label in ((1, 'second thread, the guard running'), (2, 'second thread, the guard done')):
        seen = run(label, tramp, S, state0, GUARD, GSTACK_TOP, GAME_STACK, flip_after_sleeps=3, expect_exit=False)
        if seen is not None and (seen['guard'] or seen['sleeps'] < 20):
            fail('%s: guard %d calls, %d sleeps (wanted 0 and sleeping on)' % (label, len(seen['guard']), seen['sleeps']))
    for fn, stack, label in ((0, GSTACK_TOP, 'no guard'), (GUARD, 0, 'no stack')):
        seen = run(label, tramp, S, 0, fn, stack, GAME_STACK)
        if seen is not None and seen['guard']:
            fail('%s: the guard was called' % label)
    print('exit_tramp_sim: %d failures' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
