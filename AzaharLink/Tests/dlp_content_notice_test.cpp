// H3 (1.1.4): the notice on the host's screen that names a guest ejected because it asked for the content of the child (azl/dlp_content_notice.hpp:
// what Plugin::NoteRealDlpHost does with what the adapter's TakeContentGuest gives), on a PC, pure. The adapter's side - the MAC of the guest that
// asked, once, and nothing for the guests that have their child - is part C and H of Tests/dlp_real_island_test.cpp.
//   N1 the room's name of the guest, in the text; "a player" when the room does not list it
//   N2 once per guest: the same guest again (another lobby, another ejection) gives no second notice; another guest does
//   N3 no MAC in the text, written in any of the ways the log writes one, whatever the name and the guest
//   N4 sixteen guests remembered, the seventeenth not told rather than told twice
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "azl/dlp_content_notice.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

using azl::Mac;
using azl::RoomMemberRow;
using azl::u32;
using azl::u8;
namespace dr = azl::dlp_real;

Mac M(u8 a, u8 b) {
    return Mac{0x02, 0xA1, 0x7E, 0x3C, a, b};
}

/// The MAC as the log and the room's lines write it, in upper and lower case, with ':' '-' or nothing between the bytes, and its bytes alone.
bool HasMac(const std::string& text, const Mac& m) {
    const char* const forms[] = {"%02X:%02X:%02X:%02X:%02X:%02X", "%02x:%02x:%02x:%02x:%02x:%02x", "%02X-%02X-%02X-%02X-%02X-%02X",
                                 "%02X%02X%02X%02X%02X%02X", "%02x%02x%02x%02x%02x%02x"};
    for (const char* f : forms) {
        char s[32];
        std::snprintf(s, sizeof(s), f, m[0], m[1], m[2], m[3], m[4], m[5]);
        if (text.find(s) != std::string::npos) {
            return true;
        }
    }
    return text.find(std::string(reinterpret_cast<const char*>(m.data()), m.size())) != std::string::npos;
}

/// What the plugin does for one guest the adapter gave: the notice's text, or "" when it says nothing (Plugin::NoteRealDlpHost).
std::string Notice(dr::ContentNotices& book, const std::vector<RoomMemberRow>& room, const Mac& guest) {
    if (!book.First(guest)) {
        return std::string();
    }
    return dr::ContentNoticeText(dr::GuestName(room, guest));
}

void N1_Name() {
    std::printf("N1 the room's name of the guest\n");
    const std::vector<RoomMemberRow> room = {{M(0, 1), "Daisy", "Mario Party: Island Tour", true},
                                             {M(0, 2), "Friend-A", "Download Play", true},
                                             {M(0, 3), "Az4", "", false}};
    dr::ContentNotices book;
    const std::string t = Notice(book, room, M(0, 2));
    std::printf("   \"%s\"\n", t.c_str());
    CHECK(t.find("Friend-A") != std::string::npos);
    CHECK(t.find("Daisy") == std::string::npos);
    CHECK(t.compare(0, 10, "3DS Link: ") == 0);
    CHECK(t.find("own copy") != std::string::npos);
    CHECK(t.find("no download through the room") != std::string::npos);
    CHECK(dr::GuestName(room, M(0, 3)) == "Az4");  // (a member that plays nothing is still named)
    // not in the room's list: "a player", never the MAC
    const std::string u = Notice(book, room, M(9, 9));
    std::printf("   \"%s\"\n", u.c_str());
    CHECK(u.find("a player") != std::string::npos);
    CHECK(!HasMac(u, M(9, 9)));
    CHECK(dr::GuestName({}, M(0, 2)).empty());
}

void N2_Once() {
    std::printf("N2 once per guest\n");
    const std::vector<RoomMemberRow> room = {{M(0, 1), "Daisy", "", true}, {M(0, 2), "Luigi", "", true}};
    dr::ContentNotices book;
    CHECK(!Notice(book, room, M(0, 2)).empty());
    for (int i = 0; i < 5; ++i) {
        CHECK(Notice(book, room, M(0, 2)).empty());  // (it asks again in the next lobby: nothing more on the screen)
    }
    const std::vector<RoomMemberRow> renamed = {{M(0, 2), "Luigi2", "", true}};
    CHECK(Notice(book, renamed, M(0, 2)).empty());  // (the same guest under another name: still the same guest)
    CHECK(Notice(book, room, M(0, 1)).find("Daisy") != std::string::npos);
    CHECK(Notice(book, room, M(0, 1)).empty());
    CHECK(book.count() == 2);
}

void N3_NoMac() {
    std::printf("N3 no MAC in the text\n");
    u32 texts = 0;
    for (u32 k = 0; k < 64; ++k) {
        const Mac g = M(static_cast<u8>(k * 37), static_cast<u8>(k * 11 + 1));
        std::vector<RoomMemberRow> room;
        if ((k & 1) != 0) {
            room.push_back({g, "P" + std::to_string(k), "", true});
        }
        dr::ContentNotices book;
        const std::string t = Notice(book, room, g);
        CHECK(!t.empty());
        CHECK(!HasMac(t, g));
        CHECK(t.find(':') == 8 && t.rfind(':') == 8);  // (the one ':' is the one after "3DS Link")
        texts += t.empty() ? 0u : 1u;
    }
    CHECK(texts == 64);
}

void N4_Full() {
    std::printf("N4 sixteen guests\n");
    dr::ContentNotices book;
    for (u32 i = 0; i < dr::ContentNotices::kGuests; ++i) {
        CHECK(book.First(M(1, static_cast<u8>(i))));
    }
    CHECK(book.count() == 16);
    CHECK(!book.First(M(2, 0)));  // (not remembered: not told, rather than told twice)
    CHECK(!book.First(M(2, 0)));
    CHECK(!book.First(M(1, 3)));
    CHECK(book.count() == 16);
}

}  // namespace

int main() {
    std::printf("dlp_content_notice_test: the guest ejected for the content, named on the host's screen (H3)\n");
    N1_Name();
    N2_Once();
    N3_NoMac();
    N4_Full();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
