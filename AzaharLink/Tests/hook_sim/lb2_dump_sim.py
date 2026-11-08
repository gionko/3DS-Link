#!/usr/bin/env python3
"""LEGO Batman 2 on an Old 3DS with 1.1.1 (a user's dumps 1 and 3): the REAL code and stack of the dumps, run twice.

    python Tests/hook_sim/lb2_dump_sim.py <dump 1> [<dump 3> ...] [--sim <dir from build_gate.sh>]

The dumps stay out of the repository (they are passed as arguments). What the dumps show: the shared stub `svc 0x32; bx lr` at
0x00207010 holds the two words of the old hook (`ldr pc, [pc, #-4]` + 0x070D4000) and the thread died at 0x0020702C writing to 0,
inside the next stub (svcWaitSynchronizationN), with [sp+4] = [sp+8] = its command buffer.

  * the old format (1.1.1 and 1.1.2): the thread wakes from the svc of ReceiveParameter at site+4, with the hook written back while
    it slept. From the dump's own stack (r0 and r4 are the words its next stub pushed) it must die as the dump did: a write to 0
    at 0x0020702C, with the dump's registers and the dump's words on the stack (they are wiped before the run). With Z clear
    the `streq` does not store: the dump's [sp+8] is not made, so Z was set.
  * the gate (Sources/ctr/gate.s, the game's two words put back, ONE word `b stub` at the site): the thread sleeps in the gate's
    svc while the swap closes the gate and takes the plugin's block away; whether it wakes before or after the block comes back,
    it runs the game's `bx lr` at 0x00207014 and returns to its caller (0x0012214C) with its stack at and above its SP untouched.
"""
import os
import struct
import sys

from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_MODE_ARM, Uc, UcError
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R4,
                               UC_ARM_REG_SP)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gate_sim import B, PAGE, PLUGIN, REGS, UC_CPU_ARM_1176, W, syms   # noqa: E402

SITE = 0x00207010
OLD_HOOK = (0xE51FF004, 0x070D4000)
GAME = (0xEF000032, 0xE12FFF1E)          # the game's own words (as in the EUR copy at 0x00206F00)
CRASH_PC = 0x0020702C
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print('    FAIL: ' + what)
    return cond


class Dump:
    def __init__(self, path):
        d = open(path, 'rb').read()
        h = struct.unpack_from('<10I', d, 0)
        assert h[:2] == (0xDEADC0DE, 0xDEADCAFE), 'not a Luma3DS dump'
        rs, cs, ss = h[6], h[7], h[8]
        self.regs = struct.unpack_from('<%dI' % (rs // 4), d, 40)
        self.pc, self.sp, self.lr = self.regs[15], self.regs[13], self.regs[14]
        self.code = d[40 + rs:40 + rs + cs]
        self.base = self.pc + 4 - cs
        self.stack = d[40 + rs + cs:40 + rs + cs + ss]
        self.name = os.path.basename(path)

    def word(self, a):
        return struct.unpack_from('<I', self.code, a - self.base)[0]

    def sword(self, a):
        return struct.unpack_from('<I', self.stack, a - self.sp)[0]


def machine(dump, code):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    if UC_CPU_ARM_1176 is not None:
        uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    uc.mem_map(dump.base & ~0xFFF, 0x2000)
    uc.mem_write(dump.base, code)
    uc.mem_map(dump.sp & ~0xFFF, 0x1000)
    uc.mem_write(dump.sp, dump.stack)
    uc.mem_map(dump.lr & ~0xFFF, 0x1000)
    return uc


def regs_from(uc, dump, keep):
    for i in range(13):
        if i not in keep:
            uc.reg_write(REGS[i], dump.regs[i])


def old_format(dump, zflag):
    """The thread wakes at site+4 on the old hook (the dump's own code), from its svc of ReceiveParameter."""
    sp_entry = dump.sp + 8                       # the next stub pushed {r0, r4} before it died
    r0, r4 = dump.sword(dump.sp), dump.sword(dump.sp + 4)
    uc = machine(dump, dump.code)
    for a in (dump.sp, dump.sp + 4, sp_entry):
        uc.mem_write(a, W(0x0BADF00D))           # what the crash wrote there: the run must write it again
    uc.reg_write(UC_ARM_REG_CPSR, 0x10 | (0x40000000 if zflag else 0))   # (first: sp and lr are banked by mode)
    regs_from(uc, dump, keep=(0, 1, 2, 4))
    uc.reg_write(UC_ARM_REG_R0, r0)
    uc.reg_write(UC_ARM_REG_R4, r4)
    uc.reg_write(UC_ARM_REG_SP, sp_entry)
    uc.reg_write(UC_ARM_REG_LR, dump.lr)
    out = {'svc': [], 'fault': None, 'literal': False}

    def intr(u, no, _):
        pc = u.reg_read(UC_ARM_REG_PC)
        out['svc'].append(struct.unpack('<I', u.mem_read(pc - 4, 4))[0] & 0xFFFFFF)
        u.reg_write(UC_ARM_REG_R0, dump.regs[0])     # the kernel's answer in the dump (OutOfRange)
        u.reg_write(UC_ARM_REG_R1, dump.regs[1])

    def bad(u, access, addr, size, value, _):
        out['fault'] = (addr, u.reg_read(UC_ARM_REG_PC))
        return False

    def code(u, addr, size, _):
        if addr == SITE + 4:
            out['literal'] = True
    uc.hook_add(UC_HOOK_INTR, intr)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, bad)
    uc.hook_add(UC_HOOK_CODE, code, begin=SITE, end=SITE + 8)
    try:
        uc.emu_start(SITE + 4, dump.lr, count=40)
    except UcError:
        pass
    regs = [uc.reg_read(r) for r in REGS]
    stack = [struct.unpack('<I', uc.mem_read(dump.sp + 4 * i, 4))[0] for i in range(3)]
    same = regs == list(dump.regs[:13]) and uc.reg_read(UC_ARM_REG_SP) == dump.sp
    want_stack = [dump.sword(dump.sp + 4 * i) for i in range(3)]
    print('  old format, Z=%d: literal run %s, svc %s, fault %s, registers and sp %s the dump\'s, stack %s' % (
        zflag, out['literal'], [hex(s) for s in out['svc']],
        ('write to %08X at pc %08X' % out['fault']) if out['fault'] else '-', 'are' if same else 'are NOT',
        'as in the dump' if stack == want_stack else '%s (dump %s)' % (['%08X' % x for x in stack], ['%08X' % x for x in want_stack])))
    return out['literal'] and out['fault'] == (0, CRASH_PC) and same, stack == want_stack


def gate(dump, sim_dir, back_before_wake):
    """The same thread through the gate: asleep in the gate's svc while the swap comes and goes."""
    G = syms(os.path.join(sim_dir, 'gate.sym'))
    PL = syms(os.path.join(sim_dir, 'plugin.sym'))
    blob = open(os.path.join(sim_dir, 'gate.bin'), 'rb').read()
    plug = open(os.path.join(sim_dir, 'plugin.bin'), 'rb').read()
    code = bytearray(dump.code)
    struct.pack_into('<2I', code, SITE - dump.base, *GAME)
    sp_entry = dump.sp + 8
    uc = machine(dump, bytes(code))
    above = bytes(uc.mem_read(sp_entry, dump.sp + len(dump.stack) - sp_entry))
    uc.mem_map(PAGE, 0x2000)
    uc.mem_write(PAGE, blob)
    for slot, fn in (('azl_gate_pre32', 'plugin_pre'), ('azl_gate_post32', 'plugin_post'),
                     ('azl_gate_pre23', 'plugin_pre'), ('azl_gate_post23', 'plugin_post')):
        uc.mem_write(G[slot], W(PL[fn]))
    uc.mem_write(G['azl_gate_flag'], W(1))
    stub = PAGE + ((len(blob) + 15) & ~15)
    uc.mem_write(stub, W(0xE52DE004) + W(B(stub + 4, G['azl_gate_32'], True)) + W(B(stub + 8, SITE + 4)))
    uc.mem_write(SITE, W(B(SITE, stub)))
    uc.mem_map(PLUGIN, 0x1000)
    uc.mem_write(PLUGIN, plug)
    uc.mem_write(PL['plugin_action'], W(2))           # Old 3DS, ReceiveParameter: the svc, then post
    uc.reg_write(UC_ARM_REG_CPSR, 0x40000010)
    regs_from(uc, dump, keep=(0, 4))
    uc.reg_write(UC_ARM_REG_R0, 0x001D0041)          # the APT session
    uc.reg_write(UC_ARM_REG_R4, dump.sword(dump.sp + 4))
    uc.reg_write(UC_ARM_REG_SP, sp_entry)
    uc.reg_write(UC_ARM_REG_LR, dump.lr)
    out = {'svc': [], 'fault': None, 'bx_lr': False, 'swapped': False, 'post': 0}
    mapped = [True]

    def word(a):
        return struct.unpack('<I', uc.mem_read(a, 4))[0]

    def intr(u, no, _):
        pc = u.reg_read(UC_ARM_REG_PC)
        n = word(pc - 4) & 0xFFFFFF
        out['svc'].append((n, pc - 4))
        if n == 0x32 and not out['swapped']:
            # asleep in the gate's svc: SWAP_ENTER closes the gate and the block goes
            out['swapped'] = True
            out['in_flight_asleep'] = word(G['azl_gate_in_flight'])
            uc.mem_write(G['azl_gate_flag'], W(0))
            uc.mem_unmap(PLUGIN, 0x1000)
            mapped[0] = False
            if back_before_wake:
                uc.mem_map(PLUGIN, 0x1000)
                uc.mem_write(PLUGIN, plug)
                uc.mem_write(PL['plugin_action'], W(2))
                uc.mem_write(G['azl_gate_flag'], W(1))
                mapped[0] = True
        u.reg_write(UC_ARM_REG_R0, 0)

    def bad(u, access, addr, size, value, _):
        out['fault'] = (addr, u.reg_read(UC_ARM_REG_PC))
        return False

    def code_hook(u, addr, size, _):
        if addr == SITE + 4 and word(addr) == GAME[1]:
            out['bx_lr'] = True
        if addr == PL['plugin_post']:
            out['post'] += 1
    uc.hook_add(UC_HOOK_INTR, intr)
    uc.hook_add(UC_HOOK_MEM_UNMAPPED, bad)
    uc.hook_add(UC_HOOK_CODE, code_hook, begin=SITE, end=SITE + 8)
    uc.hook_add(UC_HOOK_CODE, code_hook, begin=PLUGIN, end=PLUGIN + 0xFFF)
    try:
        uc.emu_start(SITE, dump.lr, count=400)
    except UcError:
        pass
    end_pc = uc.reg_read(UC_ARM_REG_PC)
    intact = bytes(uc.mem_read(sp_entry, len(above))) == above
    r0 = uc.reg_read(UC_ARM_REG_R0)
    if not mapped[0]:
        uc.mem_map(PLUGIN, 0x1000)
    skipped = word(G['azl_gate_post_skipped'])
    ok = (out['fault'] is None and end_pc == dump.lr and out['bx_lr'] and intact and uc.reg_read(UC_ARM_REG_SP) == sp_entry
          and [n for n, _ in out['svc']] == [0x32] and PAGE <= out['svc'][0][1] < PAGE + 0x1000 and out.get('in_flight_asleep') == 0
          and word(G['azl_gate_in_flight']) == 0 and (out['post'], skipped, r0) == ((1, 0, 0x100) if back_before_wake else (0, 1, 0)))
    print('  gate, block back %s the thread wakes: pc %08X (the caller %08X), game\'s bx lr %s, [sp] and above %s, sp %s, '
          'svc %s, in flight while asleep %s, post %d / skipped %d, r0 %08X, fault %s -> %s' % (
              'before' if back_before_wake else 'after', end_pc, dump.lr, 'run' if out['bx_lr'] else 'NOT run',
              'intact' if intact else 'CHANGED', 'as the game left it' if uc.reg_read(UC_ARM_REG_SP) == sp_entry else 'MOVED',
              [(hex(n), '%08X' % a) for n, a in out['svc']], out.get('in_flight_asleep'), out['post'], skipped, r0,
              ('%08X at pc %08X' % out['fault']) if out['fault'] else '-', 'OK' if ok else 'BROKEN'))
    return ok


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    sim_dir = os.path.join(HERE, '..', '..', 'analysis', 'gate_sim')
    if '--sim' in sys.argv:
        sim_dir = sys.argv[sys.argv.index('--sim') + 1]
        args.remove(sim_dir)
    if not args:
        sys.exit(__doc__)
    for path in args:
        dump = Dump(path)
        print('%s: pc %08X lr %08X sp %08X' % (dump.name, dump.pc, dump.lr, dump.sp))
        if not check((dump.word(SITE), dump.word(SITE + 4)) == OLD_HOOK and dump.pc == CRASH_PC,
                     '%s: not the dump of the old hook on the LEGO Batman 2 stub' % dump.name):
            continue
        died, stack_z1 = old_format(dump, True)
        check(died and stack_z1, '%s: the old format with Z set does not die as the dump did' % dump.name)
        died0, stack_z0 = old_format(dump, False)
        check(died0 and not stack_z0, '%s: with Z clear the old format should die at the same pc without the dump\'s [sp+8]' % dump.name)
        check(gate(dump, sim_dir, back_before_wake=False), '%s: the gate, block back after the thread wakes' % dump.name)
        check(gate(dump, sim_dir, back_before_wake=True), '%s: the gate, block back before the thread wakes' % dump.name)
    print('lb2_dump_sim: %d failures' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
