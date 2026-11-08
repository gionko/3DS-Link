// The app's own __system_allocateHeaps (heap_alloc.cpp; the decisions are in heap_plan.hpp) and the line it leaves for the log.
#pragma once

#include <string>

namespace heap_alloc {

// Adds what the start-up allocation did, as one line with the console's date and time and the time since boot, to <datadir>/3DSLink-heap.log,
// which keeps the lines of the last heap_plan::kLogMaxLines starts (at most kLogMaxBytes; the oldest go first). Once per start. Called once the
// app may write to the card: by main() on every start but the first, and on the first start as soon as its question was answered Yes
// (pages_startup.cpp, Begin).
void WriteStartLog(const std::string& datadir);

// Did the start find memory of another process in the APPLICATION region (heap_plan's "foreign", first or last reading)? A closed game still
// in memory is the case it is for (zombie_census.cpp looks at it).
bool ForeignAtStart();

} // namespace heap_alloc
