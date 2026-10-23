// Firmware addresses are those of the ndm module 0004013000002B02 v0x12 (firmware 02.46.01).
#include "azl/ndm_hle.hpp"

namespace azl {

namespace {

constexpr u32 Hdr(u32 id, u32 normal, u32 translate) {
    return (id << 16) | ((normal & 0x3F) << 6) | (translate & 0x3F);
}

constexpr u32 kPidDescriptor = 0x20; // IPC_Desc_CurProcessId()

inline void Reply(u32* cmd, u32 id, u32 result) {
    cmd[0] = Hdr(id, 1, 0);
    cmd[1] = result;
}

inline void ReplyValue(u32* cmd, u32 id, u32 result, u32 value) {
    cmd[0] = Hdr(id, 2, 0);
    cmd[1] = result;
    cmd[2] = result == 0 ? value : 0;
}

} // namespace

// (the game's thread opens its fake ndm:u session through the svc hook: Tests/policy_check.sh)
// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
int NdmHle::OpenSession() {
    for (int i = 0; i < kMaxSessions; ++i) {
        if (!sessions_[i].open) {
            sessions_[i] = SessionObj();
            sessions_[i].open = true;
            return i;
        }
    }
    return -1;
}
// <<< game-thread code

void NdmHle::DaemonSuspend(u32 d) {
    ++daemon_global_[d]; // (the request to the daemon itself is not modelled)
}

void NdmHle::DaemonResume(u32 d) {
    if (daemon_global_[d] > 0) { // at 0 the real one answers 0xC8A0680E, its callers ignore it
        --daemon_global_[d];
    }
}

void NdmHle::SetDefaultDaemons(u32 mask) {
    // a daemon whose bit becomes set is resumed, one whose bit becomes clear is suspended; the
    // byte is stored unmasked.
    const u32 changed = (default_mask_ ^ mask) & 0xFF;
    for (u32 d = 0; d < kDaemonCount; ++d) {
        if (changed & (1u << d)) {
            if (mask & (1u << d)) {
                DaemonResume(d);
            } else {
                DaemonSuspend(d);
            }
        }
    }
    default_mask_ = mask & 0xFF;
}

void NdmHle::Transition(u32 to) {
    if (cur_mode_ == to) {
        return;
    }
    // The exit handler of the old state ...
    switch (cur_mode_) {
    case 1:
        DaemonResume(0);
        break;
    case 2:
        for (u32 d = 0; d < kDaemonCount; ++d) {
            DaemonResume(d);
        }
        --sched_global_;
        break;
    case 3:
    case 4:
        for (u32 d = 1; d < kDaemonCount; ++d) {
            DaemonResume(d);
        }
        break;
    default:
        break;
    }
    // ... then the enter handler of the new one.
    switch (to) {
    case 1:
        DaemonSuspend(0);
        break;
    case 2:
        for (u32 d = 0; d < kDaemonCount; ++d) {
            DaemonSuspend(d);
        }
        ++sched_global_;
        break;
    case 3:
    case 4:
        for (u32 d = 1; d < kDaemonCount; ++d) {
            DaemonSuspend(d);
        }
        break;
    default:
        break; // the real service accepts modes of 5 and above as state values too
    }
    // The second stage of the real one (ac:u CloseAll for modes 2 to 4, an event to the
    // connection machine) is exactly what must not happen here.
    cur_mode_ = to;
}

u32 NdmHle::StepToTarget() {
    // The real one calls the step function until the current state is the target.
    if (cur_mode_ == 2) {
        if (target_mode_ == 0) {
            Transition(0);
            return 0;
        }
        // From local communications only "none" is reachable. A target of 5 or more would
        // make the real loop spin forever (no case for it); it gets the same answer here.
        return kResBadState;
    }
    Transition(target_mode_);
    return 0;
}

u32 NdmHle::ChangeExclusive(u32 pid, u32 mode) {
    // (Enter = mode, Leave = 0).
    if (mode != 0) {
        if (owner_pid_ != 0 && owner_pid_ == pid) {
            return kResAlreadyInState;
        }
    } else if (owner_pid_ != 0 && owner_pid_ != pid) {
        return kResNotOwner;
    }
    if (lock_count_ > 0 && lock_owner_ != pid) {
        return kResLockedByOther;
    }
    target_mode_ = mode; // the target stays changed even when the step fails
    const u32 r = StepToTarget();
    if (r != 0) {
        return r;
    }
    owner_pid_ = mode != 0 ? pid : 0;
    return 0;
}

u32 NdmHle::SuspendScheduler(SessionObj& s) {
    // The flag of the request only selects whether the real one waits for the connection
    // machine, which is not modelled.
    if (s.sched_count >= 100) {
        return kResBadState;
    }
    if (++s.sched_count == 1) {
        for (u32 d = 0; d < kDaemonCount; ++d) {
            DaemonSuspend(d);
        }
        ++sched_global_;
    }
    return 0;
}

u32 NdmHle::ResumeScheduler(SessionObj& s) {

    if (s.sched_count == 0) {
        return kResNotSuspended;
    }
    if (--s.sched_count == 0) {
        for (u32 d = 0; d < kDaemonCount; ++d) {
            DaemonResume(d);
        }
        --sched_global_;
    }
    return 0;
}

bool NdmHle::ResetOverride(SessionObj& s) {
    // only the session that owns the override can undo it.
    if (!s.override_owner) {
        return false;
    }
    s.override_owner = false;
    override_active_ = false;
    SetDefaultDaemons(override_saved_);
    return true;
}

void NdmHle::CloseSession(int si) {
    if (!session_open(si)) {
        return;
    }
    SessionObj& s = sessions_[si];
    // The daemons, in the same order.
    for (u32 d = 0; d < kDaemonCount; ++d) {
        if (s.daemon_count[d] > 0) {
            DaemonResume(d);
            s.daemon_count[d] = 0;
        }
    }
    if (lock_owner_ != 0 && lock_owner_ == s.pid) {
        lock_owner_ = 0;
        lock_count_ = 0;
    }
    if (s.sched_count > 0) { // the count is forced to 1 and resumed once
        s.sched_count = 1;
        ResumeScheduler(s);
    }
    if (s.interval_set) {
        scan_interval_ = 30;
        retry_interval_ = 10;
    }
    ResetOverride(s);
    if (s.pid != 0 && owner_pid_ == s.pid) { // leaves the state its PID owns
        ChangeExclusive(s.pid, 0);
    }
    s = SessionObj();
}

void NdmHle::Dispatch(int si, u32* cmd) {
    const u32 id = cmd[0] >> 16;
    // The real dispatcher compares the whole header word with the command's and the PID
    // descriptor with 0x20, for the commands 1, 2, 4, 5 and 0x18 only.
    auto count_if_wrong = [&](bool ok) {
        if (!ok) {
            ++header_anomalies_;
        }
    };
    if (!session_open(si)) {
        Reply(cmd, 0, kResBadHeader); // (the session was closed under this call: nothing to serve)
        return;
    }
    SessionObj& s = sessions_[si];

    switch (id) {
    case 0x01: { // EnterExclusiveState(mode) [0x00010042: mode, PID descriptor, PID]
        count_if_wrong(cmd[0] == Hdr(1, 1, 2) && cmd[2] == kPidDescriptor);
        const u32 pid = PidFrom(cmd[3]);
        s.pid = pid; // The real one stores the PID before it looks at the mode
        // A mode word of 0 is refused, a mode of 0x100 passes as 0 = Leave (only the low byte goes on).
        Reply(cmd, id, cmd[1] == 0 ? kResInvalidArgument : ChangeExclusive(pid, cmd[1] & 0xFF));
        return;
    }
    case 0x02: { // LeaveExclusiveState [0x00020002: PID descriptor, PID]
        count_if_wrong(cmd[0] == Hdr(2, 0, 2) && cmd[1] == kPidDescriptor);
        const u32 pid = PidFrom(cmd[2]);
        s.pid = pid;
        Reply(cmd, id, ChangeExclusive(pid, 0));
        return;
    }
    case 0x03: // QueryExclusiveMode
        ReplyValue(cmd, id, 0, cur_mode_);
        return;
    case 0x04: { // LockState [0x00040002]
        count_if_wrong(cmd[0] == Hdr(4, 0, 2) && cmd[1] == kPidDescriptor);
        const u32 pid = PidFrom(cmd[2]);
        s.pid = pid;
        u32 r = 0;
        if (lock_count_ > 0 && lock_owner_ != pid) {
            r = kResLockedByOther;
        } else {
            lock_owner_ = pid;
            ++lock_count_;
        }
        Reply(cmd, id, r);
        return;
    }
    case 0x05: { // UnlockState [0x00050002]
        count_if_wrong(cmd[0] == Hdr(5, 0, 2) && cmd[1] == kPidDescriptor);
        const u32 pid = PidFrom(cmd[2]);
        s.pid = pid;
        u32 r = 0;
        if (lock_count_ == 0) {
            r = kResNotLocked;
        } else if (lock_owner_ != pid) {
            r = kResLockedByOther;
        } else if (--lock_count_ == 0) {
            lock_owner_ = 0;
        }
        Reply(cmd, id, r);
        return;
    }
    case 0x06:   // SuspendDaemons(mask)
    case 0x07: { // ResumeDaemons(mask)
        const u32 mask = cmd[1];
        u32 r = 0;
        if (mask == 0 || mask >= 16) {
            r = kResInvalidArgument;
        } else if (id == 0x06) {
            // nothing changes unless every selected daemon is below the cap of 100
            for (u32 d = 0; d < kDaemonCount && r == 0; ++d) {
                if ((mask & (1u << d)) && s.daemon_count[d] >= 100) {
                    r = kResBadState;
                }
            }
            for (u32 d = 0; d < kDaemonCount && r == 0; ++d) {
                if ((mask & (1u << d)) && ++s.daemon_count[d] == 1) {
                    DaemonSuspend(d); // the global count counts sessions, not calls
                }
            }
        } else {
            // nothing changes unless every selected daemon is suspended by this session
            for (u32 d = 0; d < kDaemonCount && r == 0; ++d) {
                if ((mask & (1u << d)) && s.daemon_count[d] == 0) {
                    r = kResNotSuspended;
                }
            }
            for (u32 d = 0; d < kDaemonCount && r == 0; ++d) {
                if ((mask & (1u << d)) && --s.daemon_count[d] == 0) {
                    DaemonResume(d);
                }
            }
        }
        Reply(cmd, id, r);
        return;
    }
    case 0x08: // SuspendScheduler(flag)
        Reply(cmd, id, SuspendScheduler(s));
        return;
    case 0x09: // ResumeScheduler
        Reply(cmd, id, ResumeScheduler(s));
        return;
    case 0x0A: // GetCurrentState
    case 0x0B: // GetTargetState
        // UNVERIFIED: both are plain reads of the real connection machine, whose
        // state numbers were not decoded (nothing says what it reports after an Enter, and FFE never
        // asks). Kept as they always were: success and 0, which is what Azahar answers.
        ReplyValue(cmd, id, 0, 0);
        return;
    case 0x0C: // never implemented: an error and a null copy handle
        cmd[0] = Hdr(id, 1, 2);
        cmd[1] = kResNotImplemented;
        cmd[2] = 0;
        cmd[3] = 0;
        return;
    case 0x0D: { // QueryStatus(daemon): the low byte only (ldrb)
        const u32 d = cmd[1] & 0xFF;
        if (d >= kDaemonCount) {
            ReplyValue(cmd, id, kResInvalidArgument, 0);
            return;
        }
        ReplyValue(cmd, id, 0, daemon_global_[d] > 0 ? 3u : 1u); // Suspended / Idle
        return;
    }
    case 0x0E: { // GetDaemonDisableCount(daemon) -> [this session's count, the global count]
        const u32 d = cmd[1] & 0xFF;
        cmd[0] = Hdr(id, 3, 0);
        if (d >= kDaemonCount) {
            cmd[1] = kResInvalidArgument;
            cmd[2] = 0;
            cmd[3] = 0;
            return;
        }
        cmd[1] = 0;
        cmd[2] = s.daemon_count[d];
        cmd[3] = daemon_global_[d];
        return;
    }
    case 0x0F: // GetSchedulerDisableCount -> [this session's count, the global count]
        cmd[0] = Hdr(id, 3, 0);
        cmd[1] = 0;
        cmd[2] = s.sched_count;
        cmd[3] = static_cast<u32>(sched_global_);
        return;
    case 0x10: // SetScanInterval(seconds): no validation
        s.interval_set = true;
        scan_interval_ = cmd[1];
        Reply(cmd, id, 0);
        return;
    case 0x11:
        ReplyValue(cmd, id, 0, scan_interval_);
        return;
    case 0x12: // SetRetryInterval(seconds)
        s.interval_set = true;
        retry_interval_ = cmd[1];
        Reply(cmd, id, 0);
        return;
    case 0x13:
        ReplyValue(cmd, id, 0, retry_interval_);
        return;
    case 0x14: { // OverrideDefaultDaemons(mask): one owner at a time, no range check, the low byte is kept
        u32 r = 0;
        if (override_active_ && !s.override_owner) {
            r = kResOverrideTaken;
        } else {
            if (!override_active_) {
                override_saved_ = default_mask_;
                override_active_ = true;
            }
            s.override_owner = true;
            SetDefaultDaemons(cmd[1] & 0xFF);
        }
        Reply(cmd, id, r);
        return;
    }
    case 0x15: // ResetDefaultDaemons
        Reply(cmd, id, ResetOverride(s) ? 0 : kResBadState);
        return;
    case 0x16: // GetDefaultDaemons
        ReplyValue(cmd, id, 0, default_mask_);
        return;
    case 0x17: // ClearHalfAwakeMacFilter: only touches the Cec daemon's filter list
        Reply(cmd, id, 0);
        return;
    case 0x18: // not in libctru: [0x00180004: PID descriptor, PID, copy-handle descriptor 0, event]
        count_if_wrong(cmd[0] == Hdr(0x18, 0, 4) && cmd[1] == kPidDescriptor && cmd[3] == 0);
        // no handle is refused. The real one keeps the event and signals it at the end of every
        // scheduler cycle; there is no scheduler here, so the event is never signalled.
        Reply(cmd, id, cmd[4] == 0 ? kResOverrideTaken : 0);
        return;
    default:
        // The real switch sends every other id to the default answer: header 0x40 and 0xD900182F.
        // Only the firmware 02.46.01 module is known: an id a newer ndm might add must not break a game,
        // so it keeps getting Azahar's "success" until the New 3DS ndm has been looked at.
        Reply(cmd, id, 0);
        return;
    }
}

} // namespace azl
