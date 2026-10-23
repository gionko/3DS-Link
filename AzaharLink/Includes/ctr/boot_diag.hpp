// A DIAGNOSTIC build of the plugin (make DIAG=1, never the delivered one): what the plugin finds in the game's process in its first seconds,
// written to a file at once - AzaharLink.boot.log in /3ds/3DSLink - because the log writer thread starts only after ~5 s and a game that
// dies before that leaves no trace (2026-10-23: Pac-Man & Galaga Dimensions, Ridge Racer 3D and Ocarina of Time 3D die on Friend-A's console
// with ErrDisp 0xE0A01839, the result of the game's own svcCreateThread, before any line reaches the card).
//
// Without AZL_BOOT_DIAG every function here is an empty inline one: the delivered plugin is the same code as without this file.
//
// What the file holds: the Luma3DS facts a plugin can read (version, configuration bits - bit 3 is "Redirect app. syscore threads to core2" -,
// the plugin loader flags, whether thread redirection is disabled), every resource limit of the process (current / maximum: PRIORITY and
// CPUTIME too, which the main log does not show), the threads of the process with priority and core, and then, for ten seconds, a line each
// time the thread count, the commit or the CPU time change (and one a second). Two switches by file name make a bisect possible without a new
// build: an (empty) file /3ds/3DSLink/diag-no-soc makes the plugin skip the socket buffer (so it stays OFF for the launch, like enabled=0, with
// no hooks), /3ds/3DSLink/diag-no-hooks makes it take the socket buffer but not patch the game.
#pragma once

namespace azl {
namespace ctr {
namespace bootdiag {

#ifdef AZL_BOOT_DIAG
void Begin();                 ///< opens (starts afresh) the file and writes the state of the process as it is
void Mark(const char* what);  ///< the state again, under a name
bool Skip(const char* what);  ///< is the file /3ds/3DSLink/diag-no-<what> there?
void StartWatch();            ///< a thread that watches the process for ten seconds
#else
inline void Begin() {}
inline void Mark(const char*) {}
inline bool Skip(const char*) {
    return false;
}
inline void StartWatch() {}
#endif

} // namespace bootdiag
} // namespace ctr
} // namespace azl
