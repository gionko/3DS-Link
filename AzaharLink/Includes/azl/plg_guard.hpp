// The framework's link to Luma3DS's plugin loader (the port plg:ldr), guarded. CTRPluginFramework 0.8.0 connects once, in KeepThreadMain
// (bl plgLdrInit, its result ignored), before PatchProcess: when svcConnectToPort("plg:ldr") fails - the port has ONE session (Rosalina's
// main.c), and the loader's own session, closed just before, may not be finalized yet - plgLdrInit leaves its event pointer NULL, and the
// first PLGLDR__FetchEvent reads address 0 (console 63, SSF4, data abort at 0x07041D00: REGISTRO, phase N). And a session that came without
// its arbiter (the IPC 0x90000 failed: plgLdrInit still returns the connection's success) gives every reply of the plugin to an arbiter 0:
// Luma3DS then waits its 10 s at every event.
//   Sources/ctr/plugin.cpp wraps both (-Wl,--wrap=plgLdrInit, -Wl,--wrap=PLGLDR__FetchEvent): the connection is tried up to kMaxTries times,
// kRetryMs apart (about 2 s; svcConnectToPort never waits, a try costs microseconds, and the game is held by the framework until PatchProcess
// has run); a try counts only with the connection AND the arbiter. Without them after the budget the plugin stays out of the launch (EarlyInit:
// no hooks, no network, no chat, no mirror; a notice on screen) and FetchEvent answers 0, "no event", which KeepThreadMain takes back to its
// wait (0x0706C868-0x0706C874: values outside 1..6). Never a connection tried later: plg:ldr and the events are served by ONE loop of
// Rosalina, and a connection tried while it waits for a reply of ours blocks there 10 s, then reads an event word left from before.
// What is here is the pure part (PC-tested by Tests/plg_guard_test.cpp): the decision after each try, the loop with the calls given to it,
// the fetch, and the line. The wrappers run on the framework's keep-alive thread (4 KB of stack, before anything else of the plugin): no
// Log, no string, no heap; the line is written by EarlyInit.
#pragma once

#include <cstdio>

#include "azl/common.hpp"

namespace azl {
namespace plgguard {

constexpr u32 kMaxTries = 100;
constexpr u32 kRetryMs = 20;

struct State {
    u32 tries = 0;            ///< tries made (1 = the first one went through)
    s32 first_error = 0;      ///< the first failure of svcConnectToPort (0: none)
    u32 no_arbiter = 0;       ///< tries whose session came without its arbiter
    u32 ms = 0;               ///< how long the tries took, in all
    bool emulator = false;    ///< an emulator's plg:ldr (Azahar): a session without an arbiter is taken
    bool connected = false;   ///< the framework's events can be fetched
};

enum class Step { Done, Again, GiveUp };

/// One try's outcome: `rc` what plgLdrInit returned (the connection's result), `arbiter` the arbiter handle the session gave (0: none).
inline Step After(State* s, s32 rc, u32 arbiter) {
    ++s->tries;
    if (rc >= 0 && arbiter == 0) {
        ++s->no_arbiter;
    }
    if (rc >= 0 && (arbiter != 0 || s->emulator)) {
        s->connected = true;
        return Step::Done;
    }
    if (rc < 0 && s->first_error == 0) {
        s->first_error = rc;
    }
    return s->tries >= kMaxTries ? Step::GiveUp : Step::Again;
}

/// What one try returns to the loop.
struct Try {
    s32 rc;
    u32 arbiter;
};

/// The tries: `try_once()` -> Try (one plgLdrInit), `close()` (plgLdrExit: a session without its arbiter goes, so that the next try starts
/// clean), `pause()` (kRetryMs). Returns the last result (0 when connected).
template <class TryOnce, class Close, class Pause>
s32 Connect(State* s, TryOnce try_once, Close close, Pause pause) {
    for (;;) {
        const Try t = try_once();
        const Step step = After(s, t.rc, t.arbiter);
        if (step == Step::Done) {
            return 0;
        }
        if (t.rc >= 0) {
            close();
        }
        if (step == Step::GiveUp) {
            return t.rc < 0 ? t.rc : static_cast<s32>(0xD8E007F7); // (the arbiter missing: an invalid handle)
        }
        pause();
    }
}

/// The fetch: the framework's event when connected, else 0 ("no event"). Never a connection tried here.
template <class Fetch>
s32 FetchOrNone(const State& s, Fetch fetch) {
    return s.connected ? fetch() : 0;
}

/// Whether the synchronous line is written: anything but a first try that went through with its arbiter.
inline bool WorthALine(const State& s) {
    // (an emulator's session with no arbiter, taken at the first try, is its normal case: no synchronous line at every launch)
    return s.tries != 1 || (s.no_arbiter != 0 && !s.emulator) || !s.connected;
}

/// The line (no clock, no newline) in `out`; returns its length, or 0 if it did not fit.
inline int FormatLine(const State& s, char* out, std::size_t size) {
    int n;
    if (s.connected) {
        n = std::snprintf(out, size, "plg:ldr: the framework connected to Luma3DS's plugin loader at try %lu of %lu after %lu ms (first error 0x%08lx, %lu tries without the arbiter%s)",
                          static_cast<unsigned long>(s.tries), static_cast<unsigned long>(kMaxTries), static_cast<unsigned long>(s.ms),
                          static_cast<unsigned long>(static_cast<u32>(s.first_error)), static_cast<unsigned long>(s.no_arbiter),
                          s.emulator && s.no_arbiter ? ", taken on an emulator" : "");
    } else {
        n = std::snprintf(out, size, "plg:ldr: the framework could NOT connect to Luma3DS's plugin loader in %lu tries, %lu ms (first error 0x%08lx, %lu tries without the arbiter): the plugin stays off for this launch, restart the game",
                          static_cast<unsigned long>(s.tries), static_cast<unsigned long>(s.ms), static_cast<unsigned long>(static_cast<u32>(s.first_error)),
                          static_cast<unsigned long>(s.no_arbiter));
    }
    return n > 0 && static_cast<std::size_t>(n) < size ? n : 0;
}

} // namespace plgguard
} // namespace azl
