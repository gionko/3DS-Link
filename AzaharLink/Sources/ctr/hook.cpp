#include "ctr/hook.hpp"

#include <CTRPluginFramework.hpp>

#include <cstring>

#include "azl/apt_sleep.hpp"
#include "azl/arm_reloc.hpp"
#include "azl/svc_patch.hpp"
#include "azl/log.hpp"
#include "azl/stats.hpp"
#include "csvc.h"

// Assembly entry points (svc_entry.s).
extern "C" void azl_svc32_entry(void);
extern "C" void azl_svc23_entry(void);

namespace azl {
namespace ctr {

namespace {

constexpr u32 kSvcSendSyncRequest = 0xEF000032; // svc 0x32
constexpr u32 kSvcCloseHandle = 0xEF000023;     // svc 0x23
constexpr u32 kGameTextBase = 0x00100000;

// Room for 2048 hooked call sites; a game has a few hundred (Final Fantasy Explorers: 421).
constexpr u32 kMaxSites = 2048;

UdsHle* g_hle = nullptr;

struct Site {
    u32 address;
    u32 original[2];
};
Site g_sites[kMaxSites];
u32 g_site_count = 0;

// Executable scratch memory for the trampolines. It lives in the plugin's own image (.bss).
alignas(0x1000) u32 g_trampolines[kMaxSites * kWordsPerTrampoline];
u32 g_trampoline_words = 0;

u32 ReadGameWord(void*, u32 address) {
    return *reinterpret_cast<const u32*>(address);
}

/// Builds the trampoline for the call site at `site` (whose svc is followed by `next`).
bool BuildTrampoline(u32 site, u32 next, u32 entry, u32* tramp_address) {
    if (g_trampoline_words + kWordsPerTrampoline > kMaxSites * kWordsPerTrampoline) {
        return false;
    }
    u32* t = &g_trampolines[g_trampoline_words];
    const char* reason = "";
    if (!BuildSvcTrampoline(site, next, reinterpret_cast<u32>(t), entry, ReadGameWord, nullptr, t,
                            &reason)) {
        static u32 logged = 0;
        if (logged++ < 6) {
            Log("hook: skipped svc at 0x%08lx (next insn 0x%08lx): %s", static_cast<unsigned long>(site),
                static_cast<unsigned long>(next), reason);
        }
        return false;
    }
    g_trampoline_words += kWordsPerTrampoline;
    *tramp_address = reinterpret_cast<u32>(t);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Sites that CTRPluginFramework hooks itself
//
// The framework's GSP service module (Services::GSP::Initialize, libctrpf 0.8.0) hooks the game's
// GSPGPU::RegisterInterruptRelayQueue wrapper: it finds the wrapper by instruction pattern and
// replaces the ONE word that holds its `svc 0x32` with a branch to its own stub, which performs the
// svc itself and then resumes at the next word. That next word is where our own redirect keeps its
// literal (the address of the trampoline): had we patched the same svc, the framework's stub would
// run our literal as an instruction (a conditional load/store with arbitrary registers). Whether
// that is harmless depends only on the address of the trampoline, that is on how the plugin happens
// to be linked (presumably it was in 0.1.2 and 0.1.3, which booted the game). In 0.1.4 the literal
// is 0x070B0420 = `streq r0, [fp, -r0, lsr #8]`: with fp = &cursor it zeroed the GPU command-list
// pointer of Final Fantasy Explorers in the first second of the boot. We never need to see this
// call (it is neither nwm::UDS nor ndm:u), so
// the site is left to the framework. The patterns are the ones the framework itself searches for:
// the libctru-compiled wrapper (hook at +0x2C) and the prologue of the Nintendo-SDK-compiled one
// followed by the command header 0x00130042 somewhere in its first 0x50 bytes (hook at +0x28).
// ---------------------------------------------------------------------------------------------

constexpr u32 kGspRegisterInterruptHeader = 0x00130042;

constexpr u32 kGspWrapperCtru[] = {
    0xE92D4070, 0xE1A06003, 0xE59D5010, 0xEE1D3F70, 0xE2834080, 0xE59F3034, 0xE3A0C000,
    0xE584100C, 0xE9841004, 0xE5843000, 0xE5900000, 0xEF000032, 0xE2101102, 0x4A000004,
    0xE5940008, 0xE5850000, 0xE5940010, 0xE5860000, 0xE5940004, 0xE8BD8070, 0x00130042};
constexpr u32 kGspWrapperCtruSvcOffset = 0x2C;

constexpr u32 kGspWrapperSdk[] = {0xE92D4070, 0xE1A05003, 0xE59D6010, 0xEE1D4F70, 0xE59FC034,
                                  0xE3A03000, 0xE5A4C080, 0xE584100C, 0xE1C420F4};
constexpr u32 kGspWrapperSdkSvcOffset = 0x28;
constexpr u32 kGspWrapperSdkHeaderWindow = 0x50;

constexpr u32 kMaxFrameworkSites = 8;
u32 g_framework_sites[kMaxFrameworkSites];
u32 g_framework_site_count = 0;

bool MatchWords(u32 address, const u32* words, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (*reinterpret_cast<const u32*>(address + 4 * i) != words[i]) {
            return false;
        }
    }
    return true;
}

void AddFrameworkSite(u32 address) {
    if (g_framework_site_count < kMaxFrameworkSites) {
        g_framework_sites[g_framework_site_count++] = address;
    }
}

/// Finds the `svc 0x32` words the framework is going to hook (see above). [first, end) is the
/// readable part of the game's .text.
void FindFrameworkSites(u32 first, u32 end) {
    g_framework_site_count = 0;
    for (u32 addr = first; addr + 4 <= end; addr += 4) {
        if (*reinterpret_cast<const u32*>(addr) != 0xE92D4070) {
            continue; // both patterns start with push {r4-r6, lr}
        }
        if (addr + sizeof(kGspWrapperCtru) <= end &&
            MatchWords(addr, kGspWrapperCtru, sizeof(kGspWrapperCtru) / 4)) {
            AddFrameworkSite(addr + kGspWrapperCtruSvcOffset);
        } else if (addr + kGspWrapperSdkHeaderWindow <= end &&
                   MatchWords(addr, kGspWrapperSdk, sizeof(kGspWrapperSdk) / 4)) {
            for (u32 off = 0; off < kGspWrapperSdkHeaderWindow; off += 4) {
                if (*reinterpret_cast<const u32*>(addr + off) == kGspRegisterInterruptHeader) {
                    AddFrameworkSite(addr + kGspWrapperSdkSvcOffset);
                    break;
                }
            }
        }
    }
}

bool IsFrameworkSite(u32 address) {
    for (u32 i = 0; i < g_framework_site_count; ++i) {
        if (g_framework_sites[i] == address) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Diagnostics: trace the REAL services the game talks to (everything we do not emulate). The
// ones that can change the console's network state are followed command by command, so a log can
// show what really took the WiFi down.
//
// Everything from here to the end marker runs on the GAME's threads, on whatever stack the game gave
// the thread that made the IPC call: Tri Force Heroes' srv-notification thread has 464 bytes below
// its call, and 0.1.15 at debug=1 ran a printf chain of ~1.3 KB there, over the thread's own object
// (the game crashed on exit). So no printf family, no Log()/DebugEvent() with a format and no buffer
// over 64 bytes in this part: lines are built with LogLine / DebugLine (azl/line.hpp) or stored raw
// (DebugIpc, DebugSvcReal), and Tests/policy_check.sh and Tests/chain_sim check it.
// ---------------------------------------------------------------------------------------------

// >>> game-thread code (Tests/policy_check.sh)
constexpr u32 kSrvGetServiceHandle = 0x00050100;
constexpr u32 kMaxTracedSessions = 16;
constexpr u32 kMaxTraceLines = 160;

struct TracedSession {
    Handle handle;
    char name[12];
    bool apt; ///< an APT session: the system's notifications (home, sleep) come through it
};
TracedSession g_traced[kMaxTracedSessions];
u32 g_trace_lines = 0;

struct CommandSeen {
    u32 key;
    u32 count;
};
CommandSeen g_seen[48];
u32 g_seen_count = 0;

// The services that can change the console's network or power state, or that the sleep and lid logic of
// the system talks through (ptm:u: the power manager, cfg:u: the profile and the console's settings, cecd:u:
// StreetPass, dlp:*: download play, APT:*: the notifications). The network ones are followed command by
// command; an APT session only for the three commands of the sleep protocol (see PassThroughApt).
bool IsTracedService(const char* name) {
    static const char* const kNames[] = {"ndm:u",   "ac:u",     "ac:i",     "nwm::EXT", "nwm::INF", "nwm::SOC",
                                         "nwm::UDS", "boss:U",   "frd:u",    "ptm:u",    "cfg:u",    "cecd:u"};
    for (const char* n : kNames) {
        if (std::strcmp(n, name) == 0) {
            return true;
        }
    }
    return std::strncmp(name, "dlp:", 4) == 0;
}

bool IsAptService(const char* name) {
    return std::strcmp(name, "APT:U") == 0 || std::strcmp(name, "APT:A") == 0 || std::strcmp(name, "APT:S") == 0;
}

TracedSession* FindTraced(Handle handle) {
    for (TracedSession& s : g_traced) {
        if (s.handle == handle && handle != 0) {
            return &s;
        }
    }
    return nullptr;
}

// True for the first few times a (session, command) pair is seen.
bool WorthLogging(Handle handle, u32 command) {
    if (g_trace_lines >= kMaxTraceLines) {
        return false;
    }
    const u32 key = (handle << 12) ^ command;
    for (u32 i = 0; i < g_seen_count; ++i) {
        if (g_seen[i].key == key) {
            return ++g_seen[i].count <= 3;
        }
    }
    if (g_seen_count < 48) {
        g_seen[g_seen_count++] = {key, 1};
    }
    return true;
}

// APT: the system tells the game about the HOME button, the lid and sleep through InquireNotification;
// whether a sleep goes ahead is up to the games' answers (ReplySleepQuery: 0 reject, 1 accept, 2 later) and
// the acknowledgement that follows the sleep notification. Only these three commands are looked at, and
// only to log them: the call itself is made exactly as the game made it. The notification numbers are
// libctru's APT_Signal.
constexpr u32 kAptInquireNotification = 0x000B0040;
constexpr u32 kAptReplySleepQuery = 0x003E0080;
constexpr u32 kAptReplySleepNotificationComplete = 0x003F0040;
constexpr u32 kMaxAptLines = 120;

const char* AptSignalName(u32 id) {
    static const char* const kNames[] = {"none",          "home button",      "home button (2)", "sleep query",
                                         "sleep cancelled", "sleep begins",     "wake-up",         "shutdown",
                                         "power button",  "power button (2)", "try to sleep",    "order to close"};
    return id < sizeof(kNames) / sizeof(kNames[0]) ? kNames[id] : "?";
}

static SleepObserver g_sleep_observer = nullptr;
static HomeObserver g_home_observer = nullptr;
static AptObserver g_apt_observer = nullptr;

void SetSleepObserver(SleepObserver observer) {
    g_sleep_observer = observer;
}

void SetAptObserver(AptObserver observer) {
    g_apt_observer = observer;
}

void SetHomeObserver(HomeObserver observer) {
    g_home_observer = observer;
}

Result PassThroughApt(Handle handle) {
    u32* cmd = getThreadCommandBuffer();
    const u32 header = cmd[0];
    if (header != kAptInquireNotification && header != kAptReplySleepQuery &&
        header != kAptReplySleepNotificationComplete) {
        return svcSendSyncRequest(handle);
    }
    const u32 app_id = cmd[1];
    const u32 reply = cmd[2]; // (ReplySleepQuery: the request word; the reply buffer replaces it)
    // The trail of a sleep (azl/sleep_crumbs.hpp): the game's two replies are noted BEFORE their call - the acknowledgement
    // of a sleep may not return from it for as long as the console sleeps, or ever.
    if (g_apt_observer && g_debug_level > 0 && header != kAptInquireNotification) {
        g_apt_observer(header == kAptReplySleepQuery ? kAptNoteReplyQuery : kAptNoteReplyComplete, reply);
    }
    const Result rc = svcSendSyncRequest(handle);
    const Result result = R_SUCCEEDED(rc) ? cmd[1] : rc;
    static u32 lines = 0;
    if (header == kAptInquireNotification) {
        const u32 raw = cmd[2]; // after the call
        const u32 id = AptSignalOf(raw); // (the low 16 bits: Mario Party: The Top 100 gets 0x1BC00001 for the HOME button)
        if (id == 0) {
            return rc; // (polled all the time: only the notifications themselves are of interest)
        }
        if (g_apt_observer && g_debug_level > 0 && R_SUCCEEDED(rc)) {
            g_apt_observer(kAptNoteNotification, id); // (before the sleep observer below acts on it)
        }
        // The game's call is over and its answer is untouched; what the observer does with it is up to it.
        if (g_sleep_observer && R_SUCCEEDED(rc)) {
            switch (AptSleepEventOf(header, cmd)) {
            case AptSleepEvent::Enter:
                g_sleep_observer(true);
                break;
            case AptSleepEvent::Wake:
                g_sleep_observer(false);
                break;
            case AptSleepEvent::None:
                break;
            }
        }
        // The HOME button: the log is what a post-mortem has when the game dies while it is closed, so the writer is
        // woken to put it on the card now (the observer only signals an event: no SD write on this thread). After the
        // lines of this notification, so that the flush it asks for has them.
        const bool home = g_home_observer && g_debug_level > 0 && (id == 1 || id == 2);
        if (g_debug_level <= 0 || lines >= kMaxAptLines) {
            if (home) {
                g_home_observer(); // (the sleep observer above works at every level; the lines are the log's)
            }
            return rc;
        }
        ++lines;
        {
            LogLine line;
            line.Str("apt: notification ").Dec(id).Str(" (").Str(AptSignalName(id)).Str(") for app 0x").Hex(app_id)
                .Str(" -> 0x").Hex(result, 8);
            if (raw != id) {
                line.Str(" | reply word 0x").Hex(raw, 8); // (bits above the signal: see AptSignalOf)
            }
        }
        {
            DebugLine line;
            line.Str("apt InquireNotification app 0x").Hex(app_id).Str(" -> ").Dec(id).Str(" (").Str(AptSignalName(id))
                .Str(")");
            if (raw != id) {
                line.Str(" | reply word 0x").Hex(raw, 8);
            }
        }
        if (home) {
            g_home_observer();
        }
    } else if (g_debug_level > 0 && lines < kMaxAptLines) {
        ++lines;
        const bool query = header == kAptReplySleepQuery;
        const char* const name = query ? "ReplySleepQuery" : "ReplySleepNotificationComplete";
        {
            LogLine line;
            line.Str("apt: ").Str(name).Str(" app 0x").Hex(app_id).Str(query ? ": answer " : "");
            if (query) {
                line.Str(reply == 0 ? "0 (reject)" : reply == 1 ? "1 (accept)" : reply == 2 ? "2 (later)" : "?");
            }
            line.Str(" -> 0x").Hex(result, 8);
        }
        {
            DebugLine line;
            line.Str("apt ").Str(name).Str(" app 0x").Hex(app_id).Str(" answer ").Dec(reply);
        }
    }
    return rc;
}

// srv:GetServiceHandle: which real services the game opens (and the traced ones are remembered by session).
// Out of line, with a frame of its own: the common path below does not pay for these locals.
__attribute__((noinline)) Result PassThroughGetService(Handle handle) {
    u32* cmd = getThreadCommandBuffer();
    char name[9] = {};
    std::memcpy(name, &cmd[1], 8);
    const Result rc = svcSendSyncRequest(handle);
    const Result service_rc = R_SUCCEEDED(rc) ? cmd[1] : rc;
    const Handle session = R_SUCCEEDED(service_rc) ? cmd[3] : 0;
    if (g_debug_level > 0) {
        if (g_trace_lines < kMaxTraceLines) {
            ++g_trace_lines;
            LogLine line;
            line.Str("srv: the game opened the REAL service '").Str(name).Str("' -> 0x").Hex(service_rc, 8)
                .Str(" (handle 0x").Hex(session).Str(")");
        }
        DebugLine line;
        line.Str("srv GetServiceHandle '").Str(name).Str("' -> 0x").Hex(service_rc, 8).Str(" (handle 0x")
            .Hex(session).Str(")");
    }
    const bool apt = IsAptService(name);
    if (session && (apt || IsTracedService(name))) {
        for (TracedSession& s : g_traced) {
            if (s.handle == 0) {
                std::strncpy(s.name, name, sizeof(s.name) - 1);
                s.apt = apt;
                s.handle = session;
                break;
            }
        }
    }
    return rc;
}

// A command of a traced real service (ndm:u, ac:u, nwm::*, ptm:u, cfg:u, ...): followed for the log only.
__attribute__((noinline)) Result PassThroughTraced(Handle handle, const TracedSession* traced) {
    u32* cmd = getThreadCommandBuffer();
    if (g_debug_level <= 0) {
        return svcSendSyncRequest(handle);
    }
    const u32 command = cmd[0] >> 16;
    const u32 arg0 = cmd[1];
    const bool log_it = WorthLogging(handle, command);
    const Result rc = svcSendSyncRequest(handle);
    const u32 result = R_SUCCEEDED(rc) ? cmd[1] : static_cast<u32>(rc);
    if (log_it) {
        ++g_trace_lines;
        LogLine line;
        line.Str("svc: REAL ").Str(traced->name).Str(" command 0x").Hex(command, 4).Str(" (arg 0x").Hex(arg0)
            .Str(") -> 0x").Hex(result, 8);
    }
    DebugSvcReal(traced->name, command, arg0, result);
    return rc;
}

// What every IPC call of every thread of the game that no service of ours answers goes through. The common case -
// a call to a real service nobody traces - keeps this frame small on purpose: it is the one that runs on the
// srv-notification thread of Tri Force Heroes (464 bytes), and the branches with locals are out of line.
Result PassThrough(Handle handle) {
    u32* cmd = getThreadCommandBuffer();
    const u32 header = cmd[0];
    if (header == kSrvGetServiceHandle) {
        return PassThroughGetService(handle);
    }
    const TracedSession* traced = FindTraced(handle);
    if (traced) {
        return traced->apt ? PassThroughApt(handle) : PassThroughTraced(handle, traced);
    }
    ++g_stats.pass_calls;
    if (g_debug_level <= 0) {
        return svcSendSyncRequest(handle);
    }
    // Log on: every IPC request of the game (raw words in the ring: no formatting on this thread, whatever its stack is).
    const u32 arg = cmd[1];
    const Result rc = svcSendSyncRequest(handle);
    DebugIpc(handle, header, arg, R_SUCCEEDED(rc) ? cmd[1] : static_cast<u32>(rc));
    return rc;
}

// Called by the assembly entries, with the game's registers. The result goes back to r0 exactly
// like the result of the real svc.
extern "C" Result azl_hook_SendSyncRequest(Handle handle) {
    Result result;
    if (g_hle && g_hle->HandleSendSyncRequest(handle, &result)) {
        return result;
    }
    return PassThrough(handle);
}

extern "C" Result azl_hook_CloseHandle(Handle handle) {
    if (g_hle) {
        g_hle->NoteHandleClosed(handle);
    }
    if (TracedSession* traced = FindTraced(handle)) {
        traced->handle = 0; // the value may be reused for another object
    }
    return svcCloseHandle(handle);
}
// <<< game-thread code

HookStats InstallSvcHooks(UdsHle* hle) {
    HookStats stats;
    g_hle = hle;
    g_site_count = 0;
    g_trampoline_words = 0;

    stats.text_start = kGameTextBase;
    stats.text_size = CTRPluginFramework::Process::GetTextSize();
    if (stats.text_size < 8) {
        Log("hook: could not determine the size of the game's .text");
        return stats;
    }

    // The trampolines are code that we write at runtime: make the buffer executable.
    if (!CTRPluginFramework::Process::ProtectMemory(reinterpret_cast<u32>(g_trampolines),
                                                    sizeof(g_trampolines))) {
        Log("hook: cannot make the trampoline buffer executable");
        return stats;
    }
    // Not verified with svcQueryMemory on purpose: Luma3DS makes the whole process RWX in the MMU
    // (PROCESSOP_SET_MMU_TO_RWX) while the kernel's logical permissions stay RW, so the query
    // reports "not executable" for memory that does execute (it blocked every hook on hardware).
    stats.trampolines_ready = true;

    const u32 first = stats.text_start;
    const u32 last = stats.text_start + stats.text_size - 8;
    const u32 send_entry = reinterpret_cast<u32>(&azl_svc32_entry);
    const u32 close_entry = reinterpret_cast<u32>(&azl_svc23_entry);

    // The framework hooks one of the game's svc 0x32 words by itself: do not touch that one.
    FindFrameworkSites(first, stats.text_start + stats.text_size);

    // Pass 1: build every trampoline (nothing in the game is modified yet).
    struct Pending {
        u32 site;
        u32 tramp;
        bool is_send;
    };
    static Pending pending[kMaxSites];
    u32 pending_count = 0;

    for (u32 addr = first; addr <= last && pending_count < kMaxSites; addr += 4) {
        const u32 word = *reinterpret_cast<const u32*>(addr);
        const bool is_send = word == kSvcSendSyncRequest;
        if (!is_send && word != kSvcCloseHandle) {
            continue;
        }
        if (is_send && IsFrameworkSite(addr)) {
            ++stats.framework_sites;
            Log("hook: svc at 0x%08lx left to CTRPluginFramework (it hooks GSPGPU::RegisterInterruptRelayQueue)",
                static_cast<unsigned long>(addr));
            continue;
        }
        const u32 next = *reinterpret_cast<const u32*>(addr + 4);
        u32 tramp = 0;
        if (BuildTrampoline(addr, next, is_send ? send_entry : close_entry, &tramp)) {
            pending[pending_count++] = {addr, tramp, is_send};
        } else {
            ++stats.skipped_sites;
        }
    }
    // Thumb call sites are only counted (a Thumb "svc 0x32" is the halfword 0xDF32).
    for (u32 addr = first; addr <= last; addr += 2) {
        if (*reinterpret_cast<const u16*>(addr) == 0xDF32) {
            ++stats.thumb_candidates;
        }
    }

    // Make the trampolines visible to instruction fetch.
    svcFlushDataCacheRange(g_trampolines, sizeof(g_trampolines));
    svcInvalidateEntireInstructionCache();

    // Pass 2: redirect the game's call sites. `ldr pc,[pc,#-4]` + the trampoline address replaces
    // the svc and the instruction after it (which now lives in the trampoline).
    g_site_count = 0;
    for (u32 i = 0; i < pending_count; ++i) {
        const u32 patch[2] = {kSiteJumpWord, pending[i].tramp};
        Site& site = g_sites[g_site_count];
        site.address = pending[i].site;
        if (!CTRPluginFramework::Process::Patch(pending[i].site, const_cast<u32*>(patch),
                                                sizeof(patch), site.original)) {
            ++stats.skipped_sites;
            continue;
        }
        ++g_site_count;
        if (pending[i].is_send) {
            ++stats.send_sync_sites;
        } else {
            ++stats.close_sites;
        }
    }

    Log("hook: text 0x%08lx+0x%lx: redirected %lu SendSyncRequest + %lu CloseHandle call sites "
        "(%lu skipped, %lu left to the framework, %lu possible Thumb svc 0x32)",
        static_cast<unsigned long>(stats.text_start), static_cast<unsigned long>(stats.text_size),
        static_cast<unsigned long>(stats.send_sync_sites),
        static_cast<unsigned long>(stats.close_sites), static_cast<unsigned long>(stats.skipped_sites),
        static_cast<unsigned long>(stats.framework_sites), static_cast<unsigned long>(stats.thumb_candidates));
    return stats;
}

void ReportForeignPatches() {
    u32 foreign = 0;
    for (u32 i = 0; i < g_site_count; ++i) {
        const u32 now = *reinterpret_cast<const u32*>(g_sites[i].address);
        if (now == kSiteJumpWord) {
            continue;
        }
        if (foreign++ < 6) {
            Log("hook: site 0x%08lx was changed by someone else after we patched it (now 0x%08lx)",
                static_cast<unsigned long>(g_sites[i].address), static_cast<unsigned long>(now));
        }
    }
    if (foreign) {
        Log("hook: WARNING %lu site(s) are no longer ours: our literal word next to them may run as code",
            static_cast<unsigned long>(foreign));
    }
}

} // namespace ctr
} // namespace azl
