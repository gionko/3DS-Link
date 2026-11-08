#include "ctr/hook.hpp"
#include "ctr/text_search_guard.hpp"

#include <CTRPluginFramework.hpp>

#include <cstring>

#include "azl/apt_sleep.hpp"
#include "azl/chat_host.hpp"
#include "azl/gate_plan.hpp"
#include "azl/gsp_wrapper.hpp"
#include "azl/hook_swap.hpp"
#include "azl/traced_sessions.hpp"
#include "azl/log.hpp"
#include "azl/platform.hpp"
#include "azl/stats.hpp"
#include "csvc.h"
#include "ctr/gate_page.hpp"
#include "ctr/input_mirror.hpp"
#include "ctr/swap_gate.hpp"
#include "ctr/zombie_probe.hpp"

// The gate (Sources/ctr/gate.s): the code that is copied into its page, and its two entries.
extern "C" const u32 azl_gate_begin[];
extern "C" const u32 azl_gate_end[];
extern "C" const u32 azl_gate_32[];
extern "C" const u32 azl_gate_23[];
// The exit guard's trampoline (Sources/ctr/exit_tramp.s): copied into the tail of the gate's page; its three words written in the copy.
extern "C" const u32 azl_exit_tramp_begin[];
extern "C" const u32 azl_exit_tramp_end[];
extern "C" const u32 azl_exit_tramp_state[];
extern "C" const u32 azl_exit_tramp_fn[];
extern "C" const u32 azl_exit_tramp_stack[];

namespace azl {
namespace ctr {

namespace {

constexpr u32 kSvcSendSyncRequest = 0xEF000032; // svc 0x32
constexpr u32 kSvcCloseHandle = 0xEF000023;     // svc 0x23
constexpr u32 kSvcExitProcess = 0xEF000003;     // svc 0x03 (the exit guard: InstallExitGuard)
constexpr u32 kExitTrampStubs = 16;             // the gate's page keeps this many stubs' worth (192 bytes) for the exit guard's trampoline (168)
constexpr u32 kGameTextBase = 0x00100000;

// Room for 2048 hooked call sites; a game has a few hundred (Final Fantasy Explorers: 421).
constexpr u32 kMaxSites = 2048;

UdsHle* g_hle = nullptr;

// The hooked call sites, in the order of their stubs in the gate's page (azl/gate_plan.hpp: SiteIndex): where, and the svc that was there.
GateSite g_sites[kMaxSites];
u32 g_site_count = 0;

// The gate's page (ctr/gate_page.hpp) and its header; the swap flips the header's flag (CloseGate, OpenGate) and nothing else.
GatePage g_gate_page;
GateHeader* g_gate = nullptr;
u64 g_apt_receive_tick = 0; // the tick of the game's last APT ReceiveParameter(application) call (LastAptReceiveTick)
u32 g_stubs_base = 0;
HookGateStats g_gate_stats;
u32 g_gate_last_open_ms = 0; // (the gate last opened at this time: the line of a failed call says how long ago)
u32 g_gsp_stub_wrappers = 0; // what RewriteStubbedGspWrappers() rewrote (InstallSvcHooks reports it)

u32 ReadGameWord(void*, u32 address) {
    return *reinterpret_cast<const u32*>(address);
}

/// What WriteGate (azl/gate_plan.hpp) writes through: the gate's page with plain stores, ONE word of the game's text per site with the
/// framework's Process::Patch (and its data cache cleaned), then the instruction cache of every core. Luma3DS has made the whole process
/// RWX in the MMU (see InstallSvcHooks). This runs once, in EarlyInit, before the game's first instruction.
struct GateTextMemory {
    u32 page;
    u32 bytes;
    u32 Load(u32 address) const {
        return *reinterpret_cast<const volatile u32*>(address);
    }
    void StorePage(u32 address, u32 word) const {
        *reinterpret_cast<volatile u32*>(address) = word;
    }
    bool PatchText(u32 address, u32 word) const {
        if (!CTRPluginFramework::Process::Patch(address, word)) {
            return false;
        }
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, address, 4);
        return true;
    }
    void Sync() const {
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, page, bytes);
        svcInvalidateEntireInstructionCache();
    }
};

// ---------------------------------------------------------------------------------------------
// Sites that CTRPluginFramework hooks itself
//
// The framework's GSP service module (Services::GSP::Initialize, libctrpf 0.8.0) hooks the game's
// GSPGPU::RegisterInterruptRelayQueue wrapper: it finds the wrapper by instruction pattern and
// replaces the ONE word that holds its `svc 0x32` with a branch to its own stub, which performs the
// svc itself and then resumes at the next word. Our gate wants that same word for its `b stub`: whoever
// writes it last wins, and the other's code is never run. Up to 1.1.2 the hook took two words and the
// framework's stub resumed on our second one, a literal (an address in the plugin's block), run as an
// instruction: in 0.1.4 it was 0x070B0420 = `streq r0, [fp, -r0, lsr #8]`, and with fp = &cursor it
// zeroed the GPU command-list pointer of Final Fantasy Explorers in the first second of the boot. We
// never need to see this call (it is neither nwm::UDS nor ndm:u), so
// the site is left to the framework. The patterns are the ones the framework itself searches for:
// the libctru-compiled wrapper (hook at +0x2C) and the prologue of the Nintendo-SDK-compiled one
// followed by the command header 0x00130042 somewhere in its first 0x50 bytes (hook at +0x28).
// ---------------------------------------------------------------------------------------------

// (the patterns themselves are in azl/gsp_wrapper.hpp, where the host tests can reach them)

constexpr u32 kMaxFrameworkSites = 8;
u32 g_framework_sites[kMaxFrameworkSites]; // (azl/gsp_wrapper.hpp, FindFrameworkSites: the framework's svc 0x32 words, left as they are)
u32 g_framework_site_count = 0;

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
// The gate's flag (azl/gate_plan.hpp, GateHeader::flag; ctr/gate_flag.hpp): 0 while the swap of the plugin's block closes the gate. Until
// the gate's page exists (and for good when there is none: no game thread then comes into the plugin) it points at a word of the plugin's
// own that stays 1; InstallSvcHooks points it at the header's flag (Tests/chain_sim writes the flag to play a swap).
volatile u32 g_gate_always_open = 1;
volatile u32* g_gate_flag = &g_gate_always_open;

// What pre tells the gate (Sources/ctr/gate.s: r1 of pre, then ctx[3] for post): 0 = answered here (r0 is the result), 1 = make the svc,
// 2 and up = make the svc, then call post, which reads in ctx[3] which of the posts below it is.
constexpr u32 kActAnswer = 0;
constexpr u32 kActSvc = 1;
constexpr u32 kActPostFirst = 2;
constexpr u32 kActPostIpc = 2;         // the debug line of a plain call (PassThroughPost)
constexpr u32 kActPostGetService = 3;  // srv:GetServiceHandle (PassThroughGetServicePost)
constexpr u32 kActPostTraced = 4;      // a command of a traced real service (PassThroughTracedPost)
constexpr u32 kActPostApt = 5;         // the three commands of the sleep protocol (PassThroughAptPost)
constexpr u32 kActPostAptNoted = 6;    // ReceiveParameter(application) / AppletUtility with the log on (PassThroughAptNotedPost)

inline u64 PreReply(u32 r0, u32 action) {
    return static_cast<u64>(action) << 32 | r0;
}

constexpr u32 kSrvGetServiceHandle = 0x00050100;
constexpr u32 kSrvGetServiceHandleReply = 0x00050042; // (result, then one handle moved to the caller)
constexpr u32 kIpcMoveOneHandle = 0x10;
constexpr u32 kMaxTracedSessions = 16;
constexpr u32 kMaxTraceLines = 160;

TracedTable<kMaxTracedSessions> g_traced; // (azl/traced_sessions.hpp: what keeps it true when a close is not seen)
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
// command; an APT session only for the three commands of the sleep protocol (see PassThroughAptPre).
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
    return g_traced.Find(handle);
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

const char* AptSignalName(u32 id) {
    static const char* const kNames[] = {"none",          "home button",      "home button (2)", "sleep query",
                                         "sleep cancelled", "sleep begins",     "wake-up",         "shutdown",
                                         "power button",  "power button (2)", "try to sleep",    "order to close"};
    return id < sizeof(kNames) / sizeof(kNames[0]) ? kNames[id] : "?";
}

static SleepObserver g_sleep_observer = nullptr;
static HomeObserver g_home_observer = nullptr;
static AptObserver g_apt_observer = nullptr;
static TrailWaiter g_trail_waiter = nullptr;

void SetSleepObserver(SleepObserver observer) {
    g_sleep_observer = observer;
}

void SetAptObserver(AptObserver observer) {
    g_apt_observer = observer;
}

void SetHomeObserver(HomeObserver observer) {
    g_home_observer = observer;
}

void SetTrailWaiter(TrailWaiter waiter) {
    g_trail_waiter = waiter;
}

// The two calls of the transition to and from the HOME menu, with the log on: what the system answered, for the trail. Out of line: the other APT commands do not pay for these locals.
// Their lines: 64 a launch (kMaxReceiveNotes), and once a close command came 32 more of their own (azl/apt_sleep.hpp, AptNoteBudget): the lines after the
// close are the ones a game closed without its EXIT needs (REGISTRO, phase N), and a game that polls these used the 64 up long before.
AptNoteBudget g_receive_notes;
constexpr u32 kMaxReceiveNotes = AptNoteBudget::kNormal;
static_assert(kMaxReceiveNotes == 64, "the limit the trail's line says");

// (before the call: what the request says, for the line after it. Nothing is taken here: the line's room is taken in the post, and a
//  post that does not run - the plugin went away during the call - leaves nothing behind)
__attribute__((noinline)) u32 PassThroughAptNotedPre(u32 header, u32* ctx) {
    u32* cmd = getThreadCommandBuffer();
    u32 input = 0;
    if (header == kAptAppletUtility && cmd[2] >= 2 && cmd[5] != 0) {
        input = *reinterpret_cast<const u16*>(cmd[5]); // (the kernel would refuse the call with a bad pointer: the game's own buffer)
    }
    ctx[0] = header;
    ctx[1] = cmd[1];
    ctx[2] = input;
    return kActPostAptNoted;
}

__attribute__((noinline)) Result PassThroughAptNotedPost(Result rc, const u32* ctx) {
    u32* cmd = getThreadCommandBuffer();
    const u32 header = ctx[0];
    const u32 id = ctx[1];
    const u32 input = ctx[2];
    if (!g_apt_observer) {
        return rc;
    }
    const bool receive = header != kAptAppletUtility;
    const bool answered = R_SUCCEEDED(rc) && static_cast<s32>(cmd[1]) >= 0;
    const u32 command = receive && answered ? cmd[3] & 0xFFFF : 0;
    const u32 take = g_receive_notes.Take(AptCommandCloses(command)); // (a game that polls these must not use up the lines of the trail)
    if (take & AptNoteBudget::kNoteLimit) {
        g_apt_observer(kAptNoteLimit, 0);
    }
    if (take & AptNoteBudget::kNoteCloseLimit) {
        g_apt_observer(kAptNoteCloseLimit, 0);
    }
    if (take & AptNoteBudget::kNoteWrite) {
        if (!receive) {
            g_apt_observer(kAptNoteUtility, input << 16 | (id & 0xFFFF));
            g_apt_observer(kAptNoteUtilityResult, R_SUCCEEDED(rc) ? cmd[1] : static_cast<u32>(rc));
        } else {
            g_apt_observer(kAptNoteReceiveReply, answered ? cmd[2] << 16 | command : kAptReceiveFailed);
            // Luma3DS's word after the call (its kernel hook of this call raises HOME_EXIT at INHOME): the state the game is left in
            g_apt_observer(kAptNoteReceiveWord, LumaPluginWord());
        }
    }
    if (command == kAptCommandCloseFromHome && g_trail_waiter) {
        g_trail_waiter(); // (the close: its lines on the card before the game can be killed; bounded, ended by the gate's close, New 3DS only)
    }
    return rc;
}

// The chat's keyboard over the running game (Sources/ctr/chat_overlay.cpp) closes, its draft kept, on the notifications that take the game
// away (azl/chat_host.hpp, NoteAptNotification: HOME, power, sleep, close; the signal in the LOW BYTE of the word, which is where NS writes it:
// 0x608 is the power button on an Old 3DS). Here they are only counted, for the plugin's thread to see: at every level of the log (the lines
// below are the debug log's, the keyboard is not), out of line (the pass-through's own frame does not grow), no wait, no line, and the
// game's answer untouched. And the game's input page goes back here, BEFORE the game acts on the notification (ctr/input_mirror.hpp: an
// SDK that finalizes HID before an applet or the sleep unmaps the page, which fails with the mirror at its address); a try of the lock only.
static __attribute__((noinline)) void NoteAptForChat(Result rc, u32 result_word, u32 raw) {
    if (chat::NoteAptNotification(chat::g_signals, rc, result_word, raw)) {
        MirrorRestoreFromGame();
    }
}

// (The HOME notification is never taken away from the game: an SDK title's thread of the notifications panics on a notification 0 - Super
// Street Fighter IV 3D, svcBreak at 0x610948 from 0x60D5CC, 2026-11-04 - see AzaharLink/analysis/ssf4-crash-menu-2026-11-04.)

// (APT:ReceiveParameter(application) - the call at whose end Luma3DS raises the swap of the plugin's block on an Old 3DS - is a plain svc
//  here, or a svc and a post with the log on: nothing of the plugin is held across it)
__attribute__((noinline)) u32 PassThroughAptPre(u32* ctx) {
    u32* cmd = getThreadCommandBuffer();
    const u32 header = cmd[0];
    if (header != kAptInquireNotification && header != kAptReplySleepQuery &&
        header != kAptReplySleepNotificationComplete) {
        if (header == kAptReceiveParameter && cmd[1] == 0x300) {
            // The anchor of an Old 3DS's SWAP_ENTER deadline (ctr/swap_gate.hpp): Luma3DS raises its event at the end of this very call, and
            // waits for the framework's answer from there. One store at every debug level (the deadline is not a debug feature).
            __atomic_store_n(&g_apt_receive_tick, svcGetSystemTick(), __ATOMIC_RELAXED);
        }
        if (g_debug_level > 0 && g_apt_observer && (header == kAptAppletUtility || (header == kAptReceiveParameter && cmd[1] == 0x300))) {
            return PassThroughAptNotedPre(header, ctx);
        }
        return kActSvc;
    }
    const u32 reply = cmd[2]; // (ReplySleepQuery: the request word; the reply buffer replaces it)
    // The trail of a sleep (azl/sleep_crumbs.hpp): the game's two replies are noted BEFORE their call - the acknowledgement
    // of a sleep may not return from it for as long as the console sleeps, or ever.
    if (g_apt_observer && g_debug_level > 0 && header != kAptInquireNotification) {
        g_apt_observer(header == kAptReplySleepQuery ? kAptNoteReplyQuery : kAptNoteReplyComplete, reply);
    }
    ctx[0] = header;
    ctx[1] = cmd[1]; // (the app id)
    ctx[2] = reply;
    return kActPostApt;
}

__attribute__((noinline)) Result PassThroughAptPost(Result rc, const u32* ctx) {
    u32* cmd = getThreadCommandBuffer();
    const u32 header = ctx[0];
    const u32 app_id = ctx[1];
    const u32 reply = ctx[2];
    const Result result = R_SUCCEEDED(rc) ? cmd[1] : rc;
    static u32 lines = 0;
    if (header == kAptInquireNotification) {
        const u32 raw = cmd[2]; // after the call
        const u32 id = AptSignalOf(raw); // (the low byte: the bits above it change from call to call)
        if (id == 0) {
            return rc; // (polled all the time: only the notifications themselves are of interest)
        }
        const bool answered = R_SUCCEEDED(rc) && static_cast<s32>(cmd[1]) >= 0; // (a failed call leaves in cmd[2] what an earlier one left: with 8 bits that can look like a button)
        NoteAptForChat(rc, cmd[1], raw);
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

// The zombie probe (debug only): a control-lane line for an IPC reply that hands the game a handle - which service, which
// command, copy or move, and the handle's class token. It sees only srv's service opens (always a client session) and the
// REPLIES of the traced services (IsTracedService and APT): gsp::Gpu, soc:U, ir:USER and hid:USER are not traced, and a
// game-owned object a sysmodule keeps (gsp's RegisterInterruptRelayQueue event, soc's shared block) travels in a REQUEST,
// so this line can neither confirm nor exclude the gsp/soc/ir/hid leads: who holds the zombie is the census's to say
// (3DS Link, after the close). A handle the game receives dies with its table; the line maps what it was given.
// Control lane (a frame flood never drops it); the type read is a Luma svc, gated and never on the hot pass-through path
// (only service opens and traced commands reach here). No printf, no format: DebugLine only.
__attribute__((noinline)) void NoteIpcHandleXfer(const char* service, u32 command, const u32* reply) {
    if (g_debug_level <= 0) {
        return;
    }
    const zombie::HandleXfer x = zombie::DecodeReplyHandleXfer(reply, 64);
    if (!x.found) {
        return;
    }
    u32 type = 0xFFFFFFFFu;
    if (zombie::BackdoorOk()) {
        s64 flags = 0;
        if (R_SUCCEEDED(svcGetHandleInfo(&flags, x.handle, 0x10001))) { // 0x10001: the object's class token flags (Luma ext)
            type = static_cast<u32>(flags);
        }
    }
    DebugLine line;
    line.Str("ipc-xfer ").Str(service).Str(" cmd 0x").Hex(command, 4).Str(x.move ? " move handle 0x" : " copy handle 0x")
        .Hex(x.handle).Str(" type 0x").Hex(type, 2);
}

// srv:GetServiceHandle: which real services the game opens (and the traced ones are remembered by session). The name (8 bytes of the
// request, which the reply overwrites) is kept in ctx[0..1] by PassThroughPre; the table takes the session here, after the call, and
// only here: a post that does not run takes nothing.
// Out of line, with a frame of its own: the common path below does not pay for these locals.
__attribute__((noinline)) Result PassThroughGetServicePost(Result rc, const u32* ctx) {
    u32* cmd = getThreadCommandBuffer();
    char name[9] = {};
    std::memcpy(name, ctx, 8);
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
    if (cmd[0] == kSrvGetServiceHandleReply && cmd[2] == kIpcMoveOneHandle) {
        g_traced.Opened(session); // (a session closed out of the hook's sight frees its slot when its index comes back; only srv's own answer says a handle is new)
        NoteIpcHandleXfer(name, kSrvGetServiceHandle >> 16, cmd); // (debug only: the session handle the game just received)
    }
    const bool apt = IsAptService(name);
    if (session && (apt || IsTracedService(name))) {
        g_traced.Add(session, name, apt);
    }
    return rc;
}

// A command of a traced real service (ndm:u, ac:u, nwm::*, ptm:u, cfg:u, ...): followed for the log only. Before the call: the session,
// the command (and whether its line is worth writing: the top bit) and the first argument, by value. After it the session is looked up
// again by its handle (no pointer into the table is carried across the call: another thread may close the session meanwhile, and then
// there is no line).
constexpr u32 kTracedLogIt = 0x80000000u;

__attribute__((noinline)) u32 PassThroughTracedPre(Handle handle, u32* ctx) {
    if (g_debug_level <= 0) {
        return kActSvc;
    }
    u32* cmd = getThreadCommandBuffer();
    const u32 command = cmd[0] >> 16;
    ctx[0] = handle;
    ctx[1] = command | (WorthLogging(handle, command) ? kTracedLogIt : 0u);
    ctx[2] = cmd[1];
    return kActPostTraced;
}

__attribute__((noinline)) Result PassThroughTracedPost(Result rc, const u32* ctx) {
    const TracedSession* traced = FindTraced(ctx[0]);
    if (!traced) {
        return rc;
    }
    u32* cmd = getThreadCommandBuffer();
    const u32 command = ctx[1] & 0xFFFFu;
    const u32 arg0 = ctx[2];
    const bool log_it = (ctx[1] & kTracedLogIt) != 0;
    const u32 result = R_SUCCEEDED(rc) ? cmd[1] : static_cast<u32>(rc);
    if (log_it) {
        ++g_trace_lines;
        LogLine line;
        line.Str("svc: REAL ").Str(traced->name).Str(" command 0x").Hex(command, 4).Str(" (arg 0x").Hex(arg0)
            .Str(") -> 0x").Hex(result, 8);
    }
    DebugSvcReal(traced->name, command, arg0, result, ctx[0]); // (the session's handle: the diet counts its repeats)
    if (R_SUCCEEDED(rc)) {
        NoteIpcHandleXfer(traced->name, command, cmd); // (debug only: a handle a traced service returned, e.g. ndm's or APT's event)
    }
    if (result == kDlpWirelessOff && !g_dlp_fail_noted && std::strcmp(traced->name, "dlp:FKCL") == 0) {
        g_dlp_fail_noted = true;
        LogLine note;
        note.Str("dlp: dlp:FKCL command 0x").Hex(command, 4).Str(" answered 0xC940AC04 (the system module says "
                 "wireless is off): this game's local multiplayer needs the real radio and cannot use the tunnel");
    }
    return rc;
}

// A call of the game that failed for good, with the log on: what a dump does not say, written down before the game goes on to its error screen. One line of the trail (AzaharLink.sleep.log): the thread,
// the command buffer and the words of the stack that look like code: above the plugin's frames are the gate's return (stub + 8, in the gate's page) and the game's own lr. At most 8 invalid handles in a launch, 100 ms apart
// (a thread that insists fills the ring in microseconds, and the writes are slow); a fatal error always, and it gives the thread of the trail time to write it: what follows the call may stop the process.
constexpr u32 kKernelInvalidHandle = 0xD8E007F7;
constexpr u32 kErrfThrow = 0x00010800; // err:f ThrowFatalError: type, result, pc, process, title in the words after the header
constexpr u32 kFailureGapMs = 100;
u32 g_failures_noted = 0;
u32 g_failure_ms = 0;
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
    g_apt_observer(kAptNoteWord, GateClosing() ? 1u : 0u); // (0: the gate is open, the plugin is there; as the hooks' state of 1.1.2 was)
    g_apt_observer(kAptNoteWord, now - g_gate_last_open_ms);
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
    if (kind == kAptNoteFatal && !GateClosing()) { // (not while the swap waits for this thread to leave the plugin: it would wait 50 ms of it)
        svcSleepThread(50000000LL);
    }
}

// srv:ReceiveNotification, with the log on (this post runs only then): the termination request (0x100) the game got, for the trail. pm sends it
// when it terminates the application; when it cannot be sent, pm kills the process at once, with no EXIT (pm termination.c). A few lines a
// launch (a game that subscribed to other notifications gets them through here too: only 0x100 is noted). Out of line: PassThroughPost's frame
// is the one of the 464-byte thread of Tri Force Heroes, which is the very thread that makes this call.
constexpr u32 kSrvReceiveNotification = 0x000B0000;
constexpr u32 kSrvReceiveNotificationReply = 0x000B0080; // (result, the notification's id)
constexpr u32 kSrvNotificationTerminate = 0x100;
constexpr u32 kMaxSrvNotes = 4;
u32 g_srv_notes = 0;

__attribute__((noinline)) void NoteSrvNotification(Result rc, const u32* cmd) {
    if (R_SUCCEEDED(rc) && cmd[0] == kSrvReceiveNotificationReply && static_cast<s32>(cmd[1]) >= 0 && cmd[2] == kSrvNotificationTerminate &&
        g_apt_observer && g_srv_notes < kMaxSrvNotes) {
        ++g_srv_notes;
        g_apt_observer(kAptNoteSrvNotification, cmd[2]);
    }
}

// What every IPC call of every thread of the game that no service of ours answers goes through, in two halves: this one before the
// call, a post after it (azl_post32) when there is something to note. The common case - a call to a real service nobody traces - keeps
// this frame small on purpose: it is the one that runs on the srv-notification thread of Tri Force Heroes (464 bytes), and the branches
// with locals are out of line. Returns the handle for the svc in the low word and the action (kAct*) in the high one.
__attribute__((noinline)) u64 PassThroughPre(Handle handle, u32* ctx) {
    u32* cmd = getThreadCommandBuffer();
    const u32 header = cmd[0];
    if (header == kSrvGetServiceHandle) {
        ctx[0] = cmd[1]; // (the name, 8 bytes: the reply overwrites them)
        ctx[1] = cmd[2];
        return PreReply(handle, kActPostGetService);
    }
    TracedSession* traced = FindTraced(handle);
    if (traced) {
        if (traced->apt) {
            g_traced.Touch(*traced); // (a session the game keeps and uses is never the one a full table gives up)
            return PreReply(handle, PassThroughAptPre(ctx));
        }
        return PreReply(handle, PassThroughTracedPre(handle, ctx));
    }
    ++g_stats.pass_calls;
    if (g_debug_level <= 0) {
        return PreReply(handle, kActSvc);
    }
    // Log on: every IPC request of the game (raw words in the ring: no formatting on this thread, whatever its stack is).
    if (header == kErrfThrow) {
        NoteGameFailure(kAptNoteFatal, handle); // (before the call: it does not come back)
    }
    ctx[0] = handle;
    ctx[1] = header;
    ctx[2] = cmd[1];
    return PreReply(handle, kActPostIpc);
}

__attribute__((noinline)) Result PassThroughPost(Result rc, const u32* ctx) {
    u32* cmd = getThreadCommandBuffer();
    const Handle handle = ctx[0];
    const u32 header = ctx[1];
    const u32 arg = ctx[2];
    if (static_cast<u32>(rc) == kKernelInvalidHandle) {
        NoteGameFailure(kAptNoteBadHandle, handle);
    }
    if (header == kSrvReceiveNotification) {
        NoteSrvNotification(rc, cmd);
    }
    DebugIpc(handle, header, arg, R_SUCCEEDED(rc) ? cmd[1] : static_cast<u32>(rc));
    return rc;
}

// ---- pre and post: what the gate (Sources/ctr/gate.s) calls in the plugin's block, around the game's svc it makes itself ----
// pre(handle, stub+8, ctx) answers a call of ours (a fake session: kActAnswer, r0 the result) or says what the gate is to do; post(result,
// stub+8, ctx) runs after the svc, ctx[3] saying which post, and only while the plugin is there: a swap of the plugin's block during the svc
// skips it. So nothing that pre takes may wait for post to be given back (the table of the traced sessions takes in post only), and
// ctx[0..2] carry values only. stub+8 names the site (azl/gate_plan.hpp, SiteIndex); nothing here needs it yet.
extern "C" u64 azl_pre32(Handle handle, u32 stub_ret, u32* ctx) {
    (void)stub_ret;
    Result result;
    if (g_hle && g_hle->HandleSendSyncRequest(handle, &result)) {
        return PreReply(static_cast<u32>(result), kActAnswer);
    }
    return PassThroughPre(handle, ctx);
}

extern "C" Result azl_post32(Result result, u32 stub_ret, u32* ctx) {
    (void)stub_ret;
    switch (ctx[3]) {
    case kActPostIpc:
        return PassThroughPost(result, ctx);
    case kActPostGetService:
        return PassThroughGetServicePost(result, ctx);
    case kActPostTraced:
        return PassThroughTracedPost(result, ctx);
    case kActPostApt:
        return PassThroughAptPost(result, ctx);
    case kActPostAptNoted:
        return PassThroughAptNotedPost(result, ctx);
    default:
        return result;
    }
}

// CloseHandle: the fake sessions and the table forget the handle BEFORE the call (taking out in pre is fine: nothing waits for a post).
extern "C" u64 azl_pre23(Handle handle, u32 stub_ret, u32* ctx) {
    (void)stub_ret;
    (void)ctx;
    if (g_hle) {
        g_hle->NoteHandleClosed(handle);
    }
    g_traced.Closed(handle); // the value may be reused for another object
    return PreReply(handle, kActSvc);
}

extern "C" Result azl_post23(Result result, u32 stub_ret, u32* ctx) {
    (void)stub_ret;
    (void)ctx;
    return result; // (never asked for: azl_pre23 says kActSvc)
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

HookStats InstallSvcHooks(UdsHle* hle, bool reserve_exit_tramp) {
    HookStats stats;
    g_hle = hle;
    g_site_count = 0;
    g_gate = nullptr;

    stats.text_start = kGameTextBase;
    stats.text_size = CTRPluginFramework::Process::GetTextSize();
    if (stats.text_size < 8) {
        Log("hook: could not determine the size of the game's .text");
        return stats;
    }
    // (Nothing is checked with svcQueryMemory for the game's text: Luma3DS makes the whole process RWX in the MMU
    //  (PROCESSOP_SET_MMU_TO_RWX) while the kernel's logical permissions stay RW, so the query reports "not executable" for memory that
    //  does execute: it blocked every hook on hardware once. The gate's page says what the kernel says of it in its line, and no more.)
    const u32 first = stats.text_start;
    const u32 end = stats.text_start + stats.text_size;

    // (A game whose GSP wrapper has its svc behind a stub has been dealt with already, by RewriteStubbedGspWrappers(): it runs from Plugin::EarlyInit
    //  whether the plugin is on or off, before this, so that the search for the framework's sites below finds the wrapper as the framework does.)
    stats.gsp_stub_wrappers = g_gsp_stub_wrappers;

    // The framework hooks one of the game's svc 0x32 words by itself: do not touch that one.
    g_framework_site_count = FindFrameworkSites(ReadGameWord, nullptr, first, end, g_framework_sites, kMaxFrameworkSites);

    // The sites: every ARM svc 0x32 / 0x23 of the text (nothing in the game is written yet). One word each: the instruction after the svc
    // stays the game's own, wherever it is and whatever it is.
    u32 found = 0;
    for (u32 addr = first; addr + 4 <= end; addr += 4) {
        const u32 word = *reinterpret_cast<const u32*>(addr);
        bool framework_site = false;
        if (!IsHookSite(word, addr, g_framework_sites, g_framework_site_count, &framework_site)) {
            if (framework_site) {
                ++stats.framework_sites;
                Log("hook: svc at 0x%08lx left to CTRPluginFramework (it hooks GSPGPU::RegisterInterruptRelayQueue)",
                    static_cast<unsigned long>(addr));
            }
            continue;
        }
        ++found;
        if (g_site_count < kMaxSites) {
            g_sites[g_site_count++] = {addr, word};
        }
    }
    // Thumb call sites are only counted (a Thumb "svc 0x32" is the halfword 0xDF32).
    for (u32 addr = first; addr + 2 <= end; addr += 2) {
        if (*reinterpret_cast<const u16*>(addr) == 0xDF32) {
            ++stats.thumb_candidates;
        }
    }

    // The gate's page, then the gate, the stubs and the sites (azl/gate_plan.hpp: WriteGate, the function the host tests run).
    const GateBlob blob{azl_gate_begin, static_cast<u32>(reinterpret_cast<u32>(azl_gate_end) - reinterpret_cast<u32>(azl_gate_begin)),
                        static_cast<u32>(reinterpret_cast<u32>(azl_gate_32) - reinterpret_cast<u32>(azl_gate_begin)),
                        static_cast<u32>(reinterpret_cast<u32>(azl_gate_23) - reinterpret_cast<u32>(azl_gate_begin))};
    GateWriteResult written;
    if (g_site_count != 0) {
        const u32 bytes = PageCount(g_site_count + (reserve_exit_tramp ? kExitTrampStubs : 0), blob.bytes) * kGatePageBytes; // (+ the exit guard's trampoline, InstallExitGuard: a New 3DS only)
        g_gate_page = TakeGatePage(g_sites[0].address, g_sites[g_site_count - 1].address, bytes, &stats.no_gate);
        if (g_gate_page.address != 0) {
            const GatePointers pointers{reinterpret_cast<u32>(&azl_pre32), reinterpret_cast<u32>(&azl_post32), reinterpret_cast<u32>(&azl_pre23),
                                        reinterpret_cast<u32>(&azl_post23)};
            GateTextMemory memory{g_gate_page.address, g_gate_page.bytes};
            written = WriteGate(memory, g_gate_page.address, g_gate_page.bytes, blob, pointers, g_sites, g_site_count);
            if (!written.ok) {
                stats.no_gate = "the gate could not be written into its page";
                Log("gate: no hooks: the gate (%lu bytes, entries +0x%lx and +0x%lx) does not fit its page of %lu bytes",
                    static_cast<unsigned long>(blob.bytes), static_cast<unsigned long>(blob.entry32), static_cast<unsigned long>(blob.entry23),
                    static_cast<unsigned long>(g_gate_page.bytes));
            }
        }
    }
    if (written.ok) {
        g_gate = reinterpret_cast<GateHeader*>(g_gate_page.address);
        g_stubs_base = StubsBase(g_gate_page.address, blob.bytes);
        g_gate_flag = &g_gate->flag; // (WriteGate opened it: from now on the waits on a game thread look at the swap's flag)
        g_gate_last_open_ms = static_cast<u32>(platform::NowMs());
        stats.gate_ready = true;
        // The sites WriteGate hooked are the first `hooked`, in the order of their stubs; one whose word could not be written is not one.
        u32 hooked = 0;
        for (u32 i = 0; i < written.hooked; ++i) {
            u32 jump = 0;
            if (EncodeBranch(g_sites[i].address, g_stubs_base + i * kGateStubBytes, &jump) && *reinterpret_cast<const u32*>(g_sites[i].address) == jump) {
                ++hooked;
                if (g_sites[i].original == kSvcSendSyncRequest) {
                    ++stats.send_sync_sites;
                } else {
                    ++stats.close_sites;
                }
            }
        }
        g_site_count = written.hooked;
        stats.skipped_sites = found - hooked;
        if (written.out_of_reach != 0 || written.foreign != 0 || written.no_room != 0 || written.patch_failed != 0 || found > kMaxSites) {
            Log("gate: sites left as they are: %lu out of reach, %lu changed by someone else, %lu with no room in the page, %lu not written, "
                "%lu over the %lu the plugin keeps",
                static_cast<unsigned long>(written.out_of_reach), static_cast<unsigned long>(written.foreign),
                static_cast<unsigned long>(written.no_room), static_cast<unsigned long>(written.patch_failed),
                static_cast<unsigned long>(found > kMaxSites ? found - kMaxSites : 0), static_cast<unsigned long>(kMaxSites));
        }
    } else {
        g_site_count = 0;
        stats.skipped_sites = found;
        if (found != 0 && stats.no_gate == nullptr) {
            stats.no_gate = "the gate could not be written into its page";
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

void SettleGatePage() {
    SettleGateBooks(g_gate_page);
}

namespace {

constexpr s64 kGateStepNs = 100000;          // between two looks at the count of the game's threads in the gate
constexpr s64 kGateDrainStepNs = 1000000;    // ... in the second drain, which can last seconds on the keep-alive thread

} // namespace

u32 CloseGate(u32 timeout_ms, void (*wake)(void*), void* arg) {
    if (g_gate == nullptr) {
        return 0;
    }
    bool woken = false;
    const u32 budget_ms = SwapClampMs(timeout_ms); // (the one deadline of SWAP_ENTER, ctr/swap_gate.hpp)
    const u32 left = GateClose(
        *g_gate, static_cast<u64>(budget_ms) * 1000, []() { return platform::NowUs(); },
        [&]() {
            if (!woken && wake != nullptr) { // (a thread counted in the gate that sleeps in slices: its slice ends now)
                woken = true;
                wake(arg);
            }
            svcSleepThread(kGateStepNs);
        },
        &g_gate_stats);
    if (budget_ms < timeout_ms && left != 0) {
        NoteSwapWaitCut(4); // (the deadline ended it with a game's thread still inside)
    }
    return left;
}

u32 WaitGateDrained() {
    // (reads the count only: the flag stays as CloseGate left it, closed. A game's thread still counted when the plugin's block goes wakes in
    //  unmapped memory, a certain crash, so what is left of the deadline is better spent here than returned to Luma3DS early.)
    if (g_gate == nullptr) {
        return 0;
    }
    u32 left = __atomic_load_n(&g_gate->in_flight, __ATOMIC_SEQ_CST);
    while (left != 0 && SwapTicksLeft() > 0) {
        svcSleepThread(kGateDrainStepNs);
        left = __atomic_load_n(&g_gate->in_flight, __ATOMIC_SEQ_CST);
    }
    if (left != 0) {
        NoteSwapWaitCut(16);
    }
    return left;
}

// ---------------------------------------------------------------------------------------------
// Luma3DS's plugin word through its uncached alias, and the exit guard (azl/exit_guard.hpp, Sources/ctr/exit_tramp.s).
// ---------------------------------------------------------------------------------------------

const volatile u32* g_luma_word_ptr = reinterpret_cast<const volatile u32*>(kLumaPluginWordAddress);
namespace {
volatile u32* g_luma_word_alias = nullptr;
} // namespace

bool SetupLumaWordAlias() {
    s64 emulator = 0;
    if (svcGetSystemInfo(&emulator, 0x20000, 0) == 0) {
        return false; // (Citra's and Azahar's information type: no Luma3DS, no alias)
    }
    u32 pa = 0;
    if (svcControlProcess(CUR_PROCESS_HANDLE, PROCESSOP_GET_PA_FROM_VA, reinterpret_cast<u32>(&pa), kLumaPluginWordAddress) != 0 || pa == 0 ||
        pa >= 0x30000000) {
        return false; // (Luma's kernel maps PA 0..0x30000000 at PA | 0x80000000: k11_extension main.c)
    }
    g_luma_word_alias = reinterpret_cast<volatile u32*>(pa | 0x80000000u);
    g_luma_word_ptr = g_luma_word_alias;
    return true;
}

volatile u32* LumaWordAliasForWrite() {
    return g_luma_word_alias;
}

ExitGuardStats InstallExitGuard(void (*guard)(), u32 stack_top) {
    ExitGuardStats stats;
    const u32 text = kGameTextBase;
    const u32 text_size = CTRPluginFramework::Process::GetTextSize();
    if (g_gate == nullptr || g_gate_page.address == 0) {
        stats.why_not = "no gate page";
        return stats;
    }
    if (g_luma_word_alias == nullptr) {
        stats.why_not = "no uncached alias of Luma's word";
        return stats;
    }
    // The trampoline's copy: in the tail of the gate's page, after the stubs (InstallSvcHooks reserved kExitTrampStubs stubs' worth of it).
    const u32 tramp_bytes = static_cast<u32>(reinterpret_cast<u32>(azl_exit_tramp_end) - reinterpret_cast<u32>(azl_exit_tramp_begin));
    const u32 tramp = (g_stubs_base + g_site_count * kGateStubBytes + 7) & ~7u;
    if (tramp_bytes == 0 || tramp_bytes > kExitTrampStubs * kGateStubBytes - 8 || tramp + tramp_bytes > g_gate_page.address + g_gate_page.bytes) {
        stats.why_not = "no room for the trampoline in the gate's page";
        return stats;
    }
    const u32 off_fn = static_cast<u32>(reinterpret_cast<u32>(azl_exit_tramp_fn) - reinterpret_cast<u32>(azl_exit_tramp_begin));
    const u32 off_stack = static_cast<u32>(reinterpret_cast<u32>(azl_exit_tramp_stack) - reinterpret_cast<u32>(azl_exit_tramp_begin));
    const u32 off_state = static_cast<u32>(reinterpret_cast<u32>(azl_exit_tramp_state) - reinterpret_cast<u32>(azl_exit_tramp_begin));
    // (the sites first, counted, so that a game with none leaves the page as it was)
    for (u32 addr = text; addr + 4 <= text + text_size; addr += 4) {
        if (*reinterpret_cast<const u32*>(addr) == kSvcExitProcess) {
            ++stats.sites;
        }
    }
    for (u32 addr = text; addr + 2 <= text + text_size; addr += 2) {
        if (*reinterpret_cast<const u16*>(addr) == 0xDF03) {
            ++stats.thumb;
        }
    }
    if (stats.sites == 0) {
        stats.why_not = "no ARM svc 0x03 in the game's text";
        return stats;
    }
    const u32* src = reinterpret_cast<const u32*>(azl_exit_tramp_begin);
    for (u32 i = 0; i < tramp_bytes / 4; ++i) {
        *reinterpret_cast<volatile u32*>(tramp + 4 * i) = src[i];
    }
    *reinterpret_cast<volatile u32*>(tramp + off_state) = 0;
    *reinterpret_cast<volatile u32*>(tramp + off_stack) = stack_top & ~7u;
    *reinterpret_cast<volatile u32*>(tramp + off_fn) = reinterpret_cast<u32>(guard);
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, tramp, tramp_bytes);
    svcInvalidateEntireInstructionCache();
    stats.tramp = tramp;
    for (u32 addr = text; addr + 4 <= text + text_size; addr += 4) {
        if (*reinterpret_cast<const u32*>(addr) != kSvcExitProcess) {
            continue;
        }
        u32 jump = 0;
        if (EncodeBranch(addr, tramp, &jump) && CTRPluginFramework::Process::Patch(addr, jump)) {
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, addr, 4);
            ++stats.hooked;
        }
    }
    svcInvalidateEntireInstructionCache();
    return stats;
}

u64 LastAptReceiveTick() {
    return __atomic_load_n(&g_apt_receive_tick, __ATOMIC_RELAXED);
}

namespace {

// A slot marked closed whose handle is still alive (the gate marks before the svc: a game's thread of low priority may not have made it yet;
// or the close was refused) is looked at again on every Plugin::Tick for kFakeEveryTick calls, then on one in kFakeSlowEvery, for as long
// as the slot holds the same handle: never dropped while it could still be the closed one.
constexpr u32 kFakeEveryTick = 30;
constexpr u32 kFakeSlowEvery = 32;
u32 g_fake_full = 0;               // fake sessions that found the gate's table full
u32 g_fake_settled = 0;            // fake sessions closed with the flag 0 and forgotten once back
u32 g_fake_pending = 0;            // bits of fake_closed taken whose handle was still alive (Plugin::Tick's thread only)
u32 g_fake_looks[kGateFakeSlots] = {};
Handle g_fake_seen[kGateFakeSlots] = {}; // the handle the pending bit was for

} // namespace

int GateFakeAdd(Handle handle, u32 result) {
    if (g_gate == nullptr || handle == 0) {
        return 0;
    }
    for (u32 i = 0; i < kGateFakeSlots; ++i) {
        GateHeader::FakeSlot& slot = g_gate->fake[i];
        if (__atomic_load_n(&slot.handle, __ATOMIC_RELAXED) == 0) {
            __atomic_store_n(&slot.result, result, __ATOMIC_RELAXED);
            __atomic_store_n(&slot.handle, handle, __ATOMIC_RELEASE); // (the result is there before the gate can match the handle)
            return 1;
        }
    }
    // Full: a slot whose session is gone the hook did not see close (a Thumb call, a site left out) is freed for the new one. (Under
    // UdsHle's lock like every writer of the table; UdsHle forgets its own entry the next time a request finds the handle dead.)
    for (u32 i = 0; i < kGateFakeSlots; ++i) {
        GateHeader::FakeSlot& slot = g_gate->fake[i];
        const Handle old = __atomic_load_n(&slot.handle, __ATOMIC_RELAXED);
        if (old != 0 && R_FAILED(svcClearEvent(old))) {
            __atomic_store_n(&slot.handle, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&slot.result, result, __ATOMIC_RELAXED);
            __atomic_store_n(&slot.handle, handle, __ATOMIC_RELEASE);
            return 1;
        }
    }
    __atomic_add_fetch(&g_fake_full, 1, __ATOMIC_RELAXED);
    return -1;
}

void GateFakeRemove(Handle handle) {
    if (g_gate == nullptr || handle == 0) {
        return;
    }
    for (u32 i = 0; i < kGateFakeSlots; ++i) {
        if (__atomic_load_n(&g_gate->fake[i].handle, __ATOMIC_RELAXED) == handle) {
            __atomic_store_n(&g_gate->fake[i].handle, 0u, __ATOMIC_RELEASE);
        }
    }
}

void SettleGateFakeClosed() {
    if (g_gate == nullptr) {
        return;
    }
    // (the gate only sets bits, the slots are the plugin's: a bit is "look at this one", the kernel says whether the handle is gone)
    const u32 bits = __atomic_exchange_n(&g_gate->fake_closed, 0u, __ATOMIC_ACQ_REL) | g_fake_pending;
    if (bits == 0) {
        return;
    }
    u32 pending = 0;
    for (u32 i = 0; i < kGateFakeSlots; ++i) {
        if ((bits & (1u << i)) == 0) {
            continue;
        }
        const Handle handle = __atomic_load_n(&g_gate->fake[i].handle, __ATOMIC_ACQUIRE);
        const bool fresh = (g_fake_pending & (1u << i)) == 0 || g_fake_seen[i] != handle; // (a new bit, or the slot holds another session now)
        if (fresh) {
            g_fake_seen[i] = handle;
            g_fake_looks[i] = 0;
        }
        if (handle == 0) {
            continue; // (forgotten already: the game's close went through the hook, or a request found it dead)
        }
        const u32 look = g_fake_looks[i]++;
        if (look >= kFakeEveryTick && look % kFakeSlowEvery != 0) {
            pending |= 1u << i; // (not this time)
            continue;
        }
        if (R_FAILED(svcClearEvent(handle))) {
            ++g_fake_settled;
            Log("gate: the game closed its fake session 0x%08lx while the gate was shut (swap): forgotten now",
                static_cast<unsigned long>(handle));
            if (g_hle) {
                g_hle->NoteHandleClosed(handle); // (forgets it, and its slot here: GateFakeRemove)
            }
            g_traced.Closed(handle);
        } else {
            pending |= 1u << i; // still alive: looked at again while the slot holds it
        }
    }
    g_fake_pending = pending;
}

void OpenGate() {
    if (g_gate == nullptr) {
        return;
    }
    GateOpen(*g_gate, &g_gate_stats);
    g_gate_last_open_ms = static_cast<u32>(platform::NowMs());
}

GateStats GetGateStats() {
    GateStats stats;
    stats.gate = g_gate_stats;
    if (g_gate != nullptr) {
        stats.gate.post_skipped = __atomic_load_n(&g_gate->post_skipped, __ATOMIC_RELAXED);
        const u32 index = SiteIndex(stats.gate.last_site, g_stubs_base, g_site_count);
        stats.last_site_address = index == kNoGateSite ? 0 : g_sites[index].address;
    }
    stats.traced_stale = __atomic_load_n(&g_traced.stats.stale, __ATOMIC_RELAXED);
    stats.traced_evicted = __atomic_load_n(&g_traced.stats.evicted, __ATOMIC_RELAXED);
    stats.traced_dropped = __atomic_load_n(&g_traced.stats.dropped, __ATOMIC_RELAXED);
    if (g_gate != nullptr) {
        for (u32 i = 0; i < kGateFakeSlots; ++i) {
            stats.fake_in_table += __atomic_load_n(&g_gate->fake[i].handle, __ATOMIC_RELAXED) != 0 ? 1 : 0;
        }
        stats.fake_hits = __atomic_load_n(&g_gate->fake_hits, __ATOMIC_RELAXED);
    }
    stats.fake_settled = g_fake_settled;
    stats.fake_full = __atomic_load_n(&g_fake_full, __ATOMIC_RELAXED);
    return stats;
}

void ReportForeignPatches() {
    u32 foreign = 0;
    for (u32 i = 0; i < g_site_count; ++i) {
        const u32 now = *reinterpret_cast<const u32*>(g_sites[i].address);
        u32 jump = 0;
        if (EncodeBranch(g_sites[i].address, g_stubs_base + i * kGateStubBytes, &jump) && now == jump) {
            continue;
        }
        if (foreign++ < 6) {
            Log("hook: site 0x%08lx was changed by someone else after we patched it (now 0x%08lx)",
                static_cast<unsigned long>(g_sites[i].address), static_cast<unsigned long>(now));
        }
    }
    if (foreign) {
        Log("hook: WARNING %lu site(s) are no longer ours: whatever is there now runs instead of the gate",
            static_cast<unsigned long>(foreign));
    }
}

} // namespace ctr
} // namespace azl
