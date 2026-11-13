// The chat over the RUNNING game: the pure parts of its host (Sources/ctr/chat_overlay.cpp is the console's side, Tests/chat_host_test.cpp
// drives these on the PC). Plain C++, no heap, no framework.
//
// Two threads share the work (DECISIONI-IMPLEMENTAZIONE, "Due ospiti, stesso codice" and "Prestazioni"):
// - the plugin's thread (OnFrame) has the logic: the chord, the keyboard (azl/osk.hpp) fed at most kWorkPerSecond times a second from the
//   keys it saw held in between (PadLatch: a short press or a tap between two looks is not lost), the rules that close it (CloseRules), and
//   an Image of the keyboard drawn in the bottom framebuffer's own format, redrawn only when what it shows changed;
// - the game's thread, in the framework's OSD callback (DrawOverlay), only copies: the chat's lines on the top screen (azl/chat_draw.hpp)
//   and the Image onto the bottom one, in blocks of words (CopyImage). It never waits, never allocates, and reads only the Overlay, a block that
//   lives as long as the plugin. The Image is the one thing it reads that the plugin's thread frees: ImageSlot is the handshake that lets the
//   plugin's thread know when no copy can still be reading it (ImageKeeper frees it only then).
// The Image is written by the plugin's thread while the game's may be copying it: such a frame shows some pixels of the old picture and some
// of the new (it is redrawn at most 30 times a second, and only when it changed). A second Image to flip to would cost another 150-300 KB of
// the plugin's heap for that one frame.
//
// When the keyboard closes (the user's fallback for HOME and power, STUDIO-CHAT section 12): the APT notifications 1, 2, 3, 5, 7, 8, 9, 10
// and 11 that the game's session answered (counted by the svc hook, Sources/ctr/hook.cpp, NoteAptForChat), and the framework's HOME_ENTER,
// SWAP_ENTER, SLEEP_ENTER and EXIT (Sources/main.cpp, OnProcessEvent). The notification itself is never touched. It does not open again
// between a SWAP_ENTER and its SWAP_EXIT (the plugin's block is away), nor within kReopenHoldMs of a notification 1, 2, 8 or 9 or of a swap,
// nor ever after EXIT.
#pragma once

#include <atomic>

#include "azl/chat_core.hpp"
#include "azl/chat_draw.hpp"
#include "azl/common.hpp"
#include "azl/gfx_surface.hpp"
#include "azl/osk.hpp"

namespace azl {
namespace chat {

constexpr u32 kReopenHoldMs = 2000; ///< no opening this long after a notification 1, 2, 8, 9 or a swap
constexpr u32 kWorkPerSecond = 30;  ///< the keyboard's work on the plugin's thread at most this often (every frame at 60 fps would be twice)
constexpr u32 kWorkEarlyDiv = 500;  ///< a job's place in the pace may be this part of a second (2 ms) before it: the frames' own jitter

/// How far before its job a job's place may be on a clock of `per_second` units a second (WorkPacer): 1/500 s, the frame callback's jitter.
constexpr u64 WorkEarly(u64 per_second) {
    return per_second / kWorkEarlyDiv;
}

/// The step of the pace on a clock of `per_second` units a second, rounded UP, with the early allowance in it: 30 steps are a second and
/// WorkEarly or more, so no window of a second holds a 31st job however early each one came (WorkPacer). 1/30 s and 1/500 s are 33.4 ms, less
/// than two frames of the 3DS's 59.83 Hz (33.43 ms): a job every other frame at 60 fps, every frame at 30.
constexpr u64 WorkGap(u64 per_second) {
    return (per_second + WorkEarly(per_second) + kWorkPerSecond - 1) / kWorkPerSecond;
}

// ---------------------------------------------------------------------------------------------
// The signals that close the keyboard
// ---------------------------------------------------------------------------------------------

/// The signal of InquireNotification's reply word as NS writes it: ONE byte (the bits above it are left over; an Old 3DS gets 0x608 for its
/// power button, Mario Party: The Top 100 0x1BC00001 for HOME). azl/apt_sleep.hpp's AptSignalOf keeps 16 bits for the log and the sleep.
inline u32 AptLowSignal(u32 raw) {
    return raw & 0xFFu;
}

/// HOME (1, 2), the sleep query and the sleep (3, 5), shutdown (7), power (8, 9), "try to sleep" (10) and the order to close (11).
inline bool AptClosesKeyboard(u32 signal) {
    return (signal >= 1 && signal <= 3) || signal == 5 || (signal >= 7 && signal <= 11);
}

/// The ones that go to the HOME menu's side (and, on an Old 3DS, to the swap of the plugin's block): no opening for kReopenHoldMs after them.
inline bool AptHoldsKeyboard(u32 signal) {
    return signal == 1 || signal == 2 || signal == 8 || signal == 9;
}

/// Counters, written by the game's threads (the hook) and the framework's keep-alive thread, read by the plugin's: only increments.
struct Signals {
    std::atomic<u32> apt_close{0}; ///< answered notifications that close the keyboard
    std::atomic<u32> apt_hold{0};  ///< ... of them, the ones after which it may not open for a while
    std::atomic<u32> fw_close{0};  ///< HOME_ENTER, SWAP_ENTER, SLEEP_ENTER, EXIT
    std::atomic<u32> swap_enter{0};
    std::atomic<u32> swap_exit{0};
    std::atomic<u32> exits{0};
};

/// The one set of the plugin (Sources/ctr/chat_overlay.cpp; the tests have their own).
extern Signals g_signals;

// >>> game-thread code (Tests/policy_check.sh: the hook calls this on the game's thread that asked APT: no heap, no wait, no line)
/// What the hook saw: an InquireNotification that came back (`rc` the call's result, `result_word` the answer's cmd[1], `raw` its cmd[2]).
/// Counted only when the game got a notification (`answered`: the call and the service both said yes), whatever the level of the log.
/// True when it closes the keyboard (the hook then gives the game its input page back, Sources/ctr/input_mirror.cpp).
inline bool NoteAptNotification(Signals& s, s32 rc, u32 result_word, u32 raw) {
    if (rc < 0 || static_cast<s32>(result_word) < 0) {
        return false;
    }
    const u32 signal = AptLowSignal(raw);
    if (!AptClosesKeyboard(signal)) {
        return false;
    }
    if (AptHoldsKeyboard(signal)) {
        s.apt_hold.fetch_add(1, std::memory_order_relaxed); // (before the close: a look that sees the close sees the hold too)
    }
    s.apt_close.fetch_add(1, std::memory_order_release);
    return true;
}
// <<< game-thread code

/// The framework's process events (CTRPluginFramework::Process::Event: Sources/ctr/chat_overlay.cpp asserts the numbers).
constexpr int kEventExit = 0, kEventSleepEnter = 1, kEventHomeEnter = 3, kEventSwapEnter = 5, kEventSwapExit = 6;

/// The events that close the keyboard and give the input mirror's page back (Sources/ctr/input_mirror.cpp, MirrorProcessEvent): ONE list
/// (Tests/chat_host_test.cpp holds it). Without SWAP_ENTER an Old 3DS's mirror page, on the plugin's heap, could stay at the game's HID
/// address while the plugin's block goes to the SD; without HOME_ENTER the one synchronous way back before the applet is gone.
constexpr bool IsClosingEvent(int event) {
    return event == kEventHomeEnter || event == kEventSwapEnter || event == kEventSleepEnter || event == kEventExit;
}

/// On the framework's keep-alive thread (4 KB of stack, Luma3DS waiting for the answer at SWAP_ENTER): increments only.
inline void NoteProcessEvent(Signals& s, int event) {
    if (event == kEventSwapEnter) {
        s.swap_enter.fetch_add(1, std::memory_order_relaxed);
    } else if (event == kEventSwapExit) {
        s.swap_exit.fetch_add(1, std::memory_order_relaxed);
    } else if (event == kEventExit) {
        s.exits.fetch_add(1, std::memory_order_relaxed);
    }
    if (IsClosingEvent(event)) {
        s.fw_close.fetch_add(1, std::memory_order_release);
    }
}

/// One look at the counters.
struct Seen {
    u32 apt_close = 0, apt_hold = 0, fw_close = 0, swap_enter = 0, swap_exit = 0, exits = 0;
};
inline Seen Look(const Signals& s) {
    Seen v;
    v.apt_close = s.apt_close.load(std::memory_order_acquire);
    v.fw_close = s.fw_close.load(std::memory_order_acquire);
    v.apt_hold = s.apt_hold.load(std::memory_order_relaxed);
    v.swap_enter = s.swap_enter.load(std::memory_order_relaxed);
    v.swap_exit = s.swap_exit.load(std::memory_order_relaxed);
    v.exits = s.exits.load(std::memory_order_relaxed);
    return v;
}

/// The plugin's thread: a look a frame says whether an open keyboard closes now and whether one may open. What happened before the first
/// look closes nothing (no keyboard was open), but a swap under way or an exit still forbid the opening.
class CloseRules {
public:
    /// True: the keyboard (if open) closes now, its draft kept.
    bool Update(const Seen& s, u32 now_ms) {
        bool close = false;
        if (started_) {
            close = s.apt_close != last_.apt_close || s.fw_close != last_.fw_close;
            if (s.apt_hold != last_.apt_hold || s.swap_enter != last_.swap_enter || s.swap_exit != last_.swap_exit) {
                holding_ = true;
                hold_until_ = now_ms + kReopenHoldMs;
            }
        }
        started_ = true;
        if (holding_ && static_cast<s32>(now_ms - hold_until_) >= 0) {
            holding_ = false; // (over: and a clock that wraps can never bring it back)
        }
        in_swap_ = static_cast<s32>(s.swap_enter - s.swap_exit) > 0;
        exiting_ = s.exits != 0;
        last_ = s;
        return close || in_swap_ || exiting_;
    }
    /// The chord may open the keyboard (as of the last Update).
    bool MayOpen(u32 now_ms) const {
        return started_ && !in_swap_ && !exiting_ && !(holding_ && static_cast<s32>(now_ms - hold_until_) < 0);
    }

private:
    Seen last_;
    u32 hold_until_ = 0;
    bool started_ = false;
    bool holding_ = false;
    bool in_swap_ = false;
    bool exiting_ = false;
};

// ---------------------------------------------------------------------------------------------
// The plugin's thread: the chord, the pace, the keys between two looks
// ---------------------------------------------------------------------------------------------

/// The chord as a whole going from "not all held" to "all held" (its own edge: the framework's "pressed" is nearly always 0 in a running game).
/// A chord held when it is first looked at does not count; an empty chord never does.
class ChordEdge {
public:
    bool Update(u32 held, u32 chord) {
        const bool all = chord != 0 && (held & chord) == chord;
        const bool edge = all && !was_;
        was_ = all;
        return edge;
    }

private:
    bool was_ = true;
};

/// At most one job per `gap` of a clock (any unit, WorkGap and WorkEarly: the console gives ticks, the test microseconds), on a schedule: each
/// job has a place, a `gap` or more after the one before, never after the job and at most `early` before it. A job up to `early` late on its
/// place leaves the next place where it was, so the frame after it is not refused for coming a little sooner (the frame callback's time moves
/// by tenths of a millisecond and two frames are only 0.03 ms more than the step: a job refused for 0.1 ms went to the third frame, 22-26 jobs
/// a second at 60 fps instead of 30, 20 at 30 fps). The places of the jobs of a window of a second are within a second and `early`, a `gap`
/// apart: 30 at most (WorkGap). A job later than `early` (a stall) moves the schedule: no burst after it.
class WorkPacer {
public:
    explicit constexpr WorkPacer(u64 gap, u64 early = 0) : gap_(gap), early_(early) {}
    bool Due(u64 now) {
        if (started_ && now < next_) {
            return false;
        }
        const u64 behind = started_ ? now - next_ : early_; // (the first job: its place `early` before it)
        started_ = true;
        next_ = now + gap_ - (behind < early_ ? behind : early_); // (this job's place, and a gap; early_ < gap_)
        return true;
    }
    void Reset() {
        started_ = false;
    }

private:
    u64 gap_;
    u64 early_;
    u64 next_ = 0; // the next job's place at the earliest
    bool started_ = false;
};

/// The edges of a set of bits (the keys, or the touch) between two jobs, as the keyboard would see them fed every frame: every press and
/// every release is given, one a job and in their order. A press or a tap shorter than the pace is seen at the next job and let go at the one
/// after it; a release shorter than the pace (the stylus lifted 17-33 ms between two keys, A pressed twice in a hurry) is given first and the
/// new press at the job after it, so neither touch nor press is merged with the one before. A bit keeps at most two edges waiting (a third
/// drops a pair: a whole press and release lost, never the state the bit is in).
struct LatchEdges {
    u32 seen = 0; // what the last job was given
    u32 raw = 0;  // the last frame
    u32 one = 0;  // the bits with one edge or more waiting
    u32 two = 0;  // the bits with two
    void Sample(u32 v) {
        const u32 t = v ^ raw;
        raw = v;
        two ^= t & one;
        one |= t;
    }
    u32 Take() {
        seen ^= one;
        one = two;
        two = 0;
        return seen;
    }
};

/// The keys and the touch of every frame between two jobs, their edges kept (LatchEdges); a finger on the screen touches it where it was
/// last (the keyboard takes a touched key on the release, where the finger was at the job before it).
class PadLatch {
public:
    void Sample(u32 held, bool touch, u16 x, u16 y) {
        keys_.Sample(held);
        touch_.Sample(touch ? 1u : 0u);
        if (touch) {
            x_ = x;
            y_ = y;
        }
    }
    osk::Input Take(u32 now_ms) {
        osk::Input in;
        in.held = keys_.Take();
        in.touch = touch_.Take() != 0;
        in.tx = x_;
        in.ty = y_;
        in.now_ms = now_ms;
        return in;
    }

private:
    LatchEdges keys_;
    LatchEdges touch_;
    u16 x_ = 0, y_ = 0;
};

/// What the keyboard's Send did with the chat's box (Sources/ctr/chat_overlay.cpp).
enum class Sent : u32 { kPosted, kBusy, kNothing };

/// The text typed into the chat's box. The box is asked FIRST: only the network thread empties it and only this thread fills it, so a Post
/// that fails after the box was seen free means that nothing was left of the text (spaces only). Asked after a failed Post instead, a box
/// that the network thread emptied in between (its last message gone) read as "nothing to send", and the draft was thrown away.
inline Sent SubmitDraft(ChatSink& sink, const char* text, u32 n) {
    if (sink.OutboxBusy()) {
        return Sent::kBusy; // (the last one has not gone yet: the keyboard keeps the text, START sends again)
    }
    return sink.Post(text, n) ? Sent::kPosted : Sent::kNothing;
}

/// Our message from Send until the network thread is done with it. Send empties the keyboard (it stays open for the next message) and
/// forgets the draft as soon as the box takes it, while the network thread may still refuse it (the room left between Send and its round,
/// which waits up to kSendGapMs after our last one, or ENet not taking the packet): the box is then free, the refusal counted, and its
/// text still there (sink.Outbox()). Refused is asked every frame and is true at most once for a message posted; NotInRoom then says why.
class PostWatch {
public:
    /// Send posted: `refused` = sink.counters.refused and `left` = sink.counters.refused_left read BEFORE SubmitDraft (a refusal needs a
    /// full box: none falls in between).
    /// Returns true when the message posted before, still watched, was refused in the meantime (between the caller's last look and this
    /// baseline: the keyboard stays open after Send, so a quick second Send can fall there); NotInRoom then says why. The caller tells it:
    /// its text is gone (the box holds the new message), only the notice is left.
    bool Posted(u32 refused, u32 left = 0) {
        const bool previous = armed_ && refused != refused_;
        if (previous) {
            not_in_room_ = left != left_;
        }
        refused_ = refused;
        left_ = left;
        armed_ = true;
        return previous;
    }
    bool Refused(const ChatSink& sink) {
        if (!armed_ || sink.OutboxBusy()) {
            return false;
        }
        armed_ = false;
        not_in_room_ = sink.counters.refused_left.load(std::memory_order_relaxed) != left_;
        return sink.counters.refused.load(std::memory_order_relaxed) != refused_;
    }
    /// The last refusal Refused told: we were not in the room when its turn came (false: the room's connection, ENet, did not take it).
    bool NotInRoom() const {
        return not_in_room_;
    }

private:
    u32 refused_ = 0;
    u32 left_ = 0;
    bool armed_ = false;
    bool not_in_room_ = false;
};

// ---------------------------------------------------------------------------------------------
// The picture of the keyboard and its handshake
// ---------------------------------------------------------------------------------------------

constexpr u32 kImageW = 320, kImageH = 240; ///< the bottom screen

/// The keyboard drawn in a framebuffer's format: the screen's 320 columns, each its 240 rows packed as the framebuffer stores them (bottom row
/// first), `col_bytes` = 240 * bpp apart. Fixed once published; only the pixels change.
struct Image {
    u8* pixels = nullptr;
    u32 col_bytes = 0;
    u8 format = 0;
    u8 bpp = 0;
};

/// The bytes of the pixels of an Image of `format` (0: not a format the drawing knows).
constexpr u32 ImageBytes(u32 format) {
    return gfx::BytesPerPixel(format) * kImageH * kImageW;
}

/// The Image as a surface to draw the keyboard on (azl/osk_draw.hpp, on the plugin's thread).
inline gfx::Surface ImageSurface(const Image& im) {
    gfx::Surface s;
    s.left = im.pixels;
    s.stride = im.col_bytes;
    s.bpp = im.bpp;
    s.format = im.format;
    s.w = static_cast<u16>(kImageW);
    s.h = static_cast<u16>(kImageH);
    return s;
}

/// What the game's thread found of the bottom framebuffer, for the plugin's thread to draw in: 0 = not seen yet.
inline u32 GeometryOf(const gfx::Surface& s) {
    return 0x10000u | s.format | static_cast<u32>(s.bpp) << 8;
}
inline u8 GeometryFormat(u32 geometry) {
    return static_cast<u8>(geometry & 0xFF);
}
inline u8 GeometryBpp(u32 geometry) {
    return static_cast<u8>((geometry >> 8) & 0xFF);
}

/// The Image the game's thread may copy, and how many copies are under way. Enter counts the reader BEFORE it reads the pointer, Withdraw
/// clears the pointer BEFORE the plugin's thread reads the count (all sequentially consistent): when the plugin's thread sees no reader after
/// a Withdraw, a reader that comes later can only find nullptr. A reader that is there may still hold the old Image: the plugin's thread
/// waits for the count to be 0 by looking again at its next frame (ImageKeeper), it never blocks on it.
class ImageSlot {
public:
    // >>> game-thread code (Tests/policy_check.sh)
    const Image* Enter() {
        readers_.fetch_add(1, std::memory_order_seq_cst);
        return shown_.load(std::memory_order_seq_cst);
    }
    void Leave() {
        readers_.fetch_sub(1, std::memory_order_seq_cst);
    }
    // <<< game-thread code
    void Publish(Image* im) {
        shown_.store(im, std::memory_order_seq_cst);
    }
    Image* Withdraw() {
        return shown_.exchange(nullptr, std::memory_order_seq_cst);
    }
    bool Quiet() const {
        return readers_.load(std::memory_order_seq_cst) == 0;
    }
    const Image* shown() const {
        return shown_.load(std::memory_order_seq_cst);
    }

private:
    std::atomic<Image*> shown_{nullptr};
    std::atomic<u32> readers_{0};
};

/// The plugin's thread's side of the freeing: an Image withdrawn waits here until no copy can be reading it. One at a time: a new Image is
/// not published while an old one waits (Empty), so this never grows.
class ImageKeeper {
public:
    /// Withdraws what the slot shows (if anything) and keeps it to free. False when one is already waiting (nothing is withdrawn then).
    bool Retire(ImageSlot& slot) {
        if (waiting_ != nullptr) {
            return false;
        }
        waiting_ = slot.Withdraw();
        return true;
    }
    /// The Image the caller may free now (nullptr: none, or a copy may still read it).
    Image* Freeable(const ImageSlot& slot) {
        if (waiting_ == nullptr || !slot.Quiet()) {
            return nullptr;
        }
        Image* im = waiting_;
        waiting_ = nullptr;
        return im;
    }
    bool Empty() const {
        return waiting_ == nullptr;
    }

private:
    Image* waiting_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// The plugin's own notifications, in the corner opposite the chat
// ---------------------------------------------------------------------------------------------

constexpr u32 kTopSeenMs = 500; ///< the OSD callback drew the top screen this recently: a notification goes to it, else to the framework

/// The plugin's notifications drawn by the chat's OSD callback in the corner opposite the chat (chat_draw.hpp, Opposite), so that they never
/// cover it (the user's request, 2026-11-08: the framework draws its own at the bottom right, over a chat at the bottom). The same ring and
/// reader as the chat's lines (a seqlock: the game's thread never waits and never sees a line half written), lines with no nick: the plugin's
/// thread is the one writer, the OSD callback the one reader. A notification is cleaned like a message and cut at kTextMax with "..."
/// (the plugin's own are at most 58 characters a line: Plugin::TakeNotifications wraps them). kLines at most: a burst of more shows the last
/// ones. The host posts here only while the callback draws the top screen (Overlay::top_seen); otherwise OSD::Notify shows it.
class NoticeBoard {
public:
    /// The plugin's thread: false when nothing is left of the text (nothing to show).
    bool Show(const char* text, u32 now_ms) {
        // (kLines notifications younger than kNoticeShowMs already shown: one more would push one out before its time; false, and the
        // framework's OSD::Notify, which queues them all, shows it instead)
        u32 young = 0;
        for (u32 t : shown_ms_) {
            if (t != 0 && Recent(now_ms, t, kNoticeShowMs)) {
                ++young;
            }
        }
        if (young >= kLines) {
            return false;
        }
        Line line;
        line.text_len = static_cast<u8>(Sanitize(reinterpret_cast<const u8*>(text), std::strlen(text), line.text, kTextMax, true));
        if (line.text_len == 0) {
            return false;
        }
        line.t_ms = now_ms;
        ring_.Push(line);
        shown_ms_[next_ % kLines] = now_ms | 1u;
        ++next_;
        return true;
    }
    // >>> game-thread code (Tests/policy_check.sh)
    const View& Refresh() {
        return reader_.Refresh(ring_);
    }
    // <<< game-thread code

private:
    ChatRing ring_;
    ChatReader reader_;
    u32 shown_ms_[kLines] = {}; ///< the plugin's thread only: when the last kLines were shown (| 1; 0 = none)
    u32 next_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The latency of the other players, on the screen while the keyboard is open
// ---------------------------------------------------------------------------------------------

constexpr u32 kLatWantedMs = 1500;  ///< the plugin's thread builds the lines while the drawing asked for them this recently
constexpr u32 kLatPublishMs = 250;  ///< ... and refreshes them this often (4 a second: the values are medians over seconds)
constexpr u32 kLatStaleMs = 2000;   ///< the drawing shows lines no older than this (a board nobody refreshes goes off the screen)

/// The lines of the latency, in the chat's own ring: the plugin's thread REPLACES the whole picture (one line a player: nick = the name, text =
/// the value, pad = the colour's index, t_ms = when it was made), the game's thread reads it the way it reads the chat. Made only while the
/// keyboard is open (Overlay::lat_wanted).
class LatencyBoard {
public:
    /// The plugin's thread: the picture is these `n` lines (0: nothing).
    void Publish(const Line* rows, u32 n) {
        ring_.Replace(rows, n);
    }
    // >>> game-thread code (Tests/policy_check.sh)
    const View& Refresh() {
        return reader_.Refresh(ring_);
    }
    // <<< game-thread code

private:
    ChatRing ring_;
    ChatReader reader_;
};

// ---------------------------------------------------------------------------------------------
// The game's thread: what the OSD callback draws
// ---------------------------------------------------------------------------------------------

/// What the plugin's thread tells the game's: one block for the plugin's whole life (a static of Sources/ctr/chat_overlay.cpp), never freed.
struct Overlay {
    std::atomic<ChatSink*> sink{nullptr}; ///< the chat's block (never freed either), nullptr until the chat is first on
    std::atomic<u32> on{0};               ///< the chat's switch: off, the callback returns at once
    std::atomic<u32> corner{kBottomLeft}; ///< chat_pos
    std::atomic<u32> keyboard{0};         ///< the keyboard is open: all the lines stay, and the bottom screen gets the Image
    std::atomic<u32> bottom_geometry{0};  ///< GeometryOf the bottom framebuffer, written by the game's thread while the keyboard is open
    ImageSlot image;
    NoticeClock notices;                  ///< the framework's notifications on screen (OSD::Notify: the bottom-right corner climbs above them)
    NoticeBoard board;                    ///< the plugin's own, drawn in the corner opposite the chat
    LatencyBoard lat;                     ///< the latency of the other players, drawn in the free corner beside the notifications' (the half of the screen the chat does not use)
    std::atomic<u32> lat_wanted{0};       ///< the clock (| 1) of the last frame drawn with the keyboard open: the plugin's thread builds `lat` while it is recent
    std::atomic<u32> top_seen{0};         ///< the clock (| 1) of the last top screen the callback could draw on, 0: none (it refused it)
};

// >>> game-thread code (Tests/policy_check.sh: the OSD callback runs on the game's thread that presents: no heap, no wait, no big buffer)
/// The cost of the callback on the game's thread, and of the drawing of the Image on the plugin's (read and reset by the plugin's thread for
/// the debug log: relaxed, a sample torn by the reset is one sample).
struct Cost {
    std::atomic<u32> n{0}, sum{0}, max{0}, min{0xFFFFFFFFu};
    void Add(u32 t) {
        n.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(t, std::memory_order_relaxed);
        if (t > max.load(std::memory_order_relaxed)) {
            max.store(t, std::memory_order_relaxed);
        }
        if (t < min.load(std::memory_order_relaxed)) {
            min.store(t, std::memory_order_relaxed);
        }
    }
    void Reset() {
        n.store(0, std::memory_order_relaxed);
        sum.store(0, std::memory_order_relaxed);
        max.store(0, std::memory_order_relaxed);
        min.store(0xFFFFFFFFu, std::memory_order_relaxed);
    }
};

/// What a call of DrawOverlay did.
enum Drew : u32 { kDrewNothing = 0, kDrewLines = 1, kDrewKeyboard = 2 };


/// The Image fits this framebuffer: its format, its pixel, a column long enough, words that are words (the framebuffer's address and stride
/// by gfx::Usable, the Image's address here: a block load or store of a word that is not one is a data abort, not a slow access).
inline bool ImageFits(const Image& im, const gfx::Surface& fb) {
    return gfx::Usable(fb) && fb.format == im.format && fb.bpp == im.bpp && fb.w == kImageW && fb.h == kImageH && fb.stride >= im.col_bytes &&
           im.col_bytes % 16 == 0 && im.col_bytes == kImageH * im.bpp && (reinterpret_cast<std::uintptr_t>(im.pixels) & 3) == 0;
}

/// The Image onto the framebuffer: every column's visible rows (a column is 720, 480 or 960 bytes: a whole number of four words). The source
/// is the plugin's heap (cached, cheap to read); the framebuffer is only written. On the console (ARM) a column goes out in blocks of eight
/// words, one ldm and one stm each (BGR8's 720 bytes: one block of four first, then 22 of eight): the copy runs on the game's thread every
/// frame the keyboard is open, and one store a word was 76 800 stores a frame in RGBA8 (234 K instructions, several ms of an Old 3DS's
/// 268 MHz frame), the burst 9 600 (about 41 K instructions, Tests/chat_draw_cost.sh counts them in the plugin's own build). On the PC the
/// same words go out one at a time, so that Tests/chat_host_test.cpp counts them and checks every pixel. Kept loops of words, never a
/// memcpy (the C library's is no faster here and its stores are not these).
__attribute__((noinline, optimize("no-tree-loop-distribute-patterns"))) inline void CopyImage(const Image& im, const gfx::Surface& fb) {
#if defined(__arm__) && !defined(__thumb__)
    const u8* src = im.pixels;
    const u8* const src_end = im.pixels + kImageW * im.col_bytes;
    u8* dst = fb.left;
    const u32 col_bytes = im.col_bytes;
    const u32 gap = fb.stride - im.col_bytes; // (from the end of a column to the start of the next one)
    u8* end;
    // r3-r9 and r12 carry the words; the six operands take the other six registers (r0-r2, r10, r11, lr: the frame pointer is omitted)
    __asm__ volatile("0:  add   %[end], %[dst], %[cb]\n"
                     "    tst   %[cb], #16\n"
                     "    ldmne %[src]!, {r3-r6}\n"
                     "    stmne %[dst]!, {r3-r6}\n"
                     "1:  ldm   %[src]!, {r3-r9, r12}\n"
                     "    stm   %[dst]!, {r3-r9, r12}\n"
                     "    cmp   %[dst], %[end]\n"
                     "    bne   1b\n"
                     "    add   %[dst], %[dst], %[gap]\n"
                     "    cmp   %[src], %[src_end]\n"
                     "    bne   0b\n"
                     : [src] "+r"(src), [dst] "+r"(dst), [end] "=&r"(end)
                     : [cb] "r"(col_bytes), [gap] "r"(gap), [src_end] "r"(src_end)
                     : "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r12", "cc", "memory");
#else
    const u32 words = im.col_bytes / 4;
    for (u32 x = 0; x < kImageW; ++x) {
        const gfx::Word* src = reinterpret_cast<const gfx::Word*>(im.pixels + x * im.col_bytes);
        gfx::Word* dst = reinterpret_cast<gfx::Word*>(fb.left + x * fb.stride);
        for (u32 i = 0; i + 4 <= words; i += 4) {
            const u32 a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
            AZL_GFX_COUNT_STORE(4);
            dst[i] = a;
            AZL_GFX_COUNT_STORE(4);
            dst[i + 1] = b;
            AZL_GFX_COUNT_STORE(4);
            dst[i + 2] = c;
            AZL_GFX_COUNT_STORE(4);
            dst[i + 3] = d;
        }
    }
#endif
}

/// The latency of the other players, while the keyboard is open (the user writes): in the free corner beside the notifications' (`corner ^ 2`: the same row, in the half of the screen the chat does not use,
/// so that no message of the chat covers it), starting at the notifications' own height and above the framework's when there are some, and the drawing says
/// to the plugin's thread that it wants the lines (lat_wanted). `*edge_out` is the edge of its stack, where the chat's own stops. True when it drew.
__attribute__((noinline)) inline bool DrawLatency(Overlay& o, const gfx::Surface& fb, u32 now_ms, int corner, u32 young, int* edge_out) {
    o.lat_wanted.store(now_ms | 1u, std::memory_order_relaxed);
    const View& lv = o.lat.Refresh();
    if (lv.count == 0 || !Recent(now_ms, lv.lines[lv.count - 1].t_ms, kLatStaleMs)) {
        return false;
    }
    const int lat_corner = corner ^ 2;
    const bool lat_top = lat_corner == kTopLeft || lat_corner == kTopRight;
    // It starts at the same height as the plugin's notifications (the top row, or the bottom one): the two stand side by side - a notification is on the opposite side of the screen - and the
    // space under (or above) them is the chat's. It is drawn BEFORE the notifications, so that where a very wide one meets it the notification, which is there for a moment and matters, wins.
    const int start = lat_top ? kTopY : (young != 0 ? BottomRightY(young) : kBottomY);
    if (DrawLatencyLines(fb, lv, lat_corner, start, kNoLimit, edge_out) == 0) {
        return false;
    }
    return true;
}

/// The top screen: the plugin's notifications in the corner opposite the chat, then the chat's lines, if any are to be shown; the chat's
/// stack stops where the notifications' begins. Whether this screen could be drawn on is told to the plugin's thread (top_seen: it sends
/// its notifications to the framework when not). Out of line: the callback's own frame stays small.
__attribute__((noinline)) inline u32 DrawOverlayTop(Overlay& o, const gfx::Surface& fb, u32 now_ms) {
    if (!TopUsable(fb)) {
        o.top_seen.store(0, std::memory_order_relaxed);
        return kDrewNothing;
    }
    o.top_seen.store(now_ms | 1u, std::memory_order_relaxed);
    const int corner = static_cast<int>(o.corner.load(std::memory_order_relaxed));
    const int across = Opposite(corner);
    // (the framework's notifications at the bottom right: whichever stack is there climbs above them)
    const u32 young = corner == kBottomRight || across == kBottomRight || (corner ^ 2) == kBottomRight ? o.notices.Young(now_ms) : 0;
    u32 drew = kDrewNothing;
    int limit = kNoLimit;
    ChatSink* sink = o.sink.load(std::memory_order_acquire);
    const bool keyboard = o.keyboard.load(std::memory_order_relaxed) != 0;
    int lat_edge = kNoLimit;
    const bool lat_drawn = sink != nullptr && keyboard && DrawLatency(o, fb, now_ms, corner, young, &lat_edge); // (only while the keyboard is open: when the user writes; before the notifications)
    const View& board = o.board.Refresh(); // (the game's thread is the board's one reader)
    const u32 notes = ShownNotices(board, now_ms);
    if (notes != 0 && DrawChatLines(fb, board, notes, across, across == kBottomRight ? young : 0, kNoLimit, &limit) != 0) {
        drew = kDrewLines;
    } else {
        limit = kNoLimit;
    }
    if (lat_drawn) {
        drew = kDrewLines;
        // the chat stops where the farthest of the two stacks on that side of the screen - the notifications' and the latency's - begins
        const bool chat_top = corner == kTopLeft || corner == kTopRight;
        limit = limit == kNoLimit ? lat_edge : chat_top ? (lat_edge < limit ? lat_edge : limit) : (lat_edge > limit ? lat_edge : limit);
    }
    if (sink == nullptr) {
        return drew;
    }
    const View& v = sink->reader.Refresh(sink->ring); // (the game's thread is the ring's one reader)
    const u32 shown = ShownLines(v, now_ms, keyboard);
    if (shown != 0 && DrawChatLines(fb, v, shown, corner, corner == kBottomRight ? young : 0, limit, nullptr, true) != 0) { // (the chat's lines are translucent, the plugin's notifications above are not)
        drew = kDrewLines;
    }
    return drew;
}

/// The bottom screen: the Image of the keyboard, when it is open and drawn in this framebuffer's format; the format is told to the plugin's
/// thread, which draws the Image in it (a frame or two after the opening, or after the game changes its format, the keyboard is not there yet).
__attribute__((noinline)) inline u32 DrawOverlayBottom(Overlay& o, const gfx::Surface& fb) {
    if (o.keyboard.load(std::memory_order_acquire) == 0) {
        return kDrewNothing;
    }
    const u32 geometry = GeometryOf(fb);
    if (o.bottom_geometry.load(std::memory_order_relaxed) != geometry) {
        o.bottom_geometry.store(geometry, std::memory_order_release);
    }
    u32 drew = kDrewNothing;
    const Image* im = o.image.Enter();
    if (im != nullptr && ImageFits(*im, fb)) {
        CopyImage(*im, fb);
        drew = kDrewKeyboard;
    }
    o.image.Leave();
    return drew;
}

/// What the OSD callback does with one screen (the caller has passed the swap gate already).
inline u32 DrawOverlay(Overlay& o, const gfx::Surface& fb, bool top, u32 now_ms) {
    if (o.on.load(std::memory_order_relaxed) == 0) {
        return kDrewNothing;
    }
    return top ? DrawOverlayTop(o, fb, now_ms) : DrawOverlayBottom(o, fb);
}
// <<< game-thread code

} // namespace chat
} // namespace azl
