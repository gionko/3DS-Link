"""The instructions of one frame of the chat's lines on the top screen, counted in an ARM emulator (unicorn) on the probe that
Tests/chat_draw_cost.sh builds with the plugin's own compiler and flags (Tests/chat_draw_cost_probe.cpp).

The lines are drawn on the GAME's thread, inside the framework's OSD callback, before the game's own present, for 10 s after every
message (and while the keyboard is open): their time is the game's frame. On an Old 3DS (and on a New 3DS with a title that is not a
New 3DS one) the ARM11 runs at 268 MHz and at most one instruction a cycle, so 1 000 000 instructions are at least 3.7 ms of a 16.7 ms
frame. The worst frame is 5 messages of three full lines (5 blocks of 382 x 32 pixels, the whole width since 2026-11-08; two lines of
298 x 28 before) with the 3D on; it was 1.73-1.95 million instructions for the 298 x 28 blocks when every pixel called the colour's picker
and every column divided (2026-11-04), which is what the budgets below keep out.
The framebuffers are strongly ordered and uncached: every write is a bus transaction the core waits for, so the bus writes of a frame are
counted too (a block store is one), and the plugin's build is checked byte by byte against a reference built with AZL_GFX_NO_BURST (one
store a word, as on the PC, where Tests/chat_draw_test.cpp checks every pixel).

    python chat_draw_cost.py <probe.elf> <probe_ref.elf>   (the plugin's build and the one with AZL_GFX_NO_BURST; Python 3 with the
                                                           unicorn package; exit status 1 when a budget is passed)
"""
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_CPSR, UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP)

STACK = 0x08000000
STACK_SIZE = 0x10000
RET = 0x09000000  # the return address handed to the probe's functions: the emulation stops there
FB_BYTES = 400 * 240 * 4  # one of the probe's framebuffers (g_left, g_right)

FORMATS = ['RGBA8', 'BGR8', 'RGB565', 'RGB5A1', 'RGBA4']

# (name, format, 3D, messages, nick characters, text characters, budget in instructions, budget in bus writes)
# The instruction budgets are the counts measured on 2026-11-08 for the blocks of the whole width (three lines of 62 cells, 382 x 32 pixels;
# in the comments, the 2026-11-05 counts of the blocks of two lines of 48 cells, 298 x 28, a whole group of pixels in one block store, and
# what they were before: one store a word, and before that the picker out of line), with about 10 % on top: a change that brings back a call
# or a division a pixel or a column, or works a group of background pixels out, passes them by far. The worst frame grew with its pixels
# (1.47 times as many: 61 120 a frame against 41 720) and the third line's glyph a column. The bus writes are the measured counts (a store
# one, a block store one): the framebuffer is strongly ordered and uncached, every write is a bus transaction the core waits for, and they
# cost more than the instructions; one store a word passes them by 2, 3 or 4 times.
SCENARIOS = [
    # the worst frame: 5 blocks of three full lines, the 3D on (655 431 / 719 973 / 652 206 / 652 220 / 652 199 instructions, 30 560 bus
    # writes; 2026-11-05, two lines of 48: 433 501 / 479 508 / 429 836 / 429 850 / 429 829, 20 860 bus writes; one store a word: 449 396 /
    # 510 650 / 393 007 instructions, 83 440 / 62 580 / 41 720 bus writes; with the picker out of line 1.70 M / 1.90 M / 1.89 M instructions)
    ('5 long messages, 3D', 0, 1, 5, 20, 128, 721000, 30560),
    ('5 long messages, 3D', 1, 1, 5, 20, 128, 792000, 30560),
    ('5 long messages, 3D', 2, 1, 5, 20, 128, 718000, 30560),
    ('5 long messages, 3D', 3, 1, 5, 20, 128, 718000, 30560),
    ('5 long messages, 3D', 4, 1, 5, 20, 128, 718000, 30560),
    # the same without the 3D (624 871 / 689 413 / 621 646 instructions, 15 280 bus writes; 2026-11-05: 412 641 / 458 648 / 408 976, 10 430;
    # one store a word: 401 229 / 510 650 / 393 007, 41 720 / 31 290 / 20 860; with the picker out of line 947 K / 1.05 M / 1.04 M)
    ('5 long messages', 0, 0, 5, 20, 128, 688000, 15280),
    ('5 long messages', 1, 0, 5, 20, 128, 759000, 15280),
    ('5 long messages', 2, 0, 5, 20, 128, 684000, 15280),
    # the common frame: one short message (24 280 / 26 114 instructions, 390 bus writes: a block of 12 rows; 2026-11-05, 16 rows: 24 875 /
    # 27 374, 520; one store a word: 24 146 / 29 875, 2 080 / 1 560; with the picker out of line 49 753 / 56 802)
    ('1 short message', 0, 0, 1, 8, 10, 26700, 390),
    ('1 short message', 1, 0, 1, 8, 10, 28700, 390),
]

# The keyboard's copy onto the bottom screen (azl/chat_host.hpp, CopyImage), on the same thread every frame the keyboard is open: the whole
# 320 x 240 screen. (format, rows of a column in the framebuffer, budget in instructions)
# The budgets are the counts measured on 2026-11-04 with the copy in blocks of eight words (ldm/stm), with about 10 % on top (in the
# comments, with what they were when every word was a load and a store of its own): one store a word passes them by far.
COPIES = [
    (0, 240, 44800),  # RGBA8 (40 725; before: 234 319)
    (1, 240, 33600),  # BGR8, a block of four then 22 of eight a column (30 481; before: 176 715)
    (2, 240, 23700),  # RGB565 (21 525; before: 119 119)
    (1, 256, 33600),  # BGR8, columns farther apart than the picture's (30 481)
]

# The keyboard's picture drawn again on the PLUGIN's thread (azl/osk_draw.hpp; Sources/ctr/chat_overlay.cpp, Redraw) at most 30 times a
# second while it is open: that thread is at the game's own priority (0x30) on the Old 3DS's core 0, and a game's thread woken meanwhile
# waits for it. The whole screen only when the keyboard opens and for the other page; the blink (twice a second, the keyboard left alone),
# a key moved, a character typed, a key no longer lit and shift are drawn over what the picture showed (DrawOskOver), checked byte by byte
# against a whole new drawing. (change, format, budget in instructions)
OSK_CHANGES = ['whole screen', 'blink', 'D-pad move', 'character typed', 'key let go', 'shift']
OSK = [
    # (the counts measured on 2026-11-05 with about 10 % on top, a group of pixels one block store; on 2026-11-04, one store a word, in the
    # brackets: the picture is the plugin's heap, cached, where the block stores save little and their registers cost a few moves; before
    # DrawOskOver every one of them was the whole screen)
    (0, 0, 1754400), (0, 1, 2191000), (0, 2, 1845300),  # the whole screen (1 594 864 / 1 951 313 / 1 677 533; 1 492 666 / 1 991 556 / 1 615 865)
    (1, 0, 70700), (1, 1, 116000), (1, 2, 70000),        # the blink (64 289 / 105 169 / 63 626; 61 770 / 105 449 / 61 915)
    (2, 0, 39400), (2, 1, 42700), (2, 2, 40100),         # a key of the D-pad moved (35 844 / 37 821 / 36 429; 34 949 / 38 815 / 35 838)
    (3, 0, 196900), (3, 1, 279300), (3, 2, 212100),      # a character typed (179 015 / 249 174 / 192 773; 165 895 / 253 901 / 182 786)
    (4, 0, 27700), (4, 1, 29300), (4, 2, 28100),         # the key no longer lit (25 181 / 26 137 / 25 542; 24 882 / 26 634 / 25 263)
    (5, 0, 364100), (5, 1, 461200), (5, 2, 372000),      # shift: every letter (331 036 / 413 002 / 338 217; 313 824 / 419 314 / 329 003)
]


def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[4] == 1, 'a 32-bit ELF is expected'
    e_phoff, = struct.unpack_from('<I', d, 0x1C)
    e_shoff, = struct.unpack_from('<I', d, 0x20)
    e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x2A)
    e_shentsize, e_shnum = struct.unpack_from('<HH', d, 0x2E)
    segments = []
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from('<8I', d, e_phoff + i * e_phentsize)
        if p_type == 1:
            segments.append((p_vaddr, p_off, p_filesz, p_memsz))
    secs = [struct.unpack_from('<10I', d, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    symbols = {}
    for sh in secs:
        if sh[1] == 2:  # SHT_SYMTAB
            strtab = secs[sh[6]]
            for j in range(sh[5] // 16):
                st_name, st_value, _, _, _, st_shndx = struct.unpack_from('<IIIBBH', d, sh[4] + j * 16)
                end = d.index(b'\0', strtab[4] + st_name)
                name = d[strtab[4] + st_name:end].decode('latin1')
                if name and st_shndx != 0:
                    symbols.setdefault(name, st_value)
    return d, segments, symbols


class Probe:
    """One build of the probe in its own emulator: `call` runs one of its functions, the instructions counted when asked; `bus` is the
    bus writes into the top screen's two framebuffers since the last call (a store is one, a block store of n words is one, its n words
    reported one by one by the emulator)."""

    def __init__(self, path):
        d, segments, symbols = load(path)
        self.symbols = symbols
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        lo = min(s[0] for s in segments) & ~0xFFF
        hi = (max(s[0] + s[3] for s in segments) + 0xFFF) & ~0xFFF
        uc.mem_map(lo, hi - lo)
        for vaddr, off, filesz, _ in segments:
            uc.mem_write(vaddr, d[off:off + filesz])
        uc.mem_map(STACK, STACK_SIZE)
        uc.mem_map(RET, 0x1000)
        # (the hard-float ABI: the VFP on, as the plugin has it)
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.count = 0
        self.counting = False
        self.bus = 0
        self.burst = 0  # (words of the block store under way)
        self.fb = [self.symbol('g_left'), self.symbol('g_right')]

        def on_block(_uc, _addr, size, _data):
            if self.counting:
                self.count += size // (2 if _uc.reg_read(UC_ARM_REG_CPSR) & 0x20 else 4)

        def on_write(_uc, _access, _addr, _size, _value, _data):
            insn, = struct.unpack('<I', _uc.mem_read(_uc.reg_read(UC_ARM_REG_PC), 4))
            if (insn >> 25) & 7 == 4 and not insn & (1 << 20):  # STM: one bus write for the whole list
                self.burst += 1
                if self.burst == bin(insn & 0xFFFF).count('1'):
                    self.bus += 1
                    self.burst = 0
            else:
                self.bus += 1

        # (one hook for the whole run, counting only in the counted calls: a hook added later does not see the blocks the emulator already
        # translated, and the keyboard's setup runs the drawing it then counts)
        uc.hook_add(UC_HOOK_BLOCK, on_block)
        for fb in self.fb:
            uc.hook_add(UC_HOOK_MEM_WRITE, on_write, None, fb, fb + FB_BYTES - 1)

    def symbol(self, name):
        found = [v for k, v in self.symbols.items() if k == name or k.endswith('%d%s' % (len(name), name))]
        assert len(found) == 1, 'the probe has no single ' + name
        return found[0]

    def framebuffers(self):
        return b''.join(bytes(self.uc.mem_read(fb, FB_BYTES)) for fb in self.fb)

    def call(self, name, args, counted):
        uc = self.uc
        regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
        for r, v in zip(regs, args[:4]):
            uc.reg_write(r, v)
        sp = STACK + STACK_SIZE - 0x100
        for k, v in enumerate(args[4:]):
            uc.mem_write(sp + 4 * k, struct.pack('<I', v))
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RET)
        self.counting = counted
        self.count = 0
        self.bus = 0
        self.burst = 0
        uc.emu_start(self.symbols[name], RET)
        self.counting = False
        assert uc.reg_read(UC_ARM_REG_PC) == RET, name + ' did not return'
        assert self.burst == 0, name + ': a block store reported in part'
        return uc.reg_read(UC_ARM_REG_R0), self.count


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    probe, ref = Probe(sys.argv[1]), Probe(sys.argv[2])
    failed = 0
    for name, fmt, three_d, lines, nick_len, text_len, budget, bus_budget in SCENARIOS:
        for p in (probe, ref):
            p.call('azl_cost_setup', [fmt, three_d, lines, nick_len, text_len, 2], False)
        drawn, n = probe.call('azl_cost_draw', [], True)
        ref.call('azl_cost_draw', [], False)
        same = probe.framebuffers() == ref.framebuffers()
        ok = drawn == lines and n <= budget and probe.bus <= bus_budget and same
        print('chat draw cost: %-22s %-6s %9d instructions (budget %d), %6d bus writes (budget %d, %d one a word)%s' %
              (name, FORMATS[fmt], n, budget, probe.bus, bus_budget, ref.bus, '' if ok else '  FAILED'))
        if drawn != lines:
            print('chat draw cost FAILED: %d blocks drawn of %d' % (drawn, lines))
        if not same:
            print("chat draw cost FAILED: the framebuffers differ from the reference build's (a store a word)")
        failed |= not ok
    for fmt, rows, budget in COPIES:
        for p in (probe, ref):  # (the reference too: the two builds' framebuffers stay alike for what follows)
            p.call('azl_copy_setup', [fmt, rows], False)
        ref.call('azl_copy_run', [], False)
        copied, n = probe.call('azl_copy_run', [], True)
        wrong, _ = probe.call('azl_copy_check', [], False)
        ok = copied == 1 and wrong == 0 and n <= budget
        print('chat draw cost: keyboard copy, %3d rows %-6s %9d instructions (budget %d)%s' % (rows, FORMATS[fmt], n, budget, '' if ok else '  FAILED'))
        if copied != 1 or wrong != 0:
            print('chat draw cost FAILED: the keyboard copy %s, %d bytes wrong' % ('ran' if copied else 'did not run', wrong))
        failed |= not ok
    for change, fmt, budget in OSK:
        for p in (probe, ref):
            p.call('azl_osk_setup', [fmt, change], False)
        _, n = probe.call('azl_osk_draw', [], True)
        ref.call('azl_osk_draw', [], False)
        same = probe.framebuffers() == ref.framebuffers()
        wrong, _ = probe.call('azl_osk_check', [], False)
        ref.call('azl_osk_check', [], False)
        ok = wrong == 0 and n <= budget and same
        print('chat draw cost: keyboard redraw, %-15s %-6s %9d instructions (budget %d)%s' % (OSK_CHANGES[change], FORMATS[fmt], n, budget, '' if ok else '  FAILED'))
        if wrong != 0:
            print('chat draw cost FAILED: the keyboard drawn over its picture differs from a whole drawing in %d bytes' % wrong)
        if not same:
            print("chat draw cost FAILED: the keyboard's picture differs from the reference build's (a store a word)")
        failed |= not ok
    print('chat draw cost: %s' % ('FAILED' if failed else 'all within budget'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
