// Host-side test of azl/latency.hpp: the latency of the other players from the game's own UDS packets.
//   1. before the console joins a lobby nothing is taken and nothing is written (no ring: a game thread returns at the first test)
//   1b. the estimate does not depend on the log: with the log off (g_debug_level 0) the ring still works and Snapshot() has the values
//   (the module has no CPU counter of its own: 3DS Link's "stats:" line already measures the CPU of every thread, the module's included)
//   2. Pia: the timer echo gives the round trip; after enough packets the estimator locks on Pia and says so
//   3. PRUDP v1: the ping with a stamp (79-byte request, 91-byte reply) gives the round trip; the lock is on PRUDP
//   4. a game that sends neither: the packets are counted as other and the first bytes of one are kept
//   5. the ring never overflows into the estimator: a burst bigger than the ring drops the oldest, not the process, and the log says how many
//   6. the window: 30 s of half-seconds - a value that is steady while the samples jump, min / p90 in the log, the room link's line
//   8. Tekken: a short request (type 0xC4) of mine answered by a short reply (0x94) 33 ms later; the 281-byte frames list the node; the lock is on Tekken
//   7. the "ack" layer (Tetris): a reliable DATA of mine and the ACK of two nodes, minus my own hold; a resent message says nothing; a node that only
//      sends heartbeats is listed with no value; the lock is on the layer
//  10. a spike: a connection that gets worse for 2 s is shown at once (the last 2 s instead of the window), and the window is back when it is over
//   9. Super Street Fighter IV: my keepalive (type 7) answered by the ACK (type 3) with its number ~50 ms later, minus my own hold of ~20 ms; a keepalive sent
//      again says nothing; the data packets (type 0) list the node; the lock is on SSF4
// The clock is the real one (platform::NowTicks): the delays are made with sleeps, so the checks have a tolerance.
// Build & run: Tests/run.sh   (argument: off | nolog | pia | prudp | other | burst | window | ack | tekken | ssf4 | spike | file; no argument runs all, one process each)
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

// A packet of the "ack" layer (Tetris): see latency.cpp. type 0 DATA, 1 heartbeat, 6 ACK; msg / node at 36 / 38; sender node at 20.
static std::vector<u8> AckPacket(u8 type, u32 dest, u32 sender, u16 msg, u16 msg_node, std::size_t size = 52) {
    std::vector<u8> p(size, 0);
    auto put32 = [&](int o, u32 v) { p[o] = static_cast<u8>(v); p[o + 1] = static_cast<u8>(v >> 8); p[o + 2] = static_cast<u8>(v >> 16); p[o + 3] = static_cast<u8>(v >> 24); };
    put32(0, dest);
    p[8] = 0x80;
    p[10] = static_cast<u8>(size); p[11] = static_cast<u8>(size >> 8);
    put32(16, 1);
    put32(20, sender);
    put32(24, dest);
    p[28] = type; p[29] = 'a'; p[30] = 'c'; p[31] = 'k';
    put32(32, static_cast<u32>(size - 20));
    p[36] = static_cast<u8>(msg); p[37] = static_cast<u8>(msg >> 8);
    p[38] = static_cast<u8>(msg_node); p[39] = static_cast<u8>(msg_node >> 8);
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
    CHECK(n.window_s == 30 && !n.corrected); // (the ping's echo is a direct measure: M)
    CHECK(HasLine(" ms (30 s: min ") && HasLine(" samples, M)"));
}

// A packet of Tekken's layer: 02 01 src dst 01 00 | u16 length | 8 zero bytes | 11 11 type 00 | content; size = 16 + length
static std::vector<u8> TekkenPacket(u8 src, u8 dst, u8 type, std::size_t size) {
    std::vector<u8> p(size, 0);
    p[0] = 0x02; p[1] = 0x01; p[2] = src; p[3] = dst; p[4] = 0x01;
    const std::size_t len = size - 16;
    p[6] = static_cast<u8>(len); p[7] = static_cast<u8>(len >> 8);
    p[16] = 0x11; p[17] = 0x11; p[18] = type;
    return p;
}

// I am node 1 (the host), the other is node 2. Every "2 s" I send a request (30 bytes, 0xC4), node 2 answers with a 0x94 33 ms later; node 2 also sends its own request
// which I answer at once (it measures nothing for me); in between the 281-byte frames of both sides.
static void TestTekken() {
    Start();
    for (int i = 0; i < 14; ++i) {
        std::vector<u8> req = TekkenPacket(1, 2, 0xC4, 30);
        latency::OnSend(2, 0, req.data(), static_cast<u32>(req.size()));
        for (int k = 0; k < 2; ++k) {
            std::vector<u8> fr = TekkenPacket(2, 1, 0x82, 281);
            latency::OnRecv(2, 0, fr.data(), static_cast<u32>(fr.size()));
            std::vector<u8> my = TekkenPacket(1, 2, 0x82, 281);
            latency::OnSend(2, 0, my.data(), static_cast<u32>(my.size()));
            latency::Tick(0, true);
            SleepMs(16);
        }
        std::vector<u8> pong = TekkenPacket(2, 1, 0x94, 30);
        latency::OnRecv(2, 0, pong.data(), static_cast<u32>(pong.size()));
        std::vector<u8> theirs = TekkenPacket(2, 1, 0xC4, 30);
        latency::OnRecv(2, 0, theirs.data(), static_cast<u32>(theirs.size()));
        std::vector<u8> mine = TekkenPacket(1, 2, 0x94, 30);
        latency::OnSend(2, 0, mine.data(), static_cast<u32>(mine.size()));
        latency::Tick(0, true);
    }
    latency::Tick(2000, true);
    const latency::NodeRtt n2 = Node(2);
    char buf[300];
    CHECK(latency::ProtoSummary(buf, sizeof(buf)) > 0);
    std::printf("tekken: node 2 %d ms (%d samples), %s\n", n2.rtt_ms, n2.samples, buf);
    CHECK(n2.age_ms >= 0 && n2.samples == 14 && !n2.corrected && n2.window_s == 30);
    CHECK(n2.rtt_ms >= 28 && n2.rtt_ms <= 60); // ~32 ms: the two frames of 16 ms between the request and the reply
    CHECK(std::strstr(buf, "lock=Tekken") != nullptr && std::strstr(buf, "Tekken,") == buf);
    // a reply that nobody asked for (no request waiting) gives nothing
    std::vector<u8> stray = TekkenPacket(2, 1, 0x94, 30);
    latency::OnRecv(2, 0, stray.data(), static_cast<u32>(stray.size()));
    latency::Tick(3000, true);
    CHECK(Node(2).samples == 14);
}

// A message of Street Fighter's layer: u16 size-2, u16 size-4, u8 type, u8 flags, u32 packet number, u32 checksum, then the content
// (a keepalive: ack of the other's number at 20, its own number at 22; an ACK: the number acknowledged at 20).
static std::vector<u8> Sf4Packet(u8 type, u32 num, u16 at20, u16 at22, std::size_t size, u8 flags = 0) {
    std::vector<u8> p(size, 0);
    p[0] = static_cast<u8>(size - 2); p[1] = static_cast<u8>((size - 2) >> 8);
    p[2] = static_cast<u8>(size - 4); p[3] = static_cast<u8>((size - 4) >> 8);
    p[4] = type; p[5] = flags;
    p[6] = static_cast<u8>(num); p[7] = static_cast<u8>(num >> 8); p[8] = static_cast<u8>(num >> 16); p[9] = static_cast<u8>(num >> 24);
    p[10] = 0x5B; p[11] = 0x99; p[12] = 0xF8; p[13] = 0xE1;
    p[14] = 0x81;
    p[20] = static_cast<u8>(at20); p[21] = static_cast<u8>(at20 >> 8);
    p[22] = static_cast<u8>(at22); p[23] = static_cast<u8>(at22 >> 8);
    return p;
}

// I am the host, the other is node 2. Every "second" node 2's keepalive arrives and my game acknowledges it 20 ms later (the hold); my own keepalive leaves and node 2's
// ACK with its number comes back 50 ms later: the sample is 50 - 20 = 30 ms. In between the data packets of both sides (type 0, 100 bytes, 60 a second).
static void TestSf4() {
    Start();
    u32 num = 1;
    for (int i = 0; i < 12; ++i) {
        const u16 mine = static_cast<u16>(1 + i);
        const u16 theirs = static_cast<u16>(100 + i);
        std::vector<u8> ka = Sf4Packet(7, num++, static_cast<u16>(mine - 1), theirs, 44);
        latency::OnRecv(2, 0, ka.data(), static_cast<u32>(ka.size()));
        for (int k = 0; k < 2; ++k) {
            std::vector<u8> d = Sf4Packet(0, num++, 0x1234, 0x5678, 100, 0xFF);
            latency::OnRecv(2, 0, d.data(), static_cast<u32>(d.size()));
            std::vector<u8> my = Sf4Packet(0, num++, 0x4321, 0x8765, 100, 0xFF);
            latency::OnSend(255, 0, my.data(), static_cast<u32>(my.size()));
            latency::Tick(0, true);
            SleepMs(10);
        }
        std::vector<u8> myack = Sf4Packet(3, num++, theirs, 0, 28);
        latency::OnSend(2, 0, myack.data(), static_cast<u32>(myack.size()));
        std::vector<u8> myka = Sf4Packet(7, num++, theirs, mine, 44);
        latency::OnSend(255, 0, myka.data(), static_cast<u32>(myka.size()));
        SleepMs(50);
        std::vector<u8> ack = Sf4Packet(3, num++, mine, 0, 28);
        latency::OnRecv(2, 0, ack.data(), static_cast<u32>(ack.size()));
        latency::Tick(0, true);
    }
    // a keepalive sent again before its ACK: the ACK that follows says nothing
    std::vector<u8> again = Sf4Packet(7, num++, 111, 99, 44);
    latency::OnSend(255, 0, again.data(), static_cast<u32>(again.size()));
    SleepMs(30);
    latency::OnSend(255, 0, again.data(), static_cast<u32>(again.size()));
    SleepMs(200);
    std::vector<u8> late = Sf4Packet(3, num++, 99, 0, 28);
    latency::OnRecv(2, 0, late.data(), static_cast<u32>(late.size()));
    latency::Tick(2000, true);
    const latency::NodeRtt n2 = Node(2);
    char buf[300];
    CHECK(latency::ProtoSummary(buf, sizeof(buf)) > 0);
    std::printf("ssf4: node 2 %d ms (%d samples, A %d), %s\n", n2.rtt_ms, n2.samples, n2.corrected ? 1 : 0, buf);
    CHECK(n2.age_ms >= 0 && n2.samples == 12);          // (the keepalive sent again gave no sample)
    CHECK(n2.corrected && n2.window_s == 30);           // (the hold is an estimate: A)
    CHECK(n2.rtt_ms >= 22 && n2.rtt_ms <= 45);          // ~50 ms seen - ~20 ms of my own hold = ~30 ms
    CHECK(std::strstr(buf, "lock=SSF4") != nullptr && std::strstr(buf, "SSF4,") == buf);
    CHECK(HasLine("lat: n2 ") && HasLine(" samples, A)"));
}

// I am node 1. Node 2 and node 3 receive my reliable DATA and acknowledge it about 60 ms later (net 30 ms + their hold ~30 ms); the data of node 2 is held by my game
// for ~20 ms before my ACK leaves, which is the hold subtracted. Node 3 sends heartbeats only for the first seconds.
static void TestAck() {
    Start();
    for (int i = 0; i < 12; ++i) {
        const u16 msg = static_cast<u16>(1 + i);
        // node 2's reliable data arrives, my game acknowledges it 20 ms later
        std::vector<u8> in = AckPacket(0, 0xFFFFFFFFu, 2, msg, 2, 124);
        latency::OnRecv(2, 0, in.data(), static_cast<u32>(in.size()));
        std::vector<u8> hb = AckPacket(1, 0xFFFFFFFFu, 3, 0, 0);
        latency::OnRecv(3, 0, hb.data(), static_cast<u32>(hb.size()));
        SleepMs(20);
        std::vector<u8> ack = AckPacket(6, 2, 1, msg, 2);
        latency::OnSend(2, 0, ack.data(), static_cast<u32>(ack.size()));
        // my own reliable data, broadcast; node 2 answers 60 ms later (node 3 sends no ACK at all)
        std::vector<u8> out = AckPacket(0, 0xFFFFFFFFu, 1, msg, 1, 108);
        latency::OnSend(255, 0, out.data(), static_cast<u32>(out.size()));
        SleepMs(60);
        std::vector<u8> back = AckPacket(6, 1, 2, msg, 1);
        latency::OnRecv(2, 0, back.data(), static_cast<u32>(back.size()));
        latency::Tick(0, true);
    }
    // a message sent twice: the ACK that follows says nothing (which send does it answer?)
    std::vector<u8> out = AckPacket(0, 0xFFFFFFFFu, 1, 99, 1, 108);
    latency::OnSend(255, 0, out.data(), static_cast<u32>(out.size()));
    SleepMs(30);
    latency::OnSend(255, 0, out.data(), static_cast<u32>(out.size()));
    SleepMs(300);
    std::vector<u8> back = AckPacket(6, 1, 2, 99, 1);
    latency::OnRecv(2, 0, back.data(), static_cast<u32>(back.size()));
    latency::Tick(2000, true);
    const latency::NodeRtt n2 = Node(2);
    const latency::NodeRtt n3 = Node(3);
    std::printf("ack: node 2 %d ms (%d samples, A %d), node 3 %d ms age %d\n", n2.rtt_ms, n2.samples, n2.corrected ? 1 : 0, n3.rtt_ms, n3.age_ms);
    CHECK(n2.age_ms >= 0 && n2.samples == 12);          // (the resent message gave no sample)
    CHECK(n2.corrected && n2.window_s == 30);           // (the hold is an estimate: A)
    CHECK(n2.rtt_ms >= 28 && n2.rtt_ms <= 60);          // ~60 ms ACK delay - ~20 ms of my own hold = ~40 ms
    CHECK(n3.age_ms >= 0 && n3.rtt_ms < 0);             // node 3 is listed (it sends) with no value
    char buf[300];
    CHECK(latency::ProtoSummary(buf, sizeof(buf)) > 0);
    std::printf("ack: %s\n", buf);
    CHECK(std::strstr(buf, "AckTag") != nullptr);
    CHECK(HasLine("lat: n2 ") && HasLine(" samples, A)"));
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
    CHECK(HasLine("lat: ring dropped 4872 packets in all")); // (5000 came, the ring holds 128: what was lost is written, once)
    g_lines.clear();
    latency::Tick(4000, true);
    CHECK(!HasLine("lat: ring dropped")); // (nothing more was lost: no line)
}

// The window. Node 2 answers 10 times a second for 4 s (as Monster Hunter does): for 3 s after ~20 ms, with one answer in four after ~80 ms (the other game's
// frame), then for a whole second after ~80 ms (the connection got worse). The value stays the steady ~20 (a second of trouble does not move the median of the
// window), the p90 shows the trouble, and every line says what it is made of. The network thread empties the ring as it goes. Then the room link's own line.
static void TestWindow() {
    Start();
    for (int i = 0; i < 40; ++i) {
        const u16 timer = static_cast<u16>(1000 + i * 100);
        std::vector<u8> out = PiaPacket(timer, 0);
        latency::OnSend(2, 0, out.data(), static_cast<u32>(out.size()));
        const int wait = (i % 4) == 3 || i >= 30 ? 80 : 20;
        SleepMs(wait);
        std::vector<u8> in = PiaPacket(static_cast<u16>(5000 + i * 100), timer);
        latency::OnRecv(2, 0, in.data(), static_cast<u32>(in.size()));
        latency::Tick(0, true); // (a round of the network thread: the ring is emptied; no line is due)
        SleepMs(100 - wait);
    }
    latency::Tick(2000, true);
    const latency::NodeRtt n = Node(2);
    std::printf("window: rtt %d ms, min %d, p90 %d, last %d, %d samples, window %d s\n", n.rtt_ms, n.min_ms, n.p90_ms, n.last_ms, n.samples, n.window_s);
    CHECK(n.samples == 40 && n.window_s == 30 && !n.corrected && !n.stale);
    CHECK(n.window_ms >= 19 && n.window_ms <= 45); // the median of the half-seconds: the steady value, not the jumps
    CHECK(n.spike && n.rtt_ms >= 70 && n.rtt_ms == n.recent_ms); // ...but the last second got worse (80 ms over 40): that is what is shown (see TestSpike)
    CHECK(n.min_ms >= 19 && n.min_ms <= n.window_ms);
    CHECK(n.p90_ms >= 70 && n.p90_ms <= 110); // the bad second is in the 90th percentile
    CHECK(!HasLine("lat: ring dropped"));      // (nothing was lost: the ring was read between the packets)
    CHECK(n.last_ms >= 78);                     // (the newest sample was one of the slow ones)
    CHECK(HasLine("lat: n2 ") && HasLine(" ms (30 s: min ") && HasLine(" samples, M)"));
    // this console's own link to the room, as the plugin hands it over: a line when it changes, not when it does not
    CHECK(!HasLine("lat: room link"));
    latency::NoteLink(6, 4, 5);
    latency::Tick(4000, true);
    CHECK(HasLine("lat: room link 6+-4 ms, loss 5/1000"));
    g_lines.clear();
    latency::Tick(6000, true);
    CHECK(!HasLine("lat: room link"));
    latency::NoteLink(48, 30, 120);
    latency::Tick(8000, true);
    CHECK(HasLine("lat: room link 48+-30 ms, loss 120/1000"));
}

// Pia echoes at 40 ms for 3 s, then 500 ms for 2 s (the connection got worse), then 40 ms again: the shown value jumps to ~500 while it lasts and comes
// back to the window's ~40 when it is over; the window's own median never leaves ~40.
static void TestSpike() {
    Start();
    auto echo = [](int i, int wait) {
        const u16 timer = static_cast<u16>(1000 + i * 10);
        std::vector<u8> out = PiaPacket(timer, 0);
        latency::OnSend(2, 0, out.data(), static_cast<u32>(out.size()));
        SleepMs(wait);
        std::vector<u8> in = PiaPacket(static_cast<u16>(5000 + i * 10), timer);
        latency::OnRecv(2, 0, in.data(), static_cast<u32>(in.size()));
        latency::Tick(0, true);
    };
    int i = 0;
    for (; i < 30; ++i) { echo(i, 40); SleepMs(60); }        // 3 s at 40 ms, 10 echoes a second
    latency::Tick(1000, true);
    const latency::NodeRtt before = Node(2);
    for (; i < 34; ++i) { echo(i, 500); }                      // 2 s at 500 ms
    latency::Tick(2000, true);
    const latency::NodeRtt during = Node(2);
    for (; i < 60; ++i) { echo(i, 40); SleepMs(60); }        // 2.6 s at 40 ms again
    latency::Tick(3000, true);
    const latency::NodeRtt after = Node(2);
    std::printf("spike: before %d ms (window %d, recent %d), during %d ms (window %d, recent %d, spike %d), after %d ms (window %d, recent %d, spike %d)\n",
                before.rtt_ms, before.window_ms, before.recent_ms, during.rtt_ms, during.window_ms, during.recent_ms, during.spike ? 1 : 0,
                after.rtt_ms, after.window_ms, after.recent_ms, after.spike ? 1 : 0);
    CHECK(!before.spike && before.rtt_ms >= 38 && before.rtt_ms <= 60 && before.rtt_ms == before.window_ms);
    CHECK(during.spike && during.rtt_ms >= 450 && during.recent_ms == during.rtt_ms);
    CHECK(during.window_ms >= 38 && during.window_ms <= 80); // the window barely moved: that is why the last 2 s are shown instead
    CHECK(!after.spike && after.rtt_ms >= 38 && after.rtt_ms <= 60 && after.rtt_ms == after.window_ms);
    CHECK(HasLine("lat: n2 spike: the last 2 s are ") && HasLine("lat: n2 spike over: back to the window's "));
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
        for (const char* mode : {"off", "nolog", "pia", "prudp", "other", "burst", "window", "ack", "tekken", "ssf4", "spike", "file"}) {
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
    else if (mode == "window") TestWindow();
    else if (mode == "ack") TestAck();
    else if (mode == "tekken") TestTekken();
    else if (mode == "ssf4") TestSf4();
    else if (mode == "spike") TestSpike();
    else if (mode == "file") TestFile();
    SetLogSink(nullptr);
    if (g_failures) std::printf("%s: %d failure(s)\n", mode.c_str(), g_failures);
    return g_failures ? 1 : 0;
}
