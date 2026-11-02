#include "ctr/chat_overlay.hpp"

#ifdef AZL_CHAT_OVERLAY

#include <3ds.h> // hidSharedMem (the HID shared page the game reads its touch from)
#include <CTRPluginFramework.hpp>

#include <atomic>
#include <string>
#include <vector>

#include "azl/chat.hpp"
#include "azl/platform.hpp"
#include "azl/room_client.hpp"

namespace chat_overlay {

namespace {

using namespace CTRPluginFramework;

azl::chat::Log g_log;
azl::chat::Keyboard g_keyboard;
std::atomic<int> g_position{0}; // where the chat lines go on the top screen (plugin menu switch)
std::atomic<int> g_button{azl::chat::kDefaultButtonPosition}; // where the Chat button is (plugin menu switch)
azl::chat::TouchScrubber g_scrub;       // learns where the live touch sits in the HID shared page, and clears it there
std::atomic<bool> g_hide{false};        // true while the keyboard is open (or the CHAT button is being pressed): the game must not see the touch
std::atomic<bool> g_run{false};
// What the scrub thread saw of the touch just before it cleared it (the framework's own reading is cleared more often than it is looked at).
std::atomic<std::uint32_t> g_snap_xy{0};
std::atomic<std::uint32_t> g_snap_ms{0};
constexpr std::uint32_t kSnapFreshMs = 40; // a touch seen within this time is still down (the HID service writes a sample every few ms)
azl::platform::Thread g_scrub_thread;   // clears the touch every millisecond
azl::platform::Thread g_event_thread;   // clears it the moment the HID service has written a new sample (its pad event)
bool g_hiding = false;                  // latched: stays on until the finger is up, so a touch that began on the overlay never reaches the game
std::atomic<azl::RoomClient*> g_room{nullptr};
bool g_installed = false;

constexpr int kMargin = 4;

void DrawKey(const Screen& s, const azl::chat::Key& k, bool held, bool shift) {
    const Color face = held ? Color(70, 120, 220) : Color(40, 42, 50);
    s.DrawRect(k.x, k.y, k.w - 1, k.h - 1, face, true);
    char one[2] = {0, 0};
    const char* label = k.label;
    if (!label) {
        one[0] = shift ? k.shifted : k.normal;
        label = one;
    }
    const int len = static_cast<int>(std::char_traits<char>::length(label));
    s.Draw(label, k.x + (k.w - len * 6) / 2, k.y + (k.h - 10) / 2, Color::White, face);
}

// One pass of the guard: remember the sample the HID service wrote (the overlay reads the touch from this), then take it away from the game.
void HideTick() {
    volatile std::uint32_t* page = reinterpret_cast<volatile std::uint32_t*>(hidSharedMem);
    std::uint32_t xy = 0;
    if (g_scrub.Peek(page, xy)) {
        g_snap_xy.store(xy, std::memory_order_release);
        g_snap_ms.store(static_cast<std::uint32_t>(azl::platform::NowMs()) | 1u, std::memory_order_release);
    }
    g_scrub.Scrub(page);
}

// Called by the framework once per frame for each screen (the top one, the bottom one).
bool Frame(const Screen& screen) {
    const u64 now = azl::platform::NowMs();
    if (screen.IsTop) {
        azl::chat::Log::Line lines[azl::chat::kLines];
        const std::size_t n = g_log.Visible(now, lines);
        const int position = g_position.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i) {
            int x = 0, y = 0;
            azl::chat::ChatLineXY(position, static_cast<int>(lines[i].text.size()) * azl::chat::kCharWidth, static_cast<int>(i), static_cast<int>(n), x, y);
            screen.Draw(lines[i].text, x, y, Color::White, Color(0, 0, 0));
        }
        return true;
    }
    g_keyboard.SetButtonPosition(g_button.load(std::memory_order_relaxed));
    // bottom screen: the touch is read once per frame (here, where the bottom screen is drawn)
    const bool api_down = Touch::IsDown();
    UIntVector pos = api_down ? Touch::GetPosition() : UIntVector(0, 0);
    if (api_down) {
        g_scrub.Learn(reinterpret_cast<const volatile std::uint32_t*>(hidSharedMem), pos.x, pos.y); // (no-op until it sees a position it recognises)
    }
    // While the touch is hidden the page is cleared every millisecond, so the reading above often misses it: the scrub thread keeps the last
    // position it saw before clearing, and a touch seen within kSnapFreshMs counts as down.
    bool down = api_down;
    if (g_hide.load(std::memory_order_acquire)) {
        const std::uint32_t seen_ms = g_snap_ms.load(std::memory_order_acquire);
        const std::uint32_t xy = g_snap_xy.load(std::memory_order_acquire);
        if (seen_ms != 0 && static_cast<std::uint32_t>(now) - seen_ms < kSnapFreshMs && xy != 0) {
            down = true;
            pos = UIntVector(xy & 0xFFFFu, xy >> 16);
        }
    }
    const int tx = static_cast<int>(pos.x), ty = static_cast<int>(pos.y);
    const auto event = g_keyboard.OnTouch(down, tx, ty, now);
    if (event == azl::chat::Keyboard::Event::Send) {
        const std::string message = g_keyboard.TakeMessage();
        azl::RoomClient* room = g_room.load(std::memory_order_acquire);
        if (room && !message.empty()) {
            room->SendChat(message);
            g_log.Push("me", message, now);
        }
    }
    const auto inside = [&](const azl::chat::Key& k) {
        return down && tx >= k.x && tx < k.x + k.w && ty >= k.y && ty < k.y + k.h;
    };
    // The game's touchscreen is off while the keyboard is open, and for as long as a touch that began on the overlay lasts.
    const bool want_hide = g_keyboard.is_open() || inside(g_keyboard.button());
    if (want_hide) {
        g_hiding = true;
    } else if (!down) {
        g_hiding = false;
    }
    g_hide.store(g_hiding, std::memory_order_release);
    if (g_hiding) {
        HideTick(); // (and the threads do it between frames)
    }
    if (!g_keyboard.is_open()) {
        DrawKey(screen, g_keyboard.button(), false, false);
        return true;
    }
    screen.DrawRect(0, azl::chat::kTextBarY, 320, 240 - azl::chat::kTextBarY, Color(15, 16, 20), true);
    screen.Draw(g_keyboard.Tail(50) + "_", kMargin, azl::chat::kTextBarY + 6, Color::White, Color(15, 16, 20));
    std::size_t count = 0;
    const azl::chat::Key* keys = azl::chat::Keys(&count);
    for (std::size_t i = 0; i < count; ++i) {
        DrawKey(screen, keys[i], g_keyboard.held() == &keys[i], g_keyboard.shift());
    }
    return true;
}

} // namespace

bool Enabled() {
    return true;
}

void SetPosition(int position) {
    g_position.store(position >= 0 && position < azl::chat::kChatPositions ? position : 0, std::memory_order_relaxed);
}

int Position() {
    return g_position.load(std::memory_order_relaxed);
}

void SetButtonPosition(int position) {
    const int p = position >= 0 && position < azl::chat::kChatPositions ? position : azl::chat::kDefaultButtonPosition;
    g_button.store(p, std::memory_order_relaxed);
}

int ButtonPosition() {
    return g_button.load(std::memory_order_relaxed);
}

void Attach(azl::RoomClient& room) {
    room.SetChatCallback([](const std::string& nick, const std::string& message) {
        g_log.Push(nick, message, azl::platform::NowMs());
    });
    g_room.store(&room, std::memory_order_release);
    if (!g_installed) {
        g_installed = true;
        OSD::Run(Frame);
        // While the keyboard is open the HID page is cleared every millisecond, so the game rarely reads the touch before it is taken away.
        g_run.store(true, std::memory_order_release);
        g_scrub_thread.Start([] {
            while (g_run.load(std::memory_order_acquire)) {
                if (g_hide.load(std::memory_order_acquire)) {
                    HideTick();
                }
                azl::platform::SleepMs(1);
            }
        });
        // ... and again as soon as the HID service writes a sample (the wait resets the event first, so it cannot spin); the 1 ms thread covers the rest.
        g_event_thread.Start([] {
            while (g_run.load(std::memory_order_acquire)) {
                hidWaitForEvent(HIDEVENT_PAD0, true);
                if (g_hide.load(std::memory_order_acquire)) {
                    HideTick();
                }
            }
        });
    }
}

void Detach() {
    g_room.store(nullptr, std::memory_order_release);
    g_hide.store(false, std::memory_order_release);
    g_run.store(false, std::memory_order_release);
    g_scrub_thread.Join();
    g_event_thread.JoinTimeout(300); // (may be waiting for an event that does not come while the console sleeps: then it is left behind)
}

} // namespace chat_overlay

#else // AZL_CHAT_OVERLAY

namespace chat_overlay {
bool Enabled() {
    return false;
}
void SetPosition(int) {}
int Position() {
    return 0;
}
void SetButtonPosition(int) {}
int ButtonPosition() {
    return 2;
}
void Attach(azl::RoomClient&) {}
void Detach() {}
} // namespace chat_overlay

#endif
