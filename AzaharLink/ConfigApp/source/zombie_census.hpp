// The look at a closed game still in memory (the decisions and the lines are in zombie_plan.hpp). It writes to <datadir>/3DSLink-census.log,
// which keeps its last 200 lines.
#pragma once

#include <string>

namespace zombie_census {

// Once per start, after the heap's start line (heap_alloc::WriteStartLog): the line of the APPLICATION limit's classes, always; and, when the
// start found memory of another process in the region (heap_plan's "foreign"), the closed game's process and its shared blocks. Skipped on an
// emulator (no Luma extension, no other process).
void Run(const std::string& datadir);

// True once, when Run found a closed game still in memory: the main page then shows the message that asks to restart the console (a popup:
// a toast would be covered by the toasts of the start, the update of the plugin's copies among them).
bool TakeNotice();

// Every frame: cheap. While a closed game was found, every 5 s the process list is read again (nothing is opened) and the line that says when it
// left memory, or that it is still there (at 1, 5, 15 minutes, then every 30), is written.
void Tick(const std::string& datadir);

} // namespace zombie_census
