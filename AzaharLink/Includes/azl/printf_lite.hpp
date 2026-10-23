// The part of printf that the plugin's own log lines use, without newlib. Log() and DebugEvent() used to hand every format
// to vsnprintf, whose chain (vsnprintf -> _svfprintf_r -> ...) needs about 1.4 KB of stack below the call; the HLE proper
// (a fake session's command, the engine, NdmHle) makes those calls on the game's UDS thread at debug=1, and that was
// the ~1.9 KB it needed there (Tests/chain_sim: 176-224 bytes at debug=0, 1896 at debug=1). This formatter writes the same
// bytes for the conversions the sources use - %d %i %u %x %X %c %s %%, the flags - and 0, the width (also *), the length
// l / ll / z, and a precision on %s - with a few words of stack, and says no (Supported) to anything else, which the
// caller hands to newlib as before. Tests/line_test.cpp compares it with snprintf on every combination and reads the
// sources for the format of every Log() / DebugEvent() call (TestSourceFormats: one that this formatter does not write fails).
#pragma once

#include <cstdarg>
#include <cstddef>

#include "azl/common.hpp"
#include "azl/line.hpp"

namespace azl {
namespace printf_lite {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
struct Spec {
    bool left = false;       // '-'
    bool zero = false;       // '0'
    bool width_star = false; // '*'
    bool prec_star = false;
    int width = 0;
    int precision = -1;      // -1: none
    char length = 0;         // 0, 'l' (long), 'L' (long long), 'z' (size_t)
    char conv = 0;           // d i u x X c s %
};

/// Parses the conversion that follows a '%' at `p`. Returns the first character after it, or nullptr for what is not
/// supported (so the format as a whole is not).
inline const char* ParseSpec(const char* p, Spec* s) {
    *s = Spec();
    for (;; ++p) {
        if (*p == '-') {
            s->left = true;
        } else if (*p == '0') {
            s->zero = true;
        } else {
            break;
        }
    }
    if (*p == '+' || *p == ' ' || *p == '#') {
        return nullptr;
    }
    if (*p == '*') {
        s->width_star = true;
        ++p;
    } else {
        int w = 0;
        while (*p >= '0' && *p <= '9') {
            w = w * 10 + (*p - '0');
            if (w > 1000) {
                return nullptr;
            }
            ++p;
        }
        s->width = w;
    }
    if (*p == '.') {
        ++p;
        if (*p == '*') {
            s->prec_star = true;
            s->precision = 0;
            ++p;
        } else {
            int pr = 0;
            while (*p >= '0' && *p <= '9') {
                pr = pr * 10 + (*p - '0');
                if (pr > 1000) {
                    return nullptr;
                }
                ++p;
            }
            s->precision = pr;
        }
    }
    if (*p == 'l') {
        ++p;
        s->length = 'l';
        if (*p == 'l') {
            ++p;
            s->length = 'L';
        }
    } else if (*p == 'z') {
        ++p;
        s->length = 'z';
    }
    s->conv = *p;
    switch (s->conv) {
    case 'd':
    case 'i':
    case 'u':
    case 'x':
    case 'X':
        if (s->prec_star || s->precision >= 0) {
            return nullptr; // (a precision on an integer: not used anywhere)
        }
        break;
    case 'c':
        if (s->length != 0 || s->zero || s->prec_star || s->precision >= 0) {
            return nullptr;
        }
        break;
    case 's':
        if (s->length != 0 || s->zero) {
            return nullptr;
        }
        break;
    case '%':
        if (s->left || s->zero || s->width_star || s->width != 0 || s->precision >= 0 || s->length != 0) {
            return nullptr;
        }
        break;
    default:
        return nullptr; // (floating point, %p, %n, h, hh, j, t, ...: newlib's)
    }
    return p + 1;
}

/// True when every conversion of `fmt` is one this formatter writes (and a '%' is not the last character).
inline bool Supported(const char* fmt) {
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') {
            continue;
        }
        Spec s;
        p = ParseSpec(p + 1, &s);
        if (!p) {
            return false;
        }
        --p; // (the loop steps past the conversion)
    }
    return true;
}

namespace detail {

/// `width` characters wide: the pad on the left (right-justified) unless `left`.
inline void Pad(LineBuf& line, int count, char fill) {
    for (; count > 0; --count) {
        line.Ch(fill);
    }
}

/// An integer: `negative` puts a '-' in front, `base` is 10 or 16 (`upper`: capitals), the flags as printf has them.
inline void Integer(LineBuf& line, const Spec& s, u64 value, bool negative, unsigned base, bool upper) {
    char digits[24]; // (a 64-bit number is at most 20 digits, 16 in hex)
    int n = 0;
    const char* const alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0) {
        digits[n++] = '0';
    } else if (base == 16) {
        for (; value != 0; value >>= 4) {
            digits[n++] = alphabet[value & 0xF];
        }
    } else if ((value >> 32) == 0) {
        u32 v = static_cast<u32>(value);
        for (; v != 0; v /= 10) {
            digits[n++] = static_cast<char>('0' + v % 10);
        }
    } else {
        for (; value != 0; value /= 10) {
            digits[n++] = static_cast<char>('0' + static_cast<u32>(value % 10));
        }
    }
    const int length = n + (negative ? 1 : 0);
    const int fill = s.width > length ? s.width - length : 0;
    if (!s.left && !s.zero) {
        Pad(line, fill, ' ');
    }
    if (negative) {
        line.Ch('-');
    }
    if (!s.left && s.zero) {
        Pad(line, fill, '0');
    }
    while (n > 0) {
        line.Ch(digits[--n]);
    }
    if (s.left) {
        Pad(line, fill, ' ');
    }
}

} // namespace detail

/// Writes `fmt` with the arguments in `ap` into out[0, cap) - cut at the end like snprintf's, no terminator - and
/// returns the number of characters written. Only for a format that Supported() accepted.
inline u32 Format(char* out, u32 cap, const char* fmt, va_list ap) {
    LineBuf line(out, cap);
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            line.Ch(*p++);
            continue;
        }
        Spec s;
        p = ParseSpec(p + 1, &s);
        if (!p) {
            break; // (not reached for a supported format)
        }
        if (s.width_star) {
            const int w = va_arg(ap, int);
            s.left = s.left || w < 0;
            s.width = w < 0 ? -w : w;
        }
        if (s.prec_star) {
            const int pr = va_arg(ap, int);
            s.precision = pr < 0 ? -1 : pr;
        }
        switch (s.conv) {
        case '%':
            line.Ch('%');
            break;
        case 'c': {
            const char c = static_cast<char>(va_arg(ap, int));
            if (!s.left) {
                detail::Pad(line, s.width - 1, ' ');
            }
            line.Ch(c);
            if (s.left) {
                detail::Pad(line, s.width - 1, ' ');
            }
            break;
        }
        case 's': {
            const char* text = va_arg(ap, const char*);
            if (!text) {
                text = "(null)"; // (what newlib and glibc print)
            }
            u32 length = 0;
            while (text[length] && (s.precision < 0 || static_cast<int>(length) < s.precision)) {
                ++length;
            }
            const int fill = s.width > static_cast<int>(length) ? s.width - static_cast<int>(length) : 0;
            if (!s.left) {
                detail::Pad(line, fill, ' ');
            }
            line.Str(text, length);
            if (s.left) {
                detail::Pad(line, fill, ' ');
            }
            break;
        }
        case 'd':
        case 'i': {
            long long v;
            if (s.length == 'L') {
                v = va_arg(ap, long long);
            } else if (s.length == 'l' || s.length == 'z') {
                v = s.length == 'l' ? va_arg(ap, long) : static_cast<long long>(va_arg(ap, std::ptrdiff_t));
            } else {
                v = va_arg(ap, int);
            }
            const bool negative = v < 0;
            detail::Integer(line, s, negative ? 0ull - static_cast<u64>(v) : static_cast<u64>(v), negative, 10, false);
            break;
        }
        default: { // u x X
            unsigned long long v;
            if (s.length == 'L') {
                v = va_arg(ap, unsigned long long);
            } else if (s.length == 'l') {
                v = va_arg(ap, unsigned long);
            } else if (s.length == 'z') {
                v = va_arg(ap, std::size_t);
            } else {
                v = va_arg(ap, unsigned int);
            }
            detail::Integer(line, s, v, false, s.conv == 'u' ? 10 : 16, s.conv == 'X');
            break;
        }
        }
    }
    return line.size();
}

// <<< game-thread code

} // namespace printf_lite
} // namespace azl
