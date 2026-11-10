// Latency of the other players of a local match, read from the game's own UDS packets (Pia and PRUDP v1).
//
// What it does: every SendTo / PullPacket that carries data is copied (the first 96 bytes) into a small ring by the HLE of nwm::UDS
// (ctr/uds_hle.cpp, on the game's thread: no lock, no log, no allocation, nothing on the stack). The network thread (Tick) empties the
// ring and keeps the estimator up to date. It sends nothing and needs no plugin on the other consoles.
//
// When it runs: always. The ring (29 KB, on the heap) is created when the console joins a lobby (hosting, connecting, connected or
// spectating: Tick(now_ms, in_session)); before that a game thread pays one load and one branch per command. The log (g_debug_level)
// only decides whether lines are written ("lat: ..."), not whether the estimate is kept: Snapshot() is there for whoever needs the
// values (the chat, later).
//
// How the round trip is found (the same method as the AzaharPing plugin, tried in Azahar on Pia and PRUDP v1 games):
//   * Pia (magic 32 AB 98 64, up to 5.10): every packet carries the sender's timer (bytes 8-9, ms) and the last timer it received from
//     the receiver (bytes 10-11); the time since this console sent that timer is a full round trip with that node. Only 1-3 packets a
//     second per node carry a usable echo in most games, so every packet is looked at.
//   * PRUDP v1 (EA D0 01): the unreliable "ping" with a stamp (79 B request / 91 B response), the individual ACKs of PING packets, and
//     in Final Fantasy Explorers the game clock of the state packets (one-way delay, also between two clients).
//   * Anything else is counted and its first bytes are logged once, so that a new protocol can be looked at.
// Once the game is known to speak one protocol only that one is checked (a sample of 1 packet in 64 watches for a change).
// The value is what the player feels: it includes the wait for the other game's next frame.
#pragma once

#include "azl/common.hpp"

namespace azl {
namespace latency {

struct NodeRtt {
    int node = 0;       ///< UDS node number (1 = the host of the match)
    int rtt_ms = -1;    ///< round trip, median of the recent samples; -1 = none
    int min_ms = -1;
    int last_ms = -1;
    int age_ms = -1;    ///< >= 0: packets of the game still arrive from this node
    int samples = 0;
    bool stale = false;     ///< no recent sample: the last known value
    int owd_ms = -1;        ///< one-way delay from the game clock (Final Fantasy Explorers), -1 = none
    bool estimated = false; ///< rtt_ms is 2 x owd_ms
};

/// A SendTo / PullPacket of the game. Game-thread code: copies into the ring and returns (does nothing while the ring does not
/// exist, i.e. until the console has joined a lobby). With the log on it also counts the ticks it used (the "lat: cpu" line).
void OnSend(u16 dest_node, u8 channel, const u8* data, u32 size);
void OnRecv(u16 src_node, u8 channel, const u8* data, u32 size);

/// The network thread, once per round (cheap when nothing is due). `in_session`: the console hosts, joins or watches a network.
/// The first call with it true creates the ring. Empties the ring into the estimator; about once a second, with the log on, writes
/// the changes ("lat: ...") and every 10 s the CPU used by this module ("lat: cpu ...").
void Tick(u64 now_ms, bool in_session);

/// Estimates now (network thread only). Returns how many nodes still send packets.
int Snapshot(NodeRtt out[8]);
/// "Pia,<pia>,<prudp>,<other>,lock=..." and, for packets of an unknown kind, the first bytes of one; 0 = nothing new.
int ProtoSummary(char* buf, int cap);

} // namespace latency
} // namespace azl
