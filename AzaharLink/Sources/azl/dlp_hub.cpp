// The hub of the fake Download Play lobby (azl/dlp_hub.hpp): which thread runs which call. Design: DESIGN-FASE3.md sez. 1.
#include "azl/dlp_hub.hpp"

namespace azl {

DlpHub::DlpHub(UdsEngine* engine, KernelOps* kernel, const LobbyConfig& cfg, const IpcResolver& resolver)
    : net_(engine), lobby_(&net_, cfg), resolver_(resolver), kernel_(kernel) {
    lobby_.SetExternalPoll(true);  // (the network work is the network thread's: Tick)
    svc_.SetLobby(&lobby_);
    event_ = kernel_->CreateEvent();  // 0 = no event: every request then fails at once (Post)
}

DlpHub::~DlpHub() {
    if (event_ != 0) {
        kernel_->CloseEvent(event_);
    }
}

void DlpHub::SetPlayerName(const u16* name) {
    platform::LockGuard lock(dlp_mutex_);
    lobby_.SetDefaultName(name);
}

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

bool DlpHub::IsInline(DlpKind kind, u32 id) {
    if (id == 0 || id > (kind == DlpKind::Fkcl ? DlpService::kFkLast : DlpService::kSvLast)) {
        return true;  // (an id the service does not have: its own answer, no network)
    }
    if (kind == DlpKind::Fkcl) {
        switch (id) {
        case DlpService::kFkGetEventDesc:
        case DlpService::kFkGetChannels:
        case DlpService::kFkGetServerInfo:
        case DlpService::kFkGetTitleInfo:
        case DlpService::kFkGetTitleInfoInOrder:
        case DlpService::kFkDeleteScanInfo:
        case DlpService::kFkGetMyStatus:
        case DlpService::kFkGetConnectingNodes:
        case DlpService::kFkGetNodeInfo:
        case DlpService::kFkGetPassphrase:
            return true;
        }
        return false;
    }
    switch (id) {
    case DlpService::kSvGetServerState:
    case DlpService::kSvGetEventDescription:
    case DlpService::kSvAcceptClient:
    case DlpService::kSvGetConnectingClients:
    case DlpService::kSvGetClientState:
    case DlpService::kSvIsChild:
    case DlpService::kSvGetDupNoticeNeed:
        return true;
    }
    return false;
}

bool DlpHub::NetAlive() const {
    if (stopped_.load(std::memory_order_relaxed)) {
        return false;
    }
    const u32 last = last_tick_ms_.load(std::memory_order_relaxed);
    return last != 0 && static_cast<u32>(platform::NowMs()) - last < kAliveMs;
}

void DlpHub::WriteFailure(u32* cmd, u32 id) {
    cmd[0] = (id << 16) | 0x40;
    cmd[1] = kResultWirelessOff;
}

bool DlpHub::AlwaysSucceeds(DlpKind kind, u32 id) {
    if (kind == DlpKind::Fkcl) {
        return id == DlpService::kFkFinalize || id == DlpService::kFkStopScan || id == DlpService::kFkStopSession;
    }
    return id == DlpService::kSvFinalize || id == DlpService::kSvEndAccepting;
}

bool DlpHub::EndsRole(DlpKind kind, u32 id) {
    if (kind == DlpKind::Fkcl) {
        return id == DlpService::kFkFinalize || id == DlpService::kFkStopSession;
    }
    return id == DlpService::kSvFinalize;
}

DlpOutcome DlpHub::Serve(DlpKind kind, u32* cmd, const u32* sbuf) {
    const u32 id = cmd[0] >> 16;
    if (IsInline(kind, id)) {
        platform::LockGuard lock(dlp_mutex_);
        inline_.fetch_add(1, std::memory_order_relaxed);
        return svc_.Dispatch(kind, cmd, sbuf, resolver_);
    }
    DlpOutcome out = DlpOutcome::Done;
    if (!Post(kOpDispatch, kind, cmd, sbuf, &out)) {
        return AnswerUnserved(kind, cmd);
    }
    return out;
}

__attribute__((noinline)) DlpOutcome DlpHub::AnswerUnserved(DlpKind kind, u32* cmd) {
    // The request never ran: cmd still holds it, only the id is read from it.
    const u32 id = cmd[0] >> 16;
    if (AlwaysSucceeds(kind, id)) {
        // A command that cannot fail: the game would throw a fatal error at a negative Finalize. The role it ends is ended by the next Tick.
        if (EndsRole(kind, id)) {
            closing_.fetch_or(kind == DlpKind::Fkcl ? kClosingFkcl : kClosingSrvr, std::memory_order_release);
            deferred_.fetch_add(1, std::memory_order_relaxed);
        }
        cmd[0] = (id << 16) | 0x40;
        cmd[1] = 0;
    } else {
        WriteFailure(cmd, id);
    }
    return DlpOutcome::Done;
}

__attribute__((noinline)) bool DlpHub::RequestInFlight() {
    if (post_mutex_.try_lock()) {
        post_mutex_.unlock();
        return false;
    }
    return true;
}

bool DlpHub::PollPending(u32* cmd) {
    platform::LockGuard lock(dlp_mutex_);
    return svc_.PollPending(cmd);
}

bool DlpHub::Post(Op op, DlpKind kind, u32* cmd, const u32* sbuf, DlpOutcome* outcome) {
    if (event_ == 0 || !NetAlive()) {
        failed_fast_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    platform::LockGuard post(post_mutex_);  // (one request at a time; no other lock is held by this thread)
    if (!NetAlive()) {  // (the requester ahead of us was cancelled, or Stop() came: nobody serves us either - not another kQueuedMs of waiting)
        failed_fast_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    slot_.op = op;
    slot_.kind = kind;
    slot_.cmd = cmd;
    slot_.sbuf = sbuf;
    slot_.ok = false;
    slot_.outcome = DlpOutcome::Done;
    kernel_->ClearEvent(event_);
    state_.store(kQueued, std::memory_order_seq_cst);
    if (stopped_.load(std::memory_order_seq_cst)) {
        // Stop() came between the check above and the store: its CAS found the slot Free and nobody is left to serve this request (a
        // pair of sequentially consistent accesses: one of the two threads sees the other's).
        u32 queued = kQueued;
        if (state_.compare_exchange_strong(queued, kFree, std::memory_order_acq_rel)) {
            failed_fast_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }  // (Stop's own CAS took it to Done: the loop below collects it)
    }
    const u64 deadline = platform::NowMs() + kQueuedMs;
    for (;;) {
        kernel_->WaitEvent(event_, static_cast<s64>(kSliceMs) * 1000000);
        const u32 state = state_.load(std::memory_order_acquire);
        if (state == kDone) {
            break;
        }
        if (state == kQueued && platform::NowMs() >= deadline) {
            u32 expected = kQueued;
            if (state_.compare_exchange_strong(expected, kFree, std::memory_order_acq_rel)) {
                cancelled_.fetch_add(1, std::memory_order_relaxed);
                return false;  // (taken back before the network thread took it: it will never run)
            }
        }  // (Running: the network thread has it and is not blocked on anything; wait for the end)
    }
    const bool ok = slot_.ok;
    *outcome = slot_.outcome;
    state_.store(kFree, std::memory_order_relaxed);
    if (ok) {
        posted_.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

void DlpHub::OnGameUdsStarting() {
    if (!active_.load(std::memory_order_relaxed) && !engine_held_.load(std::memory_order_relaxed) && !RequestInFlight()) {
        // The lobby holds nothing - unless a request is in flight (StartAccepting, StartFakeSession: post_mutex_ is its requester's, and the
        // hints above are refreshed only after it ran): then wait behind it and give back what it took.
        return;  // (the game finds a free engine as it is)
    }
    DlpOutcome unused = DlpOutcome::Done;
    if (!Post(kOpGameUds, DlpKind::Fkcl, nullptr, nullptr, &unused)) {
        RunHere(kOpGameUds, DlpKind::Fkcl);
    }
}

void DlpHub::OnSessionClosed(DlpKind kind) {
    DlpOutcome unused = DlpOutcome::Done;
    if (!Post(kOpClosed, kind, nullptr, nullptr, &unused)) {
        // No network thread took it. The end of a role is the lobby's network work - the engine's Shutdown and the frames it sends, the
        // deepest chain a game thread could run (about 400 bytes at debug 0 in a room, Tests/chain_sim) on a thread that may have few -
        // and the game waits for nothing: the next Tick of the network thread ends it. (After Stop() there is no next Tick, and no network.)
        closing_.fetch_or(kind == DlpKind::Fkcl ? kClosingFkcl : kClosingSrvr, std::memory_order_release);
        deferred_.fetch_add(1, std::memory_order_relaxed);
    }
}

// <<< game-thread code

void DlpHub::FreeEngine() {
    if (net_.engine_held()) {
        net_.HostClose();  // (the adapter's Release: the engine is shut down and free)
    }
}

void DlpHub::Refresh() {
    const bool held = net_.engine_held();
    engine_held_.store(held, std::memory_order_relaxed);
    active_.store(held || lobby_.active(), std::memory_order_relaxed);
}

void DlpHub::RunHere(Op op, DlpKind kind) {
    platform::LockGuard lock(dlp_mutex_);
    if (op == kOpGameUds) {
        lobby_.OnGameUdsStarting();
        FreeEngine();
    } else {
        svc_.OnSessionClosed(kind);
    }
    ran_here_.fetch_add(1, std::memory_order_relaxed);
    Refresh();
}

void DlpHub::RunDeferred() {
    const u32 bits = closing_.exchange(0, std::memory_order_acq_rel);
    if ((bits & kClosingFkcl) != 0) {
        svc_.OnSessionClosed(DlpKind::Fkcl);
    }
    if ((bits & kClosingSrvr) != 0) {
        svc_.OnSessionClosed(DlpKind::Srvr);
    }
}

void DlpHub::Execute() {
    switch (slot_.op) {
    case kOpDispatch:
        slot_.outcome = svc_.Dispatch(slot_.kind, slot_.cmd, slot_.sbuf, resolver_);
        break;
    case kOpGameUds:
        lobby_.OnGameUdsStarting();
        FreeEngine();
        break;
    case kOpClosed:
        svc_.OnSessionClosed(slot_.kind);
        break;
    }
    slot_.ok = true;
}

void DlpHub::Stamp() {
    const u32 now = static_cast<u32>(platform::NowMs());
    last_tick_ms_.store(now != 0 ? now : 1u, std::memory_order_relaxed);  // (never 0: 0 is "no Tick yet"; never AHEAD of the clock: NetAlive's age is unsigned)
}

void DlpHub::Tick() {
    Stamp();
    const bool queued = state_.load(std::memory_order_acquire) == kQueued;
    if (!queued && closing_.load(std::memory_order_acquire) == 0 && !active_.load(std::memory_order_relaxed)) {
        return;  // (an idle lobby costs the network thread three loads)
    }
    if (!platform::TryLockBriefly(dlp_mutex_)) {
        missed_.fetch_add(1, std::memory_order_relaxed);  // (a game thread is reading a table: next round)
        return;
    }
    lobby_.SetOnNet(true);
    if (closing_.load(std::memory_order_relaxed) != 0) {
        RunDeferred();  // (the roles whose sessions the game closed while nobody served: before any request of a new session)
    }
    u32 expected = kQueued;
    if (queued && state_.compare_exchange_strong(expected, kRunning, std::memory_order_acq_rel)) {
        Execute();
        Refresh();  // (BEFORE the answer is published: the game may ask EngineHeld() or run OnGameUdsStarting() the moment it has it)
        Stamp();    // (a request that stalled the engine for long: the requester released by the answer must find the thread alive)
        state_.store(kDone, std::memory_order_release);
        kernel_->SignalEvent(event_);
    }
    lobby_.Pump();
    lobby_.SetOnNet(false);
    Refresh();
    dlp_mutex_.unlock();
    Stamp();  // (a Tick that took long - a request that stalled the engine - must not make the next requester find the thread "dead")
}

void DlpHub::Stop() {
    stopped_.store(true, std::memory_order_seq_cst);
    u32 expected = kQueued;
    if (state_.compare_exchange_strong(expected, kDone, std::memory_order_acq_rel)) {
        kernel_->SignalEvent(event_);  // (slot_.ok is false: the requester answers the failure)
    }
}

} // namespace azl
