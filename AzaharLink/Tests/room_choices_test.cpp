// The lists of the in-game menu (Includes/azl/room_choices.hpp): the saved rooms of servers.txt and the public lobbies with the user's
// own names and passwords, read from the text of the files as the app writes them; and that the memory goes when the list closes.
#include <cstdio>
#include <string>

#include "azl/room_choices.hpp"

using namespace azl::rooms;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void Saved() {
    Choice c;
    CHECK(ParseSavedLine("10.0.0.5", c) && c.host == "10.0.0.5" && c.port == kDefaultPort && c.name.empty() && c.password.empty());
    CHECK(ParseSavedLine("192.168.1.20:24999|Home PC", c) && c.port == 24999 && c.name == "Home PC" && c.password.empty());
    CHECK(ParseSavedLine("room.example.org|Friends|se|cret\r", c) && c.name == "Friends" && c.password == "se|cret"); // (everything after the second bar)
    CHECK(ParseSavedLine("host||pw", c) && c.name.empty() && c.password == "pw");
    CHECK(!ParseSavedLine("", c) && !ParseSavedLine("bad host|x", c) && !ParseSavedLine("h:0|x", c) && !ParseSavedLine("h:70000", c));

    RoomFile f;
    f.Load(RoomFile::Kind::Saved, "127.0.0.1:25002|Explorers Guild\n# a comment\nnot a room!\n127.0.0.1:25003|Tekken Night\n10.0.0.5|Casa|pw\n");
    CHECK(f.size() == 3);
    CHECK(f.Get(1, c) && c.name == "Tekken Night" && c.port == 25003);
    CHECK(f.Get(2, c) && c.name == "Casa" && c.password == "pw" && c.port == kDefaultPort);
    CHECK(!f.Get(3, c));
}

static void Public() {
    Choice c;
    CHECK(ParsePublicLine("1.2.3.4:5000:Kex Pub 3", c, false) && c.name == "Kex Pub 3" && c.password.empty());
    CHECK(ParsePublicLine("1.2.3.4:5000:Locked:a:b", c, false) && c.name == "Locked" && c.password == "a:b");
    CHECK(!ParsePublicLine("1.2.3.4:5000:", c, false) && ParsePublicLine("1.2.3.4:5000::pw", c, true) && c.password == "pw");
    CHECK(!ParsePublicLine("1.2.3.4:5000", c, false) && !ParsePublicLine("1.2.3.4:x:Name", c, false));

    const std::string list = "#updated 2026-11-03\n#titles 2026-11-03\n1.2.3.4:5000:Kex Pub 3\nROOM.example:24872:Room Two:listpw\n9.9.9.9:1:Third\n";
    const std::string local = "# mine\n#kept 0123 5.5.5.5:1:Gone\nroom.EXAMPLE:24872:My Name:\n9.9.9.9:1::typed\n";
    RoomFile f;
    f.Load(RoomFile::Kind::Public, list, local);
    CHECK(f.size() == 3);
    CHECK(f.Get(0, c) && c.name == "Kex Pub 3" && c.password.empty());
    CHECK(f.Get(1, c) && c.name == "My Name" && c.password == "listpw"); // (the user's name, the list's password: the user kept none)
    CHECK(f.Get(2, c) && c.name == "Third" && c.password == "typed");    // (the list's name, the password the user typed)

    // a limit on the rooms read, and the memory given back
    f.Load(RoomFile::Kind::Public, list, local, 2);
    CHECK(f.size() == 2);
    f.Clear();
    CHECK(f.size() == 0 && !f.Get(0, c));
}

// public-lobbies-ping.txt (written by the app): the rooms take what it measured, and the order by ping is the app's (a free slot and the
// lower ping first, then the fuller room; not measured, full, offline after); by name it is the name's, in any case.
static void Pings() {
    const std::string list = "#updated x\nb.example:1:Bravo\na.example:2:alpha\nc.example:3:Charlie\nd.example:4:Delta\ne.example:5:Echo\n";
    const std::string pings = "#measured 1730650000\nb.example:1:40:2:4\nA.EXAMPLE:2:15:1:4\nc.example:3:15:3:4\nd.example:4:-1:0:0\n"
                              "e.example:5:5:4:4\nnot a line\nz.example:9:1:1:1\n";
    RoomFile f;
    f.Load(RoomFile::Kind::Public, list);
    CHECK(f.measured_at() == 0 && f.seen(0).ping == -2);
    f.ApplyPings(pings);
    CHECK(f.measured_at() == 1730650000ull);
    CHECK(f.seen(0).ping == 40 && f.seen(0).players == 2 && f.seen(0).slots == 4);
    CHECK(f.seen(1).ping == 15);   // (the address in another case is the same room)
    CHECK(f.seen(3).ping == -1);   // offline
    f.Sort(SortBy::Ping);
    Choice c;
    // Charlie 15 ms 3/4 before alpha 15 ms 1/4 (same ping: the fuller first), then Bravo 40 ms; Echo is full (after), Delta offline (last)
    const char* want[] = {"Charlie", "alpha", "Bravo", "Echo", "Delta"};
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(f.Get(i, c) && c.name == want[i]);
    }
    CHECK(f.seen(0).players == 3 && f.seen(4).ping == -1); // (what was measured moves with its room)
    f.Sort(SortBy::Name);
    const char* by_name[] = {"alpha", "Bravo", "Charlie", "Delta", "Echo"};
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(f.Get(i, c) && c.name == by_name[i]);
    }
    // no file: nothing measured, and the order by ping is the order by name
    RoomFile g;
    g.Load(RoomFile::Kind::Public, list);
    g.ApplyPings("");
    g.Sort(SortBy::Ping);
    CHECK(g.measured_at() == 0 && g.Get(0, c) && c.name == "alpha" && g.seen(0).ping == -2);
}

int main() {
    Saved();
    Public();
    Pings();
    std::printf("room_choices_test: %s (%d failures)\n", g_failures == 0 ? "ok" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
