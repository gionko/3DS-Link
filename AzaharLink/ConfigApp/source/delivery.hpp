// Titles Delivery: what its screens share. The module that does the work (the list of the installed titles, the room of the sender, the scan
// for senders, the receiving and the installing) is reached through titleshare_host/delivery.h; the screens are drawn here, with the rest of
// 3DS Link's interface, in English. See pages_delivery.cpp (the pages) and delivery_run.cpp (the screens that run while the module is sending
// or receiving).
#pragma once

#include <3ds.h>

#include <functional>
#include <string>

#include "../titleshare_host/delivery.h"
#include "delivery_eta.hpp"
#include "ui.hpp"

namespace delivery {

// A room carries at most this many titles, and serves this many receivers (the sender is one of its places).
constexpr int kMaxTitles = DL_MAX_TITLES;
constexpr int kRoomReceivers = DL_MAX_CLIENTS - 1;

// "812 KB", "380 MB", "1.2 GB".
std::string SizeText(u64 bytes);
// "under a minute", "about 12 min", "about 1 h 20 min".
std::string DurationText(double seconds);
// The same for a narrow place (the system font has no tilde): "<1 min", "12 min", "1 h 20 min".
std::string ShortDurationText(double seconds);
// The speed to assume while none has been measured (bytes a second): the average of this console.
double SafeBytesPerSecond();
// The time `bytes` take at that speed.
double EstimateSeconds(u64 bytes);
// A "label value" pair on a line: the label dim, the value bright. Returns where the pair ends (a gap after it).
float DrawPair(float x, float yc, float scale, const std::string& label, const std::string& value, u32 color = ui::col::kText);
// "Game", "Update", "DLC 02" (the tid says which DLC), "Other".
std::string KindText(int kind, u64 tid);
// "12:34" / "1:02:03" for a time that is left; "--:--" when it is not known.
std::string ClockText(double seconds);
// What a kind of title looks like on every screen: game blue, update green, DLC red (the colour of its name and of its plate G / U / D).
u32 KindColor(int kind);
unsigned KindTag(int kind); // kTagGame | kTagUpdate | kTagDlc (0 for another kind)

// Tells the module what the app's scan made. True when it is there (the scan is over and found a context).
bool AttachModule();

// Runs `fn` on a thread of its own with a big stack and waits for it. The module's loops call back into the screens, which draw: that needs
// more stack than the main thread has to spare. False when the thread could not be made: `fn` was NOT run (on the main thread's small stack the
// module would overflow it).
bool RunOnBigStack(const std::function<void()>& fn);

// One frame of a page from inside a callback of the module (the app's main loop is not running then): the input is read, the page is
// updated, the page is drawn. `wait` = false never waits for the screen (a transfer must not stand still for a frame).
void PumpInput();
bool DrawPage(ui::Page& page, bool wait);

// The local wireless, held for as long as the object lives: the sockets are put away, the module's wireless is opened, and at the end both go
// back. Open() says whether the wireless could be opened (`error` has the result code when it could not).
class Wireless {
public:
    Wireless() = default;
    ~Wireless();
    Wireless(const Wireless&) = delete;
    Wireless& operator=(const Wireless&) = delete;
    bool Open();
    void Close();
    u32 error() const {
        return error_;
    }

private:
    bool open_ = false;
    u32 error_ = 0;
};

// The message for a wireless that could not be opened.
std::string WirelessErrorText(u32 error);

// What the user has seen of a sending or a receiving, for the page that closes it.
struct ReceiveResult {
    dl_recv_stats stats{};
    std::string room;  // the name of the console that sent
    bool cancelled = false;
    // What the receiving really took (measured with the app's clock, in milliseconds): when it began (the user confirmed what to take; 0 =
    // it never began), when its last look was, and for each title how long after the start it was installed (0 = not installed).
    u64 start_ms = 0;
    u64 end_ms = 0;
    u64 done_ms[DL_MAX_TITLES] = {};
};

// Starts the screens that run during a sending / a receiving. Both return when the room is closed / the receiving is over.
// RunReceive joins the room that dl_scan_pick took; `room_name` is the name of the console that sent (what the scan showed).
void RunSend();
void RunReceive(const std::string& room_name);

} // namespace delivery

namespace pages {

ui::Page* NewDeliveryPage();            // the menu: Send Titles, Receive Titles
ui::Page* NewSendTitlesPage();
ui::Page* NewReceiveTitlesPage();       // the rooms around
ui::Page* NewReceiveResultPage(const delivery::ReceiveResult& result);
ui::Page* NewDlpHostPage();             // 3DS Link Delivery: this console is a Download Play host that hands 3DS Link to one console at a time (pages_dlp.cpp)

} // namespace pages
