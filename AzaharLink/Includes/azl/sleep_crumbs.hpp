// The trail a sleep leaves behind. A console that went to sleep with the plugin loaded froze once, for good (PROVA 1: host,
// quest, lid closed 10 s, black screen, nothing in either log after the last statistics line - the log writer is the
// lowest-priority thread of the process and writes in batches, so what it had not written yet was lost). This is the
// evidence that does not depend on it: the few things that happen around a sleep are noted in a lock-free ring by whoever
// sees them (the game's thread inside its APT call, the framework's keep-alive thread), and a thread of its own - woken
// by an event, at a priority above the game's - turns them into short lines and writes each one to its own file
// (AzaharLink.sleep.log) with a flush, so that a freeze leaves the last thing that happened:
//   the notifications the system sent (3 query, 4 cancelled, 5 begins, 6 wake-up, HOME, power),
//   the game's answer to the query and its acknowledgement of the sleep (noted BEFORE the call: it may not return),
//   the framework's SLEEP_ENTER / SLEEP_EXIT,
//   a wake marker: the thread compares the console's clock with the tick (the tick stops during a sleep, osGetTime() does
//   not) and says when the clock ran ahead - and that it was still alive 1 s and 5 s later.
// Read the file like this: "3, 5 and no acknowledgement" - the game's wind-down hung; "acknowledged, no wake" - the system
// or the card; "WOKE UP" and nothing after - the process ran again and then froze; nothing at all - the freeze came first.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "azl/apt_sleep.hpp"
#include "azl/common.hpp"
#include "azl/line.hpp"
#include "azl/wallclock.hpp"

namespace azl {

class SleepCrumbs {
public:
    /// The framework's own events, besides the APT ones of azl/apt_sleep.hpp (kAptNote*).
    static constexpr u32 kFwEnter = 4;
    static constexpr u32 kFwExit = 5;
    static constexpr u32 kFwSwapEnter = 6;   ///< the block of the plugin goes (Old 3DS); the value: the how-manieth
    static constexpr u32 kFwSwapExit = 7;    ///< ... and is back
    static constexpr u32 kMemSystem = 15;    ///< a memory region as PackRegion says, with the swap's events
    static constexpr u32 kMemApplication = 16;
    static constexpr u32 kMemBase = 17;
    static constexpr u32 kFwSwapBack = 22;   ///< the block is back and the call sites are in the hook again; the value: how long that took, in us
    static constexpr u32 kSlots = 32;        ///< events waiting for the thread (more between two of its turns: the oldest go)
    static constexpr u32 kMaxLines = 400;    ///< lines written in one launch
    static constexpr u32 kPollMs = 250;      ///< the thread's turn while a sleep is watched (else it waits for an event)
    static constexpr u32 kWatchMs = 60000;   ///< ... at most this long after a sleep began without a wake-up
    static constexpr u32 kClockJumpMs = 200; ///< the console's clock ran this much ahead of the tick: the console slept

    // This runs on whatever thread sees the event - the game's APT thread inside the svc hook above all: a few stores, no
    // allocation, no format (Tests/policy_check.sh, Tests/callgraph_check.py).
    // >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)
    /// Notes one event. `kind`: kAptNote* (azl/apt_sleep.hpp) or kFw* / kMem* here; `value`: what the kind says it is.
    void Push(u32 kind, u32 value, u32 tick_ms) {
        const u32 i = head_.fetch_add(1, std::memory_order_relaxed);
        Slot& slot = slots_[i % kSlots];
        slot.kind.store(kind, std::memory_order_relaxed);
        slot.value.store(value, std::memory_order_relaxed);
        slot.tick_ms.store(tick_ms, std::memory_order_relaxed);
        slot.seq.store(i + 1, std::memory_order_release); // (last: the thread reads a slot only when this says it is whole)
    }
    // <<< game-thread code

    /// Used and total bytes of a memory region in one word, in units of 4 KB (a region is at most 256 MiB).
    static u32 PackRegion(u32 used_bytes, u32 total_bytes) {
        return (used_bytes / 4096) << 16 | (total_bytes / 4096);
    }

    /// The events noted so far, and those the thread has taken (only the thread may call taken()): what it took is written when its turn ends.
    u32 pushed() const {
        return head_.load(std::memory_order_acquire);
    }
    u32 taken() const {
        return tail_;
    }

    /// True while a sleep is being watched for its wake-up: the thread then takes a turn every kPollMs.
    bool watching() const {
        return watching_.load(std::memory_order_relaxed);
    }

    /// The thread's turn: appends the lines to write now, in order. `now_tick_ms` is the tick clock, `now_os_ms` the
    /// console's (osGetTime). Only the one thread calls this.
    void Poll(u32 now_tick_ms, u64 now_os_ms, std::vector<std::string>* lines) {
        if (!ref_known_) {
            ref_known_ = true;
            ref_gap_ms_ = Gap(now_tick_ms, now_os_ms); // (the clocks agree up to here: nothing has slept yet)
        }
        const u32 head = head_.load(std::memory_order_acquire);
        if (head - tail_ > kSlots) {
            Add(lines, now_tick_ms, now_os_ms, "events were lost (this thread did not run for a while)");
            tail_ = head - kSlots;
        }
        while (tail_ != head) {
            Slot& slot = slots_[tail_ % kSlots];
            if (slot.seq.load(std::memory_order_acquire) != tail_ + 1) {
                break; // (the producer has its index but has not finished the slot: the next turn)
            }
            Event(slot.kind.load(std::memory_order_relaxed), slot.value.load(std::memory_order_relaxed),
                  slot.tick_ms.load(std::memory_order_relaxed), now_tick_ms, now_os_ms, lines);
            ++tail_;
        }
        if (watch_) {
            Watch(now_tick_ms, now_os_ms, lines);
        }
    }

    /// Lines produced so far.
    u32 lines() const {
        return lines_;
    }

private:
    struct Slot {
        std::atomic<u32> seq{0}; ///< the index + 1 of the event in it, once it is whole
        std::atomic<u32> kind{0};
        std::atomic<u32> value{0};
        std::atomic<u32> tick_ms{0};
    };

    static s64 Gap(u32 tick_ms, u64 os_ms) {
        return static_cast<s64>(os_ms) - static_cast<s64>(tick_ms);
    }

    static const char* NotificationName(u32 id) {
        static const char* const kNames[] = {"none",           "home button",      "home button (2)", "sleep query",
                                             "sleep cancelled", "sleep begins",     "wake-up",         "shutdown",
                                             "power button",   "power button (2)", "try to sleep",    "order to close"};
        return id < sizeof(kNames) / sizeof(kNames[0]) ? kNames[id] : "?";
    }

    static std::string Hex(u32 value) {
        static const char kDigits[] = "0123456789abcdef";
        std::string text = "0x";
        for (int shift = 28; shift >= 0; shift -= 4) {
            text += kDigits[(value >> shift) & 0xF];
        }
        return text;
    }

    void AddRegion(std::vector<std::string>* lines, u32 tick_ms, u64 now_os_ms, const char* name, u32 packed) {
        const u32 used_kb = (packed >> 16) * 4;
        const u32 total_kb = (packed & 0xFFFF) * 4;
        Add(lines, tick_ms, now_os_ms,
            std::string("memory region ") + name + ": " + std::to_string(used_kb) + " of " + std::to_string(total_kb) + " KB used, " +
                std::to_string(total_kb > used_kb ? total_kb - used_kb : 0) + " KB free");
    }

    /// One line: "sleep: <text> | tick <ms> ms | written at console clock <time>" (`tick_ms`: the event's own).
    void Add(std::vector<std::string>* lines, u32 tick_ms, u64 now_os_ms, const std::string& text) {
        if (lines_ >= kMaxLines) {
            return;
        }
        std::string line = "sleep: ";
        line += ++lines_ == kMaxLines ? "the limit of lines for one launch is reached: nothing more is written" : text;
        char wall[24];
        FormatConsoleClock(now_os_ms, wall);
        line += " | tick " + std::to_string(tick_ms) + " ms | written at console clock " + wall;
        lines->push_back(line);
    }

    /// An event of the ring: its line, and what it means for the watch.
    void Event(u32 kind, u32 value, u32 event_tick_ms, u32 now_tick_ms, u64 now_os_ms, std::vector<std::string>* lines) {
        switch (kind) {
        case kAptNoteNotification: {
            const u32 signal = AptSignalOf(value); // (the value is the word as it came)
            Add(lines, event_tick_ms, now_os_ms,
                "APT notification " + std::to_string(signal) + " (" + NotificationName(signal) + ")" +
                    (value != signal ? ", reply word " + Hex(value) : std::string()));
            if (signal == kAptSignalSleepEnter) {
                Arm(now_tick_ms);
            }
            break;
        }
        case kAptNoteReplyQuery:
            Add(lines, event_tick_ms, now_os_ms,
                std::string("the game answers the sleep query: ") +
                    (value == 0 ? "reject" : value == 1 ? "accept" : value == 2 ? "later" : "?"));
            break;
        case kAptNoteReplyComplete:
            Add(lines, event_tick_ms, now_os_ms,
                "the game acknowledges the sleep (ReplySleepNotificationComplete: this line is written before the call)");
            break;
        case kFwEnter:
            Add(lines, event_tick_ms, now_os_ms, "framework event SLEEP_ENTER");
            Arm(now_tick_ms);
            break;
        case kFwExit:
            Add(lines, event_tick_ms, now_os_ms, "framework event SLEEP_EXIT");
            break;
        case kFwSwapEnter:
            Add(lines, event_tick_ms, now_os_ms, "framework event SWAP_ENTER #" + std::to_string(value) + ": Luma3DS takes the plugin's block away next");
            break;
        case kFwSwapExit:
            Add(lines, event_tick_ms, now_os_ms, "framework event SWAP_EXIT #" + std::to_string(value) + ": the plugin's block is back");
            break;
        case kFwSwapBack:
            Add(lines, event_tick_ms, now_os_ms,
                "the plugin's block is back: call sites in the hook again in " + std::to_string(value) + " us, before the game's thread runs");
            break;
        case kAptNoteLimit:
            Add(lines, event_tick_ms, now_os_ms, "the lines about APT:ReceiveParameter and APT:AppletUtility reached their limit for this launch (64)");
            break;
        case kMemSystem:
            AddRegion(lines, event_tick_ms, now_os_ms, "SYSTEM", value);
            break;
        case kMemApplication:
            AddRegion(lines, event_tick_ms, now_os_ms, "APPLICATION", value);
            break;
        case kMemBase:
            AddRegion(lines, event_tick_ms, now_os_ms, "BASE", value);
            break;
        case kAptNoteReceiveMade:
            Add(lines, event_tick_ms, now_os_ms, "APT:ReceiveParameter by the plugin itself (trampoline return " + Hex(value) + "): no HOME or power button seen in the last 10 s");
            break;
        case kAptNoteReceiveHanded:
            Add(lines, event_tick_ms, now_os_ms, "APT:ReceiveParameter handed back to the game's own call at " + Hex(value));
            break;
        case kAptNoteReceiveRefused:
            Add(lines, event_tick_ms, now_os_ms, "APT:ReceiveParameter: the hand-back was refused (trampoline return " + Hex(value) + "): the plugin makes the call");
            break;
        case kAptNoteReceiveReply:
            Add(lines, event_tick_ms, now_os_ms,
                value == kAptReceiveFailed ? std::string("APT:ReceiveParameter failed")
                                           : "APT:ReceiveParameter answered: sender " + Hex(value >> 16) + ", command " + std::to_string(value & 0xFFFF));
            break;
        case kAptNoteUtility:
            utility_ = value;
            break;
        case kAptNoteUtilityResult:
            Add(lines, event_tick_ms, now_os_ms,
                "APT:AppletUtility id " + std::to_string(utility_ & 0xFFFF) + " (input " + Hex(utility_ >> 16) + ") -> " + Hex(value));
            break;
        case kAptNoteBadHandle:
            words_ = "a call of the game on handle " + Hex(value) + " answered invalid handle:";
            break;
        case kAptNoteFatal:
            words_ = "the game reports a fatal error to err:f (session " + Hex(value) + "; cmd[1] type, cmd[2] result, cmd[3] pc):";
            break;
        case kAptNoteWord:
            words_ += " " + Hex(value);
            break;
        case kAptNoteEnd:
            Add(lines, event_tick_ms, now_os_ms, words_ + " | the words: thread id, priority, core, hook state (1: suspended), ms since the call sites were back in the hook, last session opened through the hook, ms since then, cmd[0..6], then what looks like code on its stack");
            words_.clear();
            break;
        default:
            break;
        }
    }

    void Arm(u32 now_tick_ms) {
        watch_ = true;
        woke_ = false;
        arm_tick_ms_ = now_tick_ms;
        alive_said_ = 0;
        watching_.store(true, std::memory_order_relaxed);
    }

    /// The clock against the tick: when the console's clock has run ahead of the tick by more than kClockJumpMs, the
    /// console slept and is awake again (the reference moves on, so that the next sleep is measured from here).
    void Watch(u32 now_tick_ms, u64 now_os_ms, std::vector<std::string>* lines) {
        const s64 jump = Gap(now_tick_ms, now_os_ms) - ref_gap_ms_;
        if (jump >= static_cast<s64>(kClockJumpMs)) {
            ref_gap_ms_ += jump;
            woke_ = true;
            woke_tick_ms_ = now_tick_ms;
            alive_said_ = 0;
            Add(lines, now_tick_ms, now_os_ms,
                "WOKE UP: the console's clock ran " + std::to_string(jump) + " ms ahead of the tick (about the time it was asleep)");
            return;
        }
        if (!woke_) {
            if (now_tick_ms - arm_tick_ms_ > kWatchMs) {
                Disarm();
                Add(lines, now_tick_ms, now_os_ms, "no wake-up seen in 60 s of running: the sleep did not happen");
            }
            return;
        }
        const u32 since = now_tick_ms - woke_tick_ms_;
        if (alive_said_ == 0 && since >= 1000) {
            alive_said_ = 1;
            Add(lines, now_tick_ms, now_os_ms, "alive 1 s after the wake-up");
        }
        if (alive_said_ == 1 && since >= 5000) {
            Add(lines, now_tick_ms, now_os_ms, "alive 5 s after the wake-up");
            Disarm();
        }
    }

    void Disarm() {
        watch_ = false;
        woke_ = false;
        watching_.store(false, std::memory_order_relaxed);
    }

    std::atomic<u32> head_{0};
    Slot slots_[kSlots];
    // (the thread's own from here on)
    u32 tail_ = 0;
    u32 lines_ = 0;
    bool ref_known_ = false;
    s64 ref_gap_ms_ = 0; ///< console clock minus tick as it was when no sleep had happened since (moves on at every wake)
    bool watch_ = false;
    bool woke_ = false;
    u32 arm_tick_ms_ = 0;
    u32 woke_tick_ms_ = 0;
    u32 alive_said_ = 0;
    std::string words_;   ///< the line of a failed call while its words arrive
    u32 utility_ = 0;     ///< the APT:AppletUtility whose result comes next
    std::atomic<bool> watching_{false};
};

} // namespace azl
