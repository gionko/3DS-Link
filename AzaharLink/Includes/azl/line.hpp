// Text for the log lines of code that runs on the GAME's own threads (the svc hook: every IPC call of every thread
// of the game passes through it). No library formatting at all: newlib's vsnprintf chain (DebugEvent + vsnprintf +
// _svfprintf_r) needs about 1.3 KB of stack, and a game thread may have far less than that to give - Tri Force
// Heroes' srv-notification thread has 464 bytes below the call, so the exit notification of 0.1.15 at debug=1 ran
// the chain over its own thread object and the game crashed on exit (hardware, 2026-10-10). This builder appends to
// a buffer the caller owns (a static one under the log's lock: see LogLine / DebugLine), uses no stack of its own
// beyond the frame of the call and writes the same bytes the printf formats it replaces did
// (tests/line_test.cpp compares every piece with snprintf on random values).
#pragma once

#include "azl/common.hpp"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
class LineBuf {
public:
    /// Appends into data[0, capacity): text that does not fit is cut, like snprintf's (the caller keeps one byte for
    /// the terminator in `capacity`, or does not need one).
    LineBuf(char* data, u32 capacity) : data_(data), cap_(capacity) {}

    /// "%s" (at most `max` characters of it, like "%.*s"; a null pointer adds nothing)
    LineBuf& Str(const char* s, u32 max = 0xFFFFFFFFu) {
        while (s && *s && max != 0 && len_ < cap_) {
            data_[len_++] = *s++;
            --max;
        }
        return *this;
    }

    /// "%0*lx" without a prefix: lowercase, at least `digits` digits (the 0x is the caller's); `upper`: "%0*lX"
    LineBuf& Hex(u32 value, u32 digits = 1, bool upper = false) {
        u32 width = 1;
        for (u32 rest = value >> 4; rest != 0; rest >>= 4) {
            ++width;
        }
        if (width < digits) {
            width = digits;
        }
        const u32 letter = upper ? 'A' : 'a';
        while (width != 0 && len_ < cap_) {
            --width;
            const u32 nibble = width >= 8 ? 0 : (value >> (4 * width)) & 0xF;
            data_[len_++] = static_cast<char>(nibble < 10 ? '0' + nibble : letter + (nibble - 10));
        }
        return *this;
    }

    /// "%lu"
    LineBuf& Dec(u32 value) {
        // (subtraction against a table: the ARM11 has no divider, a division is a call into libgcc)
        static const u32 kScale[10] = {1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
                                       10000u,      1000u,      100u,      10u,      1u};
        int i = 0;
        while (i < 9 && value < kScale[i]) {
            ++i;
        }
        for (; i < 10 && len_ < cap_; ++i) {
            char digit = '0';
            while (value >= kScale[i]) {
                value -= kScale[i];
                ++digit;
            }
            data_[len_++] = digit;
        }
        return *this;
    }

    /// "%*lu" / "%0*lu": at least `width` characters, filled on the left with `fill`
    LineBuf& DecPad(u32 value, u32 width, char fill) {
        u32 digits = 1;
        if (value >= 1000000000u) {
            digits = 10;
        } else {
            for (u32 scale = 10; value >= scale; scale *= 10) {
                ++digits;
            }
        }
        for (; digits < width && len_ < cap_; ++digits) {
            data_[len_++] = fill;
        }
        return Dec(value);
    }

    /// "%d"
    LineBuf& SDec(s32 value) {
        if (value < 0) {
            if (len_ < cap_) {
                data_[len_++] = '-';
            }
            return Dec(0u - static_cast<u32>(value));
        }
        return Dec(static_cast<u32>(value));
    }

    /// One character.
    LineBuf& Ch(char c) {
        if (len_ < cap_) {
            data_[len_++] = c;
        }
        return *this;
    }

    char* data() const {
        return data_;
    }
    u32 size() const {
        return len_;
    }

protected:
    /// An inactive line (the log is off): every append is a no-op.
    void Deactivate() {
        cap_ = 0;
        len_ = 0;
    }

private:
    char* data_;
    u32 cap_;
    u32 len_ = 0;
};
// <<< game-thread code

} // namespace azl
