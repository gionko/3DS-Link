// public_lobbies_test: what the page "Public lobbies" decides (ConfigApp/source/public_lobbies.hpp, plain C++): the lines of the list and of the
// user's file, what a new list keeps and takes away, the room in use that left the list, the titles and their regions, the order, the colours of
// the ping, the filters, the search, Quick join and the order of the members.
#include <cstdio>

#include "public_lobbies.hpp"

using namespace lobbies;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static Member M(const char* game, std::uint64_t id) {
    Member m;
    m.nickname = "p";
    m.game = game;
    m.game_id = id;
    return m;
}

static Probe Online(int ping, std::vector<Member> members, std::uint32_t slots = 4) {
    Probe p;
    p.state = State::Online;
    p.ping_ms = ping;
    p.has_info = true;
    p.slots = slots;
    p.members = members;
    return p;
}

static void Lines_() {
    Room r;
    CHECK(ParseRoomLine("1.2.3.4:5000:Kex Pub 1", r) && r.host == "1.2.3.4" && r.port == 5000 && r.name == "Kex Pub 1" && r.secret.empty());
    CHECK(ParseRoomLine("rooms.example.org:24872:Room:a:b:c\r", r) && r.secret == "a:b:c" && r.name == "Room");
    CHECK(!ParseRoomLine("1.2.3.4:5000", r));
    CHECK(!ParseRoomLine("1.2.3.4:0:Room", r));
    CHECK(!ParseRoomLine("1.2.3.4:70000:Room", r));
    CHECK(!ParseRoomLine("1.2.3.4:50a0:Room", r));
    CHECK(!ParseRoomLine("1.2.3.4:5000:", r));
    CHECK(ParseRoomLine("1.2.3.4:5000::pw", r, true) && r.name.empty() && r.secret == "pw");
    CHECK(!ParseRoomLine("bad host:5000:Room", r));
    CHECK(!ParseRoomLine("fe80::1:5000:Room", r));
    CHECK(ParseRoomLine("1.2.3.4:5000:A\tB", r) && r.name == "A B");
    CHECK(RoomLine(r) == "1.2.3.4:5000:A B");

    List list;
    CHECK(!ParseList("<html>not a list</html>", list));
    CHECK(!ParseList("#updated 2026-11-03\n", list));
    CHECK(!ParseList("1.2.3.4:5000:Room\n", list)); // (no #updated: not our file)
    CHECK(ParseList("#updated 2026-11-03\r\n#titles 2026-11-01\n# a comment\n1.2.3.4:5000:A\nbad line\n1.2.3.4:5000:Again\n5.6.7.8:1:B:pw\n", list));
    CHECK(list.updated == "2026-11-03" && list.titles_stamp == "2026-11-01" && list.rooms.size() == 2 && list.rooms[0].name == "A");
}

static void Local_() {
    Local local = ParseLocal("# header\n#kept 2026-11-03 9.9.9.9:1:Gone:pw\n1.2.3.4:5000:Mine:\n5.6.7.8:1::secret:with:colons\n1.1.1.1:1::\n");
    CHECK(local.has_kept && local.kept_since == "2026-11-03" && local.kept.name == "Gone" && local.kept.secret == "pw");
    CHECK(local.rooms.size() == 2 && local.rooms[1].secret == "secret:with:colons");
    const Local again = ParseLocal(RenderLocal(local));
    CHECK(again.rooms.size() == 2 && again.has_kept && again.kept.secret == "pw" && again.rooms[0].name == "Mine");

    Room room;
    ParseRoomLine("1.2.3.4:5000:List name", room);
    SetLocal(local, room, "", "");
    CHECK(local.rooms.size() == 1);
    SetLocal(local, room, "", "newpw");
    CHECK(local.rooms.size() == 2 && local.rooms.back().secret == "newpw" && local.rooms.back().name.empty());

    CHECK(SavedRoomName("Kex's Public Room (EU) #1", 64) == "Kex s Public Room EU 1");
    CHECK(SavedRoomName("Animal Crossing - New Leaf", 12) == "Animal Cross");
    CHECK(SavedRoomName("***", 64).empty());
}

static void Remember_() {
    List old_list, fresh;
    ParseList("#updated 2026-11-01\n1.1.1.1:1:A\n2.2.2.2:2:B\n3.3.3.3:3:C\n", old_list);
    ParseList("#updated 2026-11-02\n1.1.1.1:1:A2\n3.3.3.3:3:C\n", fresh);
    Local local = ParseLocal("1.1.1.1:1:My A:pa\n2.2.2.2:2:My B:pb\n");

    // the rooms that left the list take the user's name and password with them; the others keep them
    Outcome o = Remember(local, old_list, fresh, "", 0, false);
    CHECK(o.local.rooms.size() == 1 && o.local.rooms[0].name == "My A" && !o.local.has_kept && !o.deselect);

    // the room in use left: it is kept as the list had it, and what the user kept for it stays
    o = Remember(local, old_list, fresh, "2.2.2.2", 2, false);
    CHECK(o.local.has_kept && o.local.kept.name == "B" && o.local.kept_since == ListId(fresh) && !o.deselect);
    CHECK(o.local.rooms.size() == 2);
    const std::vector<Entry> entries = Entries(fresh, o.local);
    CHECK(entries.size() == 3 && entries.back().kept && entries.back().room.secret == "pb" && entries.back().room.name == "My B");
    CHECK(entries[0].own_name && entries[0].room.name == "My A" && entries[0].list_name == "A2");
    {
        // a name of the user's own taken away: the kept room has the list's name again
        Local without = o.local;
        Room b;
        ParseRoomLine("2.2.2.2:2:B", b);
        SetLocal(without, b, "", "pb");
        CHECK(Entries(fresh, without).back().room.name == "B" && Entries(fresh, without).back().room.secret == "pb");
    }

    // the same list again (opened twice): still kept
    Outcome same = Remember(o.local, fresh, fresh, "2.2.2.2", 2, false);
    CHECK(same.local.has_kept && !same.deselect);

    // the date renewed alone (a month without changes): the same list
    List renewed;
    ParseList("#updated 2026-12-02\n1.1.1.1:1:A2\n3.3.3.3:3:C\n", renewed);
    CHECK(ListId(renewed) == ListId(fresh));
    CHECK(Remember(same.local, fresh, renewed, "2.2.2.2", 2, false).local.has_kept);
    // a new list, still without it: not the room in use any more (also when it has the same date: it is the rooms that count)
    List newer;
    ParseList("#updated 2026-11-02\n1.1.1.1:1:A2\n", newer);
    CHECK(ListId(newer) != ListId(fresh));
    Outcome gone = Remember(same.local, fresh, newer, "2.2.2.2", 2, false);
    CHECK(gone.deselect && !gone.local.has_kept);
    CHECK(!FindRoom(gone.local.rooms, "2.2.2.2", 2)); // (and what the user kept for it goes with it)

    // it came back: nothing to keep
    List back;
    ParseList("#updated 2026-11-05\n2.2.2.2:2:B\n", back);
    Outcome returned = Remember(same.local, fresh, back, "2.2.2.2", 2, false);
    CHECK(!returned.deselect && !returned.local.has_kept);

    // a room the user saved, or one that was never public: the public list has no say
    CHECK(!Remember(local, old_list, fresh, "2.2.2.2", 2, true).local.has_kept);
    CHECK(!Remember(local, old_list, fresh, "8.8.8.8", 8, false).local.has_kept);
    // another room in use: the kept one is dropped
    CHECK(!Remember(o.local, fresh, newer, "1.1.1.1", 1, false).local.has_kept);
    // the list's own password is not the user's
    List with_secret;
    ParseList("#updated 2026-11-02\n4.4.4.4:4:D:listpw\n", with_secret);
    const std::vector<Entry> ws = Entries(with_secret, Local());
    CHECK(ws[0].room.secret == "listpw" && ws[0].list_secret == "listpw");
}

static Titles MakeTitles() {
    Titles t;
    t.Parse("#titles 2026-11-03\n# id:region:family\n"
            "126100:EUR:BFG\n126300:USA:BFG\n11D700:JPN:BFG\n"
            "55D00:FREE:EKJ\n55E00:FREE:EKJ\n"
            "7E900:EUR:ATK\n69F00:JPN:ATK\n"
            "16E700:EUR:BCE\n16E600:USA:BCE\n"
            "zz:EUR:ABC\n30700:EUR:AM\n");
    return t;
}

static void Titles_() {
    Titles t = MakeTitles();
    CHECK(t.size() == 9 && t.stamp() == "2026-11-03");
    CHECK(t.Find(0x0004000000126100ull) && t.Find(0x0004000000126100ull)->region == Region::EUR);
    CHECK(t.Find(0x0004000E00126100ull) && std::string(t.Find(0x0004000E00126100ull)->family, 3) == "BFG"); // (an update finds its game)
    CHECK(t.Find(0x0004008C0007E900ull) && t.Find(0x0004008C0007E900ull)->region == Region::EUR);         // (a DLC too)
    CHECK(!t.Find(0) && !t.Find(0x0004000000999900ull));
    Titles empty;
    CHECK(!empty.Parse("nothing\n"));
}

static void Order_() {
    Probe offline;
    offline.state = State::Offline;
    Probe waiting;
    Probe full = Online(30, {M("A", 1), M("B", 2), M("C", 3), M("D", 4)});
    Probe two_fast = Online(20, {M("A", 1), M("B", 2)});
    Probe two_slow = Online(90, {M("A", 1), M("B", 2)});
    Probe empty = Online(10, {});
    Probe one_fast = Online(20, {M("A", 1)});
    Probe full_fast = Online(5, {M("A", 1), M("B", 2), M("C", 3), M("D", 4)});
    CHECK(Before(two_fast, "z", two_slow, "a"));
    CHECK(Before(empty, "z", two_fast, "a"));     // (the lower ping first, whoever is in it)
    CHECK(Before(two_fast, "z", one_fast, "a"));  // (the same ping: the fuller room first)
    CHECK(!Before(one_fast, "a", two_fast, "z"));
    CHECK(Before(two_slow, "z", waiting, "a"));   // (a room not asked yet comes after every room with a slot ...)
    CHECK(Before(waiting, "z", full_fast, "a"));  // (... and before the full ones, whatever their ping)
    CHECK(Before(full_fast, "z", full, "a"));     // (the full ones by ping too)
    CHECK(Before(full, "z", offline, "a"));       // (the offline ones last)
    CHECK(Before(two_slow, "z", full_fast, "a"));
    Probe said_full = Online(30, {M("A", 1)});
    said_full.full = true;
    CHECK(IsFull(said_full) && IsFull(full) && !IsFull(two_fast));
    Probe no_info;
    no_info.state = State::Online;
    no_info.ping_ms = 40;
    CHECK(!IsFull(no_info) && Group(no_info) == 0);

    CHECK(TierOf(15) == PingTier::Blue && TierOf(16) == PingTier::Green && TierOf(35) == PingTier::Green);
    CHECK(TierOf(36) == PingTier::Yellow && TierOf(60) == PingTier::Yellow && TierOf(61) == PingTier::Red);
}

static void Filters_() {
    const Titles t = MakeTitles();
    std::vector<Owned> owned = {{0x126100, t.Find(0x126100)}, {0x55E00, t.Find(0x55E00)}};
    const Probe same = Online(30, {M("MH4U", 0x0004000000126100ull)});
    const Probe other_region = Online(30, {M("MH4U", 0x0004000000126300ull)});
    const Probe twin = Online(30, {M("Pokemon X", 0x0004000000055D00ull)});
    const Probe stranger = Online(30, {M("Tekken", 0x000400000007E900ull)});
    const Probe nobody = Online(30, {M("", 0)});
    CHECK(RoomMatch(same, owned, t) == Match::Same);
    CHECK(RoomMatch(other_region, owned, t) == Match::OtherRegion);
    CHECK(RoomMatch(twin, owned, t) == Match::Family);
    CHECK(RoomMatch(stranger, owned, t) == Match::None);
    CHECK(RoomMatch(Online(30, {M("MH4U", 0x0004000000126300ull), M("MH4U", 0x0004000000126100ull)}), owned, t) == Match::Same);

    CHECK(PassesKind(Kind::MyGames, other_region, owned, t) && !PassesKind(Kind::MyGames, stranger, owned, t) && !PassesKind(Kind::MyGames, nobody, owned, t));
    Probe full = Online(30, {M("A", 1)}, 1);
    CHECK(PassesKind(Kind::FreeSlots, same, owned, t) && !PassesKind(Kind::FreeSlots, full, owned, t));
    Probe offline;
    offline.state = State::Offline;
    CHECK(!PassesKind(Kind::FreeSlots, offline, owned, t) && PassesKind(Kind::All, offline, owned, t));

    CHECK(PassesRegion(Region::EUR, same, t) && !PassesRegion(Region::USA, same, t) && PassesRegion(Region::USA, other_region, t));
    CHECK(PassesRegion(Region::JPN, nobody, t) && PassesRegion(Region::JPN, offline, t)); // (a room where nobody plays: nothing says no)
    CHECK(PassesRegion(Region::Free, twin, t) && !PassesRegion(Region::EUR, twin, t));
    CHECK(!PassesRegion(Region::EUR, Online(30, {M("Unknown game", 0x0004000000999900ull)}), t));
}

static void Search_() {
    CHECK(SearchForm("MH4G / MH4U") == SearchForm("mh4gmh4u") && SearchForm("MH4G / MH4U").size() == 8);
    CHECK(SearchForm("Pok\xC3\xA9mon").size() == 7);                     // (an accented letter is one letter)
    CHECK(PartDistance(SearchForm("pokemon"), SearchForm("Pok\xC3\xA9mon X")) == 1);
    CHECK(SearchForm("\xC3\x89") == SearchForm("\xC3\xA9"));               // (and its capital is the same letter)
    CHECK(SearchForm("\xC3kex") == SearchForm("kex"));                        // (a lead byte without its continuation does not eat the letters after it)
    CHECK(SearchForm("kex\xC3") == SearchForm("kex") && SearchForm("\x80\xBF" "ab") == SearchForm("ab"));
    CHECK(AllowedErrors(3) == 0 && AllowedErrors(4) == 1 && AllowedErrors(19) == 1 && AllowedErrors(20) == 2 && AllowedErrors(35) == 3);
    CHECK(PartDistance(SearchForm("hunter"), SearchForm("monsterhunter4")) == 0);
    CHECK(PartDistance(SearchForm("huntre"), SearchForm("monsterhunter4")) == 1); // ("hunte" with an r more)
    CHECK(PartDistance(SearchForm("xyzq"), SearchForm("monsterhunter4")) == 4);
    CHECK(PartDistance(SearchForm("hunger"), SearchForm("monsterhunter4")) == 1);
    Entry e;
    e.room.name = "Kex Pub 1";
    e.list_name = "Kex Pub 1";
    Probe p = Online(30, {M("Monster Hunter 4 Ultimate", 0x126300)});
    p.name = "Kex's Public Monster Hunter Room (EU) #1";
    CHECK(Matches("", e, p));
    CHECK(Matches("kex", e, p));
    CHECK(Matches("mh4u", e, Online(30, {M("MH4U", 1)})));
    CHECK(Matches("monstr huntr", e, p) == false); // (two letters missing in 11: one is allowed)
    CHECK(Matches("monster huntr", e, p));
    CHECK(Matches("Ultimate", e, p));
    CHECK(!Matches("tekken", e, p));
    CHECK(!Matches("kez", e, p)); // (three letters: no error allowed)
}

static void Quick_() {
    const Titles t = MakeTitles();
    std::vector<Owned> owned = {{0x126100, t.Find(0x126100)}};
    std::vector<Entry> entries(5);
    entries[3].room.secret = "pw";
    std::vector<Probe> probes = {
        Online(20, {M("Tekken", 0x7E900)}),         // not my game
        Online(80, {M("MH4U", 0x126100)}),          // mine, slow
        Online(10, {M("MH4U", 0x126100)}, 1),       // mine, full
        Online(40, {M("MH4U", 0x126100)}),          // mine, password we have
        Online(5, {M("MH4U USA", 0x126300)}),       // the same game of another region: never
    };
    probes[3].lock = Lock::Password;
    CHECK(QuickPick(entries, probes, owned, t) == 3);
    entries[3].room.secret.clear();
    CHECK(QuickPick(entries, probes, owned, t) == 1);
    probes[1].state = State::Offline;
    CHECK(QuickPick(entries, probes, owned, t) == -1);
}

static void Members_() {
    const Titles t = MakeTitles();
    std::vector<Owned> owned = {{0x7E900, t.Find(0x7E900)}};
    Probe p = Online(30, {M("", 0), M("MH4U", 0x126100), M("Tekken", 0x7E900), M("MH4U", 0x126300), M("Odd", 0x999900), M("", 0)});
    const std::vector<std::size_t> order = MemberOrder(p, owned, t);
    CHECK(order.size() == 6);
    CHECK(order[0] == 1 && order[1] == 3); // (the two of Monster Hunter 4U, the biggest group)
    CHECK(order[2] == 2);                  // (then the user's game)
    CHECK(order[4] == 0 && order[5] == 5); // (who plays nothing last)
}

int main() {
    Lines_();
    Local_();
    Remember_();
    Titles_();
    Order_();
    Filters_();
    Search_();
    Quick_();
    Members_();
    std::printf("public_lobbies_test: %s (%d failure%s)\n", g_failures ? "FAILED" : "ok", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
