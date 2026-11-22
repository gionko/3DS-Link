// Cheap counters used to find out *why* a session lags. The counts are plain integers bumped from several
// threads without locking: a lost increment now and then does not matter, what matters is that the
// hot paths pay nothing. The TIME sums (64-bit, at the end of the struct) are added to atomically and
// read-and-cleared one by one, so a sum is never torn or lost: a 32-bit tick sum wraps after 16 s, and the
// statistics window of a busy writer can be longer than that. Plugin::LogStats() prints and resets the
// per-interval ones every few seconds (TakeStats).
#pragma once

#include <cstddef>
#include <string>

#include "azl/common.hpp"
#include "azl/line.hpp"
#include "azl/ticks.h"

namespace azl {

/// The soc calls the network thread makes, as counted in Stats::soc_*.
enum SocKind : int { kSocRecvData = 0, kSocRecvEmpty, kSocSend, kSocPoll, kSocKinds };

struct Stats {
    // Room thread, indexed by WifiPacket::Type (0..5).
    u32 rx_frames[6];
    u32 tx_frames[6];
    u32 rx_bytes;
    u32 tx_bytes;
    u32 tx_reliable;   ///< frames sent with ENet's reliable flag
    u32 tx_unreliable; ///< frames sent unreliably (only when "fast data" is on)
    u32 queue_max;     ///< most frames waiting in the outgoing queue at once
    u32 tx_queue_bytes_max;   ///< ... and most bytes
    u32 tx_stale_drops;       ///< frames dropped from the queue because they waited for more than a second
    u32 tx_cap_drops;         ///< ... because the queue (frames, bytes) or ENet's reliable backlog was full
    u32 tx_duplicates;        ///< copies of unicast game frames sent a round after the original (radio_like=2)
    u32 rx_early;             ///< reliable packets of the room handed over out of order, ahead of a hole (radio_like=2: ENet's azlDeliverOutOfOrder)
    u32 tx_throttled;         ///< unreliable packets ENet's packet throttle destroyed at the sender (0 since the throttle no longer decelerates)
    u32 ack_samples_skipped;  ///< acknowledgements whose round-trip sample was not taken (the thread had been away: a pause, not the link)
    // The room's reliable stream as ENet sees it (ENetAzlCounters; the plugin takes them once per network round):
    u32 enet_ooo;             ///< reliable packets of the room that arrived behind a hole (an earlier datagram was lost or late)
    u32 enet_hol_max_ms;      ///< ... the longest one waited behind the hole (ms; a maximum): the head-of-line blocking
    u32 enet_dup_rx;          ///< reliable packets the room sent again after they had been handled: its retransmissions that reached us
    u32 enet_ack_only;        ///< datagrams sent to the room that held acknowledgements and nothing else (a soc call for nothing)
    u32 enet_ack_data;        ///< ... that held acknowledgements and data: the acknowledgement rode along
    u32 enet_passes;          ///< receive passes that read at least one datagram
    u32 enet_pass_dgrams_max; ///< most datagrams one pass read
    u32 enet_pass_ms_max;     ///< longest pass (ms)
    u32 enet_recv_skipped;    ///< receives not made because the socket was known empty and a poll followed (each is a soc call saved)
    u32 enet_passes_capped;   ///< read passes that stopped at the limit of datagrams or of time with the socket not empty (the backlog was handed over in pieces)
    u32 enet_silence_probes;  ///< pings sent because the room spoke while packets of ours had gone unanswered for 0.5 s or more (the link came back)
    u32 enet_events_before_ack; ///< packets of the room handed to the game's code before the acknowledgement of their datagram was sent
    u32 enet_ack_limit_hits;  ///< times the acknowledgements were sent because they had waited for 8 packets or 8 ms
    u32 enet_ack_hold_max_ms; ///< the longest an acknowledgement waited between being queued and being sent (ms; a maximum)
    // The flow of the room's stream (2026-11-14, Mario Party: Island Tour; the line "room stream flow", azl/flow_relief.hpp):
    u32 enet_cap_refused;     ///< commands of the room ENet did not take because the data that waited was at the limit (192 KB): not acknowledged, the room sends them again
    u32 enet_cap_admitted;    ///< ... and the ones taken over the limit because they were the next of the sequence (they close the hole the waiting data is behind)
    u32 enet_cap_dropped;     ///< ... and the game frames acknowledged and dropped at the limit instead of refused (the listed titles: RoomClient::SetCapDrop)
    u32 enet_cap_full;        ///< ... the refused ones (in enet_cap_refused) that would have been dropped but the placeholders were at their maximum (the title's bound)
    u32 enet_cap_held_max;    ///< ... the most placeholders of dropped frames held at once (a maximum)
    u32 enet_seq_skipped;     ///< reliable sequence numbers of the room never seen (a new command lost on the way - Wi-Fi or the socket's ring - or late)
    u32 enet_seq_skipped_away; ///< ... found in what had waited in the socket while the network thread was away 100 ms or more (the ring may have overflowed)
    u32 enet_away_episodes;   ///< (2026-11-15) the times the network thread was away 100 ms or more and the socket's ring filled alone ...
    u32 enet_away_dgrams_max; ///< ... the most datagrams read after one of them before the socket was empty (a maximum) ...
    u32 enet_away_bytes_max;  ///< ... the same in bytes (a maximum: ring bound by bytes or by datagrams) ...
    u32 enet_away_dup_max;    ///< ... and the most copies the room sent again among them (a maximum)
    u32 enet_waiting_max;     ///< the most data that waited in ENet, ready or behind a hole (bytes; a maximum)
    u32 enet_lag_max_ms;      ///< the latest a packet of the room reached the game's code: its datagram's delay against the best one of the connection, plus its wait in ENet (ms; a maximum)
    u32 rx_same_round;        ///< packets of the room handed over in the round of another one (the hand-over burst of the dense-broadcast titles) ...
    u32 rx_round_max;         ///< ... and the most one round handed over
    u32 tx_budget_rounds;     ///< (2026-11-15) rounds that sent the title's send budget and left the rest of the game's queue for the next one
    u32 tx_drain_rounds;      ///< (2026-11-15) rounds of a title with a send budget that held the game's queue back to read what the ring held after an absence
    u32 tx_same_dropped;      ///< broadcast frames of the game not sent because they were the frame last sent, a moment before (the same-frame filter of those titles)
    u32 loop_iters;    ///< iterations of the network loop
    u32 loop_gap_max;  ///< longest pause between two iterations (ms): thread starvation
    u32 delay_max;     ///< longest time a frame waited before being handed to ENet (ms)
    u32 delay_sum;
    u32 delay_n;
    u32 service_max;  ///< longest enet_host_service + send round (ms)
    u32 service_errors; ///< enet_host_service() calls that failed (socket errors)

    // UDS engine (frames that arrived but were not delivered to the game, and why).
    u32 data_delivered;
    u32 drop_not_connected;
    u32 drop_echo;
    u32 drop_not_for_me;
    u32 drop_management;
    u32 drop_no_bind; ///< nobody bound that data channel
    u32 drop_src_filter;
    u32 drop_bad_size;     ///< frames without data or over 1482 bytes: nwm does not queue them
    u32 drop_foreign;      ///< frames of another network in the same room (their transmitter is not ours) that were refused
    u32 foreign_shadow;    ///< ... and those a client let through because its foreign-lobby filter only counts (yet)
    u32 deauth_late;       ///< host: Deauthentications swallowed as the late second copy of a goodbye (a node that joined again)
    u32 auth_other_game;   ///< host of a title that keeps to its game: Authentications not answered, the newcomer plays another game (azl/one_game_host.hpp)
    u32 ring_overwritten;  ///< frames a bind's ring pushed out to make room for newer ones (the game reads too slowly)
    u32 ring_peak_units;   ///< the most 32-byte units any ring held at once
    u32 ring_flushed;      ///< frames of the last session still queued when the next one began: dropped
    u32 binds_signalled;   ///< bind events signalled by the end of a session (the binds themselves stay, like nwm's)
    u32 forwarded;         ///< host routing a client's frame to the others
    u32 fwd_candidates;    ///< frames that came through the host and could have a twin that reached us directly
    u32 fwd_unmatched;     ///< ... of which none had: delivered (the only copy there is)
    u32 drop_fwd_dup;      ///< ... of which the direct twin was delivered already: dropped
    u32 drop_dup_copy;     ///< the copy a radio_like=2 sender adds to a unicast frame (the same sequence number seen lately): dropped
    u32 fwd_evictions;     ///< twins waiting for their copy that the table had to forget (more senders than slots)
    u32 host_beacons;        ///< beacons of the host we are a client of
    u32 host_beacon_age_max; ///< longest time (ms) since the last of them: a silent host
    u32 scans;               ///< scans the game asked for
    u32 scan_entries;        ///< networks they reported
    u32 scan_id_differs;     ///< ... of which have another wlancommid / id8 than the scan asked for (not enforced)
    u32 scan_odd_input;      ///< scans with an input the firmware would call invalid (type, mode, dwell, SSID length)
    u32 sync_checks;         ///< host beacons compared with the node set the Logoff frames built (shadow)
    u32 sync_disagree;       ///< ... that listed another node set
    u32 sync_streak_max;     ///< most beacons in a row that disagreed: more than 1 or 2 is a real difference
    u32 deauth_rx[3];        ///< Deauthentications acted on, by the role that got them: host, client/spectator, connecting
    u32 node_removed[5];     ///< nodes removed from a network, by cause: deauth, authenticated again, ejected, left the room, link lost
    u32 connect_result[3];   ///< ConnectToNetwork outcomes: success, 0xC92113FE (host silent), 0xC92113FB (cut short)
    u32 transport_down[3];   ///< the room link ended under a session: a client/spectator dropped, a Connect aborted, a host lost its nodes
    u32 presence_host_gone;  ///< ... and the host's member left the room (after the debounce): the same, by presence
    u32 net_stalls;          ///< pauses of the network thread of 2 s or more (the room gives up on a peer that is silent so long)
    u32 loss_deferred;       ///< link losses that followed such a pause: the game's session waited for the room
    u32 loss_resumed;        ///< ... of which the room took the member back in time: the session went on
    u32 loss_declared;       ///< link losses that ended the game's session (at once, or when the wait for the room ran out)
    u32 pull_calls;
    u32 pull_empty;
    u32 pull_too_large;
    u32 sendto_calls;
    u32 sendto_errors;
    u32 sendto_bytes;
    u32 sendto_addressed;     ///< ... of which sent with the broadcast flag to a node the room has: addressed to its MAC, so that the room delivers it to that member alone
    u32 beacon_built;         ///< host: beacons generated (SHA-1 + MD5 + AES in software): one per change of the network (UdsEngine::BeaconFrameLocked)
    u32 beacon_reused;        ///< host: beacons sent from the copy of the last one (nothing in the network had changed)

    // Where the plugin's time goes (the 64-bit sums are at the end of the struct).
    u32 net_slow_rounds;   ///< network rounds longer than 50 ms (the pause after a socket error not counted: ClassifyRound)
    u32 net_pause_rounds;  ///< (2m-diag D1) ... and the rounds longer than 50 ms only by that pause: the loop waiting, not a slow round
    u32 service_max_ticks; ///< the slowest single enet_host_service() call
    u32 frames;            ///< frame callbacks
    u32 frame_gap_max;     ///< the longest interval between two frame callbacks (ticks)
    u32 frame_hitches;     ///< intervals longer than 50 ms
    u32 writer_wakes;
    u32 writer_late_sum;   ///< how much later than asked (ms) the lowest-priority writer woke up: it only runs
    u32 writer_late_max;   ///< when core 0 has nothing else to do, so this measures how busy the core is
    u32 lock_contended;    ///< UdsEngine's mutex found taken (game threads and network thread share it) ...
    u32 lock_wait_max;     ///< ... and the longest wait (ticks)
    u32 lock_holds;        ///< times the engine's lock was taken (every entry point, the network thread's tick when due)
    u32 lock_hold_max;     ///< the longest it was held (ticks; measured only with the log on) ...
    u32 lock_hold_max_net; ///< ... 1 when the network thread held it then, 0 a game thread
    u32 polls_now;         ///< network rounds that did not wait in the socket (a frame of the game was already queued)
    u32 polls_quick;       ///< ... that waited 1 ms instead of 3 (within 10 ms of the game queuing a frame)
    u32 pull_latency[8];   ///< queued -> PullPacket, ms: <1 <2 <5 <10 <20 <50 <100 and more (with the log on)
    u32 pull_latency_max;  ///< ... and the longest (ms)

    // The soc calls of the network thread, by SocKind: every one is counted and timed (the sum of their time is
    // below, with the other 64-bit sums).
    u32 soc_calls[kSocKinds];
    u32 soc_max_ticks[kSocKinds];
    u32 soc_over20[kSocKinds];    ///< calls that took more than 20 ms / more than 100 ms
    u32 soc_over100[kSocKinds];
    // The datagrams the network thread hands to the socket (2026-11-11, Fantasy Life: a client's broadcast frames above ENet's
    // fragment length never reached the room, and a send the socket refuses is taken by ENet as "0 bytes sent", in silence).
    u32 soc_send_refused;         ///< sends the socket refused (EWOULDBLOCK): the datagram is gone, ENet resends only a reliable one
    u32 soc_send_refused_big;     ///< ... of them, datagrams of kSocSendBig bytes or more
    u32 soc_send_failed;          ///< sends that failed with another error (ENet's service call returns an error)
    u32 soc_send_short;           ///< sends that took fewer bytes than the datagram held
    u32 soc_send_big;             ///< datagrams of kSocSendBig bytes or more the socket took whole
    u32 soc_send_max_bytes;       ///< the largest datagram the socket took whole
    u32 drain_dgrams_max;         ///< most datagrams (and their bytes) one enet_host_service() read from the socket:
    u32 drain_bytes_max;          ///< how much the soc ring held when the thread got to it
    u32 drain_dgrams_cur;         ///< (running totals of the call in progress)
    u32 drain_bytes_cur;
    u32 rx_datagrams;             ///< datagrams read from the room socket
    u32 gap_hist[5];              ///< gaps between two of them: 50-99, 100-149, 150-249, 250-499, 500+ ms

    // IPC front-end.
    u32 ipc_requests;
    u32 ipc_max_ticks; ///< slowest single request handled in the game's thread (system ticks)
    u32 pass_calls;    ///< IPC calls of the game to real services that went straight through the hook
    u32 hle_declined;  ///< nwm::UDS / ndm:u sessions the game was left to get from the REAL service (no event, no slot)
    u32 hle_throttled; ///< failing replies of the fake sessions that waited 0.5 ms (a game thread polling a command that fails)
    u32 chat_osd_draws;      ///< the chat over the game: the OSD callback's calls that drew (lines or keyboard; timed only with the log on) ...
    u32 chat_osd_max_ticks;  ///< ... and the longest of them (game thread, single writer: plain)

    // ---- The time sums, 64-bit, in system ticks (268.1 per us). They stay at the END of the struct: TakeStats() clears
    // everything before this point with one memset and these one by one, atomically.
    //
    // The CPU time of the plugin's own code. Each section is its wall time MINUS what the thread spent waiting inside it:
    // soc calls (a wait on another process), the engine's lock, the deliberate pauses after socket errors, the SD card.
    // A thread that is preempted inside a section is still charged for it, so a figure is an upper bound of the CPU
    // used; nothing is counted twice and no wait is counted at all.
    Ticks64 t_net_service;     ///< network thread: ENet's own code in enet_host_service() (the soc calls in it are out)
    Ticks64 t_net_events;      ///< network thread: decoding frames and handing them to the engine (lock waits out)
    Ticks64 t_net_send;        ///< network thread: the game's frames into ENet, flush, the engine's tick (the flush's soc
                               ///< calls, lock waits and error pauses out)
    Ticks64 t_cb;              ///< CTRPF frame callback (once per game frame)
    Ticks64 t_chat_osd;        ///< the chat's OSD callback on the game's thread, at the present (not in t_cb; with the log on)
    Ticks64 t_writer;          ///< log writer thread: its passes, the SD card out
    Ticks64 ipc_sum_ticks;     ///< game threads inside the UDS / ndm commands, scans and connects out (the engine-lock waits
                               ///< of those threads are taken off when the figure is made: see MakeCpuNumbers)
    // The waits and the clock they are measured against.
    Ticks64 t_net_wall;        ///< network thread: the wall time of its loop (rounds back to back)
    Ticks64 t_net_pause;       ///< network thread: pauses after a failing socket call
    Ticks64 t_writer_wait;     ///< log writer thread: the time its passes waited for the SD card
    Ticks64 lock_wait_ticks;   ///< the engine's mutex: time threads waited for it, all of them ...
    Ticks64 lock_wait_net_ticks; ///< ... and the network thread's share
    Ticks64 lock_hold_ticks;   ///< ... and how long it was held in all (with the log on)
    Ticks64 soc_ticks[kSocKinds]; ///< total time of the soc calls, by kind (a poll is the thread idle, a recv/send the soc process working)
    Ticks64 frame_gap_sum;     ///< sum of the intervals between two frame callbacks
};
// TakeStats() treats everything from t_net_service on as a time sum: nothing else may be added after it.
static_assert(sizeof(Stats) - offsetof(Stats, t_net_service) == (14 + kSocKinds) * sizeof(Ticks64),
              "the 64-bit sums are the end of Stats, and TakeStats clears each of them");

extern Stats g_stats;

/// For a counter that more than one thread writes without a common lock (the game's thread and the network thread
/// both count the frames the TX queue drops, the room link's loss is declared from either): a plain `+=` there
/// loses counts when the two writes meet, and a toast and the tests read these counts. Single-writer counters, and
/// the ones written under the engine's lock, stay plain.
inline void StatsAdd(u32& counter, u32 n = 1) {
    __atomic_fetch_add(&counter, n, __ATOMIC_RELAXED);
}

/// The same for the 64-bit time sums (ldrexd / strexd on the console: no library call).
inline void StatsAdd64(Ticks64& sum, u64 n) {
    __atomic_fetch_add(&sum, n, __ATOMIC_RELAXED);
}

/// One call of the chat's OSD callback that drew, `ticks` long (the game's thread, inside the swap's gate). Out of line (Sources/azl/log.cpp):
/// inline, its 64-bit add took 16 bytes more of the callback's frame, under the drawing's chain on the game's stack.
void StatsNoteChatOsd(u32 ticks);

/// How many CTRPF OSD::Notify calls the chat has made (Sources/ctr/chat_overlay.cpp): each wakes CTRPF's notification
/// workers, which the zombie probe suspects (the chat is the one path that leaves the game in memory, review ~10%). The
/// plugin's thread is the only writer; the forced res line and the chat's cost line read it. Debug diagnostics only.
void StatsNoteChatOsdNotify();
unsigned long StatsChatOsdNotifies();
/// The plugin's own OSD::Notify calls (room joined, warnings: main.cpp's PluginWork, through ChatNoteNotify), counted
/// apart from the chat's so a session without the chat has its own figure to compare against.
void StatsNotePluginOsdNotify();
unsigned long StatsPluginOsdNotifies();

/// A queued -> PullPacket latency (ms) into its bucket of Stats::pull_latency (called under the engine's lock).
inline void NotePullLatency(u32 ms) {
    static const u32 edges[7] = {1, 2, 5, 10, 20, 50, 100};
    u32 bucket = 7;
    for (u32 i = 0; i < 7; ++i) {
        if (ms < edges[i]) {
            bucket = i;
            break;
        }
    }
    ++g_stats.pull_latency[bucket];
    if (ms > g_stats.pull_latency_max) {
        g_stats.pull_latency_max = ms;
    }
}

/// The window that just ended: every counter of g_stats, which starts again from zero. The 64-bit sums are read and
/// cleared in one atomic step each, so an add that lands meanwhile is in this window or the next, never lost or torn.
Stats TakeStats();

/// Number of distinct nwm::UDS command ids the per-command counters cover.
constexpr int kStatsCommands = 0x24;
extern u32 g_cmd_counts[kStatsCommands];

/// 50 ms in system ticks: a game frame / network round longer than this is a hitch.
constexpr u32 kSlowTicks = 50 * AZL_TICKS_PER_MS;

/// (2m-diag D1, 2026-11-21) What a network round longer than kSlowTicks was. The pause after a failing socket call
/// (RoomClient::ServiceErrorPauseMs, up to 128 ms) is the loop waiting on purpose: Tetris with eight, console 63, lid closed and the WiFi
/// gone, made 2.369 lines "slow round" of that pause alone. A round is Slow when it is longer than kSlowTicks WITHOUT the pause (a round
/// with no pause: exactly as before), PauseOnly when only the pause made it so (counted in net_pause_rounds, no line), Normal otherwise.
enum class RoundKind { Normal, Slow, PauseOnly };
constexpr RoundKind ClassifyRound(u64 round_ticks, u64 pause_ticks) {
    return round_ticks <= kSlowTicks ? RoundKind::Normal
           : round_ticks - (pause_ticks < round_ticks ? pause_ticks : round_ticks) > kSlowTicks ? RoundKind::Slow
                                                                                                 : RoundKind::PauseOnly;
}
/// (2m-rev, 2026-11-22) ... and the pause it takes off is the one the loop ASKED for (ServiceErrorPauseMs) plus kPauseSlackMs, never
/// more: the time measured around SleepMs also holds the thread woken up late, and a round late by the scheduler is a slow round that
/// says so with its line, as before the 2m (428 ms measured for 128 asked: 300 ms of the round are not the pause).
constexpr u32 kPauseSlackMs = 10;
constexpr u64 PauseTakenOff(u64 measured_ticks, u32 asked_ms) {
    return measured_ticks < (static_cast<u64>(asked_ms) + kPauseSlackMs) * AZL_TICKS_PER_MS
               ? measured_ticks
               : (static_cast<u64>(asked_ms) + kPauseSlackMs) * AZL_TICKS_PER_MS;
}

/// (2m-diag D2, 2026-11-21) The notice "room link silent" (debug=1 only, Plugin::CheckLinkSilence): at least kSilenceMinGaps silences of
/// 150 ms or more in a window whose datagrams came at kSilenceMinDatagramsPerSecond or more, over the REAL length of the window. Until the
/// 2m the bound was 100 datagrams in the window, which is this rate in the nominal 5 s, but a window the log writer held for 17 s (the
/// SD card of console 63) passed it with a third of the rate.
/// (2m-rev, 2026-11-22) The rate is taken over the nominal window (nominal_ms) as long as the real one is not longer than that by more
/// than kSilenceWindowSlackMs, and over the real one less kSilenceWindowSlackMs beyond: the writer closes the window at its first service
/// after the 5 s (every 900-1000 ms), so an ordinary window lasts 5.0-5.9 s and its bound is the 100 datagrams of before, not 100-118.
constexpr u32 kSilenceMinDatagramsPerSecond = 20;
constexpr u32 kSilenceMinGaps = 2;
constexpr u32 kSilenceWindowSlackMs = 1000;
constexpr bool LinkSilenceWorthANotice(u32 rx_datagrams, u32 silences, u64 window_ms, u32 nominal_ms) {
    return window_ms != 0 && silences >= kSilenceMinGaps &&
           static_cast<u64>(rx_datagrams) * 1000u >=
               static_cast<u64>(kSilenceMinDatagramsPerSecond) *
                   (window_ms > static_cast<u64>(nominal_ms) + kSilenceWindowSlackMs ? window_ms - kSilenceWindowSlackMs
                                                                                    : static_cast<u64>(nominal_ms));
}
/// ... and where the notice says the silence is: with the shared page's WiFi strength at 3 (full) the radio of this console is not
/// the likely place, the room or the network behind the access point is (Tetris with eight: 6 notices, all of them with the strength at 3).
/// (2m-rev) The strength given is the LOWEST of the window (Plugin::SampleLinkState, every 50 ms), not the one read when it closes: a
/// WiFi that fell and came back to 3 inside the window is the WiFi.
constexpr const char* LinkSilenceWhere(u8 wifi_strength) {
    return wifi_strength == 3 ? "room/network" : "WiFi";
}

/// (2m-diag D3, 2026-11-21) A statistics window the log writer stretched: the writer closes the windows and it is the thread that sits in
/// the SD card (Tetris with eight: one write of 13.4 s), so a window "of 5 s" can last 17 s. Twice the nominal length or more.
/// (2m-rev) The figures per second are averages over the real length and stay right; what such a window changes are its counts
/// (frames, rounds longer than 50 ms, silences), which are those of all of it.
constexpr bool WindowStretched(u64 window_ms, u32 nominal_ms) {
    return window_ms >= 2u * static_cast<u64>(nominal_ms);
}
/// (2m-rev, 2026-11-22) ... and who stretched it, which the mark says instead of always blaming the writer: with no writer thread the
/// window is closed by the thread that calls Plugin::Tick (ServiceLog's fallback); with the writer, it is the writer when the time it
/// spent in the SD card plus its latest wake-up cover at least half of the excess over the nominal window, and something else (HOME,
/// sleep, the plugin's threads held) otherwise.
enum class StretchBy { None, LogWriter, NotTheWriter, TickCaller };
constexpr StretchBy WhoStretchedTheWindow(u64 window_ms, u32 nominal_ms, u64 card_ms, u64 writer_late_max_ms, bool writer_thread) {
    return !WindowStretched(window_ms, nominal_ms) ? StretchBy::None
           : !writer_thread                         ? StretchBy::TickCaller
           : 2u * (card_ms + writer_late_max_ms) >= window_ms - nominal_ms ? StretchBy::LogWriter
                                                                           : StretchBy::NotTheWriter;
}
constexpr const char* StretchedByText(StretchBy by) {
    return by == StretchBy::LogWriter      ? "by the log writer"
           : by == StretchBy::TickCaller   ? "by the caller of Tick (no writer thread)"
           : by == StretchBy::NotTheWriter ? "not by the log writer"
                                           : "";
}

// Directions of the frames in the debug capture (below).
// Other: frames between other nodes; Dup: the host's second copy of a frame that reached us directly, dropped
// (header only: its bytes are those of the Rx record of the direct copy); Echo: our own frame, which the room gave
// back to us (dropped). The records of Other, Dup and Echo are header only - the bytes of a frame that is not for us
// are not what a capture is read for, and they were half of its volume (PROVA 2: 19.7 of 39.9 MB of FRAME lines);
// the frames for us (Rx) and ours (Tx) keep their whole payload.
enum DebugDir : u8 { kDirTx = 0, kDirRx = 1, kDirOther = 2, kDirDup = 3, kDirEcho = 4 };
/// OR-ed into the direction given to DebugFrame(): a frame of the Download Play's network, kept whole whatever the diet says (its
/// frames are what its protocol is studied from). It is not stored: a record's dir is the DebugDir alone.
constexpr u8 kDirWhole = 0x80;
/// The bytes of a game's data frame that the capture keeps (the diet of the frames, below).
constexpr u32 kDebugFrameHead = 32;

// ---------------------------------------------------------------------------------------------
// The log ("debug=1" in the ini): EVERYTHING the protocol does. There are two levels and nothing between (what the capture
// keeps of a game's data frames and of the commands a game polls is "the diet of the capture", after the two levels):
//   0: no log of any kind. Log() and the Debug*() calls return at once (nothing is formatted, allocated or
//      locked), the capture ring below is not allocated and no file is created or written; only the
//      always-on counters (Stats, StatusNumbers) go on, because the warnings and the menu's status page
//      feed on them;
//   1: AzaharLink.log (2 files of 1 MB) with the control plane, the statistics blocks and the memory lines,
//      and AzaharLink.debug.log (rotated over several files), the capture: every UDS data frame (see
//      the diet below), UDS/ndm commands with ALL the request/reply words their IPC headers declare (SendTo,
//      failed PullPackets and every scan/status read included), every beacon (whole body: replayable),
//      the other room frames, every service the game opens, the commands of the network-relevant real
//      services and every other IPC request of the game (session handle, command, result).
// Producers (game threads, the engine, the network thread) append binary records to an in-memory ring under
// a small lock; the log writer thread drains the ring, formats it and writes it. The ring is allocated only
// when level 1 is on.
//
// The diet of the capture (1.1.4, 2026-11-13). Four players of Final Fantasy Explorers for 43 minutes: the ring stayed full, the
// card took 16 KB a second and two heavy records of three were lost - and three quarters of what was written were the bytes of
// the game's frames (540 characters a line), another sixth the PullPacket and SendTo lines. The ring is the games' memory and the
// writer gives the card up to the game on purpose, so the capture writes less:
//   - a data frame of the GAME (to this node, or its own) keeps its header - time, direction, nodes, channel, length - and its
//     first kDebugFrameHead bytes: the transport's header, the checksum and the library's sequence numbers, which is what tells
//     the protocol and a lost or repeated frame. Its line says "189B (cut): <64 hex digits>";
//   - a data frame of the Download Play's network (the fake lobby's and the real one's, wlancommid 0x2810: DebugFrame with
//     kDirWhole) is whole, as are the beacons and every management frame: they are what that protocol is studied from;
//   - a command a game polls (the heavy lane's: PullPacket, SendTo, the status and node reads, the scans) that succeeds is
//     written whole ONCE a second and kind, then counted in one line a second, as the IPC calls are (kDbgIpcAgg below):
//     "uds PullPacket bind 2: 197 bytes from node 3 x27 from 11.345 to 12.340" (a kind: the bind and the node; the bytes of the
//     last), "uds cmd 0x17 (req 00000001) to node 2 ch243 flags 0x03: 189 bytes x27 from ..." (a kind: the channel, the
//     destination, the first word and the flags - the two words of a SendTo that no other line of the capture has), and for the
//     others "uds cmd 0x0d (req 00000001) same x57 from ..." - a kind is then ALL the words of the request and the reply, so
//     a count line stands for calls whose line would have been the whole line of that second, word for word. A command
//     that fails is always whole, and so is every command of the control lane and of ndm:u.
// g_debug_whole takes the diet of the frames and of the commands away (every frame whole, every command a line: what level 1
// was until 1.1.4), for the study of a game's protocol (Tests/make_goldens.py needs it). The delivered plugin never sets it:
// a study build does (make EXTRA_CFLAGS=-DAZL_CAPTURE_WHOLE, by hand, to one console), and the tests.
//
// The ring has two lanes. A "heavy" record (a frame, a beacon, an IPC call, a PullPacket / SendTo line) may fill the ring
// up to 480 KB; a "control" record (a role change, a Deauthentication, a session opening or closing, a sleep, a state
// command like Initialize or Connect, the markers below) may use the whole 512 KB, so the last 32 KB are only theirs and a
// flood of frames can never push out what tells what the game did. A heavy record that finds no room, and any record
// whose producer could not get the ring's lock within a few microseconds (a game thread never waits for it: the log
// writer has the lowest priority and may be holding it), is dropped and counted; the next record of that lane that
// does go in is preceded by a LOST marker with the count and the interval, so a hole in the capture is always visible.
// ---------------------------------------------------------------------------------------------
// kDbgIpc / kDbgSvcReal are the two lines of the svc hook's pass-through, i.e. of EVERY IPC request of EVERY thread
// of the game. Their record holds the raw words, not the text: the game thread stores 16 / 24 bytes and the writer
// thread formats them (DebugFormat) into the same line a DebugEvent() of the old formats made, so that a game thread
// with a tiny stack never runs a formatter (see azl/line.hpp).
//   kDbgIpcAgg  the "diet": an IPC call with result 0 that the same session and command repeat more than 3 times in a second
//               is stored as a count: one line per second and kind with the last argument, how many calls and when the
//               first and the last came ("ipc h=... cmd=... arg=... -> 0x00000000 x57 from 11.345 to 12.340"). The first
//               three calls of the second, every call with a result other than 0, and every other record are whole. The
//               count line is a control record (written by the log writer thread, a handful a second at the most): it is
//               not lost with the heavy lane's records it stands for.
//   kDbgCommand a UDS / ndm command of the HLE proper, as the raw words of its request and reply (the writer thread makes
//               the line, "uds cmd 0x17 req ... -> rep ..."): the game's UDS thread no longer formats anything.
//   kDbgLost    the marker of a hole (see above).
enum DebugKind : u8 {
    kDbgFrame = 0,
    kDbgText = 1,
    kDbgBeacon = 2,
    kDbgIpc = 3,
    kDbgSvcReal = 4,
    kDbgIpcAgg = 5,
    kDbgCommand = 6,
    kDbgLost = 7
};

/// DebugRecord::pad: the lane of a record.
enum DebugLane : u8 { kLaneControl = 0, kLaneHeavy = 1 };

struct DebugRecord {
    u16 stored;  ///< bytes of payload / text that follow the header
    u8 kind;
    u8 dir;
    u32 ms;
    u16 src;
    u16 dest;
    u16 size;    ///< original payload size (may be larger than `stored`)
    u8 channel;
    u8 pad;      ///< the lane (DebugLane)
};
static_assert(sizeof(DebugRecord) == 16, "DebugRecord layout");

extern volatile int g_debug_level; ///< 0 no log at all, 1 everything (the master switch of Log() and the Debug*() calls)
extern volatile bool g_debug_whole; ///< the capture without its diet of frames and commands: a study build and the tests only (see above)
/// Allocates the ring (call once, before the producers run). Returns false if memory is short.
bool DebugInit();
/// True once DebugInit() has given the capture a ring.
bool DebugRingReady();
/// A UDS data frame. `dir` is a DebugDir, with kDirWhole for a frame of the Download Play's network; without it the record
/// keeps the first kDebugFrameHead bytes of `payload` (and the whole length in its header).
void DebugFrame(u8 dir, u16 src, u16 dest, u8 channel, const u8* payload, u32 size);
void DebugEvent(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// ---- Capture lines for code on the GAME's threads (the svc hook, Tests/policy_check.sh keeps the printf family out
// of it): no format string, no formatter, no buffer on the caller's stack. ----
/// "ipc h=0x<handle> cmd=0x<header:8> arg=0x<arg> -> 0x<result:8>": any IPC request that no service of ours answers.
void DebugIpc(u32 handle, u32 header, u32 arg, u32 result);
/// "svc REAL <service> cmd 0x<command:4> arg 0x<arg> -> 0x<result:8>": a command of a traced real service. Under the diet of
/// DebugIpc when `handle` (the session's) is given: past three calls a second that succeed, one count line a second.
void DebugSvcReal(const char* service, u32 command, u32 arg, u32 result, u32 handle = 0);
/// A text line of the capture built piece by piece (azl/line.hpp), pushed when the object goes out of scope. The text
/// sits in one static buffer, under the ring's lock, which the object holds: build nothing else (no Log, no second
/// DebugLine) while one is alive. At level 0, or without a ring, every append does nothing and nothing is locked.
class DebugLine : public LineBuf {
public:
    DebugLine();
    ~DebugLine();
    DebugLine(const DebugLine&) = delete;
    DebugLine& operator=(const DebugLine&) = delete;

private:
    bool locked_;
};
/// One UDS/ndm command: "<head> req <w1..wN> -> rep <w1..wM>" (head: "uds cmd 0x1b"), N and M being
/// the word counts (normal + translate) the request and the reply header declare. The record holds the raw words (and
/// the head: at most 90 characters); the writer thread makes the line (DebugFormat), so the caller formats nothing.
void DebugCommand(const char* head, const u32* request, u32 request_words, const u32* reply,
                  u32 reply_words);
/// DebugCommand with the head made by the writer: "uds cmd 0x<id:2>" / "ndm cmd 0x<id:2>" ...
void DebugServiceCommand(bool uds, u32 id, const u32* request, u32 request_words, const u32* reply, u32 reply_words);
/// ... or "uds PullPacket bind <bind>: <bytes> bytes from node <node>" (the line the analysis scripts read).
void DebugPullPacket(u32 bind, u32 bytes, u32 node, const u32* request, u32 request_words, const u32* reply,
                     u32 reply_words);
/// "wifi <dir> type <type> <bytes>B ch<channel> from AA:BB:CC:DD:EE:FF to AA:BB:CC:DD:EE:FF": an 802.11 frame that is not game
/// data or a beacon. Built by hand (azl/line.hpp): the engine sends its Deauthentications from the thread that ends a session.
void DebugWifiLine(const char* dir, u32 type, u32 bytes, u32 channel, const u8* transmitter, const u8* destination);
/// A beacon, whole. The record carries the transmitter MAC first and the 802.11 body after it.
void DebugBeacon(u8 dir, const u8* transmitter_mac, u8 channel, const u8* body, u32 size);
/// Takes the oldest record out of the ring: header into `rec`, up to `cap` payload bytes into `buf`.
bool DebugPop(DebugRecord* rec, u8* buf, u32 cap);
/// The time (platform::NowMs, low 32 bits) of the oldest record waiting in the ring; false when there is none.
bool DebugOldestMs(u32* ms);
/// Appends the text line of one popped record to `out` (the format AzaharLink.debug.log has):
///   "  12.345 <text>"                                          text events, commands, roles
///   "  12.345 FRAME rx    3>2 ch243 140B: <hex>"                UDS data frames (header only for other / dup / echo;
///                                                               "140B (cut): <hex>" when the record keeps the first bytes)
///   "  12.345 BEACON rx    00:1f:32:aa:bb:cc ch11 377B: <hex>"  beacons (whole 802.11 body)
///   "  12.345 LOST 52 heavy records from 11.900 to 12.300 (ring full 40, lock busy 12)"   a hole
/// `start_ms` is the time the plugin started (the lines show the time since).
void DebugFormat(const DebugRecord& rec, const u8* payload, u32 start_ms, std::string* out);
/// Records dropped (ring without room for them, or the ring's lock busy) since the last call.
u32 DebugTakeDropped();
/// How many bytes the capture ring holds now (an approximate read, with no lock: the writer uses it to decide whether a pass is due).
u32 DebugRingUsedBytes();
/// Lines of the main log that a thread gave up on because the log's lock was busy (azl/log.cpp LockLogBriefly: a thread
/// above the writer never waits for it), since the last call; the writer says the count in the log (LogFiles::Flush).
u32 LogTakeBusyDrops();
/// Writes the IPC counts that are still waiting for their second to end into the ring (the log writer, before it drains:
/// the last call of a kind may have come a moment ago and no later call is there to close the count); `final`: all of them.
void DebugFlushAggregates(bool final);
/// Bytes waiting in the ring.
u32 DebugBytesQueued();
/// The capture's own numbers, since the ring was allocated: what went in, what did not, how the diet did.
struct CaptureTotals {
    u32 records[2];     ///< records stored, by lane (control, heavy)
    u32 lost_full[2];   ///< ... dropped because the lane had no room
    u32 lost_busy[2];   ///< ... dropped because the ring's lock was busy for longer than a producer waits
    u32 markers;        ///< LOST markers written
    u32 agg_lines;      ///< IPC count lines written (the diet) ...
    u32 agg_calls;      ///< ... and the calls they stand for
    u32 ring_peak;      ///< most bytes the ring held at once
};
CaptureTotals DebugCaptureTotals();
/// Forgets everything the capture holds and counts (the ring itself stays): for the tests, which share one process.
void DebugReset();

// What the C sources (the ENet platform layer) report; they live in log.cpp so that the host build links.
/// A soc call of the network thread: `kind` is a SocKind, `ticks` its duration. Every call is timed: the network thread was
/// waiting for the soc process that long, which is not its CPU (platform::NoteNetWait).
void StatsSocCall(int kind, u64 ticks);
/// A datagram of `bytes` handed to the room socket by the network thread, and what the socket did with it (a SocSendOutcome).
enum SocSendOutcome : int { kSocSendTaken = 0, kSocSendRefused = 1, kSocSendFailed = 2, kSocSendShort = 3 };
/// A "big" datagram is a FULL one, whatever it carries: ENet fills every datagram to its MTU (ENET_HOST_DEFAULT_MTU, 1392 here) with
/// the commands that wait - the fragments of a large frame, or many small frames of one round of the network thread. So the two
/// counts "of kSocSendBig bytes or more" say that full datagrams were taken or refused, not that a large frame was sent: to tell,
/// hold them against the frames above ENet's fragment length that the capture shows going out in the same window.
constexpr u32 kSocSendBig = 1024;
void StatsSocSend(u32 bytes, int outcome);
/// A datagram of `bytes` read from the room, `gap_ticks` after the previous one (0 = the first).
void StatsRoomDatagram(u32 bytes, u64 gap_ticks);
/// End of one enet_host_service() call, which lasted `ticks`: closes the drain count and returns how many
/// datagrams the call read from the socket.
u32 StatsServiceCall(u32 ticks);
/// The SO_RCVBUF the room socket asked for (after the clamp) and the ring the soc service reports for it
/// (-1: unreadable); 0 / -1 until a socket exists.
extern int g_rcvbuf_asked;
extern int g_rcvbuf_got;

// ---------------------------------------------------------------------------------------------
// What the status page of the in-game menu shows of the last statistics window. It is made of the always-on
// counters above, so it is there at debug=0 (no log) too; at debug=1 the same numbers go to the log lines.
// ---------------------------------------------------------------------------------------------
/// Tenths of a millisecond per second of the window that `ticks` of work make.
unsigned long PerSecond10(u64 ticks, u64 window_ms);

/// Where the time of a window went, in tenths of a millisecond per second: the CPU of the plugin's own code and, apart,
/// the waits that are not CPU (the thread was blocked on something else). All from the exclusive sums of Stats.
struct CpuNumbers {
    u32 enet10 = 0;     ///< network thread: ENet's own code (protocol, packets) ...
    u32 frames10 = 0;   ///< ... decoding the room's frames and the engine's work on them ...
    u32 queue10 = 0;    ///< ... the game's frames into ENet, the flush, the engine's tick
    u32 net10 = 0;      ///< the three together
    u32 uds10 = 0;      ///< game threads inside the UDS / ndm commands (the engine-lock waits of those threads taken off)
    u32 frame10 = 0;    ///< the frame callback
    u32 chat10 = 0;     ///< the chat's OSD callback (the lines and the keyboard drawn over the game, at its present: not in the frame callback)
    u32 writer10 = 0;   ///< the log writer's passes, the SD card out: WALL time of the lowest-priority thread (what it waited for the core
                        ///< is in it: Tekken showed more than a core), so it is not in the total
    u32 plugin10 = 0;   ///< net + uds + frame + chat = the CPU of the plugin in ms per second, an upper bound (the writer is out: see above)
    // not CPU:
    u32 soc10 = 0;      ///< waiting for the soc process: recv and send calls
    u32 poll10 = 0;     ///< ... and idle in poll()
    u32 lock10 = 0;     ///< waiting for the engine's lock (all threads) ...
    u32 netlock10 = 0;  ///< ... of which the network thread
    u32 pause10 = 0;    ///< the network thread's pauses after socket errors
    u32 sd10 = 0;       ///< the log writer waiting for the SD card
    u32 wall10 = 0;     ///< the network loop's wall time (1000.0 when it ran the whole window) ...
    u32 other10 = 0;    ///< ... less its CPU and the waits it noted: rounding, the waits of the connect phase
};
/// The numbers of a window of `window_ms` made of `s`.
CpuNumbers MakeCpuNumbers(const Stats& s, u64 window_ms);

struct StatusNumbers {
    bool valid = false;          ///< a window has been measured (the first one closes a few seconds after the start)
    u32 window_ms = 0;
    u32 fps10 = 0;               ///< game frames per second (x10): the window's average ...
    u32 fps_min10 = 0;           ///< ... and the rate of its longest frame interval
    u32 plugin10 = 0;            ///< ms of CPU the plugin used per second (x10), waiting not counted: all of it ...
    u32 net10 = 0;               ///< ... the network thread (ENet, frames, queue; the soc calls are waiting, not in it) ...
    u32 uds10 = 0;               ///< ... the game's threads inside the UDS commands ...
    u32 frame10 = 0;             ///< ... the frame callback and the chat's OSD callback ...
    u32 writer10 = 0;            ///< ... and the log writer thread
    u32 loop_pause_ms = 0;       ///< longest pause between two rounds of the network loop
    u32 soc_mean_us = 0;         ///< soc calls (recv, send; not the poll wait): mean and longest ...
    u32 soc_max_us = 0;
    u32 soc_over20 = 0;          ///< ... and how many took more than 20 ms / more than 100 ms
    u32 soc_over100 = 0;
    u32 ring_peak_bytes = 0;     ///< the most any receive ring held at once
    u32 ring_lost = 0;           ///< frames a ring pushed out because the game read too slowly
    u32 tx_stale = 0;            ///< frames not sent: waited more than a second / the queue was full
    u32 tx_cap = 0;
    u32 twin_candidates = 0;     ///< frames that came through the host and could have a twin ...
    u32 twin_dropped = 0;        ///< ... of which the direct twin was delivered already: dropped
    u32 rtt_ms = 0;              ///< the room link now (from ENet)
    u32 loss_permille = 0;
    int soc_ring_kb = -1;        ///< the receive ring the soc service gave the room socket (-1: not known yet)
};

/// The numbers of one window: `s` are its counters, `dt_ms` its length, the link figures are read when it closes.
StatusNumbers MakeStatusNumbers(const Stats& s, u64 dt_ms, u32 rtt_ms, u32 loss_permille, int soc_ring_bytes);
/// The text of the status page (a few short lines, each of about 50 characters).
std::string FormatStatusNumbers(const StatusNumbers& n);

} // namespace azl
