// (DPAPP-2j, R-A) zombie_free.hpp. What it opens: each process of the list, as zombie_census.cpp does (svcOpenProcess, its title id and name, its
// state and its threads - type 7, never svcGetThreadList, which keeps a reference of the process for ever), and pm:app, non-blocking, for the
// command 12 alone, closed right after. The handles of the closed games of the key stay open while pm is asked (the object cannot go away under the
// look), and are closed right after: the kernel then frees the process when no other reference is left.
#include <3ds.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "dlp_guest_store.hpp"
#include "zombie_census.hpp"
#include "zombie_free.hpp"
#include "zombie_plan.hpp"

namespace zombie_free {
namespace {

constexpr int kMaxPids = 64;
constexpr u32 kRecheckStepMs = 50;
constexpr u32 kRecheckMaxMs = 500;

u64 NowMs() {
    return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

bool OnEmulator() {
    s64 v = 0;
    svcGetSystemInfo(&v, 0x20000, 0); // (DPAPP-2k: 0 on a console's Luma, 1 on Citra, 2 on Azahar - never its Result: Luma's is 1, positive)
    return v != 0;
}

bool ListPids(std::vector<u32>& out) {
    s32 n = 0;
    u32 pids[kMaxPids] = {};
    if (R_FAILED(svcGetProcessList(&n, pids, kMaxPids)) || n < 0 || n >= kMaxPids) {
        return false; // (a full buffer may have left a process out: not read)
    }
    out.assign(pids, pids + n);
    return true;
}

struct Seen {
    zombie_plan::ProcFact fact;
    char name[9] = {};
    Handle h = 0; // (open only for a process of the key)
    long long refs_others = -1;
    long long refs_after = -1; // (DPAPP-2j-rev) the references besides ours once pm was asked, our handle still open
};

// Every process but ours: its id, and for those of `key` (pm's comparison) its state, threads and references, with its handle kept open. false: the
// list could not be read whole, or a process still listed could not be opened (nothing is decided on half a list). (DPAPP-2j-rev) A process that
// could not be opened and is gone from the list read again left in between: the list stays whole.
bool ReadKey(u64 key, u32 self, std::vector<Seen>& out) {
    std::vector<u32> pids;
    if (!ListPids(pids)) {
        return false;
    }
    bool whole = true;
    std::vector<u32> unopened;
    for (u32 pid : pids) {
        if (pid == self) {
            continue;
        }
        Handle h = 0;
        if (R_FAILED(svcOpenProcess(&h, pid))) {
            unopened.push_back(pid); // (a process that left between the list and the open is fine: the list read again below says which)
            continue;
        }
        Seen s;
        s.fact.pid = pid;
        s64 v = 0;
        if (R_SUCCEEDED(svcGetProcessInfo(&v, h, 0x10001))) { // (Luma: the code set's title id - the ACI's, the one pm registered)
            s.fact.tid = static_cast<u64>(v);
        } else {
            whole = false;
        }
        if (!zombie_plan::SameKeyAsPm(s.fact.tid, key)) {
            svcCloseHandle(h);
            continue;
        }
        v = 0;
        if (R_SUCCEEDED(svcGetProcessInfo(&v, h, 0x10000))) {
            std::memcpy(s.name, &v, 8);
            s.name[8] = '\0';
        }
        s.fact.signaled = zombie_plan::WaitState(static_cast<uint32_t>(svcWaitSynchronization(h, 0))); // (== 0 exactly: the timeout is positive)
        v = -1;
        s.fact.threads = R_SUCCEEDED(svcGetProcessInfo(&v, h, 7)) ? static_cast<int>(v) : -1; // (type 7: balanced, unlike GetThreadList)
        s64 refs = -1;
        s.refs_others = R_SUCCEEDED(svcGetHandleInfo(&refs, h, 1)) ? zombie_plan::OthersFromHandleInfo(refs) : -1;
        s.h = h;
        out.push_back(s);
    }
    if (!unopened.empty()) {
        std::vector<u32> again;
        whole = whole && ListPids(again) && zombie_plan::NoneListed(again, unopened); // (DPAPP-2j-rev: one still listed is not known)
    }
    return whole;
}

void CloseAll(std::vector<Seen>& seen) {
    for (Seen& s : seen) {
        if (s.h != 0) {
            svcCloseHandle(s.h);
            s.h = 0;
        }
    }
}

// The APPLICATION limit's commit (KiB), 3DS Link's own included: what a freed closed game gives back shows here.
long long CommitKiB() {
    Handle rl = 0;
    if (R_FAILED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) {
        return -1;
    }
    ResourceLimitType type = RESLIMIT_COMMIT;
    s64 cur = -1;
    svcGetResourceLimitCurrentValues(&cur, rl, &type, 1);
    svcCloseHandle(rl);
    return cur < 0 ? -1 : static_cast<long long>(cur / 1024);
}

// pm:app, non-blocking: a port with every session taken answers 0xD0401834 at once instead of waiting (sm/source/services.c:186-190).
Result OpenPmApp(Handle* pm) {
    *pm = 0;
    srvSetBlockingPolicy(true);
    const Result rc = srvGetServiceHandleDirect(pm, "pm:app");
    srvSetBlockingPolicy(false);
    return rc;
}

// pm:app command 12 (pmapp.c:108-112): UnregisterProcess(key). The only command this file sends.
Result Unregister(Handle pm, u64 key, u32* answer) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = zombie_plan::kPmAppUnregisterHeader;
    cmdbuf[1] = static_cast<u32>(key);
    cmdbuf[2] = static_cast<u32>(key >> 32);
    const Result rc = svcSendSyncRequest(pm);
    *answer = R_SUCCEEDED(rc) ? cmdbuf[1] : 0xFFFFFFFFu;
    return rc;
}

std::string Stamped(const std::string& line, u64 since_ms) {
    return line + " [" + std::to_string(static_cast<unsigned long long>(NowMs() - since_ms)) + " ms since the look began]";
}

// One key: look, judge, ask pm, look again. Lines appended to `lines`.
void FreeKey(u64 key, u32 self, bool emulator, std::vector<std::string>& lines) {
    const u64 t0 = NowMs();
    std::vector<Seen> seen;
    const bool whole = emulator ? true : ReadKey(key, self, seen);
    std::vector<zombie_plan::ProcFact> facts;
    for (const Seen& s : seen) {
        facts.push_back(s.fact);
    }
    int calls = 0;
    zombie_plan::FreeVerdict v = zombie_plan::Judge(facts, self, key, dlpguest::IsOwnedGuestTid(key), emulator, &calls);
    if (!whole && v == zombie_plan::FreeVerdict::Free) {
        v = zombie_plan::FreeVerdict::Unsure; // (a process not read may be a live one of the key: never asked)
        calls = 0;
    }
    if (v == zombie_plan::FreeVerdict::Nothing || v == zombie_plan::FreeVerdict::Emulator) {
        CloseAll(seen);
        return; // (nothing of the key in memory: no line - the census says the rest)
    }
    lines.push_back(zombie_plan::FreeLookLine(key, v, static_cast<int>(seen.size()), static_cast<u32>(NowMs() - t0)) +
                    (whole ? "" : " (the list of the processes not read whole)"));
    if (v != zombie_plan::FreeVerdict::Free) {
        CloseAll(seen);
        return;
    }
    const long long before = CommitKiB();
    Handle pm = 0;
    const Result open_rc = OpenPmApp(&pm);
    if (R_FAILED(open_rc)) {
        char line[120];
        std::snprintf(line, sizeof(line), "free: pm:app not opened rc=0x%08lX (its sessions taken?): nothing done", static_cast<unsigned long>(open_rc));
        lines.push_back(Stamped(line, t0));
        CloseAll(seen);
        return;
    }
    int asked = 0;
    for (const Seen& s : seen) {
        if (asked >= calls) {
            break;
        }
        const u64 c0 = NowMs();
        u32 answer = 0;
        const Result rc = Unregister(pm, key, &answer);
        ++asked;
        lines.push_back(zombie_plan::FreeCallLine(s.fact.pid, s.name, key, dlpguest::AmTitleOf(key), static_cast<u32>(rc), answer,
                                                  static_cast<u32>(NowMs() - c0)));
    }
    // (DPAPP-2j-rev) the references once pm was asked, our handles still open: pm closes its own before it answers (manager.c:86-87). None fell:
    // no reference of pm's was there, the process cannot leave when ours go - one look at the list, no sleep on the screen's thread
    bool wait = false;
    for (Seen& s : seen) {
        s64 now = -1;
        s.refs_after = s.h != 0 && R_SUCCEEDED(svcGetHandleInfo(&now, s.h, 1)) ? zombie_plan::OthersFromHandleInfo(now) : -1;
        wait = wait || zombie_plan::WaitAfterUnregister(s.refs_others, s.refs_after);
    }
    svcCloseHandle(pm); // (never a session kept: pm:app has 4, Rosalina keeps one)
    CloseAll(seen);    // (our references gone: the kernel frees a process nobody else holds)
    // The list again, every 50 ms for 500 ms at most: a pid gone left memory
    std::vector<u32> left;
    for (const Seen& s : seen) {
        left.push_back(s.fact.pid);
    }
    const u64 r0 = NowMs();
    for (;;) {
        std::vector<u32> pids;
        if (ListPids(pids)) {
            std::vector<u32> still;
            for (u32 pid : left) {
                if (zombie_plan::Listed(pids, pid)) {
                    still.push_back(pid);
                } else {
                    lines.push_back(zombie_plan::FreeAfterLine(pid, true, static_cast<u32>(NowMs() - r0), 0));
                }
            }
            left.swap(still);
        }
        if (left.empty() || !wait || NowMs() - r0 >= kRecheckMaxMs) { // (DPAPP-2j-rev: no reference given back - one look only)
            break;
        }
        svcSleepThread(static_cast<s64>(kRecheckStepMs) * 1000000LL);
    }
    for (u32 pid : left) {
        long long refs = -1;
        for (const Seen& s : seen) {
            if (s.fact.pid == pid) {
                refs = s.refs_after >= 0 ? s.refs_after : s.refs_others - 1; // (as read once pm was asked; else pm's was one of them, given back)
            }
        }
        lines.push_back(zombie_plan::FreeAfterLine(pid, false, static_cast<u32>(NowMs() - r0), refs));
    }
    const long long after = CommitKiB();
    char line[120];
    std::snprintf(line, sizeof(line), "free: APPLICATION commit before %lld KiB, after %lld KiB", before, after);
    lines.push_back(Stamped(line, t0));
}

} // namespace

void FreeAtStart(const std::string& datadir) {
    static bool done = false; // (one look per start)
    if (done) {
        return;
    }
    done = true;
    if (envIsHomebrew()) {
        return; // (only the installed app is surely THE running application: pm then lets no other start, launch.c:299)
    }
    const bool emulator = OnEmulator();
    u32 self = 0;
    svcGetProcessId(&self, CUR_PROCESS_HANDLE);
    std::vector<std::string> lines;
    for (const dlpguest::Guest& g : dlpguest::LoadGuests()) {
        if (dlpguest::IsOwnedGuestTid(g.prepared)) {
            FreeKey(g.prepared, self, emulator, lines);
        }
    }
    if (!lines.empty()) {
        zombie_census::Write(datadir, lines);
        zombie_census::Recheck(datadir);
    }
}

bool Remaining(std::uint64_t key, std::uint32_t* ms, bool* unread) {
    const u64 t0 = NowMs();
    *ms = 0;
    *unread = false;
    if (OnEmulator()) {
        return false; // (pm of an emulator keeps nothing)
    }
    u32 self = 0;
    svcGetProcessId(&self, CUR_PROCESS_HANDLE);
    std::vector<Seen> seen;
    bool whole = ReadKey(key, self, seen);
    if (!whole && seen.empty()) {
        whole = ReadKey(key, self, seen); // (DPAPP-2j-rev: read once more before the gate refuses on a list not read)
    }
    const bool any = !seen.empty();
    CloseAll(seen);
    *ms = static_cast<u32>(NowMs() - t0);
    *unread = !any && !whole; // (DPAPP-2j-rev: no closed game seen, the list not read whole - said as it is, never "a closed game")
    return any || !whole;
}

} // namespace zombie_free
