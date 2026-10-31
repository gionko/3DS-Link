// End-to-end simulation of the fake Download Play lobby (phase 1 of analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE1.md).
//
// Several consoles, each a REAL DlpService over a REAL DlpLobby, joined by the in-memory air of Tests/dlp_fakenet.hpp
// (FakeLobbyNet) and a simulated clock. Every console is driven ONLY through IPC commands: the request words are laid out by
// hand the way a game sends them (SPEC-DLP.md sez. 2-3, GIOCO-TD-PR.md sez. 1), the answers are read from the reply words and
// from the buffers of the "game" (a memory of regions with canaries around them), StartFakeSession is the JoinPending /
// PollPending loop, and time only moves when the test moves the clock. The only calls that are not IPC are the two things the
// plugin itself does around the game: the network thread's Pump() and the event OnGameUdsStarting(); and the observation
// of counters (lobby.active(), the air's call counters) that no game could see.
//
// The "games" are two small state machines written after what the games really call:
//   * Guest (the searcher, dlp:FKCL): Triple Deluxe / Planet Robobot order (FKCL Initialize with no name, GetChannels inside
//     StartScan, GetTitleInfoInOrder(1)/(0) until exactly "no data" + GetServerInfo, the 16-entry list aged by the game's own
//     arithmetic, DeleteScanInfo, StopScan, StartFakeSession, GetMyStatus every frame, GetConnectingNodes + GetNodeInfo,
//     GetWirelessRebootPassphrase at 9, Finalize) or Battle Royale order (InitializeWithName, StopSession);
//   * Host (the creator, dlp:SRVR): TD/PR order (Initialize(3) + GetDupNoticeNeed, StartAccepting(0, 6), GetServerState every
//     frame, GetConnectingClients(4) + GetClientInfo, StartDistribution, the passphrase "%08x" sent EVERY frame in state 8,
//     "state 9 AND zero clients", Finalize) or BR order (InitializeWithName with 4 clients, the passphrase sent once).
// Expected values are literals written by hand from the specification, not the constants of the code under test.
//
// Scenarios:
//   1 Battle Royale: 2 consoles and 4 consoles (the clients come in at different times, and all at once), the whole sequence
//     of FLUSSO-DLP-KIRBY-BR.md; and the fifth console that tries to come in (late, with a stale list that still shows the group;
//     racing the other three; or only looking): it is never admitted (the engine stays silent, the join times out at 3000 ms) and
//     nothing changes for the four players. Also with Triple Deluxe's order.
//   2 Triple Deluxe / Planet Robobot as the game really does it (GIOCO-TD-PR.md sez. 3-4; SRVR Initialize [4] left as rubbish in
//     the command buffer), and the manual-accept variant, which is only a hypothesis (AcceptClient, IsChild, GetMyStatus right
//     after Initialize; ids wider than a byte; a client never accepted is ejected at Start).
//   3 The game goes to UDS (OnGameUdsStarting) at every point of the lobby, for the creator and for a client, and for all three
//     consoles at once: the lobby closes in order and once, the console that leaves sees 1 (9 stays 9 with its passphrase), the
//     thread waiting inside StartFakeSession is released, the remote clients whose host went see the host go (state 0x80, the
//     games' own error: DESIGN sez. 4.5), or have the GO already (9); nobody is left hanging.
//   4 Loss / duplication / delay / reordering / partitions with fixed seeds over 200 complete lobbies of 4 consoles on three
//     worlds reused round after round (the clocks: ordinary, crossing 2^32 ms, past it with ticks beyond 2^40): every round ends,
//     in a complete lobby or a clean closure with the game's error, with the host's FIRST passphrase wherever there is one,
//     and the memory of the air does not grow. One mild fault at a time (a few messages lost, duplicated, reordered, held) must
//     ALWAYS complete. The same seeds give the same rounds.
//   5 Closures: a client leaves (StopSession, or falls off the air without a word) at four moments; the creator vanishes or
//     cancels at four moments; two creators of the same game on one air (a searcher sees both, takes one, the other is not
//     touched; both filled at once); a creator of another game; a freed seat reused.
//   6 A soak of 100000 valid and invalid commands with a fixed seed, with the invariants checked at every step (states, never
//     more than 4 players / 3 clients, canaries, exact-size and misaligned buffers, the lists of the design: some commands always
//     succeed, others never say "invalid state"), guided episodes that leave the lobbies in the deep states, and chaos on the air;
//     then every console is finalised and a complete lobby is played on the same consoles.
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_lobby_sim_test.cpp -o /tmp/dlp_lobby_sim_test
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "azl/dlp_ipc.hpp"
#include "dlp_fakenet.hpp"

using namespace azl;

// A report of the undefined-behaviour sanitizer stops the test (the default only prints it): an out-of-range index or an
// unaligned access is a failure, not a line nobody reads. (Ignored when the test is built without -fsanitize=undefined.)
extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}

static int g_checks = 0;
static int g_failures = 0;
static std::string g_ctx;  // what is being run: printed with every failure

static void FailLine(const char* file, int line, const char* what, const char* extra) {
    ++g_failures;
    if (g_failures <= 150) {
        std::printf("  FAIL %s:%d: %s%s [%s]\n", file, line, what, extra, g_ctx.c_str());
    } else if (g_failures == 151) {
        std::printf("  (further failures are counted, not printed)\n");
    }
}
#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(cond)) {                                                                               \
            FailLine(__FILE__, __LINE__, #cond, "");                                                 \
        }                                                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        const unsigned long long va_ = static_cast<unsigned long long>(a);                           \
        const unsigned long long vb_ = static_cast<unsigned long long>(b);                           \
        if (va_ != vb_) {                                                                            \
            char buf_[80];                                                                           \
            std::snprintf(buf_, sizeof(buf_), " (0x%llx vs 0x%llx)", va_, vb_);                      \
            FailLine(__FILE__, __LINE__, #a " == " #b, buf_);                                        \
        }                                                                                            \
    } while (0)
#define CHECK_STR(a, b)                                                                              \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        const std::string sa_ = (a);                                                                 \
        const std::string sb_ = (b);                                                                 \
        if (sa_ != sb_) {                                                                            \
            FailLine(__FILE__, __LINE__, #a " == " #b, (" (\"" + sa_ + "\" vs \"" + sb_ + "\")").c_str()); \
        }                                                                                            \
    } while (0)
#define CHECK_BYTES(actual, expected, n)                                                             \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (std::memcmp((actual), (expected), static_cast<std::size_t>(n)) != 0) {                   \
            FailLine(__FILE__, __LINE__, #actual " bytes == " #expected, "");                        \
        }                                                                                            \
    } while (0)

// Results and message types (written out, not the constants of the headers).
static constexpr u32 R_INVALID_STATE = 0xE0A0AC01;
static constexpr u32 R_BUSY = 0xE0A0AC03;
static constexpr u32 R_NO_DATA = 0xC880AFEF;
static constexpr u32 R_NOT_FOUND = 0xC880AC05;
static constexpr u32 R_TIMEOUT = 0xC920AFFE;
static constexpr u32 R_OUT_OF_RANGE = 0xD8E0AFFD;
static constexpr u32 R_BAD_POINTER = 0xD8E0AFF6;
static constexpr u32 R_BAD_HEADER = 0xD9001830;
static constexpr int T_START = 1;
static constexpr int T_START_ACK = 2;
static constexpr int T_GO = 3;
static constexpr int T_GO_ACK = 4;

static constexpr DlpKind kFk = DlpKind::Fkcl;
static constexpr DlpKind kSv = DlpKind::Srvr;

// Reply headers by command id (SPEC-DLP.md sez. 2 and 3, "Header risposta"; the two event descriptions are the design's choice).
static const u32 kFkReplyHeader[0x12] = {0,          0x00010040, 0x00020040, 0x000301C0, 0x00040080, 0x00050040,
                                         0x00060040, 0x00070042, 0x00080042, 0x00090042, 0x000A0040, 0x000B0040,
                                         0x000C0180, 0x000D0082, 0x000E02C0, 0x000F0100, 0x00100040, 0x00110040};
static const u32 kSvReplyHeader[0x11] = {0,          0x00010040, 0x00020040, 0x00030080, 0x00040200, 0x00050040,
                                         0x00060040, 0x00070040, 0x00080040, 0x00090040, 0x000A0040, 0x000B0082,
                                         0x000C02C0, 0x000D0180, 0x000E0080, 0x000F0040, 0x00100080};

// ---------------------------------------------------------------------------------------------------------------------
// A small deterministic generator (xorshift64*): every random choice of the test comes from one of these, with a fixed seed.

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {
        if (s == 0) {
            s = 1;
        }
        Next();
        Next();
    }
    u32 Next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<u32>((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    u32 Below(u32 n) { return n == 0 ? 0 : Next() % n; }
    bool Chance(u32 percent) { return Below(100) < percent; }
};

// ---------------------------------------------------------------------------------------------------------------------
// The memory of the game: regions of bytes at fake 32-bit addresses, with canaries around them (or of the exact size, so
// that a byte past the end is an AddressSanitizer report). The service gets addresses (u32); the resolver maps them.

class Mem {
public:
    static constexpr u8 kCanary = 0xA5;
    static constexpr u8 kFill = 0xCC;
    static constexpr u32 kPad = 32;

    u32 Alloc(u32 size, u32 misalign = 0, bool exact = false) {
        Region r;
        r.size = size;
        r.exact = exact;
        r.addr = next_ + misalign;
        if (exact) {
            r.heap.reset(new u8[size == 0 ? 1 : size]);
            std::memset(r.heap.get(), kFill, size);
            r.data = r.heap.get();
        } else {
            r.mem.assign(kPad + misalign + size + kPad, kCanary);
            r.data = r.mem.data() + kPad + misalign;
            std::memset(r.data, kFill, size);
        }
        next_ += (size + misalign + 0x40 + 15) & ~15u;
        regions_.push_back(std::move(r));
        return regions_.back().addr;
    }
    IpcResolver Resolver() {
        IpcResolver r;
        r.ctx = this;
        r.map = &Thunk;
        return r;
    }
    static u8* Thunk(void* ctx, u32 addr, u32 size) { return static_cast<Mem*>(ctx)->Find(addr, size); }
    /// The host pointer of a range that lies inside ONE region, or nullptr.
    u8* Find(u32 addr, u32 size = 1) {
        for (Region& r : regions_) {
            if (addr >= r.addr && static_cast<u64>(addr) + size <= static_cast<u64>(r.addr) + r.size) {
                return r.data + (addr - r.addr);
            }
        }
        return nullptr;
    }
    bool Intact() const {
        for (const Region& r : regions_) {
            if (r.exact) {
                continue;
            }
            for (const u8* p = r.mem.data(); p < r.data; ++p) {
                if (*p != kCanary) {
                    return false;
                }
            }
            for (const u8* p = r.data + r.size; p < r.mem.data() + r.mem.size(); ++p) {
                if (*p != kCanary) {
                    return false;
                }
            }
        }
        return true;
    }
    bool Untouched(u32 addr) {
        for (Region& r : regions_) {
            if (r.addr == addr) {
                for (u32 i = 0; i < r.size; ++i) {
                    if (r.data[i] != kFill) {
                        return false;
                    }
                }
                return true;
            }
        }
        return false;
    }
    void Refill(u32 addr) {
        for (Region& r : regions_) {
            if (r.addr == addr) {
                std::memset(r.data, kFill, r.size);
            }
        }
    }

private:
    struct Region {
        u32 addr = 0;
        u32 size = 0;
        bool exact = false;
        std::vector<u8> mem;
        std::unique_ptr<u8[]> heap;
        u8* data = nullptr;
    };
    std::vector<Region> regions_;
    u32 next_ = 0x08000000;
};

/// What a game thread holds for static buffer 0: the descriptor, the address, and where it lives in the memory of the game.
struct Sb {
    u32 w[2];  // [0] descriptor ((capacity << 14) | 2), [1] address: the pair the thread's TLS holds
    u32 addr;
    u32 cap;
};

struct Reply {
    u32 w[16];
    DlpOutcome out;
    u32 hdr() const { return w[0]; }
    u32 res() const { return w[1]; }
};

// ---------------------------------------------------------------------------------------------------------------------
// The games we imitate.

struct Style {
    const char* name;
    u32 tid_low;    // the low 32 bits of the game's title id
    u32 unique_id;  // tid_low >> 8, the unique id the game filters by
    bool td_like;   // Triple Deluxe / Planet Robobot call order (else Battle Royale's)
};
static const Style kTd = {"TD", 0x0010C000, 0x10C0, true};
static const Style kPr = {"PR", 0x00183600, 0x1836, true};
static const Style kBr = {"BR", 0x001C2100, 0x1C21, false};
static u64 FilterOf(const Style& s) { return 0x0004000100000000ull | s.tid_low; }  // child index 0, as TD scans

static const char* const kNames[5] = {"Host", "Cli1", "Cli2", "Cli3", "Cli4"};
static std::string NameOf(const Style& s, int idx, bool by_name) {
    if (s.td_like || (idx == 0 && !by_name)) {
        return kNames[idx];
    }
    return idx == 0 ? "BrHost" : std::string("BrCli") + static_cast<char>('0' + idx);
}

static std::string Ascii(const u16* name) {
    std::string s;
    for (int i = 0; i < 10 && name[i] != 0; ++i) {
        s += static_cast<char>(name[i]);
    }
    return s;
}
static std::string AsciiLE(const u8* p) {
    u16 n[10];
    for (int i = 0; i < 10; ++i) {
        n[i] = LoadLE16(p + 2 * i);
    }
    return Ascii(n);
}

static u32 MacW1(const Mac& m) {
    return static_cast<u32>(m[0]) | (static_cast<u32>(m[1]) << 8) | (static_cast<u32>(m[2]) << 16) | (static_cast<u32>(m[3]) << 24);
}
static u32 MacW2(const Mac& m) {
    return static_cast<u32>(m[4]) | (static_cast<u32>(m[5]) << 8);
}
static void NameWords(const u16* name, u32 w[5]) {
    for (int i = 0; i < 5; ++i) {
        w[i] = static_cast<u32>(name[2 * i]) | (static_cast<u32>(name[2 * i + 1]) << 16);
    }
}
static void PassWords(const u8* b, u32 w[3], u32 high = 0) {
    w[0] = static_cast<u32>(b[0]) | (static_cast<u32>(b[1]) << 8) | (static_cast<u32>(b[2]) << 16) | (static_cast<u32>(b[3]) << 24);
    w[1] = static_cast<u32>(b[4]) | (static_cast<u32>(b[5]) << 8) | (static_cast<u32>(b[6]) << 16) | (static_cast<u32>(b[7]) << 24);
    w[2] = static_cast<u32>(b[8]) | (high << 8);
}

static constexpr u32 kCmdWords = 64;
static constexpr u32 kGuardWords = 8;
static constexpr u32 kGuard = 0xFEEDFACE;

// ---------------------------------------------------------------------------------------------------------------------
// One console: the lobby, the service, and the memory of its game with the buffers the games use.

struct Con {
    FakeLobbyNet net;
    DlpLobby lobby;
    DlpService svc;
    Mem mem;
    int idx;
    int variant = 0;  // 0 canaries, 1 exact-size regions (ASan), 2 canaries with misaligned addresses (UBSan)
    u32 a_title[3] = {};
    u32 a_server[3] = {};
    u32 a_nodes[3] = {};
    u32 a_clients[3] = {};

    Con(FakeAir& air, int i, const LobbyConfig& cfg) : net(air, i), lobby(&net, cfg), idx(i) {
        svc.SetLobby(&lobby);
        for (int v = 0; v < 3; ++v) {
            const bool ex = v == 1;
            a_title[v] = mem.Alloc(0x13A8, v == 2 ? 1 : 0, ex);
            a_server[v] = mem.Alloc(0x298, v == 2 ? 3 : 0, ex);
            a_nodes[v] = mem.Alloc(8, v == 2 ? 1 : 0, ex);
            a_clients[v] = mem.Alloc(8, v == 2 ? 2 : 0, ex);
        }
    }

    static Sb MakeSb(u32 addr, u32 cap) {
        Sb s;
        s.addr = addr;
        s.cap = cap;
        s.w[0] = (cap << 14) | 2;
        s.w[1] = addr;
        return s;
    }
    Sb TitleSb() const { return MakeSb(a_title[variant], 0x13A8); }
    Sb ServerSb() const { return MakeSb(a_server[variant], 0x298); }
    Sb NodesSb() const { return MakeSb(a_nodes[variant], 8); }
    u32 ClientsAddr() const { return a_clients[variant]; }
    const u8* Data(const Sb& s) { return mem.Find(s.addr, 1); }

    /// One request through Dispatch. The request words after the arguments are garbage (a service that read one it should not
    /// would show), the 8 words after the 64 are guards. The reply is copied out; its shape and the canaries are checked.
    Reply Call(DlpKind kind, u32 header, std::initializer_list<u32> args, const u32* sbuf = nullptr,
               const IpcResolver* resolver = nullptr) {
        return CallN(kind, header, args.begin(), static_cast<u32>(args.size()), sbuf, resolver);
    }
    Reply CallN(DlpKind kind, u32 header, const u32* args, u32 n, const u32* sbuf, const IpcResolver* resolver) {
        u32 buf[kCmdWords + kGuardWords];
        u32 before[kCmdWords];
        for (u32 i = 0; i < kCmdWords; ++i) {
            buf[i] = 0xC0DE0000u + i;
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            buf[kCmdWords + i] = kGuard;
        }
        buf[0] = header;
        for (u32 i = 0; i < n && 1 + i < kCmdWords; ++i) {
            buf[1 + i] = args[i];
        }
        std::memcpy(before, buf, sizeof(before));
        const IpcResolver r = resolver != nullptr ? *resolver : mem.Resolver();
        Reply rp;
        std::memset(&rp, 0, sizeof(rp));
        rp.out = svc.Dispatch(kind, buf, sbuf, r);
        std::memcpy(rp.w, buf, sizeof(rp.w));
        bool shape = true;
        u32 extent = 0;  // words of the command buffer the reply may have written
        if (rp.out == DlpOutcome::Done) {
            extent = 1 + ((buf[0] >> 6) & 0x3F) + (buf[0] & 0x3F);
            if (extent > kCmdWords) {
                extent = kCmdWords;
            }
        }
        for (u32 i = extent; i < kCmdWords; ++i) {
            if (buf[i] != before[i]) {
                shape = false;
            }
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            if (buf[kCmdWords + i] != kGuard) {
                shape = false;
            }
        }
        CHECK(shape);
        CHECK(mem.Intact());
        return rp;
    }
    /// PollPending with a garbage command buffer. *done = the join ended and the reply is in the buffer.
    Reply Pending(bool* done) {
        u32 buf[kCmdWords + kGuardWords];
        for (u32 i = 0; i < kCmdWords; ++i) {
            buf[i] = 0xC0DE0000u + i;
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            buf[kCmdWords + i] = kGuard;
        }
        Reply rp;
        std::memset(&rp, 0, sizeof(rp));
        *done = svc.PollPending(buf);
        std::memcpy(rp.w, buf, sizeof(rp.w));
        rp.out = DlpOutcome::Done;
        bool shape = true;
        for (u32 i = 2; i < kCmdWords; ++i) {  // a reply is [header, result]: nothing else may change
            if (buf[i] != 0xC0DE0000u + i) {
                shape = false;
            }
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            if (buf[kCmdWords + i] != kGuard) {
                shape = false;
            }
        }
        if (!*done && buf[0] != 0xC0DE0000u) {  // a join still running leaves the buffer alone
            shape = false;
        }
        CHECK(shape);
        return rp;
    }

    // ---- dlp:FKCL, as the games send it (request headers of SPEC-DLP.md sez. 2) --------------------------------------
    /// TD / PR: FKCL Initialize, name NULL (the glue passes none): [1] shared memory size, [2] beacons (u8), [3] the translate
    /// descriptor of two handles, [4][5] the handles.
    Reply FkInit(u32 beacons = 16) { return Call(kFk, 0x00010083, {0x30000, beacons, 0x04000000, 0x1111, 0x2222}); }
    /// BR: InitializeWithName: [3..7] the name, [8] one word more, [9] the translate descriptor, [10][11] the handles.
    Reply FkInitName(const u16* name, u32 beacons = 16) {
        u32 n[5];
        NameWords(name, n);
        return Call(kFk, 0x00110203, {0x30000, beacons, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x3333, 0x4444});
    }
    Reply FkFinalize() { return Call(kFk, 0x00020000, {}); }
    Reply FkEventDesc() { return Call(kFk, 0x00030000, {}); }
    Reply FkChannels() { return Call(kFk, 0x00040000, {}); }
    /// StartScan: [1] the channel mask (the value GetChannels gave), [2][3] the title id filter, [4][5] the MAC filter (zeros), [6] 0.
    Reply FkScan(u64 filter, u32 mask = 0x0421) {
        return Call(kFk, 0x00050180, {mask, static_cast<u32>(filter), static_cast<u32>(filter >> 32), 0, 0, 0});
    }
    Reply FkStopScan() { return Call(kFk, 0x00060000, {}); }
    Reply FkServerInfo(const Mac& m, const u32* sbuf) { return Call(kFk, 0x00070080, {MacW1(m), MacW2(m)}, sbuf); }
    Reply FkTitleInfo(const Mac& m, const u32* sbuf) {
        return Call(kFk, 0x00080100, {MacW1(m), MacW2(m), 0x0010C000, 0x00040001}, sbuf);
    }
    Reply FkInOrder(u32 restart, const u32* sbuf) { return Call(kFk, 0x00090040, {restart}, sbuf); }
    Reply FkDelete(const Mac& m) { return Call(kFk, 0x000A0080, {MacW1(m), MacW2(m)}); }
    Reply FkJoin(const Mac& m, u32 tid_lo, u32 tid_hi) { return Call(kFk, 0x000B0100, {MacW1(m), MacW2(m), tid_lo, tid_hi}); }
    Reply FkStatus() { return Call(kFk, 0x000C0000, {}); }
    Reply FkNodes(u32 n, const u32* sbuf) { return Call(kFk, 0x000D0040, {n}, sbuf); }
    Reply FkNodeInfo(u32 node) { return Call(kFk, 0x000E0040, {node}); }
    Reply FkPass() { return Call(kFk, 0x000F0000, {}); }
    Reply FkStopSession() { return Call(kFk, 0x00100000, {}); }

    // ---- dlp:SRVR (SPEC-DLP.md sez. 3) -------------------------------------------------------------------------------
    /// [1] shared memory size, [2] max clients (u8), [3] process id, [4] child index (u8; TD's wrapper does not write it),
    /// [5][6] block size and count, [7] the translate descriptor, [8][9] the handles.
    Reply SvInit(u32 max_clients, u32 child_word = 0, u32 pid = 0xE) {
        return Call(kSv, 0x00010183, {0x85000, max_clients, pid, child_word, 0x40000, 2, 0x04000000, 0x5555, 0x6666});
    }
    Reply SvInitName(u32 max_clients, u32 child_word, const u16* name) {
        u32 n[5];
        NameWords(name, n);
        return Call(kSv, 0x000F0303, {0x85000, max_clients, 0xE, child_word, 0x40000, 2, n[0], n[1], n[2], n[3], n[4], 0,
                                      0x04000000, 0x7777, 0x8888});
    }
    Reply SvFinalize() { return Call(kSv, 0x00020000, {}); }
    Reply SvState() { return Call(kSv, 0x00030000, {}); }
    Reply SvEventDesc() { return Call(kSv, 0x00040000, {}); }
    Reply SvAccepting(u32 manual, u32 channel) { return Call(kSv, 0x00050080, {manual, channel}); }
    Reply SvEnd() { return Call(kSv, 0x00060000, {}); }
    Reply SvDistribute() { return Call(kSv, 0x00070000, {}); }
    Reply SvPass(const u8* nine, u32 high = 0) {
        u32 w[3];
        PassWords(nine, w, high);
        return Call(kSv, 0x000800C0, {w[0], w[1], w[2]});
    }
    Reply SvAccept(u32 node) { return Call(kSv, 0x00090040, {node}); }
    Reply SvDisconnect(u32 node) { return Call(kSv, 0x000A0040, {node}); }
    /// GetConnectingClients: [1] the number of elements, [2] the mapped-write descriptor, [3] the address.
    Reply SvClients(u32 n, u32 bytes, u32 addr) { return Call(kSv, 0x000B0042, {n, (bytes << 4) | 0xC, addr}); }
    Reply SvClientInfo(u32 node) { return Call(kSv, 0x000C0040, {node}); }
    Reply SvClientState(u32 node) { return Call(kSv, 0x000D0040, {node}); }
    Reply SvIsChild(u32 pid) { return Call(kSv, 0x000E0040, {pid}); }
    Reply SvDup() { return Call(kSv, 0x00100000, {}); }

    // ---- what a game does with the answers ---------------------------------------------------------------------------
    /// GetConnectingClients(n) into `bytes` bytes of the console's 8-byte region; also checks that nothing was written but
    /// what the reply says (and nothing at all on an error).
    std::vector<u16> Clients(u32 n, u32 bytes, u32* total, u32* result = nullptr) {
        const u32 addr = ClientsAddr();
        mem.Refill(addr);
        const Reply r = SvClients(n, bytes, addr);
        std::vector<u16> ids;
        if (result != nullptr) {
            *result = r.res();
        }
        if (r.res() != 0) {
            CHECK(mem.Untouched(addr));
            if (total != nullptr) {
                *total = 0;
            }
            return ids;
        }
        CHECK_EQ(r.hdr(), 0x000B0082u);
        CHECK_EQ(r.w[3], (bytes << 4) | 0xC);  // the request's descriptor and address come back
        CHECK_EQ(r.w[4], addr);
        if (total != nullptr) {
            *total = r.w[2];
        }
        u32 k = r.w[2] < n ? r.w[2] : n;
        if (k > bytes / 2) {
            k = bytes / 2;
        }
        const u8* p = mem.Find(addr, 1);
        for (u32 i = 0; i < k; ++i) {
            ids.push_back(LoadLE16(p + 2 * i));
        }
        for (u32 i = 2 * k; i < 8; ++i) {
            CHECK_EQ(p[i], Mem::kFill);
        }
        return ids;
    }
    /// GetConnectingNodes(max) into the console's 8-byte static buffer, same checks.
    std::vector<u16> Nodes(u32 max, u32* total, u32* result = nullptr) {
        const Sb sb = NodesSb();
        mem.Refill(sb.addr);
        const Reply r = FkNodes(max, sb.w);
        std::vector<u16> ids;
        if (result != nullptr) {
            *result = r.res();
        }
        if (r.res() != 0) {
            CHECK(mem.Untouched(sb.addr));
            if (total != nullptr) {
                *total = 0;
            }
            return ids;
        }
        CHECK_EQ(r.hdr(), 0x000D0082u);
        if (total != nullptr) {
            *total = r.w[2];
        }
        u32 k = r.w[2] < max ? r.w[2] : max;
        if (k > 4) {
            k = 4;
        }
        CHECK_EQ(r.w[3], ((2 * k) << 14) | 2);
        CHECK_EQ(r.w[4], sb.addr);
        const u8* p = Data(sb);
        for (u32 i = 0; i < k; ++i) {
            ids.push_back(LoadLE16(p + 2 * i));
        }
        for (u32 i = 2 * k; i < 8; ++i) {
            CHECK_EQ(p[i], Mem::kFill);
        }
        return ids;
    }
};

static LobbyConfig MakeCfg(u32 tid_low, u64 seed, const char* name) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = 0x0004000000000000ull | tid_low;
    c.friend_seed = seed;
    FakeAir::Name(c.name, name);
    return c;
}

static u32 FrameMs(u32 n) {  // 60 frames per second: 16, 17, 17, 16, 17, 17 ...
    return (n * 50) / 3 - ((n - 1) * 50) / 3;
}

/// The consoles of a room on one air, the clock, and the network thread of the plugin (Pump every net_period ms).
struct World {
    FakeAir air;
    std::vector<std::unique_ptr<Con>> c;
    u32 frames = 0;
    bool net_thread = true;
    u32 net_period = 50;
    u64 net_last = 0;

    World(int n, const Style& st, u64 start = 100000) : air(start) {
        for (int i = 0; i < n; ++i) {
            c.push_back(std::make_unique<Con>(air, i, MakeCfg(st.tid_low, 0x1000u + static_cast<u32>(i), kNames[i])));
        }
    }
    /// A console of another game (its lobbies are not ours).
    void AddConsole(const Style& st) {
        const int i = static_cast<int>(c.size());
        c.push_back(std::make_unique<Con>(air, i, MakeCfg(st.tid_low, 0x1000u + static_cast<u32>(i), kNames[i])));
    }
    Con& operator[](int i) { return *c[static_cast<std::size_t>(i)]; }
    void NetThread() {
        if (net_thread && air.now() >= net_last + net_period) {
            net_last = air.now();
            for (auto& x : c) {
                x->lobby.Pump();
            }
        }
    }
    void Tick(u32 ms) {
        air.Advance(ms);
        NetThread();
    }
    void NextFrame() {
        ++frames;
        Tick(FrameMs(frames));
    }
    /// No game runs; only the clock and the network thread (and nobody issues a command).
    void Idle(u32 ms) {
        for (u32 t = 0; t < ms; t += 16) {
            Tick(16);
        }
    }
};

/// The state of the game's aging arithmetic (Triple Deluxe): ticks x 3.7296 ns against 5000 ms.
static bool GameDropsEntry(u64 now_ticks, u64 tick) {
    const u64 diff = now_ticks >= tick ? now_ticks - tick : 0;
    return diff * 37296 / 10000 >= 5000ull * 1000000ull;
}

// ---------------------------------------------------------------------------------------------------------------------
// The searcher game (dlp:FKCL).

class Guest {
public:
    enum class Phase { Idle, Scanning, Joining, InSession, Done, Error, Left };
    struct Seen {
        bool used = false;
        Mac mac = {};
        u32 unique_id = 0;
        u32 variation = 0;
        u32 max = 0;
        u32 joined = 0;
        std::string host;
        u64 tick = 0;
        u32 fresh = 0;  // the frame this entry was last read
    };

    Con& c;
    const Style& st;
    int idx;
    // knobs of the test
    u32 start_frame = 0;
    u32 choose_delay = 3;       // frames between seeing a group and choosing it
    bool allow_full = false;    // chooses a group the game would hide (a list that went stale)
    bool observer = false;      // never chooses; not waited for
    bool deep = false;          // byte-for-byte checks of the records it reads
    bool hold_final = false;    // at state 9 do not Finalize (test control)
    bool status_after_init = false;
    bool no_filter = false;     // StartScan with no title filter (0): every lobby of the air is a candidate for the table
    bool has_prefer = false;    // chooses only the group of this host
    Mac prefer = {};
    u32 expect_child = 0;       // deep: the child index the creator announces (the low byte of TitleInfo.variation)
    // results
    Phase phase = Phase::Idle;
    int error_type = -1;        // the game's error type (2 join failed, 6 session lost)
    u32 join_result = 0xFFFFFFFFu;
    u32 last_state = 0;         // the last GetMyStatus byte
    u32 my_node = 0;
    u8 pass[9] = {};
    bool has_pass = false;
    std::vector<u16> node_ids;      // the last GetConnectingNodes
    std::vector<std::string> names; // ... and their names
    u32 chosen_joined = 0;          // the clients+host counter the entry showed when chosen
    u32 aged_out = 0;               // entries the game dropped by its own aging
    u32 frames_joining = 0;
    Mac chosen = {};
    Seen table[16];
    u32 frame_ = 0;

    Guest(Con& con, const Style& s, int index) : c(con), st(s), idx(index) {}

    void Frame(u32 frame) {
        frame_ = frame;
        switch (phase) {
        case Phase::Idle:
            if (frame >= start_frame) {
                Start();
            }
            break;
        case Phase::Scanning:
            ScanFrame();
            TryChoose();
            break;
        case Phase::Joining:
            PollJoin();
            break;
        case Phase::InSession:
            SessionFrame();
            break;
        default:
            break;
        }
    }

    void Start() {
        if (st.td_like) {
            const Reply r = c.FkInit(16);
            CHECK_EQ(r.hdr(), 0x00010040u);
            CHECK_EQ(r.res(), 0u);
        } else {
            u16 nm[10];
            FakeAir::Name(nm, NameOf(st, idx, true).c_str());
            const Reply r = c.FkInitName(nm, 16);
            CHECK_EQ(r.hdr(), 0x00110040u);
            CHECK_EQ(r.res(), 0u);
        }
        if (status_after_init) {
            CHECK_EQ(c.FkStatus().w[2] >> 24, 1u);
        }
        const Reply ch = c.FkChannels();  // a negative answer is fatal for TD
        CHECK_EQ(ch.hdr(), 0x00040080u);
        CHECK_EQ(ch.res(), 0u);
        CHECK_EQ(ch.w[2], 0x0421u);
        const Reply sc = c.FkScan(no_filter ? 0 : FilterOf(st), ch.w[2]);
        CHECK_EQ(sc.hdr(), 0x00050040u);
        CHECK_EQ(sc.res(), 0u);
        phase = Phase::Scanning;
    }

    /// The scan frame of the game: GetTitleInfoInOrder(1) then (0) until EXACTLY "no data" (any other answer is "there is a
    /// title"); for each title GetServerInfo of its MAC (an error discards it); the list of 16 entries keyed by MAC, aged by
    /// the game's own arithmetic on the tick of the server record.
    void ScanFrame() {
        const Sb t = c.TitleSb();
        const Sb s = c.ServerSb();
        c.mem.Refill(t.addr);
        Reply r = c.FkInOrder(1, t.w);
        int guard = 0;
        for (; guard < 24 && r.res() != R_NO_DATA; ++guard) {
            CHECK_EQ(r.res(), 0u);  // anything but "no data" is "there is a title" to the game: the lobby never says it
            if (r.res() != 0) {
                break;
            }
            CHECK_EQ(r.hdr(), 0x00090042u);
            CHECK_EQ(r.w[2], 0x04EA0002u);  // static buffer 0, 0x13A8 bytes
            CHECK_EQ(r.w[3], t.addr);
            const u8* tp = c.Data(t);
            Seen e;
            e.unique_id = LoadLE32(tp + 0);
            e.variation = LoadLE32(tp + 4);
            std::memcpy(e.mac.data(), tp + 8, 6);
            c.mem.Refill(s.addr);
            const Reply si = c.FkServerInfo(e.mac, s.w);
            if (si.res() == 0) {
                CHECK_EQ(si.hdr(), 0x00070042u);
                CHECK_EQ(si.w[2], 0x00A60002u);  // static buffer 0, 0x298 bytes
                CHECK_EQ(si.w[3], s.addr);
                const u8* q = c.Data(s);
                e.max = q[8];
                e.joined = q[9];
                e.host = AsciiLE(q + 0x1C);
                e.tick = LoadLE64(q + 0x290);
                if (deep) {
                    DeepCheck(tp, q, e);
                }
                Upsert(e);
            }
            c.mem.Refill(t.addr);
            r = c.FkInOrder(0, t.w);
        }
        CHECK(r.res() == R_NO_DATA);  // the loop ended on "no data", not on the guard
        Age();
    }

    /// Byte for byte, with the values written by hand (SPEC-DLP.md sez. 5): everything the games never read stays zero.
    void DeepCheck(const u8* tp, const u8* q, const Seen& e) {
        CHECK_EQ(e.unique_id, st.unique_id);
        CHECK_EQ(e.variation >> 8, 0u);
        CHECK_EQ(e.variation & 0xFF, expect_child & 0xFF);
        for (u32 i = 0x0E; i < 0x13A8; ++i) {
            CHECK_EQ(tp[i], 0u);
        }
        CHECK_BYTES(q, e.mac.data(), 6);
        CHECK_EQ(q[6], 0u);
        CHECK(q[7] <= 3);
        CHECK_EQ(q[8], 4u);  // max nodes: host + 3 clients
        CHECK(q[9] >= 1 && q[9] <= q[8]);
        CHECK_EQ(q[0x0A] | q[0x0B] | q[0x0C] | q[0x0D] | q[0x0E] | q[0x0F], 0u);
        for (u32 i = 0x10; i < 0x1C; ++i) {
            CHECK_EQ(q[i], 0u);  // node_info[0]: seed and padding
        }
        for (u32 i = 0x1C + 20; i < 0x290; ++i) {
            CHECK_EQ(q[i], 0u);  // the rest of node_info[0] and the 15 other nodes
        }
        // the tick: the last time this entry was heard; fresh in a live scan (never older than the lobby's 1500 ms life)
        const u64 now = c.net.air().ticks();
        CHECK(e.tick <= now);
        CHECK(now - e.tick <= 1600ull * 268112ull);
    }

    void Upsert(const Seen& e) {
        Seen* slot = nullptr;
        for (Seen& x : table) {
            if (x.used && x.mac == e.mac) {
                slot = &x;
                break;
            }
        }
        for (Seen& x : table) {
            if (slot == nullptr && !x.used) {
                slot = &x;
            }
        }
        if (slot != nullptr) {
            *slot = e;
            slot->used = true;
            slot->fresh = frame_;
        }
    }
    void Age() {
        const u64 now = c.net.air().ticks();
        for (Seen& x : table) {
            if (x.used && GameDropsEntry(now, x.tick)) {
                x.used = false;
                ++aged_out;
                const Reply d = c.FkDelete(x.mac);  // the result is ignored by the game
                CHECK(d.res() == 0 || d.res() == R_NO_DATA);
            }
        }
    }
    /// The first group read THIS frame that the game would show (it hides a group with +9 == +8).
    const Seen* Pick() const {
        for (const Seen& x : table) {
            if (x.used && x.fresh == frame_ && x.unique_id == st.unique_id && (allow_full || x.joined < x.max) &&
                (!has_prefer || x.mac == prefer)) {
                return &x;
            }
        }
        return nullptr;
    }
    void TryChoose() {
        const Seen* p = observer ? nullptr : Pick();
        if (p == nullptr) {
            seen_since_ = 0;
            return;
        }
        if (seen_since_ == 0) {
            seen_since_ = frame_;
        }
        if (frame_ - seen_since_ >= choose_delay) {
            Choose(*p);
        }
    }
    void Choose(const Seen& e) {
        const Reply sp = c.FkStopScan();
        CHECK_EQ(sp.hdr(), 0x00060040u);
        CHECK_EQ(sp.res(), 0u);
        chosen = e.mac;
        chosen_joined = e.joined;
        // the title id the game builds from the entry it chose (GIOCO-TD-PR.md sez. 2): 0x00040001 : ((unique_id & 0xFF0FFFFF) << 8) | child index
        const u32 lo = ((e.unique_id & 0xFF0FFFFFu) << 8) | (e.variation & 0xFF);
        const Reply j = c.FkJoin(e.mac, lo, 0x00040001);
        if (j.out == DlpOutcome::JoinPending) {
            phase = Phase::Joining;
            frames_joining = 0;
        } else {
            join_result = j.res();
            error_type = 2;  // any error of StartFakeSession is error type 2 for the game
            phase = Phase::Error;
        }
    }
    void PollJoin() {
        bool done = false;
        const Reply r = c.Pending(&done);
        if (!done) {
            CHECK(++frames_joining < 600);  // bounded by the lobby's 3 s (the loop of the plugin sleeps 10 ms: <= 300)
            return;
        }
        join_result = r.res();
        CHECK_EQ(r.hdr(), 0x000B0040u);  // one normal word: [header, result]
        if (r.res() != 0) {
            error_type = 2;
            phase = Phase::Error;
            return;
        }
        const Reply s = c.FkStatus();  // right after the join the game must see > 0 and != 0x80
        const u32 stt = s.w[2] >> 24;
        CHECK_EQ(stt, 5u);
        phase = Phase::InSession;
        SessionFrame();
    }
    void SessionFrame() {
        const Reply s = c.FkStatus();
        CHECK_EQ(s.hdr(), 0x000C0180u);
        CHECK_EQ(s.res(), 0u);
        CHECK_EQ(s.w[3] | s.w[4] | s.w[5] | s.w[6], 0u);
        const u32 w = s.w[2];
        const u32 stt = w >> 24;
        last_state = stt;
        CHECK(stt == 0 || stt == 1 || stt == 2 || stt == 5 || stt == 6 || stt == 9 || stt == 0x80);  // never 3, never 7
        if (stt == 0 || stt == 0x80) {  // the game's error type 6
            error_type = 6;
            phase = Phase::Error;
            return;
        }
        if (stt == 5 || stt == 6 || stt == 9) {
            CHECK_EQ((w >> 16) & 0xFF, 1u);
            my_node = w & 0xFFFF;
            CHECK(my_node >= 2 && my_node <= 4);
            ReadRoster();
        }
        if (stt == 9) {
            const Reply p = c.FkPass();  // (TD calls it; BR does not link it: here it is the test's way to see the passphrase)
            CHECK_EQ(p.hdr(), 0x000F0100u);
            CHECK_EQ(p.res(), 0u);
            std::memcpy(pass, p.w + 2, 9);
            has_pass = true;
            const u8* pb = reinterpret_cast<const u8*>(p.w + 2);
            CHECK_EQ(pb[9] | pb[10] | pb[11], 0u);
            if (hold_final) {
                return;
            }
            if (!st.td_like) {
                CHECK_EQ(c.FkStopSession().res(), 0u);
            }
            CHECK_EQ(c.FkFinalize().res(), 0u);  // fatal for the game if it were negative
            phase = Phase::Done;
        }
    }
    void ReadRoster() {
        u32 total = 99;
        u32 res = 99;
        node_ids = c.Nodes(4, &total, &res);
        CHECK_EQ(res, 0u);
        CHECK(total <= 4);
        names.clear();
        std::set<u16> seen;
        for (const u16 id : node_ids) {
            CHECK(id >= 1 && id <= 4);
            CHECK(seen.insert(id).second);
            const Reply ni = c.FkNodeInfo(id);
            CHECK_EQ(ni.hdr(), 0x000E02C0u);
            CHECK_EQ(ni.res(), 0u);
            CHECK_EQ(ni.w[2 + 9], id);  // the node id at +0x24
            const u8* rec = reinterpret_cast<const u8*>(ni.w + 2);
            names.push_back(AsciiLE(rec + 0x0C));
            if (deep) {
                const std::string& nm = names.back();
                const u64 want = 0x1000u + (nm.empty() || nm.back() < '1' || nm.back() > '4' ? 0u : static_cast<u32>(nm.back() - '0'));
                CHECK_EQ(LoadLE64(rec), want);  // the seed of the console, at +0
                CHECK_EQ(LoadLE32(rec + 8), 0u);
                CHECK_EQ(LoadLE32(rec + 0x20), 0u);
            }
        }
    }

private:
    u32 seen_since_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// The creator game (dlp:SRVR).

class Host {
public:
    enum class Phase { Idle, Lobby, Started, Done, Error, Left };
    struct Member {
        u16 node;
        std::string name;
    };

    Con& c;
    const Style& st;
    bool manual;
    // knobs
    bool by_name = true;            // BR: InitializeWithName (else Initialize)
    u32 start_frame = 0;
    u32 child_word = 0;             // SRVR Initialize [4] (TD's wrapper does not write it: whatever was in the buffer)
    bool hold_pass = false;         // the game's UI has not sent the passphrase yet (test control)
    bool hold_final = false;        // at state 9 do not Finalize (test control)
    u32 accept_limit = 99;          // manual: accept at most this many
    // results
    Phase phase = Phase::Idle;
    int error_type = -1;
    u32 last_state = 0;
    std::vector<Member> roster;
    u32 listed = 0;
    u32 accepted = 0;
    bool sent_first = false;
    u8 first_pass[9] = {};
    u32 refused_repeats = 0;
    u32 max_listed = 0;

    Host(Con& con, const Style& s, bool man) : c(con), st(s), manual(man), by_name(!s.td_like), rng_(0xC0FFEEull + static_cast<u64>(con.idx)) {}

    std::string name() const { return NameOf(st, 0, by_name); }

    void Start() {
        if (st.td_like) {
            const Reply r = c.SvInit(3, child_word);  // max clients 3
            CHECK_EQ(r.hdr(), 0x00010040u);
            CHECK_EQ(r.res(), 0u);
            const Reply d = c.SvDup();  // right after Initialize: a negative answer would fail the game's whole Initialize
            CHECK_EQ(d.hdr(), 0x00100080u);
            CHECK_EQ(d.res(), 0u);
            CHECK_EQ(d.w[2], 0u);
            if (manual) {  // (the hypothesis: not what TD calls)
                const Reply ic = c.SvIsChild(0xE);
                CHECK_EQ(ic.hdr(), 0x000E0080u);
                CHECK_EQ(ic.res(), 0u);
                CHECK_EQ(ic.w[2], 0u);
            }
        } else {
            Reply r;
            if (by_name) {
                u16 nm[10];
                FakeAir::Name(nm, NameOf(st, 0, true).c_str());
                r = c.SvInitName(4, child_word, nm);  // BR asks for 4 clients: the room has 3
                CHECK_EQ(r.hdr(), 0x000F0040u);
            } else {
                r = c.SvInit(4, child_word);
                CHECK_EQ(r.hdr(), 0x00010040u);
            }
            CHECK_EQ(r.res(), 0u);
        }
        const Reply a = c.SvAccepting(manual ? 1 : 0, 6);
        CHECK_EQ(a.hdr(), 0x00050040u);
        if (a.res() != 0) {
            error_type = 3;
            phase = Phase::Error;
            return;
        }
        phase = Phase::Lobby;
    }

    void Frame(u32 frame) {
        if (phase == Phase::Idle) {
            if (frame >= start_frame) {
                Start();
            }
            return;
        }
        if (phase != Phase::Lobby && phase != Phase::Started) {
            return;
        }
        const Reply s = c.SvState();
        CHECK_EQ(s.hdr(), 0x00030080u);
        CHECK_EQ(s.res(), 0u);
        const u32 stt = s.w[2] & 0xFF;
        last_state = stt;
        CHECK(stt == 0 || stt == 1 || stt == 2 || stt == 6 || stt == 8 || stt == 9);
        if (stt == 0 || stt == 10) {  // the game's error type 4
            error_type = 4;
            phase = Phase::Error;
            return;
        }
        if (stt == 1) {  // our network was lost: the lobby is back to "initialised"; the player gives up
            error_type = 99;
            CHECK_EQ(c.SvFinalize().res(), 0u);
            phase = Phase::Error;
            return;
        }
        if (stt == 2) {
            ReadClients();
            if (manual) {
                AcceptPending();
            }
        }
        if (stt == 8 && !hold_pass && (!sent_first || st.td_like)) {  // TD every frame, BR once
            u8 p[9];
            std::snprintf(reinterpret_cast<char*>(p), sizeof(p), "%08x", rng_.Next() & 0x7FFFFFFFu);
            const Reply pr = c.SvPass(p);
            CHECK_EQ(pr.hdr(), 0x00080040u);
            if (!sent_first) {
                CHECK_EQ(pr.res(), 0u);
                std::memcpy(first_pass, p, 9);
                sent_first = true;
            } else {
                CHECK_EQ(pr.res(), R_INVALID_STATE);  // the first one stays; the others are refused, nothing changes
                ++refused_repeats;
            }
        }
        if (stt == 9 && !hold_final) {
            u32 total = 99;
            c.Clients(4, 8, &total);
            if (total == 0) {  // "state 9 AND no connecting clients", and only then Finalize
                CHECK_EQ(c.SvFinalize().res(), 0u);
                phase = Phase::Done;
            }
        }
    }
    void ReadClients() {
        u32 total = 99;
        u32 res = 99;
        const std::vector<u16> ids = c.Clients(4, 8, &total, &res);
        CHECK_EQ(res, 0u);
        CHECK(total <= 3);  // never more than 3 clients
        CHECK_EQ(ids.size(), static_cast<std::size_t>(total));
        roster.clear();
        std::set<u16> seen;
        for (const u16 id : ids) {
            CHECK(id >= 2 && id <= 4);
            CHECK(seen.insert(id).second);
            const Reply ci = c.SvClientInfo(id);
            if (ci.res() != 0) {  // an error: the game skips that client
                continue;
            }
            CHECK_EQ(ci.hdr(), 0x000C02C0u);
            CHECK_EQ(ci.w[2 + 9], id);
            roster.push_back({id, AsciiLE(reinterpret_cast<const u8*>(ci.w + 2) + 0x0C)});
        }
        listed = static_cast<u32>(ids.size());
        max_listed = std::max(max_listed, listed);
    }
    void AcceptPending() {
        for (const Member& m : roster) {
            const Reply cs = c.SvClientState(m.node);
            CHECK_EQ(cs.hdr(), 0x000D0180u);
            CHECK_EQ(cs.res(), 0u);
            const u32 ext = cs.w[2] >> 24;
            CHECK_EQ(cs.w[2] & 0xFFFF, m.node);
            if (ext == 0x40) {
                CHECK_EQ((cs.w[2] >> 16) & 0xFF, 0u);  // waiting: not joined yet
                if (accepted < accept_limit) {
                    const Reply ar = c.SvAccept(m.node);
                    CHECK_EQ(ar.res(), 0u);
                    ++accepted;
                }
            } else {
                CHECK_EQ(ext, 5u);
                CHECK_EQ((cs.w[2] >> 16) & 0xFF, 1u);
            }
        }
    }
    void PressStart() {
        const Reply r = c.SvDistribute();
        CHECK_EQ(r.hdr(), 0x00070040u);
        if (r.res() != 0) {
            error_type = 5;
            phase = Phase::Error;
            return;
        }
        phase = Phase::Started;
    }

private:
    Rng rng_;
};

// ---------------------------------------------------------------------------------------------------------------------
// A room: one host and its guests on one world, the frame loop, and the checks that hold at every frame.

class Room {
public:
    World& w;
    const Style& st;
    Host host;
    std::vector<std::unique_ptr<Guest>> guests;
    u32 frame = 0;
    u32 expect = 0;                 // the creator presses Start when it lists this many for `start_after` frames
    u32 start_after = 20;
    u32 press_at = 0;               // non-zero: press Start at this frame whatever the list says (0xFFFFFFFF = never)
    std::function<void(Room&)> hook;  // runs at the start of every frame, before the games
    u32 stable = 0;
    bool pressed = false;
    u32 press_frame = 0;

    /// `map` (optional): which console of the world plays the host (map[0]) and each guest (map[i]); else 0, 1, 2 ...
    Room(World& world, const Style& s, int nclients, bool manual = false, const std::vector<int>* map = nullptr)
        : w(world), st(s), host(*world.c[static_cast<std::size_t>(map != nullptr ? (*map)[0] : 0)], s, manual) {
        expect = static_cast<u32>(nclients);
        for (int i = 1; i <= nclients; ++i) {
            const int at = map != nullptr ? (*map)[static_cast<std::size_t>(i)] : i;
            guests.push_back(std::make_unique<Guest>(*world.c[static_cast<std::size_t>(at)], s, i));
        }
    }
    Con& con(int i) { return *w.c[static_cast<std::size_t>(i)]; }

    static bool HostOver(Host::Phase p) { return p == Host::Phase::Done || p == Host::Phase::Error || p == Host::Phase::Left; }
    static bool GuestOver(Guest::Phase p) { return p == Guest::Phase::Done || p == Guest::Phase::Error || p == Guest::Phase::Left; }
    bool Terminal() const {
        if (!HostOver(host.phase)) {
            return false;
        }
        for (const auto& g : guests) {
            if (!g->observer && !GuestOver(g->phase)) {
                return false;
            }
        }
        return true;
    }
    void StepFrame() {
        ++frame;
        w.NextFrame();
        if (hook) {
            hook(*this);
        }
        host.Frame(frame);
        TryPressStart();
        for (auto& g : guests) {
            g->Frame(frame);
        }
        Invariants();
    }
    void TryPressStart() {
        if (pressed || host.phase != Host::Phase::Lobby) {
            return;
        }
        bool go = false;
        if (press_at != 0) {
            go = frame >= press_at;
        } else if (host.listed >= expect) {
            go = ++stable >= start_after;
        } else {
            stable = 0;
        }
        if (go) {
            host.PressStart();
            pressed = true;
            press_frame = frame;
        }
    }
    void Invariants() {
        CHECK(w.air.message_count() <= 64);  // the memory of the air does not grow
        std::set<u32> nodes;
        for (const auto& g : guests) {
            if (g->phase == Guest::Phase::InSession) {
                CHECK(g->my_node >= 2 && g->my_node <= 4);
                CHECK(nodes.insert(g->my_node).second);  // two consoles never share a node
            }
        }
        for (int h = 0; h < FakeAir::kPorts; ++h) {
            CHECK(w.air.Present(h) <= 4);  // never more than 4 players in a lobby
        }
    }
    bool Run(u32 max_frames) {
        while (!Terminal() && frame < max_frames) {
            StepFrame();
        }
        return Terminal();
    }
    template <typename F>
    bool RunUntil(F cond, u32 max_frames) {
        while (!cond() && frame < max_frames) {
            StepFrame();
        }
        return cond();
    }
    int InSessionCount() const {
        int n = 0;
        for (const auto& g : guests) {
            n += g->phase == Guest::Phase::InSession ? 1 : 0;
        }
        return n;
    }
};

/// A complete clean lobby: the host and every guest finished, with the host's FIRST passphrase, nothing refused, nobody kicked.
static void ExpectComplete(Room& r, bool exact_counters = true) {
    Host& h = r.host;
    CHECK(h.phase == Host::Phase::Done);
    CHECK(h.sent_first);
    for (const auto& g : r.guests) {
        CHECK(g->phase == Guest::Phase::Done);
        CHECK(g->has_pass);
        CHECK_BYTES(g->pass, h.first_pass, 9);
        CHECK_EQ(g->join_result, 0u);
        CHECK(g->node_ids.size() >= 2);
    }
    // the best roster among the guests holds every player: a guest that finished first has left the network when a slower one
    // reads its last roster, so not every guest is required to see all of them
    std::size_t best = 0;
    for (const auto& g : r.guests) {
        best = std::max(best, g->node_ids.size());
    }
    CHECK(best == r.guests.size() + 1);
    CHECK(h.max_listed == r.guests.size());
    for (std::size_t i = 0; i <= r.guests.size(); ++i) {  // the host and the guests of the room (another game's console may share the air)
        Con& c = r.con(static_cast<int>(i));
        CHECK(!c.lobby.active());
        CHECK_EQ(c.svc.unknown_commands(), 0u);
        CHECK_EQ(c.svc.bad_pointer(), 0u);
        for (int n = 0; n < FakeAir::kNodes; ++n) {
            CHECK_EQ(c.net.Kicked(n), 0);
        }
        if (exact_counters) {
            CHECK_EQ(c.lobby.bad_messages(), 0u);
        }
    }
    CHECK_EQ(r.con(0).net.HostOpenCalls(), 1);
    CHECK_EQ(r.con(0).net.HostCloseCalls(), 1);
    CHECK_STR(r.con(0).net.CloseOrder(), "H");
    CHECK_EQ(r.con(0).svc.refused(), h.refused_repeats);
    for (const auto& g : r.guests) {
        Con& c = g->c;
        CHECK_EQ(c.net.ScanOpenCalls(), 1);
        CHECK_EQ(c.net.ScanCloseCalls(), 1);
        CHECK_EQ(c.net.ConnectBeginCalls(), 1);
        CHECK_EQ(c.net.DisconnectCalls(), 1);
        CHECK_STR(c.net.CloseOrder(), "SD");
        CHECK_EQ(c.svc.refused(), 0u);
        CHECK(g->aged_out == 0);
    }
    // the books of the air: each client sent one START_ACK and one GO_ACK at least, the host START and GO
    for (const auto& g : r.guests) {
        CHECK(g->c.net.SentTotal(T_START_ACK) >= 1);
        CHECK(g->c.net.SentTotal(T_GO_ACK) >= 1);
    }
    CHECK(r.con(0).net.SentTotal(T_START) >= static_cast<int>(r.guests.size()));
    CHECK(r.con(0).net.SentTotal(T_GO) >= static_cast<int>(r.guests.size()));
}

/// What each client saw of the group when it chose it: the host and the clients before it already counted (the game hides a
/// group with +9 == +8). Exact when the clients come in one after the other (>= 40 frames apart), else between 1 and all.
static void ExpectJoinedCounts(Room& r, const std::vector<u32>& starts) {
    bool sequential = true;
    for (std::size_t i = 1; i < starts.size(); ++i) {
        sequential = sequential && starts[i] >= starts[i - 1] + 40;
    }
    for (std::size_t i = 0; i < r.guests.size(); ++i) {
        const u32 j = r.guests[i]->chosen_joined;
        if (sequential) {
            CHECK_EQ(j, 1 + i);
        } else {
            CHECK(j >= 1 && j <= r.guests.size());
        }
    }
}

// =====================================================================================================================
// 1. Battle Royale

/// A complete BR lobby: `starts` are the frames at which each client opens its search (they enter at different times).
static void BrRound(const char* label, int nclients, const std::vector<u32>& starts, bool by_name, bool net_thread) {
    g_ctx = label;
    std::printf("  %s\n", label);
    World w(1 + nclients, kBr);
    w.net_thread = net_thread;
    Room room(w, kBr, nclients);
    room.host.by_name = by_name;
    room.host.child_word = 0;
    room.start_after = 25;
    for (int i = 0; i < nclients; ++i) {
        Guest& g = *room.guests[static_cast<std::size_t>(i)];
        g.start_frame = starts[static_cast<std::size_t>(i)];
        g.choose_delay = 2 + static_cast<u32>(i);
        g.deep = true;
        g.status_after_init = true;
        g.expect_child = 0;
        g.c.variant = i % 3;  // canaries, exact-size, misaligned
    }
    room.host.c.variant = 1;
    CHECK(room.Run(3000));
    ExpectComplete(room);
    ExpectJoinedCounts(room, starts);
    // the creator saw the names the clients introduced themselves with
    std::set<std::string> want;
    for (int i = 1; i <= nclients; ++i) {
        want.insert(NameOf(kBr, i, true));
    }
    std::set<std::string> got;
    for (const auto& g : room.guests) {
        if (g->names.size() == static_cast<std::size_t>(nclients) + 1) {
            for (const std::string& n : g->names) {
                got.insert(n);
            }
        }
    }
    got.erase(room.host.name());
    CHECK(got == want);
    CHECK_EQ(room.host.refused_repeats, 0u);  // BR sends the passphrase once
}

/// The fifth console tries to come in. `late`: it scans when the three are already in and, with a stale list, picks the
/// full group anyway; else it races the others from the start. Either way it is NOT admitted: the engine stays silent, the
/// join times out at exactly 3000 ms, and nothing changes for the four players.
static void FifthConsole(const Style& st, bool late) {
    g_ctx = std::string(st.name) + (late ? " fifth console, late" : " fifth console, racing");
    std::printf("  %s\n", g_ctx.c_str());
    World w(5, st);
    Room room(w, st, 4);
    room.press_at = 330;  // the three have been in for a long time and the extra has timed out by then
    for (int i = 0; i < 4; ++i) {
        Guest& g = *room.guests[static_cast<std::size_t>(i)];
        g.choose_delay = 2;
        g.start_frame = late ? static_cast<u32>(10 * i) : 0;
    }
    Guest& extra = *room.guests[3];
    if (late) {
        extra.start_frame = 120;
        extra.allow_full = true;  // a list that went stale: it picks a group the game would have hidden
        extra.choose_delay = 0;
    }
    Con& host = room.con(0);
    bool checked = false;
    room.hook = [&](Room& r) {
        if (extra.phase == Guest::Phase::Error && !checked) {
            checked = true;
            // right now the three are in, the lobby is full and still open
            CHECK(r.host.phase == Host::Phase::Lobby);
            CHECK_EQ(r.w.air.Present(0), 4);
            CHECK_EQ(r.host.listed, 3u);
            CHECK_EQ(r.InSessionCount(), 3);
            for (int n = 0; n < FakeAir::kNodes; ++n) {
                CHECK_EQ(host.net.Kicked(n), 0);  // the host never saw the extra: nothing to eject
            }
            CHECK_EQ(extra.c.net.DisconnectCalls(), 1);
            CHECK(!extra.c.lobby.active());
            CHECK_EQ(extra.c.FkStatus().w[2] >> 24, 1u);  // back to "initialised": no stray state 5
        }
    };
    CHECK(room.Run(3000));
    CHECK(checked);
    CHECK(extra.phase == Guest::Phase::Error);
    CHECK_EQ(extra.error_type, 2);
    CHECK_EQ(extra.join_result, R_TIMEOUT);
    // 3000 ms from the join to the answer (frames of 16.67 ms: the loop of the plugin polls every 10 ms, we every frame)
    CHECK(extra.frames_joining >= 170 && extra.frames_joining <= 190);
    if (late) {
        CHECK_EQ(extra.chosen_joined, 4u);  // it saw the group full: host + 3
    }
    // the other three completed with one passphrase (racing: the engine admits the first three ports), the extra got nothing
    for (int i = 0; i < 3; ++i) {
        const Guest& g = *room.guests[static_cast<std::size_t>(i)];
        CHECK(g.phase == Guest::Phase::Done);
        CHECK(g.has_pass);
        CHECK_BYTES(g.pass, room.host.first_pass, 9);
    }
    CHECK(room.host.phase == Host::Phase::Done);
    CHECK(!extra.has_pass);
    CHECK_EQ(room.host.max_listed, 3u);
    for (int n = 0; n < FakeAir::kNodes; ++n) {
        CHECK_EQ(host.net.Kicked(n), 0);
    }
    CHECK_EQ(host.net.HostCloseCalls(), 1);
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        CHECK(!room.con(static_cast<int>(i)).lobby.active());
        CHECK_EQ(room.con(static_cast<int>(i)).lobby.bad_messages(), 0u);
    }
}

/// A fifth console that only looks: it sees the group while it is open (full: the game hides it), never joins, and the group
/// vanishes from its list once the host stops accepting.
static void BrFifthObserver() {
    g_ctx = "BR fifth console, only looking";
    std::printf("  %s\n", g_ctx.c_str());
    World w(5, kBr);
    Room room(w, kBr, 4);
    room.press_at = 260;
    for (int i = 0; i < 3; ++i) {
        room.guests[static_cast<std::size_t>(i)]->start_frame = static_cast<u32>(10 * i);
    }
    Guest& watch = *room.guests[3];
    watch.observer = true;
    watch.start_frame = 150;
    u32 saw_full = 0;
    u32 saw_any_after = 0;
    room.hook = [&](Room& r) {
        if (watch.phase != Guest::Phase::Scanning) {
            return;
        }
        for (const Guest::Seen& e : watch.table) {
            if (e.used && e.fresh + 1 == r.frame) {  // read in the frame that just ended
                if (!r.pressed && e.joined == e.max) {
                    ++saw_full;
                }
                if (r.pressed && r.frame > r.press_frame + 4) {
                    ++saw_any_after;  // a group that is read after the host stopped accepting
                }
            }
        }
    };
    CHECK(room.RunUntil([&] { return room.pressed && room.frame > room.press_frame + 200; }, 3000));
    CHECK(saw_full > 20);       // it saw the group, full (+9 == +8): the game hides it, nothing to choose
    CHECK_EQ(saw_any_after, 0u);  // after StartDistribution the group is gone for the searchers
    CHECK(watch.phase == Guest::Phase::Scanning);
    CHECK(watch.pass[0] == 0 && !watch.has_pass);
}

static void ScenarioBattleRoyale() {
    std::printf("1. Battle Royale (SPEC-DLP.md sez. 7-8, FLUSSO-DLP-KIRBY-BR.md)\n");
    BrRound("BR, 2 consoles (InitializeWithName)", 1, {0}, true, true);
    BrRound("BR, 2 consoles (plain Initialize, no network thread)", 1, {0}, false, false);
    BrRound("BR, 4 consoles, clients come in at different times", 3, {0, 70, 160}, true, true);
    BrRound("BR, 4 consoles, all three search at once", 3, {0, 0, 0}, true, true);
    FifthConsole(kBr, true);
    FifthConsole(kBr, false);
    FifthConsole(kTd, true);
    FifthConsole(kTd, false);
    BrFifthObserver();
}

// =====================================================================================================================
// 2. Triple Deluxe / Planet Robobot

static void TdRound(const char* label, const Style& st, int nclients, const std::vector<u32>& starts, u32 child_word,
                    bool net_thread) {
    g_ctx = label;
    std::printf("  %s\n", label);
    World w(1 + nclients, st);
    w.net_thread = net_thread;
    Room room(w, st, nclients);
    room.host.child_word = child_word;  // the left-over word [4] of the wrapper
    room.start_after = 25;
    for (int i = 0; i < nclients; ++i) {
        Guest& g = *room.guests[static_cast<std::size_t>(i)];
        g.start_frame = starts[static_cast<std::size_t>(i)];
        g.choose_delay = 2;
        g.deep = true;
        g.expect_child = child_word & 0xFF;
        g.c.variant = (i + 1) % 3;
    }
    CHECK(room.Run(3000));
    ExpectComplete(room);
    ExpectJoinedCounts(room, starts);
    // TD sends the passphrase on EVERY frame of state 8: the first call wins, every other one is "invalid state" and nothing else
    CHECK(room.host.refused_repeats >= 1);
    // the passphrase is the game's "%08x" and its NUL, byte for byte as it went in
    CHECK_EQ(room.host.first_pass[8], 0u);
    for (int i = 0; i < 8; ++i) {
        const u8 ch = room.host.first_pass[i];
        CHECK((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
    }
}

/// The manual-accept variant, a HYPOTHESIS (TD really calls StartAccepting(0, 6) and no AcceptClient, no IsChild): the clients
/// wait in the list as "waiting" (0x40, not joined) until the game accepts them; one that is not accepted when Start is pressed
/// is ejected, and the others go on.
static void TdManual() {
    g_ctx = "TD manual accept (hypothesis)";
    std::printf("  %s\n", g_ctx.c_str());
    {
        World w(4, kTd);
        Room room(w, kTd, 3, true);
        room.start_after = 25;
        room.guests[0]->status_after_init = true;
        for (int i = 0; i < 3; ++i) {
            room.guests[static_cast<std::size_t>(i)]->start_frame = static_cast<u32>(20 * i);
            room.guests[static_cast<std::size_t>(i)]->choose_delay = 2;
        }
        // while the first client waits to be accepted (0x40) the commands that take a node id are tried with ids wider than a
        // byte: 0x0102 is NOT node 2, whatever its low byte is, and the waiting client is not touched
        room.host.accept_limit = 0;
        bool probed = false;
        room.hook = [&](Room& r) {
            if (!probed && r.host.listed >= 1) {
                probed = true;
                Con& hc = r.host.c;
                CHECK_EQ(hc.SvClientState(2).w[2] >> 24, 0x40u);   // waiting, not joined
                for (const u32 wide : {0x0102u, 0x0202u, 0x1002u, 0xFF02u}) {
                    CHECK_EQ(hc.SvAccept(wide).res(), R_NO_DATA);
                    CHECK_EQ(hc.SvClientState(wide).res(), R_NO_DATA);
                    CHECK_EQ(hc.SvClientInfo(wide).res(), R_NO_DATA);
                    CHECK_EQ(hc.SvDisconnect(wide).res(), R_NO_DATA);
                }
                CHECK_EQ(hc.SvClientState(2).w[2] >> 24, 0x40u);   // still waiting
                CHECK_EQ(hc.net.Kicked(2), 0);
                CHECK_EQ(r.guests[0]->c.FkNodeInfo(0x0102).res(), R_NO_DATA);  // (the searcher's side, same width rule)
                r.host.accept_limit = 99;
            }
        };
        CHECK(room.Run(3000));
        CHECK(probed);
        ExpectComplete(room);
        CHECK_EQ(room.host.accepted, 3u);
    }
    {
        // the game accepts only two of the three: the third is ejected when Start is pressed and sees the host go
        g_ctx = "TD manual accept, one client never accepted";
        World w(4, kTd);
        Room room(w, kTd, 3, true);
        room.host.accept_limit = 2;
        room.start_after = 25;
        room.expect = 3;
        for (int i = 0; i < 3; ++i) {
            room.guests[static_cast<std::size_t>(i)]->start_frame = static_cast<u32>(20 * i);
            room.guests[static_cast<std::size_t>(i)]->choose_delay = 2;
        }
        CHECK(room.Run(3000));
        int done = 0;
        int lost = 0;
        for (const auto& g : room.guests) {
            done += g->phase == Guest::Phase::Done ? 1 : 0;
            if (g->phase == Guest::Phase::Error) {
                ++lost;
                CHECK_EQ(g->error_type, 6);  // 0x80: the game's own error dialog
            }
        }
        CHECK_EQ(done, 2);
        CHECK_EQ(lost, 1);
        CHECK(room.host.phase == Host::Phase::Done);
        int kicks = 0;
        for (int n = 0; n < FakeAir::kNodes; ++n) {
            kicks += room.con(0).net.Kicked(n);
        }
        CHECK_EQ(kicks, 1);
        for (const auto& g : room.guests) {
            if (g->phase == Guest::Phase::Done) {
                CHECK_BYTES(g->pass, room.host.first_pass, 9);
            }
        }
    }
}

/// TD's SRVR Initialize leaves word [4] as the thread's command buffer had it: whatever it is, the group is found, the
/// child index travels in TitleInfo.variation and the join works (the filter and the join ignore the child index).
static void TdChildGarbage() {
    TdRound("TD, SRVR Initialize [4] left as 0xDEAD00A7", kTd, 2, {0, 40}, 0xDEAD00A7u, true);
    TdRound("TD, SRVR Initialize [4] left as 0xFFFFFFFF", kTd, 1, {0}, 0xFFFFFFFFu, true);
    TdRound("TD, SRVR Initialize [4] left as 0x00000100", kTd, 1, {0}, 0x00000100u, true);
}

static void ScenarioTripleDeluxe() {
    std::printf("2. Triple Deluxe / Planet Robobot (GIOCO-TD-PR.md sez. 3-4)\n");
    TdRound("TD, 2 consoles", kTd, 1, {0}, 0, true);
    TdRound("TD, 4 consoles, clients come in at different times", kTd, 3, {0, 55, 140}, 0, true);
    TdRound("TD, 4 consoles, commands only (no network thread)", kTd, 3, {0, 30, 90}, 0, false);
    TdRound("PR, 4 consoles", kPr, 3, {10, 10, 60}, 0, true);
    TdChildGarbage();
    TdManual();
}

// =====================================================================================================================
// 3. The game goes to UDS at every point of the lobby

enum class Point { H_Accepting0, H_Accepting2, H_Distributing, H_Passphrase, H_GoInFlight, H_Complete,
                   G_Scanning, G_Joining, G_Session, G_Started, G_Complete, A_Distributing };
static const char* PointName(Point p) {
    switch (p) {
    case Point::H_Accepting0: return "creator, accepting, nobody in";
    case Point::H_Accepting2: return "creator, accepting, 2 clients in";
    case Point::H_Distributing: return "creator, distributing (START sent)";
    case Point::H_Passphrase: return "creator, state 8 (before the passphrase)";
    case Point::H_GoInFlight: return "creator, passphrase sent (GO acks lost)";
    case Point::H_Complete: return "creator, complete (9)";
    case Point::G_Scanning: return "client, scanning";
    case Point::G_Joining: return "client, join pending";
    case Point::G_Session: return "client, in session (5)";
    case Point::G_Started: return "client, started (6)";
    case Point::G_Complete: return "client, complete (9)";
    case Point::A_Distributing: return "all three consoles at once, distributing";
    }
    return "?";
}

/// An entry read in the frame that just ended.
static bool AnyFresh(const Guest& g, u32 frame) {
    for (const Guest::Seen& e : g.table) {
        if (e.used && e.fresh == frame) {
            return true;
        }
    }
    return false;
}

/// The event on ONE console: true the first time (unless there was nothing to release), false the second; no network left
/// taken; and what its own game sees afterwards through the IPC: 1 (a creator or a searcher), or 9 if it was complete.
static void FireUds(Con& c, bool host_side, bool expect_released, u32 expect_state, const char* close_order) {
    const bool first = c.lobby.OnGameUdsStarting();
    const bool second = c.lobby.OnGameUdsStarting();
    CHECK_EQ(first, expect_released);
    CHECK(!second);  // idempotent
    CHECK(!c.lobby.active());
    CHECK_EQ(host_side ? c.SvState().w[2] : (c.FkStatus().w[2] >> 24), expect_state);
    CHECK_STR(c.net.CloseOrder(), close_order);  // released exactly once, in the right order
}

/// Runs a lobby to `p`, lets the game of one console (or of all) go to UDS (OnGameUdsStarting), and runs on: the lobby
/// closes in order; the console that left sees 1 (9 stays 9); the others see the host go (0x80, their game's error) or
/// finish (9) with the host's passphrase; and nobody is left hanging.
static void UdsPoint(const Style& st, Point p) {
    g_ctx = std::string("UDS ") + st.name + ": " + PointName(p);
    const bool host_point = p <= Point::H_Complete;
    const int nclients = p == Point::H_Accepting0 ? 0 : (p == Point::G_Scanning ? 1 : 2);
    World w(1 + nclients, st);
    Room room(w, st, nclients);
    room.start_after = 15;
    for (int i = 0; i < nclients; ++i) {
        room.guests[static_cast<std::size_t>(i)]->choose_delay = 2;
        room.guests[static_cast<std::size_t>(i)]->start_frame = static_cast<u32>(12 * i);
    }
    Guest* target = nclients > 0 ? room.guests[0].get() : nullptr;
    Guest* other = nclients > 1 ? room.guests[1].get() : nullptr;
    constexpr u32 kNever = 0xFFFFFFFFu;
    switch (p) {
    case Point::H_Accepting0:
    case Point::H_Accepting2:
    case Point::G_Session:
        room.press_at = kNever;
        break;
    case Point::H_Distributing:
    case Point::G_Started:
    case Point::A_Distributing:
        w.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);  // the host stays in 6 for 1500 ms
        break;
    case Point::H_Passphrase:
        room.host.hold_pass = true;
        break;
    case Point::H_GoInFlight:
        w.air.Drop(T_GO_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);  // the host stays in 8 (GO sent) for 2000 ms
        break;
    case Point::H_Complete:
        room.host.hold_final = true;
        break;
    case Point::G_Scanning:
        room.press_at = kNever;
        target->observer = true;
        break;
    case Point::G_Joining:
        room.press_at = kNever;
        target->c.net.ConnectDelay(600);
        break;
    case Point::G_Complete:
        target->hold_final = true;
        break;
    }
    auto reached = [&]() -> bool {
        switch (p) {
        case Point::H_Accepting0: return room.host.phase == Host::Phase::Lobby && room.frame > 30;
        case Point::H_Accepting2: return room.host.listed == 2 && room.InSessionCount() == 2 && room.frame > 60;
        case Point::H_Distributing:
        case Point::A_Distributing: return room.pressed && room.host.last_state == 6;
        case Point::H_Passphrase: return room.host.last_state == 8;
        case Point::H_GoInFlight: return room.host.sent_first && room.host.last_state == 8;
        case Point::H_Complete: return room.host.last_state == 9;
        case Point::G_Scanning: return AnyFresh(*target, room.frame) && room.frame > 30;
        case Point::G_Joining: return target->phase == Guest::Phase::Joining && target->frames_joining >= 6;
        case Point::G_Session:
            return target->phase == Guest::Phase::InSession && target->last_state == 5 && other->phase == Guest::Phase::InSession &&
                   room.frame > 90;
        case Point::G_Started: return target->phase == Guest::Phase::InSession && target->last_state == 6;
        case Point::G_Complete: return target->phase == Guest::Phase::InSession && target->last_state == 9;
        }
        return false;
    };
    CHECK(room.RunUntil(reached, 4000));

    // ---- the event
    if (host_point) {
        const bool at9 = p == Point::H_Complete;
        FireUds(room.con(0), true, !at9, at9 ? 9u : 1u, "H");  // at 9 the network was closed already: nothing to release
        CHECK_EQ(w.air.Present(0), 0);
        room.host.phase = Host::Phase::Left;
    } else if (p == Point::A_Distributing) {
        FireUds(room.con(0), true, true, 1, "H");
        room.host.phase = Host::Phase::Left;
        for (auto& g : room.guests) {
            FireUds(g->c, false, true, 1, "SD");
            g->phase = Guest::Phase::Left;
        }
        CHECK_EQ(w.air.Present(0), 0);
    } else if (p == Point::G_Scanning) {
        FireUds(target->c, false, true, 1, "S");
        target->phase = Guest::Phase::Left;
    } else {
        const bool keeps9 = p == Point::G_Complete;
        FireUds(target->c, false, true, keeps9 ? 9u : 1u, "SD");
        if (keeps9) {  // 9 stays 9: the passphrase and the roster are still readable
            const Reply pr = target->c.FkPass();
            CHECK_BYTES(pr.w + 2, target->pass, 9);
            CHECK(target->has_pass);
            u32 total = 0;
            target->c.Nodes(4, &total);
            CHECK(total >= 2);
        }
        if (p == Point::G_Joining) {
            // the thread of the game that is waiting inside StartFakeSession is released: the answer is "invalid state"
            bool done = false;
            const Reply pr = target->c.Pending(&done);
            CHECK(done);
            CHECK_EQ(pr.res(), R_INVALID_STATE);
            CHECK(!target->c.svc.join_pending());
            CHECK(!target->c.lobby.join_active());
            room.expect = 1;  // the host starts with the one client that is left
            room.press_at = 0;
            room.stable = 0;
        }
        if (p == Point::G_Session) {
            room.expect = 1;
            room.press_at = 0;
            room.stable = 0;
        }
        target->phase = Guest::Phase::Left;
    }

    // ---- the others carry on and everything comes to rest
    if (p == Point::G_Scanning) {
        w.Idle(3000);
        // the table the searcher had stays (frozen): the game that already left UDS-ward could still read it
        const Reply r = target->c.FkInOrder(1, target->c.TitleSb().w);
        CHECK_EQ(r.res(), 0u);
        CHECK_EQ(room.con(0).SvState().w[2], 2u);  // the creator is still open and waiting, untouched
        CHECK_EQ(room.con(0).SvFinalize().res(), 0u);
    } else {
        CHECK(room.Run(room.frame + 1800));
        for (const auto& g : room.guests) {
            if (g->phase == Guest::Phase::Left) {
                continue;
            }
            if (p == Point::H_GoInFlight || p == Point::H_Complete || p == Point::G_Joining || p == Point::G_Session ||
                p == Point::G_Complete) {
                CHECK(g->phase == Guest::Phase::Done);  // the GO had got through / the host finished with the others
                CHECK_BYTES(g->pass, room.host.first_pass, 9);
            } else {
                CHECK(g->phase == Guest::Phase::Error);  // the host went before the GO: the game's own error state 0x80
                CHECK_EQ(g->error_type, 6);
            }
        }
        if (!host_point && p != Point::A_Distributing) {
            CHECK(room.host.phase == Host::Phase::Done);  // the creator finished with whoever was left
            CHECK(other != nullptr);
        }
    }
    w.Idle(3000);
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        Con& c = room.con(static_cast<int>(i));
        CHECK(!c.lobby.active());       // no network left taken
        CHECK(!c.svc.join_pending());   // nobody inside StartFakeSession
        CHECK(!c.lobby.join_active());
    }
    CHECK_EQ(w.air.Present(0), 0);
    CHECK(w.air.message_count() <= 64);
}

static void ScenarioUds() {
    std::printf("3. the game goes to UDS (OnGameUdsStarting) at every point of the lobby\n");
    const Point all[] = {Point::H_Accepting0, Point::H_Accepting2, Point::H_Distributing, Point::H_Passphrase, Point::H_GoInFlight,
                         Point::H_Complete, Point::G_Scanning, Point::G_Joining, Point::G_Session, Point::G_Started,
                         Point::G_Complete, Point::A_Distributing};
    for (const Style* st : {&kTd, &kBr}) {
        for (const Point p : all) {
            UdsPoint(*st, p);
        }
    }
}

// =====================================================================================================================
// 4. Loss, duplication, reordering: 200 complete lobbies of 4 consoles

struct LossStats {
    int complete = 0;
    int clean_error = 0;
    u32 peak_in_flight = 0;
    u32 peak_at_rest = 0;     // messages still held by the air when a round is over and the clock has run on
    u32 first_half_peak = 0;  // ... in the first / second hundred rounds: the memory of the air does not grow
    u32 second_half_peak = 0;
    u32 max_frames = 0;
    u32 bad_messages = 0;
    u32 total_frames = 0;
};

/// Rules for one round, from the seed. Returns a description for the failure line.
static std::string LossRules(World& w, Rng& rng) {
    std::string d;
    const int n = static_cast<int>(rng.Below(4));  // 0..3 rules
    for (int i = 0; i < n; ++i) {
        const int type = 1 + static_cast<int>(rng.Below(4));
        const int kind = static_cast<int>(rng.Below(9));
        char b[64];
        switch (kind) {
        case 0:
        case 1:
        case 2: {
            const int cnt = 1 + static_cast<int>(rng.Below(4));
            w.air.Drop(type, FakeAir::kAny, FakeAir::kAny, cnt);
            std::snprintf(b, sizeof(b), "drop%d x%d;", type, cnt);
            break;
        }
        case 3: {
            const int from = (rng.Below(2) == 0) ? 1 : FakeAir::kAny;
            const int to = (from == 1) ? 2 + static_cast<int>(rng.Below(3)) : FakeAir::kAny;
            w.air.Drop(type, from, to, FakeAir::kForever);  // one client never gets (or never answers) this type
            std::snprintf(b, sizeof(b), "drop%d %d>%d forever;", type, from, to);
            break;
        }
        case 4: {
            const int k = 1 + static_cast<int>(rng.Below(3));
            w.air.Dup(type, k);
            std::snprintf(b, sizeof(b), "dup%d +%d;", type, k);
            break;
        }
        case 5: {
            const u32 ms = 20 + rng.Below(430);
            w.air.Delay(type, ms);
            std::snprintf(b, sizeof(b), "delay%d %ums;", type, ms);
            break;
        }
        case 6:
            w.air.Reorder(type);
            std::snprintf(b, sizeof(b), "reorder%d;", type);
            break;
        case 7: {
            const int a = 1 + static_cast<int>(rng.Below(4));
            const int c2 = 1 + static_cast<int>(rng.Below(4));
            const u32 ms = 100 + rng.Below(2900);
            w.air.Partition(a, c2, ms);
            std::snprintf(b, sizeof(b), "partition %d>%d %ums;", a, c2, ms);
            break;
        }
        default: {
            w.air.Drop(0, FakeAir::kAny, FakeAir::kAny, 1 + static_cast<int>(rng.Below(6)));  // any type
            std::snprintf(b, sizeof(b), "dropany;");
            break;
        }
        }
        d += b;
    }
    return d;
}

/// One round on `w` (reused across rounds). Returns a signature of what happened (for the determinism check).
static u64 LossRound(World& w, const Style& st, int round, LossStats* stats) {
    Rng rng(static_cast<u64>(round) + 1000);
    w.air.ClearRules();
    const std::string rules = LossRules(w, rng);
    char b[160];
    std::snprintf(b, sizeof(b), "loss round %d %s world-clock=%llu rules: %s", round, st.name,
                  static_cast<unsigned long long>(w.air.now()), rules.c_str());
    g_ctx = b;
    w.net_thread = rng.Below(4) != 0;
    Room room(w, st, 3);
    room.start_after = 10 + rng.Below(15);
    room.host.child_word = rng.Below(2) ? 0 : rng.Next();
    for (int i = 0; i < 3; ++i) {
        Guest& g = *room.guests[static_cast<std::size_t>(i)];
        g.start_frame = rng.Below(70);
        g.choose_delay = rng.Below(12);
        g.expect_child = room.host.child_word & 0xFF;
        g.c.variant = static_cast<int>(rng.Below(3));
    }
    room.host.c.variant = static_cast<int>(rng.Below(3));
    const bool over = room.Run(1800);
    CHECK(over);  // no round hangs: 30 simulated seconds is far beyond the worst case (3 s of join + 3.5 s of phases)
    if (!over) {
        std::printf("    host phase %d state %u; guests:", static_cast<int>(room.host.phase), room.host.last_state);
        for (const auto& g : room.guests) {
            std::printf(" [%d st %u]", static_cast<int>(g->phase), g->last_state);
        }
        std::printf("\n");
    }
    // everything comes to rest: the host back, the clients' hosts-gone timers (2 s) elapsed
    w.Idle(2600);
    // classification
    Host& h = room.host;
    int done = 0;
    int lost = 0;
    u64 sig = room.frame;
    for (const auto& g : room.guests) {
        if (g->phase == Guest::Phase::Done) {
            ++done;
            CHECK(h.sent_first);
            CHECK_BYTES(g->pass, h.first_pass, 9);  // the FIRST passphrase of the game, never another
        } else {
            CHECK(g->phase == Guest::Phase::Error);
            CHECK_EQ(g->error_type, 6);  // told by the game's own error state 0x80 (every join succeeds here: nobody fails a join)
            ++lost;
        }
        sig = sig * 131 + static_cast<u64>(g->phase) * 7 + static_cast<u64>(g->error_type + 2);
    }
    sig = sig * 131 + static_cast<u64>(h.phase);
    for (int i = 0; i < 9; ++i) {
        sig = sig * 131 + h.first_pass[i];
    }
    CHECK(h.phase == Host::Phase::Done);  // no network of the creator is lost here (nothing vanishes): it always gets to 9 and Finalize
    const bool complete = done == 3;
    if (complete) {
        ++stats->complete;
    } else {
        ++stats->clean_error;
        CHECK(lost > 0);  // not complete means somebody was told so
    }
    // nothing is left taken, nothing hangs, the air's memory does not grow
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        Con& c = *w.c[i];
        CHECK(!c.lobby.active());
        CHECK(!c.svc.join_pending());
        CHECK(!c.lobby.join_active());
        stats->bad_messages += c.lobby.bad_messages();
    }
    CHECK_EQ(w.air.Present(0), 0);
    CHECK(w.air.message_count() <= 64);
    stats->peak_in_flight = std::max<u32>(stats->peak_in_flight, static_cast<u32>(w.air.message_count()));
    stats->peak_at_rest = std::max<u32>(stats->peak_at_rest, static_cast<u32>(w.air.message_count()));
    (round < 100 ? stats->first_half_peak : stats->second_half_peak) =
        std::max<u32>(round < 100 ? stats->first_half_peak : stats->second_half_peak, static_cast<u32>(w.air.message_count()));
    stats->max_frames = std::max(stats->max_frames, room.frame);
    stats->total_frames += room.frame;
    w.air.ClearRules();
    return sig;
}

/// One mild fault at a time (a few messages of one type lost, duplicated, reordered or held back less than the resend period
/// allows): the lobby MUST complete, nobody ejected, whatever the type. These are the losses the resends exist for.
static void MildFaults() {
    std::printf("  one mild fault at a time must always complete\n");
    struct Fault {
        const char* name;
        int kind;  // 0 drop, 1 dup, 2 reorder, 3 delay
        int arg;
    };
    const Fault faults[] = {{"drop 1", 0, 1}, {"drop 2", 0, 2}, {"drop 3", 0, 3}, {"drop 4", 0, 4},
                            {"dup +1", 1, 1}, {"dup +3", 1, 3}, {"reorder", 2, 0}, {"delay 100 ms", 3, 100}, {"delay 400 ms", 3, 400}};
    for (const Style* st : {&kTd, &kBr}) {
        for (int type = T_START; type <= T_GO_ACK; ++type) {
            for (const Fault& f : faults) {
                char b[96];
                std::snprintf(b, sizeof(b), "mild fault %s: message type %d, %s", st->name, type, f.name);
                g_ctx = b;
                World w(4, *st);
                switch (f.kind) {
                case 0: w.air.Drop(type, FakeAir::kAny, FakeAir::kAny, f.arg); break;
                case 1: w.air.Dup(type, f.arg); break;
                case 2: w.air.Reorder(type); break;
                default: w.air.Delay(type, static_cast<u64>(f.arg)); break;
                }
                Room room(w, *st, 3);
                room.start_after = 12;
                for (int i = 0; i < 3; ++i) {
                    room.guests[static_cast<std::size_t>(i)]->start_frame = static_cast<u32>(4 * i);
                    room.guests[static_cast<std::size_t>(i)]->choose_delay = 1;
                }
                CHECK(room.Run(1500));
                ExpectComplete(room, false);
            }
        }
    }
}

static void ScenarioLoss() {
    std::printf("4. loss / duplication / reordering, 200 complete lobbies of 4 consoles, fixed seeds\n");
    // Three worlds reused across the rounds (so a round starts from what the last one left): the clock of the first is
    // ordinary, the second crosses 2^32 ms during its first round, the third lives past it with ticks beyond 2^40.
    World wa(4, kTd, 100000);
    World wb(4, kBr, 0xFFFFFE00ull);
    World wc(4, kPr, 5000000000ull);
    wc.air.SetTickOffset(1ull << 41);
    World* worlds[3] = {&wa, &wb, &wc};
    const Style* styles[3] = {&kTd, &kBr, &kPr};
    LossStats stats;
    for (int round = 0; round < 200; ++round) {
        const int k = round % 3;
        LossRound(*worlds[k], *styles[k], round, &stats);
    }
    std::printf("  %d complete, %d clean closures with an error; messages held by the air: peak %u in flight, %u at rest"
                " (first 100 rounds %u, last 100 %u); longest round %u frames, mean %u; %u bad messages in all\n",
                stats.complete, stats.clean_error, stats.peak_in_flight, stats.peak_at_rest, stats.first_half_peak,
                stats.second_half_peak, stats.max_frames, stats.total_frames / 200, stats.bad_messages);
    CHECK_EQ(stats.complete + stats.clean_error, 200);
    CHECK(stats.complete >= 120);     // the resends make most of them complete
    CHECK(stats.clean_error >= 5);    // and the permanent losses do end in a clean closure
    CHECK(stats.peak_in_flight <= 64);
    CHECK(stats.peak_at_rest <= 12);                              // a round leaves almost nothing behind
    CHECK(stats.second_half_peak <= stats.first_half_peak + 2);  // ... and the second hundred rounds hold no more than the first
    MildFaults();
    // the same seeds give the same rounds (fresh worlds, so the clocks and the leftovers are the same)
    for (const int round : {3, 11, 40, 77}) {
        World x1(4, kTd, 100000);
        World x2(4, kTd, 100000);
        LossStats s1;
        LossStats s2;
        const u64 a = LossRound(x1, kTd, round, &s1);
        const u64 b = LossRound(x2, kTd, round, &s2);
        CHECK_EQ(a, b);
        CHECK_EQ(s1.total_frames, s2.total_frames);
    }
}

// =====================================================================================================================
// 5. Closures: a client leaves, the creator vanishes, two foreign lobbies on one air

enum class Moment { Accepting, Distributing, Passphrase, GoSent };
static const char* MomentName(Moment m) {
    switch (m) {
    case Moment::Accepting: return "accepting (2 clients in)";
    case Moment::Distributing: return "distributing (the leaver's START_ACK lost)";
    case Moment::Passphrase: return "state 8, before the passphrase";
    case Moment::GoSent: return "GO sent (the leaver's GO_ACK lost)";
    }
    return "?";
}

/// Host + 2 clients brought to `m`, then the first client leaves: `gentle` = its game calls StopSession, else its console
/// falls off the air without a word. The creator does not wait for what will never come, and the other client completes.
static void ClientLeaves(const Style& st, Moment m, bool gentle) {
    g_ctx = std::string("client leaves ") + st.name + ": " + MomentName(m) + (gentle ? ", StopSession" : ", silent");
    World w(3, st);
    Room room(w, st, 2);
    room.start_after = 15;
    Guest& leaver = *room.guests[0];
    Guest& stayer = *room.guests[1];
    leaver.hold_final = true;  // it is the one that goes: it must not finish by itself before
    stayer.start_frame = 10;
    switch (m) {
    case Moment::Accepting:
        room.press_at = 0xFFFFFFFFu;
        break;
    case Moment::Distributing:
        w.air.Drop(T_START_ACK, 2, 1, FakeAir::kForever);  // only the leaver's acknowledgement (it is node 2) is lost
        break;
    case Moment::Passphrase:
        room.host.hold_pass = true;
        break;
    case Moment::GoSent:
        w.air.Drop(T_GO_ACK, 2, 1, FakeAir::kForever);
        break;
    }
    auto reached = [&]() -> bool {
        switch (m) {
        case Moment::Accepting: return room.host.listed == 2 && room.InSessionCount() >= 2 && room.frame > 70;
        case Moment::Distributing: return room.pressed && room.frame > room.press_frame + 12 && room.host.last_state == 6;
        case Moment::Passphrase: return room.host.last_state == 8 && room.frame > room.press_frame + 3;
        case Moment::GoSent:
            return room.host.sent_first && room.host.last_state == 8 && stayer.last_state == 9 && leaver.last_state == 9;
        }
        return false;
    };
    CHECK(room.RunUntil(reached, 3000));
    CHECK_EQ(leaver.my_node, 2u);
    const u32 frame_left = room.frame;
    if (gentle) {
        CHECK_EQ(leaver.c.FkStopSession().res(), 0u);
        CHECK_EQ(leaver.c.FkStatus().w[2] >> 24, 1u);
        leaver.phase = Guest::Phase::Left;
    } else {
        w.air.Leave(0, 2);  // the console fell off: it sees it is not in a network any more, and the game's 0x80 follows 2 s later
        leaver.hold_final = false;
    }
    room.host.hold_pass = false;
    if (m == Moment::Accepting) {
        room.expect = 1;  // the host starts with whoever is left
        room.press_at = 0;
        room.stable = 0;
    }
    CHECK(room.RunUntil([&] { return room.host.phase == Host::Phase::Done; }, frame_left + 1500));
    if (m == Moment::Distributing || m == Moment::GoSent) {
        // the slot was freed the moment the client left: the creator did not wait for the acknowledgement that never comes,
        // nor for its grace (1500 ms = 90 frames for START, 2000 ms = 120 for GO)
        CHECK(room.frame - frame_left < 45);
    }
    CHECK(room.Run(room.frame + 1500));
    CHECK(stayer.phase == Guest::Phase::Done);
    CHECK_BYTES(stayer.pass, room.host.first_pass, 9);
    if (!gentle) {
        if (m == Moment::GoSent) {
            CHECK(leaver.phase == Guest::Phase::Done);  // it had the GO already: its game goes on
            CHECK_BYTES(leaver.pass, room.host.first_pass, 9);
        } else {
            CHECK(leaver.phase == Guest::Phase::Error);  // the game's own error state 0x80, never stuck in 5 or 6
            CHECK_EQ(leaver.error_type, 6);
        }
    }
    w.Idle(3000);
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        CHECK(!room.con(static_cast<int>(i)).lobby.active());
    }
    CHECK_EQ(w.air.Present(0), 0);
    CHECK_EQ(room.con(0).net.HostCloseCalls(), 1);
}

/// The creator disappears (`vanish`: the air forgets it with no word) or its game cancels (Finalize), at `m`: every client is
/// told (0x80, their game's error) or already has the GO; the creator's own lobby does not stay taken.
static void CreatorGone(const Style& st, Moment m, bool vanish) {
    g_ctx = std::string("creator gone ") + st.name + ": " + MomentName(m) + (vanish ? ", vanishes" : ", Finalize");
    World w(3, st);
    Room room(w, st, 2);
    room.start_after = 15;
    room.guests[1]->start_frame = 10;
    switch (m) {
    case Moment::Accepting:
        room.press_at = 0xFFFFFFFFu;
        break;
    case Moment::Distributing:
        w.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        break;
    case Moment::Passphrase:
        room.host.hold_pass = true;
        break;
    case Moment::GoSent:
        w.air.Drop(T_GO_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        break;
    }
    auto reached = [&]() -> bool {
        switch (m) {
        case Moment::Accepting: return room.host.listed == 2 && room.InSessionCount() == 2 && room.frame > 70;
        case Moment::Distributing: return room.pressed && room.frame > room.press_frame + 12 && room.host.last_state == 6;
        case Moment::Passphrase: return room.host.last_state == 8;
        case Moment::GoSent: return room.host.sent_first && room.host.last_state == 8;
        }
        return false;
    };
    CHECK(room.RunUntil(reached, 3000));
    if (vanish) {
        w.air.Vanish(0);
    } else {
        CHECK_EQ(room.con(0).SvFinalize().res(), 0u);
        CHECK_EQ(room.con(0).SvState().w[2], 0u);
        room.host.phase = Host::Phase::Left;
    }
    CHECK(room.Run(room.frame + 1500));
    // every client is told: the game's error state 0x80 (never stuck in 5 or 6), or 9 if the GO had got through before
    for (const auto& g : room.guests) {
        if (m == Moment::GoSent) {
            CHECK(g->phase == Guest::Phase::Done);
            CHECK_BYTES(g->pass, room.host.first_pass, 9);
        } else {
            CHECK(g->phase == Guest::Phase::Error);
            CHECK_EQ(g->error_type, 6);
            CHECK_EQ(g->c.FkStatus().w[2] >> 24, 0x80u);  // it stays 0x80 until the game leaves the session itself
        }
    }
    if (m != Moment::GoSent) {
        // ... and any of the three ways out brings it back to "initialised": StopSession, Finalize, Initialize
        CHECK_EQ(room.guests[0]->c.FkStopSession().res(), 0u);
        CHECK_EQ(room.guests[0]->c.FkStatus().w[2] >> 24, 1u);
        CHECK_EQ(room.guests[1]->c.FkFinalize().res(), 0u);
        CHECK_EQ(room.guests[1]->c.FkStatus().w[2] >> 24, 1u);
    }
    if (vanish) {
        // the creator's own lobby lost its network: it either went on with nobody left (the clients vanished with the network:
        // 6 -> 8 -> 9 with zero clients) or, after the 2 s, is back to "initialised" and its game sees 1 and gives up
        if (m == Moment::Accepting || m == Moment::Passphrase) {
            CHECK(room.host.phase == Host::Phase::Error);
            CHECK_EQ(room.host.error_type, 99);
        } else {
            CHECK(room.host.phase == Host::Phase::Done || (room.host.phase == Host::Phase::Error && room.host.error_type == 99));
        }
        CHECK(room.con(0).net.HostCloseCalls() >= 1);  // it closed what was left
    }
    w.Idle(3000);
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        CHECK(!room.con(static_cast<int>(i)).lobby.active());
    }
    CHECK_EQ(w.air.Present(0), 0);
}

/// Two creators of the same game on one air (3 consoles: they do not make a lobby of more than 4 together): a searcher sees both,
/// takes one, and the other is not affected. With `both`, a second searcher takes the other lobby (4 consoles: two lobbies of
/// two) and both complete independently, each client with the passphrase of ITS host.
static void TwoLobbies(const Style& st, bool both, bool second_one) {
    g_ctx = std::string("two lobbies ") + st.name + (both ? ", both filled" : (second_one ? ", takes the second" : ", takes the first"));
    World w(both ? 4 : 3, st);
    Host hostA(*w.c[0], st, false);
    Host hostB(*w.c[1], st, false);
    hostA.child_word = 0;
    hostB.child_word = 1;  // a different child index announced
    Guest gA(*w.c[2], st, 2);
    std::unique_ptr<Guest> gB;
    const Mac mac_a = w.c[0]->net.mac();
    const Mac mac_b = w.c[1]->net.mac();
    gA.has_prefer = true;
    gA.prefer = (both || !second_one) ? mac_a : mac_b;
    gA.choose_delay = 8;  // long enough to have seen both
    if (both) {
        gB = std::make_unique<Guest>(*w.c[3], st, 3);
        gB->has_prefer = true;
        gB->prefer = mac_b;
        gB->choose_delay = 8;
        gB->start_frame = 5;
    }
    Host& chosen = (both || !second_one) ? hostA : hostB;
    Host& other = (both || !second_one) ? hostB : hostA;
    bool saw_two = false;
    u32 start_a = 0;
    u32 start_b = 0;
    bool done = false;
    for (u32 frame = 1; frame < 3000 && !done; ++frame) {
        w.NextFrame();
        hostA.Frame(frame);
        hostB.Frame(frame);
        gA.Frame(frame);
        if (gB) {
            gB->Frame(frame);
        }
        if (gA.phase == Guest::Phase::Scanning) {
            int n = 0;
            for (const Guest::Seen& e : gA.table) {
                n += (e.used && e.fresh == frame) ? 1 : 0;
            }
            saw_two = saw_two || n == 2;
        }
        // each creator that has its client presses Start 20 frames later
        if (hostA.phase == Host::Phase::Lobby && hostA.listed >= 1 && (both || &chosen == &hostA) && ++start_a >= 20) {
            hostA.PressStart();
        }
        if (hostB.phase == Host::Phase::Lobby && hostB.listed >= 1 && (both || &chosen == &hostB) && ++start_b >= 20) {
            hostB.PressStart();
        }
        done = chosen.phase == Host::Phase::Done && gA.phase != Guest::Phase::Scanning &&
               (!both || (hostA.phase == Host::Phase::Done && hostB.phase == Host::Phase::Done && gB->phase == Guest::Phase::Done));
        if (!both) {
            done = (&chosen == &hostA ? hostA.phase : hostB.phase) == Host::Phase::Done && gA.phase == Guest::Phase::Done;
        }
    }
    CHECK(done);
    CHECK(saw_two);  // the searcher saw both lobbies at once
    CHECK(gA.phase == Guest::Phase::Done);
    CHECK_BYTES(gA.pass, chosen.first_pass, 9);
    CHECK(gA.chosen == (&chosen == &hostA ? mac_a : mac_b));
    if (!both) {
        // the other lobby: still open, accepting, nobody in it, never told anything, never touched
        CHECK_EQ(other.c.SvState().w[2], 2u);
        CHECK_EQ(w.air.Present(other.c.idx), 1);
        CHECK_EQ(other.c.net.SentTotal(T_START), 0);
        CHECK_EQ(other.c.net.SentTotal(T_GO), 0);
        CHECK_EQ(other.c.net.HostCloseCalls(), 0);
        CHECK_EQ(other.c.net.HostUpdateCalls(), 0);
        CHECK_EQ(other.c.net.HostOpenCalls(), 1);
        CHECK_EQ(other.listed, 0u);
        CHECK_EQ(other.c.lobby.bad_messages(), 0u);
        CHECK(other.c.lobby.active());
        CHECK_EQ(other.c.SvFinalize().res(), 0u);
    } else {
        CHECK(gB->phase == Guest::Phase::Done);
        CHECK_BYTES(gB->pass, hostB.first_pass, 9);
        CHECK(std::memcmp(hostA.first_pass, hostB.first_pass, 9) != 0);  // two different games of lobby, two passphrases
        CHECK(gA.chosen != gB->chosen);
        CHECK(hostA.c.net.SentTotal(T_START) >= 1 && hostB.c.net.SentTotal(T_START) >= 1);
        // no message crossed from one lobby to the other: nothing was refused anywhere
        for (std::size_t i = 0; i < w.c.size(); ++i) {
            CHECK_EQ(w.c[i]->lobby.bad_messages(), 0u);
            CHECK(!w.c[i]->lobby.active());
        }
    }
}

/// A lobby of ANOTHER game on the same air is invisible to the searcher and unaffected by it.
static void ForeignGame() {
    g_ctx = "foreign game";
    World w(3, kTd);
    w.AddConsole(kPr);  // console 3 hosts a Planet Robobot lobby
    Host hp(*w.c[3], kPr, false);
    Room room(w, kTd, 2);
    room.start_after = 20;
    room.guests[0]->no_filter = true;  // the unique id of the game is then the only thing that keeps the other game out
    room.guests[1]->start_frame = 10;
    room.hook = [&](Room& r) { hp.Frame(r.frame); };
    CHECK(room.Run(3000));
    ExpectComplete(room);
    // the PR host is still waiting, alone and untouched
    CHECK(hp.phase == Host::Phase::Lobby);
    CHECK_EQ(hp.last_state, 2u);
    CHECK_EQ(w.air.Present(3), 1);
    CHECK_EQ(w.c[3]->net.SentTotal(T_START), 0);
    CHECK_EQ(w.c[3]->net.HostUpdateCalls(), 0);
    CHECK_EQ(hp.listed, 0u);
    for (const auto& g : room.guests) {
        for (const Guest::Seen& e : g->table) {
            CHECK(!(e.used && e.mac == w.c[3]->net.mac()));
        }
    }
}

/// A seat that was freed is reused: the lowest free node goes to whoever comes next, and the console that left can come back.
/// Host + 3 clients (A, B, C): A leaves while the creator accepts, C takes A's node, A comes back and takes the last one; then
/// the lobby of four completes with one passphrase and nobody was ever ejected.
static void SeatReuse(const Style& st) {
    g_ctx = std::string("seat reuse ") + st.name;
    std::printf("  %s\n", g_ctx.c_str());
    World w(4, st);
    Room room(w, st, 3);
    room.start_after = 15;
    room.press_at = 0xFFFFFFFFu;
    Guest& a = *room.guests[0];
    Guest& b = *room.guests[1];
    Guest& c = *room.guests[2];
    b.start_frame = 10;
    c.start_frame = 1000000;  // not yet
    a.hold_final = b.hold_final = c.hold_final = true;
    Guest again(a.c, st, 1);  // the same console, searching again
    again.start_frame = 1000000;
    again.hold_final = true;
    room.hook = [&](Room& r) { again.Frame(r.frame); };
    CHECK(room.RunUntil([&] { return room.host.listed == 2 && a.phase == Guest::Phase::InSession && b.phase == Guest::Phase::InSession && room.frame > 70; }, 2000));
    CHECK_EQ(a.my_node, 2u);
    CHECK_EQ(b.my_node, 3u);
    CHECK_EQ(a.c.FkStopSession().res(), 0u);
    a.phase = Guest::Phase::Left;
    CHECK(room.RunUntil([&] { return room.host.listed == 1; }, room.frame + 200));
    CHECK_EQ(room.host.roster.size(), 1u);
    CHECK_EQ(room.host.roster[0].node, 3u);
    CHECK_STR(room.host.roster[0].name, NameOf(st, 2, true));
    c.start_frame = room.frame + 1;
    CHECK(room.RunUntil([&] { return room.host.listed == 2 && c.phase == Guest::Phase::InSession; }, room.frame + 400));
    CHECK_EQ(c.my_node, 2u);  // the lowest free node
    again.start_frame = room.frame + 1;
    CHECK(room.RunUntil([&] { return room.host.listed == 3 && again.phase == Guest::Phase::InSession; }, room.frame + 400));
    CHECK_EQ(again.my_node, 4u);
    CHECK_EQ(room.host.roster.size(), 3u);
    std::set<std::string> names;
    for (const Host::Member& m : room.host.roster) {
        names.insert(m.name);
    }
    CHECK(names == (std::set<std::string>{NameOf(st, 1, true), NameOf(st, 2, true), NameOf(st, 3, true)}));
    // the lobby is full (4): the game would hide it; Start, and everybody finishes with the host's passphrase
    room.press_at = 0;
    room.expect = 3;
    room.stable = 0;
    a.hold_final = b.hold_final = c.hold_final = again.hold_final = false;
    CHECK(room.RunUntil([&] { return room.Terminal() && again.phase == Guest::Phase::Done; }, room.frame + 1500));
    CHECK(b.phase == Guest::Phase::Done && c.phase == Guest::Phase::Done && again.phase == Guest::Phase::Done);
    CHECK_BYTES(b.pass, room.host.first_pass, 9);
    CHECK_BYTES(c.pass, room.host.first_pass, 9);
    CHECK_BYTES(again.pass, room.host.first_pass, 9);
    CHECK(room.host.phase == Host::Phase::Done);
    for (int n = 0; n < FakeAir::kNodes; ++n) {
        CHECK_EQ(room.con(0).net.Kicked(n), 0);
    }
    w.Idle(1000);
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        CHECK(!room.con(static_cast<int>(i)).lobby.active());
    }
}

static void ScenarioClosures() {
    std::printf("5. closures\n");
    const Moment ms[] = {Moment::Accepting, Moment::Distributing, Moment::Passphrase, Moment::GoSent};
    for (const Style* st : {&kTd, &kBr}) {
        for (const Moment m : ms) {
            ClientLeaves(*st, m, true);
            ClientLeaves(*st, m, false);
        }
    }
    for (const Style* st : {&kTd, &kBr}) {
        for (const Moment m : ms) {
            CreatorGone(*st, m, true);
            CreatorGone(*st, m, false);
        }
    }
    for (const Style* st : {&kTd, &kBr}) {
        TwoLobbies(*st, false, false);
        TwoLobbies(*st, false, true);
        TwoLobbies(*st, true, false);
    }
    ForeignGame();
    SeatReuse(kTd);
    SeatReuse(kBr);
}

// =====================================================================================================================
// 6. A soak of 100000 valid and invalid commands

struct SoakStats {
    u32 cli_seen[0x81] = {};
    u32 srv_seen[16] = {};
    u32 joins_pending = 0;
    u32 joins_ok = 0;
    u32 max_nodes_total = 0;
    u32 max_clients_total = 0;
    u32 max_present = 0;
    u32 commands = 0;
    u32 bad_ptr_replies = 0;
    u32 deep_replies = 0;
};

static const u32 kSoakNodes[] = {0, 1, 2, 3, 4, 5, 6, 0xFF, 0x100, 0x102, 0x10002, 0xFFFF, 0xFFFFFFFFu};

static bool AllowedResult(u32 r) {
    return r == 0 || r == R_INVALID_STATE || r == R_NO_DATA || r == R_BUSY || r == R_NOT_FOUND || r == R_TIMEOUT ||
           r == R_OUT_OF_RANGE || r == R_BAD_POINTER;
}

/// Which static buffer / mapped region a command may write: 0 none, 1 title, 2 server, 3 nodes, 4 clients.
static int WritesRegion(DlpKind kind, u32 id) {
    if (kind == kFk) {
        return id == 0x07 ? 2 : (id == 0x08 || id == 0x09) ? 1 : id == 0x0D ? 3 : 0;
    }
    return id == 0x0B ? 4 : 0;
}

struct Soak {
    World w;
    Rng rng;
    SoakStats* stats;
    std::vector<bool> pending;
    bool sv_bias[8] = {};
    u32 req_node_ = 0;   // the node id of the last request that carried one
    u32 req_n_ = 0;      // the element count / byte count of the last list request (for the buffer checks)
    u32 req_bytes_ = 0;
    u32 episodes[8] = {};
    u64 hash = 1469598103934665603ull;

    Soak(u64 seed, SoakStats* s) : w(4, kTd, 100000), rng(seed), stats(s), pending(5, false) {
        w.AddConsole(kBr);  // a fifth console of another game: its lobbies are not ours
        pending.assign(5, false);
    }

    void Mix(u64 v) { hash = (hash ^ v) * 1099511628211ull; }

    /// A MAC: half the time that of a console whose creator is accepting (the group a scanning game has in its list), else any
    /// console, a random MAC or the broadcast.
    Mac PickMac() {
        if (rng.Chance(50)) {
            std::vector<int> open;
            for (std::size_t i = 0; i < w.c.size(); ++i) {
                if (w.c[i]->lobby.server_state() == kSrvAccepting) {
                    open.push_back(static_cast<int>(i));
                }
            }
            if (!open.empty()) {
                return w.c[static_cast<std::size_t>(open[rng.Below(static_cast<u32>(open.size()))])]->net.mac();
            }
        }
        switch (rng.Below(6)) {
        case 0:
        case 1:
        case 2:
        case 3:
            return w.c[rng.Below(static_cast<u32>(w.c.size()))]->net.mac();
        case 4: {
            Mac m = {};
            for (u8& b : m) {
                b = static_cast<u8>(rng.Next());
            }
            return m;
        }
        default:
            return kBroadcastMac;
        }
    }
    u32 PickNode(int q) {
        if (q == 0 && rng.Chance(80)) {
            return 1 + rng.Below(4);
        }
        return kSoakNodes[rng.Below(static_cast<u32>(sizeof(kSoakNodes) / sizeof(kSoakNodes[0])))];
    }
    /// A static-buffer pair for `which` (1 title, 2 server, 3 nodes): good, or one of the ways a game can get it wrong.
    /// `sel.use_null` = no pair at all.
    struct SbSel {
        u32 w[2];
        bool use_null;
        const u32* ptr() const { return use_null ? nullptr : w; }
    };
    SbSel PickSb(Con& c, int which, int q) {
        const Sb good = which == 1 ? c.TitleSb() : which == 2 ? c.ServerSb() : c.NodesSb();
        SbSel s;
        s.w[0] = good.w[0];
        s.w[1] = good.w[1];
        s.use_null = false;
        const u32 pick = q == 0 ? (rng.Chance(92) ? 0 : 1 + rng.Below(6)) : (q == 1 ? (rng.Chance(60) ? 0 : 1 + rng.Below(6)) : rng.Below(8));
        switch (pick) {
        case 0:
            break;
        case 1:
            s.use_null = true;
            break;
        case 2:
            s.w[0] = ((good.cap - 1) << 14) | 2;  // one byte short
            break;
        case 3:
            s.w[0] = (good.w[0] & ~0x3C0Fu) | 0x0C02;  // another static buffer id / type bits
            break;
        case 4:
            s.w[1] = 0;
            break;
        case 5:
            s.w[1] = 0x1234;  // unmapped
            break;
        case 6:
            s.w[0] = ((good.cap + 100) << 14) | 2;  // claims more than it has: the range is still what we write
            break;
        default:
            s.w[0] = rng.Next();
            s.w[1] = rng.Next();
            break;
        }
        return s;
    }
    u32 NowHash(const Reply& r) {
        u32 h = 0;
        for (int i = 0; i < 12; ++i) {
            h = h * 31 + r.w[i];
        }
        return h;
    }

    // ---- FKCL ------------------------------------------------------------------------------------------------------
    Reply Fk(Con& c, u32 id, int q) {
        switch (id) {
        case 0x01:
            return c.FkInit(q == 2 ? rng.Next() : 1 + rng.Below(20));
        case 0x11: {
            u16 nm[10];
            for (u16& u : nm) {
                u = static_cast<u16>(rng.Chance(30) ? 0 : 0x20 + rng.Below(0x5F));
            }
            return c.FkInitName(nm, q == 2 ? rng.Next() : rng.Below(18));
        }
        case 0x02:
            return c.FkFinalize();
        case 0x03:
            return c.FkEventDesc();
        case 0x04:
            return c.FkChannels();
        case 0x05: {
            static const u64 fl[] = {0, 0x000400010010C000ull, 0x000400010010C0A7ull, 0x00040001001C2100ull, 0x0004000100183600ull,
                                     0xFFFFFFFFFFFFFFFFull};
            const u64 f = q == 2 ? ((static_cast<u64>(rng.Next()) << 32) | rng.Next()) : fl[rng.Below(6)];
            return c.FkScan(f, q == 2 ? rng.Next() : 0x0421);
        }
        case 0x06:
            return c.FkStopScan();
        case 0x07: {
            const SbSel sel = PickSb(c, 2, q);
            return c.FkServerInfo(PickMac(), sel.ptr());
        }
        case 0x08: {
            const SbSel sel = PickSb(c, 1, q);
            return c.FkTitleInfo(PickMac(), sel.ptr());
        }
        case 0x09: {
            const SbSel sel = PickSb(c, 1, q);
            return c.FkInOrder(q == 2 ? rng.Next() : rng.Below(2), sel.ptr());
        }
        case 0x0A:
            return c.FkDelete(PickMac());
        case 0x0B: {
            const Mac m = PickMac();
            return c.FkJoin(m, q == 2 ? rng.Next() : 0x0010C000, q == 2 ? rng.Next() : 0x00040001);
        }
        case 0x0C:
            return c.FkStatus();
        case 0x0D: {
            const SbSel sel = PickSb(c, 3, q);
            const u32 n = q == 0 ? 1 + rng.Below(5) : (rng.Chance(30) ? 0xFFFF : rng.Next() & 0x1FFFF);
            req_n_ = n;
            return c.FkNodes(n, sel.ptr());
        }
        case 0x0E:
            req_node_ = PickNode(q);
            return c.FkNodeInfo(req_node_);
        case 0x0F:
            return c.FkPass();
        case 0x10:
            return c.FkStopSession();
        default: {  // an id the service does not have (and garbage words)
            u32 args[12];
            for (u32& a : args) {
                a = rng.Next();
            }
            return c.CallN(kFk, (id << 16) | (rng.Next() & 0xFFFF), args, 12, nullptr, nullptr);
        }
        }
    }

    // ---- SRVR ------------------------------------------------------------------------------------------------------
    Reply Sv(Con& c, u32 id, int q, bool* oor_expected) {
        *oor_expected = false;
        switch (id) {
        case 0x01:
            return c.SvInit(q == 2 ? rng.Next() : rng.Below(6), rng.Next(), rng.Next());
        case 0x0F: {
            u16 nm[10];
            for (u16& u : nm) {
                u = static_cast<u16>(rng.Chance(30) ? 0 : 0x20 + rng.Below(0x5F));
            }
            return c.SvInitName(q == 2 ? rng.Next() : rng.Below(6), rng.Next(), nm);
        }
        case 0x02:
            return c.SvFinalize();
        case 0x03:
            return c.SvState();
        case 0x04:
            return c.SvEventDesc();
        case 0x05: {
            static const u32 good[] = {0, 1, 6, 11};
            u32 ch = good[rng.Below(4)];
            if (q == 2 || rng.Chance(8)) {
                ch = rng.Next();
            } else if (rng.Chance(30)) {
                ch |= rng.Next() & 0xFFFFFF00u;  // rubbish in the high bytes: only the low byte is the channel
            }
            *oor_expected = ch != 0 && (ch & 0xFF) != 0 && (ch & 0xFF) != 1 && (ch & 0xFF) != 6 && (ch & 0xFF) != 11;
            if ((ch & 0xFF) == 0 || (ch & 0xFF) == 1 || (ch & 0xFF) == 6 || (ch & 0xFF) == 11) {
                *oor_expected = false;
            } else {
                *oor_expected = true;
            }
            return c.SvAccepting(rng.Next() & (rng.Chance(50) ? 0xFF : 1), ch);
        }
        case 0x06:
            return c.SvEnd();
        case 0x07:
            return c.SvDistribute();
        case 0x08: {
            u8 nine[9];
            for (u8& b : nine) {
                b = static_cast<u8>(rng.Next());
            }
            return c.SvPass(nine, rng.Next() & 0xFFFFFF);
        }
        case 0x09:
            req_node_ = PickNode(q);
            return c.SvAccept(req_node_);
        case 0x0A:
            req_node_ = PickNode(q);
            return c.SvDisconnect(req_node_);
        case 0x0B: {
            const u32 bytes = q == 0 ? (rng.Chance(80) ? 8 : 2 * rng.Below(5)) : rng.Below(80);
            u32 addr = c.ClientsAddr();
            const u32 bad = rng.Below(10);
            if (q != 0 && bad == 0) {
                addr = 0;
            } else if (q != 0 && bad == 1) {
                addr = 0x1234;
            }
            const u32 n = q == 0 ? 1 + rng.Below(5) : (rng.Chance(30) ? 0xFFFF : rng.Next() & 0x1FFFF);
            req_n_ = n;
            req_bytes_ = bytes;
            if (q != 0 && rng.Chance(10)) {
                return c.Call(kSv, 0x000B0042, {n, rng.Next(), addr});  // a descriptor that is not a mapped-write one
            }
            return c.SvClients(n, bytes, addr);
        }
        case 0x0C:
            req_node_ = PickNode(q);
            return c.SvClientInfo(req_node_);
        case 0x0D:
            req_node_ = PickNode(q);
            return c.SvClientState(req_node_);
        case 0x0E:
            return c.SvIsChild(rng.Next());
        case 0x10:
            return c.SvDup();
        default: {
            u32 args[12];
            for (u32& a : args) {
                a = rng.Next();
            }
            return c.CallN(kSv, (id << 16) | (rng.Next() & 0xFFFF), args, 12, nullptr, nullptr);
        }
        }
    }

    /// The next logical command of a game in the state its lobby is in (a game that follows the flow, with some noise).
    u32 NextFk(Con& c) {
        const u8 s = c.lobby.client_state();
        switch (s) {
        case kCliOff:
            return rng.Chance(60) ? 0x01 : 0x11;
        case kCliIdle:
            switch (rng.Below(6)) {
            case 0: return 0x04;
            case 1:
            case 2: return 0x05;
            case 3: return 0x0C;
            case 4: return 0x09;
            default: return 0x0B;
            }
        case kCliScanning:
            switch (rng.Below(8)) {
            case 0:
            case 1: return 0x09;
            case 2:
            case 3: return 0x07;
            case 4: return 0x08;
            case 5: return rng.Chance(30) ? 0x06 : 0x0A;
            case 6: return 0x0C;
            default: return rng.Chance(40) ? 0x0B : 0x09;
            }
        case kCliJoining:
            return rng.Chance(50) ? 0x0C : (rng.Chance(30) ? 0x10 : 0x0D);
        case kCliInSession:
        case kCliStarted:
            switch (rng.Below(9)) {
            case 0:
            case 1:
            case 2: return 0x0C;
            case 3:
            case 4: return 0x0D;
            case 5: return 0x0E;
            case 6: return 0x07;
            case 7: return rng.Chance(20) ? 0x10 : 0x0F;
            default: return rng.Chance(10) ? 0x02 : 0x0C;
            }
        case kCliComplete:
            switch (rng.Below(8)) {
            case 0: return 0x0F;
            case 1: return 0x0D;
            case 2: return 0x0E;
            case 3: return rng.Chance(60) ? 0x02 : 0x10;
            case 4: return 0x01;
            default: return 0x0C;
            }
        default:  // 0x80
            return rng.Chance(40) ? 0x0C : (rng.Chance(40) ? 0x10 : (rng.Chance(50) ? 0x02 : 0x01));
        }
    }
    u32 NextSv(Con& c) {
        const u8 s = c.lobby.server_state();
        switch (s) {
        case kSrvOff:
            return rng.Chance(50) ? 0x01 : (rng.Chance(50) ? 0x0F : (rng.Chance(50) ? 0x10 : 0x0E));
        case kSrvIdle:
            return rng.Chance(70) ? 0x05 : (rng.Chance(50) ? 0x03 : 0x06);
        case kSrvAccepting:
            if (w.air.Present(c.idx) >= 2 ? rng.Chance(12) : rng.Chance(1)) {
                return 0x07;  // the Start button: with somebody in the lobby, much more often
            }
            switch (rng.Below(10)) {
            case 0:
            case 1: return 0x03;
            case 2:
            case 3: return 0x0B;
            case 4: return 0x0C;
            case 5: return 0x0D;
            case 6: return rng.Chance(50) ? 0x09 : 0x0A;
            case 7: return rng.Chance(70) ? 0x07 : 0x06;
            default: return 0x0B;
            }
        case kSrvDistributing:
            switch (rng.Below(5)) {
            case 0: return 0x03;
            case 1: return 0x0B;
            case 2: return 0x08;
            case 3: return 0x0A;
            default: return 0x0D;
            }
        case kSrvPassphrase:
            return rng.Chance(70) ? 0x08 : (rng.Chance(50) ? 0x03 : 0x0B);
        default:  // 9
            return rng.Chance(40) ? 0x03 : (rng.Chance(30) ? 0x0B : (rng.Chance(50) ? 0x02 : 0x05));
        }
    }

    // ---- the invariants ----------------------------------------------------------------------------------------------
    void CheckStates() {
        std::set<u32> clients_of[8];
        for (std::size_t i = 0; i < w.c.size(); ++i) {
            Con& c = *w.c[i];
            const u8 cs = c.lobby.client_state();
            const u8 ss = c.lobby.server_state();
            CHECK(cs == 0 || cs == 1 || cs == 2 || cs == 3 || cs == 5 || cs == 6 || cs == 9 || cs == 0x80);
            CHECK(ss == 0 || ss == 1 || ss == 2 || ss == 6 || ss == 8 || ss == 9);
            const u32 shown = c.lobby.ClientStatusWord() >> 24;
            CHECK(shown == 0 || shown == 1 || shown == 2 || shown == 5 || shown == 6 || shown == 9 || shown == 0x80);  // never 3, never 7
            ++stats->cli_seen[shown];
            ++stats->srv_seen[ss];
            CHECK(c.mem.Intact());
        }
        for (int h = 0; h < FakeAir::kPorts; ++h) {
            const u32 present = static_cast<u32>(w.air.Present(h));
            CHECK(present <= 4);  // never more than 4 players in a lobby
            stats->max_present = std::max(stats->max_present, present);
        }
        CHECK(w.air.message_count() <= 96);
    }
    void CheckFkReply(Con& c, u32 id, const Reply& rp) {
        const u32 res = rp.res();
        if (rp.out == DlpOutcome::JoinPending) {
            CHECK_EQ(id, 0x0Bu);
            return;
        }
        if (id < 1 || id > 0x11) {
            CHECK_EQ(rp.hdr(), 0x00000040u);
            CHECK_EQ(res, R_BAD_HEADER);
            return;
        }
        CHECK(AllowedResult(res));
        if (res == 0) {
            CHECK_EQ(rp.hdr(), kFkReplyHeader[id]);
        } else {
            CHECK_EQ(rp.hdr(), (id << 16) | 0x40);
        }
        if (res == R_BAD_POINTER) {
            ++stats->bad_ptr_replies;
        }
        // the lists of the design: S commands always succeed, T commands never say "invalid state"
        switch (id) {
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x06: case 0x0C: case 0x0F: case 0x10: case 0x11:
            CHECK_EQ(res, 0u);
            break;
        case 0x0D:
            CHECK(res == 0 || res == R_BAD_POINTER);
            break;
        case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0E:
            CHECK(res == 0 || res == R_NO_DATA || res == R_BAD_POINTER);
            break;
        default:
            break;
        }
        if (id == 0x04 && res == 0) {
            CHECK_EQ(rp.w[2], 0x0421u);
        }
        if (id == 0x0C && res == 0) {
            const u32 st = rp.w[2] >> 24;
            CHECK(st == 0 || st == 1 || st == 2 || st == 5 || st == 6 || st == 9 || st == 0x80);
            CHECK_EQ(rp.w[3] | rp.w[4] | rp.w[5] | rp.w[6], 0u);
        }
        if (id == 0x0D && res == 0) {
            CHECK(rp.w[2] <= 4);  // never more than 4 players
            ++stats->deep_replies;
            stats->max_nodes_total = std::max(stats->max_nodes_total, rp.w[2]);
        }
        if (id == 0x0E && (req_node_ & 0xFFFF) > 0xFF) {
            CHECK_EQ(res, R_NO_DATA);  // 0x0102 is not node 2
        }
        if (id == 0x0E && res == 0) {
            CHECK(rp.w[2 + 9] >= 1 && rp.w[2 + 9] <= 4);
        }
        if ((id == 0x08 || id == 0x09) && res == 0) {
            const u32 own = c.idx == 4 ? kBr.unique_id : kTd.unique_id;  // console 4 plays another game
            CHECK_EQ(LoadLE32(c.mem.Find(c.TitleSb().addr, 1)), own);
        }
        if ((id == 0x07 || id == 0x08 || id == 0x09) && res == 0) {
            ++stats->deep_replies;
            const u32 addr = id == 0x07 ? c.ServerSb().addr : c.TitleSb().addr;
            const u8* p = c.mem.Find(addr, 1);
            if (id == 0x07) {
                CHECK(p[8] >= 2 && p[8] <= 4);  // max nodes
                CHECK(p[9] <= p[8]);            // joined never above max
                CHECK(p[7] <= 3);
            }
        }
    }
    /// Every region of the console was refilled before the call: only the region of THIS command may have changed, and only
    /// if the command succeeded (an error writes nothing).
    void CheckBuffers(Con& c, DlpKind kind, u32 id, const Reply& rp) {
        const u32 res = rp.out == DlpOutcome::JoinPending ? 1u : rp.res();
        const int wr = WritesRegion(kind, id);
        const u32 addr[5] = {0, c.TitleSb().addr, c.ServerSb().addr, c.NodesSb().addr, c.ClientsAddr()};
        for (int r = 1; r <= 4; ++r) {
            if (r != wr || res != 0) {
                CHECK(c.mem.Untouched(addr[r]));
            }
        }
        // a list of ids: never more of them than the game asked for, than the players there can be, or than its buffer holds
        if (res == 0 && (wr == 3 || wr == 4)) {
            u32 ids = std::min(rp.w[2], req_n_ & 0xFFFF);
            ids = wr == 3 ? std::min<u32>(ids, 4) : std::min<u32>(std::min<u32>(ids, 3), req_bytes_ / 2);
            const u8* p = c.mem.Find(addr[wr], 1);
            for (u32 i = 2 * ids; i < 8; ++i) {
                CHECK_EQ(p[i], Mem::kFill);
            }
        }
    }
    void CheckSvReply(Con& c, u32 id, const Reply& rp, bool oor_expected) {
        const u32 res = rp.res();
        if (id < 1 || id > 0x10) {
            CHECK_EQ(rp.hdr(), 0x00000040u);
            CHECK_EQ(res, R_BAD_HEADER);
            return;
        }
        CHECK(AllowedResult(res));
        if (id == 0x0C && res == R_NO_DATA && rp.hdr() == kSvReplyHeader[id]) {
            // node 1: the host's record with "no data" (the convention of Azahar)
            CHECK_EQ(rp.w[2 + 9], 1u);
        } else if (res == 0) {
            CHECK_EQ(rp.hdr(), kSvReplyHeader[id]);
        } else {
            CHECK_EQ(rp.hdr(), (id << 16) | 0x40);
        }
        switch (id) {
        case 0x01: case 0x02: case 0x03: case 0x04: case 0x06: case 0x0E: case 0x0F: case 0x10:
            CHECK_EQ(res, 0u);
            break;
        default:
            break;
        }
        if ((id == 0x09 || id == 0x0A || id == 0x0C || id == 0x0D) && (req_node_ & 0xFFFF) > 0xFF) {
            // a node id above 0xFF is no node (and 0x0102 is NOT node 2): unknown, or the state forbids the command
            CHECK(res == R_NO_DATA || res == R_INVALID_STATE);
            CHECK(res != R_INVALID_STATE || c.lobby.server_state() == kSrvOff);
        }
        if (id == 0x05 && oor_expected) {
            CHECK_EQ(res, R_OUT_OF_RANGE);  // the channel is the first thing checked, in every state
        }
        if (id == 0x03) {
            const u32 st = rp.w[2];
            CHECK(st == 0 || st == 1 || st == 2 || st == 6 || st == 8 || st == 9);
        }
        if (id == 0x0B && res == 0) {
            CHECK(rp.w[2] <= 3);  // never more than 3 clients
            ++stats->deep_replies;
            stats->max_clients_total = std::max(stats->max_clients_total, rp.w[2]);
            if (c.lobby.server_state() == kSrvComplete) {
                CHECK_EQ(rp.w[2], 0u);
            }
        }
        if (id == 0x0C && res == 0) {
            CHECK(rp.w[2 + 9] >= 2 && rp.w[2 + 9] <= 4);
        }
        if (id == 0x0D && res == 0) {
            const u32 ext = rp.w[2] >> 24;
            CHECK(ext == 0x40 || ext == 5 || ext == 6 || ext == 7 || ext == 9);
            CHECK(rp.w[2] % 65536 >= 2 && rp.w[2] % 65536 <= 4);
            CHECK_EQ(rp.w[3] | rp.w[4] | rp.w[5] | rp.w[6], 0u);
        }
    }

    void AirChaos() {
        switch (rng.Below(14)) {
        case 0: w.air.Drop(1 + static_cast<int>(rng.Below(4)), FakeAir::kAny, FakeAir::kAny, 1 + static_cast<int>(rng.Below(5))); break;
        case 1: w.air.Dup(1 + static_cast<int>(rng.Below(4)), 1 + static_cast<int>(rng.Below(3))); break;
        case 2: w.air.Delay(1 + static_cast<int>(rng.Below(4)), rng.Below(500)); break;
        case 3: w.air.Reorder(1 + static_cast<int>(rng.Below(4))); break;
        case 4: w.air.Partition(1 + static_cast<int>(rng.Below(4)), 1 + static_cast<int>(rng.Below(4)), rng.Below(2500)); break;
        case 5: w.air.ClearRules(); break;
        case 6: w.air.HideBeacon(static_cast<int>(rng.Below(static_cast<u32>(w.c.size()))), rng.Below(2500)); break;
        case 7: w.air.SetKickLag(rng.Chance(50) ? 0 : rng.Below(600)); break;
        case 8: w.c[rng.Below(static_cast<u32>(w.c.size()))]->net.FailSend(1 + static_cast<int>(rng.Below(4))); break;
        case 9: w.c[rng.Below(static_cast<u32>(w.c.size()))]->net.FailHostOpen(rng.Chance(30)); break;
        case 10: w.c[rng.Below(static_cast<u32>(w.c.size()))]->net.FailScanOpen(rng.Chance(30)); break;
        case 11: w.c[rng.Below(static_cast<u32>(w.c.size()))]->net.FailConnectBegin(rng.Chance(30)); break;
        case 12: w.c[rng.Below(static_cast<u32>(w.c.size()))]->net.ConnectDelay(rng.Chance(50) ? 0 : rng.Below(2500)); break;
        default:
            if (rng.Chance(20)) {
                w.air.Vanish(static_cast<int>(rng.Below(static_cast<u32>(w.c.size()))));
            } else {
                w.air.Leave(static_cast<int>(rng.Below(static_cast<u32>(w.c.size()))), 2 + static_cast<int>(rng.Below(3)));
            }
            break;
        }
    }

    /// A guided episode inside the soak: a lobby is played by the game models on random consoles up to a random point of its
    /// life, then abandoned where it stands. The random commands go on from there, in states a blind random walk almost never
    /// reaches (a client started, complete, a creator with clients in, ...). Before it, every console is finalised through
    /// the IPC and the air healed, so that the models' own expectations hold.
    void Episode(int kind) {
        for (std::size_t i = 0; i < w.c.size(); ++i) {
            Con& c = *w.c[i];
            c.FkFinalize();
            c.SvFinalize();
            c.FkStopSession();
            if (pending[i]) {
                bool done = false;
                c.Pending(&done);
                pending[i] = false;
            }
        }
        w.air.ClearRules();
        w.air.SetKickLag(0);
        for (std::size_t i = 0; i < w.c.size(); ++i) {
            w.c[i]->net.FailSend(0);
            w.c[i]->net.FailHostOpen(false);
            w.c[i]->net.FailScanOpen(false);
            w.c[i]->net.FailConnectBegin(false);
            w.c[i]->net.ConnectDelay(0);
        }
        w.air.Advance(3000);  // the 2 s timers of the former lives are over
        for (auto& x : w.c) {
            x->lobby.Pump();
        }
        std::vector<int> ids = {0, 1, 2, 3};
        for (std::size_t i = ids.size() - 1; i > 0; --i) {
            std::swap(ids[i], ids[rng.Below(static_cast<u32>(i + 1))]);
        }
        const int n = 1 + static_cast<int>(rng.Below(3));
        ids.resize(static_cast<std::size_t>(n) + 1);
        Room room(w, kTd, n, false, &ids);
        room.start_after = 5 + rng.Below(20);
        room.w.net_thread = rng.Chance(75);
        for (int i = 0; i < n; ++i) {
            Guest& g = *room.guests[static_cast<std::size_t>(i)];
            g.start_frame = rng.Below(60);
            g.choose_delay = rng.Below(10);
            g.c.variant = static_cast<int>(rng.Below(3));
            g.hold_final = kind == 6;
        }
        room.host.hold_final = kind == 4;
        room.host.c.variant = static_cast<int>(rng.Below(3));
        auto reached = [&]() -> bool {
            switch (kind) {
            case 0: return room.host.listed == static_cast<u32>(n) && room.frame > 40;
            case 1: return room.pressed;
            case 2: return room.pressed && room.host.last_state == 6;
            case 3: return room.host.last_state == 8;
            case 4: return room.host.last_state == 9;
            case 5:
                for (const auto& g : room.guests) {
                    if (g->last_state == 6) {
                        return true;
                    }
                }
                return false;
            case 6:
                for (const auto& g : room.guests) {
                    if (g->last_state == 9) {
                        return true;
                    }
                }
                return false;
            default: return room.frame >= 10u + static_cast<u32>(kind * 37) % 300u;
            }
        };
        room.RunUntil(reached, 700);
        for (std::size_t i = 0; i < w.c.size(); ++i) {
            pending[i] = w.c[i]->svc.join_pending();
        }
        ++episodes[kind < 8 ? kind : 7];
        CheckStates();
    }

    u64 Run(u32 steps) {
        for (u32 step = 0; step < steps; ++step) {
            if (step % 600 == 300) {
                Episode(static_cast<int>((step / 600) % 9));
            }
            // time
            const u32 t = rng.Below(1000);
            const u32 ms = t < 8 ? 500 + rng.Below(4500) : t < 200 ? 20 + rng.Below(130) : rng.Below(25);
            w.air.Advance(ms);
            if (rng.Chance(20)) {
                w.c[rng.Below(static_cast<u32>(w.c.size()))]->lobby.Pump();
            }
            if (step % 2500 == 0) {  // who plays which role for a while: one console at least in each, the BR console creates
                for (std::size_t i = 0; i < w.c.size(); ++i) {
                    sv_bias[i] = i == 4 || rng.Chance(35);
                }
                sv_bias[0] = true;
                sv_bias[1] = false;
            }
            if (rng.Below(400) == 0) {
                AirChaos();
            }
            if (step % 900 == 899) {  // the air heals: a soak in a world where everything always fails would prove little
                w.air.ClearRules();
                w.air.SetKickLag(0);
                for (std::size_t i = 0; i < w.c.size(); ++i) {
                    w.c[i]->net.FailSend(0);
                    w.c[i]->net.FailHostOpen(false);
                    w.c[i]->net.FailScanOpen(false);
                    w.c[i]->net.FailConnectBegin(false);
                    w.c[i]->net.ConnectDelay(0);
                }
            }
            const u32 k = rng.Below(static_cast<u32>(w.c.size()));
            Con& c = *w.c[k];
            c.variant = static_cast<int>(rng.Below(3));
            ++stats->commands;
            if (pending[k] && rng.Chance(35)) {
                bool done = false;
                const Reply r = c.Pending(&done);
                if (done) {
                    pending[k] = false;
                    CHECK_EQ(r.hdr(), 0x000B0040u);
                    CHECK(r.res() == 0 || r.res() == R_INVALID_STATE || r.res() == R_NOT_FOUND || r.res() == R_TIMEOUT);
                    stats->joins_ok += r.res() == 0 ? 1 : 0;
                    Mix(r.res());
                }
                CheckStates();
                continue;
            }
            const bool fk = rng.Chance(sv_bias[k] ? 20 : 85);
            const u32 mode = rng.Below(10);
            const int q = mode < 6 ? 0 : (mode < 9 ? 1 : 2);
            u32 id;
            if (mode < 5) {
                id = fk ? NextFk(c) : NextSv(c);
            } else if (mode < 8) {
                id = 1 + rng.Below(fk ? 0x11 : 0x10);
            } else {
                id = rng.Below(0x20);
            }
            Reply rp;
            bool oor = false;
            c.mem.Refill(c.TitleSb().addr);
            c.mem.Refill(c.ServerSb().addr);
            c.mem.Refill(c.NodesSb().addr);
            c.mem.Refill(c.ClientsAddr());
            if (fk) {
                rp = Fk(c, id, q);
                CheckFkReply(c, id, rp);
                if (rp.out == DlpOutcome::JoinPending) {
                    pending[k] = true;
                    ++stats->joins_pending;
                }
                CheckBuffers(c, kFk, id, rp);
            } else {
                rp = Sv(c, id, q, &oor);
                CheckSvReply(c, id, rp, oor);
                CheckBuffers(c, kSv, id, rp);
            }
            Mix(NowHash(rp));
            Mix(c.lobby.client_state() * 256u + c.lobby.server_state());
            CheckStates();
        }
        return hash;
    }
};

/// After the soak: every console is finalised through the IPC, the clock runs, everything must be at rest, and a complete
/// lobby must still work on the same consoles (nothing was left corrupted).
static void SoakHealth(Soak& s) {
    g_ctx = "after the soak";
    World& w = s.w;
    // the join that a thread of the game could still be waiting in is released by the Finalize
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        Con& c = *w.c[i];
        CHECK_EQ(c.FkFinalize().res(), 0u);
        CHECK_EQ(c.SvFinalize().res(), 0u);
        CHECK_EQ(c.FkStopSession().res(), 0u);
        if (s.pending[i]) {
            bool done = false;
            const Reply r = c.Pending(&done);
            CHECK(done);
            CHECK_EQ(r.res(), R_INVALID_STATE);
            s.pending[i] = false;
        }
    }
    w.air.ClearRules();
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        w.c[i]->net.FailSend(0);
        w.c[i]->net.FailHostOpen(false);
        w.c[i]->net.FailScanOpen(false);
        w.c[i]->net.FailConnectBegin(false);
        w.c[i]->net.ConnectDelay(0);
        w.c[i]->net.ConnectResult(0);
        w.c[i]->variant = 0;
    }
    w.air.SetKickLag(0);
    w.Idle(4000);
    for (std::size_t i = 0; i < w.c.size(); ++i) {
        CHECK(!w.c[i]->lobby.active());
        CHECK(!w.c[i]->svc.join_pending());
        CHECK_EQ(w.c[i]->FkStatus().w[2] >> 24, 1u);  // Finalize left every searcher "initialised"
        CHECK_EQ(w.c[i]->SvState().w[2], 0u);
    }
    for (int h = 0; h < FakeAir::kPorts; ++h) {
        CHECK_EQ(w.air.Present(h), 0);
    }
    // a complete TD lobby on the same four consoles
    g_ctx = "complete lobby after the soak";
    Room room(w, kTd, 3);
    room.start_after = 20;
    for (int i = 0; i < 3; ++i) {
        room.guests[static_cast<std::size_t>(i)]->choose_delay = 2;
    }
    CHECK(room.Run(3000));
    CHECK(room.host.phase == Host::Phase::Done);
    for (const auto& g : room.guests) {
        CHECK(g->phase == Guest::Phase::Done);
        CHECK_BYTES(g->pass, room.host.first_pass, 9);
    }
}

static void ScenarioSoak() {
    std::printf("6. soak: 100000 valid and invalid commands, fixed seed\n");
    g_ctx = "soak";
    SoakStats stats;
    Soak s(20261028, &stats);
    const u64 h = s.Run(100000);
    std::printf("  %u commands, hash %016llx; deepest: %u nodes, %u clients, %u present; %u joins begun, %u ok; %u bad-pointer replies;"
                " %u deep replies\n",
                stats.commands, static_cast<unsigned long long>(h), stats.max_nodes_total, stats.max_clients_total,
                stats.max_present, stats.joins_pending, stats.joins_ok, stats.bad_ptr_replies, stats.deep_replies);
    std::printf("  searcher states reached (shown): 1:%u 2:%u 5:%u 6:%u 9:%u 0x80:%u; creator: 1:%u 2:%u 6:%u 8:%u 9:%u\n",
                stats.cli_seen[1], stats.cli_seen[2], stats.cli_seen[5], stats.cli_seen[6], stats.cli_seen[9], stats.cli_seen[0x80],
                stats.srv_seen[1], stats.srv_seen[2], stats.srv_seen[6], stats.srv_seen[8], stats.srv_seen[9]);
    // the soak must have gone where the dangerous states are, or it proves nothing
    CHECK(stats.cli_seen[1] > 100 && stats.cli_seen[2] > 100 && stats.cli_seen[5] > 100 && stats.cli_seen[6] > 20);
    CHECK(stats.cli_seen[9] > 20 && stats.cli_seen[0x80] > 20);
    CHECK(stats.srv_seen[1] > 100 && stats.srv_seen[2] > 100 && stats.srv_seen[6] > 20 && stats.srv_seen[8] > 20 && stats.srv_seen[9] > 20);
    CHECK(stats.joins_ok > 10);
    CHECK(stats.max_nodes_total <= 4 && stats.max_clients_total <= 3 && stats.max_present <= 4);
    CHECK(stats.max_clients_total >= 2);
    CHECK(stats.bad_ptr_replies > 100);
    SoakHealth(s);
    // determinism: the same seed gives the same run
    g_ctx = "soak determinism";
    SoakStats s1;
    SoakStats s2;
    Soak a(77, &s1);
    Soak b(77, &s2);
    CHECK_EQ(a.Run(6000), b.Run(6000));
}

int main() {
    std::printf("sizeof(DlpLobby)=%zu DlpService=%zu Group=%zu NodeEntry=%zu pointer=%zu\n", sizeof(DlpLobby), sizeof(DlpService),
                sizeof(Group), sizeof(NodeEntry), sizeof(void*));
    ScenarioBattleRoyale();
    ScenarioTripleDeluxe();
    ScenarioUds();
    ScenarioLoss();
    ScenarioClosures();
    ScenarioSoak();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
