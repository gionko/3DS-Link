// (DPAPP-2j, R-A) The closed game pm keeps of a Download Play guest in the form M3c1, freed by 3DS Link (the rules are zombie_plan.hpp Judge: only a
// TERMINATED process with no thread left, only a key of the band of a record of ours, never 3DS Link, never on an emulator, only while 3DS Link is the
// running application). The one file of the app that opens pm:app, and it sends one command only: 12, UnregisterProcess. Lines in
// <datadir>/3DSLink-census.log, every one with its milliseconds.
#pragma once

#include <cstdint>
#include <string>

namespace zombie_free {

// Once per start, after the census (zombie_census::Run) and after the records are read (dlpguest::StartGuests): for each key of a record of ours,
// the processes of the key are read, judged, and pm is asked to forget each closed one (UnregisterProcess, pm:app opened non-blocking and closed at
// once); then the process list is read again for 500 ms at most: a pid gone is freed, a pid still there was not pm's (the line says to restart).
void FreeAtStart(const std::string& datadir);

// The Join's gate, right before A joins a host whose guest is `key`: true when a process of the key is still in memory (a closed game pm or
// somebody else keeps), or the list of the processes could not be read on a console - the jump would start a second one (pm looks for the id NS
// launches, 00040001|low, and does not see the closed 00040000|low). `ms` gets the time of the look. Never true on an emulator. (DPAPP-2j-rev)
// A list not read whole is read once more; `unread` is true when the answer is true for that alone (no process of the key seen): the gate says it
// so, never "a closed game".
bool Remaining(std::uint64_t key, std::uint32_t* ms, bool* unread);

} // namespace zombie_free
