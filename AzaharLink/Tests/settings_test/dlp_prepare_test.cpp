// What 3DS Link Delivery decides before its room opens (ConfigApp/source/dlp_prepare_logic.hpp, the very file, plain C++ with the composer's dlpc.h for the
// numbers of its stages and results), run by Tests/run.sh. What it pins: the app's version as the TMD holds it; how a CIA is read (made-up ones of every
// signature type, truncated and damaged ones, and the real CIAs of the tree when they are there); when child.cia is good (the right kind of title AND this
// version of the app: a package of another version is made again, a good one never) and when 3dslink.cia is the app's own; where the composer's input comes from
// in every combination (the file, the installed title, the wait for the scan, nothing); the one-time work's time and bar (never full before the end, never
// longer than the three minutes the user accepted for the usual sizes, the time left corrected by the pace); the marker of a run that died; and that every way
// the work can fail says what to do about it. Given the path of a model (Tests/run.sh makes one from the finished child) it also asks the real composer what the
// app asks it - the size of the package, the memory, what each stage does - and checks the plan made from the answers.
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "dlp_prepare_logic.hpp"
#include "dlp_text.hpp"

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                          \
    } while (0)

using dlp::u16;
using dlp::u32;
using dlp::u64;
using dlp::u8;

// ---------------------------------------------------------------------------------------------
// A made-up CIA: header, certificates, ticket, TMD, contents (and a meta), laid out as the files of the tree are
// ---------------------------------------------------------------------------------------------

static void PutLe32(std::vector<u8>& b, std::size_t at, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b[at + i] = static_cast<u8>(v >> (8 * i));
    }
}
static void PutBe32(std::vector<u8>& b, std::size_t at, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b[at + i] = static_cast<u8>(v >> (24 - 8 * i));
    }
}

struct CiaSpec {
    u64 tid = dlp::kAppTid;
    u16 version = 1024;
    u16 contents = 1;
    u32 sig_type = 0x10004; // RSA-2048 with SHA-256, what the CIAs of the tree carry
    u32 cert = 0xA00, ticket = 0x350, tmd = 0xB34, meta = 0;
    u64 content = 0x1000;   // the size field of the header (the contents' bytes)
    u32 header = 0x2020;
};

static std::vector<u8> MakeCia(const CiaSpec& s) {
    const u64 tmd_off = dlp::Align64(0x2020) + dlp::Align64(s.cert) + dlp::Align64(s.ticket);
    const u64 total = tmd_off + dlp::Align64(s.tmd) + dlp::Align64(s.content) + s.meta;
    std::vector<u8> b(static_cast<std::size_t>(total), 0);
    PutLe32(b, 0, s.header);
    PutLe32(b, 8, s.cert);
    PutLe32(b, 12, s.ticket);
    PutLe32(b, 16, s.tmd);
    PutLe32(b, 20, s.meta);
    PutLe32(b, 0x18, static_cast<u32>(s.content));
    PutBe32(b, static_cast<std::size_t>(tmd_off), s.sig_type);
    u32 sig = 0x100, pad = 0x3C;
    if (s.sig_type == 0x10000 || s.sig_type == 0x10003) {
        sig = 0x200;
    } else if (s.sig_type == 0x10002 || s.sig_type == 0x10005) {
        sig = 0x3C;
        pad = 0x40;
    }
    const std::size_t body = static_cast<std::size_t>(tmd_off) + 4 + sig + pad;
    PutBe32(b, body + 0x4C, static_cast<u32>(s.tid >> 32));
    PutBe32(b, body + 0x50, static_cast<u32>(s.tid));
    b[body + 0x9C] = static_cast<u8>(s.version >> 8);
    b[body + 0x9D] = static_cast<u8>(s.version);
    b[body + 0x9E] = static_cast<u8>(s.contents >> 8);
    b[body + 0x9F] = static_cast<u8>(s.contents);
    return b;
}

static dlp::CiaFacts Look(const std::vector<u8>& b) {
    return dlp::InspectCiaBytes(b.data(), b.size());
}

// ---------------------------------------------------------------------------------------------

static void TestVersion16() {
    CHECK(dlp::Version16("1.0.0") == 1024);
    CHECK(dlp::Version16("1.2.3") == 1024u + 32u + 3u);
    CHECK(dlp::Version16("0.1.17") == 0); // (micro stops at 15: the TMD holds four bits of it)
    CHECK(dlp::Version16("0.1.15") == 16u + 15u);
    CHECK(dlp::Version16("63.63.15") == (63u << 10 | 63u << 4 | 15u));
    CHECK(dlp::Version16("64.0.0") == 0 && dlp::Version16("1.64.0") == 0 && dlp::Version16("1.0.16") == 0);
    CHECK(dlp::Version16("1.0") == 0 && dlp::Version16("1") == 0 && dlp::Version16("1.0.0.0") == 0 && dlp::Version16("") == 0);
    CHECK(dlp::Version16(".1.0") == 0 && dlp::Version16("1..0") == 0 && dlp::Version16("1.0.") == 0 && dlp::Version16("a.b.c") == 0);
    CHECK(dlp::Version16("1.0.0 ") == 0 && dlp::Version16(" 1.0.0") == 0 && dlp::Version16("-1.0.0") == 0 && dlp::Version16(nullptr) == 0);
    // and the other way, with the app's own words for a version: 1024 is 1.0.0
    CHECK(dlp::VersionText(dlp::Version16("1.0.0")) == "1.0.0" && dlp::VersionText(dlp::Version16("2.5.9")) == "2.5.9");
    // the file VERSION of the tree (run from AzaharLink/): the number of the whole product, which has to fit the TMD
    if (FILE* f = std::fopen("VERSION", "rb")) {
        char buf[64] = {};
        const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        std::string v(buf, n);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) {
            v.pop_back();
        }
        CHECK(dlp::Version16(v.c_str()) != 0);
        CHECK(dlp::VersionText(dlp::Version16(v.c_str())) == v);
    } else {
        std::printf("  (VERSION not found: the file of the tree is not checked)\n");
    }
}

static void TestInspectCia() {
    // every signature type the TMD can carry
    for (const u32 sig : {0x10000u, 0x10001u, 0x10002u, 0x10003u, 0x10004u, 0x10005u}) {
        CiaSpec s;
        s.sig_type = sig;
        const dlp::CiaFacts f = Look(MakeCia(s));
        CHECK(f.parsed && f.tid == dlp::kAppTid && f.version == 1024 && f.contents == 1);
    }
    CiaSpec odd;
    odd.sig_type = 0x10006;
    CHECK(!Look(MakeCia(odd)).parsed); // not a signature type
    odd = CiaSpec();
    odd.tid = 0x0004000100123400ull;
    odd.version = 1059;
    odd.contents = 3;
    const dlp::CiaFacts child = Look(MakeCia(odd));
    CHECK(child.parsed && child.tid == 0x0004000100123400ull && child.version == 1059 && child.contents == 3);

    // a header that is not one, and one that lies
    CiaSpec bad;
    bad.header = 0x2021;
    CHECK(!Look(MakeCia(bad)).parsed);
    bad = CiaSpec();
    bad.contents = 0;
    CHECK(!Look(MakeCia(bad)).parsed); // a TMD with no content is no title
    bad = CiaSpec();
    bad.tid = 0;
    CHECK(!Look(MakeCia(bad)).parsed);
    for (const u32 field : {0u, 1u}) { // the certificate chain and the ticket cannot be empty
        bad = CiaSpec();
        (field == 0 ? bad.cert : bad.ticket) = 0;
        CHECK(!Look(MakeCia(bad)).parsed);
    }
    bad = CiaSpec();
    bad.cert = 0x20000; // more than any chain
    CHECK(!Look(MakeCia(bad)).parsed);

    // a file cut short: the header says more than the file holds
    std::vector<u8> whole = MakeCia(CiaSpec());
    CHECK(Look(whole).parsed);
    std::vector<u8> cut(whole.begin(), whole.end() - 0x200);
    CHECK(!Look(cut).parsed);
    std::vector<u8> pad_only(whole.begin(), whole.end() - 0x20);
    CHECK(Look(pad_only).parsed); // (a file that lacks only the padding of the last block is the usual end of a CIA)
    CHECK(!dlp::InspectCiaBytes(whole.data(), 0x10).parsed && !dlp::InspectCiaBytes(whole.data(), 0x3000).parsed);
    CHECK(!dlp::InspectCiaBytes(nullptr, 0).parsed);

    // a meta at the end counts in the size
    CiaSpec meta;
    meta.meta = 0x3AC0;
    std::vector<u8> with_meta = MakeCia(meta);
    CHECK(Look(with_meta).parsed && Look(with_meta).end == with_meta.size());
    std::vector<u8> no_meta(with_meta.begin(), with_meta.end() - 0x3AC0);
    CHECK(!Look(no_meta).parsed);

    // a reader that fails at either read
    for (int failing = 0; failing < 2; ++failing) {
        int calls = 0;
        const dlp::CiaFacts f = dlp::InspectCia(
            [&](u64 off, u8* buf, std::size_t len) {
                if (calls++ == failing || off + len > whole.size()) {
                    return false;
                }
                std::memcpy(buf, whole.data() + off, len);
                return true;
            },
            whole.size());
        CHECK(!f.parsed);
    }
    // nothing but the two small reads is asked of a file (no whole CIA goes through memory to be judged)
    std::size_t read_total = 0;
    int reads = 0;
    const dlp::CiaFacts f = dlp::InspectCia(
        [&](u64 off, u8* buf, std::size_t len) {
            read_total += len;
            ++reads;
            std::memcpy(buf, whole.data() + off, len);
            return true;
        },
        whole.size());
    CHECK(f.parsed && reads == 2 && read_total == 0x20 + dlp::kTmdNeeded);
}

static void TestJudge() {
    CiaSpec app;
    const dlp::CiaFacts good = Look(MakeCia(app));
    const unsigned v = 1024;
    // 3dslink.cia: the app's title, this version
    CHECK(dlp::JudgeAppCia(true, 2500000, good, v) == dlp::Verdict::Good);
    CHECK(dlp::JudgeAppCia(false, 0, dlp::CiaFacts(), v) == dlp::Verdict::Missing);
    CHECK(dlp::JudgeAppCia(true, 0, good, v) == dlp::Verdict::BadSize);
    CHECK(dlp::JudgeAppCia(true, dlp::kMaxAppCiaBytes + 1, good, v) == dlp::Verdict::BadSize);
    CHECK(dlp::JudgeAppCia(true, 2500000, dlp::CiaFacts(), v) == dlp::Verdict::NotCia);
    CHECK(dlp::JudgeAppCia(true, 2500000, good, v + 1) == dlp::Verdict::WrongVersion); // (an older or a newer one: never taken for this)
    CHECK(dlp::JudgeAppCia(true, 2500000, good, v - 1) == dlp::Verdict::WrongVersion);
    CiaSpec other = app;
    other.tid = 0x0004000000123400ull; // another application
    CHECK(dlp::JudgeAppCia(true, 2500000, Look(MakeCia(other)), v) == dlp::Verdict::WrongTitle);
    other.tid = 0x0004000100123400ull; // a title of the Download Play kind is not the app either
    CHECK(dlp::JudgeAppCia(true, 2500000, Look(MakeCia(other)), v) == dlp::Verdict::WrongTitle);

    // child.cia: a title of the Download Play kind (00040001), THIS version of the app
    CiaSpec child;
    child.tid = 0x0004000100123400ull;
    const dlp::CiaFacts child_facts = Look(MakeCia(child));
    CHECK(dlp::JudgeChild(true, 2800000, child_facts, v) == dlp::Verdict::Good);
    CHECK(dlp::JudgeChild(false, 0, dlp::CiaFacts(), v) == dlp::Verdict::Missing);
    CHECK(dlp::JudgeChild(true, 0, child_facts, v) == dlp::Verdict::BadSize);
    CHECK(dlp::JudgeChild(true, dlp::kMaxChildBytes, child_facts, v) == dlp::Verdict::Good);      // (the limit of the library is 32 MiB, itself included)
    CHECK(dlp::JudgeChild(true, dlp::kMaxChildBytes + 1, child_facts, v) == dlp::Verdict::BadSize);
    CHECK(dlp::JudgeChild(true, 2800000, dlp::CiaFacts(), v) == dlp::Verdict::NotCia);
    CHECK(dlp::JudgeChild(true, 2800000, child_facts, v + 1) == dlp::Verdict::WrongVersion); // the app was updated: the package is made again
    CHECK(dlp::JudgeChild(true, 2800000, child_facts, v - 1) == dlp::Verdict::WrongVersion);
    CHECK(dlp::JudgeChild(true, 2800000, good, v) == dlp::Verdict::WrongTitle); // the app's own CIA is not a package to hand out
    other = child;
    other.tid = 0x0004000000123400ull;
    CHECK(dlp::JudgeChild(true, 2800000, Look(MakeCia(other)), v) == dlp::Verdict::WrongTitle);
}

static void TestChooseSource() {
    using dlp::Source;
    using dlp::Verdict;
    // a good file is used whatever else is true (it needs nothing: not the scan, not a key)
    for (const bool scan : {false, true}) {
        for (const bool keys : {false, true}) {
            CHECK(dlp::ChooseSource(Verdict::Good, scan, keys) == Source::File);
        }
    }
    // no good file: the installed title, which needs the keys the scan's context holds
    for (const Verdict bad : {Verdict::Missing, Verdict::NotCia, Verdict::WrongVersion, Verdict::WrongTitle, Verdict::BadSize}) {
        CHECK(dlp::ChooseSource(bad, false, false) == Source::WaitScan); // the scan is not done: the key is not known yet, the page waits for it
        CHECK(dlp::ChooseSource(bad, false, true) == Source::WaitScan);
        CHECK(dlp::ChooseSource(bad, true, true) == Source::Title);
        CHECK(dlp::ChooseSource(bad, true, false) == Source::None);      // the page says what to do
    }
    // with no source at all (None), what the page names first: a file that is there and cannot be used (damaged, not a CIA, another title, another version, no size
    // a package can have), and not a file that is not there; a good one is never to blame (the composer refusing it is something the page knows by itself)
    for (const Verdict bad : {Verdict::NotCia, Verdict::WrongVersion, Verdict::WrongTitle, Verdict::BadSize}) {
        CHECK(dlp::FileIsToBlame(bad));
    }
    CHECK(!dlp::FileIsToBlame(Verdict::Missing) && !dlp::FileIsToBlame(Verdict::Good));
    // room on the card: the package, the copy when it is made from the installed title, and a margin
    CHECK(dlp::SpaceNeeded(false, 2500000, 2900000) == 2900000 + dlp::kSpareBytes);
    CHECK(dlp::SpaceNeeded(true, 2500000, 2900000) == 2900000 + 2500000 + dlp::kSpareBytes);
}

static void TestPlan() {
    const u64 app = 2500000, plugin = 250000, child = 2900000;
    const dlp::Plan plain = dlp::MakePlan(false, app, plugin, child);
    const dlp::Plan copy = dlp::MakePlan(true, app, plugin, child);
    CHECK(plain.total > 0.0 && plain.seconds[dlp::kStageCopy] == 0.0);
    CHECK(copy.seconds[dlp::kStageCopy] > 0.0 && copy.total > plain.total);
    // up to three minutes are fine for a one-time job (the user's decision): the usual sizes stay well inside it, with the copy too
    CHECK(copy.total < 180.0 && plain.total < 120.0);
    // every stage of the composer has a cost, and the sum is the total
    double sum = 0.0;
    for (int stage = 1; stage < dlp::kStages; ++stage) {
        CHECK(plain.seconds[stage] > 0.0);
        sum += plain.seconds[stage];
    }
    CHECK(sum > plain.total - 1e-9 && sum < plain.total + 1e-9);
    // the plugin is a resource of the app (in memory): its stage hashes it and reads nothing from the card
    dlp::Work plugin_work;
    plugin_work.sha = plugin;
    CHECK(plain.seconds[DLPC_STAGE_READ_PLUGIN] > dlp::WorkSeconds(plugin_work, dlp::Speeds()) - 1e-9 &&
          plain.seconds[DLPC_STAGE_READ_PLUGIN] < dlp::WorkSeconds(plugin_work, dlp::Speeds()) + 1e-9);
    // the arithmetic: 3 MiB read from a card at 3 MiB/s is a second; 0.7 MiB of AES at 0.7 MiB/s is a second
    dlp::Work w;
    w.sd_read = 3u << 20;
    CHECK(dlp::WorkSeconds(w, dlp::Speeds()) > 0.999 && dlp::WorkSeconds(w, dlp::Speeds()) < 1.001);
    dlp::Work a;
    a.aes = static_cast<u64>(0.7 * 1048576.0);
    CHECK(dlp::WorkSeconds(a, dlp::Speeds()) > 0.999 && dlp::WorkSeconds(a, dlp::Speeds()) < 1.001);
    dlp::Work all;
    all.sd_read = 3u << 20;
    all.sd_write = 3u << 20;
    all.sha = 2u << 20;
    all.aes = static_cast<u64>(0.7 * 1048576.0);
    CHECK(dlp::WorkSeconds(all, dlp::Speeds()) > 3.999 && dlp::WorkSeconds(all, dlp::Speeds()) < 4.001); // 1 + 1 + 1 + 1
    // a slower card makes it longer, a faster one shorter
    dlp::Speeds slow;
    slow.card_read = slow.card_write = 1.5;
    CHECK(dlp::MakePlan(false, app, plugin, child, slow).total > plain.total);
    // the plan from the composer's own work counts the same as the one from the sizes
    dlp::Work work[dlp::kStages];
    work[DLPC_STAGE_WRITE].sd_write = 3u << 20;
    CHECK(dlp::MakePlanFromWork(work).total > 0.999 && dlp::MakePlanFromWork(work).total < 1.001);
    CHECK(dlp::CopyWork(1000).sd_read == 1000 && dlp::CopyWork(1000).sd_write == 1000 && dlp::CopyWork(1000).aes == 1000 && dlp::CopyWork(1000).sha == 0);

    // the bar: starts at 0, only goes up through the stages and inside one, and is never full before the end
    double last = dlp::Fraction(copy, dlp::kStageCopy, 0, 100);
    CHECK(last == 0.0);
    for (int stage = 0; stage < dlp::kStages; ++stage) {
        for (u64 done = 0; done <= 100; done += 25) {
            const double f = dlp::Fraction(copy, stage, done, 100);
            CHECK(f >= last - 1e-12 && f <= 0.99);
            last = f;
        }
    }
    CHECK(last == 0.99); // the last stage, all done: still not 100%: the page fills the bar when the work says it is over
    CHECK(dlp::Fraction(plain, DLPC_STAGE_VERIFY, 100, 100) <= 0.99);
    CHECK(dlp::Fraction(plain, DLPC_STAGE_VERIFY, 200, 100) <= 0.99);            // (a stage that counts past its total)
    CHECK(dlp::Fraction(plain, DLPC_STAGE_SEAL, 5, 0) >= 0.0);                   // (a stage that has no total yet)
    CHECK(dlp::Fraction(dlp::Plan(), DLPC_STAGE_SEAL, 5, 10) == 0.0);            // (no plan: nothing to say)
    CHECK(dlp::Fraction(plain, 99, 5, 10) == 0.0 && dlp::Fraction(plain, -1, 5, 10) == 0.0);
    // the bar follows the cost, not the count of the stages: the last stage (the check, the dearest per byte) starts where the stages before it end
    const double verify_start = dlp::Fraction(plain, DLPC_STAGE_VERIFY, 0, 100);
    CHECK(verify_start > (plain.total - plain.seconds[DLPC_STAGE_VERIFY]) / plain.total - 1e-9 && verify_start < (plain.total - plain.seconds[DLPC_STAGE_VERIFY]) / plain.total + 1e-9);
    CHECK(verify_start < 5.0 / 6.0); // (the last of six stages starts before five sixths of the bar: it costs more than a sixth of the time)
    CHECK(dlp::Fraction(plain, DLPC_STAGE_WRITE, 100, 100) < verify_start + 1e-9);

    // the time left: the plan's at the start, the pace's once a seventh is done
    CHECK(dlp::SecondsLeft(plain, 0.0, 0.0) == static_cast<int>(plain.total + 0.5));
    CHECK(dlp::SecondsLeft(plain, 0.05, 1.0) == static_cast<int>(plain.total * 0.95 + 0.5));
    CHECK(dlp::SecondsLeft(plain, 0.5, 10.0) == 10);   // half done in 10 s: 10 s to go, whatever the plan said
    CHECK(dlp::SecondsLeft(plain, 0.5, 30.0) == 30);   // the card is slower than the plan: the time says so
    CHECK(dlp::SecondsLeft(plain, 0.99, 100.0) >= 1);  // never "0 s left" while it works
    CHECK(dlp::SecondsLeft(plain, 0.5, 1.0) == static_cast<int>(plain.total * 0.5 + 0.5)); // (too early to trust the clock: under two seconds)
    CHECK(dlp::SecondsLeft(dlp::Plan(), 0.5, 10.0) == -1);
    // the sentence on the screen for it (dlp_text.hpp)
    CHECK(dlp::TimeLeftText(dlp::SecondsLeft(plain, 0.5, 10.0)) == "about 10 s left");
    CHECK(dlp::PrepareIntro(0, false).find("about") == std::string::npos && dlp::PrepareIntro(0, true).find("about") == std::string::npos);
    CHECK(dlp::PrepareIntro(20, false).find("about 20 s") != std::string::npos);
    CHECK(dlp::PrepareIntro(95, false).find("about 1 min 35 s") != std::string::npos);
    CHECK(dlp::PrepareIntro(20, false).find("One time only") != std::string::npos && dlp::PrepareIntro(20, false).find("updated") != std::string::npos);
    // the speeds of the plan were never measured on a console: until the pace is seen the time is a first guess ("it can take longer"); once it is, it is the pace's
    // time, said plainly
    CHECK(dlp::PrepareIntro(20, false).find("should take about 20 s") != std::string::npos && dlp::PrepareIntro(20, false).find("it can take longer") != std::string::npos);
    CHECK(dlp::PrepareIntro(20, true).find("takes about 20 s") != std::string::npos && dlp::PrepareIntro(20, true).find("longer") == std::string::npos);
    CHECK(dlp::PrepareIntro(20, true).find("should") == std::string::npos && dlp::PrepareIntro(20, true).find("One time only") != std::string::npos);
    // when the pace counts: a seventh of the work and two seconds, the same moment for the time left and for the time of the whole work
    CHECK(!dlp::PaceKnown(0.0, 0.0) && !dlp::PaceKnown(0.14, 10.0) && !dlp::PaceKnown(0.5, 1.9) && dlp::PaceKnown(0.15, 2.0) && dlp::PaceKnown(0.9, 50.0));
    CHECK(dlp::WholeSeconds(plain, 0.05, 1.0) == plain.total);                 // (before: the plan's)
    CHECK(dlp::WholeSeconds(plain, 0.5, 1.0) == plain.total);                  // (too early to trust the clock)
    CHECK(dlp::WholeSeconds(plain, 0.5, 10.0) > 19.999 && dlp::WholeSeconds(plain, 0.5, 10.0) < 20.001);   // (half done in 10 s: 20 s in all, whatever the plan said)
    CHECK(dlp::WholeSeconds(plain, 0.25, 30.0) > 119.99 && dlp::WholeSeconds(plain, 0.25, 30.0) < 120.01);
    for (const double f : {0.0, 0.1, 0.15, 0.5, 0.99}) {                       // (the time left is the whole time over what is still to do: one rule)
        for (const double t : {0.0, 1.0, 2.0, 40.0}) {
            const double left = dlp::WholeSeconds(plain, f, t) * (1.0 - f);
            CHECK(dlp::SecondsLeft(plain, f, t) == (left < 1.0 ? 1 : static_cast<int>(left + 0.5)));
        }
    }
    // a plan made again (the file the composer refused, then the copy from the installed title): its clock begins at 0, so the time left is its own, not the first
    // attempt's: with the first attempt's 40 s on the clock and the bar back at the start the whole would be taken as 400 s; with a new clock it is the plan's
    CHECK(dlp::SecondsLeft(copy, 0.2, 2.0) == static_cast<int>(2.0 / 0.2 * 0.8 + 0.5));   // (a clock that began with the plan: 2 s for a fifth is 8 s to go)
    CHECK(dlp::SecondsLeft(copy, 0.2, 42.0) > 100);                                        // (the same bar on a clock that kept the 40 s of the attempt that was dropped)
}

static void TestMarker() {
    CHECK(dlp::MarkerText(1, "1.0.0") == "attempt=1\nversion=1.0.0\n");
    CHECK(dlp::MarkerText(3, "1.2.3") == "attempt=3\nversion=1.2.3\n");
    for (int n = 1; n <= 9; ++n) {
        CHECK(dlp::MarkerAttempts(dlp::MarkerText(n, "1.0.0")) == n);
    }
    CHECK(dlp::MarkerAttempts("") == 1);                 // a marker that says nothing is still a run that did not end
    CHECK(dlp::MarkerAttempts("some other text") == 1);
    CHECK(dlp::MarkerAttempts("attempt=x\n") == 1 && dlp::MarkerAttempts("attempt=0\n") == 1 && dlp::MarkerAttempts("attempt=-4\n") == 1);
    CHECK(dlp::MarkerAttempts("attempt=500\n") == 99);   // (a damaged number does not become a long sentence)
    CHECK(dlp::MarkerAttempts("version=1.0.0\nattempt=2\n") == 2);
    // what the question says: one run, then several in a row (and the way out)
    CHECK(dlp::DiedQuestion(1).find("did not finish") != std::string::npos && dlp::DiedQuestion(1).find("Try again?") != std::string::npos);
    CHECK(dlp::DiedQuestion(1).find("in a row") == std::string::npos);
    CHECK(dlp::DiedQuestion(3).find("3 preparations in a row") != std::string::npos && dlp::DiedQuestion(3).find("switch Download Play delivery off") != std::string::npos);
    CHECK(dlp::DiedQuestion(3).find("works without it") != std::string::npos);
}

static void TestFailures() {
    // every result of the composer and every one of ours has a phrase or a sentence and advice
    for (int r = 1; r <= DLPC_E_CANCEL; ++r) {
        CHECK(std::strlen(dlp::PrepAdvice(r)) > 10);
        CHECK(dlp::PrepPhrase(r) == nullptr); // (the composer's own words: dlpc_result_text)
    }
    for (const int r : {dlp::kPrepNoCopy, dlp::kPrepAppVersion, dlp::kPrepNoSpace, dlp::kPrepNoMemory, dlp::kPrepNoThread, dlp::kPrepNoModel}) {
        CHECK(dlp::PrepPhrase(r) != nullptr && std::strlen(dlp::PrepPhrase(r)) > 10);
        CHECK(std::strlen(dlp::PrepAdvice(r)) > 10);
    }
    // what another try can change
    // (the plugin is a resource of the app, the same on every try: a plugin the composer refuses is for a new CIA, not for another try)
    for (const int r : std::initializer_list<int>{DLPC_E_MODEL, DLPC_E_VERSION, DLPC_E_TOOBIG, dlp::kPrepNoModel, DLPC_E_CANCEL, DLPC_E_READ_PLUGIN, DLPC_E_PLUGIN_BAD}) {
        CHECK(!dlp::CanTryAgain(r));
    }
    for (const int r : std::initializer_list<int>{DLPC_E_READ_APP, DLPC_E_APP_BAD, DLPC_E_NOMEM, DLPC_E_WRITE, DLPC_E_VERIFY, dlp::kPrepNoCopy,
                        dlp::kPrepAppVersion, dlp::kPrepNoSpace, dlp::kPrepNoMemory, dlp::kPrepNoThread}) {
        CHECK(dlp::CanTryAgain(r));
    }
    // the sentences
    CHECK(dlp::Sentence("the card refused the file") == "The card refused the file.");
    CHECK(dlp::Sentence("The SD card refused the file.") == "The SD card refused the file.");
    CHECK(dlp::Sentence("Not enough memory!") == "Not enough memory!" && dlp::Sentence("").empty());
    CHECK(dlp::Capitalized("reading the plugin") == "Reading the plugin" && dlp::Capitalized("Reading") == "Reading" && dlp::Capitalized("").empty());
    CHECK(dlp::PrepareFailedText("The card refused the file.", "Free some space.") == "The card refused the file. Free some space.");
    CHECK(dlp::PrepareFailedText("", "Free some space.") == "Free some space." && dlp::PrepareFailedText("Sentence.", "") == "Sentence.");
    // the card never reads the same thing twice: the composer's detail often starts with the phrase of its result (the real pair of a damaged app CIA is below)
    CHECK(dlp::FailureSentence("the card refused the file", "") == "The card refused the file.");
    CHECK(dlp::FailureSentence("", "The detail.") == "The detail." && dlp::FailureSentence("", "").empty());
    CHECK(dlp::FailureSentence("The SD card refused the file.", "The SD card refused the file.") == "The SD card refused the file.");   // (the same: once)
    CHECK(dlp::FailureSentence("the card refused the file", "The card refused the file") == "The card refused the file.");              // (the same, in another form: once)
    CHECK(dlp::FailureSentence("The SD card refused the file.", "The SD card refused the file at the end (0x1234).") == "The SD card refused the file at the end (0x1234).");
    CHECK(dlp::FailureSentence("The SD card refused the file.", "The SD card is full.") == "The SD card refused the file. The SD card is full.");   // (something else: both)
    CHECK(dlp::FailureSentence("it went wrong", "the card said no") == "It went wrong. The card said no.");
    CHECK(dlp::FailureSentence("The app's installation file is not valid for this version.", "The app's installation file is not valid for this version.") ==
          "The app's installation file is not valid for this version.");
    const std::string bad_app = "The app's installation file is not valid for this version (the hash of the content does not match).";
    CHECK(dlp::FailureSentence(dlpc_result_text(DLPC_E_APP_BAD), bad_app) == bad_app);             // (what the composer says for a damaged CIA: the phrase and a bracket, once)
    CHECK(dlp::FailureSentence(dlpc_result_text(DLPC_E_APP_BAD), bad_app).find("version") == dlp::FailureSentence(dlpc_result_text(DLPC_E_APP_BAD), bad_app).rfind("version"));
    CHECK(dlp::FailureSentence(dlpc_result_text(DLPC_E_APP_BAD), "").find("not valid") != std::string::npos);
    CHECK(dlp::FailureSentence(dlpc_result_text(DLPC_E_PLUGIN_BAD), "The plugin file is not valid for this version (the header is not a 3gx).") ==
          "The plugin file is not valid for this version (the header is not a 3gx).");
    CHECK(dlp::PrepareFailedText(dlp::FailureSentence(dlpc_result_text(DLPC_E_APP_BAD), bad_app), dlp::PrepAdvice(DLPC_E_APP_BAD)).find("not valid for this version") ==
          dlp::PrepareFailedText(dlp::FailureSentence(dlpc_result_text(DLPC_E_APP_BAD), bad_app), dlp::PrepAdvice(DLPC_E_APP_BAD)).rfind("not valid for this version"));
    // a package that cannot be made says it, in words that name the file the user has to look at
    CHECK(std::string(dlp::PrepAdvice(DLPC_E_APP_BAD)).find("3DSLink.cia") != std::string::npos);
    // ... and the plugin is not a file anyone is asked to put anywhere: it is inside the app, and the way out is the CIA
    CHECK(std::string(dlp::PrepAdvice(DLPC_E_PLUGIN_BAD)).find("plugin inside this app") != std::string::npos);
    CHECK(std::string(dlp::PrepAdvice(DLPC_E_PLUGIN_BAD)).find("Install the 3DS Link CIA again") != std::string::npos);
    CHECK(std::string(dlp::PrepAdvice(DLPC_E_READ_PLUGIN)).find("plugin inside this app") != std::string::npos);
    CHECK(std::string(dlp::PrepAdvice(DLPC_E_PLUGIN_BAD)).find("next to 3DS Link") == std::string::npos && std::string(dlp::PrepAdvice(DLPC_E_READ_PLUGIN)).find("SD card") == std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// The composer's own figures, when the tree has a model to ask it with (Tests/run.sh makes one from the finished child of DlpLink/out and hands its path in)
// ---------------------------------------------------------------------------------------------

static bool ReadWhole(const char* path, std::vector<u8>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    u8 buf[65536];
    std::size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

static void TestComposerFigures(const char* model_path) {
    // bytes that are no model (the placeholder in ConfigApp/data is one): the composer says version 0, which is how the page knows this build has no installer
    const char* placeholder = "3DS Link: no child model in this build\n";
    CHECK(dlpc_model_version(reinterpret_cast<const u8*>(placeholder), std::strlen(placeholder)) == 0);
    CHECK(dlpc_model_version(nullptr, 0) == 0);

    std::vector<u8> model;
    std::vector<u8> app_cia;
    if (model_path == nullptr || !ReadWhole(model_path, model) || model.empty()) {
        std::printf("  (no model to ask the composer with: its figures are not checked)\n");
        return;
    }
    const unsigned version = dlpc_model_version(model.data(), model.size());
    std::printf("  model %s: %zu bytes, version %u (%s)\n", model_path, model.size(), version, dlp::VersionText(version).c_str());
    CHECK(version != 0);
    const std::string text = dlp::VersionText(version);
    const u64 app = 700000, plugin = 1000000; // (about what the tree's CIA and plugin weigh)

    // the composer lays the job out from the manifest, which says the version: a job without the version text gets 0 for everything (SizesJob carries it)
    const dlpc_job none = dlp::SizesJob(model.data(), model.size(), app, plugin, version, nullptr);
    CHECK(dlpc_output_size(&none) == 0 && dlpc_memory_needed(&none) == 0);
    const dlpc_job wrong = dlp::SizesJob(model.data(), model.size(), app, plugin, version + 1, text.c_str());
    CHECK(dlpc_output_size(&wrong) == 0); // (a text that is not the version the model carries)
    const dlpc_job job = dlp::SizesJob(model.data(), model.size(), app, plugin, version, text.c_str());
    const u64 child = dlpc_output_size(&job);
    CHECK(child > app + plugin && child < app + plugin + model.size() + 65536); // the model, the two payloads and a few tables
    CHECK(dlpc_memory_needed(&job) >= child);                                   // the package is built whole in memory
    CHECK(child <= dlp::kMaxChildBytes);

    // the plan from what each of the composer's stages does: every stage costs something, the bar steps through all of them, the total is within the user's minutes
    const dlp::Plan plain = dlp::PlanFor(job, false);
    const dlp::Plan copy = dlp::PlanFor(job, true);
    CHECK(plain.seconds[dlp::kStageCopy] == 0.0 && copy.seconds[dlp::kStageCopy] > 0.0 && copy.total > plain.total);
    for (int stage = DLPC_STAGE_READ_APP; stage <= DLPC_STAGE_VERIFY; ++stage) {
        CHECK(plain.seconds[stage] > 0.0);
        CHECK(plain.seconds[stage] == copy.seconds[stage]);
    }
    CHECK(plain.total > 1.0 && plain.total < 90.0 && copy.total < 180.0);
    // ... and what the composer counts as a read of the plugin from the card is not counted: the plugin is in memory (no card time, only the hash)
    dlp::Work plugin_hash_only, plugin_from_card;
    plugin_hash_only.sha = plugin;
    plugin_from_card.sha = plugin;
    plugin_from_card.sd_read = plugin;
    CHECK(plain.seconds[DLPC_STAGE_READ_PLUGIN] < dlp::WorkSeconds(plugin_from_card, dlp::Speeds()) &&
          plain.seconds[DLPC_STAGE_READ_PLUGIN] > dlp::WorkSeconds(plugin_hash_only, dlp::Speeds()) - 1e-9 &&
          plain.seconds[DLPC_STAGE_READ_PLUGIN] < dlp::WorkSeconds(plugin_hash_only, dlp::Speeds()) + 1e-9);
    std::printf("  the plan for 0.7 MB + 1.0 MB: %.1f s, %.1f s with the copy of the app's CIA (bar at the start of the last stage: %.0f%%)\n", plain.total, copy.total,
                100.0 * dlp::Fraction(plain, DLPC_STAGE_VERIFY, 0, 100));
    // the figures grow with the payloads
    const dlpc_job big = dlp::SizesJob(model.data(), model.size(), app * 4, plugin * 4, version, text.c_str());
    CHECK(dlpc_output_size(&big) > child && dlp::PlanFor(big, false).total > plain.total * 2.0);
    // a payload that cannot fit in a package: the composer gives a size past the limit, and the plan still comes out (from what the stages say)
    const dlpc_job huge = dlp::SizesJob(model.data(), model.size(), 40ull << 20, plugin, version, text.c_str());
    CHECK(dlpc_output_size(&huge) > dlp::kMaxChildBytes);
    // the plan when the composer cannot say anything (a job it cannot lay out) falls back on the sizes: the same order of magnitude
    const dlp::Plan fallback = dlp::PlanFor(none, false);
    CHECK(fallback.total > 0.0 && fallback.total < 90.0);
}

// ---------------------------------------------------------------------------------------------
// The real CIAs of the tree, when they are there (run from AzaharLink/; DlpLink is its neighbour): what the page would think of them
// ---------------------------------------------------------------------------------------------

static void TestRealFiles() {
    struct Real {
        const char* path;
        bool app; // the app's own CIA (else a child)
    };
    const Real files[] = {{"../DlpLink/out/3dslink.cia", true}, {"dist/3DSLink.cia", true}, {"../DlpLink/out/3dslink-child.cia", false}}; // (the child of makerom is the reference of the tests: dist/ no longer has one)
    int seen = 0;
    for (const Real& real : files) {
        std::vector<u8> bytes;
        if (!ReadWhole(real.path, bytes)) {
            continue;
        }
        ++seen;
        const dlp::CiaFacts f = Look(bytes);
        std::printf("  %s: %zu bytes, title %016llX version %u (%s), %u content(s)\n", real.path, bytes.size(), static_cast<unsigned long long>(f.tid),
                    static_cast<unsigned>(f.version), dlp::VersionText(f.version).c_str(), static_cast<unsigned>(f.contents));
        CHECK(f.parsed);
        if (real.app) {
            CHECK(f.tid == dlp::kAppTid);
            CHECK(dlp::JudgeAppCia(true, bytes.size(), f, f.version) == dlp::Verdict::Good);
            CHECK(dlp::JudgeAppCia(true, bytes.size(), f, f.version + 1) == dlp::Verdict::WrongVersion);
            CHECK(dlp::JudgeChild(true, bytes.size(), f, f.version) == dlp::Verdict::WrongTitle);
        } else {
            CHECK((f.tid >> 32) == 0x00040001u);
            CHECK(dlp::JudgeChild(true, bytes.size(), f, f.version) == dlp::Verdict::Good);
            CHECK(dlp::JudgeChild(true, bytes.size(), f, f.version + 1) == dlp::Verdict::WrongVersion);
            CHECK(dlp::JudgeAppCia(true, bytes.size(), f, f.version) == dlp::Verdict::WrongTitle);
            // cut anywhere in the contents: the header says more than the file holds, so it is not good
            const std::vector<u8> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
            CHECK(dlp::JudgeChild(true, cut.size(), Look(cut), f.version) == dlp::Verdict::NotCia);
        }
        // a file read as it is on the card: through reads at an offset, not through memory
        std::size_t asked = 0;
        const dlp::CiaFacts again = dlp::InspectCia(
            [&](u64 off, u8* buf, std::size_t len) {
                asked += len;
                if (off + len > bytes.size()) {
                    return false;
                }
                std::memcpy(buf, bytes.data() + off, len);
                return true;
            },
            bytes.size());
        CHECK(again.parsed && again.tid == f.tid && again.version == f.version && asked < 2048);
    }
    if (seen == 0) {
        std::printf("  (none of the real CIAs is in the tree: only the made-up ones were looked at)\n");
    }
}

int main(int argc, char** argv) {
    TestVersion16();
    TestInspectCia();
    TestJudge();
    TestChooseSource();
    TestPlan();
    TestMarker();
    TestFailures();
    TestComposerFigures(argc > 1 ? argv[1] : nullptr);
    TestRealFiles();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
