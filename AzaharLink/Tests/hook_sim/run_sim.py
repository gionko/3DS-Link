#!/usr/bin/env python3
"""Runs the game's real IPC wrappers in an ARM emulator, unpatched vs hooked, and compares them.

    python run_sim.py <code.bin> <sim-dir>        (sim-dir from patch_image: patched.bin, tramps.bin, sites.txt)

Every `svc 0x32` / `svc 0x23` is replaced by the trampoline built by the plugin's own C++ code
(azl::BuildSvcTrampoline). The emulated "kernel" and the emulated assembly entry both answer the
call with the same deterministic result and leave every other register alone, which is the
contract of svc_entry.s. For each call site we run the enclosing function from random register
state and require identical: observed svc calls (number, r0, command buffer), final registers,
condition flags and memory.
"""
import random
import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_INTR, UC_MODE_ARM, Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_C13_C0_3, UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC,
                               UC_ARM_REG_R0, UC_ARM_REG_SP)

BASE = 0x00100000
TRAMP_BASE = 0x07100000
SYMS = {}
ENTRY_BASE = 0x07000100
HANDLER = {}   # address of azl_hook_* stub -> svc number
TLS = 0x0D000000
STACK_TOP = 0x0E0FF000
TRAP = 0x0C000000
ARG_BASE = 0x08000000
ARG_SIZE = 0x10000
MAX_INSN = 4000


ENTRY_CODE = b''
TOTAL_MISALIGNED = 0


def result_for(n):
    return (0, 0xE0E01BF9, 0x12345678, 0x80000000, 0xD8E007F7)[n % 5]


class Run:
    def __init__(self, image, tramps, hooked):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.mem_map(BASE, 0x700000)
        uc.mem_write(BASE, image)
        uc.mem_map(0x07000000, 0x200000)
        if hooked:
            uc.mem_write(TRAMP_BASE, tramps)
            uc.mem_write(ENTRY_BASE, ENTRY_CODE)   # the REAL svc_entry.s, assembled
        uc.mem_map(STACK_TOP - 0xF000, 0x10000)
        uc.mem_map(TLS, 0x1000)
        uc.mem_map(ARG_BASE, ARG_SIZE)
        uc.mem_map(TRAP, 0x1000)
        uc.reg_write(UC_ARM_REG_C13_C0_3, TLS)
        self.trace = []
        self.count = 0
        self.done = False
        self.hooked = hooked
        uc.hook_add(UC_HOOK_INTR, self.on_svc)
        self.misaligned = 0
        if hooked:
            for addr in HANDLER:
                uc.hook_add(UC_HOOK_CODE, self.on_handler, begin=addr, end=addr)
        uc.hook_add(UC_HOOK_CODE, self.on_trap, begin=TRAP, end=TRAP)
        uc.hook_add(UC_HOOK_CODE, self.on_count)

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

    def on_handler(self, uc, address, size, user):
        # This is where the C handler would run. AAPCS: the stack must be 8-byte aligned here.
        if uc.reg_read(UC_ARM_REG_SP) & 7:
            self.misaligned += 1
        self.record(HANDLER[address])
        # Compiled C may clobber the caller-saved registers and the flags: do it on purpose, the
        # entry code must give them back to the game untouched.
        for i, reg in enumerate((UC_ARM_REG_R0 + 1, UC_ARM_REG_R0 + 2, UC_ARM_REG_R0 + 3, UC_ARM_REG_R0 + 12)):
            uc.reg_write(reg, 0xBAD00000 + i)
        uc.reg_write(UC_ARM_REG_CPSR, (uc.reg_read(UC_ARM_REG_CPSR) & 0x0FFFFFFF) | 0xA0000000)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))  # bx lr

    def on_trap(self, uc, address, size, user):
        self.done = True
        uc.emu_stop()

    def on_count(self, uc, address, size, user):
        self.count += 1
        if self.count > MAX_INSN:
            uc.emu_stop()

    def state(self):
        uc = self.uc
        regs = [uc.reg_read(r) for r in range(UC_ARM_REG_R0, UC_ARM_REG_R0 + 13)]
        regs += [uc.reg_read(UC_ARM_REG_SP), uc.reg_read(UC_ARM_REG_LR),
                 uc.reg_read(UC_ARM_REG_CPSR) & 0xF0000000]
        # Only the live part of the stack (from sp up): the trampoline leaves a dead copy of lr
        # below sp, which is invisible to the game.
        sp = uc.reg_read(UC_ARM_REG_SP)
        live = bytes(uc.mem_read(sp, STACK_TOP - sp)) if STACK_TOP - 0x2000 <= sp <= STACK_TOP else b'?'
        mem = (live, bytes(uc.mem_read(TLS, 0x1000)), bytes(uc.mem_read(ARG_BASE, ARG_SIZE)))
        return regs, mem


def find_function_start(code, site):
    off = site - BASE
    for k in range(1, 300):
        w = struct.unpack_from('<I', code, off - 4 * k)[0]
        if (w & 0xFFFF0000) == 0xE92D0000 and (w & 0x4000):
            return site - 4 * k
    return None


def run_once(image, tramps, hooked, start, seed):
    rnd = random.Random(seed)
    r = Run(image, tramps, hooked)
    uc = r.uc
    # argument memory with pseudo-random contents (pointers into itself where derefs are likely)
    blob = bytearray()
    for i in range(ARG_SIZE // 4):
        blob += struct.pack('<I', ARG_BASE + (rnd.randrange(0, ARG_SIZE // 4) * 4)
                            if rnd.random() < 0.5 else rnd.getrandbits(32))
    uc.mem_write(ARG_BASE, bytes(blob))
    # Switch to user mode first: writing CPSR swaps the banked SP/LR, so it must precede them.
    flags = rnd.getrandbits(4) << 28
    uc.reg_write(UC_ARM_REG_CPSR, 0x10 | flags)
    for i in range(13):
        v = ARG_BASE + rnd.randrange(0, ARG_SIZE // 4 - 64) * 4
        uc.reg_write(UC_ARM_REG_R0 + i, v)
    uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
    uc.reg_write(UC_ARM_REG_LR, TRAP)
    try:
        uc.emu_start(start, TRAP + 0x100, count=0)
        err = None
    except UcError as e:
        err = str(e)
    return r, err


def main():
    global ENTRY_CODE
    code = open(sys.argv[1], 'rb').read()
    sim = sys.argv[2]
    ENTRY_CODE = open(sim + '/entry.bin', 'rb').read()
    for line in open(sim + '/symbols.txt'):
        name, addr = line.split()
        SYMS[name] = int(addr, 16)
    HANDLER[SYMS['azl_hook_SendSyncRequest']] = 0x32
    HANDLER[SYMS['azl_hook_CloseHandle']] = 0x23
    patched = open(sim + '/patched.bin', 'rb').read()
    tramps = open(sim + '/tramps.bin', 'rb').read()
    sites = [(int(a, 16), k) for a, k in (l.split() for l in open(sim + '/sites.txt'))]

    global TOTAL_MISALIGNED
    TOTAL_MISALIGNED = 0
    ok = mismatch = incomplete = faulted = 0
    starts = set()
    for site, kind in sites:
        start = find_function_start(code, site)
        if start is None or start in starts:
            continue
        starts.add(start)
        for seed in range(3):
            a, ea = run_once(code, tramps, False, start, seed * 1000 + (start >> 2))
            b, eb = run_once(patched, tramps, True, start, seed * 1000 + (start >> 2))
            if ea or eb:
                faulted += 1
                if bool(ea) != bool(eb):
                    mismatch += 1
                    print('MISMATCH (only one run faulted) at function 0x%08x site 0x%08x: %s | %s'
                          % (start, site, ea, eb))
                continue
            if not (a.done and b.done):
                incomplete += 1
                continue
            TOTAL_MISALIGNED += b.misaligned
            sa, sb = a.state(), b.state()
            if a.trace != b.trace or sa != sb:
                mismatch += 1
                print('MISMATCH at function 0x%08x site 0x%08x (%s)' % (start, site, kind))
                if a.trace != b.trace:
                    print('   trace differs:', a.trace[:2], b.trace[:2])
                if sa[0] != sb[0]:
                    print('   regs  orig:', [hex(x) for x in sa[0]])
                    print('   regs hook :', [hex(x) for x in sb[0]])
                if sa[1] != sb[1]:
                    print('   memory differs')
            else:
                ok += 1
    print('functions: %d, identical runs: %d, mismatches: %d, faulted-in-both: %d, did-not-finish: %d'
          % (len(starts), ok, mismatch, faulted, incomplete))
    print('handler calls with a misaligned stack: %d' % TOTAL_MISALIGNED)
    return 1 if (mismatch or TOTAL_MISALIGNED) else 0


if __name__ == '__main__':
    sys.exit(main())
