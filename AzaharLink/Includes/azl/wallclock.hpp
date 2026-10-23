// The console's wall clock as text, for the lines of the log that a person has to line up with another clock (a PC's,
// a photo of the Luma exception screen, the dates of the files on the card): the log's own "[sss.mmm]" is the
// plugin's uptime and says nothing about WHEN. No printf (see azl/line.hpp).
#pragma once

#include "azl/common.hpp"
#include "azl/line.hpp"

namespace azl {

/// Writes "YYYY-MM-DD hh:mm:ss.mmm" (23 characters and a terminator: `out` holds at least 24) for `os_ms`, the value
/// of libctru's osGetTime(): milliseconds since 1900-01-01 00:00, in the console's own clock (the time the user set;
/// the console has no time zone). Years 1900 to 9999.
inline void FormatConsoleClock(u64 os_ms, char* out) {
    const u64 total_seconds = os_ms / 1000;
    const u32 millis = static_cast<u32>(os_ms % 1000);
    const u32 days_since_1900 = static_cast<u32>(total_seconds / 86400);
    const u32 second_of_day = static_cast<u32>(total_seconds % 86400);

    // Days since 1970-01-01 (25567 days after 1900-01-01) to year, month, day: Howard Hinnant's civil_from_days, on
    // a year that starts in March so the leap day is the last day of it.
    const s32 z = static_cast<s32>(days_since_1900) - 25567 + 719468;
    const s32 era = (z >= 0 ? z : z - 146096) / 146097;
    const u32 doe = static_cast<u32>(z - era * 146097);                              // [0, 146096]
    const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;           // [0, 399]
    const s32 y = static_cast<s32>(yoe) + era * 400;
    const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                         // [0, 365]
    const u32 mp = (5 * doy + 2) / 153;                                              // [0, 11]
    const u32 day = doy - (153 * mp + 2) / 5 + 1;                                    // [1, 31]
    const u32 month = mp < 10 ? mp + 3 : mp - 9;                                     // [1, 12]
    const u32 year = static_cast<u32>(y + (month <= 2 ? 1 : 0));

    LineBuf line(out, 23);
    line.DecPad(year, 4, '0').Ch('-').DecPad(month, 2, '0').Ch('-').DecPad(day, 2, '0').Ch(' ')
        .DecPad(second_of_day / 3600, 2, '0').Ch(':').DecPad(second_of_day / 60 % 60, 2, '0').Ch(':')
        .DecPad(second_of_day % 60, 2, '0').Ch('.').DecPad(millis, 3, '0');
    out[line.size()] = '\0';
}

} // namespace azl
