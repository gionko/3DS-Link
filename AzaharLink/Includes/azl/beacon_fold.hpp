// The beacons of the capture that say nothing new (2026-11-10): a host sends ten beacons a second, each a line of 700 characters of
// hexadecimal, and 99.9% of them are the bytes of the one before from the same transmitter - 55% of all the bytes of the capture on the
// user's console, and the whole load of the card while the console sits in a room doing nothing. The WRITER counts them instead of
// writing them (the producers - the network thread, the engine - store every beacon in the ring as before: nothing changes on their
// side, nor in DebugFormat):
//   - a beacon whose body is the one last written for its transmitter and direction is not written; at the end of the block a line says
//       "  12.345 BEACON rx    00:1f:32:aa:bb:cc ch11 377B: same x19 from 10.402 to 12.340, longest gap 104 ms"
//     (how many, the first and the last of them, and the longest interval between two beacons of that transmitter in the run);
//   - the whole beacon is written when it CHANGES (the count of the old one first), once a minute, and as the first of every file;
//   - a ninth transmitter (the table has eight places; one not heard for a minute gives its place up) is written whole, as before;
//   - a LOST marker of the heavy lane in between: beacons may be among the lost, so the interval across it is not a gap that was seen
//     (the line then ends ", 1 gaps not known (LOST)").
// What is no longer in the file: the arrival time of each identical beacon. The table is the writer's (LogFiles::DrainDebug), made on the
// plugin's heap the first time a pass runs with the log on: about 4.5 KB, nothing with the log off.
#pragma once

#include <string>

#include "azl/common.hpp"
#include "azl/stats.hpp"

namespace azl {

class BeaconFold {
public:
    static constexpr u32 kSlots = 8;
    static constexpr u32 kMaxBody = 512;        ///< a longer body (none seen: they are 331-377 bytes) is written whole
    static constexpr u32 kWholeEveryMs = 60000; ///< the whole beacon once a minute, whatever it says

    /// A record on its way to the file. True: write it (DebugFormat); a count that belongs before it has been appended to `out`.
    /// False: a beacon equal to the last one written for its transmitter, counted here.
    bool Keep(const DebugRecord& rec, const u8* payload, u32 start_ms, std::string* out);
    /// The count lines of what was not written since the last call, appended to `out` (the end of a block, before it is written).
    void Summaries(u32 start_ms, std::string* out);
    /// A new file begins: nothing is remembered, so the first beacon of every transmitter in it is whole.
    void Reset();
    /// Beacons counted and not written since the last call (the writer's line).
    u32 TakeFolded() {
        const u32 n = folded_;
        folded_ = 0;
        return n;
    }

private:
    struct Slot {
        bool used;
        bool prev_known; ///< false after a hole: the interval to the next beacon is not a gap that was seen
        u8 dir;
        u8 channel;
        u8 mac[6];
        u16 size;
        u16 body_len;
        u32 whole_ms;    ///< when the last whole line of this transmitter was written
        u32 prev_ms;     ///< the beacon before, written or counted
        u32 same;        ///< beacons counted since the last line
        u32 first_ms;
        u32 last_ms;
        u32 longest_gap_ms;
        u32 holes;
        u8 body[kMaxBody];
    };
    void Summary(Slot& slot, u32 start_ms, std::string* out);

    Slot slots_[kSlots] = {};
    u32 last_ms_ = 0; ///< the time of the last record seen: the time column of a count line (the column never goes back)
    u32 folded_ = 0;
};

} // namespace azl
