// Tests for the plan of the gate's page (Includes/azl/gate_plan.hpp): the words of `b`/`bl` and of the stubs and their +-32 MB, the
// pages, the site found from the stub's return, where the page goes (ChooseAddress over the address space of the launches in our logs,
// Tests/gate_maps.json, and over made-up spaces: no room, a page that is not free, the framework's page never taken, sites out of
// reach, a query that fails) and WriteGate, the function the plugin runs to install the gate: one word, and only one, per site.
//   gate_plan_test            (from the project folder: it reads Tests/gate_maps.json)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "azl/gate_plan.hpp"

using namespace azl;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

namespace {

constexpr u32 kBxLrWordForTest = 0xE12FFF1E;

/// Where a `b`/`bl` word at `from` goes (the decoder of the test, written apart from the encoder).
u32 Target(u32 from, u32 word) {
    s32 imm = static_cast<s32>(word << 8) >> 8; // sign-extended 24 bits
    return static_cast<u32>(static_cast<s64>(from) + 8 + static_cast<s64>(imm) * 4);
}

void TestEncodeBranch() {
    u32 w = 0;
    CHECK(EncodeBranch(0x00100000, 0x00100008, &w) && w == 0xEA000000);
    CHECK(EncodeBranch(0x00100000, 0x00100000, &w) && w == 0xEAFFFFFE); // b .
    CHECK(EncodeBranch(0x00100000, 0x00100008, &w, true) && w == 0xEB000000);
    // the reach: +0x1FFFFFC and -0x2000000 from pc + 8, not one word more
    CHECK(EncodeBranch(0x00100000, 0x00100008 + 0x1FFFFFC, &w) && w == 0xEA7FFFFF);
    CHECK(!EncodeBranch(0x00100000, 0x00100008 + 0x2000000, &w));
    CHECK(EncodeBranch(0x02100000, 0x02100008 - 0x2000000, &w) && w == 0xEA800000);
    CHECK(!EncodeBranch(0x02100000, 0x02100004 - 0x2000000, &w));
    CHECK(!EncodeBranch(0x00100000, 0x00100002, &w)); // not on a word
    // no wrap through the top or the bottom of the address space
    CHECK(!EncodeBranch(0x00000000, 0xFFFFFFF8, &w));
    CHECK(!EncodeBranch(0xFFFFFFF0, 0x00000010, &w));
    // the words the plugin writes, against gate_sim's: LEGO Batman 2 EUR's stub at 0x00206F00 to a stub at 0x01E7E34C
    CHECK(EncodeBranch(0x00206F00, 0x01E7E34C, &w) && w == (0xEA000000u | (((0x01E7E34Cu - 0x00206F08u) >> 2) & 0xFFFFFFu)));
    // a sweep: every encoded word goes where it was asked
    u32 bad = 0;
    for (u32 from = 0x00100000; from < 0x03F00000; from += 0x0003F004) {
        for (s64 off = -0x2000000; off <= 0x1FFFFFC; off += 0x10A04) {
            const s64 t = static_cast<s64>(from) + 8 + (off & ~3LL);
            if (t < 0) {
                continue;
            }
            const u32 to = static_cast<u32>(t);
            if (!EncodeBranch(from, to, &w) || Target(from, w) != to || (w >> 24) != 0xEA) {
                ++bad;
            }
        }
    }
    CHECK(bad == 0);
}

void TestBuildStub() {
    u32 words[3] = {};
    const u32 site = 0x002E3FB8; // Monster Hunter XX's shared stub
    const u32 stub = 0x01E7E000 + kGateBytes + 12 * 5;
    const u32 entry = 0x01E7E000 + kGateCode;
    CHECK(BuildStub(site, entry, stub, words));
    CHECK(words[0] == 0xE52DE004); // str lr, [sp, #-4]!
    CHECK((words[1] >> 24) == 0xEB && Target(stub + 4, words[1]) == entry);
    CHECK((words[2] >> 24) == 0xEA && Target(stub + 8, words[2]) == site + 4);
    // a site too far for the stub's way back
    CHECK(!BuildStub(0x04000000, entry, stub, words));
    // a gate entry too far for the bl (never on the console: the gate and its stubs share the page)
    CHECK(!BuildStub(site, 0x07000000, stub, words));
}

void TestPageCountAndSiteIndex() {
    CHECK(PageCount(0) == 1);
    CHECK(PageCount((kGatePageBytes - kGateBytes) / 12) == 1); // 253 stubs fit beside the gate (271 before 2026-11-08)
    CHECK(PageCount((kGatePageBytes - kGateBytes) / 12 + 1) == 2);
    CHECK(PageCount(468) == 2);  // Tetris
    CHECK(PageCount(1293) == 5); // `paradise`, the most sites in our logs (4 pages until the table of the fake sessions, 2026-11-08: the gate 836 -> 1048 bytes)
    CHECK(PageCount(2048) == 7); // kMaxSites of ctr/hook.cpp: 28 KB at most
    CHECK(PageCount(10, 4096) == 2);
    const u32 base = StubsBase(0x01E7E000);
    CHECK(base == 0x01E7E000 + kGateBytes);
    CHECK(SiteIndex(base + 8, base, 4) == 0);
    CHECK(SiteIndex(base + 8 + 12 * 3, base, 4) == 3);
    CHECK(SiteIndex(base + 8 + 12 * 4, base, 4) == kNoGateSite); // (one past the last)
    CHECK(SiteIndex(base + 12, base, 4) == kNoGateSite);         // not a stub's return
    CHECK(SiteIndex(base + 4, base, 4) == kNoGateSite);
    CHECK(SiteIndex(base, base, 4) == kNoGateSite);
    CHECK(SiteIndex(0, base, 4) == kNoGateSite);
    CHECK(SiteIndex(base + 8, base, 0) == kNoGateSite);
}

// ---- Where the page goes ----

struct Span {
    u32 base;
    u32 size;
    u32 state;
};

/// An address space: the spans that are mapped, everything else up to 0x04000000 free (svcQueryMemory's answer for a free address
/// is the whole free span around it).
struct Space {
    std::vector<Span> maps;
    u32 queries = 0;
    bool fail = false;
    bool lie = false;
    bool Query(u32 a, GateSpan* out) {
        ++queries;
        if (fail) {
            return false;
        }
        u32 lo = 0;
        u32 hi = 0x04000000;
        for (const Span& s : maps) {
            if (a >= s.base && a - s.base < s.size) {
                *out = {s.base, s.size, s.state};
                if (lie) {
                    out->base = a + 0x1000;
                }
                return true;
            }
            if (s.base + s.size <= a && s.base + s.size > lo) {
                lo = s.base + s.size;
            }
            if (s.base > a && s.base < hi) {
                hi = s.base;
            }
        }
        *out = {lo, hi - lo, kGateMemFree};
        if (lie) {
            out->base = a + 0x1000;
        }
        return true;
    }
    bool AllFree(u32 base, u32 size) const {
        for (const Span& s : maps) {
            if (base < s.base + s.size && s.base < base + size) {
                return false;
            }
        }
        return base + size <= 0x04000000;
    }
};

u32 Choose(Space& sp, u32 lo, u32 hi, u32 size, GatePlaceInfo* info) {
    return ChooseAddress([&sp](u32 a, GateSpan* out) { return sp.Query(a, out); }, lo, hi, size, info);
}

bool OverlapsFramework(u32 base, u32 size) {
    return base < kGateCeiling + kFrameworkPageBytes && kGateCeiling < base + size;
}

/// The launches of Tests/gate_maps.json (one per line: the text, the number of sites, the mappings below 0x04000000).
struct Launch {
    std::string game;
    u32 text_base = 0;
    u32 text_len = 0;
    u32 sites = 0;
    std::vector<Span> maps;
};

std::vector<Launch> ReadMaps(const char* path) {
    std::vector<Launch> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const size_t g = line.find("\"game\": \"");
        if (g == std::string::npos) {
            continue;
        }
        Launch l;
        l.game = line.substr(g + 9, line.find('"', g + 9) - (g + 9));
        const size_t t = line.find("\"text\": [\"");
        l.text_base = static_cast<u32>(std::strtoul(line.c_str() + t + 10, nullptr, 16));
        const size_t t2 = line.find("\", \"", t);
        l.text_len = static_cast<u32>(std::strtoul(line.c_str() + t2 + 4, nullptr, 16));
        const size_t n = line.find("\"sites\": ");
        l.sites = static_cast<u32>(std::strtoul(line.c_str() + n + 9, nullptr, 10));
        size_t m = line.find("\"maps\": [");
        while ((m = line.find("[\"0x", m + 1)) != std::string::npos) {
            char* end = nullptr;
            Span s{};
            s.base = static_cast<u32>(std::strtoul(line.c_str() + m + 2, &end, 16));
            const char* p = std::strstr(end, "\"0x");
            s.size = static_cast<u32>(std::strtoul(p + 1, &end, 16));
            p = std::strchr(end, ',');
            s.state = static_cast<u32>(std::strtoul(p + 1, nullptr, 10));
            l.maps.push_back(s);
        }
        out.push_back(l);
    }
    return out;
}

// Every launch of our logs: the page goes right under 0x01E80000, free, all its sites in reach, in a handful of queries.
void TestTheLaunchesOfTheLogs() {
    const std::vector<Launch> launches = ReadMaps("Tests/gate_maps.json");
    CHECK(launches.size() >= 34);
    u32 found = 0;
    u32 most_queries = 0;
    for (const Launch& l : launches) {
        CHECK(l.text_len > 0 && l.maps.size() >= 2 && l.sites > 0);
        Space sp{l.maps};
        const u32 size = PageCount(l.sites) * kGatePageBytes;
        GatePlaceInfo info;
        const u32 base = Choose(sp, l.text_base, l.text_base + l.text_len - 4, size, &info);
        const bool ok = base != 0 && info.result == GatePlace::Found && base == kGateCeiling - size && sp.AllFree(base, size) &&
                        !OverlapsFramework(base, size) && GateInReach(l.text_base, l.text_base + l.text_len - 4, base, size);
        CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "  %s: 0x%08x (%s)\n", l.game.c_str(), base, GatePlaceName(info.result));
        }
        found += ok;
        most_queries = info.queries > most_queries ? info.queries : most_queries;
        // with the most stubs the hook may have (kMaxSites of ctr/hook.cpp): the same
        const u32 big = PageCount(2048) * kGatePageBytes;
        Space sp2{l.maps};
        CHECK(Choose(sp2, l.text_base, l.text_base + l.text_len - 4, big, nullptr) == kGateCeiling - big);
    }
    CHECK(most_queries == 1);
    std::printf("  %u launches of the logs: the page right under 0x01E80000 in %u (at most %u query)\n", static_cast<unsigned>(launches.size()),
                found, most_queries);
}

void TestMadeUpSpaces() {
    const u32 lo = 0x00100000;
    const u32 hi = 0x005FFFFC;
    GatePlaceInfo info;
    // CTRPF's page not mapped yet, everything free up to 0x04000000: still below 0x01E80000, never in 0x01E80000-0x01E82000
    {
        Space sp{{{0x00100000, 0x00500000, 4}}};
        const u32 b = Choose(sp, lo, hi, 0x2000, &info);
        CHECK(b == 0x01E7E000 && info.result == GatePlace::Found && !OverlapsFramework(b, 0x2000));
    }
    // CTRPF's page mapped and a CRO right under it: below the CRO
    {
        Space sp{{{0x00100000, 0x00500000, 4}, {0x01E00000, 0x80000, 10}, {0x01E80000, 0x2000, 6}}};
        const u32 b = Choose(sp, lo, hi, 0x3000, &info);
        CHECK(b == 0x01DFD000 && sp.AllFree(b, 0x3000) && info.queries == 2);
    }
    // a free hole under 0x01E80000 too small for the page: the page goes under the mapping below the hole
    {
        Space sp{{{0x00100000, 0x00500000, 4}, {0x01E00000, 0x7F000, 10}, {0x01E80000, 0x2000, 6}}};
        const u32 b = Choose(sp, lo, hi, 0x2000, &info);
        CHECK(b == 0x01DFE000 && sp.AllFree(b, 0x2000));
    }
    // a free span that ends before the page would: the page ends where the span does
    {
        Space sp{{{0x00100000, 0x00500000, 4}, {0x01E7F000, 0x1000, 11}, {0x01E80000, 0x2000, 6}}};
        const u32 b = Choose(sp, lo, hi, 0x2000, &info);
        CHECK(b == 0x01E7D000 && sp.AllFree(b, 0x2000));
    }
    // no room at all below 0x01E80000
    {
        Space sp{{{0x00100000, 0x01D80000, 4}, {0x01E80000, 0x2000, 6}}};
        CHECK(Choose(sp, lo, hi, 0x1000, &info) == 0 && info.result == GatePlace::NoRoom);
    }
    // the sites are beyond 32 MB of every free place (made up: the game's text above the page)
    {
        Space sp{{{0x03F00000, 0x00080000, 4}, {0x01E80000, 0x2000, 6}}};
        CHECK(Choose(sp, 0x03F00000, 0x03F7FFFC, 0x1000, &info) == 0 && info.result == GatePlace::OutOfReach);
        CHECK(info.queries <= kGateMaxQueries);
    }
    // the only free room is far from the sites (made up: the text above, everything between mapped): out of reach, not "no room"
    {
        Space sp{{{0x00110000, 0x03CF0000, 10}, {0x03E00000, 0x1000, 4}}};
        CHECK(Choose(sp, 0x03E00000, 0x03E00FFC, 0x1000, &info) == 0 && info.result == GatePlace::OutOfReach);
    }
    // the query fails, or answers a span that does not hold the address asked
    {
        Space sp{{{0x00100000, 0x00500000, 4}}};
        sp.fail = true;
        CHECK(Choose(sp, lo, hi, 0x1000, &info) == 0 && info.result == GatePlace::QueryFailed);
        Space sp2{{{0x00100000, 0x00500000, 4}}};
        sp2.lie = true;
        CHECK(Choose(sp2, lo, hi, 0x1000, &info) == 0 && info.result == GatePlace::QueryFailed);
    }
    // bad input
    {
        Space sp{{{0x00100000, 0x00500000, 4}}};
        CHECK(Choose(sp, lo, hi, 0x1800, &info) == 0 && info.result == GatePlace::BadInput);
        CHECK(Choose(sp, lo, hi, 0, &info) == 0 && info.result == GatePlace::BadInput);
        CHECK(Choose(sp, hi, lo, 0x1000, &info) == 0 && info.result == GatePlace::BadInput);
        CHECK(sp.queries == 0);
    }
    // every free place of a space full of small mappings is looked at: the answer is the highest one that fits
    {
        Space sp;
        sp.maps.push_back({0x00100000, 0x00700000, 4});
        for (u32 a = 0x00800000; a < 0x01E80000; a += 0x10000) {
            sp.maps.push_back({a, 0xF000, 10}); // one free page every 64 KB
        }
        sp.maps.push_back({0x01E80000, 0x2000, 6});
        CHECK(Choose(sp, lo, hi, 0x1000, &info) == 0x01E7F000);
        CHECK(Choose(sp, lo, hi, 0x2000, &info) == 0 && info.result == GatePlace::NoRoom); // (two pages never fit: it gives up after kGateMaxQueries)
        CHECK(info.queries <= kGateMaxQueries);
    }
}

// ---- WriteGate ----

constexpr u32 kPage = 0x01E7E000;

/// The game's text (sparse) and the page, with every event in order.
struct FakeMem {
    std::map<u32, u32> text;
    std::vector<u32> page;
    std::vector<std::pair<char, u32>> events; // 'p' page store, 't' text store, 's' sync
    std::map<u32, u32> text_stores;          // how many stores each text address got
    u32 refuse = 0;                          // PatchText fails at this address
    explicit FakeMem(u32 bytes) : page(bytes / 4, 0xDEADBEEF) {}
    u32 Load(u32 a) { return text.count(a) ? text[a] : 0; }
    void StorePage(u32 a, u32 w) {
        events.push_back({'p', a});
        page.at((a - kPage) / 4) = w;
    }
    bool PatchText(u32 a, u32 w) {
        events.push_back({'t', a});
        ++text_stores[a];
        if (a == refuse) {
            return false;
        }
        text[a] = w;
        return true;
    }
    void Sync() { events.push_back({'s', 0}); }
    u32 P(u32 a) const { return page.at((a - kPage) / 4); }
};

std::vector<u32> MakeBlob() {
    std::vector<u32> b(kGateBytes / 4);
    for (u32 i = 0; i < b.size(); ++i) {
        b[i] = 0xE1A00000 | i;
    }
    return b; // (the header too holds junk: WriteGate puts it at zero)
}

void TestWriteGate() {
    FakeMem m(2 * kGatePageBytes);
    std::vector<GateSite> sites;
    for (u32 i = 0; i < 20; ++i) {
        const u32 a = 0x00100000 + i * 0x400;
        const u32 svc = i % 3 == 0 ? kGateSvc23Word : kGateSvc32Word;
        m.text[a] = svc;
        m.text[a + 4] = kBxLrWordForTest;
        sites.push_back({a, svc});
    }
    m.text[0x00100000 + 7 * 0x400] = 0xEA001234; // site 7: somebody else changed it since the scan
    sites[11].original = 0xE1A00000;             // site 11: not a svc at all
    m.text[0x04100000] = kGateSvc32Word; // a site beyond 32 MB of the page
    sites.push_back({0x04100000, kGateSvc32Word});
    const std::map<u32, u32> before = m.text;
    const std::vector<u32> blob = MakeBlob();
    const GatePointers ptrs{0x07010000, 0x07010100, 0x07010200, 0x07010300};
    const GateWriteResult r = WriteGate(m, kPage, 2 * kGatePageBytes, GateBlob{blob.data(), kGateBytes, kGateCode, kGateEntry23}, ptrs,
                                        sites.data(), static_cast<u32>(sites.size()));
    CHECK(r.ok && r.hooked == 18 && r.foreign == 2 && r.out_of_reach == 1 && r.no_room == 0 && r.patch_failed == 0);
    // ONE store per hooked site, and nothing else in the text
    CHECK(m.text_stores.size() == 18);
    for (const auto& kv : m.text_stores) {
        CHECK(kv.second == 1);
    }
    const u32 stubs = StubsBase(kPage);
    for (u32 i = 0; i < r.hooked; ++i) {
        const GateSite& s = sites[i];
        const u32 stub = stubs + 12 * i;
        CHECK(Target(s.address, m.text[s.address]) == stub && (m.text[s.address] >> 24) == 0xEA);
        CHECK(m.text[s.address + 4] == kBxLrWordForTest); // site+4 stays the game's
        CHECK(m.P(stub) == kStubSaveLr);
        CHECK(Target(stub + 4, m.P(stub + 4)) == kPage + (s.original == kGateSvc32Word ? kGateCode : kGateEntry23));
        CHECK(Target(stub + 8, m.P(stub + 8)) == s.address + 4);
        CHECK(SiteIndex(stub + 8, stubs, r.hooked) == i);
    }
    // the hooked sites are at the front, in the order of the scan; the others are left as they were
    CHECK(sites[0].address == 0x00100000 && sites[7].address == 0x00100000 + 8 * 0x400 && sites[9].address == 0x00100000 + 10 * 0x400 &&
          sites[10].address == 0x00100000 + 12 * 0x400);
    CHECK(m.text[0x00100000 + 7 * 0x400] == 0xEA001234 && m.text[0x00100000 + 11 * 0x400] == before.at(0x00100000 + 11 * 0x400) &&
          m.text[0x04100000] == kGateSvc32Word);
    // the header: flag 1, the pointers, the code of the gate as it was given
    CHECK(m.P(kPage + kGateFlag) == 1 && m.P(kPage + kGateInFlight) == 0);
    for (u32 off = kGateLastSite; off < kGateCode; off += 4) {
        CHECK(m.P(kPage + off) == 0); // last_site, last_handle, post_skipped, the table: at zero whatever the blob held
    }
    CHECK(m.P(kPage + kGatePre32) == ptrs.pre32 && m.P(kPage + kGatePost32) == ptrs.post32 && m.P(kPage + kGatePre23) == ptrs.pre23 &&
          m.P(kPage + kGatePost23) == ptrs.post23);
    for (u32 i = kGateCode / 4; i < kGateBytes / 4; ++i) {
        CHECK(m.P(kPage + 4 * i) == blob[i]);
    }
    // the order: everything of the page, a sync, flag = 1 (the last store of the page), a sync, then the sites, then a sync
    size_t first_text = m.events.size();
    size_t flag_at = m.events.size();
    for (size_t i = 0; i < m.events.size(); ++i) {
        if (m.events[i].first == 't' && first_text == m.events.size()) {
            first_text = i;
        }
        if (m.events[i].first == 'p' && m.events[i].second == kPage + kGateFlag) {
            flag_at = i; // (the last store to the flag)
        }
    }
    CHECK(flag_at < first_text && first_text < m.events.size());
    CHECK(flag_at > 0 && m.events[flag_at - 1].first == 's' && m.events[flag_at + 1].first == 's');
    for (size_t i = flag_at + 1; i < m.events.size(); ++i) {
        CHECK(m.events[i].first != 'p'); // nothing of the page after the flag
    }
    for (size_t i = 0; i < flag_at; ++i) {
        CHECK(m.events[i].first != 't'); // nothing of the text before
    }
    CHECK(m.events.back().first == 's');
}

void TestWriteGateLimits() {
    const std::vector<u32> blob = MakeBlob();
    const GateBlob gb{blob.data(), kGateBytes, kGateCode, kGateEntry23};
    const GatePointers ptrs{1, 2, 3, 4};
    // more sites than stubs fit: the first ones are hooked, the rest counted and left alone
    {
        FakeMem m(kGatePageBytes);
        std::vector<GateSite> sites;
        for (u32 i = 0; i < 300; ++i) {
            m.text[0x00100000 + 8 * i] = kGateSvc32Word;
            sites.push_back({0x00100000 + 8 * i, kGateSvc32Word});
        }
        const GateWriteResult r = WriteGate(m, kPage, kGatePageBytes, gb, ptrs, sites.data(), 300);
        CHECK(r.ok && r.hooked == (kGatePageBytes - kGateBytes) / 12 && r.no_room == 300 - r.hooked);
        CHECK(m.text_stores.size() == r.hooked);
        CHECK(m.text[0x00100000 + 8 * 299] == kGateSvc32Word);
    }
    // a word that cannot be written: counted; its stub stays, the others go on
    {
        FakeMem m(kGatePageBytes);
        std::vector<GateSite> sites;
        for (u32 i = 0; i < 4; ++i) {
            m.text[0x00200000 + 0x100 * i] = kGateSvc32Word;
            sites.push_back({0x00200000 + 0x100 * i, kGateSvc32Word});
        }
        m.refuse = 0x00200100;
        const GateWriteResult r = WriteGate(m, kPage, kGatePageBytes, gb, ptrs, sites.data(), 4);
        CHECK(r.ok && r.hooked == 4 && r.patch_failed == 1);
        CHECK(m.text[0x00200100] == kGateSvc32Word && m.text_stores.size() == 4);
        CHECK(Target(0x00200200, m.text[0x00200200]) == StubsBase(kPage) + 24);
    }
    // a bad page or a bad blob: nothing written at all
    {
        GateSite one{0x00100000, kGateSvc32Word};
        FakeMem m(kGatePageBytes);
        m.text[0x00100000] = kGateSvc32Word;
        CHECK(!WriteGate(m, kPage + 4, kGatePageBytes, gb, ptrs, &one, 1).ok);
        CHECK(!WriteGate(m, kPage, 0x800, gb, ptrs, &one, 1).ok);
        CHECK(!WriteGate(m, kPage, kGatePageBytes, GateBlob{blob.data(), kGateBytes, 0x10, kGateEntry23}, ptrs, &one, 1).ok);
        CHECK(!WriteGate(m, kPage, kGatePageBytes, GateBlob{blob.data(), kGateBytes, kGateCode, kGateBytes}, ptrs, &one, 1).ok);
        CHECK(!WriteGate(m, kPage, kGatePageBytes, GateBlob{nullptr, kGateBytes, kGateCode, kGateEntry23}, ptrs, &one, 1).ok);
        CHECK(!WriteGate(m, 0, kGatePageBytes, gb, ptrs, &one, 1).ok);
        CHECK(m.events.empty());
    }
    // no site at all: the gate goes in, the text is not touched
    {
        FakeMem m(kGatePageBytes);
        const GateWriteResult r = WriteGate(m, kPage, kGatePageBytes, gb, ptrs, nullptr, 0);
        CHECK(r.ok && r.hooked == 0 && m.text_stores.empty() && m.P(kPage + kGateFlag) == 1);
    }
}

} // namespace

// How the page is taken, four ways out: an alias on Azahar; svc 0xA3 on an Old or New 3DS from Luma3DS 13.0 (an alias is out of the books
// while the game's process outlives the plugin's block: 3DS Link, New 3DS XL, 1.1.2); before it (or without Luma3DS) the alias on a New 3DS
// (the consoles C and D have 10.1.1 and 10.2.1) and nothing on an Old 3DS (the swap takes the plugin's block away).
void TestChooseGateWay() {
    struct Case {
        bool citra, is_new;
        u32 luma;
        GateWay want;
    };
    const u32 v10_1_1 = (10u << 24) | (1u << 16) | (1u << 8), v10_2_1 = (10u << 24) | (2u << 16) | (1u << 8);
    const u32 v12_9_9 = (12u << 24) | (9u << 16) | (9u << 8), v13_0_0 = 13u << 24, v13_3_3 = (13u << 24) | (3u << 16) | (3u << 8);
    const Case cases[] = {
        {true, false, 0, GateWay::Alias},        {true, true, v13_3_3, GateWay::Alias},   {true, false, v10_2_1, GateWay::Alias},
        {false, true, v10_1_1, GateWay::Alias},  {false, true, v10_2_1, GateWay::Alias},  {false, true, v12_9_9, GateWay::Alias},
        {false, true, 0, GateWay::Alias},        {false, true, v13_0_0, GateWay::System}, {false, true, v13_3_3, GateWay::System},
        {false, false, v13_3_3, GateWay::System}, {false, false, v13_0_0, GateWay::System}, {false, false, v12_9_9, GateWay::None},
        {false, false, v10_2_1, GateWay::None},  {false, false, 0, GateWay::None},
    };
    bool seen[3] = {false, false, false};
    for (const Case& c : cases) {
        const char* reason = nullptr;
        const GateWay way = ChooseGateWay(c.citra, c.is_new, c.luma, &reason);
        CHECK(way == c.want);
        CHECK(reason != nullptr && reason[0] != '\0');
        seen[static_cast<u32>(way)] = true;
    }
    CHECK(seen[0] && seen[1] && seen[2]); // (Alias, System, None: every way is reached)
    // the reasons tell every way out apart (the line of the page in the log, and the notice on the screen): Azahar, a New and an Old 3DS
    // before Luma3DS 13.0, a New and an Old 3DS from it (a SYSTEM page that fails has no alias to fall back on: gate_page.cpp, no hooks)
    const char* reasons[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    CHECK(ChooseGateWay(true, true, v13_3_3, &reasons[0]) == GateWay::Alias);
    CHECK(ChooseGateWay(false, true, v10_2_1, &reasons[1]) == GateWay::Alias);
    CHECK(ChooseGateWay(false, false, v10_2_1, &reasons[2]) == GateWay::None);
    CHECK(ChooseGateWay(false, true, v13_3_3, &reasons[3]) == GateWay::System);
    CHECK(ChooseGateWay(false, false, v13_3_3, &reasons[4]) == GateWay::System);
    for (int i = 0; i < 5; ++i) {
        CHECK(reasons[i] != nullptr && reasons[i][0] != '\0');
        for (int j = i + 1; j < 5; ++j) {
            CHECK(reasons[i] != nullptr && reasons[j] != nullptr && std::strcmp(reasons[i], reasons[j]) != 0);
        }
    }
    // (Azahar says the same whatever the console)
    const char* azahar_old = nullptr;
    ChooseGateWay(true, false, 0, &azahar_old);
    CHECK(azahar_old != nullptr && reasons[0] != nullptr && std::strcmp(azahar_old, reasons[0]) == 0);
}

int main() {
    TestChooseGateWay();
    TestEncodeBranch();
    TestBuildStub();
    TestPageCountAndSiteIndex();
    TestTheLaunchesOfTheLogs();
    TestMadeUpSpaces();
    TestWriteGate();
    TestWriteGateLimits();
    std::printf("gate_plan_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
