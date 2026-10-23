#pragma once

namespace azl {
namespace ctr {

/// Takes the memory the socket layer needs from the game's heap region. It counts against the
/// game's memory budget, so it has to happen before the game starts (games size their heap from
/// what is left at that point; many leave nothing for later). Idempotent; a failure is final.
bool ReserveSocketMemory();
/// Address and size of the buffer given to the socket layer (0 before it is reserved).
unsigned int SocketBufferAddress();
unsigned int SocketBufferSize();
/// Initialises the BSD socket layer (idempotent). Called from the menu thread only (no locking).
bool InitSockets();
/// Why the last InitSockets() failed, in words for the screen (empty after a success).
const char* SocketInitFailure();
void ShutdownSockets();

} // namespace ctr
} // namespace azl
