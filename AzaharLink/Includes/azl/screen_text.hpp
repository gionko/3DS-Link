// What the plugin says on the screen about two things the numbers decide: how much memory the game has left, and why the
// game's receive rings overflowed. Pure functions and a small state machine, no system calls, so the PC tests run them
// (Tests/screen_text_test.cpp); Plugin (ctr/plugin.cpp) reads the numbers from the system and shows the result.
#pragma once

#include <string>

#include "azl/common.hpp"

namespace azl {

// ---------------------------------------------------------------------------------------------
// The game's memory. "Free" is what the application region of the console has left: its size (Luma's
// svcGetSystemInfo 0x10000/0x80) less what is handed out (svcGetSystemInfo 0/1) - the figure of the log's `mem:` line, and
// the plugin's own 5 MiB block is in it on a New 3DS (Luma takes it from this region) and not on an Old 3DS (it comes
// from the system region).
//
// Two texts. About 20 s after the plugin started, when a game has finished allocating, ONE toast says how much is free and
// in which memory mode the console runs ("New 3DS memory mode: 55 MB free", "Old 3DS memory mode: 28 MB free"), whatever the
// figure is; the Status page shows the same figure, read again every 30 s. A WARNING ("Game memory almost full: 3 MB free") is
// told when the figure FALLS below a limit after that first reading - the memory filled up while the game ran - once each time
// it falls, not at the first reading and not every minute: a game that takes all of its region at the start and runs fine
// would otherwise be warned about at every launch.
//
// The limits and that rule are chosen, not measured on a game that crosses them:
//   * Hardware: Zelda Tri Force Heroes (extended memory) and Final Fantasy Explorers on a New 3DS both sit at 70,656 KB used of
//     126,976 KB (56,320 KB = 55 MB free) from the first reading at 10 s to the end of a 2,000 s session, plugin block
//     (5,120 KB) included. 0.1.15 warned of a low memory margin for them: it compared what the game MAY commit with what is
//     free, which no game does.
//   * Emulator, the Old 3DS memory layout (64 MiB region): both of them use 65,532 / 65,536 of 65,536 KB (4 and 0 KB free)
//     from the first reading: a game that sizes its heap from what is left (Final Fantasy Explorers: the 64 MiB minus its commit)
//     takes the whole region by design. A limit applied to the first reading would warn at every launch of such a game, which is
//     the false alarm of 0.1.15 again; hence the rule above.
//   * ONE limit, 5 MB, on both layouts: the size of AzaharLink's own block. A fall to less than the plugin's own block is what
//     the 5 MiB could be blamed for on a New 3DS; no title on record is near it (a research pass found none documented to need
//     more than 119 MiB; the only ones that might want the whole 124 MiB are Xenoblade Chronicles 3D and Minecraft New 3DS
//     Edition, and only the second has local wireless). On an Old 3DS the plugin takes nothing from this region and no Old 3DS
//     has run it, so a different number there would be invented: it gets the same one, to be revised with the first log (the
//     toast says the figure either way).
// ---------------------------------------------------------------------------------------------
namespace gamemem {

/// An application region of at least this size is the New 3DS layout (124 MiB); an Old 3DS / 2DS has 64 MiB or 80 MiB.
constexpr long long kNewLayoutRegionBytes = 100ll << 20;
constexpr long kLowKb = 5 * 1024;
/// A game that went below the limit and came back must be 1 MB above it before the next fall is told again.
constexpr long kRearmKb = 1024;
/// The first reading, after the plugin started (a game is done allocating by then: 10-20 s on the games measured), and the
/// time between the readings after it.
constexpr u32 kFirstReadingMs = 20000;
constexpr u32 kRefreshMs = 30000;

struct Reading {
    bool valid = false;
    bool new_layout = false; ///< the console runs the New 3DS memory layout
    long region_kb = 0;
    long used_kb = 0;
    long free_kb = 0;
};

/// From the two numbers the system gives (bytes). Not valid when either is unusable.
inline Reading MakeReading(long long region_bytes, long long used_bytes) {
    Reading r;
    if (region_bytes <= 0 || used_bytes < 0 || used_bytes > region_bytes) {
        return r;
    }
    r.valid = true;
    r.new_layout = region_bytes >= kNewLayoutRegionBytes;
    r.region_kb = static_cast<long>(region_bytes / 1024);
    r.used_kb = static_cast<long>(used_bytes / 1024);
    r.free_kb = r.region_kb - r.used_kb;
    return r;
}

inline bool IsLow(const Reading& r) {
    return r.valid && r.free_kb < kLowKb;
}

/// "55 MB" (MB = 1024 KB, rounded down: "at least"), or "640 KB" below one MB.
inline std::string FreeText(long free_kb) {
    if (free_kb < 1024) {
        return std::to_string(free_kb < 0 ? 0 : free_kb) + " KB";
    }
    return std::to_string(free_kb / 1024) + " MB";
}

/// Under this much free the game has taken the whole region (Mario Party: The Top 100 leaves 0 KB, hardware 2026-10-14; Final
/// Fantasy Explorers on the Old layout of the emulator 4 KB): "0 KB free" is true and reads like an error, so the texts say what
/// it means.
constexpr long kAllInUseKb = 256;

inline bool AllInUse(const Reading& r) {
    return r.free_kb < kAllInUseKb;
}

inline std::string InfoText(const Reading& r) {
    const std::string mode = std::string(r.new_layout ? "New 3DS" : "Old 3DS") + " memory mode: ";
    return AllInUse(r) ? mode + "all memory in use" : mode + FreeText(r.free_kb) + " free";
}

inline std::string LowText(const Reading& r) {
    return "Game memory almost full: " + FreeText(r.free_kb) + " free";
}

/// The line of the Status page (at most 44 characters: the width of the framework's message box).
inline std::string StatusText(const Reading& r) {
    if (!r.valid) {
        return "Game memory: read " + std::to_string(kFirstReadingMs / 1000) + " s after the start";
    }
    const std::string mode = std::string(" (") + (r.new_layout ? "New 3DS" : "Old 3DS") + " mode)";
    return AllInUse(r) ? "Game memory: all in use" + mode : "Game memory: " + FreeText(r.free_kb) + " free" + mode;
}

/// When to read and what to say. Times are the milliseconds since the plugin started.
class Watch {
public:
    enum class Say { Nothing, Info, Low };

    /// Time for a reading? (The first one at kFirstReadingMs, then every kRefreshMs.)
    bool Due(u32 uptime_ms) const {
        return uptime_ms >= next_ms_;
    }

    /// A reading taken at `uptime_ms`. Info: the one toast of the start (the memory mode and the figure, whatever it is); Low: the
    /// figure is below the limit and the last reading was not - a fall, never the first reading.
    Say Update(const Reading& r, u32 uptime_ms) {
        next_ms_ = uptime_ms + kRefreshMs;
        if (!r.valid) {
            return Say::Nothing; // (tried again at the next refresh; the first toast waits for a reading)
        }
        const bool low = IsLow(r);
        if (!first_done_) {
            first_done_ = true;
            warned_ = low; // (a game that is full from the start is not warned about: only a fall is news)
            return Say::Info;
        }
        if (low) {
            if (!warned_) {
                warned_ = true;
                return Say::Low;
            }
            return Say::Nothing;
        }
        if (r.free_kb >= kLowKb + kRearmKb) {
            warned_ = false; // (back above the limit: the next fall is news again)
        }
        return Say::Nothing;
    }

private:
    u32 next_ms_ = kFirstReadingMs;
    bool first_done_ = false;
    bool warned_ = false;
};

} // namespace gamemem

// ---------------------------------------------------------------------------------------------
// "frames lost": the game's receive ring (a bind's queue of frames) pushed frames out to make room for newer ones. Two
// very different things look the same in that counter. The game reads, but not fast enough - "reads too slowly" - or the
// game is not reading at all: paused, in a loading screen, waiting for a menu, with the console's lid closed. The numbers
// that tell them apart are the game's UDS calls: a game that reads calls PullPacket 95 to 300 times a second, so a window
// with none, or a game that has made none for a second when the window ends, is not reading (a pause that began in the
// middle of a window is the common case: the window still holds the calls from before it).
// ---------------------------------------------------------------------------------------------
namespace frameslost {

/// More frames than this pushed out of the rings in a window of the statistics is worth a toast.
constexpr unsigned long kWarnAbove = 50;
/// No UDS call for this long at the end of the window is "not reading" (a reading game never stops for a second).
constexpr u32 kNotReadingMs = 1000;

inline bool NotReading(unsigned long uds_calls_in_window, u32 idle_ms) {
    return uds_calls_in_window == 0 || idle_ms >= kNotReadingMs;
}

/// How long ago the game last made a UDS call, from looks at a counter of its calls taken about once a second. The counter is
/// cleared when a window of the statistics ends (CounterCleared), so "changed" is "different", not "bigger". The first look is
/// taken as activity (nothing has overflowed before the game has bound anything).
class Idle {
public:
    void Look(u32 calls, u64 now_ms) {
        if (!looked_ || calls != seen_) {
            last_call_ms_ = now_ms;
            looked_ = true;
        }
        seen_ = calls;
    }
    void CounterCleared() {
        seen_ = 0;
    }
    /// Milliseconds since a look found the counter changed (as of the look, so a second's resolution at the most).
    u32 IdleMs(u64 now_ms) const {
        return static_cast<u32>(now_ms > last_call_ms_ ? now_ms - last_call_ms_ : 0);
    }

private:
    bool looked_ = false;
    u32 seen_ = 0;
    u64 last_call_ms_ = 0;
};

inline std::string Text(unsigned long lost, bool not_reading) {
    return "AzaharLink: " + std::to_string(lost) +
           (not_reading ? " frames lost, the game is not reading the network (paused?)"
                        : " frames lost, the game reads too slowly");
}

} // namespace frameslost

// ---------------------------------------------------------------------------------------------
// The pointer to the log that the texts of the start-up carry while the log is off: with debug=1 the details are being
// written already, and telling the user to switch on what is on is noise.
// ---------------------------------------------------------------------------------------------
inline const char* DebugHint(bool log_on) {
    return log_on ? "" : " (set debug=1 for details)";
}

} // namespace azl
