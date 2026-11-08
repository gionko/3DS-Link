// The kernel's books of the game's process (its KResourceLimit), for the blocks the plugin takes from outside the game's budget:
// the socket block of a late title (ctr/soc.cpp) and the gate's page of the svc hook. The arithmetic and why it is needed:
// Includes/azl/books_math.hpp. Here: finding the live object, its limit written in kernel mode, its current moved through the kernel's calls.
#pragma once

#include <3ds/types.h>

namespace azl {
namespace ctr {

/// Finds the game's KResourceLimit and checks the offsets of its words against what the kernel's own calls say. Nothing below
/// writes anything until this said yes once (a later look that disagrees - the game allocating between the two reads - does not take
/// it back: the object and its offsets do not move). Logs what it found.
/// The calls below may come from two of the plugin's threads (the gate's page at LateInit, the socket block of a late title at its first
/// connection): they take turns.
bool FindBooks();
/// current += size (a block taken after the game sized its heap, the game running): the books read, the limit lifted by `size` and by what
/// current stands above it (books::ChargeLift), the kernel's own reservation (svcControlMemory, freed with svc 0xA3), the limit back down by
/// the same (books::ChargeThroughKernel); when the kernel had no pages for the reservation (0xD86007F3: an Old 3DS whose game holds the whole
/// region, Monster Hunter XX), current += size written in kernel mode instead, as 1.0.13-1.1.1 did (books::ChargeOrWrite). Made BEFORE the
/// block is taken. False: the books as they were (books not found, or the kernel refused: 0xC860180A, a game thread took the lifted room
/// first, no fallback); the line says which way and the kernel's result. Not on the keep-alive thread (it makes svcs and logs).
bool ChargeBooks(u32 size);
/// current -= size: a ChargeBooks taken back when the block was not had (a block of svc 0xA3 freed with svcControlMemory, whose FREE releases
/// it: books::ReleaseThroughKernel, in the SYSTEM, APPLICATION, BASE regions); when no region had a page, what was not released is written
/// off current in kernel mode (books::UnchargeOrWrite). False: what was not released stays (current above the private blocks by it until a
/// reboot: the end of the game still gives its budget back); the line says which way and how much.
bool UnchargeBooks(u32 size);
/// current += size and limit += size (a block taken before the game sizes its heap: the room the game sees does not change): the limit lifted,
/// then the kernel's own reservation (books::RaiseThroughKernel). False: the books as they were (books not found, or the kernel refused, the
/// limit back down); the block must not stay then. `rc`: the kernel's result of the step that failed (0 when it was made).
bool RaiseBooks(u32 size, Result* rc);
/// limit -= size, after the game has its heap (WaitForGameHeap). False when the books were not found. Waits for the lock of the books:
/// not on the keep-alive thread (TrySettleOwedBooks).
bool SettleBooks(u32 size);
/// What TrySettleOwedBooks did.
enum class OwedSettle : u32 {
    Nothing, ///< nothing owed (or taken by another before this one)
    Settled, ///< the bytes owed taken and the limit lowered by them
    Busy,    ///< the lock of the books held by another thread: nothing taken, the bytes stay owed
    NoHeap,  ///< the game's heap not there yet: nothing taken, the bytes stay owed
};
/// The framework's keep-alive thread (SWAP_ENTER, EXIT; 4 KB of stack, it answers Luma3DS): the Settle still owed, as SettleBooks, but
/// never waiting: the lock of the books is only tried, and the bytes (books::TakeOwedOnceHeapSeen on `owed`) are taken under it. `bytes`:
/// what was settled. No line.
OwedSettle TrySettleOwedBooks(u32* owed, bool heap_seen, u32* bytes);
/// The commit's limit and current value, read from the object in kernel mode. False when the books were not found.
bool ReadBooks(s32* limit, s32* current);
/// Whether the game has mapped its heap (its region at 0x08000000 is not FREE): one svcQueryMemory, no wait.
bool GameHeapSeen();
/// Waits (at most 2 s, in steps of 50 ms) until GameHeapSeen. Not on a game thread. False when no heap was seen.
bool WaitForGameHeap();

} // namespace ctr
} // namespace azl
