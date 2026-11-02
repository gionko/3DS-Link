// Built and run by Tests/run.sh: the chat core (Includes/azl/chat.hpp) and its packets (room_proto).
#include <cstdio>

#include "azl/chat.hpp"
#include "azl/room_proto.hpp"

using namespace azl;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static const chat::Key* KeyFor(char c, bool shifted = false) {
    std::size_t n = 0;
    const chat::Key* k = chat::Keys(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (k[i].kind == chat::Key::Char && (shifted ? k[i].shifted : k[i].normal) == c) return &k[i];
    }
    return nullptr;
}
static const chat::Key* KindKey(chat::Key::Kind kind) {
    std::size_t n = 0;
    const chat::Key* k = chat::Keys(&n);
    for (std::size_t i = 0; i < n; ++i) if (k[i].kind == kind) return &k[i];
    return nullptr;
}
static std::uint64_t t = 1000;
static chat::Keyboard::Event Tap(chat::Keyboard& kb, const chat::Key* k) {
    const int x = k->x + k->w / 2, y = k->y + k->h / 2;
    t += 50;
    auto e = kb.OnTouch(true, x, y, t);
    t += 50;
    kb.OnTouch(false, 0, 0, t);
    return e;
}

int main() {
    // --- cleaning: ASCII only, no control characters, collapsed spaces, cut
    CHECK(chat::Clean("  hello   world \n\t!", 100) == "hello world !");
    CHECK(chat::Clean("caf\xC3\xA9 ok", 100) == "caf ok");              // the two bytes of the e-acute are dropped
    CHECK(chat::Clean("\x1b[31mred\x07", 100) == "[31mred");             // ESC and BEL are gone
    CHECK(chat::Clean("abcdefghij", 4) == "abcd");
    CHECK(chat::Clean("", 10).empty());

    // --- packets
    std::vector<u8> pkt = room::EncodeChat("hi there");
    CHECK(pkt.size() == 1 + 4 + 8 && pkt[0] == room::IdChatMessage);
    std::string big(900, 'x');
    CHECK(room::EncodeChat(big).size() == 1 + 4 + room::kMaxChatChars);
    // a relayed packet: [id][nickname][username][message]
    room::PacketWriter w;
    w.U8(room::IdChatMessage); w.String("Vedy"); w.String("account"); w.String("ready?");
    std::string nick, msg;
    CHECK(room::DecodeChat(w.data().data(), w.data().size(), &nick, &msg) && nick == "Vedy" && msg == "ready?");
    CHECK(!room::DecodeChat(w.data().data(), 7, &nick, &msg));          // cut short
    u8 wrong[8] = {room::IdWifiPacket, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!room::DecodeChat(wrong, sizeof(wrong), &nick, &msg));

    // --- the log: oldest first, only what is recent, the ring forgets the oldest
    chat::Log log;
    chat::Log::Line out[chat::kLines];
    CHECK(log.Visible(10000, out) == 0);
    log.Push("A", "one", 10000);
    log.Push("B", "two", 11000);
    CHECK(log.Visible(12000, out) == 2 && out[0].text == "A: one" && out[1].text == "B: two");
    CHECK(log.Visible(10000 + chat::kShowMs + 1, out) == 1 && out[0].text == "B: two");   // the first expired
    for (int i = 0; i < 10; ++i) log.Push("C", "line" + std::to_string(i), 20000 + i);
    CHECK(log.Visible(20010, out) == chat::kLines && out[0].text == "C: line2" && out[chat::kLines - 1].text == "C: line9");
    log.Push("", std::string(300, 'z'), 30000);
    CHECK(log.Visible(30001, out) >= 1 && out[chat::kLines - 1].text.size() <= chat::kLineChars);
    {   // a long message is wrapped, not cut: every word is still there, no line is longer than kLineChars
        chat::Log wl;
        std::string msg, joined;
        for (int i = 0; i < 20; ++i) msg += "word" + std::to_string(i) + " ";
        wl.Push("Kai", msg, 40000);
        chat::Log::Line wo[chat::kLines];
        const std::size_t n = wl.Visible(40001, wo);
        bool fine = n >= 3 && wo[0].text.compare(0, 5, "Kai: ") == 0 && wo[1].text.compare(0, 2, "  ") == 0;
        for (std::size_t i = 0; i < n; ++i) { fine = fine && wo[i].text.size() <= chat::kLineChars; joined += wo[i].text + " "; }
        CHECK(fine);
        CHECK(joined.find("word0") != std::string::npos && joined.find("word19") != std::string::npos);   // (the 200-character cap is not reached here)
        wl.Push("Kai", "short", 41000);
        CHECK(wl.Visible(41001, wo) == n + 1 && wo[n].text == "Kai: short");
    }
    log.Push("D", "\x01\x02", 31000);                                    // nothing printable: not shown
    CHECK(log.Visible(31001, out) >= 1 && out[0].text.find("D:") == std::string::npos);

    // --- the keyboard
    chat::Keyboard kb;
    CHECK(!kb.is_open());
    CHECK(kb.OnTouch(true, 5, 200, 1000) == chat::Keyboard::Event::None);   // a tap elsewhere does nothing while closed
    kb.OnTouch(false, 0, 0, 1050);
    CHECK(kb.OnTouch(true, kb.button().x + 5, kb.button().y + 5, 1100) == chat::Keyboard::Event::Opened && kb.is_open());
    kb.OnTouch(false, 0, 0, 1150);
    CHECK(kb.OnTouch(true, 100, 5, 1200) == chat::Keyboard::Event::None && kb.is_open());   // a tap outside the keys is ignored
    kb.OnTouch(false, 0, 0, 1250);
    Tap(kb, KeyFor('h')); Tap(kb, KeyFor('i'));
    CHECK(kb.text() == "hi");
    Tap(kb, KindKey(chat::Key::Shift)); Tap(kb, KeyFor('y'));
    CHECK(kb.text() == "hiY" && !kb.shift());                         // shift lasts for one letter
    Tap(kb, KindKey(chat::Key::Shift)); Tap(kb, KeyFor('!', true));    // the '1' key with shift: '!'
    CHECK(kb.text() == "hiY!" && !kb.shift());
    Tap(kb, KindKey(chat::Key::Back));
    CHECK(kb.text() == "hiY");
    Tap(kb, KindKey(chat::Key::Space));
    CHECK(kb.text() == "hiY ");
    CHECK(Tap(kb, KindKey(chat::Key::Send)) == chat::Keyboard::Event::Send && kb.TakeMessage() == "hiY" && kb.text().empty());
    CHECK(kb.is_open());                                                    // stays open for the next line
    CHECK(Tap(kb, KindKey(chat::Key::Send)) == chat::Keyboard::Event::None);   // an empty box sends nothing
    // held touch must not repeat characters; backspace repeats after 450 ms
    Tap(kb, KeyFor('a')); Tap(kb, KeyFor('b')); Tap(kb, KeyFor('c'));
    const chat::Key* back = KindKey(chat::Key::Back);
    const int bx = back->x + 4, by = back->y + 4;
    kb.OnTouch(true, bx, by, 5000);                                      // press: erases one
    CHECK(kb.text() == "ab");
    kb.OnTouch(true, bx, by, 5200);
    CHECK(kb.text() == "ab");                                            // still inside the 450 ms
    kb.OnTouch(true, bx, by, 5500);
    CHECK(kb.text() == "a");                                             // repeat
    kb.OnTouch(false, 0, 0, 5600);
    // the length limit
    for (int i = 0; i < 150; ++i) Tap(kb, KeyFor('x'));
    CHECK(kb.text().size() <= chat::kTypedChars);
    CHECK(kb.Tail(10).size() == 10);
    CHECK(Tap(kb, KindKey(chat::Key::Close)) == chat::Keyboard::Event::Closed && !kb.is_open());
    // every key lies inside the 320 x 240 bottom screen and the keys do not overlap
    std::size_t n = 0;
    const chat::Key* keys = chat::Keys(&n);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(keys[i].x >= 0 && keys[i].x + keys[i].w <= 320 && keys[i].y >= chat::kKeysY && keys[i].y + keys[i].h <= 240);
        for (std::size_t j = i + 1; j < n; ++j) {
            const bool apart = keys[i].x + keys[i].w <= keys[j].x || keys[j].x + keys[j].w <= keys[i].x ||
                               keys[i].y + keys[i].h <= keys[j].y || keys[j].y + keys[j].h <= keys[i].y;
            CHECK(apart);
        }
    }

    // --- hiding the touch: it learns where the live touch is and clears only that
    {
        std::uint32_t page[0x140 / 4 + 4] = {};
        for (std::size_t w = 0; w < sizeof(page) / 4; ++w) page[w] = (0x01010101u * static_cast<std::uint32_t>(w & 0x7F)) & ~chat::TouchScrubber::kKeyTouch; // other data (no touch key in it): must stay
        std::uint32_t before[sizeof(page) / 4];
        for (std::size_t w = 0; w < sizeof(page) / 4; ++w) before[w] = page[w];
        chat::TouchScrubber ts;
        ts.Scrub(page);                                              // nothing learned: nothing written
        bool same = true;
        for (std::size_t w = 0; w < sizeof(page) / 4; ++w) same = same && page[w] == before[w];
        CHECK(same);
        // a finger at (100, 150): the ring holds the position in two entries and the key in the pad entries
        const std::uint32_t xy = 100u | (150u << 16);
        page[0xD4 / 4] = xy; page[0xDC / 4] = xy;
        page[0x28 / 4] = chat::TouchScrubber::kKeyTouch | 0x8;       // entry 0 current: touch + the A key
        page[0x38 / 4] = chat::TouchScrubber::kKeyTouch;             // entry 1 current
        page[0x3C / 4] = chat::TouchScrubber::kKeyTouch;             // entry 1 pressed
        for (std::size_t w = 0; w < sizeof(page) / 4; ++w) before[w] = page[w];
        ts.Learn(page, 100, 150);
        CHECK(ts.touch_slots() == 2 && ts.pad_learned());
        ts.Scrub(page);
        CHECK(page[0xD4 / 4] == 0 && page[0xDC / 4] == 0);
        CHECK(page[0x28 / 4] == 0x8);                                // the A key stays, the touch key goes
        CHECK(page[0x38 / 4] == 0 && page[0x3C / 4] == 0);
        bool others = true;
        for (std::size_t w = 0; w < sizeof(page) / 4; ++w) {
            if (w == 0xD4 / 4 || w == 0xDC / 4 || w == 0x28 / 4 || w == 0x38 / 4 || w == 0x3C / 4) continue;
            others = others && page[w] == before[w];
        }
        CHECK(others);                                               // everything else is untouched
        // a later touch at another place: the ring moved on, the new slot is learned, the old ones are cleared again
        page[0xE4 / 4] = 7u | (9u << 16);
        ts.Learn(page, 7, 9);
        CHECK(ts.touch_slots() == 3);
        ts.Scrub(page);
        CHECK(page[0xE4 / 4] == 0);
        {   // Peek: what the HID service wrote since the last clear, read before clearing
            std::uint32_t got = 0;
            CHECK(!ts.Peek(page, got));                                  // everything was cleared above
            page[0xD4 / 4] = 100u | (150u << 16);
            CHECK(ts.Peek(page, got) && got == (100u | (150u << 16)));
            CHECK(!ts.Peek(nullptr, got));
            ts.Scrub(page);
            CHECK(!ts.Peek(page, got));
        }
        chat::TouchScrubber none;                                    // a position that is nowhere in the page learns nothing
        none.Learn(page, 4000, 4000);
        CHECK(none.touch_slots() == 0);
        none.Learn(nullptr, 1, 1);
        none.Scrub(nullptr);                                         // (no crash without a page)
    }

    // --- the saved room lists and the picker
    {
        CHECK(chat::RoomListFile(1) == "servers.txt" && chat::RoomListFile(2) == "servers2.txt" && chat::RoomListFile(5) == "servers5.txt" && chat::RoomListFile(10) == "servers10.txt");
        chat::RoomEntry e;
        CHECK(chat::ParseRoomLine("192.168.1.20:25000|Home PC|pa|ss", e) && e.host == "192.168.1.20" && e.port == 25000 && e.name == "Home PC" &&
              e.password == "pa|ss");
        CHECK(chat::ParseRoomLine("mh.example.net", e) && e.port == 24872 && e.name.empty() && e.password.empty());
        CHECK(chat::ParseRoomLine("host||secret\r", e) && e.name.empty() && e.password == "secret");
        CHECK(!chat::ParseRoomLine("", e) && !chat::ParseRoomLine("# comment", e) && !chat::ParseRoomLine("bad host", e));
        CHECK(!chat::ParseRoomLine("h:0", e) && !chat::ParseRoomLine("h:70000", e) && !chat::ParseRoomLine("h:x", e) && !chat::ParseRoomLine(":24872", e));
        std::string text;
        for (int i = 0; i < 60; ++i) text += "h" + std::to_string(i) + ".net|r" + std::to_string(i) + "\n";
        CHECK(chat::ParseRoomList(text).size() == chat::kRoomsPerList);           // a long file is cut
        CHECK(chat::ParseRoomList("a\n\n#x\nb:1\r\nbad host\n").size() == 2);
        CHECK(chat::RoomLabel(chat::RoomEntry{"Home", "h.net", 24872, ""}) == "Home  h.net");
        CHECK(chat::RoomLabel(chat::RoomEntry{"", "h.net", 25000, ""}) == "h.net:25000");

        // pages of the menu's room browser
        CHECK(chat::PageCount(0) == 1 && chat::PageCount(1) == 1 && chat::PageCount(5) == 1 && chat::PageCount(6) == 2 && chat::PageCount(50) == 10);
        CHECK(chat::PageFirst(0) == 0 && chat::PageFirst(3) == 15);
        // the public list the app writes: "<players>/<max> name", "?" for a password
        {
            const std::vector<chat::RoomEntry> pub = chat::ParseRoomList("1.2.3.4:24872|2/4 MH Room\n5.6.7.8:25000|0/8 Locked|?\n", chat::kPublicRooms);
            CHECK(pub.size() == 2 && pub[0].name == "2/4 MH Room" && pub[0].password.empty() && pub[1].port == 25000 && pub[1].password == chat::kAskPassword);
            CHECK(chat::PublicRoomsFile() == "public_rooms.txt");
        }
        // the last room first, and whole names
        {
            std::vector<chat::RoomEntry> rs = chat::ParseRoomList("a.net:1|first\nB.net:2|second\nc.net:3|third\n");
            CHECK(chat::MoveRoomFirst(rs, "b.NET", 2) && rs.size() == 3 && rs[0].name == "second" && rs[1].name == "first" && rs[2].name == "third");
            CHECK(chat::MoveRoomFirst(rs, "b.net", 2) && rs[0].name == "second");        // already first
            CHECK(!chat::MoveRoomFirst(rs, "b.net", 3) && !chat::MoveRoomFirst(rs, "nope", 2) && rs.size() == 3);   // not in the list: nothing moves
            const std::string longname = "2/4 Monster Hunter XX - hunting rank 7+ only, no AFK please! [EU]";
            const std::vector<chat::RoomEntry> ln = chat::ParseRoomList("h.net:5|" + longname + "\n");
            CHECK(ln.size() == 1 && ln[0].name == longname);                                  // the whole name, with its special characters
        }
        // the position of the chat lines on the 400 x 240 top screen
        {
            int x = 0, y = 0;
            chat::ChatLineXY(0, 60, 0, 4, x, y);  CHECK(x == 4 && y == 4);                      // top-left, first line
            chat::ChatLineXY(0, 60, 3, 4, x, y);  CHECK(x == 4 && y == 4 + 3 * 12);
            chat::ChatLineXY(1, 60, 1, 4, x, y);  CHECK(x == 170 && y == 16);                   // top-middle: centred
            chat::ChatLineXY(2, 60, 0, 4, x, y);  CHECK(x == 400 - 4 - 60 && y == 4);           // top-right
            chat::ChatLineXY(3, 60, 3, 4, x, y);  CHECK(x == 4 && y == 240 - 4 - 12);           // bottom-left: newest line lowest
            chat::ChatLineXY(4, 60, 0, 4, x, y);  CHECK(x == 170 && y == 240 - 4 - 4 * 12);
            chat::ChatLineXY(5, 312, 0, 1, x, y); CHECK(x == 400 - 4 - 312 && y == 240 - 4 - 12);
            chat::ChatLineXY(9, 60, 0, 1, x, y);  CHECK(x == 4 && y == 4);                      // a wrong value is top-left
            chat::ChatLineXY(1, 500, 0, 1, x, y); CHECK(x == 0);                                // never off the left edge
            for (int p = 0; p < chat::kChatPositions; ++p) {
                chat::ChatLineXY(p, 312, 0, 4, x, y);
                CHECK(x >= 0 && x + 312 <= 400 && y >= 0 && y + 12 <= 240);                     // the widest line fits on the screen in all six
            }
            CHECK(std::string(chat::ChatPositionName(0)) == "Top-left" && std::string(chat::ChatPositionName(5)) == "Bottom-right" && std::string(chat::ChatPositionName(77)) == "Top-left");
            CHECK(true);
            int pl = 9, pb = 9;
            chat::ParseChatPositions("3 4\n", pl, pb);  CHECK(pl == 3 && pb == 4);
            chat::ParseChatPositions("5\n", pl, pb);    CHECK(pl == 5 && pb == chat::kDefaultButtonPosition);   // an old one-digit file
            chat::ParseChatPositions("", pl, pb);       CHECK(pl == 0 && pb == chat::kDefaultButtonPosition);
            chat::ParseChatPositions("x4", pl, pb);     CHECK(pl == 0 && pb == chat::kDefaultButtonPosition);
            chat::ParseChatPositions("7 1", pl, pb);    CHECK(pl == 0 && pb == chat::kDefaultButtonPosition);
            // the Chat button: small, inside the bottom screen, six different places, and the keyboard opens from where it is put
            for (int p = 0; p < chat::kChatPositions; ++p) {
                const chat::Key k = chat::ChatButtonKey(p);
                CHECK(k.w == chat::kButtonW && k.h == chat::kButtonH && k.x >= 0 && k.y >= 0 && k.x + k.w <= 320 && k.y + k.h <= 240);
                for (int q = 0; q < p; ++q) { const chat::Key o = chat::ChatButtonKey(q); CHECK(o.x != k.x || o.y != k.y); }
            }
            chat::Keyboard kb2;
            kb2.SetButtonPosition(3);
            const chat::Key b3 = chat::ChatButtonKey(3);
            CHECK(kb2.OnTouch(true, 300, 10, 100) == chat::Keyboard::Event::None && !kb2.is_open());        // the old top-right corner is just the game's now
            kb2.OnTouch(false, 0, 0, 110);
            CHECK(kb2.OnTouch(true, b3.x + 3, b3.y + 3, 120) == chat::Keyboard::Event::Opened && kb2.is_open());
        }
    }
    std::printf(fails ? "chat_test: %d FAILED\n" : "chat_test: ALL OK\n", fails);
    return fails ? 1 : 0;
}
