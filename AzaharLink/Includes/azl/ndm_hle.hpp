// Emulation of the "ndm:u" service, session by session like the real module.
//
// Games ask ndm:u to enter the "local communications" exclusive state before using UDS. On a real
// console that closes every ac:u client and drops the infrastructure (internet) connection, which
// would also cut our link to the room, so the requests are answered here and never reach the real
// service: the console keeps its WiFi connection, the game sees what it would see on a console.
//
// Modelled on the behaviour of the ndm module of firmware 02.46.01; the New 3DS build of ndm was not available, its command set is assumed to be the same.
//
// What it models:
//  * the process-wide state of the real ndm: the exclusive state machine with its owner PID and state
//    lock, the per-daemon suspend counts, the default-daemon mask with its single-owner override, the
//    scheduler suspend count and the scan/retry intervals;
//  * one object per ndm:u session (the real service has 6): the last PID it saw, its own suspend counts,
//    whether it changed the intervals or owns the override. Closing the session releases all of it, like
//    the real service does when a process closes its handle or dies;
//  * the request validation and the result codes of the 24 documented commands and of the undocumented 0x18.
// What it deliberately does not do: anything that reaches the real ndm / ac / nwm.
#pragma once

#include "azl/common.hpp"

namespace azl {

class NdmHle {
public:
    /// The real service registers ndm:u with 6 sessions.
    static constexpr int kMaxSessions = 6;
    static constexpr u32 kDaemonCount = 4; // Cec, Boss, Nim, Friend

    // Result codes: level Status / summary InvalidState / module 26 (ndm) unless noted.
    static constexpr u32 kResInvalidArgument = 0xD8E06BED;  // Usage, InvalidArgument, ndm, 1005
    static constexpr u32 kResLockedByOther = 0xC8A0680A;    // desc 10
    static constexpr u32 kResNotLocked = 0xC8A0680B;        // desc 11
    static constexpr u32 kResNotSuspended = 0xC8A0680D;     // desc 13
    static constexpr u32 kResBadState = 0xC8A0680E;         // desc 14 (cap of 100, bad transition, not the override's owner)
    static constexpr u32 kResOverrideTaken = 0xC8A06809;    // desc 9
    static constexpr u32 kResNotOwner = 0xC8A06810;         // desc 16 (Leave by a PID that is not the owner)
    static constexpr u32 kResAlreadyInState = 0xC8A06811;   // desc 17 (Enter by the owner)
    static constexpr u32 kResNotImplemented = 0xE0C06BF4;   // Usage, NotSupported, ndm, 1012 (command 0x0C)
    static constexpr u32 kResBadHeader = 0xD9001830;        // Usage, WrongArgument, os, 48 (the dispatcher's own answer)

    /// The process the sessions belong to; 0 is kept out of the way because the real state uses it for
    /// "nobody". A request carries its sender's PID in the word after a "current process id" descriptor
    /// (0x20) and the KERNEL fills that word in. The plugin serves the request before the kernel does, so
    /// there the word is 0 or whatever the buffer held: `request_pids_are_real` = false makes every
    /// request the one process' own and the word is never read. With true (a caller that sits behind a
    /// kernel) a non-zero word is the PID and 0 stands for this process.
    void SetSelfPid(u32 pid, bool request_pids_are_real = true) {
        self_pid_ = pid ? pid : 1;
        request_pids_are_real_ = request_pids_are_real;
    }

    /// Requests whose header (commands 1, 2, 4, 5, 0x18) or PID descriptor is not what the 02.46.01
    /// module accepts. The real service refuses them with [0x40, 0xD9001830]; a title built for a newer
    /// firmware might legitimately send a variant, so they are only counted and parsed with the
    /// standard layout.
    u32 header_anomalies() const {
        return header_anomalies_;
    }

    /// Opens a session; -1 when the real service would refuse (all 6 slots in use).
    int OpenSession();
    /// Releases everything the session still holds (the real module's session close handler).
    void CloseSession(int session);

    /// Handles one request of `session`. `cmd` is the IPC command buffer, the reply replaces it.
    /// No locking: the caller serialises the calls.
    void Dispatch(int session, u32* cmd);

    // Read access for diagnostics and tests.
    u32 exclusive_state() const {
        return cur_mode_;
    }
    u32 exclusive_owner() const {
        return owner_pid_;
    }
    u32 daemon_global_count(u32 daemon) const {
        return daemon < kDaemonCount ? daemon_global_[daemon] : 0;
    }
    int scheduler_global_count() const {
        return sched_global_;
    }
    u32 default_daemons() const {
        return default_mask_;
    }
    u32 scan_interval() const {
        return scan_interval_;
    }
    u32 retry_interval() const {
        return retry_interval_;
    }
    bool session_open(int session) const {
        return session >= 0 && session < kMaxSessions && sessions_[session].open;
    }

private:
    struct SessionObj {
        bool open = false;
        u32 pid = 0;                         // the last PID passed by Enter / Leave / Lock / Unlock
        u32 daemon_count[kDaemonCount] = {}; // this session's suspends of each daemon
        u32 sched_count = 0;                 // this session's scheduler suspends
        bool interval_set = false;           // this session changed the scan or retry interval
        bool override_owner = false;         // this session owns the default-daemon override
    };

    u32 PidFrom(u32 word) const {
        return request_pids_are_real_ && word ? word : self_pid_;
    }
    void DaemonSuspend(u32 daemon);
    void DaemonResume(u32 daemon);
    void SetDefaultDaemons(u32 mask);
    u32 ChangeExclusive(u32 pid, u32 mode); // mode 0 = leave
    u32 StepToTarget();
    void Transition(u32 to);
    u32 SuspendScheduler(SessionObj& s);
    u32 ResumeScheduler(SessionObj& s);
    bool ResetOverride(SessionObj& s);

    u32 self_pid_ = 1;
    bool request_pids_are_real_ = true;
    u32 header_anomalies_ = 0;
    SessionObj sessions_[kMaxSessions];

    // The process-wide state of the real ndm.
    u32 cur_mode_ = 0;     // 0 none, 1 infrastructure, 2 local communications, 3 StreetPass, 4 StreetPass data
    u32 target_mode_ = 0;
    u32 owner_pid_ = 0;    // who entered the state
    u32 lock_owner_ = 0;   // who holds the state lock
    u32 lock_count_ = 0;
    // The real constructor starts with the default mask 0x0F and then applies 0x09: Boss and Nim begin
    // suspended once. A count is "sessions that suspend it" plus what the exclusive state and the
    // default mask add.
    u32 daemon_global_[kDaemonCount] = {0, 1, 1, 0};
    u32 default_mask_ = 0x09; // Cec | Friend
    bool override_active_ = false;
    u32 override_saved_ = 0x09;
    int sched_global_ = 0;
    u32 scan_interval_ = 30;
    u32 retry_interval_ = 10;
};

} // namespace azl
