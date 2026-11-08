// A scripted walk through the app for its screenshots, in a test build only (make EXTRA_CFLAGS=-DAZL_TOUR): nothing of it is in a release.
// The script is sdmc:/azl-tour.txt, one command a line:
//   wait N        N frames with no key
//   key K         K pressed for one frame (A B X Y L R START SELECT UP DOWN LEFT RIGHT)
//   text T        what the next software keyboard would give (it is not opened)
//   shot NAME     both screens to sdmc:/azl-shots/NAME-top.bmp and NAME-bottom.bmp (after two quiet frames)
//   quit          the app ends
//   nofetch       the page "Public lobbies" does not fetch its lists (it shows the files put on the card)
//   open PAGE     opens a page over the current one: servers (Azahar Emulator Room), lobbies (Public lobbies) or settings (Settings)
//   homeoff       the bars say "HOME off" from now on, as during the copies of the plugin (only the badge: HOME itself is not touched)
// While the script runs, the pad and the touch screen are not read.
#pragma once

#include <3ds.h>

#include <string>

namespace tour {

#ifdef AZL_TOUR
// Reads the script once (false when there is none).
bool Begin();
bool Active();
// The keys of this frame.
u32 Keys();
// The text for a keyboard the app wants to open: true when the script had one.
bool TakeText(std::string& value);
// After a frame was shown: takes the screenshot the script asks for, and says whether the app should end.
bool AfterFrame();
bool SkipFetch();
// The page the script wants opened ("" = none): taken once.
std::string TakeOpen();
// Whether the script said "homeoff".
bool HomeOff();
#else
inline bool Begin() {
    return false;
}
inline bool Active() {
    return false;
}
inline u32 Keys() {
    return 0;
}
inline bool TakeText(std::string&) {
    return false;
}
inline bool AfterFrame() {
    return false;
}
inline bool SkipFetch() {
    return false;
}
inline std::string TakeOpen() {
    return std::string();
}
inline bool HomeOff() {
    return false;
}
#endif

} // namespace tour
