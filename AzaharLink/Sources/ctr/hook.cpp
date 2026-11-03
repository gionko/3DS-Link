#include "ctr/hook.hpp"
#include "ctr/text_search_guard.hpp"

#include <CTRPluginFramework.hpp>

#include <cstring>

#include "azl/apt_sleep.hpp"
#include "azl/arm_reloc.hpp"
#include "azl/gsp_wrapper.hpp"
#include "azl/hook_swap.hpp"
#include "azl/svc_patch.hpp"
#include "azl/log.hpp"
#include "azl/platform.hpp"
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

HookSite g_sites[kMaxSites];
u32 g_site_count = 0;

// The swap of the plugin's memory on an Old 3DS (azl/hook_swap.hpp): what is ours to remember between the two events.
HandBackLedger g_ledger;
volatile u32 g_hooks_state = kHooksActive;
volatile u32 g_patch_lock = 0; // the plugin's threads that change the game's sites (the keep-alive thread, the frame callback) take turns; the game's thread never waits for it
SwapStats g_swap_stats;
bool g_old_3ds = false;
u32 g_gsp_stub_wrappers = 0; // what RewriteStubbedGspWrappers() rewrote (InstallSvcHooks reports it)

/// The game's text, written and read as the host tests' array is: the words, the data cache cleaned for them, the instruction cache invalidated for all cores
/// by Sync(). Luma3DS has made the whole process RWX in the MMU (see InstallSvcHooks), so the stores need no permission call.
struct GameTextMemory {
    u32 Load(u32 address) const {
        return *reinterpret_cast<const volatile u32*>(address);
    }
    // (a thread that passes a site between the two stores finds the svc and then the literal word: the svc is the game's own, and the literal, a trampoline
    //  address, runs as a conditional instruction that most often does nothing; the other order would jump into the second word)
    void StoreOriginal(u32 address, const u32 words[2]) const {
        Poke(address, words[0]);
        Poke(address + 4, words[1]);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, address, 8);
    }
    void StoreHook(u32 address, const u32 words[2]) const {
        Poke(address + 4, words[1]);
        Poke(address, words[0]);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, address, 8);
    }
    void Sync() const {
        svcInvalidateEntireInstructionCache();
    }
    static void Poke(u32 address, u32 value) {
        *reinterpret_cast<volatile u32*>(address) = value;
    }
};

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

// (the patterns themselves are in azl/gsp_wrapper.hpp, where the host tests can reach them)

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
// The threads that are inside an entry of svc_entry.s right now (it counts them in and out; Tests/hook_sim/entry_sim.py holds the assembly, ctr/hook.cpp's
// SuspendSvcHooks waits for it to reach zero before the framework answers Luma3DS and the plugin's block is freed).
extern "C" {
volatile u32 azl_hooks_in_flight = 0;
}

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

// Download Play (dlp:FKCL, the fake client of the games that join a Download Play session as a game with its own
// copy: Kirby Battle Royale, Mario Kart 7, Luigi's Mansion): the system module starts a radio of its own, which
// cannot happen while ndm is faked to keep the tunnel up, and answers "wireless off". One explanatory line per
// launch, only with the log on (LogLine is inactive with the log off: the card stays untouched).
constexpr u32 kDlpWirelessOff = 0xC940AC04;
bool g_dlp_open_noted = false;
bool g_dlp_fail_noted = false;

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
constexpr u32 kAptReceiveParameter = 0x000D0080;
constexpr u32 kAptAppletUtility = 0x004B00C2; // (utility id, input size, output size, static buffer descriptor, input)
constexpr u32 kMaxAptLines = 120;

// The HOME button and the power button as the game's APT session tells them (InquireNotification, AptSignalPrecedesSwap). On an Old 3DS the swap of the plugin's memory follows within a moment, and
// it is the game's next APT:ReceiveParameter that raises it (azl_handback_site below): that call is only handed back to its call site in the seconds after this.
constexpr u32 kHomeWindowMs = 10000;
volatile u32 g_home_seen = 0;
volatile u32 g_home_ms = 0;

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

// The two calls of the transition to and from the HOME menu, with the log on: what the system answered, for the trail. Out of line: the other APT commands do not pay for these locals.
u32 g_receive_notes = 0;
constexpr u32 kMaxReceiveNotes = 64;

// One more line for the trail, while there is room; the last one says that the limit is reached.
inline bool TakeReceiveNote() {
    if (g_receive_notes > kMaxReceiveNotes) {
        return false;
    }
    if (++g_receive_notes > kMaxReceiveNotes) {
        g_apt_observer(kAptNoteLimit, 0);
        return false;
    }
    return true;
}

__attribute__((noinline)) Result PassThroughAptNoted(Handle handle, u32 header) {
    u32* cmd = getThreadCommandBuffer();
    const u32 id = cmd[1];
    u32 input = 0;
    if (header == kAptAppletUtility && cmd[2] >= 2 && cmd[5] != 0) {
        input = *reinterpret_cast<const u16*>(cmd[5]); // (the kernel would refuse the call with a bad pointer: the game's own buffer)
    }
    const Result rc = svcSendSyncRequest(handle);
    if (TakeReceiveNote()) { // (a game that polls these must not use up the lines of the trail)
        if (header == kAptAppletUtility) {
            g_apt_observer(kAptNoteUtility, input << 16 | (id & 0xFFFF));
            g_apt_observer(kAptNoteUtilityResult, R_SUCCEEDED(rc) ? cmd[1] : static_cast<u32>(rc));
        } else {
            g_apt_observer(kAptNoteReceiveReply, R_SUCCEEDED(rc) && static_cast<s32>(cmd[1]) >= 0 ? cmd[2] << 16 | (cmd[3] & 0xFFFF) : kAptReceiveFailed);
        }
    }
    return rc;
}

Result PassThroughApt(Handle handle) {
    u32* cmd = getThreadCommandBuffer();
    const u32 header = cmd[0];
    if (header != kAptInquireNotification && header != kAptReplySleepQuery &&
        header != kAptReplySleepNotificationComplete) {
        if (g_debug_level > 0 && g_apt_observer && (header == kAptAppletUtility || (header == kAptReceiveParameter && cmd[1] == 0x300))) {
            return PassThroughAptNoted(handle, header);
        }
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
        const u32 id = AptSignalOf(raw); // (the low byte: the bits above it change from call to call)
        if (id == 0) {
            return rc; // (polled all the time: only the notifications themselves are of interest)
        }
        const bool answered = R_SUCCEEDED(rc) && static_cast<s32>(cmd[1]) >= 0; // (a failed call leaves in cmd[2] what an earlier one left: with 8 bits that can look like a button)
        if (answered && AptSignalPrecedesSwap(id)) {
            g_home_ms = static_cast<u32>(platform::NowMs());
            g_home_seen = 1;
        }
        if (g_apt_observer && g_debug_level > 0 && answered) {
            g_apt_observer(kAptNoteNotification, raw); // (the word as it came; before the sleep observer below acts on it)
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
        // The HOME and the power button: the log is what a post-mortem has when the game dies while it is closed, so the writer is
        // woken to put it on the card now (the observer only signals an event: no SD write on this thread). After the
        // lines of this notification, so that the flush it asks for has them.
        const bool home = g_home_observer && g_debug_level > 0 && AptSignalPrecedesSwap(id);
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

// The last session the game opened through the hook, and when (with the log on): a call that later fails on handle 0 says with these whether the hook saw that session being opened.
u32 g_last_service = 0;
u32 g_last_service_ms = 0;

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
        g_last_service = session;
        g_last_service_ms = static_cast<u32>(platform::NowMs());
        if (g_trace_lines < kMaxTraceLines) {
            ++g_trace_lines;
            LogLine line;
            line.Str("srv: the game opened the REAL service '").Str(name).Str("' -> 0x").Hex(service_rc, 8)
                .Str(" (handle 0x").Hex(session).Str(")");
        }
        DebugLine line;
        line.Str("srv GetServiceHandle '").Str(name).Str("' -> 0x").Hex(service_rc, 8).Str(" (handle 0x")
            .Hex(session).Str(")");
        if (session && !g_dlp_open_noted && std::strcmp(name, "dlp:FKCL") == 0) {
            g_dlp_open_noted = true;
            LogLine note;
            note.Str("dlp: the game opened dlp:FKCL: its local multiplayer is Download Play, which the system module "
                     "cannot run while this plugin keeps the console on the access point (ndm is faked); expect "
                     "'wireless must be turned on' and no tunnel for this game");
        }
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
    if (result == kDlpWirelessOff && !g_dlp_fail_noted && std::strcmp(traced->name, "dlp:FKCL") == 0) {
        g_dlp_fail_noted = true;
        LogLine note;
        note.Str("dlp: dlp:FKCL command 0x").Hex(command, 4).Str(" answered 0xC940AC04 (the system module says "
                 "wireless is off): this game's local multiplayer needs the real radio and cannot use the tunnel");
    }
    return rc;
}

// A call of the game that failed for good, with the log on: what a dump does not say, written down before the game goes on to its error screen. One line of the trail (AzaharLink.sleep.log): the thread,
// the command buffer and the words of the stack that look like code: above the plugin's frames are the trampoline's return and the game's own lr. At most 8 invalid handles in a launch, 100 ms apart
// (a thread that insists fills the ring in microseconds, and the writes are slow); a fatal error always, and it gives the thread of the trail time to write it: what follows the call may stop the process.
constexpr u32 kKernelInvalidHandle = 0xD8E007F7;
constexpr u32 kErrfThrow = 0x00010800; // err:f ThrowFatalError: type, result, pc, process, title in the words after the header
constexpr u32 kFailureGapMs = 100;
u32 g_failures_noted = 0;
u32 g_failure_ms = 0;
u32 g_resume_end_ms = 0; // (the call sites were last put back in the hook at this time: ResumeSvcHooks)
u32 g_failure_busy = 0; // (one line at a time: two threads would mix their words)

__attribute__((noinline)) void NoteGameFailure(u32 kind, Handle handle) {
    if (!g_apt_observer) {
        return;
    }
    const u32 now = static_cast<u32>(platform::NowMs());
    if (kind != kAptNoteFatal && (g_failures_noted >= 8 || (g_failures_noted != 0 && now - g_failure_ms < kFailureGapMs))) {
        return;
    }
    if (__atomic_exchange_n(&g_failure_busy, 1u, __ATOMIC_ACQUIRE) != 0) {
        return;
    }
    if (kind != kAptNoteFatal) {
        ++g_failures_noted;
        g_failure_ms = now;
    }
    const u32* const cmd = getThreadCommandBuffer();
    const u32* stack;
    __asm__ volatile("mov %0, sp" : "=r"(stack));
    u32 thread_id = 0;
    svcGetThreadId(&thread_id, CUR_THREAD_HANDLE);
    g_apt_observer(kind, handle);
    s32 priority = -1;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    g_apt_observer(kAptNoteWord, thread_id);
    g_apt_observer(kAptNoteWord, static_cast<u32>(priority));
    g_apt_observer(kAptNoteWord, static_cast<u32>(svcGetProcessorID()));
    g_apt_observer(kAptNoteWord, g_hooks_state);
    g_apt_observer(kAptNoteWord, now - g_resume_end_ms);
    g_apt_observer(kAptNoteWord, g_last_service);
    g_apt_observer(kAptNoteWord, now - g_last_service_ms);
    for (u32 i = 0; i < 7; ++i) {
        g_apt_observer(kAptNoteWord, cmd[i]);
    }
    u32 kept = 0;
    for (u32 i = 0; i < 40 && kept < 14; ++i) {
        const u32 word = stack[i];
        if ((word >= kGameTextBase && word < 0x04000000) || (word >= 0x07000000 && word < 0x07200000)) {
            g_apt_observer(kAptNoteWord, word);
            ++kept;
        }
    }
    g_apt_observer(kAptNoteEnd, 0);
    __atomic_store_n(&g_failure_busy, 0u, __ATOMIC_RELEASE);
    if (kind == kAptNoteFatal) {
        svcSleepThread(50000000LL);
    }
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
    if (header == kErrfThrow) {
        NoteGameFailure(kAptNoteFatal, handle); // (before the call: it does not come back)
    }
    const Result rc = svcSendSyncRequest(handle);
    if (static_cast<u32>(rc) == kKernelInvalidHandle) {
        NoteGameFailure(kAptNoteBadHandle, handle);
    }
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

// Called by azl_svc32_entry (svc_entry.s) before it makes an APT:ReceiveParameter(application) call, with the trampoline's return address: the call at whose end
// Luma3DS's kernel raises the swap of the plugin's block on an Old 3DS, on the calling thread, which it then puts to sleep until the block is gone and back.
// Returns the call site the game's thread must go back to (its svc and next instruction are the game's own again) to make the call itself, or 0 to make it here.
// A few loads on every path but the one answer (with the log on, a line for the trail: its ring and an event signal): no printf, no wait, and the frame is held by Tests/stack_usage.sh.
inline void NoteReceiveCall(u32 kind, u32 where) {
    if (g_apt_observer && g_debug_level > 0 && TakeReceiveNote()) { // (a game that polls this call must not use up the lines of the trail)
        g_apt_observer(kind, where); // (SWAP_ENTER waits for the trail: the crash this is about comes at the end of the call, with the line on the card)
    }
}

extern "C" u32 azl_handback_site(Handle, u32 tramp_return) {
    __atomic_fetch_add(&g_swap_stats.asked, 1u, __ATOMIC_RELAXED);
    if (!g_old_3ds) {
        return 0;
    }
    NoteReceiveSite(g_sites, g_site_count, g_ledger, tramp_return);
    const u32 now = static_cast<u32>(platform::NowMs());
    if (!g_home_seen || static_cast<u32>(now - g_home_ms) > kHomeWindowMs) {
        __atomic_fetch_add(&g_swap_stats.no_home, 1u, __ATOMIC_RELAXED);
        NoteReceiveCall(kAptNoteReceiveMade, tramp_return);
        return 0;
    }
    const GameTextMemory memory;
    const u32 site = HandBackSite(memory, g_sites, g_site_count, g_ledger, &g_hooks_state, tramp_return, now);
    __atomic_fetch_add(site ? &g_swap_stats.handbacks : &g_swap_stats.handback_refused, 1u, __ATOMIC_RELAXED);
    NoteReceiveCall(site ? kAptNoteReceiveHanded : kAptNoteReceiveRefused, site ? site : tramp_return);
    return site;
}
// <<< game-thread code

u32 RewriteStubbedGspWrappers() {
    const u32 first = kGameTextBase;
    const u32 text_size = CTRPluginFramework::Process::GetTextSize();
    if (text_size < 8) {
        return 0; // (InstallSvcHooks says so in the log when the plugin is on)
    }
    // A game whose GSP wrapper has its svc behind a stub (LEGO Batman 2) would make the framework abort at start: the svc is written in
    // place of the `bl` (the same for the CPU), so that the framework finds the wrapper it knows. This must come before the framework's search
    // (GSP::Initialize, after PatchProcess) and before FindFrameworkSites; it reads the game's .text and writes one word, so it does not need the
    // plugin to be on: with the main switch off Luma3DS keeps the plugin loaded and the framework still runs.
    u32 stubbed[4];
    const u32 count = FindStubbedGspWrappers(ReadGameWord, nullptr, first, first + text_size, stubbed, 4);
    u32 rewritten = 0;
    for (u32 i = 0; i < count; ++i) {
        if (CTRPluginFramework::Process::Patch(stubbed[i], kSvcSendSyncRequest)) {
            ++rewritten;
            Log("hook: the GSP wrapper at 0x%08lx calls its svc through a stub: the svc is now in place of the bl, so that the "
                "framework finds the wrapper (it aborts at start otherwise)",
                static_cast<unsigned long>(stubbed[i] - kGspWrapperCtruSvcOffset));
        } else {
            Log("hook: could not rewrite the bl of the GSP wrapper at 0x%08lx", static_cast<unsigned long>(stubbed[i] - kGspWrapperCtruSvcOffset));
        }
    }
    g_gsp_stub_wrappers = rewritten;
    return rewritten;
}

HookStats InstallSvcHooks(UdsHle* hle) {
    HookStats stats;
    g_hle = hle;
    g_site_count = 0;
    g_trampoline_words = 0;
    g_old_3ds = !CTRPluginFramework::System::IsNew3DS();
    g_hooks_state = kHooksActive;
    LedgerClear(g_ledger);

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

    // (A game whose GSP wrapper has its svc behind a stub has been dealt with already, by RewriteStubbedGspWrappers(): it runs from Plugin::EarlyInit
    //  whether the plugin is on or off, before this, so that the search for the framework's sites below finds the wrapper as the framework does.)
    stats.gsp_stub_wrappers = g_gsp_stub_wrappers;

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
        HookSite& site = g_sites[g_site_count];
        site.address = pending[i].site;
        site.tramp = pending[i].tramp;
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
    if (TextSearchTrims() != 0) {
        Log("hook: the framework's startup search for the GSP call was cut to the game's code %lu time(s) "
            "(it would have read past the end of .text and crashed)",
            static_cast<unsigned long>(TextSearchTrims()));
    }
    return stats;
}

namespace {

bool TakePatchLock() {
    return __atomic_exchange_n(&g_patch_lock, 1u, __ATOMIC_ACQUIRE) == 0;
}

void ReleasePatchLock() {
    __atomic_store_n(&g_patch_lock, 0u, __ATOMIC_RELEASE);
}

// The threads of the plugin that change the game's sites take turns (a re-hook and a suspend must not interleave: the block would be freed with a site pointing in
// it). The suspend and the resume wait a little; the re-hook gives up and comes back with the next frame.
void WaitPatchLock() {
    for (u32 i = 0; i < 2000 && !TakePatchLock(); ++i) {
        svcSleepThread(100000);
    }
}

constexpr s64 kSwapStepNs = 100000;   // between two looks at the counter of threads in the hook
constexpr s64 kSwapGraceNs = 2000000; // after the last one left: the few instructions each has left in its trampoline

} // namespace

u32 HooksInFlight() {
    return __atomic_load_n(&azl_hooks_in_flight, __ATOMIC_ACQUIRE);
}

void SuspendSvcHooks(u32 timeout_ms) {
    if (g_site_count == 0) {
        return;
    }
    WaitPatchLock(); // (not obtained after 200 ms: goes on without it, the block is about to go)
    const GameTextMemory memory;
    const u64 start = platform::NowUs();
    g_swap_stats.in_flight = HooksInFlight();
    SuspendSites(memory, g_sites, g_site_count, g_ledger, &g_hooks_state, &g_swap_stats);
    // The threads that are inside the hook already: a call that waits in a real service, a lock of the hub. (The thread that raised the swap is not one of
    // them: it went back to the game's code, see azl_handback_site.)
    u32 left = HooksInFlight();
    for (;;) {
        const u64 waited_us = platform::NowUs() - start;
        if (left == 0 || waited_us >= static_cast<u64>(timeout_ms) * 1000) {
            break;
        }
        svcSleepThread(kSwapStepNs);
        left = HooksInFlight();
    }
    svcSleepThread(kSwapGraceNs);
    left = HooksInFlight();
    if (left != 0) { // (one more look: a thread that was in the trampoline's first instructions when the counter was zero)
        svcSleepThread(kSwapStepNs);
        left = HooksInFlight();
    }
    const u64 waited_us = platform::NowUs() - start;
    g_swap_stats.in_flight_left = left;
    g_swap_stats.waited_us = waited_us > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(waited_us);
    ReleasePatchLock();
}

void ResumeSvcHooks() {
    if (g_site_count == 0) {
        return;
    }
    WaitPatchLock();
    const GameTextMemory memory;
    ResumeSites(memory, g_sites, g_site_count, g_ledger, &g_hooks_state, static_cast<u32>(platform::NowMs()), &g_swap_stats);
    ReleasePatchLock();
    g_resume_end_ms = static_cast<u32>(platform::NowMs());
}

void RearmHandedBackSites() {
    if (!LedgerAny(g_ledger) || !TakePatchLock()) {
        return;
    }
    const GameTextMemory memory;
    RearmSites(memory, g_sites, g_site_count, g_ledger, &g_hooks_state, static_cast<u32>(platform::NowMs()), &g_swap_stats);
    ReleasePatchLock();
}

SwapStats GetSwapStats() {
    SwapStats stats = g_swap_stats;
    stats.handbacks = __atomic_load_n(&g_swap_stats.handbacks, __ATOMIC_RELAXED);
    stats.handback_refused = __atomic_load_n(&g_swap_stats.handback_refused, __ATOMIC_RELAXED);
    stats.asked = __atomic_load_n(&g_swap_stats.asked, __ATOMIC_RELAXED);
    stats.no_home = __atomic_load_n(&g_swap_stats.no_home, __ATOMIC_RELAXED);
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
