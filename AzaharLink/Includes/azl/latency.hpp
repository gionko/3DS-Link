// Latency of the other players of a local match, read from the game's own UDS packets (Pia and PRUDP v1).
//
// What it does: every SendTo / PullPacket that carries data is copied (the first 96 bytes) into a small ring by the HLE of nwm::UDS
// (ctr/uds_hle.cpp, on the game's thread: no lock, no log, no allocation, nothing on the stack). The network thread (Tick) empties the
// ring and keeps the estimator up to date. It sends nothing and needs no plugin on the other consoles.
//
// When it runs: always. The ring (128 records, 15 KB, on the heap) is created when the console joins a lobby (hosting, connecting, connected or
// spectating: Tick(now_ms, in_session)); before that a game thread pays one load and one branch per command. The log (g_debug_level)
// only decides whether lines are written ("lat: ..."), not whether the estimate is kept: Snapshot() is there for whoever needs the
// values (the chat, later).
//
// How the round trip is found (the same method as the AzaharPing plugin, tried in Azahar on Pia and PRUDP v1 games):
//   * Pia (magic 32 AB 98 64, up to 5.10): every packet carries the sender's timer (bytes 8-9, ms) and the last timer it received from
//     the receiver (bytes 10-11); the time since this console sent that timer is a full round trip with that node. Only 1-3 packets a
//     second per node carry a usable echo in most games, so every packet is looked at.
// What is shown and logged for a node: the median of its last 30 s, kept as 60 half-seconds with one value each (120 s for PRUDP, whose
// samples are rare), so the number does not move while the network does not; the log also has the lowest and the 90th percentile.
//   * PRUDP v1 (EA D0 01): the unreliable "ping" with a stamp (79 B request / 91 B response), the individual ACKs of PING packets, and
//     in Final Fantasy Explorers the game clock of the state packets (one-way delay, also between two clients).
//   * Anything else is counted and its first bytes are logged once, so that a new protocol can be looked at.
// Once the game is known to speak one protocol only that one is checked (a sample of 1 packet in 64 watches for a change).
// The value is what the player feels: it includes the wait for the other game's next frame.
#pragma once

#include <string>

#include "azl/common.hpp"
#include "azl/log_files.hpp"
#include "azl/platform.hpp"

namespace azl {
namespace latency {

/// latency.log: where the lines of this module go (with the log on), each one begun by the console's clock, "hh:mm:ss.mmm lat: ...".
/// The network thread only QUEUES a line (Append: it never touches the card and never waits for a lock); the log writer thread, which
/// holds the swap gate of the Old 3DS, writes the queue (Flush). Nothing is created while the log is off (g_debug_level 0). When the
/// file reaches `rotate_bytes` it becomes latency.1.log (the one before it goes) and a fresh latency.log begins.
class LatencyFile {
public:
    /// `dir` ends with a separator ("/3ds/3DSLink/").
    LatencyFile(FileLayer* files, std::string dir, u64 rotate_bytes = 8ull * 1024 * 1024);
    ~LatencyFile();
    LatencyFile(const LatencyFile&) = delete;
    LatencyFile& operator=(const LatencyFile&) = delete;

    /// A whole line, with its newline. Any thread except that it gives the line up (counted) when the queue's lock is busy or full.
    void Append(const char* text, u32 size);
    /// Writes what is queued (one write, one flush); creates the file with the first line. The writer thread.
    void Flush();
    /// Flush(), then the file is closed.
    void Close();
    /// Lines given up (lock busy or queue full) since the start.
    u32 dropped() const {
        return __atomic_load_n(&dropped_, __ATOMIC_RELAXED);
    }

private:
    void RotateLocked();

    FileLayer* files_;
    std::string dir_;
    u64 rotate_bytes_;
    platform::Mutex queue_mutex_; ///< guards pending_
    std::string pending_;
    u32 dropped_ = 0;
    platform::Mutex file_mutex_; ///< guards file_ and size_ (the writer's)
    void* file_ = nullptr;
    u64 size_ = 0;
};

/// Where Tick() sends its lines: `file` (latency.log) with the console's clock `console_clock_ms` (osGetTime: ms since 1900) for the stamp.
/// Without a sink (the PC tests) the lines go to Log() as before, with no stamp.
void SetSink(LatencyFile* file, u64 (*console_clock_ms)());

/// Where the log gets the name the game gave a node (its UDS username as plain ASCII, `out` of `cap` bytes; false: unknown), written next to the
/// node's number in the "lat: nN ..." lines. Called on the network thread. Only names: no address of any console goes into latency.log.
void SetNameSource(bool (*name_of)(int node, char* out, u32 cap));

/// This console's own link to the room as 3DS Link already measures it (RoomClient::LinkStats: ENet's round trip, its variance, reliable packets lost
/// per thousand), handed over with every window of the plugin's statistics. Any thread (one atomic store). With the log on Tick() writes it into
/// latency.log next to the players' values ("lat: room link 6+-4 ms, loss 5/1000"), so that who reads the file can tell this console's own
/// connection from the other players'. Nothing is measured or sent for it here.
void NoteLink(u32 rtt_ms, u32 rtt_var_ms, u32 loss_permille);

struct NodeRtt {
    int node = 0;       ///< UDS node number (1 = the host of the match)
    int rtt_ms = -1;    ///< round trip, what is shown: the median of the window (one value a half-second, see latency.cpp kSlots), or, while the
                        ///< last 2 s are clearly worse than the window (`spike`), the value of those 2 s; -1 = none
    int window_ms = -1; ///< the median of the window alone
    int recent_ms = -1; ///< the value of the last 2 s alone (-1 = no sample in them)
    bool spike = false; ///< rtt_ms is recent_ms: the last 2 s are at least 1.5 x the window and 30 ms above it (a connection that just got worse)
    int min_ms = -1;    ///< the lowest value of the window
    int p90_ms = -1;    ///< its 90th percentile: far above rtt_ms = a connection that jumps
    int window_s = 30;  ///< how long the window is (30 s)
    int last_ms = -1;   ///< the newest sample
    int age_ms = -1;    ///< >= 0: packets of the game still arrive from this node
    int samples = 0;
    bool stale = false;     ///< no recent sample: the last known value
    bool corrected = false; ///< most samples are PRUDP aggregated acknowledgements with the hold time removed, not a direct measure (the log's A)
    int owd_ms = -1;        ///< one-way delay from the game clock (Final Fantasy Explorers), -1 = none
    bool estimated = false; ///< rtt_ms is 2 x owd_ms
};

/// A SendTo / PullPacket of the game. Game-thread code: copies into the ring and returns (does nothing while the ring does not
/// exist, i.e. until the console has joined a lobby). (No timing of its own: the CPU it uses is in the figures of 3DS Link's own "stats:" line.)
void OnSend(u16 dest_node, u8 channel, const u8* data, u32 size);
void OnRecv(u16 src_node, u8 channel, const u8* data, u32 size);

/// The network thread, once per round (cheap when nothing is due). `in_session`: the console hosts, joins or watches a network.
/// The first call with it true creates the ring. Empties the ring into the estimator; about once a second, with the log on, writes
/// the changes ("lat: ...").
void Tick(u64 now_ms, bool in_session);

/// Estimates now (network thread only). Returns how many nodes still send packets.
int Snapshot(NodeRtt out[8]);
/// "Pia,<pia>,<prudp>,<other>,lock=..." and, for packets of an unknown kind, the first bytes of one; 0 = nothing new.
int ProtoSummary(char* buf, int cap);

} // namespace latency
} // namespace azl
