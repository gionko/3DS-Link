#include "azl/arm_reloc.hpp"

namespace azl {

namespace {

constexpr u32 kLdrPcMinus4 = 0xE51FF004; // ldr pc, [pc, #-4]  (jumps to the following word)

u32 Ror(u32 value, u32 amount) {
    amount &= 31;
    return amount ? (value >> amount) | (value << (32 - amount)) : value;
}

RelocResult Fail(const char* reason) {
    RelocResult r;
    r.reason = reason;
    return r;
}

/// The plain case: the instruction does not depend on its own address.
RelocResult Copy(u32 insn, u32 resume) {
    RelocResult r;
    r.ok = true;
    r.words[0] = insn;
    r.words[1] = kLdrPcMinus4;
    r.words[2] = resume;
    r.count = 3;
    return r;
}

/// The instruction was rewritten to read `literal` through `core` (an `ldr` from [pc, #4], i.e.
/// words[3]).
RelocResult WithLiteral(u32 core, u32 literal, u32 resume) {
    RelocResult r;
    r.ok = true;
    r.words[0] = core;
    r.words[1] = kLdrPcMinus4;
    r.words[2] = resume;
    r.words[3] = literal;
    r.count = 4;
    return r;
}

} // namespace

RelocResult RelocateArm(u32 insn, u32 address, u32 resume, ReadWordFn read, void* context) {
    const u32 cond = insn >> 28;
    const u32 pc_value = address + 8; // what a read of r15 yields at the original location

    if (cond == 0xF) {
        // Unconditional space: only PLD (a cache hint) is harmless to copy.
        if ((insn & 0xFF70F000) == 0xF550F000 && ((insn >> 16) & 15) != 15) {
            return Copy(insn, resume);
        }
        return Fail("unconditional-space instruction");
    }

    const u32 rn = (insn >> 16) & 15;
    const u32 rd = (insn >> 12) & 15;
    const u32 rs = (insn >> 8) & 15;
    const u32 rm = insn & 15;

    switch ((insn >> 25) & 7) {
    case 0:
    case 1: { // data processing, miscellaneous, multiplies, extra load/store
        const bool immediate = ((insn >> 25) & 7) == 1;

        if ((insn & 0x0FFFFFF0) == 0x012FFF10) { // bx rm
            return rm == 15 ? Fail("bx pc") : Copy(insn, resume);
        }
        if ((insn & 0x0FFFFFD0) == 0x012FFF10 || (insn & 0x0FFFFFF0) == 0x012FFF30 ||
            (insn & 0x0FFFFFF0) == 0x012FFF20) {
            return Fail("blx/bxj register"); // lr would be wrong
        }
        if ((insn & 0x0FBF0FFF) == 0x010F0000) { // mrs rd, cpsr/spsr
            return rd == 15 ? Fail("mrs pc") : Copy(insn, resume);
        }
        if ((insn & 0x0FB0FFF0) == 0x0120F000) { // msr reg
            return rm == 15 ? Fail("msr pc") : Copy(insn, resume);
        }
        if ((insn & 0x0FB0F000) == 0x0320F000) { // msr immediate / hints (nop, yield...)
            return Copy(insn, resume);
        }

        if (!immediate && (insn & 0x90) == 0x90) {
            // Multiplies, swp, ldrh/strh/ldrd/strd: be strict about any use of the PC.
            if (rn == 15 || rd == 15 || rm == 15 || rs == 15) {
                return Fail("multiply/extra load-store using pc");
            }
            return Copy(insn, resume);
        }

        const u32 opcode = (insn >> 21) & 15;
        const bool s_flag = (insn >> 20) & 1;
        const bool uses_rn = opcode != 13 && opcode != 15; // mov, mvn do not read Rn

        if (immediate) {
            if (uses_rn && rn == 15) {
                // add/sub rd, pc, #imm (what "adr" assembles to) -> load the absolute address.
                if ((opcode == 4 || opcode == 2) && !s_flag && rd != 15) {
                    const u32 imm = Ror(insn & 0xFF, ((insn >> 8) & 15) * 2);
                    const u32 value = opcode == 4 ? pc_value + imm : pc_value - imm;
                    return WithLiteral((cond << 28) | 0x059F0000 | (rd << 12) | 4, value, resume);
                }
                return Fail("pc-relative data processing");
            }
            return Copy(insn, resume);
        }

        // mov rd, pc : the value is a constant of the original location.
        if (opcode == 13 && !s_flag && rm == 15 && (insn & 0xFF0) == 0 && rd != 15) {
            return WithLiteral((cond << 28) | 0x059F0000 | (rd << 12) | 4, pc_value, resume);
        }

        const bool register_shift = (insn >> 4) & 1;
        if (rm == 15) {
            return Fail("data processing reads pc as Rm");
        }
        if (register_shift && rs == 15) {
            return Fail("data processing reads pc as Rs");
        }
        if (uses_rn && rn == 15) {
            return Fail("data processing reads pc as Rn");
        }
        return Copy(insn, resume);
    }

    case 2:
    case 3: { // single data transfer (ldr/str/ldrb/strb)
        const bool register_offset = ((insn >> 25) & 7) == 3;
        const bool pre = (insn >> 24) & 1;
        const bool up = (insn >> 23) & 1;
        const bool write_back = (insn >> 21) & 1;
        const bool load = (insn >> 20) & 1;

        if (register_offset) {
            if ((insn >> 4) & 1) {
                return Fail("media instruction");
            }
            if (rm == 15) {
                return Fail("register offset is pc");
            }
        }
        if (rn == 15) {
            // Only "ldr/ldrb rd, [pc, #imm]" with a plain offset: copy the constant it reads.
            if (register_offset || !pre || write_back || !load) {
                return Fail("unsupported pc-based load/store");
            }
            const u32 imm12 = insn & 0xFFF;
            const u32 literal_address = up ? pc_value + imm12 : pc_value - imm12;
            const bool byte = (insn >> 22) & 1;
            if (!byte && (literal_address & 3)) {
                return Fail("unaligned literal load");
            }
            // The same instruction, now reading words[3] (offset +4 from pc = word 0 + 8).
            const u32 core = (insn & ~(0xFFFu | (1u << 23))) | (1u << 23) | 4;
            return WithLiteral(core, read(context, literal_address & ~3u), resume);
        }
        if (!load && rd == 15) {
            return Fail("store of pc");
        }
        return Copy(insn, resume);
    }

    case 4: { // ldm / stm
        const bool load = (insn >> 20) & 1;
        if (rn == 15) {
            return Fail("block transfer based on pc");
        }
        if (!load && (insn & 0x8000)) {
            return Fail("stm of pc");
        }
        return Copy(insn, resume);
    }

    case 5: { // b / bl
        if ((insn >> 24) & 1) {
            return Fail("bl"); // would need lr = address + 4
        }
        s32 offset = static_cast<s32>(insn << 8) >> 6; // sign-extended imm24 * 4
        const u32 target = pc_value + static_cast<u32>(offset);
        // ldr<cond> pc, [pc, #4] reads words[3]; the fall-through path (condition false) continues
        // at words[1], which jumps to `resume`.
        return WithLiteral((cond << 28) | 0x059FF004, target, resume);
    }

    case 6: { // coprocessor load/store (incl. VFP vldr/vstr/vldm)
        if (rn == 15) {
            return Fail("coprocessor load/store based on pc");
        }
        return Copy(insn, resume);
    }

    case 7: // svc, coprocessor data operations and register transfers (mrc/mcr, VFP)
        return Copy(insn, resume);
    }
    return Fail("unreachable");
}

} // namespace azl
