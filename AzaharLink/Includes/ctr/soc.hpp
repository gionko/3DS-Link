#pragma once

namespace azl {
namespace ctr {

/// Takes the memory the socket layer needs from the game's heap region. It counts against the
/// game's memory budget, so it has to happen before the game starts (games size their heap from
/// what is left at that point; many leave nothing for later). Idempotent; a failure is final.
bool ReserveSocketMemory();
/// For a title of socplan::kLateBlockTitles, instead of ReserveSocketMemory at start-up: the first InitSockets() takes the block, once the game has
/// sized its heap, charged as usual or, when the game has left nothing (an Old 3DS), from the SYSTEM region. `system_at_once`: straight from the SYSTEM region (the
/// way to try that road on a New 3DS).
void LeaveSocketMemoryForLater(bool system_at_once);
/// Address and size of the buffer given to the socket layer (0 before it is reserved).
unsigned int SocketBufferAddress();
unsigned int SocketBufferSize();
/// Initialises the BSD socket layer (idempotent). Called from the menu thread only (no locking).
bool InitSockets();
/// Why the last InitSockets() failed, in words for the screen (empty after a success).
const char* SocketInitFailure();
void ShutdownSockets();
/// The game initialised its own sockets first and the plugin's live in its context (see Sources/ctr/soc.cpp, the guest).
bool SocketsAreGuest();
/// EarlyInit, for a title of socplan::kSocGuestTitles: on 0xC8A0700B the plugin becomes a guest in the game's sockets (else it gives up).
void AllowSocketGuest();

} // namespace ctr
} // namespace azl
