// Host-side test of azl/latency.hpp: the latency of the other players from the game's own UDS packets.
//   1. before the console joins a lobby nothing is taken and nothing is written (no ring: a game thread returns at the first test)
//   1b. the estimate does not depend on the log: with the log off (g_debug_level 0) the ring still works and Snapshot() has the values
//   (the module has no CPU counter of its own: 3DS Link's "stats:" line already measures the CPU of every thread, the module's included)
//   2. Pia: the timer echo gives the round trip; after enough packets the estimator locks on Pia and says so
//   3. PRUDP v1: the ping with a stamp (79-byte request, 91-byte reply) gives the round trip; the lock is on PRUDP
//   4. a game that sends neither: the packets are counted as other and the first bytes of one are kept
//   5. the ring never overflows into the estimator: a burst bigger than the ring drops the oldest, not the process
// The clock is the real one (platform::NowTicks): the delays are made with sleeps, so the checks have a tolerance.
// Build & run: Tests/run.sh   (argument: off | nolog | pia | prudp | other | burst | file; no argument runs all, one process each)
#include <chrono>
#include <ctime>
#include <map>
#include <regex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "azl/latency.hpp"
#include "azl/log.hpp"
#include "azl/stats.hpp"

using namespace azl;

static int g_failures = 0;
static std::vector<std::string> g_lines;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// A Pia packet: 12 bytes of header, timer of the sender (BE) at 8-9, the echo (BE) at 10-11, then some body.
static std::vector<u8> PiaPacket(u16 timer, u16 echo) {
    std::vector<u8> p(48, 0);
    p[0] = 0x32; p[1] = 0xAB; p[2] = 0x98; p[3] = 0x64;
    p[8] = static_cast<u8>(timer >> 8); p[9] = static_cast<u8>(timer);
    p[10] = static_cast<u8>(echo >> 8); p[11] = static_cast<u8>(echo);
    return p;
}

// A PRUDP v1 packet as the game gives it over UDS: 16 bytes of wrapper, EA D0 01, signature size 3, payload length, type 2 (DATA)
// with flags 0x8 (unreliable), then the payload of the NEX ping: u32 length, ..., byte 20 = 2 request / 3 reply, byte 21 = 9, u16 stamp at 22.
static std::vector<u8> PrudpPing(bool reply, u16 stamp, u16 seq) {
    const int total = reply ? 91 : 79;
    std::vector<u8> p(total, 0);
    const int po = 16;
    p[po] = 0xEA; p[po + 1] = 0xD0; p[po + 2] = 0x01; p[po + 3] = 3;
    const int pay = total - (po + 14 + 3 + 16);
    p[po + 4] = static_cast<u8>(pay); p[po + 5] = static_cast<u8>(pay >> 8);
    const u16 tf = static_cast<u16>((0x8 << 4) | 2);
    p[po + 8] = static_cast<u8>(tf); p[po + 9] = static_cast<u8>(tf >> 8);
    p[po + 12] = static_cast<u8>(seq); p[po + 13] = static_cast<u8>(seq >> 8);
    u8* q = &p[po + 14 + 3 + 16];
    const u32 declared = static_cast<u32>(pay - 4);
    q[0] = static_cast<u8>(declared); q[1] = static_cast<u8>(declared >> 8); q[2] = static_cast<u8>(declared >> 16); q[3] = static_cast<u8>(declared >> 24);
    q[20] = reply ? 3 : 2;
    q[21] = 9;
    q[22] = static_cast<u8>(stamp); q[23] = static_cast<u8>(stamp >> 8);
    return p;
}

static latency::NodeRtt Node(int node) {
    latency::NodeRtt nr[8];
    latency::Snapshot(nr);
    return nr[node - 1];
}

static void Start() {
    SetLogSink([](const char* line) { g_lines.push_back(line); });
    g_debug_level = 1;
    latency::Tick(0, true); // the console is in a lobby: creates the ring
}

static bool HasLine(const char* part) {
    for (const std::string& l : g_lines) {
        if (l.find(part) != std::string::npos) return true;
    }
    return false;
}

static void TestOff() {
    g_debug_level = 0;
    std::vector<u8> p = PiaPacket(100, 0);
    latency::OnSend(2, 0, p.data(), static_cast<u32>(p.size())); // no ring: nothing
    latency::Tick(0, false);
    latency::Tick(5000, false);
    latency::OnRecv(2, 0, p.data(), static_cast<u32>(p.size()));
    latency::Tick(6000, false); // still not in a lobby: nothing
    CHECK(g_lines.empty());
    latency::NodeRtt nr[8];
    latency::Snapshot(nr);
    for (int i = 0; i < 8; ++i) CHECK(nr[i].age_ms < 0);
}


static void TestNoLog() {
    SetLogSink([](const char* line) { g_lines.push_back(line); });
    g_debug_level = 0; // the log is off, the console is in a lobby
    latency::Tick(0, true);
    for (int i = 0; i < 30; ++i) {
        const u16 timer = static_cast<u16>(1000 + i * 100);
        std::vector<u8> out = PiaPacket(timer, 0);
        latency::OnSend(2, 0, out.data(), static_cast<u32>(out.size()));
        SleepMs(30);
        std::vector<u8> in = PiaPacket(static_cast<u16>(5000 + i * 100), timer);
        latency::OnRecv(2, 0, in.data(), static_cast<u32>(in.size()));
    }
    latency::Tick(2000, true);
    latency::Tick(15000, true);
    CHECK(g_lines.empty()); // nothing written while the log is off ...
    const latency::NodeRtt n = Node(2);
    std::printf("nolog: rtt %d ms, %d samples\n", n.rtt_ms, n.samples);
    CHECK(n.age_ms >= 0 && n.rtt_ms >= 28 && n.rtt_ms <= 80); // ... but the estimate is kept
}

static void TestPia() {
    Start();
    // 40 round trips of about 40 ms with node 2: I send timer T, it answers with echo T (and its own timer)
    for (int i = 0; i < 40; ++i) {
        const u16 timer = static_cast<u16>(1000 + i * 100);
        std::vector<u8> out = PiaPacket(timer, 0);
        latency::OnSend(2, 0, out.data(), static_cast<u32>(out.size()));
        SleepMs(40);
        std::vector<u8> in = PiaPacket(static_cast<u16>(5000 + i * 100), timer);
        latency::OnRecv(2, 0, in.data(), static_cast<u32>(in.size()));
    }
    latency::Tick(2000, true);
    const latency::NodeRtt n = Node(2);
    std::printf("pia: rtt %d ms, %d samples\n", n.rtt_ms, n.samples);
    CHECK(n.age_ms >= 0);
    CHECK(n.rtt_ms >= 38 && n.rtt_ms <= 90);
    CHECK(n.samples >= 20);
    char buf[300];
    CHECK(latency::ProtoSummary(buf, sizeof(buf)) > 0);
    CHECK(std::strstr(buf, "lock=Pia") != nullptr);
    CHECK(HasLine("lat: nodes sending"));
    CHECK(HasLine("lat: n2"));
}

static void TestPrudp() {
    Start();
    for (int i = 0; i < 40; ++i) {
        const u16 stamp = static_cast<u16>(0x1000 + i);
        std::vector<u8> req = PrudpPing(false, stamp, static_cast<u16>(i));
        latency::OnSend(1, 0, req.data(), static_cast<u32>(req.size()));
        SleepMs(30);
        std::vector<u8> rep = PrudpPing(true, stamp, static_cast<u16>(i));
        latency::OnRecv(1, 0, rep.data(), static_cast<u32>(rep.size()));
    }
    latency::Tick(2000, true);
    const latency::NodeRtt n = Node(1);
    std::printf("prudp: rtt %d ms, %d samples\n", n.rtt_ms, n.samples);
    CHECK(n.age_ms >= 0);
    CHECK(n.rtt_ms >= 28 && n.rtt_ms <= 80);
    char buf[300];
    CHECK(latency::ProtoSummary(buf, sizeof(buf)) > 0);
    CHECK(std::strstr(buf, "PRUDP v1") != nullptr);
    CHECK(std::strstr(buf, "lock=PRUDP") != nullptr);
}

static void TestOther() {
    Start();
    std::vector<u8> p(60, 0x5A);
    p[0] = 0x02; p[1] = 0x01; p[2] = 0x01; p[3] = 0xFF;
    for (int i = 0; i < 10; ++i) {
        latency::OnRecv(1, 0, p.data(), static_cast<u32>(p.size()));
    }
    latency::Tick(2000, true);
    char buf[300];
    CHECK(latency::ProtoSummary(buf, sizeof(buf)) > 0);
    std::printf("other: %s\n", buf);
    CHECK(std::strstr(buf, "lock=-") != nullptr);
    CHECK(std::strstr(buf, ",UR,60,020101FF") != nullptr); // the first bytes of the unknown packet, once
    CHECK(Node(1).rtt_ms < 0);
}

static void TestBurst() {
    Start();
    std::vector<u8> p = PiaPacket(1, 0);
    for (int i = 0; i < 5000; ++i) { // far more than the ring holds, with no Tick in between
        latency::OnRecv(static_cast<u16>(1 + (i % 3)), 0, p.data(), static_cast<u32>(p.size()));
    }
    latency::Tick(2000, true);
    CHECK(Node(1).age_ms >= 0 || Node(2).age_ms >= 0 || Node(3).age_ms >= 0);
}

// ---- latency.log: the lines go to their own file, each begun by the console's clock ----
struct MemFiles : FileLayer {
    std::map<std::string, std::string> data;
    int opens = 0, flushes = 0;
    struct H {
        std::string path;
    };
    void* OpenAppend(const std::string& path) override {
        ++opens;
        data[path];
        return new H{path};
    }
    u64 Size(void* f) override { return data[static_cast<H*>(f)->path].size(); }
    void Write(void* f, const char* d, std::size_t n) override { data[static_cast<H*>(f)->path].append(d, n); }
    void Flush(void*) override { ++flushes; }
    void Close(void* f) override { delete static_cast<H*>(f); }
    void Remove(const std::string& p) override { data.erase(p); }
    void Rename(const std::string& a, const std::string& b) override {
        data[b] = data[a];
        data.erase(a);
    }
    bool Exists(const std::string& p) override { return data.count(p) != 0; }
};

static u64 g_fake_os_ms = 0;
static u64 FakeClock() {
    return g_fake_os_ms;
}

// 2026-11-11 12:20:31.482 on the console's clock (ms since 1900-01-01)
static u64 OsMs(int y, int mo, int d, int h, int mi, int s, int ms) {
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
#ifdef _WIN32
    const time_t e = _mkgmtime(&t);
#else
    const time_t e = timegm(&t);
#endif
    return (static_cast<u64>(e) + 2208988800ULL) * 1000ULL + static_cast<u64>(ms);
}

static bool IsStamped(const std::string& line) { // "hh:mm:ss.mmm lat: ..."
    if (line.size() < 18) return false;
    for (int i : {0, 1, 3, 4, 6, 7, 9, 10, 11}) {
        if (line[i] < '0' || line[i] > '9') return false;
    }
    return line[2] == ':' && line[5] == ':' && line[8] == '.' && line.compare(12, 5, " lat:") == 0;
}

static void TestFile() {
    MemFiles fs;
    {
        latency::LatencyFile file(&fs, "/3ds/3DSLink/");
        latency::SetSink(&file, &FakeClock);
        latency::SetNameSource([](int node, char* out, u32 cap) { // (the game's name for node 2; none for the others)
            if (node != 2 || cap < 4) return false;
            std::strcpy(out, "Bob");
            return true;
        });
        g_fake_os_ms = OsMs(2026, 11, 11, 12, 20, 31, 482);
        // log off: the ring still works, nothing is queued for a file, nothing is created
        SetLogSink([](const char* line) { g_lines.push_back(line); });
        g_debug_level = 0;
        latency::Tick(0, false);
        file.Flush();
        CHECK(fs.data.empty() && fs.opens == 0);
        // log on, the console joins a lobby: the ring begins and the file's first line says the day
        g_debug_level = 1;
        latency::Tick(0, true);
        for (int i = 0; i < 40; ++i) {
            const u16 timer = static_cast<u16>(1000 + i * 100);
            std::vector<u8> out = PiaPacket(timer, 0);
            latency::OnSend(2, 0, out.data(), static_cast<u32>(out.size()));
            SleepMs(20);
            std::vector<u8> in = PiaPacket(static_cast<u16>(5000 + i * 100), timer);
            latency::OnRecv(2, 0, in.data(), static_cast<u32>(in.size()));
        }
        latency::Tick(2000, true);
        CHECK(fs.data.empty()); // (queued: the network thread never touches the card)
        file.Flush();
        CHECK(fs.data.count("/3ds/3DSLink/latency.log") == 1);
        const std::string text = fs.data["/3ds/3DSLink/latency.log"];
        std::printf("latency.log:\n%s", text.c_str());
        CHECK(!text.empty());
        std::size_t pos = 0, lines = 0;
        bool all_stamped = true;
        while (pos < text.size()) {
            const std::size_t e = text.find('\n', pos);
            if (e == std::string::npos) { all_stamped = false; break; }
            all_stamped = all_stamped && IsStamped(text.substr(pos, e - pos));
            ++lines;
            pos = e + 1;
        }
        CHECK(all_stamped && lines >= 2);
        CHECK(text.compare(0, 13, "12:20:31.482 ") == 0); // (the console's clock, hours:minutes:seconds.milliseconds)
        CHECK(text.find("lat: ---- the console joined a lobby, console clock 2026-11-11 12:20:31.482 ----\n") == 13);
        CHECK(text.find("lat: n2 \"Bob\" ") != std::string::npos); // (who it was, by name)
        CHECK(!std::regex_search(text, std::regex("[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}"))); // (and never an address of a console: six pairs of hex digits with colons)
        CHECK(text.find("lat: nodes sending") != std::string::npos);
        CHECK(fs.flushes >= 1);
        // nothing of the module in the main log's sink any more
        CHECK(!HasLine("lat:"));
        // the file is written again with the next lines (appended, one handle)
        g_fake_os_ms += 5000;
        latency::Tick(9000, true);
        file.Flush();
        CHECK(fs.opens == 1);
        CHECK(fs.data["/3ds/3DSLink/latency.log"].size() > text.size());
        // the log goes off: what was queued is dropped, nothing more is written
        latency::Tick(15000, true);
        g_debug_level = 0;
        const std::size_t size_off = fs.data["/3ds/3DSLink/latency.log"].size();
        file.Append("12:00:00.000 lat: late\n", static_cast<u32>(std::strlen("12:00:00.000 lat: late\n")));
        file.Flush();
        CHECK(fs.data["/3ds/3DSLink/latency.log"].size() == size_off);
        latency::SetSink(nullptr, nullptr);
        latency::SetNameSource(nullptr);
    }
    // rotation: when the file reaches its size it becomes latency.1.log (the one before it goes) and a fresh one begins
    {
        MemFiles r;
        g_debug_level = 1;
        latency::LatencyFile file(&r, "/d/", 100);
        for (int i = 0; i < 12; ++i) {
            file.Append("12:00:00.000 lat: 0123456789012345678901234567890\n", static_cast<u32>(std::strlen("12:00:00.000 lat: 0123456789012345678901234567890\n")));
            file.Flush();
        }
        CHECK(r.data.count("/d/latency.1.log") == 1 && r.data.count("/d/latency.log") <= 1);
        CHECK(r.data.size() <= 2);
        CHECK(r.data["/d/latency.1.log"].size() >= 100 && r.data["/d/latency.1.log"].size() <= 200);
        file.Close();
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        int bad = 0;
        for (const char* mode : {"off", "nolog", "pia", "prudp", "other", "burst", "file"}) {
            const std::string cmd = std::string("\"") + argv[0] + "\" " + mode;
            if (std::system(cmd.c_str()) != 0) ++bad;
        }
        std::printf(bad ? "latency_test: FAILED\n" : "latency_test: all passed\n");
        return bad ? 1 : 0;
    }
    const std::string mode = argv[1];
    if (mode == "off") TestOff();
    else if (mode == "nolog") TestNoLog();
    else if (mode == "pia") TestPia();
    else if (mode == "prudp") TestPrudp();
    else if (mode == "other") TestOther();
    else if (mode == "burst") TestBurst();
    else if (mode == "file") TestFile();
    SetLogSink(nullptr);
    if (g_failures) std::printf("%s: %d failure(s)\n", mode.c_str(), g_failures);
    return g_failures ? 1 : 0;
}
