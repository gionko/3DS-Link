// The guard of the ids of the prepared children (1.1.4, the build 2h of 2026-11-19 evening: ConfigApp/source/dlp_guest_ids.cpp, specifiche/6-APP-boot9.md
// 15.12), the very file, built on a PC against a made-up console and a folder called "sdmc:". What it holds:
//   - the census: every source read, its count and FNV; a refresh puts in again ONLY the sources that changed, and says which, with the milliseconds;
//     a list that fails is "not read" (R1: nothing is written then), and only the SD card, the NAND, the tickets and the processes are needed;
//   - ids.cache: the text and back, the seal and the version (a file cut, edited, of another version, or whose lists do not match their lines is
//     never trusted: LoadCensus says false and the caller reads the console again);
//   - the three ids of a slot (the system form 00040010, the old 00040000, M3c1's 00040001), a process found without its low byte (pm's own way);
//   - JudgeInstall: every IdTaken - a title of the SD card that is not ours at ANY form's id, the NAND (a title or one begun), a ticket of another
//     title, the game card, a process (a closed game still in memory too), a title begun by nobody of ours, a list not read; ours: Free;
//   - JudgeDelete: never an id of the NAND, of a process, of the card, never a title that is not ours, never without our record;
//   - TicketMayGo (R7), AmMayWrite (the second guard), SlotIsSound (R0: never 3DS Link, with or without its low byte);
//   - no game of the catalogue and no system title known has its system id in the band 00040010|0F8xxxxx: the prepared ids never meet them.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "azl/title_rules.hpp"
#include "dlp_guest_ids.hpp"
#include "dlp_guest_paths.hpp"
#include "dlp_guest_store.hpp"
#include "fileio.hpp"

namespace fs = std::filesystem;
using namespace dlpguest;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)

constexpr u64 kSlot = 0x000400000F8F8200ull;    // Island Tour's key
constexpr u64 kSys = 0x000400100F8F8200ull;     // ... its system id (the form made since the build 2h)
constexpr u64 kM3c1 = 0x000400010F8F8200ull;    // ... M3c1's id of AM (the builds 2f/2g)

static void Clean() {
    fs::remove_all("sdmc:");
    mkdir("sdmc:", 0777);
}

// A made-up console: one list per source, a list that fails, a clock that moves a millisecond per call.
struct FakeReader final : IdReader {
    std::vector<u64> lists[kSrcCount];
    bool fails[kSrcCount] = {};
    int reads[kSrcCount] = {};
    u64 now = 1000;
    bool List(int source, std::vector<u64>* ids) override {
        ++reads[source];
        if (fails[source]) {
            return false;
        }
        *ids = lists[source];
        return true;
    }
    u64 NowMs() override {
        return now++;
    }
};

static LiveSlot Live(FakeReader& r) {
    IdCensus c;
    RefreshCensus(&c, r, nullptr);
    return SlotIn(c, kSlot);
}

static void TestCensus() {
    FakeReader r;
    r.lists[kSrcSd] = {0x00040000000F8200ull, kSys, 0x000400000F83D100ull};
    r.lists[kSrcNand] = {0x0004001000022100ull, 0x0004013000001502ull};
    r.lists[kSrcTicket] = {0x00040000000F8200ull, kSys};
    r.lists[kSrcProcess] = {0x000400000F83D100ull, 0x0004013000003202ull};
    IdCensus c;
    RefreshReport rep;
    CHECK(RefreshCensus(&c, r, &rep));
    CHECK(c.valid && rep.changed == (1u << kSrcCount) - 1 && rep.unread == 0); // (the first time every source is put in)
    CHECK(c.src[kSrcSd].read && c.src[kSrcSd].count == 3 && c.src[kSrcSd].fnv == FnvOfIds(c.ids[kSrcSd]) && c.ids[kSrcSd].front() < c.ids[kSrcSd].back());
    CHECK(c.src[kSrcCard].read && c.src[kSrcCard].count == 0);
    // nothing changed: nothing is put in again (the lists are still read: a few milliseconds each)
    CHECK(RefreshCensus(&c, r, &rep) && rep.changed == 0 && r.reads[kSrcSd] == 2);
    // the tickets changed (one more, same count elsewhere): only that source
    r.lists[kSrcTicket].push_back(0x000400000F8F8200ull);
    CHECK(RefreshCensus(&c, r, &rep) && rep.changed == SrcBit(kSrcTicket) && c.src[kSrcTicket].count == 3);
    // a title swapped for another (the same count): its FNV says it
    r.lists[kSrcSd][2] = 0x000400000F800100ull;
    CHECK(RefreshCensus(&c, r, &rep) && rep.changed == SrcBit(kSrcSd));
    const std::string line = RefreshLine(c, rep, "the test");
    CHECK(line.find("ids (the test): sd 3 (") == 0 && line.find("changed: sd;") != std::string::npos && line.find(" ms in all") != std::string::npos);
    // a needed list that fails: not read, and RefreshCensus says false (R1); the card or the pending titles that fail: read stays true
    r.fails[kSrcCard] = true;
    CHECK(RefreshCensus(&c, r, &rep) && !c.src[kSrcCard].read && (rep.unread & SrcBit(kSrcCard)));
    r.fails[kSrcCard] = false;
    for (const int s : {kSrcSd, kSrcNand, kSrcTicket, kSrcProcess}) {
        r.fails[s] = true;
        IdCensus d;
        CHECK(!RefreshCensus(&d, r, &rep) && !d.src[s].read && d.ids[s].empty());
        CHECK(RefreshLine(d, rep, "x").find("NOT READ") != std::string::npos);
        r.fails[s] = false;
    }
}

static void TestCacheFile() {
    Clean();
    FakeReader r;
    r.lists[kSrcSd] = {kSys, 0x00040000000F8200ull};
    r.lists[kSrcNand] = {0x0004001000022100ull};
    r.lists[kSrcTicket] = {kSys, 0x00040000000F8200ull};
    r.lists[kSrcProcess] = {0x000400000F83D100ull};
    r.fails[kSrcPendingNand] = true;
    IdCensus c;
    RefreshCensus(&c, r, nullptr);
    const std::string text = CensusText(c);
    CHECK(text.compare(0, 16, "#dlp-ids 1\nseal ") == 0);
    CHECK(text.find("000400100F8F8200:05\n") != std::string::npos); // (the SD card and a ticket: bits 0 and 2)
    IdCensus back;
    CHECK(ParseCensus(text, &back) && back.valid);
    for (int s = 0; s < kSrcCount; ++s) {
        CHECK(back.ids[s] == c.ids[s] && back.src[s].read == c.src[s].read && back.src[s].count == c.src[s].count && back.src[s].fnv == c.src[s].fnv);
    }
    CHECK(SameSlot(SlotIn(back, kSlot), SlotIn(c, kSlot)));
    // never trusted when it is not whole: a byte changed, the seal changed, cut, another version, a line that does not read, a list that is not
    // the one its line says (sealed again by hand)
    std::string t = text;
    t[t.size() - 3] = t[t.size() - 3] == '0' ? '1' : '0';
    CHECK(!ParseCensus(t, &back));
    t = text;
    t[17] = t[17] == '0' ? '1' : '0';
    CHECK(!ParseCensus(t, &back));
    CHECK(!ParseCensus(text.substr(0, text.size() - 4), &back));
    t = text;
    t[9] = '2';
    CHECK(!ParseCensus(t, &back));
    CHECK(!ParseCensus("", &back) && !ParseCensus("#dlp-ids 1\nseal 0000000000000000\n", &back));
    // the file: saved and loaded; a file that does not read whole gives false and an empty census (the caller reads the console)
    CHECK(SaveCensus(c) && IdsCachePath() == std::string("sdmc:/3ds/3DSLink/dlp-guests/") + "0123456789abcdef" + "0123456789abcdef/ids.cache"); // (DPAPP-2j)
    // (DPAPP-2j, R-B) the ID0 not known: no census file - and another console's is never read
    SetThisId0("");
    CHECK(IdsCachePath().empty() && !SaveCensus(c));
    SetThisId0(std::string("fedcba9876543210") + "fedcba9876543210");
    IdCensus elsewhere;
    CHECK(!LoadCensus(&elsewhere) && !elsewhere.valid);
    SetThisId0(std::string("0123456789abcdef") + "0123456789abcdef");
    IdCensus loaded;
    CHECK(LoadCensus(&loaded) && loaded.valid && loaded.ids[kSrcSd] == c.ids[kSrcSd]);
    CHECK(setup::WriteTextFile(IdsCachePath(), text.substr(0, text.size() - 1) + "1\n"));
    CHECK(!LoadCensus(&loaded) && !loaded.valid);
    IdCensus none;
    CHECK(!SaveCensus(none)); // (a census never read is never written)
}

static void TestSlot() {
    FakeReader r;
    r.lists[kSrcSd] = {kSys};
    r.lists[kSrcTicket] = {kSys, kM3c1};
    r.lists[kSrcProcess] = {0x000400000F8F8205ull}; // (the key's process with another low byte: pm's search drops it)
    LiveSlot l = Live(r);
    CHECK((l.read & kNeededSources) == kNeededSources);
    CHECK(l.mask[kFormSystem] == (SrcBit(kSrcSd) | SrcBit(kSrcTicket)));
    CHECK(l.mask[kFormDlpChild] == (SrcBit(kSrcTicket) | SrcBit(kSrcProcess))); // (M3c1's process id is the key)
    CHECK(l.mask[kFormApplication] == SrcBit(kSrcProcess));
    // (DPAPP-2j: the forms in the order of kForms - M3c1, the one made, first)
    CHECK(SlotText(l).find("dlp ticket+process") == 0 && SlotText(l).find("sys sd+ticket") != std::string::npos && SlotText(l).find("app process") != std::string::npos);
    IdCensus never;
    CHECK(SlotIn(never, kSlot).read == 0);
    // another slot, with nothing of these: Free of everything
    const LiveSlot other = SlotIn([&] {
        IdCensus c;
        RefreshCensus(&c, r, nullptr);
        return c;
    }(), 0x000400000F84C100ull);
    CHECK(other.mask[0] == 0 && other.mask[1] == 0 && other.mask[2] == 0);
}

static void TestJudge() {
    Ownership none;            // no record: nothing is ours
    Ownership ours;            // a record whose titles are ours in every form, its ticket too
    ours.record = true;
    ours.sd_ours = FormBit(kFormSystem) | FormBit(kFormApplication) | FormBit(kFormDlpChild);
    ours.ticket_ours = true;
    FakeReader r;
    r.lists[kSrcProcess] = {0x000400000F83D100ull};
    CHECK(JudgeInstall(Live(r), kFormSystem, none) == IdTaken::Free); // (an empty slot: free)
    // a title of the SD card that is not ours, at ANY form's id
    for (const u64 id : {kSys, kSlot, kM3c1}) {
        r.lists[kSrcSd] = {id};
        CHECK(JudgeInstall(Live(r), kFormSystem, none) == IdTaken::Sd);
        CHECK(JudgeInstall(Live(r), kFormSystem, ours) == IdTaken::Free);
        Ownership only_sys = ours;
        only_sys.sd_ours = FormBit(kFormSystem);
        CHECK(JudgeInstall(Live(r), kFormSystem, only_sys) == (id == kSys ? IdTaken::Free : IdTaken::Sd));
    }
    r.lists[kSrcSd].clear();
    // the NAND: a title or one begun, at any form's id, ours or not (R2)
    for (const int src : {kSrcNand, kSrcPendingNand}) {
        for (const u64 id : {kSys, kSlot, kM3c1}) {
            r.lists[src] = {id};
            CHECK(JudgeInstall(Live(r), kFormSystem, ours) == IdTaken::Nand);
            CHECK(JudgeInstall(Live(r), kFormApplication, ours) == IdTaken::Nand);
        }
        r.lists[src].clear();
    }
    // a process with an id of the slot (a closed game still in memory: the 2g zombie '3STARS' at 000400000F8F8200), its low byte or not (R3)
    for (const u64 id : {kSlot, kSys, kSlot | 0x7F}) {
        r.lists[kSrcProcess] = {0x000400000F83D100ull, id};
        CHECK(JudgeInstall(Live(r), kFormSystem, ours) == IdTaken::Running);
    }
    r.lists[kSrcProcess] = {0x000400000F83D100ull};
    // the game card (R4)
    r.lists[kSrcCard] = {kSlot};
    CHECK(JudgeInstall(Live(r), kFormSystem, ours) == IdTaken::Card);
    r.lists[kSrcCard].clear();
    // a ticket of the id written that is not ours (R5); one of ANOTHER form's id does not stop an install in this one
    r.lists[kSrcTicket] = {kSys};
    CHECK(JudgeInstall(Live(r), kFormSystem, none) == IdTaken::Ticket);
    Ownership record_no_ticket = ours;
    record_no_ticket.ticket_ours = false;
    CHECK(JudgeInstall(Live(r), kFormSystem, record_no_ticket) == IdTaken::Ticket);
    CHECK(JudgeInstall(Live(r), kFormSystem, ours) == IdTaken::Free);
    CHECK(JudgeInstall(Live(r), kFormApplication, none) == IdTaken::Free);
    r.lists[kSrcTicket].clear();
    // a title begun at the id that no run of ours began (no record)
    r.lists[kSrcPendingSd] = {kSys};
    CHECK(JudgeInstall(Live(r), kFormSystem, none) == IdTaken::Pending);
    CHECK(JudgeInstall(Live(r), kFormSystem, ours) == IdTaken::Free);
    r.lists[kSrcPendingSd].clear();
    // a needed list not read: Unknown, whatever else (R1); the card that does not answer is not one of them
    for (const int s : {kSrcSd, kSrcNand, kSrcTicket, kSrcProcess}) {
        r.fails[s] = true;
        CHECK(JudgeInstall(Live(r), kFormSystem, ours) == IdTaken::Unknown);
        CHECK(JudgeDelete(Live(r), kFormSystem, ours) == IdTaken::Unknown);
        r.fails[s] = false;
    }
    r.fails[kSrcCard] = true;
    CHECK(JudgeInstall(Live(r), kFormSystem, ours) == IdTaken::Free);
    r.fails[kSrcCard] = false;
    // the order of the reasons: the NAND before everything
    r.lists[kSrcNand] = {kSys};
    r.lists[kSrcSd] = {kSys};
    r.lists[kSrcCard] = {kSys};
    CHECK(JudgeInstall(Live(r), kFormSystem, none) == IdTaken::Nand);
    r.lists[kSrcNand].clear();
    r.lists[kSrcCard].clear();
    // JudgeDelete: only our title; never the NAND, a process, the card; never without our record (R6)
    CHECK(JudgeDelete(Live(r), kFormSystem, ours) == IdTaken::Free);
    CHECK(JudgeDelete(Live(r), kFormSystem, none) == IdTaken::Sd);
    Ownership record_not_this = ours;
    record_not_this.sd_ours = FormBit(kFormApplication);
    CHECK(JudgeDelete(Live(r), kFormSystem, record_not_this) == IdTaken::Sd);
    r.lists[kSrcSd].clear();
    CHECK(JudgeDelete(Live(r), kFormSystem, record_not_this) == IdTaken::Free); // (nothing on the SD card: the pending part and the ticket of a run)
    Ownership no_record;
    CHECK(JudgeDelete(Live(r), kFormSystem, no_record) == IdTaken::Sd);
    r.lists[kSrcNand] = {kSys};
    CHECK(JudgeDelete(Live(r), kFormSystem, ours) == IdTaken::Nand);
    CHECK(JudgeDelete(Live(r), kFormApplication, ours) == IdTaken::Free); // (another form's id in the NAND does not stop the delete of this one)
    r.lists[kSrcNand].clear();
    r.lists[kSrcProcess] = {kSys};
    CHECK(JudgeDelete(Live(r), kFormSystem, ours) == IdTaken::Running);
    r.lists[kSrcProcess] = {kSlot};
    CHECK(JudgeDelete(Live(r), kFormDlpChild, ours) == IdTaken::Running); // (M3c1's process is the key)
    r.lists[kSrcProcess].clear();
    r.lists[kSrcCard] = {kSys};
    CHECK(JudgeDelete(Live(r), kFormSystem, ours) == IdTaken::Card);
    // every reason has a word for the log
    for (const IdTaken t : {IdTaken::Free, IdTaken::Unknown, IdTaken::Nand, IdTaken::Running, IdTaken::Card, IdTaken::Sd, IdTaken::Ticket, IdTaken::Pending}) {
        CHECK(std::string(TakenWord(t)).size() > 3 && std::string(TakenWord(t)) != "?");
    }
}

static void TestTicketAndSecondGuard() {
    Guest g;
    g.form = kFormSystem;
    SayTicketOurs(g); // (DPAPP-2h-sec: with the stamp of this console)
    CHECK(g.ticket_known && g.ticket_ours && g.console == ThisConsole() && g.console != 0);
    CHECK(TicketMayGo(g, kFormSystem, false));                // (the record says it)
    {
        // (DPAPP-2h-sec) a record of ANOTHER console (a card moved, copied, shared): its word is of that console's ticket.db - the legacy rule here
        Guest other = g;
        other.console = ConsoleStampOf(0x2222);
        CHECK(!TicketWordHere(other) && !TicketMayGo(other, kFormSystem, false) && TicketMayGo(other, kFormSystem, true));
        other.console = 0; // (a record that says no console)
        CHECK(!TicketWordHere(other) && !TicketMayGo(other, kFormSystem, false));
        const u32 here = ThisConsole();
        SetThisConsole(0); // (this console's stamp not known: no word holds)
        CHECK(!TicketWordHere(g) && !TicketMayGo(g, kFormSystem, false));
        SetThisConsole(here);
        CHECK(TicketWordHere(g));
    }
    g.ticket_ours = false;
    CHECK(!TicketMayGo(g, kFormSystem, true));                // (the record says it is NOT ours: never, whatever is listed)
    g.ticket_known = false;
    CHECK(TicketMayGo(g, kFormSystem, true) && !TicketMayGo(g, kFormSystem, false)); // (the builds before: our title listed now)
    g.ticket_known = true;
    g.ticket_ours = true;
    CHECK(!TicketMayGo(g, kFormApplication, false) && TicketMayGo(g, kFormApplication, true)); // (another form: the legacy rule)
    // AmMayWrite: the second guard, on the lists read again
    FakeReader r;
    r.lists[kSrcProcess] = {0x000400000F83D100ull};
    IdCensus c;
    RefreshCensus(&c, r, nullptr);
    CHECK(AmMayWrite(c, kSys, kSys, true, false, false) && AmMayWrite(c, kSys, kSys, false, false, false));
    CHECK(!AmMayWrite(c, 0, kSys, false, false, false));
    CHECK(!AmMayWrite(c, 0x0004001000022100ull, 0x0004001000022100ull, false, false, false)); // (not an id of a prepared child at all)
    r.lists[kSrcTicket] = {kSys};
    RefreshCensus(&c, r, nullptr);
    CHECK(!AmMayWrite(c, kSys, kSys, true, false, false) && AmMayWrite(c, kSys, kSys, true, true, false) && AmMayWrite(c, kSys, kSys, false, false, false));
    for (const int s : {kSrcNand, kSrcPendingNand, kSrcCard}) {
        r.lists[s] = {kSys};
        RefreshCensus(&c, r, nullptr);
        CHECK(!AmMayWrite(c, kSys, kSys, false, false, false) && !AmMayWrite(c, kSys, kSys, true, true, false));
        r.lists[s].clear();
    }
    r.lists[kSrcProcess] = {kSlot};
    RefreshCensus(&c, r, nullptr);
    CHECK(!AmMayWrite(c, kM3c1, kSlot, false, false, false) && AmMayWrite(c, kSys, kSys, false, false, false));
    r.lists[kSrcProcess].clear();
    r.lists[kSrcTicket].clear();
    r.lists[kSrcSd] = {kSys}; // (a title of the SD card at the id: an install only when the caller's guard found it and judged it ours)
    RefreshCensus(&c, r, nullptr);
    CHECK(!AmMayWrite(c, kSys, kSys, true, false, false) && AmMayWrite(c, kSys, kSys, true, false, true) && AmMayWrite(c, kSys, kSys, false, false, false));
    r.lists[kSrcSd].clear();
    r.lists[kSrcProcess].clear();
    r.fails[kSrcNand] = true;
    RefreshCensus(&c, r, nullptr);
    CHECK(!AmMayWrite(c, kSys, kSys, false, false, false)); // (a list not read: refused)
    IdCensus never;
    CHECK(!AmMayWrite(never, kSys, kSys, false, false, false));
    // (the review of the build 2h, R5) R0 in the second guard too: 3DS Link's own id, with or without its low byte, on a console whose lists all read
    r.fails[kSrcNand] = false;
    RefreshCensus(&c, r, nullptr);
    CHECK(AmMayWrite(c, kSys, kSys, false, false, false));
    CHECK(!AmMayWrite(c, azl::title_rules::kAppTid, azl::title_rules::kAppTid, false, false, false));
    CHECK(!AmMayWrite(c, azl::title_rules::kAppTid | 0x40, azl::title_rules::kAppTid | 0x40, true, false, false));
    CHECK(!AmMayWrite(c, azl::title_rules::PreparedSystemOf(azl::title_rules::kAppTid & ~0xFFull), azl::title_rules::PreparedSystemOf(azl::title_rules::kAppTid & ~0xFFull), false, false, false));
    // R0: a key the app may ever write about
    CHECK(SlotIsSound(kSlot) && !SlotIsSound(azl::title_rules::kAppTid) && !SlotIsSound(azl::title_rules::kAppTid | 0x40));
    CHECK(!SlotIsSound(kSys) && !SlotIsSound(kM3c1) && !SlotIsSound(0x00040000000F8200ull) && !SlotIsSound(0));
}

// The band of the system ids (00040010|0F8xxxxx) against the system titles known: the applications of the system are 00040010 with a low word of
// 0x00020000-0x0002DA00 and their New 3DS forms 0x2002xxxx (Azahar's src/core/system_titles.cpp, master, read 2026-11-19: no id of that file has a
// low word in 0x0F800000-0x0FFFFFFF). Held here as the RANGES that file spans (every low word of them, by steps of 0x100): none is in the band, none
// is taken by SlotKeyOf for an id of a prepared child. A demo or a rare regional title out of that list is not proven (STIMA): the NAND guard (R2)
// and the tickets' (R5) stay for that.
static void TestBandAgainstSystemTitles() {
    int looked = 0;
    for (u32 low = 0x00020000; low <= 0x0002FF00; low += 0x100) {
        for (const u32 l : {low, low | 0x20000000u}) {
            const u64 sys = (static_cast<u64>(azl::title_rules::kTitleHighSystemApplication) << 32) | l;
            CHECK(SlotKeyOf(sys) == 0 && (l & azl::title_rules::kPreparedBandMask) != azl::title_rules::kPreparedBand);
            ++looked;
        }
    }
    CHECK(looked == 2 * 256);
    // the system's Download Play application (its six ids, text_search_guard.hpp) is never one of ours
    for (const u32 l : {0x00020100u, 0x00021100u, 0x00022100u, 0x00026100u, 0x00027100u, 0x00028100u}) {
        CHECK(SlotKeyOf((static_cast<u64>(azl::title_rules::kTitleHighSystemApplication) << 32) | l) == 0);
    }
    // every key of the band has its system id in the band, never 3DS Link's, and SlotKeyOf gives the key back
    for (const u64 key : {0x000400000F800000ull, 0x000400000F8F8200ull, 0x000400000FFFFF00ull}) {
        const u64 sys = azl::title_rules::PreparedSystemOf(key);
        CHECK(static_cast<u32>(sys >> 32) == azl::title_rules::kTitleHighSystemApplication && static_cast<u32>(sys) == static_cast<u32>(key));
        CHECK(SlotKeyOf(sys) == key && sys != azl::title_rules::kAppTid);
    }
}

// (the review of the build 2h, R6) the 109 ids of the System Applications (00040010) one by one, as Azahar's src/core/system_titles.cpp lists them
// (azahar-emu/azahar, master, read 2026-11-19: every region, the Old and the New 3DS forms): none is in the band, none is taken by SlotKeyOf for a
// prepared child, none is the system id of a key of the band. (Their names are in that file: System Settings, Download Play, Activity Log, Health
// and Safety, Camera, Sound, Mii Maker, StreetPass Mii Plaza, eShop, System Transfer, Nintendo Zone, Face Raiders, AR Games, the updater of the Safe
// Mode, Nintendo Network ID Settings, microSD Management, the digital manuals.)
static constexpr u32 kSystemApplicationLows[] = {
        0x00020000u, 0x00021000u, 0x00022000u, 0x00026000u, 0x00027000u, 0x00028000u, 0x00020100u, 0x00021100u,
        0x00022100u, 0x00026100u, 0x00027100u, 0x00028100u, 0x00020200u, 0x00021200u, 0x00022200u, 0x00026200u,
        0x00027200u, 0x00028200u, 0x00020300u, 0x00021300u, 0x00022300u, 0x00026300u, 0x00027300u, 0x00028300u,
        0x20020300u, 0x20021300u, 0x20022300u, 0x20027300u, 0x00020400u, 0x00021400u, 0x00022400u, 0x00026400u,
        0x00027400u, 0x00028400u, 0x00020500u, 0x00021500u, 0x00022500u, 0x00026500u, 0x00027500u, 0x00028500u,
        0x00020700u, 0x00021700u, 0x00022700u, 0x00026700u, 0x00027700u, 0x00028700u, 0x00020800u, 0x00021800u,
        0x00022800u, 0x00026800u, 0x00027800u, 0x00028800u, 0x00020900u, 0x00021900u, 0x00022900u, 0x00027900u,
        0x00028900u, 0x00020A00u, 0x00021A00u, 0x00022A00u, 0x00027A00u, 0x00028A00u, 0x00020B00u, 0x00021B00u,
        0x00022B00u, 0x00020D00u, 0x00021D00u, 0x00022D00u, 0x00026D00u, 0x00027D00u, 0x00028D00u, 0x20020D00u,
        0x20021D00u, 0x20022D00u, 0x20027D00u, 0x00020E00u, 0x00021E00u, 0x00022E00u, 0x00026E00u, 0x00027E00u,
        0x00028E00u, 0x00020F00u, 0x00021F00u, 0x00022F00u, 0x00026F00u, 0x00027F00u, 0x00028F00u, 0x0002BF00u,
        0x0002C000u, 0x0002C100u, 0x20023100u, 0x20024100u, 0x20025100u, 0x2002C800u, 0x2002CF00u, 0x2002D000u,
        0x2002D700u, 0x2002C900u, 0x2002D100u, 0x2002D200u, 0x2002D800u, 0x2002CA00u, 0x2002D300u, 0x2002D400u,
        0x2002D900u, 0x2002CB00u, 0x2002D500u, 0x2002D600u, 0x2002DA00u,
};
static_assert(sizeof(kSystemApplicationLows) / sizeof(kSystemApplicationLows[0]) == 109, "the 109 ids of Azahar's list");

static void TestSystemApplicationsListed() {
    for (const u32 l : kSystemApplicationLows) {
        const u64 sys = (static_cast<u64>(azl::title_rules::kTitleHighSystemApplication) << 32) | l;
        CHECK(SlotKeyOf(sys) == 0);
        CHECK((l & azl::title_rules::kPreparedBandMask) != azl::title_rules::kPreparedBand);
        CHECK(!azl::title_rules::IsPreparedId(0x0004000000000000ull | l));
        CHECK(azl::title_rules::PreparedSystemOf(0x0004000000000000ull | l) == 0);
    }
}

int main() {
    const fs::path dir = fs::temp_directory_path() / ("azl_dlpids_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    if (chdir(dir.string().c_str()) != 0) {
        std::fprintf(stderr, "cannot enter %s\n", dir.string().c_str());
        return 2;
    }
    SetThisConsole(ConsoleStampOf(0x1111)); // (DPAPP-2h-sec: the console the records are written on)
    SetThisId0(std::string("0123456789abcdef") + "0123456789abcdef"); // (DPAPP-2j, R-B: the folder of this console's records)
    TestCensus();
    TestCacheFile();
    TestSlot();
    TestJudge();
    TestTicketAndSecondGuard();
    TestBandAgainstSystemTitles();
    TestSystemApplicationsListed();
    if (chdir("/") != 0) {
        std::fprintf(stderr, "cannot leave %s\n", dir.string().c_str());
    }
    fs::remove_all(dir);
    std::printf("dlp_guest_ids_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
