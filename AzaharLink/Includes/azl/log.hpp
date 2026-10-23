// Minimal logging. On the console the sink appends to a file on the SD card (see main.cpp), on
// the PC it goes to stderr. Never log from hot per-packet paths.
#pragma once

#include <cstdarg>
#include <functional>

#include "azl/line.hpp"

namespace azl {

using LogSink = std::function<void(const char* line)>;

/// Replaces the sink (default: stderr on the host, discard on the console until set).
void SetLogSink(LogSink sink);
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/// A line of the main log built piece by piece (azl/line.hpp: Str, Hex, Dec, SDec), sent when the object goes out of
/// scope: the same bytes Log() makes from the matching format, with no printf, no formatter and no buffer on the
/// caller's stack. This is what code on the GAME's threads uses (the svc hook): Tests/policy_check.sh keeps Log() out
/// of it. The text sits in one static buffer under the log's lock, which the object holds: build nothing else (no
/// Log, no second LogLine) while one is alive. With the log off every append does nothing and nothing is locked.
class LogLine : public LineBuf {
public:
    LogLine();
    ~LogLine();
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

private:
    bool locked_;
};

} // namespace azl
