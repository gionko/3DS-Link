// The chat over the running game (ctr/chat_overlay.hpp): the console's side of azl/chat_host.hpp. Two threads, nothing shared but the
// Overlay block below and the chat's own block (azl/chat_core.hpp): the plugin's thread (ChatFrame, from OnFrame) does all the work, the
// game's thread (OnChatOsd, the framework's OSD callback, before the game's own present) only copies.
#include "ctr/chat_overlay.hpp"

#include <3ds.h>

#include <CTRPluginFramework.hpp>

#include <new>

#include "azl/chat_host.hpp"
#include "azl/log.hpp"
#include "azl/osk.hpp"
#include "azl/osk_draw.hpp"
#include "azl/platform.hpp"
#include "azl/stats.hpp"
#include "azl/swap_gate.hpp"
#include "ctr/input_mirror.hpp"
#include "ctr/plugin.hpp"
#include "ctr/zombie_probe.hpp"

namespace azl {
namespace chat {
Signals g_signals; // (counted by the svc hook and the framework's events, read by ChatFrame)
} // namespace chat

namespace ctr {

namespace {

using CTRPluginFramework::Controller;
using CTRPluginFramework::Key;
using CTRPluginFramework::OSD;
using CTRPluginFramework::Process;
using CTRPluginFramework::Screen;
using CTRPluginFramework::Touch;

static_assert(static_cast<int>(Process::Event::EXIT) == chat::kEventExit && static_cast<int>(Process::Event::SLEEP_ENTER) == chat::kEventSleepEnter &&
                  static_cast<int>(Process::Event::HOME_ENTER) == chat::kEventHomeEnter &&
                  static_cast<int>(Process::Event::SWAP_ENTER) == chat::kEventSwapEnter && static_cast<int>(Process::Event::SWAP_EXIT) == chat::kEventSwapExit,
              "the framework's process events are the numbers the chat counts");
static_assert(osk::kKeyTouch == static_cast<u32>(Key::Touchpad), "the keyboard's touch bit is the framework's");

constexpr u64 kWorkGapTicks = chat::WorkGap(SYSCLOCK_ARM11); // (33.4 ms: two frames of the 3DS's 59.83 Hz are 33.43, a job every other frame)
constexpr u64 kWorkEarlyTicks = chat::WorkEarly(SYSCLOCK_ARM11); // (2 ms: the time of the frame callback moves, the pace takes it)
constexpr u32 kCostLogMs = 10000;                                    // the debug log's line of the costs, at most this often
constexpr u32 kShowWaitMs = 1000; // an opened keyboard not on the screen by then (the game never presented its bottom screen) is closed

// What the game's thread reads: a static, so it is there for as long as the plugin is (the OSD callback, once registered, is never taken
// away). The Image it points to is the one thing that comes and goes, through the slot's handshake.
chat::Overlay g_overlay;
chat::Cost g_cost_top;      // the callback with the lines drawn on the top screen
chat::Cost g_cost_bottom;   // ... with the keyboard copied on the bottom one
chat::Cost g_cost_redraw;   // the Image drawn again, on the plugin's thread

// The plugin's thread's own.
bool g_osd_running = false;            // the callback is registered (once: the chat on for the first time)
osk::Osk* g_osk = nullptr;             // the keyboard, only while it is open
char g_draft[chat::kTextMax + 1] = {}; // what was typed and not sent: kept when the keyboard closes, until it is sent
chat::Image* g_image = nullptr;        // the Image published in the slot (ours to free, through the keeper)
chat::ImageKeeper g_keeper;
osk::View g_drawn;                     // what the Image shows
bool g_drawn_valid = false;
chat::CloseRules g_rules;
chat::WorkPacer g_pacer(kWorkGapTicks, kWorkEarlyTicks);
chat::PadLatch g_latch;
chat::ChordEdge g_chord;
bool g_shown = false;                  // this opening's Image was published: the game's input is the mirror's from then on
u32 g_opened_ms = 0;
u32 g_cost_logged_ms = 0;
u32 g_counted_logged = 0; // the sum of the chat's counters at the last line (they only grow: a new sum, a counter moved)
chat::PostWatch g_watch;  // our last message, until the network thread sent it or refused it

// >>> game-thread code (Tests/policy_check.sh: the framework's OSD callback runs on the game's thread that presents a frame; stack 64 bytes
// at most, Tests/stack_usage.sh; no heap, no wait, no line of the log)
// The framework's screen as the plugin's drawing takes it (azl/gfx_surface.hpp): the right image only on the top screen with the 3D on;
// the bytes of a pixel from the masked format, never the framework's own (worked out of the game's byte unmasked: 3 for 0x40 | RGBA8).
inline gfx::Surface SurfaceOf(const Screen& scr) {
    return gfx::ScreenSurface(reinterpret_cast<u8*>(scr.LeftFramebuffer),
                              scr.IsTop && scr.Is3DEnabled ? reinterpret_cast<u8*>(scr.RightFramebuffer) : nullptr, scr.Stride,
                              static_cast<u32>(scr.Format), static_cast<u16>(scr.IsTop ? 400 : 320), 240);
}

// Once per screen and per present of the game, under the framework's OSD lock. First the swap's gate (an Old 3DS): closed, the plugin's block
// is about to go and nothing of it is touched; this thread is the game's, so it never stops there (GateTryPass, never GateEnter). Then out
// at once when the chat is off (turned off after an on: the callback stays registered, it reads no clock, no svc) or there is nothing to draw.
bool OnChatOsd(const Screen& scr) {
    if (!GateTryPass(g_swap_gate, kGateOsd)) {
        return false;
    }
    u32 drew = chat::kDrewNothing;
    if (g_overlay.on.load(std::memory_order_relaxed) != 0) {
        const bool timed = g_debug_level > 0;
        const u64 start = timed ? svcGetSystemTick() : 0;
        drew = chat::DrawOverlay(g_overlay, SurfaceOf(scr), scr.IsTop, static_cast<u32>(platform::NowMs()));
        if (timed && drew != chat::kDrewNothing) {
            const u32 ticks = static_cast<u32>(svcGetSystemTick() - start);
            (drew == chat::kDrewKeyboard ? g_cost_bottom : g_cost_top).Add(ticks);
            StatsNoteChatOsd(ticks); // (the stats line's total: the frame callback does not hold this time)
        }
    }
    GateLeave(g_swap_gate, kGateOsd);
    return drew != chat::kDrewNothing;
}
// <<< game-thread code

// The chat's own notifications: in the corner opposite the chat when its callback draws there (ChatNotice), else the framework's.
void Notify(const char* text) {
    if (ChatNotice(text)) {
        return;
    }
    OSD::Notify(text);
    StatsNoteChatOsdNotify(); // (how often the chat wakes CTRPF's notification workers: the zombie probe's ~10% lead)
    g_overlay.notices.Note(static_cast<u32>(platform::NowMs()));
}

// Our last message refused by the network thread (g_watch, every frame and right before a new Send): the player told why, and its text
// back where it can be sent again - the open keyboard if nothing is typed in it, else the draft if no newer one is kept.
void TellRefused(ChatSink* sink) {
    if (!g_watch.Refused(*sink)) {
        return;
    }
    if (g_osk) {
        if (g_osk->length() == 0) {
            g_osk->SetText(sink->Outbox());
        }
    } else if (g_draft[0] == '\0') {
        std::memcpy(g_draft, sink->Outbox(), sizeof g_draft);
    }
    const bool left = g_watch.NotInRoom();
    Log("chat: our message was not sent (%s)", left ? "not in the room when its turn came" : "the room's connection did not take it");
    Notify(left ? "Message not sent: not in the room" : "Message not sent: the room did not take it");
}

// The Image again, when what the keyboard shows changed (or `force`: a new Image). On the plugin's thread, at most kWorkPerSecond times a
// second: the game's thread may be copying it meanwhile (azl/chat_host.hpp: a frame of mixed pixels at worst). Only what changed is drawn
// again (osk::DrawOskOver: the blink one line, a key typed a line, the bar and a key or two): this thread is at the game's own priority on
// the Old 3DS's core 0, and the whole screen (1.5-2 million instructions, 5.6-7.4 ms at 268 MHz; the blink 62-105 K) would keep a game's
// thread woken meanwhile waiting. The whole screen only for a new Image or a new keyboard (g_drawn_valid false) and for the other page.
void Redraw(bool force) {
    if (!g_osk || !g_image) {
        return;
    }
    osk::View v;
    g_osk->Snapshot(&v);
    if (!force && g_drawn_valid && osk::Same(v, g_drawn)) {
        return;
    }
    const bool timed = g_debug_level > 0; // (the time only with the log on, as OnChatOsd: at debug=0 no svc)
    const u64 start = timed ? svcGetSystemTick() : 0;
    if (force || !g_drawn_valid) {
        osk::DrawOsk(chat::ImageSurface(*g_image), v);
    } else {
        osk::DrawOskOver(chat::ImageSurface(*g_image), v, g_drawn); // (the Image shows g_drawn: only Redraw draws it)
    }
    g_drawn = v;
    g_drawn_valid = true;
    if (timed) {
        g_cost_redraw.Add(static_cast<u32>(svcGetSystemTick() - start));
    }
}

void FreeImage(chat::Image* im) {
    if (im) {
        delete[] im->pixels;
        delete im;
    }
}

// An Image in the format the game's thread saw on the bottom screen, if there is none in it yet (or the game changed its format). The old
// Image is freed before a new one is made, at once when no copy reads it, else at a next frame (the heap never holds two: 230-307 KB more
// on the Old 3DS's 5 MiB block while the heap is the fullest - review of the chat, defect 35). The first one published hands the game's
// input to the mirror: never before the keyboard can be seen (else the game's keys gone, typed blind into the draft). What to tell the user
// when no Image can be made (no memory, or a format the keyboard does not draw in): the keyboard is then closed, no new try every frame;
// nullptr otherwise (made, or still waiting).
const char* EnsureImage() {
    const u32 geometry = g_overlay.bottom_geometry.load(std::memory_order_acquire);
    if (geometry == 0) {
        return nullptr; // (the game has not presented its bottom screen since the keyboard first opened: Type waits kShowWaitMs)
    }
    const u8 format = chat::GeometryFormat(geometry), bpp = chat::GeometryBpp(geometry);
    if (g_image && g_image->format == format && g_image->bpp == bpp) {
        return nullptr;
    }
    if (gfx::BytesPerPixel(format) != bpp || bpp == 0) {
        Log("chat: the bottom screen's format %u (%u bytes a pixel) is not one the keyboard draws in: it is closed",
            static_cast<unsigned>(format), static_cast<unsigned>(bpp));
        return "The keyboard can't be shown in this game";
    }
    if (!g_keeper.Empty()) {
        return nullptr;
    }
    if (g_image) {
        g_keeper.Retire(g_overlay.image); // (the slot shows g_image: it waits there until no copy reads it)
        g_image = nullptr;
        chat::Image* old = g_keeper.Freeable(g_overlay.image);
        FreeImage(old);
        if (old == nullptr) {
            return nullptr; // (a copy still reads it: ChatFrame frees it, the new one is made at a next frame)
        }
    }
    chat::Image* im = new (std::nothrow) chat::Image();
    u8* pixels = im ? new (std::nothrow) u8[chat::ImageBytes(format)] : nullptr;
    if (!pixels) {
        delete im;
        Log("chat: no memory for the keyboard's picture (%lu bytes): it is closed", static_cast<unsigned long>(chat::ImageBytes(format)));
        return "Not enough memory for the keyboard";
    }
    im->pixels = pixels;
    im->format = format;
    im->bpp = bpp;
    im->col_bytes = chat::kImageH * bpp;
    g_image = im;
    Redraw(true);
    g_overlay.image.Publish(im);
    if (!g_shown) {
        g_shown = true;
        MirrorWant(); // (the game's input becomes the mirror's at the end of this frame's work)
    }
    return nullptr;
}

// The keyboard closes: the draft kept (`keep`; Send no longer closes it: it empties it), the game's thread told first, the Image withdrawn (freed once no copy
// can read it), the keyboard deleted. The game's input page goes back once the keys are let go (`at_once`: now - the game is going away,
// the room was left or the chat is off).
void Close(bool keep, const char* why, bool at_once = false) {
    if (!g_osk) {
        return;
    }
    if (keep) {
        const u32 n = g_osk->length() < chat::kTextMax ? g_osk->length() : chat::kTextMax;
        for (u32 i = 0; i < n; ++i) {
            g_draft[i] = g_osk->text()[i];
        }
        g_draft[n] = '\0';
    } else {
        g_draft[0] = '\0';
    }
    g_overlay.keyboard.store(0, std::memory_order_release);
    if (g_image && g_keeper.Retire(g_overlay.image)) {
        g_image = nullptr;
    }
    delete g_osk;
    g_osk = nullptr;
    g_drawn_valid = false;
    Log("chat: keyboard closed (%s)%s", why, keep && g_draft[0] ? ", the draft kept" : "");
    // The zombie probe (debug only): what the game's process reference count and thread states are at the keyboard's
    // close, to compare with the clean closes (the keyboard path is the one the stuck sessions share).
    zombie::GameRefcount("keyboard close");
    zombie::ThreadSchedDump("keyboard close");
    if (at_once) {
        MirrorRestoreNow(why);
    } else {
        MirrorRelease(static_cast<u32>(platform::NowMs()));
    }
}

void Open(Plugin& plugin, u32 now_ms) {
    if (!g_rules.MayOpen(now_ms)) {
        return; // (a HOME, the power button or a swap just now: the game is going away, the keyboard would be closed at once)
    }
    if (plugin.room().state() != RoomClient::State::Joined) {
        Notify("Join a room to chat");
        return;
    }
    osk::Options o;
    o.max_len = chat::kTextMax;
    o.title = "Chat";
    o.submit = "Send";
    o.cancel = "Close"; // (Send keeps the keyboard open: this key, SELECT, or B with nothing typed close it)
    g_osk = new (std::nothrow) osk::Osk(o, g_draft);
    if (!g_osk) {
        Log("chat: no memory for the keyboard");
        return;
    }
    g_latch = chat::PadLatch();
    g_pacer.Reset();
    g_drawn_valid = false;
    g_shown = false; // (the game's input stays its own until the Image is published: EnsureImage)
    g_opened_ms = now_ms;
    g_overlay.keyboard.store(1, std::memory_order_release);
    Log("chat: keyboard open%s", g_draft[0] ? " with the draft" : "");
    zombie::GameRefcount("keyboard open"); // (debug only: the reference count at the open, the baseline for the close)
    zombie::ThreadSchedDump("keyboard open");
}

// One frame of the open keyboard: the pad sampled every frame, the keyboard's work at most kWorkPerSecond times a second.
void Type(Plugin& plugin, ChatSink* sink, u32 held, u32 now_ms) {
    const bool touch = (held & Key::Touchpad) != 0;
    u16 x = 0, y = 0;
    if (touch) {
        const CTRPluginFramework::UIntVector p = Touch::GetPosition();
        x = static_cast<u16>(p.x);
        y = static_cast<u16>(p.y);
    }
    g_latch.Sample(held, touch, x, y);
    const char* cannot = EnsureImage();
    if (!cannot && !g_shown && now_ms - g_opened_ms >= kShowWaitMs) {
        Log("chat: the game did not show its bottom screen in %lu ms: the keyboard is closed", static_cast<unsigned long>(kShowWaitMs));
        cannot = "The keyboard can't be shown in this game";
    }
    if (cannot) {
        Close(true, "not shown"); // (the mirror never taken, or let go once the keys are: the game's input is its own again)
        Notify(cannot);
        return;
    }
    if (!g_shown || !g_pacer.Due(svcGetSystemTick())) {
        return; // (nothing typed before the keyboard is seen: the keys are still the game's)
    }
    const osk::Result r = g_osk->Feed(g_latch.Take(now_ms));
    if (r == osk::Result::kCancel) {
        Close(true, "cancelled");
        return;
    }
    if (r == osk::Result::kSubmit) {
        if (plugin.room().state() != RoomClient::State::Joined) {
            Close(true, "not in a room");
            Notify("Join a room to chat");
            return;
        }
        // (a refusal of the last message that came since this frame's look is told now: in the baseline of the new one it would be lost)
        TellRefused(sink);
        const u32 left = sink->counters.refused_left.load(std::memory_order_relaxed);
        const u32 refused = sink->counters.refused.load(std::memory_order_relaxed); // (before the Post: g_watch)
        const chat::Sent sent = chat::SubmitDraft(*sink, g_osk->text(), g_osk->length());
        if (sent == chat::Sent::kPosted) {
            if (g_watch.Posted(refused, left)) {
                // (the message before was refused between TellRefused's look and the baseline: its text is gone, the notice is not)
                const bool gone = g_watch.NotInRoom();
                Log("chat: our message before this one was not sent (%s)", gone ? "not in the room when its turn came" : "the room's connection did not take it");
                Notify(gone ? "Message not sent: not in the room" : "Message not sent: the room did not take it");
            }
            // The keyboard stays open, empty, for the next message (the user's request, 2026-11-08): Close (or SELECT, or B with nothing
            // typed) closes it. The text is the box's now: a refusal gives it back (TellRefused).
            g_osk->Clear();
            g_draft[0] = '\0';
            Log("chat: sent, the keyboard stays open");
        } else if (sent == chat::Sent::kNothing) {
            g_osk->Clear(); // (spaces only: nothing is left of it)
        } else {
            // (kBusy: the last one has not gone yet - one a second: the keyboard keeps the text, Send sends it again)
            Log("chat: one message a second: the text waits for Send again");
            Notify("One message a second: Send again");
        }
    }
    Redraw(false);
}

// The debug log's line of the costs (the callback's per frame on the game's thread, the Image's redraw on the plugin's) and of the chat's
// counters since the start (the room's messages taken, dropped by the rate limit, malformed, empty after the cleaning; ours sent and
// refused), every kCostLogMs, when something was drawn or a counter moved: a message that never shows is told apart in the log.
void LogCosts(u32 now_ms, const ChatSink& sink) {
    if (g_debug_level <= 0 || now_ms - g_cost_logged_ms < kCostLogMs) {
        return;
    }
    g_cost_logged_ms = now_ms;
    const chat::Counters& k = sink.counters;
    const auto get = [](const std::atomic<u32>& c) { return static_cast<unsigned long>(c.load(std::memory_order_relaxed)); };
    const unsigned long taken = get(k.taken), drops = get(k.rate_drops), malformed = get(k.malformed), empty = get(k.empty);
    const unsigned long sent = get(k.sent), refused = get(k.refused), refused_left = get(k.refused_left);
    const u32 counted = static_cast<u32>(taken + drops + malformed + empty + sent + refused);
    if (g_cost_top.n.load() == 0 && g_cost_bottom.n.load() == 0 && g_cost_redraw.n.load() == 0 && counted == g_counted_logged) {
        return;
    }
    g_counted_logged = counted;
    const auto us = [](u32 ticks) { return static_cast<unsigned long>(static_cast<u64>(ticks) * 1000 / AZL_TICKS_PER_MS); };
    const auto avg = [&](const chat::Cost& c) { return c.n.load() ? us(c.sum.load() / c.n.load()) : 0ul; };
    const auto low = [&](const chat::Cost& c) { return c.n.load() ? us(c.min.load()) : 0ul; };
    Log("chat: cost per frame: lines %lu (min %lu / avg %lu / max %lu us), keyboard %lu (min %lu / avg %lu / max %lu us) | picture redrawn %lu "
        "(avg %lu / max %lu us) | messages: taken %lu, over the rate %lu, malformed %lu, empty %lu | ours: sent %lu, refused %lu (not in the room %lu, by the "
        "room's connection %lu) | OSD notifies: chat %lu, plugin %lu",
        static_cast<unsigned long>(g_cost_top.n.load()), low(g_cost_top), avg(g_cost_top), us(g_cost_top.max.load()),
        static_cast<unsigned long>(g_cost_bottom.n.load()), low(g_cost_bottom), avg(g_cost_bottom), us(g_cost_bottom.max.load()),
        static_cast<unsigned long>(g_cost_redraw.n.load()), avg(g_cost_redraw), us(g_cost_redraw.max.load()), taken, drops,
        malformed, empty, sent, refused, refused_left, refused - refused_left, StatsChatOsdNotifies(), StatsPluginOsdNotifies());
    g_cost_top.Reset();
    g_cost_bottom.Reset();
    g_cost_redraw.Reset();
}

} // namespace

void ChatFrame(u32 chord, bool pad_read) {
    Plugin& plugin = Plugin::Get();
    const u32 now_ms = static_cast<u32>(platform::NowMs());
    // (an Image withdrawn earlier, once no copy of the game's thread can read it any more)
    FreeImage(g_keeper.Freeable(g_overlay.image));
    if (!g_osk && g_image && g_keeper.Retire(g_overlay.image)) {
        g_image = nullptr; // (one the close could not retire, the keeper busy then: the picture lives only while the keyboard is open)
    }
    ChatSink* sink = plugin.chat_sink();
    const bool on = plugin.chat_on() && sink != nullptr;
    if (on && !g_osd_running) {
        // Registered once, the first time the chat is on (never with the chat off); the callback stays and returns at once while it is off.
        OSD::Run(OnChatOsd);
        g_osd_running = true;
        Log("chat: drawing over the game (the lines on the top screen, %s for the keyboard)", plugin.config().chat_key.c_str());
    }
    if (!g_osd_running) {
        return; // (the chat was never on: nothing of it runs)
    }
    g_overlay.sink.store(sink, std::memory_order_release);
    g_overlay.corner.store(static_cast<u32>(plugin.chat_pos()), std::memory_order_relaxed);
    g_overlay.on.store(on ? 1u : 0u, std::memory_order_release);

    MirrorBeforeLook(); // (before the look: a closing the look misses cancels this frame's switch)
    const bool must_close = g_rules.Update(chat::Look(chat::g_signals), now_ms);
    if (g_osk && must_close) {
        Close(true, "HOME, power, sleep or exit", true);
    } else if (g_osk && !on) {
        Close(true, "chat off", true);
    } else if (g_osk && plugin.room().state() != RoomClient::State::Joined) {
        Close(true, "left the room", true);
    }
    if (!pad_read) {
        Controller::Update();
    }
    const u32 held = Controller::GetKeysDown();
    const bool chord_now = g_chord.Update(held, chord); // (every frame, open or not: the chord still held after a close does not reopen)
    if (on) {
        TellRefused(sink); // (our last message refused by the network thread after Send forgot the draft)
        if (!g_osk && chord_now) {
            Open(plugin, now_ms);
        }
        if (g_osk) {
            Type(plugin, sink, held, now_ms);
        }
        LogCosts(now_ms, *sink); // (on: the sink is there)
    }
    // The game's input page, after the keyboard's work of the frame (the switch, the mirror's live parts, the wait for the release), with the
    // chat on or off: a page given back late is still given back.
    MirrorFrame(now_ms);
}

bool ChatKeyboardOpen() {
    return g_osk != nullptr;
}

bool ChatNotice(const char* text) {
    // (the callback registered, the chat on, and a top screen it could draw on within kTopSeenMs: it is there to show it; a game that does
    // not present, a format it refuses, an Old 3DS's swap with the gate closed, and the framework shows it instead)
    if (!g_osd_running || g_overlay.on.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    const u32 now_ms = static_cast<u32>(platform::NowMs());
    const u32 seen = g_overlay.top_seen.load(std::memory_order_relaxed);
    if (seen == 0 || !chat::Recent(now_ms, seen, chat::kTopSeenMs)) {
        return false;
    }
    return g_overlay.board.Show(text, now_ms);
}

void ChatNoteNotify() {
    StatsNotePluginOsdNotify(); // (the plugin's own notification, not the chat's: counted apart)
    g_overlay.notices.Note(static_cast<u32>(platform::NowMs()));
}

void ChatProcessEvent(int event) {
    chat::NoteProcessEvent(chat::g_signals, event); // (the signal before the closing: MirrorBeforeLook)
    MirrorProcessEvent(event); // (then the game's input page back, before the plugin's swap work, which this precedes)
}

} // namespace ctr
} // namespace azl
