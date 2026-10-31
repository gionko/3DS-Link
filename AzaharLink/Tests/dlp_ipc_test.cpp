// Tests of the IPC front-end of the fake Download Play lobby (azl/dlp_ipc.hpp): the words of every dlp:FKCL / dlp:SRVR
// command in both modes (ProbeOnly, which must be DlpFkclHle word for word, and Lobby), the three records of the game
// byte for byte, the buffers of the game (null / wrong / too small pointers, canaries around every buffer), the widths of
// the words, the two-step StartFakeSession, the seven defects of Azahar that are not copied, complete rounds in the order
// Battle Royale and Triple Deluxe call the commands, and a fuzz of 100000 requests.
//
// Over the in-memory air of Tests/dlp_fakenet.hpp and the real DlpLobby. The expected values below are written by hand
// from SPEC-DLP.md and DESIGN-FASE1.md: headers, result codes, offsets and bytes are literals, not the constants of the
// code under test. The "game" is a memory of regions with canaries around them (Mem): the service gets addresses (u32), a
// resolver maps them, and a write outside a region shows up as a broken canary (and, for the regions of exact size, as an
// ASan report).
//
//   dlp_ipc_test      (needs nothing; a plain test like dlp_lobby_test, for Tests/run.sh)
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -IIncludes -ITests -pthread -fsanitize=address,undefined
//       Tests/dlp_ipc_test.cpp -o /tmp/dlp_ipc_test
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "azl/uds_engine.hpp"  // (beside the DLP headers: the plugin's uds_hle.cpp includes both)
#include "azl/dlp_fkcl_hle.hpp"
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
#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        if (!(cond)) {                                                                               \
            ++g_failures;                                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        const unsigned long long va_ = static_cast<unsigned long long>(a);                           \
        const unsigned long long vb_ = static_cast<unsigned long long>(b);                           \
        if (va_ != vb_) {                                                                            \
            ++g_failures;                                                                            \
            std::printf("  FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a, #b,   \
                        va_, vb_);                                                                   \
        }                                                                                            \
    } while (0)
/// n bytes equal (the first difference is reported).
#define CHECK_BYTES(actual, expected, n)                                                             \
    do {                                                                                             \
        ++g_checks;                                                                                  \
        const unsigned char* a_ = reinterpret_cast<const unsigned char*>(actual);                    \
        const unsigned char* e_ = reinterpret_cast<const unsigned char*>(expected);                  \
        std::size_t i_ = 0;                                                                          \
        while (i_ < static_cast<std::size_t>(n) && a_[i_] == e_[i_]) {                               \
            ++i_;                                                                                    \
        }                                                                                            \
        if (i_ != static_cast<std::size_t>(n)) {                                                     \
            ++g_failures;                                                                            \
            std::printf("  FAIL %s:%d: %s differs from %s at +0x%zx (0x%02x vs 0x%02x)\n", __FILE__, \
                        __LINE__, #actual, #expected, i_, a_[i_], e_[i_]);                           \
        }                                                                                            \
    } while (0)

// Results (written out, not the constants of the header).
static constexpr u32 R_INVALID_STATE = 0xE0A0AC01;
static constexpr u32 R_BUSY = 0xE0A0AC03;
static constexpr u32 R_NO_DATA = 0xC880AFEF;
static constexpr u32 R_NOT_FOUND = 0xC880AC05;
static constexpr u32 R_TIMEOUT = 0xC920AFFE;
static constexpr u32 R_OUT_OF_RANGE = 0xD8E0AFFD;
static constexpr u32 R_BAD_POINTER = 0xD8E0AFF6;
static constexpr u32 R_BAD_HEADER = 0xD9001830;

// Message types on the wire (written out).
static constexpr int T_START_ACK = 2;

static constexpr DlpKind kFk = DlpKind::Fkcl;
static constexpr DlpKind kSv = DlpKind::Srvr;

// The headers of the replies, by command id (SPEC-DLP.md sez. 2 and 3, column "Header risposta"; 0x000301C0 and 0x00040200
// are the design's choice for the two event descriptions). Index = id.
static const u32 kFkReplyHeader[0x12] = {0,          0x00010040, 0x00020040, 0x000301C0, 0x00040080, 0x00050040,
                                         0x00060040, 0x00070042, 0x00080042, 0x00090042, 0x000A0040, 0x000B0040,
                                         0x000C0180, 0x000D0082, 0x000E02C0, 0x000F0100, 0x00100040, 0x00110040};
static const u32 kSvReplyHeader[0x11] = {0,          0x00010040, 0x00020040, 0x00030080, 0x00040200, 0x00050040,
                                         0x00060040, 0x00070040, 0x00080040, 0x00090040, 0x000A0040, 0x000B0082,
                                         0x000C02C0, 0x000D0180, 0x000E0080, 0x000F0040, 0x00100080};

// ---------------------------------------------------------------------------------------------------------------------
// The memory of the game: regions of bytes at fake 32-bit addresses, with canaries around them.

class Mem {
public:
    static constexpr u8 kCanary = 0xA5;
    static constexpr u8 kFill = 0xCC;
    static constexpr u32 kPad = 32;
    u32 map_calls = 0;
    u32 last_map_size = 0;

    /// A region of `size` bytes. `misalign` bytes after an address aligned to 16 (UBSan: the service must never assume more
    /// than the byte alignment). `exact`: no canaries, exactly `size` bytes of heap (an overrun is an ASan report).
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
    static u8* Thunk(void* ctx, u32 addr, u32 size) {
        return static_cast<Mem*>(ctx)->Map(addr, size);
    }
    /// The resolver: the range must lie inside ONE region.
    u8* Map(u32 addr, u32 size) {
        ++map_calls;
        last_map_size = size;
        return Find(addr, size);
    }
    /// The host pointer of an address inside a region (no call counted), or nullptr.
    u8* Find(u32 addr, u32 size = 1) {
        for (Region& r : regions_) {
            if (addr >= r.addr && static_cast<u64>(addr) + size <= static_cast<u64>(r.addr) + r.size) {
                return r.data + (addr - r.addr);
            }
        }
        return nullptr;
    }
    /// Every canary of every region is intact.
    bool Intact() const {
        for (const Region& r : regions_) {
            if (r.exact) {
                continue;
            }
            const u8* lo = r.mem.data();
            const u8* hi = r.data + r.size;
            const u8* end = r.mem.data() + r.mem.size();
            for (const u8* p = lo; p < r.data; ++p) {
                if (*p != kCanary) {
                    return false;
                }
            }
            for (const u8* p = hi; p < end; ++p) {
                if (*p != kCanary) {
                    return false;
                }
            }
        }
        return true;
    }
    /// The region at `addr` still holds only the fill byte: nothing was written to it.
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
    u32 count() const {
        return static_cast<u32>(regions_.size());
    }
    u32 addr_of(u32 i) const {
        return regions_[i].addr;
    }
    u32 size_of(u32 i) const {
        return regions_[i].size;
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

// ---------------------------------------------------------------------------------------------------------------------
// One reply as the test sees it, and one console of the world.

struct Reply {
    u32 w[16];
    DlpOutcome out;
    bool shape_ok;  // nothing beyond the words the header declares was touched, and the guard words are intact
    u32 hdr() const { return w[0]; }
    u32 res() const { return w[1]; }
    u32 id() const { return w[0] >> 16; }
};

static constexpr u32 kCmdWords = 64;
static constexpr u32 kGuardWords = 8;
static constexpr u32 kGuard = 0xFEEDFACE;

static LobbyConfig MakeCfg(u32 tid_low, u64 seed, const char* name) {
    LobbyConfig c;
    std::memset(&c, 0, sizeof(c));
    c.game_tid = tid_low;
    c.friend_seed = seed;
    FakeAir::Name(c.name, name);
    return c;
}
static const char* const kNames[4] = {"Host", "Cli1", "Cli2", "Cli3"};

// MAC to the two request words, written out (bytes 0-3 little endian in the first, bytes 4-5 in the low half of the second).
static u32 MacW1(const Mac& m) {
    return static_cast<u32>(m[0]) | (static_cast<u32>(m[1]) << 8) | (static_cast<u32>(m[2]) << 16) | (static_cast<u32>(m[3]) << 24);
}
static u32 MacW2(const Mac& m) {
    return static_cast<u32>(m[4]) | (static_cast<u32>(m[5]) << 8);
}
/// 10 UTF-16 units to 5 request words (the first unit in the low half).
static void NameWords(const u16* name, u32 w[5]) {
    for (int i = 0; i < 5; ++i) {
        w[i] = static_cast<u32>(name[2 * i]) | (static_cast<u32>(name[2 * i + 1]) << 16);
    }
}
/// 9 bytes to 3 request words (memory order); the 3 bytes above the ninth are `high` (whatever the game had there).
static void PassWords(const u8* b, u32 w[3], u32 high = 0) {
    w[0] = static_cast<u32>(b[0]) | (static_cast<u32>(b[1]) << 8) | (static_cast<u32>(b[2]) << 16) | (static_cast<u32>(b[3]) << 24);
    w[1] = static_cast<u32>(b[4]) | (static_cast<u32>(b[5]) << 8) | (static_cast<u32>(b[6]) << 16) | (static_cast<u32>(b[7]) << 24);
    w[2] = static_cast<u32>(b[8]) | (high << 8);
}

struct Console {
    FakeLobbyNet net;
    DlpLobby lobby;
    DlpService svc;
    Mem mem;
    Console(FakeAir& air, int i, const LobbyConfig& cfg) : net(air, i), lobby(&net, cfg) { svc.SetLobby(&lobby); }

    Sb NewSb(u32 cap, u32 misalign = 0, bool exact = false) {
        Sb s;
        s.addr = mem.Alloc(cap, misalign, exact);
        s.cap = cap;
        s.w[0] = (cap << 14) | 2;
        s.w[1] = s.addr;
        return s;
    }
    const u8* Data(const Sb& s) { return mem.Find(s.addr, s.cap == 0 ? 0 : 1); }

    /// One request through Dispatch. The request words after the arguments are garbage (a service that read one it should not
    /// would show), the 8 words after the 64 are guards. The reply is copied out; the shape and the canaries are checked.
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
        rp.out = svc.Dispatch(kind, buf, sbuf, r);
        std::memcpy(rp.w, buf, sizeof(rp.w));
        rp.shape_ok = true;
        u32 extent = 0;  // words of the command buffer the reply may have written
        if (rp.out == DlpOutcome::Done) {
            extent = 1 + ((buf[0] >> 6) & 0x3F) + (buf[0] & 0x3F);
            if (extent > kCmdWords) {
                extent = kCmdWords;
            }
        }
        for (u32 i = extent; i < kCmdWords; ++i) {
            if (buf[i] != before[i]) {
                rp.shape_ok = false;
            }
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            if (buf[kCmdWords + i] != kGuard) {
                rp.shape_ok = false;
            }
        }
        CHECK(rp.shape_ok);
        CHECK(mem.Intact());
        return rp;
    }
    /// PollPending with a garbage command buffer; on true the reply is copied out (words 0..15).
    Reply Pending(bool* done) {
        u32 buf[kCmdWords + kGuardWords];
        for (u32 i = 0; i < kCmdWords; ++i) {
            buf[i] = 0xC0DE0000u + i;
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            buf[kCmdWords + i] = kGuard;
        }
        Reply rp;
        *done = svc.PollPending(buf);
        std::memcpy(rp.w, buf, sizeof(rp.w));
        rp.out = DlpOutcome::Done;
        rp.shape_ok = true;
        for (u32 i = 2; i < kCmdWords; ++i) {
            if (*done && buf[i] != 0xC0DE0000u + i) {
                rp.shape_ok = false;
            }
        }
        for (u32 i = 0; i < kGuardWords; ++i) {
            if (buf[kCmdWords + i] != kGuard) {
                rp.shape_ok = false;
            }
        }
        if (!*done) {  // a join still running leaves the buffer alone
            for (u32 i = 0; i < kCmdWords; ++i) {
                if (buf[i] != 0xC0DE0000u + i) {
                    rp.shape_ok = false;
                }
            }
        }
        CHECK(rp.shape_ok);
        return rp;
    }

    // ---- dlp:FKCL, as the games send it (request headers of SPEC-DLP.md sez. 2) -----------------------------------
    Reply FkInit(u32 beacons = 16) { return Call(kFk, 0x00010083, {0x30000, beacons, 0x04000000, 0x1111, 0x2222}); }
    Reply FkInitName(const u16* name, u32 beacons = 16) {
        u32 n[5];
        NameWords(name, n);
        return Call(kFk, 0x00110203, {0x30000, beacons, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x3333, 0x4444});
    }
    Reply FkFinalize() { return Call(kFk, 0x00020000, {}); }
    Reply FkEventDesc() { return Call(kFk, 0x00030000, {}); }
    Reply FkChannels() { return Call(kFk, 0x00040000, {}); }
    Reply FkScan(u64 filter, u32 channels = 0x0421) {
        return Call(kFk, 0x00050180, {channels, static_cast<u32>(filter), static_cast<u32>(filter >> 32), 0, 0, 0});
    }
    Reply FkStopScan() { return Call(kFk, 0x00060000, {}); }
    Reply FkServerInfo(const Mac& m, const Sb& s) { return Call(kFk, 0x00070080, {MacW1(m), MacW2(m)}, s.w); }
    Reply FkTitleInfo(const Mac& m, const Sb& s) { return Call(kFk, 0x00080100, {MacW1(m), MacW2(m), 0x0010C000, 0x00040001}, s.w); }
    Reply FkInOrder(u32 restart, const Sb& s) { return Call(kFk, 0x00090040, {restart}, s.w); }
    Reply FkDelete(const Mac& m) { return Call(kFk, 0x000A0080, {MacW1(m), MacW2(m)}); }
    Reply FkJoin(const Mac& m) { return Call(kFk, 0x000B0100, {MacW1(m), MacW2(m), 0x0010C000, 0x00040001}); }
    Reply FkStatus() { return Call(kFk, 0x000C0000, {}); }
    Reply FkNodes(u32 n, const Sb& s) { return Call(kFk, 0x000D0040, {n}, s.w); }
    Reply FkNodeInfo(u32 node) { return Call(kFk, 0x000E0040, {node}); }
    Reply FkPass() { return Call(kFk, 0x000F0000, {}); }
    Reply FkStopSession() { return Call(kFk, 0x00100000, {}); }

    // ---- dlp:SRVR (SPEC-DLP.md sez. 3) -----------------------------------------------------------------------------
    Reply SvInit(u32 max_clients, u32 child_index = 0, u32 pid = 0xE) {
        return Call(kSv, 0x00010183, {0x85000, max_clients, pid, child_index, 0x40000, 2, 0x04000000, 0x5555, 0x6666});
    }
    Reply SvInitName(u32 max_clients, u32 child_index, const u16* name) {
        u32 n[5];
        NameWords(name, n);
        return Call(kSv, 0x000F0303, {0x85000, max_clients, 0xE, child_index, 0x40000, 2, n[0], n[1], n[2], n[3], n[4], 0,
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
    /// GetConnectingClients with a mapped-write buffer of `bytes` at `addr`.
    Reply SvClients(u32 n, u32 bytes, u32 addr) { return Call(kSv, 0x000B0042, {n, (bytes << 4) | 0xC, addr}); }
    Reply SvClientInfo(u32 node) { return Call(kSv, 0x000C0040, {node}); }
    Reply SvClientState(u32 node) { return Call(kSv, 0x000D0040, {node}); }
    Reply SvIsChild(u32 pid) { return Call(kSv, 0x000E0040, {pid}); }
    Reply SvDup() { return Call(kSv, 0x00100000, {}); }

    // ---- what a game does with the answers ------------------------------------------------------------------------
    /// The ids of GetConnectingClients(n) in a buffer of `bytes` bytes.
    std::vector<u16> Clients(u32 n, u32 bytes, u32* total = nullptr) {
        const u32 addr = mem.Alloc(bytes == 0 ? 1 : bytes);
        const Reply r = SvClients(n, bytes, addr);
        std::vector<u16> ids;
        if (r.res() != 0) {
            return ids;
        }
        if (total != nullptr) {
            *total = r.w[2];
        }
        const u8* p = mem.Find(addr, 1);
        u32 k = r.w[2] < n ? r.w[2] : n;
        if (k > bytes / 2) {
            k = bytes / 2;
        }
        for (u32 i = 0; i < k; ++i) {
            ids.push_back(LoadLE16(p + 2 * i));
        }
        return ids;
    }
};

struct World {
    FakeAir air;
    std::vector<std::unique_ptr<Console>> c;
    explicit World(int n = 4, u64 start = 100000, u32 tid_low = 0x0010C000) : air(start) {
        for (int i = 0; i < n; ++i) {
            c.push_back(std::make_unique<Console>(air, i, MakeCfg(tid_low, 0x1000u + static_cast<u32>(i), kNames[i])));
        }
    }
    Console& operator[](int i) { return *c[static_cast<std::size_t>(i)]; }
    /// The clock in steps, every lobby pumped after each (what the plugin's network thread does).
    void Run(u32 ms, u32 step = 20) {
        for (u32 t = 0; t < ms; t += step) {
            air.Advance(step);
            for (auto& x : c) {
                x->lobby.Pump();
            }
        }
    }
    template <typename F>
    bool RunUntil(F cond, u32 max_ms, u32 step = 20) {
        for (u32 t = 0; t <= max_ms; t += step) {
            if (cond()) {
                return true;
            }
            air.Advance(step);
            for (auto& x : c) {
                x->lobby.Pump();
            }
        }
        return cond();
    }
};

static const Mac& MacOf(Console& c) {
    return c.net.mac();
}
static const u64 kFilterTd = 0x000400010010C000ull;  // Triple Deluxe's scan filter (child index 0)
static const u64 kFilterBr = 0x00040001001C2100ull;  // Battle Royale's

/// The byte [3] of GetMyStatus' first word.
static u32 CliState(Console& c) {
    return c.FkStatus().w[2] >> 24;
}
static u32 SrvState(Console& c) {
    return c.SvState().w[2];
}

/// The state of the game's aging arithmetic (Triple Deluxe): ticks * 3.7296 ns against 5000 ms.
static bool GameDropsEntry(u64 now_ticks, u64 tick) {
    const u64 diff = now_ticks >= tick ? now_ticks - tick : 0;
    const u64 ns = diff * 37296 / 10000;
    return ns >= 5000ull * 1000000ull;
}


// =====================================================================================================================
// Sizes, limits, helpers.

static void TestSizesAndHelpers() {
    std::printf("  sizeof(DlpService)=%zu  IpcResolver=%zu  DlpLobby=%zu  pointer=%zu\n", sizeof(DlpService), sizeof(IpcResolver),
                sizeof(DlpLobby), sizeof(void*));
    CHECK(sizeof(DlpService) <= 64);
    // Not a member: the service holds no record and no buffer, only a pointer, the counters and a flag.
    CHECK(sizeof(DlpService) <= sizeof(void*) + 8 * sizeof(u32) + 8);
    // the helpers of the IPC words, against the literals of the games (TD: 0x00A60002 = 0x298 bytes, 0x04EA0002 = 0x13A8)
    CHECK_EQ(StaticBufferDesc(0x298, 0), 0x00A60002u);
    CHECK_EQ(StaticBufferDesc(0x13A8, 0), 0x04EA0002u);
    CHECK_EQ(Header(0x0B, 2, 2), 0x000B0082u);
    CHECK_EQ(Header(0x01, 1, 0), 0x00010040u);
    CHECK_EQ(MappedWriteDesc(8), 0x8Cu);
    // the records and the limits
    CHECK_EQ(kTitleInfoSize, 0x13A8u);
    CHECK_EQ(kServerInfoSize, 0x298u);
    CHECK_EQ(kDlpNodeInfoSize, 0x28u);
    CHECK_EQ(kMaxPlayers, 4u);
    CHECK_EQ(kMaxClients, 3u);
    CHECK_EQ(kMaxGroups, 4u);
    // the defaults
    DlpService s;
    CHECK(s.mode() == DlpMode::ProbeOnly);
    CHECK(!s.join_pending());
    CHECK_EQ(s.commands() + s.initializes() + s.finalizes() + s.refused() + s.unknown_commands() + s.bad_pointer() + s.last_id() + s.last_result(), 0u);
}

// =====================================================================================================================
// ProbeOnly = DlpFkclHle, word for word and counter for counter.

static u32 g_rng_state;
static u32 Rnd() {
    g_rng_state = g_rng_state * 1664525u + 1013904223u;
    u32 x = g_rng_state;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return x;
}

static void TestProbeOnlyIsDlpFkclHle() {
    DlpService s;
    DlpFkclHle h;
    Mem mem;
    const Sb sb = {{(0x298u << 14) | 2, mem.Alloc(0x298)}, 0, 0x298};
    const IpcResolver res = mem.Resolver();
    g_rng_state = 0x5EEDC0DEu;
    u32 n = 0;
    u32 unknown = 0;
    u32 diffs = 0;
    auto one = [&](u32 header, bool with_buffers) {
        u32 a[64];
        u32 b[64];
        for (int i = 0; i < 64; ++i) {
            a[i] = b[i] = Rnd();
        }
        a[0] = b[0] = header;
        const DlpOutcome out = s.Dispatch(kFk, a, with_buffers ? sb.w : nullptr, with_buffers ? res : IpcResolver{nullptr, nullptr});
        h.Dispatch(b);
        ++n;
        const u32 id = header >> 16;
        if (id == 0 || id > 0x11) {
            ++unknown;
        }
        if (out != DlpOutcome::Done || std::memcmp(a, b, sizeof(a)) != 0 || s.initializes() != h.initializes() ||
            s.finalizes() != h.finalizes() || s.refused() != h.refused()) {
            ++diffs;
        }
    };
    // every id 0..0x40 with the headers the games send (and others), with and without buffers
    const u32 lows[] = {0x40, 0x00, 0x83, 0x203, 0x180, 0x082, 0xFFFF, 0x01, 0x3F3F};
    for (u32 id = 0; id < 0x40; ++id) {
        for (u32 lo : lows) {
            one((id << 16) | lo, (id & 1) != 0);
        }
    }
    // random headers (the whole range of ids) and random garbage words
    for (int i = 0; i < 60000; ++i) {
        one(Rnd(), (i & 3) == 0);
    }
    CHECK_EQ(diffs, 0u);
    CHECK_EQ(s.commands(), n);
    CHECK_EQ(s.unknown_commands(), unknown);
    CHECK(s.unknown_commands() <= s.refused());
    CHECK_EQ(s.bad_pointer(), 0u);
    CHECK_EQ(mem.map_calls, 0u);  // a probe never looks at a buffer of the game
    CHECK(s.mode() == DlpMode::ProbeOnly);
    // the same in a fixed sequence, counter by counter, with the words of the three answers written out
    DlpService t;
    DlpFkclHle g;
    u32 w[64];
    std::memset(w, 0, sizeof(w));
    w[0] = 0x00110203;  // InitializeWithName
    CHECK(t.Dispatch(kFk, w, nullptr, IpcResolver{nullptr, nullptr}) == DlpOutcome::Done);
    CHECK_EQ(w[0], 0x00110040u);
    CHECK_EQ(w[1], 0u);
    w[0] = 0x000C0000;  // GetMyStatus: invalid state
    t.Dispatch(kFk, w, nullptr, IpcResolver{nullptr, nullptr});
    CHECK_EQ(w[0], 0x000C0040u);
    CHECK_EQ(w[1], 0xE0A0AC01u);
    w[0] = 0x00120000;  // an id the service does not have
    t.Dispatch(kFk, w, nullptr, IpcResolver{nullptr, nullptr});
    CHECK_EQ(w[0], 0x00000040u);
    CHECK_EQ(w[1], 0xD9001830u);
    w[0] = 0x00020000;  // Finalize
    t.Dispatch(kFk, w, nullptr, IpcResolver{nullptr, nullptr});
    CHECK_EQ(w[0], 0x00020040u);
    CHECK_EQ(t.initializes(), 1u);
    CHECK_EQ(t.finalizes(), 1u);
    CHECK_EQ(t.refused(), 2u);
    CHECK_EQ(t.unknown_commands(), 1u);
    CHECK_EQ(t.commands(), 4u);
    CHECK_EQ(t.last_id(), 2u);
    CHECK_EQ(t.last_result(), 0u);
    (void)g;
}

static void TestProbeOnlyServer() {
    // ProbeOnly on dlp:SRVR: IsChild answers "no" with success, everything else is "invalid state"; nothing else is touched
    DlpService s;
    u32 expect_refused = 0;
    u32 expect_unknown = 0;
    g_rng_state = 0xABCD1234u;
    for (u32 id = 0; id < 0x30; ++id) {
        for (u32 lo : {0x40u, 0x0000u, 0x0183u, 0x0303u, 0xFFFFu}) {
            u32 a[64];
            u32 b[64];
            for (int i = 0; i < 64; ++i) {
                a[i] = b[i] = Rnd();
            }
            a[0] = b[0] = (id << 16) | lo;
            s.Dispatch(kSv, a, nullptr, IpcResolver{nullptr, nullptr});
            if (id == 0x0E) {
                b[0] = 0x000E0080;
                b[1] = 0;
                b[2] = 0;
            } else if (id >= 1 && id <= 0x10) {
                b[0] = (id << 16) | 0x40;
                b[1] = 0xE0A0AC01;
                ++expect_refused;
            } else {
                b[0] = 0x00000040;
                b[1] = 0xD9001830;
                ++expect_refused;
                ++expect_unknown;
            }
            CHECK_BYTES(a, b, sizeof(a));
        }
    }
    CHECK_EQ(s.refused(), expect_refused);
    CHECK_EQ(s.unknown_commands(), expect_unknown);
    CHECK_EQ(s.initializes(), 0u);  // Initialize of the server is refused in this mode: not an initialisation
    CHECK_EQ(s.finalizes(), 0u);
    CHECK_EQ(s.commands(), 0x30u * 5u);
}

// =====================================================================================================================
// Ids the service does not have, in Lobby mode, for both services: the answer of DlpFkclHle.

static void TestUnknownIdsInLobbyMode() {
    World w(1);
    Console& c = w[0];
    u32 unknown = 0;
    for (u32 id = 0; id < 0x30; ++id) {
        const Reply f = c.Call(kFk, (id << 16) | 0x40, {});
        const Reply v = c.Call(kSv, (id << 16) | 0x40, {});
        if (id == 0 || id > 0x11) {
            CHECK_EQ(f.hdr(), 0x40u);
            CHECK_EQ(f.res(), 0xD9001830u);
            ++unknown;
        } else {
            CHECK(f.hdr() == kFkReplyHeader[id] || f.hdr() == ((id << 16) | 0x40));
            CHECK(f.res() != 0xD9001830u);
        }
        if (id == 0 || id > 0x10) {
            CHECK_EQ(v.hdr(), 0x40u);
            CHECK_EQ(v.res(), 0xD9001830u);
            ++unknown;
        } else {
            CHECK(v.res() != 0xD9001830u);
        }
    }
    CHECK_EQ(c.svc.unknown_commands(), unknown);
    CHECK(c.svc.unknown_commands() <= c.svc.refused());
    CHECK_EQ(c.svc.commands(), 0x60u);
    // a request whose header does not match its id is NOT refused: the reply follows the id
    Reply r = c.Call(kFk, 0x00040000 | 0x3F3F, {});  // GetChannels with a header that declares other word counts
    CHECK_EQ(r.hdr(), 0x00040080u);
    CHECK_EQ(r.w[2], 0x0421u);
    r = c.Call(kFk, 0x00040FFF, {});
    CHECK_EQ(r.hdr(), 0x00040080u);
    World w2(1);  // (a creator never initialised: state 0)
    r = w2[0].Call(kSv, 0x00030FFF, {});
    CHECK_EQ(r.hdr(), 0x00030080u);
    CHECK_EQ(r.w[2], 0u);
}

// =====================================================================================================================
// The handles of Initialize are not read or touched; nothing in the shared memory is.

static void TestHandlesIgnored() {
    World w(1);
    Console& c = w[0];
    const u32 hs[][2] = {{0, 0}, {0xFFFFFFFF, 0xFFFFFFFF}, {0x1234, 0xBEEF}, {0x04000000, 0}};
    for (const auto& h : hs) {
        Reply r = c.Call(kFk, 0x00010083, {0x30000, 16, 0x04000000, h[0], h[1]});
        CHECK_EQ(r.hdr(), 0x00010040u);
        CHECK_EQ(r.res(), 0u);
        r = c.Call(kSv, 0x00010183, {0x85000, 3, 0xE, 0, 0x40000, 2, 0x04000000, h[0], h[1]});
        CHECK_EQ(r.hdr(), 0x00010040u);
        CHECK_EQ(r.res(), 0u);
        r = c.Call(kFk, 0x00110203, {0x30000, 16, 'a', 0, 0, 0, 0, 0, h[0], h[0], h[1]});
        CHECK_EQ(r.res(), 0u);
        r = c.Call(kSv, 0x000F0303, {0x85000, 3, 0xE, 0, 0x40000, 2, 'a', 0, 0, 0, 0, 0, h[0], h[0], h[1]});
        CHECK_EQ(r.res(), 0u);
    }
    CHECK_EQ(c.mem.map_calls, 0u);  // no handle, no buffer: the resolver is never asked
    CHECK_EQ(c.svc.initializes(), 16u);
    // the unread words of Initialize (shared memory size, process id, block size and count) do not matter either
    const Reply r = c.Call(kSv, 0x00010183, {0xFFFFFFFF, 3, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0xFFFFFFFF});
    CHECK_EQ(r.res(), 0u);
}

// =====================================================================================================================
// The books: commands, initializes, finalizes, refused, unknown, bad pointers, last id and result.

static void TestCounters() {
    World w(1);
    Console& c = w[0];
    DlpService& s = c.svc;
    c.FkInit();
    CHECK_EQ(s.commands(), 1u);
    CHECK_EQ(s.initializes(), 1u);
    CHECK_EQ(s.last_id(), 1u);
    CHECK_EQ(s.last_result(), 0u);
    c.FkStatus();
    CHECK_EQ(s.last_id(), 0x0Cu);
    const Sb sb = c.NewSb(0x298);
    const Mac unknown = {1, 2, 3, 4, 5, 6};
    c.FkServerInfo(unknown, sb);  // NoData: an answer, not a refusal
    CHECK_EQ(s.last_result(), 0xC880AFEFu);
    CHECK_EQ(s.refused(), 0u);
    c.FkJoin(unknown);  // NoData
    CHECK_EQ(s.refused(), 0u);
    c.FkScan(kFilterTd);
    CHECK_EQ(s.refused(), 0u);
    c.FkScan(kFilterTd);  // invalid state
    CHECK_EQ(s.refused(), 1u);
    CHECK_EQ(s.last_result(), 0xE0A0AC01u);
    c.Call(kFk, 0x00200000, {});  // an id the service does not have
    CHECK_EQ(s.refused(), 2u);
    CHECK_EQ(s.unknown_commands(), 1u);
    CHECK_EQ(s.last_id(), 0x20u);  // the id of the request (the answer carries 0)
    CHECK_EQ(s.last_result(), 0xD9001830u);
    u32 bad[2] = {0, 0};  // a pair of static buffers with no address
    c.Call(kFk, 0x00070080, {1, 2}, bad);
    CHECK_EQ(s.bad_pointer(), 1u);
    CHECK_EQ(s.refused(), 3u);
    c.FkFinalize();
    CHECK_EQ(s.finalizes(), 1u);
    CHECK_EQ(s.initializes(), 1u);
    c.SvInit(3);
    c.SvInitName(3, 0, reinterpret_cast<const u16*>(u"abcdefghij"));
    c.SvFinalize();
    CHECK_EQ(s.initializes(), 3u);
    CHECK_EQ(s.finalizes(), 2u);
    CHECK_EQ(s.commands(), 12u);
    CHECK_EQ(s.refused(), 3u);
    // the books do not reach into ProbeOnly after SetLobby(nullptr): the mode changes, the counters keep counting
    s.SetLobby(nullptr);
    CHECK(s.mode() == DlpMode::ProbeOnly);
    c.FkStatus();  // now "invalid state" like DlpFkclHle
    CHECK_EQ(s.last_result(), 0xE0A0AC01u);
    CHECK_EQ(s.refused(), 4u);
    s.SetLobby(&c.lobby);
    CHECK(s.mode() == DlpMode::Lobby);
}


// =====================================================================================================================
// The matrix: every command x every state of its role x both services (ProbeOnly: see above). Each cell is a fresh world
// brought to the state through the IPC itself; the expected result is a literal of the table below (DESIGN sez. 4.5).

static const u8 kNine[9] = {'1', '2', '3', '4', 'a', 'b', 'c', 'd', 0};  // the game's "%08x" and its NUL

static void HostUp(World& w, u32 max_clients = 3) {
    Console& h = w[0];
    h.SvInit(max_clients, 0);
    h.SvDup();
    h.SvAccepting(0, 6);
    w.Run(40);
}

/// Console 1 joins the lobby of console 0 through its FKCL service (JoinPending then PollPending until the end).
static bool JoinThroughIpc(World& w, int client, u32 max_ms = 4000) {
    Console& c = w[client];
    const Reply j = c.FkJoin(MacOf(w[0]));
    if (j.out != DlpOutcome::JoinPending) {
        return false;
    }
    for (u32 t = 0; t <= max_ms; t += 20) {
        bool done = false;
        const Reply r = c.Pending(&done);
        if (done) {
            return r.res() == 0;
        }
        w.Run(20);
    }
    return false;
}

// Searcher states (the columns): 0 off, 1 idle, 2 scanning, 3 joining, 4 in session (5), 5 started (6), 6 complete (9), 7 lost (0x80).
static const u32 kCliShown[8] = {0, 1, 2, 1, 5, 6, 9, 0x80};

/// Console 1 in the state `st`, console 0 the host that makes it possible.
static void MakeClientState(World& w, int st) {
    Console& c = w[1];
    if (st == 0) {
        return;
    }
    if (st >= 2) {
        HostUp(w);
    }
    c.FkInit();
    if (st == 1) {
        return;
    }
    c.FkScan(kFilterTd);
    w.Run(60);
    if (st == 2) {
        return;
    }
    c.FkStopScan();
    if (st == 3) {
        c.net.ConnectDelay(100000);
        const Reply j = c.FkJoin(MacOf(w[0]));
        CHECK(j.out == DlpOutcome::JoinPending);
        return;
    }
    CHECK(JoinThroughIpc(w, 1));
    if (st == 4) {
        return;
    }
    if (st == 7) {
        w.air.Vanish(0);
        w.Run(2100);
        return;
    }
    w[0].SvDistribute();
    w.Run(100);
    if (st == 5) {
        return;
    }
    CHECK(w.RunUntil([&] { return w[0].lobby.server_state() == 8; }, 2000));
    w[0].SvPass(kNine);
    w.Run(200);
}

/// One FKCL command with the usual arguments (a MAC and a node no lobby knows).
static Reply FkCell(Console& c, u32 id) {
    const Mac unknown = {1, 2, 3, 4, 5, 6};
    switch (id) {
    case 0x01: return c.FkInit();
    case 0x02: return c.FkFinalize();
    case 0x03: return c.FkEventDesc();
    case 0x04: return c.FkChannels();
    case 0x05: return c.FkScan(kFilterTd);
    case 0x06: return c.FkStopScan();
    case 0x07: return c.FkServerInfo(unknown, c.NewSb(0x298));
    case 0x08: return c.FkTitleInfo(unknown, c.NewSb(0x13A8));
    case 0x09: return c.FkInOrder(1, c.NewSb(0x13A8));
    case 0x0A: return c.FkDelete(unknown);
    case 0x0B: return c.FkJoin(unknown);
    case 0x0C: return c.FkStatus();
    case 0x0D: return c.FkNodes(4, c.NewSb(8));
    case 0x0E: return c.FkNodeInfo(1);
    case 0x0F: return c.FkPass();
    case 0x10: return c.FkStopSession();
    default: {
        const u16 name[10] = {'Z', 'e', 'd', 0, 0, 0, 0, 0, 0, 0};
        return c.FkInitName(name);
    }
    }
}

// Expected results, by [id][searcher state]. Z = success, I = invalid state, N = no data.
static constexpr u32 Z = 0u;
static constexpr u32 I = R_INVALID_STATE;
static constexpr u32 N = R_NO_DATA;
static const u32 kFkMatrix[0x12][8] = {
    {Z, Z, Z, Z, Z, Z, Z, Z},  // (id 0 is not a command)
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 01 Initialize
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 02 Finalize
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 03 GetEventDesc
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 04 GetChannels
    {I, Z, I, I, I, I, I, I},  // 05 StartScan: from "idle" only
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 06 StopScan
    {N, N, N, N, N, N, N, N},  // 07 GetServerInfo (a MAC no lobby has)
    {N, N, N, N, N, N, N, N},  // 08 GetTitleInfo
    {N, N, Z, Z, N, N, N, N},  // 09 GetTitleInfoInOrder: the table of the scan is there in 2 and while joining
    {N, N, N, N, N, N, N, N},  // 0A DeleteScanInfo
    {I, N, I, I, I, I, I, I},  // 0B StartFakeSession: from "idle" only, and the MAC must be in the table
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 0C GetMyStatus
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 0D GetConnectingNodes
    {N, N, N, N, Z, Z, Z, N},  // 0E GetNodeInfo(1): the host is in the roster of a session
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 0F GetWirelessRebootPassphrase
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 10 StopSession
    {Z, Z, Z, Z, Z, Z, Z, Z},  // 11 InitializeWithName
};
static const u32 kFkNodesTotal[8] = {0, 0, 0, 0, 2, 2, 2, 0};

static void TestMatrixSearcher() {
    for (u32 id = 1; id <= 0x11; ++id) {
        for (int st = 0; st < 8; ++st) {
            World w(2);
            MakeClientState(w, st);
            Console& c = w[1];
            // the setup really brought the console to the state (the byte GetMyStatus shows)
            const bool joining_ok = st != 3 || c.lobby.client_state() == 3;
            CHECK(joining_ok);
            if (st != 3) {
                CHECK_EQ(c.lobby.ClientStatusWord() >> 24, kCliShown[st]);
            }
            const Reply r = FkCell(c, id);
            if (r.out != DlpOutcome::Done) {
                CHECK(false);
                continue;
            }
            const u32 want = kFkMatrix[id][st];
            CHECK_EQ(r.res(), want);
            if (want == 0) {
                CHECK_EQ(r.hdr(), kFkReplyHeader[id]);
            } else {
                CHECK_EQ(r.hdr(), (id << 16) | 0x40u);
            }
            if (want != 0) {
                continue;
            }
            if (id == 0x03) {
                for (int i = 2; i <= 7; ++i) {
                    CHECK_EQ(r.w[i], 0u);
                }
            }
            if (id == 0x04) {
                CHECK_EQ(r.w[2], 0x0421u);
            }
            if (id == 0x09) {
                CHECK_EQ(r.w[2], 0x04EA0002u);  // the descriptor of the record written
            }
            if (id == 0x0C) {
                CHECK_EQ(r.w[2] >> 24, kCliShown[st]);
                CHECK_EQ(r.w[3] + r.w[4] + r.w[5] + r.w[6], 0u);
                if (st >= 4 && st <= 6) {
                    CHECK_EQ(r.w[2] & 0xFFFFFF, 0x010002u);  // connected, node 2
                } else {
                    CHECK_EQ(r.w[2] & 0xFFFFFF, 0u);
                }
            }
            if (id == 0x0D) {
                CHECK_EQ(r.w[2], kFkNodesTotal[st]);
            }
            if (id == 0x0E) {
                CHECK_EQ(r.w[2], 0x1000u);  // the host's seed, written out in the rig (0x1000 + console number)
                CHECK_EQ(r.w[2 + 3] & 0xFFFF, 'H');  // +0x0C: the first unit of "Host"
                CHECK_EQ(r.w[11], 1u);               // +0x24: node 1
            }
            if (id == 0x0F) {
                if (st == 6) {
                    const u8* p = reinterpret_cast<const u8*>(r.w + 2);
                    CHECK_BYTES(p, kNine, 9);
                    CHECK_EQ(p[9] | p[10] | p[11], 0u);
                } else {
                    CHECK_EQ(r.w[2] | r.w[3] | r.w[4], 0u);
                }
            }
        }
    }
}

// Creator states (the columns): 0 off, 1 idle, 2 accepting, 3 distributing (6), 4 passphrase (8), 5 complete (9).
static const u32 kSrvShown[6] = {0, 1, 2, 6, 8, 9};

static void MakeServerState(World& w, int st) {
    Console& h = w[0];
    if (st == 0) {
        return;
    }
    h.SvInit(3, 0);
    if (st == 1) {
        return;
    }
    h.SvAccepting(0, 6);
    w.Run(40);
    if (st == 2) {
        return;
    }
    if (st == 3) {  // one client in, its START_ACK never arrives: the host waits in 6
        Console& c = w[1];
        c.FkInit();
        c.FkScan(kFilterTd);
        w.Run(60);
        c.FkStopScan();
        CHECK(JoinThroughIpc(w, 1));
        w.Run(60);
        w.air.Drop(T_START_ACK, FakeAir::kAny, FakeAir::kAny, FakeAir::kForever);
        h.SvDistribute();
        w.Run(40);
        return;
    }
    h.SvDistribute();  // nobody in: straight to 8
    if (st == 4) {
        return;
    }
    h.SvPass(kNine);  // nobody to wait for: straight to 9
}

static Reply SvCell(Console& c, u32 id) {
    switch (id) {
    case 0x01: return c.SvInit(3);
    case 0x02: return c.SvFinalize();
    case 0x03: return c.SvState();
    case 0x04: return c.SvEventDesc();
    case 0x05: return c.SvAccepting(0, 6);
    case 0x06: return c.SvEnd();
    case 0x07: return c.SvDistribute();
    case 0x08: return c.SvPass(kNine);
    case 0x09: return c.SvAccept(7);
    case 0x0A: return c.SvDisconnect(7);
    case 0x0B: return c.SvClients(4, 8, c.mem.Alloc(8));
    case 0x0C: return c.SvClientInfo(7);
    case 0x0D: return c.SvClientState(7);
    case 0x0E: return c.SvIsChild(0xE);
    case 0x0F: {
        const u16 name[10] = {'Z', 'e', 'd', 0, 0, 0, 0, 0, 0, 0};
        return c.SvInitName(3, 0, name);
    }
    default: return c.SvDup();
    }
}

static const u32 kSvMatrix[0x11][6] = {
    {Z, Z, Z, Z, Z, Z},  // (id 0 is not a command)
    {Z, Z, Z, Z, Z, Z},  // 01 Initialize
    {Z, Z, Z, Z, Z, Z},  // 02 Finalize
    {Z, Z, Z, Z, Z, Z},  // 03 GetServerState
    {Z, Z, Z, Z, Z, Z},  // 04 GetEventDescription
    {I, Z, I, I, I, I},  // 05 StartAccepting: from "idle" only
    {Z, Z, Z, Z, Z, Z},  // 06 EndAccepting
    {I, I, Z, I, I, I},  // 07 StartDistribution: from "accepting" only
    {I, I, I, I, Z, I},  // 08 SendWirelessRebootPassphrase: in state 8 only
    {I, N, N, N, N, N},  // 09 AcceptClient (a node nobody holds)
    {I, N, N, N, N, N},  // 0A DisconnectClient
    {I, Z, Z, Z, Z, Z},  // 0B GetConnectingClients
    {I, N, N, N, N, N},  // 0C GetClientInfo(7)
    {I, N, N, N, N, N},  // 0D GetClientState(7)
    {Z, Z, Z, Z, Z, Z},  // 0E IsChild
    {Z, Z, Z, Z, Z, Z},  // 0F InitializeWithName
    {Z, Z, Z, Z, Z, Z},  // 10 GetDupNoticeNeed
};
static const u32 kSvClientsTotal[6] = {0, 0, 0, 1, 0, 0};

static void TestMatrixCreator() {
    for (u32 id = 1; id <= 0x10; ++id) {
        for (int st = 0; st < 6; ++st) {
            World w(2);
            MakeServerState(w, st);
            Console& h = w[0];
            CHECK_EQ(h.lobby.ServerState(), kSrvShown[st]);
            const Reply r = SvCell(h, id);
            if (r.out != DlpOutcome::Done) {
                CHECK(false);
                continue;
            }
            const u32 want = kSvMatrix[id][st];
            CHECK_EQ(r.res(), want);
            if (want == 0) {
                CHECK_EQ(r.hdr(), kSvReplyHeader[id]);
            } else {
                CHECK_EQ(r.hdr(), (id << 16) | 0x40u);
            }
            if (want != 0) {
                continue;
            }
            if (id == 0x03) {
                CHECK_EQ(r.w[2], kSrvShown[st]);
            }
            if (id == 0x04) {
                for (int i = 2; i <= 8; ++i) {
                    CHECK_EQ(r.w[i], 0u);
                }
            }
            if (id == 0x0B) {
                CHECK_EQ(r.w[2], kSvClientsTotal[st]);
                CHECK_EQ(r.w[3], 0x8Cu);  // the descriptor and the address of the request, echoed
                CHECK_EQ(r.w[4], h.mem.addr_of(h.mem.count() - 1));
            }
            if (id == 0x0E || id == 0x10) {
                CHECK_EQ(r.w[2], 0u);
            }
        }
    }
    // The host's own record: GetClientInfo(1) is "no data" WITH the full header and the host's record (Azahar's convention)
    World w(2);
    MakeServerState(w, 2);
    const Reply r = w[0].SvClientInfo(1);
    CHECK_EQ(r.hdr(), 0x000C02C0u);
    CHECK_EQ(r.res(), 0xC880AFEFu);
    CHECK_EQ(r.w[2], 0x1000u);
    CHECK_EQ(r.w[2 + 1], 0u);
    CHECK_EQ(r.w[11], 1u);
    // ... but not in state 0
    World w0(1);
    const Reply r0 = w0[0].SvClientInfo(1);
    CHECK_EQ(r0.hdr(), 0x000C0040u);
    CHECK_EQ(r0.res(), 0xE0A0AC01u);
}

// ProbeOnly column: no state at all; the same table for every state of the game.
static void TestMatrixProbeOnly() {
    for (int kind = 0; kind < 2; ++kind) {
        DlpService s;
        const u32 last = kind == 0 ? 0x11u : 0x10u;
        for (u32 id = 0; id <= 0x20; ++id) {
            u32 cmd[64];
            std::memset(cmd, 0, sizeof(cmd));
            cmd[0] = id << 16;
            s.Dispatch(kind == 0 ? kFk : kSv, cmd, nullptr, IpcResolver{nullptr, nullptr});
            if (id == 0 || id > last) {
                CHECK_EQ(cmd[0], 0x40u);
                CHECK_EQ(cmd[1], 0xD9001830u);
            } else if (kind == 0 && (id == 1 || id == 2 || id == 0x11)) {
                CHECK_EQ(cmd[0], (id << 16) | 0x40u);
                CHECK_EQ(cmd[1], 0u);
            } else if (kind == 1 && id == 0x0E) {
                CHECK_EQ(cmd[0], 0x000E0080u);
                CHECK_EQ(cmd[1], 0u);
                CHECK_EQ(cmd[2], 0u);
            } else {
                CHECK_EQ(cmd[0], (id << 16) | 0x40u);
                CHECK_EQ(cmd[1], 0xE0A0AC01u);
            }
        }
    }
}


// =====================================================================================================================
// The three records, byte for byte. The expected bytes are written out from SPEC-DLP.md sez. 5 and DESIGN sez. 5.2: nothing
// here is computed with the offsets of the code under test.

static HostAnnounce MakeAnnounce(u32 child_tid_low, const u16* name, u8 max_nodes, u8 accepting) {
    HostAnnounce a;
    std::memset(&a, 0, sizeof(a));
    a.child_tid_low = child_tid_low;
    if (name != nullptr) {
        for (int i = 0; i < 10; ++i) {
            a.name[i] = name[i];
        }
    }
    a.max_nodes = max_nodes;
    a.accepting = accepting;
    return a;
}

static const Mac kMacA = {0x40, 0xF4, 0x07, 0x99, 0x88, 0x77};
static const Mac kMacB = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const Mac kMacC = {0x40, 0xF4, 0x07, 0x99, 0x88, 0x78};
static const u16 kNameA[10] = {0x30C6, 0x00E9, 'A', 'l', 'i', 'c', 'e', 0, 0, 0xFFFE};
static const u16 kNameC[10] = {'M', 'a', 'x', 0, 0, 0, 0, 0, 0, 0};

/// Three lobbies in the air (no console runs them), heard by console 1 which scans and then stops.
static void ThreeLobbiesHeard(World& w) {
    w.air.AddAir(kMacA, MakeAnnounce(0x0010C002, kNameA, 4, 1), 2, 3);
    w.air.AddAir(kMacB, MakeAnnounce(0x0010C0FF, nullptr, 2, 1), 1, 0);
    w.air.AddAir(kMacC, MakeAnnounce(0x0010C000, kNameC, 3, 1), 3, 7);
    Console& c = w[1];
    c.FkInit();
    c.FkScan(kFilterTd);
    c.lobby.Pump();  // heard now (at the start time of the world)
    c.FkStopScan();
}

static void ExpectTitleInfo(const u8* got, const Mac& mac, u8 variation) {
    u8 exp[0x13A8];
    std::memset(exp, 0, sizeof(exp));
    exp[0] = 0xC0;  // unique_id 0x10C0 (child title id low 0x0010C0xx >> 8)
    exp[1] = 0x10;
    exp[4] = variation;  // variation: the child index, in the low byte
    for (int i = 0; i < 6; ++i) {
        exp[8 + i] = mac[static_cast<std::size_t>(i)];
    }
    CHECK_BYTES(got, exp, sizeof(exp));  // everything else (version, ratings, titles, icon, size) is zero
}

static void TestRecordsByteForByte() {
    World w(2, 9000);
    ThreeLobbiesHeard(w);
    Console& c = w[1];
    w.air.Advance(1000);  // now 10000, the lobbies were heard at 9000
    // GetTitleInfoInOrder: the three, in the order they were heard, then "no data"
    Sb t0 = c.NewSb(0x13A8);
    Sb t1 = c.NewSb(0x13A8);
    Sb t2 = c.NewSb(0x13A8);
    Sb t3 = c.NewSb(0x13A8);
    Reply r = c.FkInOrder(1, t0);
    CHECK_EQ(r.hdr(), 0x00090042u);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 0x04EA0002u);  // StaticBufferDesc(0x13A8, 0)
    CHECK_EQ(r.w[3], t0.addr);      // the address of the game
    ExpectTitleInfo(c.Data(t0), kMacA, 0x02);
    r = c.FkInOrder(0, t1);
    CHECK_EQ(r.res(), 0u);
    ExpectTitleInfo(c.Data(t1), kMacB, 0xFF);
    r = c.FkInOrder(0, t2);
    CHECK_EQ(r.res(), 0u);
    ExpectTitleInfo(c.Data(t2), kMacC, 0x00);
    r = c.FkInOrder(0, t3);
    CHECK_EQ(r.hdr(), 0x00090040u);
    CHECK_EQ(r.res(), 0xC880AFEFu);
    CHECK(c.mem.Untouched(t3.addr));  // an error writes nothing
    // ... and again, from the start, with the restart flag in any non-zero byte
    r = c.FkInOrder(0x7F, t3);
    CHECK_EQ(r.res(), 0u);
    ExpectTitleInfo(c.Data(t3), kMacA, 0x02);
    // GetTitleInfo(mac): the same bytes
    Sb g = c.NewSb(0x13A8);
    r = c.FkTitleInfo(kMacB, g);
    CHECK_EQ(r.hdr(), 0x00080042u);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 0x04EA0002u);
    CHECK_EQ(r.w[3], g.addr);
    ExpectTitleInfo(c.Data(g), kMacB, 0xFF);

    // GetServerInfo: MAC, signal, max, joined (the host counted), the name at +0x1C, the tick at +0x290
    const u32 want_tick = 0x8FD39480;  // (10000 - 1000) ms * 268112 ticks/ms
    Sb s = c.NewSb(0x298);
    r = c.FkServerInfo(kMacA, s);
    CHECK_EQ(r.hdr(), 0x00070042u);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 0x00A60002u);  // StaticBufferDesc(0x298, 0)
    CHECK_EQ(r.w[3], s.addr);
    u8 expa[0x298];
    std::memset(expa, 0, sizeof(expa));
    for (int i = 0; i < 6; ++i) {
        expa[i] = kMacA[static_cast<std::size_t>(i)];
    }
    expa[0x07] = 3;  // signal
    expa[0x08] = 4;  // max_nodes
    expa[0x09] = 2;  // players in the lobby now (the host counted)
    const u8 name_a[20] = {0xC6, 0x30, 0xE9, 0x00, 0x41, 0x00, 0x6C, 0x00, 0x69, 0x00, 0x63, 0x00, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0xFF};
    std::memcpy(expa + 0x1C, name_a, 20);
    const u8 tick_bytes[8] = {0x80, 0x94, 0xD3, 0x8F, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(expa + 0x290, tick_bytes, 8);
    CHECK_BYTES(c.Data(s), expa, sizeof(expa));
    CHECK_EQ(LoadLE32(c.Data(s) + 0x290), want_tick);
    // the second: nobody but the host, one signal bar less, an empty name
    Sb s2 = c.NewSb(0x298);
    r = c.FkServerInfo(kMacB, s2);
    CHECK_EQ(r.res(), 0u);
    u8 expb[0x298];
    std::memset(expb, 0, sizeof(expb));
    expb[0] = 0x02;
    expb[5] = 0x01;
    expb[0x07] = 0;
    expb[0x08] = 2;
    expb[0x09] = 1;
    std::memcpy(expb + 0x290, tick_bytes, 8);
    CHECK_BYTES(c.Data(s2), expb, sizeof(expb));
    // the third: a signal of 7 is 3 bars (0..3), the lobby is full (3 of 3), a short name
    Sb s3 = c.NewSb(0x298);
    r = c.FkServerInfo(kMacC, s3);
    CHECK_EQ(r.res(), 0u);
    u8 expc[0x298];
    std::memset(expc, 0, sizeof(expc));
    for (int i = 0; i < 6; ++i) {
        expc[i] = kMacC[static_cast<std::size_t>(i)];
    }
    expc[0x07] = 3;
    expc[0x08] = 3;
    expc[0x09] = 3;
    expc[0x1C] = 'M';
    expc[0x1E] = 'a';
    expc[0x20] = 'x';
    std::memcpy(expc + 0x290, tick_bytes, 8);
    CHECK_BYTES(c.Data(s3), expc, sizeof(expc));
    // an unknown MAC and a MAC nobody sent: "no data", nothing written
    Sb s4 = c.NewSb(0x298);
    r = c.FkServerInfo(Mac{9, 9, 9, 9, 9, 9}, s4);
    CHECK_EQ(r.hdr(), 0x00070040u);
    CHECK_EQ(r.res(), 0xC880AFEFu);
    CHECK(c.mem.Untouched(s4.addr));
}

static void TestNodeInfoRecords() {
    // two consoles with seeds and names written out; the full record and the empty one
    for (int full = 1; full >= 0; --full) {
        FakeAir air(100000);
        LobbyConfig hc = MakeCfg(0x0010C000, full ? 0x1122334455667788ull : 0, full ? "ABCDEFGHIJ" : "");
        LobbyConfig cc = MakeCfg(0x0010C000, full ? 0xA1A2A3A4A5A6A7A8ull : 0, full ? "Zed" : "");
        Console h(air, 0, hc);
        Console c(air, 1, cc);
        auto run = [&](u32 ms) {
            for (u32 t = 0; t < ms; t += 20) {
                air.Advance(20);
                h.lobby.Pump();
                c.lobby.Pump();
            }
        };
        h.SvInit(3, 0);
        h.SvAccepting(0, 6);
        run(40);
        c.FkInit();
        c.FkScan(kFilterTd);
        run(60);
        c.FkStopScan();
        const Reply j = c.FkJoin(h.net.mac());
        CHECK(j.out == DlpOutcome::JoinPending);
        bool done = false;
        for (int i = 0; i < 20 && !done; ++i) {
            c.Pending(&done);
            run(20);
        }
        CHECK(done);
        run(300);
        u8 exp_host[0x28];
        u8 exp_cli[0x28];
        std::memset(exp_host, 0, sizeof(exp_host));
        std::memset(exp_cli, 0, sizeof(exp_cli));
        if (full) {
            const u8 seed_h[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
            const u8 seed_c[8] = {0xA8, 0xA7, 0xA6, 0xA5, 0xA4, 0xA3, 0xA2, 0xA1};
            std::memcpy(exp_host, seed_h, 8);
            std::memcpy(exp_cli, seed_c, 8);
            for (int i = 0; i < 10; ++i) {
                exp_host[0x0C + 2 * i] = static_cast<u8>('A' + i);
            }
            exp_cli[0x0C] = 'Z';
            exp_cli[0x0E] = 'e';
            exp_cli[0x10] = 'd';
        }
        exp_host[0x24] = 1;  // the network node id (u32)
        exp_cli[0x24] = 2;
        // the searcher reads both nodes: [header 0x000E02C0, 0, 10 words of the record]
        Reply r = c.FkNodeInfo(1);
        CHECK_EQ(r.hdr(), 0x000E02C0u);
        CHECK_EQ(r.res(), 0u);
        CHECK_BYTES(r.w + 2, exp_host, 0x28);
        r = c.FkNodeInfo(2);
        CHECK_EQ(r.hdr(), 0x000E02C0u);
        CHECK_BYTES(r.w + 2, exp_cli, 0x28);
        // the creator reads the client the same way, and its own record by the node 1 convention
        r = h.SvClientInfo(2);
        CHECK_EQ(r.hdr(), 0x000C02C0u);
        CHECK_EQ(r.res(), 0u);
        CHECK_BYTES(r.w + 2, exp_cli, 0x28);
        r = h.SvClientInfo(1);
        CHECK_EQ(r.hdr(), 0x000C02C0u);
        CHECK_EQ(r.res(), 0xC880AFEFu);
        CHECK_BYTES(r.w + 2, exp_host, 0x28);
        // anything else: "no data", no payload
        r = h.SvClientInfo(3);
        CHECK_EQ(r.hdr(), 0x000C0040u);
        CHECK_EQ(r.res(), 0xC880AFEFu);
        r = c.FkNodeInfo(3);
        CHECK_EQ(r.hdr(), 0x000E0040u);
        CHECK_EQ(r.res(), 0xC880AFEFu);
        // the creator's list of clients: the one id, in the game's u16 array
        u32 total = 99;
        const std::vector<u16> ids = h.Clients(4, 8, &total);
        CHECK_EQ(total, 1u);
        CHECK_EQ(ids.size(), 1u);
        CHECK_EQ(ids.empty() ? 0 : ids[0], 2u);
    }
}

// The tick of ServerInfo+0x290 for the four clock situations: small clock, clock past 2^32 ms, and each with a tick offset
// of 2^40 (a console that has been up for days). The entry was heard 1000 ms before the call.
static void TestTickArithmetic() {
    struct Case {
        u64 start;
        u64 offset;
        u8 expect[8];  // the u64 tick, little endian: (start + 1000 - 1000) ms * 268112 + offset
    };
    const Case cases[4] = {
        {9000, 0, {0x80, 0x94, 0xD3, 0x8F, 0x00, 0x00, 0x00, 0x00}},
        {9000, 1ull << 40, {0x80, 0x94, 0xD3, 0x8F, 0x00, 0x01, 0x00, 0x00}},
        {4294967296ull + 9000, 0, {0x80, 0x94, 0xD3, 0x8F, 0x50, 0x17, 0x04, 0x00}},
        {4294967296ull + 9000, 1ull << 40, {0x80, 0x94, 0xD3, 0x8F, 0x50, 0x18, 0x04, 0x00}},
    };
    for (const Case& k : cases) {
        World w(2, k.start);
        w.air.SetTickOffset(k.offset);
        ThreeLobbiesHeard(w);
        w.air.Advance(1000);
        Console& c = w[1];
        Sb s = c.NewSb(0x298);
        const Reply r = c.FkServerInfo(kMacA, s);
        CHECK_EQ(r.res(), 0u);
        CHECK_BYTES(c.Data(s) + 0x290, k.expect, 8);
        // the game's own aging arithmetic keeps the entry
        CHECK(!GameDropsEntry(w.air.ticks(), LoadLE64(c.Data(s) + 0x290)));
    }
}

// The age of an entry is the age the game sees: 1.4 s in a scan, and for an entry frozen by StopScan 3.9 s and 5.1 s (the
// second one the game itself would drop and DeleteScanInfo, the first one it keeps).
static void TestTickAges() {
    {
        World w(2, 50000);
        w.air.AddAir(kMacA, MakeAnnounce(0x0010C002, kNameA, 4, 1), 2, 3);
        Console& c = w[1];
        c.FkInit();
        c.FkScan(kFilterTd);
        c.lobby.Pump();         // heard at 50000
        w.air.RemoveAir(0);     // and never again
        w.air.Advance(1400);
        Sb s = c.NewSb(0x298);
        Reply r = c.FkServerInfo(kMacA, s);
        CHECK_EQ(r.res(), 0u);
        const u64 tick = LoadLE64(c.Data(s) + 0x290);
        CHECK_EQ(w.air.ticks() - tick, 1400ull * 268112ull);  // exactly the age: [1.4 s, 1.4 s + 1 ms]
        CHECK(!GameDropsEntry(w.air.ticks(), tick));
        // 1550 ms without a beacon: gone for the game as well (kGroupTtl), from both commands
        w.air.Advance(150);
        r = c.FkServerInfo(kMacA, s);
        CHECK_EQ(r.res(), 0xC880AFEFu);
        Sb t = c.NewSb(0x13A8);
        r = c.FkInOrder(1, t);
        CHECK_EQ(r.res(), 0xC880AFEFu);
        r = c.FkTitleInfo(kMacA, t);
        CHECK_EQ(r.res(), 0xC880AFEFu);
    }
    {
        World w(2, 60000);
        w.air.AddAir(kMacA, MakeAnnounce(0x0010C002, kNameA, 4, 1), 2, 3);
        w.air.AddAir(kMacB, MakeAnnounce(0x0010C0FF, nullptr, 2, 1), 1, 0);
        Console& c = w[1];
        c.FkInit();
        c.FkScan(kFilterTd);
        c.lobby.Pump();      // both heard at 60000
        w.air.RemoveAir(0);  // A is never heard again; B keeps being heard
        w.Run(1200);
        c.FkStopScan();      // frozen at 61200: A last heard at 60000, B at 61200
        w.air.Advance(3900); // now 65100: A is 5100 ms old, B 3900
        Sb sa = c.NewSb(0x298);
        Sb sb2 = c.NewSb(0x298);
        CHECK_EQ(c.FkServerInfo(kMacA, sa).res(), 0u);
        CHECK_EQ(c.FkServerInfo(kMacB, sb2).res(), 0u);
        const u64 ta = LoadLE64(c.Data(sa) + 0x290);
        const u64 tb = LoadLE64(c.Data(sb2) + 0x290);
        CHECK_EQ(w.air.ticks() - ta, 5100ull * 268112ull);
        CHECK_EQ(w.air.ticks() - tb, 3900ull * 268112ull);
        CHECK(GameDropsEntry(w.air.ticks(), ta));    // the game drops it (and calls DeleteScanInfo)
        CHECK(!GameDropsEntry(w.air.ticks(), tb));   // and keeps this one
        // what the game does with the old one: DeleteScanInfo, which removes it and nothing else
        CHECK_EQ(c.FkDelete(kMacA).res(), 0u);
        CHECK_EQ(c.FkServerInfo(kMacA, sa).res(), 0xC880AFEFu);
        CHECK_EQ(c.FkServerInfo(kMacB, sb2).res(), 0u);
    }
}

// =====================================================================================================================
// The buffers of the game: null, wrong, too small; canaries; no effect on the lobby; exact-size regions for ASan;
// unaligned addresses for UBSan.

static void TestStaticBuffers() {
    World w(2, 9000);
    ThreeLobbiesHeard(w);
    Console& c = w[1];
    const u32 sizes[3] = {0x298, 0x13A8, 0x13A8};
    const u32 ids[3] = {0x07, 0x08, 0x09};
    for (int k = 0; k < 3; ++k) {
        const u32 size = sizes[k];
        const u32 id = ids[k];
        auto call = [&](const u32* sbuf, const IpcResolver* res) -> Reply {
            if (k == 0) {
                return c.Call(kFk, 0x00070080, {MacW1(kMacA), MacW2(kMacA)}, sbuf, res);
            }
            if (k == 1) {
                return c.Call(kFk, 0x00080100, {MacW1(kMacA), MacW2(kMacA), 0x0010C000, 0x00040001}, sbuf, res);
            }
            return c.Call(kFk, 0x00090040, {1}, sbuf, res);
        };
        const Sb good = c.NewSb(size);
        const u32 bad_before = c.svc.bad_pointer();
        u32 expected_bad = 0;
        auto expect_bad = [&](const Reply& r) {
            CHECK_EQ(r.hdr(), (id << 16) | 0x40u);
            CHECK_EQ(r.res(), R_BAD_POINTER);
            ++expected_bad;
            CHECK_EQ(c.svc.bad_pointer(), bad_before + expected_bad);
        };
        // no pair of static buffers at all
        expect_bad(call(nullptr, nullptr));
        // a descriptor that is not static buffer 0: another type, another buffer id, a mapped-write one
        const u32 d_type[2] = {(size << 14) | 6, good.addr};
        const u32 d_id[2] = {(size << 14) | (1u << 10) | 2, good.addr};
        const u32 d_mapped[2] = {(size << 4) | 0xC, good.addr};
        expect_bad(call(d_type, nullptr));
        expect_bad(call(d_id, nullptr));
        expect_bad(call(d_mapped, nullptr));
        // an address of 0, an address nobody mapped
        const u32 d_zero[2] = {(size << 14) | 2, 0};
        const u32 d_wild[2] = {(size << 14) | 2, 0x0DEAD000};
        expect_bad(call(d_zero, nullptr));
        const u32 maps = c.mem.map_calls;
        expect_bad(call(d_wild, nullptr));
        CHECK_EQ(c.mem.map_calls, maps + 1);  // (the resolver was asked, and said no)
        // a resolver without a function
        const IpcResolver nomap = {nullptr, nullptr};
        expect_bad(call(good.w, &nomap));
        // a capacity one byte short: nothing written, not a byte
        const Sb shortcap = c.NewSb(size - 1);
        expect_bad(call(shortcap.w, nullptr));
        CHECK(c.mem.Untouched(shortcap.addr));
        // the declared capacity is enough but the memory of the game is not: the resolver refuses, nothing is written
        const u32 d_decl[2] = {(size << 14) | 2, shortcap.addr};
        expect_bad(call(d_decl, nullptr));
        CHECK(c.mem.Untouched(shortcap.addr));
        // a range that starts inside a region and runs out of it
        const u32 d_straddle[2] = {(size << 14) | 2, good.addr + 1};
        expect_bad(call(d_straddle, nullptr));
        CHECK(c.mem.Untouched(good.addr));
        // the lobby saw none of this; now the good ones: exact size, a larger capacity, and unaligned addresses
        Reply r = call(good.w, nullptr);
        CHECK_EQ(r.res(), 0u);
        CHECK(!c.mem.Untouched(good.addr));
        const Sb big = c.NewSb(size + 0x100);
        r = call(big.w, nullptr);
        CHECK_EQ(r.res(), 0u);
        CHECK_EQ(r.w[2], (size << 14) | 2);  // the descriptor carries the size WRITTEN, not the capacity
        CHECK_EQ(r.w[3], big.addr);
        for (u32 mis = 1; mis <= 3; ++mis) {
            const Sb odd = c.NewSb(size, mis);
            CHECK_EQ(odd.addr & 3, mis & 3);
            r = call(odd.w, nullptr);
            CHECK_EQ(r.res(), 0u);
            CHECK_EQ(r.w[3], odd.addr);
        }
        const Sb exact = c.NewSb(size, 0, true);
        r = call(exact.w, nullptr);
        CHECK_EQ(r.res(), 0u);
    }
    // the bad buffer has NO effect on the lobby: the cursor of GetTitleInfoInOrder does not move, and a restart that comes
    // with a bad buffer is not applied
    {
        World w2(2, 9000);
        ThreeLobbiesHeard(w2);
        Console& d = w2[1];
        Sb good = d.NewSb(0x13A8);
        const u32 wild[2] = {(0x13A8u << 14) | 2, 0x0DEAD000};
        Reply r = d.FkInOrder(1, good);
        CHECK_EQ(r.res(), 0u);
        CHECK_BYTES(d.Data(good) + 8, kMacA.data(), 6);
        r = d.Call(kFk, 0x00090040, {1}, wild);  // a restart with a bad buffer
        CHECK_EQ(r.res(), R_BAD_POINTER);
        r = d.Call(kFk, 0x00090040, {0}, wild);
        CHECK_EQ(r.res(), R_BAD_POINTER);
        r = d.FkInOrder(0, good);
        CHECK_EQ(r.res(), 0u);
        CHECK_BYTES(d.Data(good) + 8, kMacB.data(), 6);  // the second, not the first and not "no data"
        r = d.FkInOrder(0, good);
        CHECK_BYTES(d.Data(good) + 8, kMacC.data(), 6);
    }
}

/// Console 0 hosts, consoles 1..n join it (through the IPC), and the roster of console 1 is read.
static void HostWithClients(World& w, int clients) {
    HostUp(w);
    for (int i = 1; i <= clients; ++i) {
        Console& c = w[i];
        c.FkInit();
        c.FkScan(kFilterTd);
        w.Run(60);
        c.FkStopScan();
        CHECK(JoinThroughIpc(w, i));
        w.Run(60);
    }
    w.Run(300);
}

static void TestConnectingNodesBuffer() {
    World w(4);
    HostWithClients(w, 3);
    Console& c = w[1];
    CHECK_EQ(c.lobby.ClientStatusWord() >> 24, 5u);
    const u8 all[8] = {1, 0, 2, 0, 3, 0, 4, 0};
    // n = 4, room for 4: the four ids as u16, the total, the descriptor of the bytes written, the address
    Sb b = c.NewSb(8, 0, true);  // exact size: an overrun is an ASan report
    Reply r = c.FkNodes(4, b);
    CHECK_EQ(r.hdr(), 0x000D0082u);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 4u);
    CHECK_EQ(r.w[3], 0x00020002u);  // 8 bytes written
    CHECK_EQ(r.w[4], b.addr);
    CHECK_BYTES(c.Data(b), all, 8);
    // n = 0xFFFF and a huge buffer: still four, never more (the game's array may be any size)
    Sb big = c.NewSb(0x100);
    r = c.FkNodes(0xFFFF, big);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 4u);
    CHECK_EQ(r.w[3], 0x00020002u);
    CHECK_BYTES(c.Data(big), all, 8);
    for (u32 i = 8; i < 0x100; ++i) {
        CHECK_EQ(c.Data(big)[i], Mem::kFill);
    }
    // the count is a u16: 0x10002 is 2
    c.mem.Refill(big.addr);
    r = c.FkNodes(0x10002, big);
    CHECK_EQ(r.w[2], 4u);
    CHECK_EQ(r.w[3], 0x00010002u);  // 2 ids, 4 bytes
    // n = 2: two ids, the total is still 4
    c.mem.Refill(big.addr);
    r = c.FkNodes(2, big);
    CHECK_EQ(r.w[2], 4u);
    CHECK_EQ(r.w[3], 0x00010002u);
    CHECK_BYTES(c.Data(big), all, 4);
    CHECK_EQ(c.Data(big)[4], Mem::kFill);
    // n = 0: nothing written, nothing mapped, the total
    c.mem.Refill(big.addr);
    const u32 maps = c.mem.map_calls;
    r = c.FkNodes(0, big);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 4u);
    CHECK_EQ(r.w[3], 0x00000002u);
    CHECK_EQ(c.mem.map_calls, maps);
    CHECK(c.mem.Untouched(big.addr));
    // a buffer that holds 3 ids, then 1 byte (so none), by capacity
    Sb three = c.NewSb(6);
    r = c.FkNodes(4, three);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 4u);
    CHECK_EQ(r.w[3], 0x00018002u);  // 6 bytes written
    Sb one = c.NewSb(1);
    r = c.FkNodes(4, one);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 4u);
    CHECK_EQ(r.w[3], 2u);
    CHECK(c.mem.Untouched(one.addr));
    // pointers: null pair, wrong type, unmapped, no resolver function (with ids to write)
    r = c.Call(kFk, 0x000D0040, {4}, nullptr);
    CHECK_EQ(r.hdr(), 0x000D0040u);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    const u32 wrong[2] = {(8u << 14) | 6, big.addr};
    r = c.Call(kFk, 0x000D0040, {4}, wrong);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    const u32 wild[2] = {(8u << 14) | 2, 0x0DEAD000};
    r = c.Call(kFk, 0x000D0040, {4}, wild);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    const u32 zero[2] = {(8u << 14) | 2, 0};
    r = c.Call(kFk, 0x000D0040, {4}, zero);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    const IpcResolver nomap = {nullptr, nullptr};
    r = c.Call(kFk, 0x000D0040, {4}, big.w, &nomap);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    // ... but with nothing to write the pair is not looked at: a console outside any session
    World w2(1);
    r = w2[0].Call(kFk, 0x000D0040, {4}, nullptr);
    CHECK_EQ(r.hdr(), 0x000D0082u);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 0u);
    CHECK_EQ(r.w[3], 2u);
    CHECK_EQ(r.w[4], 0u);
}

static void TestConnectingClientsBuffer() {
    World w(4);
    HostWithClients(w, 3);
    Console& h = w[0];
    const u8 ids3[6] = {2, 0, 3, 0, 4, 0};
    // [1] n, [2] (bytes << 4) | 0xC, [3] the address: the reply echoes the descriptor and the address of the REQUEST
    u32 addr = h.mem.Alloc(8, 0, true);
    Reply r = h.SvClients(4, 8, addr);
    CHECK_EQ(r.hdr(), 0x000B0082u);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 3u);
    CHECK_EQ(r.w[3], 0x8Cu);
    CHECK_EQ(r.w[4], addr);
    CHECK_BYTES(h.mem.Find(addr, 6), ids3, 6);
    CHECK_EQ(h.mem.Find(addr, 8)[6], Mem::kFill);  // the bytes after the three ids: not touched
    CHECK_EQ(h.mem.Find(addr, 8)[7], Mem::kFill);
    // a game that offers more room than there are clients, and a count of 0xFFFF: three ids, never four
    addr = h.mem.Alloc(0x40);
    r = h.SvClients(0xFFFF, 0x40, addr);
    CHECK_EQ(r.w[2], 3u);
    CHECK_BYTES(h.mem.Find(addr, 6), ids3, 6);
    CHECK_EQ(h.mem.Find(addr, 8)[6], Mem::kFill);
    // the count is a u16: 0x10002 is 2
    h.mem.Refill(addr);
    r = h.SvClients(0x10002, 0x40, addr);
    CHECK_EQ(r.w[2], 3u);
    CHECK_BYTES(h.mem.Find(addr, 4), ids3, 4);
    CHECK_EQ(h.mem.Find(addr, 8)[4], Mem::kFill);
    // n = 0: the total, no id
    h.mem.Refill(addr);
    r = h.SvClients(0, 0x40, addr);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 3u);
    CHECK(h.mem.Untouched(addr));
    // the buffer holds one id (2 bytes) with three clients: one id, the total says 3
    addr = h.mem.Alloc(2);
    r = h.SvClients(4, 2, addr);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 3u);
    CHECK_EQ(r.w[3], 0x2Cu);
    CHECK_BYTES(h.mem.Find(addr, 2), ids3, 2);
    // one byte: no id fits, still a success
    addr = h.mem.Alloc(1);
    r = h.SvClients(4, 1, addr);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 3u);
    CHECK(h.mem.Untouched(addr));
    // no buffer at all (0 bytes): nothing is mapped, any address is fine, the total is returned
    const u32 maps = h.mem.map_calls;
    r = h.SvClients(4, 0, 0xDEADBEEF);
    CHECK_EQ(r.res(), 0u);
    CHECK_EQ(r.w[2], 3u);
    CHECK_EQ(r.w[3], 0xCu);
    CHECK_EQ(r.w[4], 0xDEADBEEFu);
    CHECK_EQ(h.mem.map_calls, maps);
    // pointers: a descriptor that is not a mapped-write one, an unmapped address, the address 0, a range that runs out
    addr = h.mem.Alloc(8);
    r = h.Call(kSv, 0x000B0042, {4, (8u << 4) | 0x6, addr});
    CHECK_EQ(r.hdr(), 0x000B0040u);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    r = h.Call(kSv, 0x000B0042, {4, (8u << 4) | 0x2, addr});
    CHECK_EQ(r.res(), R_BAD_POINTER);
    r = h.SvClients(4, 8, 0x0DEAD000);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    r = h.SvClients(4, 8, 0);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    r = h.SvClients(4, 8, addr + 4);  // 8 bytes from 4 bytes before the end
    CHECK_EQ(r.res(), R_BAD_POINTER);
    r = h.SvClients(4, 0x100, addr);  // the declared size is larger than the memory
    CHECK_EQ(r.res(), R_BAD_POINTER);
    CHECK(h.mem.Untouched(addr));
    const IpcResolver nomap = {nullptr, nullptr};
    r = h.Call(kSv, 0x000B0042, {4, (8u << 4) | 0xC, addr}, nullptr, &nomap);
    CHECK_EQ(r.res(), R_BAD_POINTER);
    // an unaligned buffer (+1, +2, +3) under UBSan
    for (u32 mis = 1; mis <= 3; ++mis) {
        const u32 a = h.mem.Alloc(6, mis);
        r = h.SvClients(4, 6, a);
        CHECK_EQ(r.res(), 0u);
        CHECK_BYTES(h.mem.Find(a, 6), ids3, 6);
    }
}

// =====================================================================================================================
// The widths of the words.

static void TestWidths() {
    // StartAccepting: the channel is the low byte of [2], "manual" is the low byte of [1] (Luigi's Mansion: 0x03a12006)
    {
        World w(2);
        Console& h = w[0];
        Console& c = w[1];
        h.SvInit(3, 0);
        CHECK_EQ(h.SvAccepting(0x03a12001, 0x03a12006).res(), 0u);
        CHECK_EQ(h.net.LastChannel(), 6);
        w.Run(40);
        c.FkInit();
        c.FkScan(kFilterTd);
        w.Run(60);
        c.FkStopScan();
        CHECK(JoinThroughIpc(w, 1));
        w.Run(60);
        CHECK_EQ(h.SvClientState(2).w[2] >> 24, 0x40u);  // manual: the client waits to be accepted
        CHECK_EQ(h.SvAccept(2).res(), 0u);
        CHECK_EQ(h.SvClientState(2).w[2] >> 24, 5u);
    }
    {
        World w(2);
        Console& h = w[0];
        Console& c = w[1];
        h.SvInit(3, 0);
        CHECK_EQ(h.SvAccepting(0x100, 0x00000106).res(), 0u);  // the byte of [1] is 0: not manual; the channel is 6
        CHECK_EQ(h.net.LastChannel(), 6);
        w.Run(40);
        c.FkInit();
        c.FkScan(kFilterTd);
        w.Run(60);
        c.FkStopScan();
        CHECK(JoinThroughIpc(w, 1));
        w.Run(60);
        CHECK_EQ(h.SvClientState(2).w[2] >> 24, 5u);  // accepted by itself
    }
    {
        World w(1);
        Console& h = w[0];
        h.SvInit(3, 0);
        CHECK_EQ(h.SvAccepting(0, 0x03a12005).res(), R_OUT_OF_RANGE);  // 5 is not a channel
        CHECK_EQ(h.SvAccepting(0, 12).res(), R_OUT_OF_RANGE);
        CHECK_EQ(h.SvAccepting(0, 2).res(), R_OUT_OF_RANGE);
        CHECK_EQ(h.SvAccepting(0, 0x10B).res(), 0u);  // the low byte is 11
        CHECK_EQ(h.net.LastChannel(), 11);
        h.SvInit(3, 0);
        CHECK_EQ(h.SvAccepting(0, 1).res(), 0u);
        h.SvInit(3, 0);
        CHECK_EQ(h.SvAccepting(0, 0).res(), 0u);
    }
    // SRVR Initialize: the maximum of clients is a byte, 0 is 1, never above 3 (the lobby has host + 3); the child index is a byte
    {
        const u32 maxes[10] = {0, 1, 2, 3, 4, 15, 0x103, 0xFFFFFF03, 0x100, 0x20001};
        const u32 want_max_nodes[10] = {2, 2, 3, 4, 4, 4, 4, 4, 2, 2};  // (the low byte: 0x100 is 0, which counts as 1)
        for (int i = 0; i < 10; ++i) {
            World w(2, 9000);
            w[0].SvInit(maxes[i], 0);
            w[0].SvAccepting(0, 6);
            w.Run(40);
            Console& c = w[1];
            c.FkInit();
            c.FkScan(kFilterTd);
            w.Run(60);
            Sb s = c.NewSb(0x298);
            CHECK_EQ(c.FkServerInfo(MacOf(w[0]), s).res(), 0u);
            CHECK_EQ(c.Data(s)[8], want_max_nodes[i]);
            CHECK_EQ(c.Data(s)[9], 1u);  // the host alone
        }
        const u32 indexes[5] = {0, 2, 0xFF, 0x12345678, 0xFFFFFF00};
        const u32 want_variation[5] = {0, 2, 0xFF, 0x78, 0};
        for (int i = 0; i < 5; ++i) {
            World w(2);
            w[0].SvInit(3, indexes[i]);
            w[0].SvAccepting(0, 6);
            w.Run(40);
            Console& c = w[1];
            c.FkInit();
            c.FkScan(kFilterTd);  // the scan asks for child index 0: the index is ignored by the filter
            w.Run(60);
            Sb t = c.NewSb(0x13A8);
            CHECK_EQ(c.FkInOrder(1, t).res(), 0u);
            CHECK_EQ(LoadLE32(c.Data(t)), 0x10C0u);
            CHECK_EQ(LoadLE32(c.Data(t) + 4), want_variation[i]);
            CHECK_BYTES(c.Data(t) + 8, MacOf(w[0]).data(), 6);
            // another game's unique id: invisible
            CHECK_EQ(c.FkScan(kFilterTd).res(), R_INVALID_STATE);  // (already scanning)
            c.FkFinalize();
            c.FkInit();
            c.FkScan(0x000400010010D000ull);
            w.Run(60);
            CHECK_EQ(c.FkInOrder(1, t).res(), 0xC880AFEFu);
        }
    }
    // the node ids are u16: 0x0102 is NOT node 2 (a cast to u8 would say so), and the upper half of the word is not read
    {
        World w(2);
        HostWithClients(w, 1);
        Console& h = w[0];
        Console& c = w[1];
        CHECK_EQ(h.SvClientInfo(2).res(), 0u);
        CHECK_EQ(h.SvClientInfo(0x0102).res(), 0xC880AFEFu);
        CHECK_EQ(h.SvClientInfo(0x10002).res(), 0u);  // [1] is a u16: this is node 2
        CHECK_EQ(h.SvClientState(2).res(), 0u);
        CHECK_EQ(h.SvClientState(0x0102).res(), 0xC880AFEFu);
        CHECK_EQ(h.SvAccept(0x0102).res(), 0xC880AFEFu);
        CHECK_EQ(h.SvDisconnect(0x0102).res(), 0xC880AFEFu);
        CHECK_EQ(h.net.Kicked(2), 0);  // the client was not touched
        CHECK_EQ(c.FkNodeInfo(0x0102).res(), 0xC880AFEFu);
        CHECK_EQ(c.FkNodeInfo(2).res(), 0u);
        CHECK_EQ(c.FkNodeInfo(0x10002).res(), 0u);
        CHECK_EQ(c.FkNodeInfo(0).res(), 0xC880AFEFu);
        CHECK_EQ(h.SvClientState(0).res(), 0xC880AFEFu);
        CHECK_EQ(h.SvClientState(0xFFFF).res(), 0xC880AFEFu);
    }
    // GetTitleInfoInOrder: restart is a byte
    {
        World w(2, 9000);
        ThreeLobbiesHeard(w);
        Console& c = w[1];
        Sb t = c.NewSb(0x13A8);
        c.FkInOrder(1, t);
        c.FkInOrder(0, t);
        CHECK_BYTES(c.Data(t) + 8, kMacB.data(), 6);
        c.FkInOrder(0x100, t);  // the low byte is 0: go on
        CHECK_BYTES(c.Data(t) + 8, kMacC.data(), 6);
        c.FkInOrder(0xFFFFFF00u, t);  // still 0
        CHECK_EQ(c.FkInOrder(0xFFFFFF00u, t).res(), 0xC880AFEFu);
        c.FkInOrder(0x101, t);  // the low byte is 1: restart
        CHECK_BYTES(c.Data(t) + 8, kMacA.data(), 6);
        c.FkInOrder(2, t);
        CHECK_BYTES(c.Data(t) + 8, kMacA.data(), 6);
    }
    // The number of beacons of Initialize: 0 and 1 are 1, never above 4 (a room of 4 players has at most 4 lobbies)
    {
        const u32 beacons[5] = {0, 1, 2, 16, 0xFFFFFFFF};
        const u32 want[5] = {1, 1, 2, 4, 4};
        for (int i = 0; i < 5; ++i) {
            World w(2, 9000);
            for (int g = 0; g < 6; ++g) {
                const Mac mac = {0x40, 0xF4, 0x07, 0x99, 0x88, static_cast<u8>(0x70 + g)};
                w.air.AddAir(mac, MakeAnnounce(0x0010C000, kNameC, 4, 1), 2, 3);
            }
            Console& c = w[1];
            c.FkInit(beacons[i]);
            c.FkScan(kFilterTd);
            c.lobby.Pump();
            Sb t = c.NewSb(0x13A8);
            u32 n = 0;
            Reply r = c.FkInOrder(1, t);
            while (r.res() == 0 && n < 10) {
                ++n;
                r = c.FkInOrder(0, t);
            }
            CHECK_EQ(n, want[i]);
        }
    }
    // the channel mask and the MAC filter of StartScan are not read, whatever they hold (Azahar asserted on the mask)
    {
        World w(1);
        Console& c = w[0];
        c.FkInit();
        CHECK_EQ(c.Call(kFk, 0x00050180, {0x0002, 0x0010C000, 0x00040001, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}).res(), 0u);
        CHECK_EQ(c.lobby.client_state(), 2u);
    }
}


// =====================================================================================================================
// StartFakeSession in two steps: JoinPending, then PollPending until the lobby has an outcome.

static void ScanAndStop(World& w, Console& c) {
    c.FkInit();
    c.FkScan(kFilterTd);
    w.Run(60);
    c.FkStopScan();
}

static void TestJoinFlow() {
    // (a) the plain case: JoinPending, the command words not written, the first PollPending ends it
    {
        World w(2);
        HostUp(w);
        Console& c = w[1];
        ScanAndStop(w, c);
        const u32 refused0 = c.svc.refused();
        Reply j = c.FkJoin(MacOf(w[0]));
        CHECK(j.out == DlpOutcome::JoinPending);  // (Call checked that not a word of the command changed)
        CHECK(c.svc.join_pending());
        CHECK_EQ(c.svc.last_id(), 0x0Bu);
        CHECK_EQ(CliState(c), 1u);  // while it joins the game still sees "idle"
        bool done = false;
        Reply r = c.Pending(&done);
        CHECK(done);
        CHECK_EQ(r.hdr(), 0x000B0040u);
        CHECK_EQ(r.res(), 0u);
        CHECK(!c.svc.join_pending());
        CHECK_EQ(CliState(c), 5u);  // 5 at once: never 0 or 0x80 (the game shows an error screen for those)
        CHECK_EQ(c.svc.refused(), refused0);
        // no join in progress any more: true with the invalid-state reply of command 0x0B
        r = c.Pending(&done);
        CHECK(done);
        CHECK_EQ(r.hdr(), 0x000B0040u);
        CHECK_EQ(r.res(), 0xE0A0AC01u);
        CHECK_EQ(c.svc.last_id(), 0x0Bu);
        CHECK_EQ(c.svc.last_result(), 0xE0A0AC01u);
        CHECK_EQ(c.svc.refused(), refused0 + 1);
    }
    // (b) the host answers late: PollPending says "not yet" with the words untouched, then the result
    {
        World w(2);
        HostUp(w);
        Console& c = w[1];
        ScanAndStop(w, c);
        c.net.ConnectDelay(250);
        CHECK(c.FkJoin(MacOf(w[0])).out == DlpOutcome::JoinPending);
        bool done = true;
        c.Pending(&done);
        CHECK(!done);
        w.Run(100);
        c.Pending(&done);
        CHECK(!done);
        CHECK(c.svc.join_pending());
        w.Run(100);
        c.Pending(&done);
        CHECK(!done);
        w.Run(80);
        const Reply r = c.Pending(&done);
        CHECK(done);
        CHECK_EQ(r.res(), 0u);
        CHECK_EQ(CliState(c), 5u);
    }
    // (c) a full host never answers: the join ends at 3000 ms with the timeout, the state is 1 (not the 5 Azahar leaves), one
    // Disconnect, and the same table can be tried again
    {
        World w(3);
        HostUp(w, 1);  // host + 1 client
        ScanAndStop(w, w[1]);
        CHECK(JoinThroughIpc(w, 1));
        w.Run(60);
        Console& c = w[2];
        ScanAndStop(w, c);
        CHECK(c.FkJoin(MacOf(w[0])).out == DlpOutcome::JoinPending);
        w.Run(2980);
        bool done = true;
        c.Pending(&done);
        CHECK(!done);  // 2980 ms: not yet
        w.Run(20);
        const Reply r = c.Pending(&done);
        CHECK(done);  // 3000 ms
        CHECK_EQ(r.hdr(), 0x000B0040u);
        CHECK_EQ(r.res(), 0xC920AFFEu);
        CHECK_EQ(CliState(c), 1u);
        CHECK_EQ(c.net.DisconnectCalls(), 1);
        CHECK(!c.svc.join_pending());
        CHECK_EQ(c.FkScan(kFilterTd).res(), 0u);  // idle again: a new scan is allowed
    }
    // (d) the engine cannot even begin: "server not found" at once, no JoinPending
    {
        World w(2);
        HostUp(w);
        Console& c = w[1];
        ScanAndStop(w, c);
        c.net.FailConnectBegin(true);
        const Reply r = c.FkJoin(MacOf(w[0]));
        CHECK(r.out == DlpOutcome::Done);
        CHECK_EQ(r.hdr(), 0x000B0040u);
        CHECK_EQ(r.res(), 0xC880AC05u);
        CHECK(!c.svc.join_pending());
        CHECK_EQ(CliState(c), 1u);
    }
    // (e) the engine says the join failed: "server not found" from PollPending
    {
        World w(2);
        HostUp(w);
        Console& c = w[1];
        ScanAndStop(w, c);
        c.net.ConnectResult(-1);
        CHECK(c.FkJoin(MacOf(w[0])).out == DlpOutcome::JoinPending);
        bool done = false;
        const Reply r = c.Pending(&done);
        CHECK(done);
        CHECK_EQ(r.res(), 0xC880AC05u);
        CHECK_EQ(CliState(c), 1u);
        CHECK_EQ(c.net.DisconnectCalls(), 1);
    }
    // (f) the errors that come back at once: unknown MAC (no data), wrong state (invalid state)
    {
        World w(2);
        HostUp(w);
        Console& c = w[1];
        c.FkInit();
        const Mac nobody = {1, 2, 3, 4, 5, 6};
        Reply r = c.FkJoin(nobody);
        CHECK(r.out == DlpOutcome::Done);
        CHECK_EQ(r.res(), 0xC880AFEFu);
        c.FkScan(kFilterTd);
        w.Run(60);
        r = c.FkJoin(MacOf(w[0]));  // still scanning
        CHECK(r.out == DlpOutcome::Done);
        CHECK_EQ(r.res(), 0xE0A0AC01u);
        CHECK(!c.svc.join_pending());
    }
    // (g) a second StartFakeSession while one is running is refused, the first goes on
    {
        World w(2);
        HostUp(w);
        Console& c = w[1];
        ScanAndStop(w, c);
        c.net.ConnectDelay(100);
        CHECK(c.FkJoin(MacOf(w[0])).out == DlpOutcome::JoinPending);
        const Reply r = c.FkJoin(MacOf(w[0]));
        CHECK(r.out == DlpOutcome::Done);
        CHECK_EQ(r.res(), 0xE0A0AC01u);
        CHECK(c.svc.join_pending());
        w.Run(120);
        bool done = false;
        const Reply p = c.Pending(&done);
        CHECK(done);
        CHECK_EQ(p.res(), 0u);
    }
    // (h) cancelled from outside: the session closed, StopSession, Finalize, Initialize: the waiting thread gets the
    // invalid-state reply and the lobby is idle, one Disconnect
    for (int how = 0; how < 4; ++how) {
        World w(2);
        HostUp(w);
        Console& c = w[1];
        ScanAndStop(w, c);
        c.net.ConnectDelay(100000);
        CHECK(c.FkJoin(MacOf(w[0])).out == DlpOutcome::JoinPending);
        if (how == 0) {
            c.svc.OnSessionClosed(kFk);
            CHECK(!c.svc.join_pending());
        } else if (how == 1) {
            CHECK_EQ(c.FkStopSession().res(), 0u);
        } else if (how == 2) {
            CHECK_EQ(c.FkFinalize().res(), 0u);
        } else {
            CHECK_EQ(c.FkInit().res(), 0u);
        }
        bool done = false;
        const Reply r = c.Pending(&done);
        CHECK(done);
        CHECK_EQ(r.hdr(), 0x000B0040u);
        CHECK_EQ(r.res(), 0xE0A0AC01u);
        CHECK_EQ(c.net.DisconnectCalls(), 1);
        CHECK(!c.svc.join_pending());
        CHECK(c.lobby.client_state() == 1 || c.lobby.client_state() == 0);
    }
    // (i) without a lobby, and SetLobby forgets a join
    {
        DlpService probe;
        u32 cmd[64];
        std::memset(cmd, 0, sizeof(cmd));
        CHECK(probe.PollPending(cmd));
        CHECK_EQ(cmd[0], 0x000B0040u);
        CHECK_EQ(cmd[1], 0xE0A0AC01u);
        World w(2);
        HostUp(w);
        Console& c = w[1];
        ScanAndStop(w, c);
        c.net.ConnectDelay(100000);
        CHECK(c.FkJoin(MacOf(w[0])).out == DlpOutcome::JoinPending);
        c.svc.SetLobby(&c.lobby);
        CHECK(!c.svc.join_pending());
    }
}

// Two real threads under ONE mutex, as the plugin does (phase 3): one is the game thread inside StartFakeSession (a
// PollPending loop that sleeps OUTSIDE the mutex), the other makes other calls, moves the clock and gives the network back at
// the end (OnGameUdsStarting). The air checks that no call into the network runs outside the mutex and that two threads are
// never in it together.
static void TestJoinThreads() {
    World w(2);
    HostUp(w);
    Console& h = w[0];
    Console& c = w[1];
    ScanAndStop(w, c);
    c.net.ConnectDelay(150);
    std::mutex m;
    std::atomic<bool> held{false};
    w.air.WatchLock(&held);
    auto locked = [&](auto fn) {
        std::lock_guard<std::mutex> g(m);
        held = true;
        fn();
        held = false;
    };
    std::atomic<bool> finished{false};
    u32 result = 0xFFFFFFFF;
    bool began = false;
    std::thread a([&] {
        locked([&] { began = c.FkJoin(MacOf(h)).out == DlpOutcome::JoinPending; });
        for (int i = 0; i < 5000; ++i) {
            bool done = false;
            Reply r;
            locked([&] { r = c.Pending(&done); });
            if (done) {
                result = r.res();
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));  // outside the mutex
        }
        finished = true;
    });
    std::thread b([&] {
        for (int i = 0; i < 5000 && !finished.load(); ++i) {
            locked([&] {
                w.air.Advance(10);
                h.lobby.Pump();
                c.lobby.Pump();
                c.FkStatus();  // the other game threads of the same console keep working while one waits
                h.SvState();
            });
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    a.join();
    b.join();
    CHECK(began);
    CHECK_EQ(result, 0u);
    locked([&] {
        CHECK_EQ(c.FkStatus().w[2] >> 24, 5u);
        CHECK(c.lobby.OnGameUdsStarting());  // the game is about to open nwm::UDS: the network is given back
        CHECK(!c.lobby.OnGameUdsStarting());
        CHECK_EQ(c.FkStatus().w[2] >> 24, 1u);
    });
    CHECK_EQ(w.air.overlaps(), 0);
    CHECK_EQ(w.air.unlocked_calls(), 0);
    w.air.WatchLock(nullptr);
}

// =====================================================================================================================
// The seven defects of Azahar that are NOT copied (DESIGN sez. 8), one test each with its name.

static void DefectAzahar1_ScanChannelMaskIsNotAsserted() {
    // Azahar ASSERTs [1] == 0x0421; Luigi's Mansion sends 0x2. Any value is fine here.
    World w(1);
    Console& c = w[0];
    c.FkInit();
    CHECK_EQ(c.FkScan(kFilterTd, 0x0002).res(), 0u);
    c.FkStopScan();
    CHECK_EQ(c.FkScan(kFilterTd, 0x0000).res(), 0u);
    c.FkStopScan();
    CHECK_EQ(c.FkScan(kFilterTd, 0xFFFFFFFF).res(), 0u);
    c.FkStopScan();
    CHECK_EQ(c.FkScan(kFilterTd, 0x0421).res(), 0u);
    CHECK_EQ(c.FkChannels().w[2], 0x0421u);
}

static void DefectAzahar2_NoState5AfterAFailedJoin() {
    // Azahar leaves state 5 after a timed-out StartFakeSession; here a failed join leaves 1 (and an idle lobby)
    World w(3);
    HostUp(w, 1);
    ScanAndStop(w, w[1]);
    CHECK(JoinThroughIpc(w, 1));
    w.Run(60);
    Console& c = w[2];
    ScanAndStop(w, c);
    CHECK(c.FkJoin(MacOf(w[0])).out == DlpOutcome::JoinPending);
    CHECK_EQ(CliState(c), 1u);  // joining: shown as 1
    w.Run(3000);
    bool done = false;
    const Reply r = c.Pending(&done);
    CHECK(done);
    CHECK_EQ(r.res(), 0xC920AFFEu);
    CHECK_EQ(CliState(c), 1u);
    CHECK(c.FkStatus().w[2] != 5u << 24);
    CHECK_EQ(c.FkStatus().w[2] & 0xFFFFFF, 0u);  // not connected, no node
    CHECK(!c.lobby.active());
}

static void DefectAzahar3_ClientStateOutsideTheVector() {
    // Azahar indexes its vector with the node id; the node ids 0, 1 and anything outside the clients are "no data" here
    World w(2);
    HostWithClients(w, 1);
    Console& h = w[0];
    const u32 nodes[8] = {0, 1, 5, 6, 0xFF, 0x100, 0xFFFF, 0x7FFF};
    for (u32 n : nodes) {
        const Reply r = h.SvClientState(n);
        CHECK_EQ(r.hdr(), 0x000D0040u);
        CHECK_EQ(r.res(), 0xC880AFEFu);
    }
    CHECK_EQ(h.SvClientState(2).res(), 0u);
    CHECK_EQ(h.SvClientState(3).res(), 0xC880AFEFu);
}

static void DefectAzahar4_ServerFinalizeResetsTheState() {
    World w(2);
    Console& h = w[0];
    h.SvInit(3, 0);
    CHECK_EQ(SrvState(h), 1u);
    h.SvAccepting(0, 6);
    CHECK_EQ(SrvState(h), 2u);
    CHECK_EQ(h.SvFinalize().res(), 0u);
    CHECK_EQ(SrvState(h), 0u);  // Azahar keeps the old state
    CHECK_EQ(h.SvFinalize().res(), 0u);  // twice is fine
    CHECK_EQ(SrvState(h), 0u);
    CHECK_EQ(h.net.HostCloseCalls(), 1);
}

static void DefectAzahar5_NoMinusOneAsAnErrorCode() {
    // the passphrase sent twice, AcceptClient and DisconnectClient on an unknown node: real module codes, never 0xFFFFFFFF
    World w(2);
    HostWithClients(w, 1);
    Console& h = w[0];
    CHECK_EQ(h.SvDistribute().res(), 0u);
    CHECK(w.RunUntil([&] { return h.lobby.server_state() == 8; }, 2000));
    CHECK_EQ(h.SvPass(kNine).res(), 0u);
    const u8 second[9] = {'z', 'y', 'x', 'w', 'v', 'u', 't', 's', 0};
    const Reply r = h.SvPass(second);
    CHECK_EQ(r.hdr(), 0x00080040u);
    CHECK_EQ(r.res(), 0xE0A0AC01u);
    CHECK(r.res() != 0xFFFFFFFFu);
    CHECK_EQ(h.SvAccept(9).res(), 0xC880AFEFu);
    CHECK_EQ(h.SvDisconnect(9).res(), 0xC880AFEFu);
    CHECK_EQ(h.SvAccept(2).res(), 0xC880AFEFu);  // already accepted: not in the waiting state either
    // and the first passphrase is the one that reaches the client
    w.Run(400);
    CHECK_EQ(w[1].FkStatus().w[2] >> 24, 9u);
    const Reply p = w[1].FkPass();
    CHECK_BYTES(p.w + 2, kNine, 9);
}

static void DefectAzahar6_ChildIndexIsNotIgnored() {
    // the index of SRVR Initialize [4] goes into the child title id (and so into the variation of the title record)
    World w(2);
    w[0].SvInit(3, 2);
    w[0].SvAccepting(0, 6);
    w.Run(40);
    Console& c = w[1];
    c.FkInit();
    c.FkScan(kFilterTd);
    w.Run(60);
    Sb t = c.NewSb(0x13A8);
    CHECK_EQ(c.FkInOrder(1, t).res(), 0u);
    CHECK_EQ(LoadLE32(c.Data(t) + 4), 2u);
    CHECK_EQ(LoadLE32(c.Data(t)), 0x10C0u);
}

static void DefectAzahar7_HostIsCountedInTheClientsOnPurpose() {
    // NOT a fix: ServerInfo+8 (max clients) is the room of the lobby and ServerInfo+9 (clients joined) the players now,
    // BOTH with the host, the convention Azahar's real games were proved with; the game hides a group whose +9 equals +8.
    World w(3);
    HostUp(w);
    Console& c = w[2];
    c.FkInit();
    c.FkScan(kFilterTd);
    w.Run(60);
    Sb s = c.NewSb(0x298);
    CHECK_EQ(c.FkServerInfo(MacOf(w[0]), s).res(), 0u);
    CHECK_EQ(c.Data(s)[8], 4u);  // SRVR Initialize(3) -> 4
    CHECK_EQ(c.Data(s)[9], 1u);  // nobody but the host
    ScanAndStop(w, w[1]);
    CHECK(JoinThroughIpc(w, 1));
    w.Run(200);
    CHECK_EQ(c.FkServerInfo(MacOf(w[0]), s).res(), 0u);
    CHECK_EQ(c.Data(s)[8], 4u);
    CHECK_EQ(c.Data(s)[9], 2u);  // host + one client
}

// =====================================================================================================================
// Session closes without a Finalize.

static void TestSessionClosed() {
    World w(1);
    Console& c = w[0];
    // nothing initialised: nothing happens
    c.svc.OnSessionClosed(kFk);
    c.svc.OnSessionClosed(kSv);
    CHECK_EQ(c.net.AllCalls(), 0);
    CHECK_EQ(c.lobby.client_state(), 0u);
    CHECK_EQ(c.lobby.server_state(), 0u);
    // a scanning searcher and a creator at rest: closing the SRVR session does not touch the searcher
    c.FkInit();
    c.FkScan(kFilterTd);
    c.SvInit(3, 0);
    c.svc.OnSessionClosed(kSv);
    CHECK_EQ(c.lobby.server_state(), 0u);
    CHECK_EQ(c.lobby.client_state(), 2u);
    CHECK_EQ(c.net.ScanCloseCalls(), 0);
    c.svc.OnSessionClosed(kFk);
    CHECK_EQ(c.lobby.client_state(), 1u);
    CHECK_EQ(c.net.ScanCloseCalls(), 1);
    c.svc.OnSessionClosed(kFk);  // again: Finalize of an initialised role is idempotent
    CHECK_EQ(c.net.ScanCloseCalls(), 1);
    // an accepting creator: the SRVR session closes -> HostClose once, state 0
    c.SvInit(3, 0);
    c.SvAccepting(0, 6);
    CHECK_EQ(c.lobby.server_state(), 2u);
    c.svc.OnSessionClosed(kFk);  // the other role's session: not the creator's lobby
    CHECK_EQ(c.lobby.server_state(), 2u);
    c.svc.OnSessionClosed(kSv);
    CHECK_EQ(c.lobby.server_state(), 0u);
    CHECK_EQ(c.net.HostCloseCalls(), 1);
    c.svc.OnSessionClosed(kSv);
    CHECK_EQ(c.net.HostCloseCalls(), 1);
    // ProbeOnly: nothing to close
    DlpService probe;
    probe.OnSessionClosed(kFk);
    probe.OnSessionClosed(kSv);
    CHECK_EQ(probe.commands(), 0u);
}


// =====================================================================================================================
// Complete rounds through the IPC, in the order the games call the commands (GIOCO-TD-PR.md sez. 3-4 for Triple Deluxe;
// SPEC-DLP.md sez. 7-8 / FLUSSO for Battle Royale). The "game" here reads the records the way the games do: the unique id,
// the child index, the MAC from the title record; the hosts' name, the numbers and the tick from the server record; and it
// ages its list with its own arithmetic (ticks x 3.7296 ns against 5000 ms).

struct GameGroup {
    Mac mac;
    u32 unique_id;
    u32 variation;
    u8 max_clients;
    u8 joined;
    u16 host_name[10];
    u64 tick;
};

static std::string Ascii(const u16* name) {
    std::string s;
    for (int i = 0; i < 10 && name[i] != 0; ++i) {
        s += static_cast<char>(name[i]);
    }
    return s;
}

/// The scan frame of the game: GetTitleInfoInOrder(1) then (0) until EXACTLY "no data"; for each title GetServerInfo of its
/// MAC (an error discards it); the aging check on the tick of the server record.
static std::vector<GameGroup> ScanFrame(Console& c, const Sb& t, const Sb& s, u64 now_ticks, bool* dropped) {
    std::vector<GameGroup> v;
    Reply r = c.FkInOrder(1, t);
    for (int guard = 0; guard < 20 && r.res() != 0xC880AFEFu; ++guard) {
        CHECK_EQ(r.res(), 0u);  // anything but "no data" is "there is a title" to the game
        GameGroup g;
        std::memset(&g, 0, sizeof(g));
        const u8* p = c.Data(t);
        g.unique_id = LoadLE32(p);
        g.variation = LoadLE32(p + 4);
        std::memcpy(g.mac.data(), p + 8, 6);
        if (c.FkServerInfo(g.mac, s).res() == 0) {
            const u8* q = c.Data(s);
            g.max_clients = q[8];
            g.joined = q[9];
            for (int i = 0; i < 10; ++i) {
                g.host_name[i] = LoadLE16(q + 0x1C + 2 * i);
            }
            g.tick = LoadLE64(q + 0x290);
            if (GameDropsEntry(now_ticks, g.tick)) {
                *dropped = true;
            }
            v.push_back(g);
        }
        r = c.FkInOrder(0, t);
    }
    return v;
}

/// One complete round: a host and `nclients` clients; `td` = the call order of Triple Deluxe, else Battle Royale's.
/// `net_thread`: the network thread of the plugin pumps every lobby each frame; without it ONLY the commands of the game pump
/// (the Dispatch of every command), which is all that is needed when the game calls its status commands every frame.
static void RunRound(bool td, int nclients, bool net_thread = true) {
    const u32 tid_low = td ? 0x0010C000u : 0x001C2100u;
    const u32 unique_id = td ? 0x10C0u : 0x1C21u;
    const u64 filter = td ? kFilterTd : kFilterBr;
    World w(1 + nclients, 100000, tid_low);
    Console& h = w[0];
    auto frame = [&]() {
        w.air.Advance(16);
        if (net_thread) {
            for (auto& x : w.c) {
                x->lobby.Pump();
            }
        }
    };
    // --- the creator: Initialize, StartAccepting(0, 6)
    const u16 host_name[10] = {'B', 'r', 'H', 'o', 's', 't', 0, 0, 0, 0};
    if (td) {
        CHECK_EQ(h.SvInit(3, 0).res(), 0u);
        const Reply d = h.SvDup();  // right after Initialize; a negative answer would fail the game's whole Initialize
        CHECK_EQ(d.hdr(), 0x00100080u);
        CHECK_EQ(d.res(), 0u);
        CHECK_EQ(d.w[2], 0u);
    } else {
        CHECK_EQ(h.SvInitName(4, 0, host_name).res(), 0u);  // BR asks for 4 clients: the room has 3
    }
    CHECK_EQ(h.SvAccepting(0, 6).res(), 0u);
    CHECK_EQ(SrvState(h), 2u);
    // --- each client: scan, choose, join
    for (int i = 1; i <= nclients; ++i) {
        Console& c = w[i];
        if (td) {
            CHECK_EQ(c.FkInit(16).res(), 0u);
        } else {
            const u16 name[10] = {'B', 'r', 'C', 'l', 'i', static_cast<u16>('0' + i), 0, 0, 0, 0};
            CHECK_EQ(c.FkInitName(name, 16).res(), 0u);
        }
        const Reply ch = c.FkChannels();
        CHECK_EQ(ch.res(), 0u);
        CHECK_EQ(ch.w[2], 0x0421u);
        CHECK_EQ(c.FkScan(filter, ch.w[2]).res(), 0u);
        const Sb t = c.NewSb(0x13A8);
        const Sb s = c.NewSb(0x298);
        Mac host_mac = {};
        bool found = false;
        for (int f = 0; f < 200 && !found; ++f) {
            frame();
            CHECK_EQ(CliState(c), 2u);
            bool dropped = false;
            const std::vector<GameGroup> groups = ScanFrame(c, t, s, w.air.ticks(), &dropped);
            CHECK(!dropped);  // fresh entries: the game's own aging keeps every one
            for (const GameGroup& g : groups) {
                CHECK_EQ(g.unique_id, unique_id);
                CHECK_EQ(g.variation & 0xFF, 0u);
                if (g.mac == MacOf(h) && g.joined < g.max_clients) {  // the game hides a group with +9 == +8
                    found = true;
                    host_mac = g.mac;
                    CHECK_EQ(g.max_clients, 4u);
                    CHECK_EQ(g.joined, static_cast<u32>(i));  // the host and the clients before this one
                    CHECK(Ascii(g.host_name) == (td ? "Host" : "BrHost"));
                }
            }
        }
        CHECK(found);
        // choose: StopScan, StartFakeSession (blocking for the game: the first reply is JoinPending, then PollPending)
        CHECK_EQ(c.FkStopScan().res(), 0u);
        CHECK(c.FkJoin(host_mac).out == DlpOutcome::JoinPending);
        bool done = false;
        Reply jr;
        for (int f = 0; f < 300 && !done; ++f) {
            jr = c.Pending(&done);
            if (!done) {
                frame();
            }
        }
        CHECK(done);
        CHECK_EQ(jr.res(), 0u);
        CHECK_EQ(CliState(c), 5u);  // at once: never 0 or 0x80
    }
    // --- the creator sees the clients: GetConnectingClients(4) and GetClientInfo of each
    for (int f = 0; f < 20; ++f) {
        frame();
    }
    {
        u32 total = 0;
        const std::vector<u16> ids = h.Clients(4, 8, &total);
        CHECK_EQ(total, static_cast<u32>(nclients));
        CHECK_EQ(ids.size(), static_cast<std::size_t>(nclients));
        for (std::size_t k = 0; k < ids.size(); ++k) {
            CHECK_EQ(ids[k], k + 2);
            const Reply ci = h.SvClientInfo(ids[k]);
            CHECK_EQ(ci.res(), 0u);
            // the name of the player at +0x0C of the record (10 units), the node id at +0x24
            u16 nm[10];
            for (int j = 0; j < 10; ++j) {
                nm[j] = LoadLE16(reinterpret_cast<const u8*>(ci.w + 2) + 0x0C + 2 * j);
            }
            const std::string want = std::string(td ? "Cli" : "BrCli") + static_cast<char>('0' + k + 1);
            CHECK(Ascii(nm) == want);
            CHECK_EQ(ci.w[2 + 9], ids[k]);
        }
        CHECK_EQ(SrvState(h), 2u);
    }
    // --- the clients see each other: GetConnectingNodes(4) + GetNodeInfo for each (states 3,4,5,6,9 in the game)
    for (int i = 1; i <= nclients; ++i) {
        Console& c = w[i];
        const Sb nb = c.NewSb(8);
        const Reply nr = c.FkNodes(4, nb);
        CHECK_EQ(nr.res(), 0u);
        CHECK_EQ(nr.w[2], static_cast<u32>(nclients + 1));
        for (u32 k = 0; k < nr.w[2]; ++k) {
            const u16 id = LoadLE16(c.Data(nb) + 2 * k);
            CHECK_EQ(id, k + 1);
            const Reply ni = c.FkNodeInfo(id);
            CHECK_EQ(ni.res(), 0u);
            CHECK_EQ(ni.w[2 + 9], id);
        }
    }
    // --- the Start button: StartDistribution; the clients go 5 -> 6; the creator reaches 8
    CHECK_EQ(h.SvDistribute().res(), 0u);
    u8 first_pass[9] = {};
    bool sent_first = false;
    bool host_done = false;
    for (int f = 0; f < 400 && !host_done; ++f) {
        frame();
        const u32 st = SrvState(h);
        CHECK(st != 0 && st != 10);  // the game treats 0 and 10 as errors
        for (int i = 1; i <= nclients; ++i) {
            const u32 cs = CliState(w[i]);
            CHECK(cs == 5 || cs == 6 || cs == 9);
        }
        if (st == 8) {
            // the game generates "%08x" of a random number and sends it: Triple Deluxe EVERY frame, Battle Royale once
            if (td || !sent_first) {
                u8 pass[9];
                std::snprintf(reinterpret_cast<char*>(pass), sizeof(pass), "%08x", Rnd() & 0x7FFFFFFF);
                const Reply pr = h.SvPass(pass);
                if (!sent_first) {
                    CHECK_EQ(pr.res(), 0u);
                    std::memcpy(first_pass, pass, 9);
                    sent_first = true;
                } else {
                    CHECK_EQ(pr.res(), 0xE0A0AC01u);  // the first one stays; the others are refused, nothing changes
                }
            }
        }
        if (st == 9) {
            // Triple Deluxe waits for "state 9 AND no connecting clients", and only then Finalize
            u32 total = 99;
            h.Clients(4, 8, &total);
            if (total == 0) {
                host_done = true;
            }
        }
    }
    CHECK(sent_first);
    CHECK(host_done);
    CHECK_EQ(h.SvFinalize().res(), 0u);
    CHECK_EQ(SrvState(h), 0u);
    // --- every client: state 9, the passphrase of the FIRST call, the roster still readable, then Finalize
    for (int i = 1; i <= nclients; ++i) {
        Console& c = w[i];
        for (int f = 0; f < 200 && CliState(c) != 9; ++f) {
            frame();
        }
        CHECK_EQ(CliState(c), 9u);
        const Reply pr = c.FkPass();
        CHECK_EQ(pr.hdr(), 0x000F0100u);
        CHECK_EQ(pr.res(), 0u);
        CHECK_BYTES(pr.w + 2, first_pass, 9);
        CHECK_EQ(reinterpret_cast<const u8*>(pr.w + 2)[9] | reinterpret_cast<const u8*>(pr.w + 2)[10] | reinterpret_cast<const u8*>(pr.w + 2)[11], 0u);
        // the host's network is closed; state 9 stays and the roster is still there
        const Sb nb = c.NewSb(8);
        const Reply nr = c.FkNodes(4, nb);
        CHECK_EQ(nr.res(), 0u);
        CHECK(nr.w[2] >= 2);
        for (int f = 0; f < 200; ++f) {
            frame();
        }
        CHECK_EQ(CliState(c), 9u);
        CHECK_EQ(c.FkFinalize().res(), 0u);  // fatal for the game if it were negative
        CHECK(!c.lobby.active());
    }
    CHECK(!h.lobby.active());
    CHECK(h.net.HostCloseCalls() >= 1);
    // nothing broke the books: only the refusals of the passphrase repeated each frame (Triple Deluxe) and nothing unknown
    CHECK_EQ(h.svc.unknown_commands(), 0u);
    CHECK_EQ(h.svc.bad_pointer(), 0u);
    for (int i = 1; i <= nclients; ++i) {
        CHECK_EQ(w[i].svc.refused(), 0u);
        CHECK_EQ(w[i].svc.unknown_commands(), 0u);
        CHECK_EQ(w[i].svc.bad_pointer(), 0u);
    }
}

static void TestCompleteRounds() {
    for (int n = 1; n <= 3; ++n) {
        std::printf("  TD, %d client(s)\n", n);
        RunRound(true, n);
    }
    for (int n = 1; n <= 3; ++n) {
        std::printf("  BR, %d client(s)\n", n);
        RunRound(false, n);
    }
    std::printf("  TD, 2 clients, no network thread (the commands pump)\n");
    RunRound(true, 2, false);
    std::printf("  BR, 1 client, no network thread\n");
    RunRound(false, 1, false);
}

// Every command in Lobby mode pumps the lobby first (rate limited by the lobby: 20 ms), a probe has nothing to pump.
static void TestDispatchPumps() {
    World w(2);
    HostUp(w);
    Console& h = w[0];
    const int before = h.net.MembersCalls();
    h.SvState();  // (the clock did not move since the last pump: the lobby's own rate limit)
    CHECK_EQ(h.net.MembersCalls(), before);
    w.air.Advance(100);
    h.SvState();
    CHECK_EQ(h.net.MembersCalls(), before + 1);
    w.air.Advance(100);
    h.Call(kSv, 0x00300000, {});  // even an id the service does not have
    CHECK_EQ(h.net.MembersCalls(), before + 2);
    w.air.Advance(100);
    h.FkStatus();  // either service
    CHECK_EQ(h.net.MembersCalls(), before + 3);
    // a client that joined shows up in the creator's list through the commands alone
    Console& c = w[1];
    c.FkInit();
    c.FkScan(kFilterTd);
    w.air.Advance(100);
    c.FkStatus();  // the pump of the searcher hears the lobby
    c.FkStopScan();
    CHECK(c.FkJoin(MacOf(h)).out == DlpOutcome::JoinPending);
    bool done = false;
    c.Pending(&done);
    CHECK(done);
    w.air.Advance(100);
    u32 total = 99;
    h.Clients(4, 8, &total);  // no external pump anywhere: this command pumps and tracks the client
    CHECK_EQ(total, 1u);
}

// The name goes through the words as the games lay it out: FKCL InitializeWithName [3..7], SRVR InitializeWithName [7..11],
// 10 UTF-16 units each, the first unit in the low half of the first word; and the passphrase bytes keep their order even
// with rubbish in the three bytes above the ninth.
static void TestNamesAndPassphraseWords() {
    World w(2);
    Console& h = w[0];
    Console& c = w[1];
    const u16 hname[10] = {0x30C6, 0x00E9, 'S', 'v', 'r', 0x1234, 'x', 'y', 'z', 0xFFFF};
    const u16 cname[10] = {'F', 'k', 0x0100, 0xABCD, 'c', 'l', 'i', 0, 0, 0x4321};
    CHECK_EQ(h.SvInitName(3, 0, hname).res(), 0u);
    CHECK_EQ(h.SvAccepting(0, 6).res(), 0u);
    w.Run(40);
    CHECK_EQ(c.FkInitName(cname).res(), 0u);
    c.FkScan(kFilterTd);
    w.Run(60);
    Sb s = c.NewSb(0x298);
    CHECK_EQ(c.FkServerInfo(MacOf(h), s).res(), 0u);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(LoadLE16(c.Data(s) + 0x1C + 2 * i), hname[i]);
    }
    c.FkStopScan();
    CHECK(JoinThroughIpc(w, 1));
    w.Run(300);
    const Reply ci = h.SvClientInfo(2);
    CHECK_EQ(ci.res(), 0u);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(LoadLE16(reinterpret_cast<const u8*>(ci.w + 2) + 0x0C + 2 * i), cname[i]);
    }
    // the passphrase: 9 bytes, rubbish above the ninth
    CHECK_EQ(h.SvDistribute().res(), 0u);
    CHECK(w.RunUntil([&] { return h.lobby.server_state() == 8; }, 2000));
    const u8 pass[9] = {0x00, 0xFF, 0x7F, 0x80, 'a', 'b', 'c', 'd', 0x5A};
    CHECK_EQ(h.SvPass(pass, 0xFFFFFF).res(), 0u);
    w.Run(400);
    CHECK_EQ(CliState(c), 9u);
    const Reply pr = c.FkPass();
    CHECK_BYTES(pr.w + 2, pass, 9);
    const u8* pb = reinterpret_cast<const u8*>(pr.w + 2);
    CHECK_EQ(pb[9] | pb[10] | pb[11], 0u);  // the padding of the reply is zero whatever the request had there
    CHECK_EQ(c.lobby.ClientStatusWord() >> 24, 9u);
}


// =====================================================================================================================
// Fuzz: 100000 requests of random words (fixed seed) against both services in both modes. Two lobby consoles talk to each
// other over the air (so scans, joins, START / GO really happen when a random MAC or node id happens to be right: the words
// are biased towards the real MACs, small node ids and the real buffers), one probe-only service. No crash, no undefined
// behaviour, no byte outside a buffer (canaries; exact-size regions for ASan), every reply well-shaped, the books add up.

static bool AllowedResult(u32 r) {
    return r == 0 || r == R_INVALID_STATE || r == R_BUSY || r == R_NO_DATA || r == R_NOT_FOUND || r == R_TIMEOUT ||
           r == R_OUT_OF_RANGE || r == R_BAD_POINTER || r == R_BAD_HEADER;
}

static void CheckFuzzReply(DlpKind kind, u32 id, const Reply& rp, bool lobby_mode) {
    const u32 last = kind == kFk ? 0x11u : 0x10u;
    CHECK(AllowedResult(rp.res()));
    if (id < 1 || id > last) {
        CHECK_EQ(rp.hdr(), 0x40u);
        CHECK_EQ(rp.res(), R_BAD_HEADER);
        return;
    }
    CHECK(rp.res() != R_BAD_HEADER);
    if (rp.res() == 0) {
        CHECK_EQ(rp.hdr(), kind == kFk ? kFkReplyHeader[id] : kSvReplyHeader[id]);
    } else {
        CHECK(rp.hdr() == ((id << 16) | 0x40u) || (kind == kSv && id == 0x0C && rp.res() == R_NO_DATA && rp.hdr() == 0x000C02C0u));
    }
    if (!lobby_mode) {
        CHECK(rp.res() == 0 || rp.res() == R_INVALID_STATE);
    }
}

static u32 RandWord(Console& c) {
    switch (Rnd() % 9) {
    case 0: return Rnd() % 8;
    case 1: return Rnd() & 0xFF;
    case 2: {
        static const u32 v[] = {0x100, 0x102, 0x10002, 0xFFFF, 0x10000, 0xFFFFFFFF, 0x80000000, 0x0421, 0x03a12006, 0x0DEAD000};
        return v[Rnd() % (sizeof(v) / sizeof(v[0]))];
    }
    case 3: return c.mem.addr_of(Rnd() % c.mem.count());
    case 4: return ((Rnd() % 20) << 4) | 0xC;
    case 5: return Rnd();
    case 6: return Rnd() & 0xFFFF;
    case 7: return 6;
    default: return 0;
    }
}

/// A pair of static buffers: none, rubbish, a good one, one a byte short, a declared capacity that is not the memory's.
static const u32* RandSbuf(Console& c, u32 sb[2]) {
    const u32 r = Rnd() % c.mem.count();
    switch (Rnd() % 10) {
    case 0:
        return nullptr;
    case 1:
        sb[0] = Rnd();
        sb[1] = (Rnd() & 1) ? Rnd() : c.mem.addr_of(r);
        return sb;
    case 2:
        sb[0] = ((Rnd() % 0x2000) << 14) | 2;
        sb[1] = c.mem.addr_of(r);
        return sb;
    case 3:
        sb[0] = ((c.mem.size_of(r) - 1) << 14) | 2;
        sb[1] = c.mem.addr_of(r);
        return sb;
    default:
        sb[0] = (c.mem.size_of(r) << 14) | 2;
        sb[1] = c.mem.addr_of(r) + ((Rnd() % 8 == 0) ? 1 : 0);
        return sb;
    }
}

/// The typical flow of each role, as a script of (id, arguments): the fuzz mixes it in so that scans, joins, START and GO really
/// happen. Console 0 is the creator, console 1 the searcher; `step` is the position in the script.
struct Guided {
    DlpKind kind;
    u32 id;
    u32 args[15];
    int region;  // the buffer the command needs: -1 none, else the index of the region of its console (0 = 0x298, 1 = 0x13A8, 3 = 8)
};
static Guided GuidedStep(int who, u32 step, const Mac& other) {
    Guided g;
    std::memset(&g, 0, sizeof(g));
    g.region = -1;
    if (who == 0) {
        g.kind = kSv;
        switch (step % 11) {
        case 0: g.id = 1; g.args[1] = 3; g.args[2] = 0xE; g.args[3] = 0; break;           // Initialize(3)
        case 1: g.id = 5; g.args[0] = 0; g.args[1] = 6; break;                             // StartAccepting(0, 6)
        case 2: g.id = 0x0B; g.args[0] = 4; g.args[1] = (8u << 4) | 0xC; g.region = 3; break;  // GetConnectingClients
        case 3: g.id = 0x0C; g.args[0] = 2; break;                                         // GetClientInfo(2)
        case 4: g.id = 7; break;                                                           // StartDistribution
        case 5:
        case 6:
        case 7:
        case 9: g.id = 3; break;                                                           // GetServerState
        case 8: g.id = 8; g.args[0] = 0x34333231; g.args[1] = 0x64636261; g.args[2] = 0; break;  // the passphrase
        default: g.id = 2; break;                                                          // Finalize
        }
        return g;
    }
    g.kind = kFk;
    switch (step % 13) {
    case 0: g.id = 1; g.args[0] = 0x30000; g.args[1] = 16; break;                // Initialize
    case 1: g.id = 5; g.args[0] = 0x0421; g.args[1] = static_cast<u32>(kFilterTd); g.args[2] = static_cast<u32>(kFilterTd >> 32); break;
    case 2: g.id = 9; g.args[0] = 1; g.region = 1; break;                        // GetTitleInfoInOrder(1)
    case 3: g.id = 7; g.args[0] = MacW1(other); g.args[1] = MacW2(other); g.region = 0; break;  // GetServerInfo
    case 4: g.id = 6; break;                                                     // StopScan
    case 5: g.id = 0x0B; g.args[0] = MacW1(other); g.args[1] = MacW2(other); break;  // StartFakeSession
    case 6:
    case 7:
    case 8: g.id = 0x0C; break;                                                  // GetMyStatus
    case 9: g.id = 0x0D; g.args[0] = 4; g.region = 3; break;                     // GetConnectingNodes(4)
    case 10: g.id = 0x0E; g.args[0] = 1; break;                                  // GetNodeInfo(1)
    case 11: g.id = 0x0F; break;                                                 // GetWirelessRebootPassphrase
    default: g.id = 2; break;                                                    // Finalize
    }
    return g;
}

static void TestFuzz() {
    FakeAir air(100000);
    Console a(air, 0, MakeCfg(0x0010C000, 0x1000, "Host"));
    Console b(air, 1, MakeCfg(0x0010C000, 0x1001, "Cli1"));
    Console p(air, 2, MakeCfg(0x0010C000, 0x1002, "Probe"));
    p.svc.SetLobby(nullptr);  // a service in ProbeOnly (its lobby is never used)
    air.AddAir(kMacA, MakeAnnounce(0x0010C002, kNameA, 4, 1), 2, 3);
    Console* const cons[3] = {&a, &b, &p};
    const u32 region_sizes[8] = {0x298, 0x13A8, 0x13A8 + 0x40, 8, 6, 2, 0x40, 0x1000};
    for (Console* c : cons) {
        for (u32 sz : region_sizes) {
            c->mem.Alloc(sz, 0, false);
        }
        c->mem.Alloc(0x298, 0, true);  // (exact sizes: an overrun is an ASan report)
        c->mem.Alloc(0x13A8, 0, true);
        c->mem.Alloc(8, 1, false);
        c->mem.Alloc(6, 3, false);
    }
    g_rng_state = 0xF0221ED5u;
    u32 requests[3] = {0, 0, 0};
    bool pending[3] = {false, false, false};
    u32 oks = 0;
    u32 joins_begun = 0;
    u32 joins_ended = 0;
    u32 joins_ok = 0;
    u32 deep_ok = 0;
    bool a_probe = false;
    u32 script_step[2] = {0, 0};
    const Mac macs[3] = {a.net.mac(), b.net.mac(), kMacA};
    for (u32 it = 0; it < 100000;) {
        // the clock: small steps, now and then a jump past the timeouts; the network thread pumps both lobbies
        const u32 tick = Rnd() % 100;
        if (tick < 40) {
            air.Advance(Rnd() % 40);
        } else if (tick < 42) {
            air.Advance(Rnd() % 3000);
        }
        if (tick < 42) {
            if (!a_probe) {
                a.lobby.Pump();
            }
            b.lobby.Pump();
        }
        // now and then: an event from outside
        const u32 ev = Rnd() % 1000;
        if (ev == 1) {
            a.lobby.OnGameUdsStarting();
        } else if (ev == 2) {
            b.lobby.OnGameUdsStarting();
        } else if (ev == 3) {
            a.svc.OnSessionClosed((Rnd() & 1) ? kFk : kSv);
            pending[0] = false;
        } else if (ev == 4) {
            b.svc.OnSessionClosed((Rnd() & 1) ? kFk : kSv);
            pending[1] = false;
        } else if (ev == 5) {  // the service of console a goes from Lobby to ProbeOnly and back
            a_probe = !a_probe;
            a.svc.SetLobby(a_probe ? nullptr : &a.lobby);
            pending[0] = false;
        }
        const u32 pick = Rnd() % 10;
        const int who = pick < 4 ? 0 : pick < 8 ? 1 : 2;
        Console& c = *cons[who];
        // a join still running: sometimes the game's thread asks again
        if (pending[who] && (Rnd() & 1)) {
            bool done = false;
            const Reply r = c.Pending(&done);
            if (done) {
                pending[who] = false;
                ++joins_ended;
                CHECK_EQ(r.hdr(), 0x000B0040u);
                CHECK(AllowedResult(r.res()));
                if (r.res() == 0) {
                    ++joins_ok;
                }
            }
            continue;
        }
        DlpKind kind = (Rnd() & 1) ? kFk : kSv;
        u32 id;
        u32 args[15];
        u32 sb[2];
        const u32* sbuf;
        if (who < 2 && Rnd() % 100 < 45) {  // the typical flow of the console's role, with real arguments and buffers
            const Guided g = GuidedStep(who, script_step[who]++, macs[1 - who]);
            kind = g.kind;
            id = g.id;
            std::memcpy(args, g.args, sizeof(args));
            sbuf = nullptr;
            if (g.region >= 0) {
                const u32 r = static_cast<u32>(g.region);
                if (kind == kFk) {
                    sb[0] = (c.mem.size_of(r) << 14) | 2;
                    sb[1] = c.mem.addr_of(r);
                    sbuf = sb;
                } else {
                    args[2] = c.mem.addr_of(r);
                }
            }
        } else {
            const u32 last = kind == kFk ? 0x11u : 0x10u;
            id = (Rnd() % 10 != 0) ? 1 + Rnd() % last : Rnd() % 0x40;
            for (u32& x : args) {
                x = RandWord(c);
            }
            if (Rnd() % 3 == 0) {  // the MAC of a real lobby, or a node id of a real player
                const Mac& m = macs[Rnd() % 3];
                args[0] = MacW1(m);
                args[1] = MacW2(m);
            } else if (Rnd() % 3 == 0) {
                args[0] = Rnd() % 6;
            }
            if (kind == kSv && id == 0x05 && Rnd() % 2) {
                args[0] = Rnd() & 1;  // StartAccepting: a plausible manual flag and channel
                static const u32 chans[4] = {0, 1, 6, 11};
                args[1] = chans[Rnd() % 4];
            }
            if (kind == kFk && id == 0x05 && Rnd() % 2) {
                args[1] = static_cast<u32>(kFilterTd);  // StartScan: the real filter (low, high)
                args[2] = static_cast<u32>(kFilterTd >> 32);
            }
            sbuf = RandSbuf(c, sb);
        }
        const u32 header = (id << 16) | (Rnd() & 0xFFFF);
        const bool lobby_mode = c.svc.mode() == DlpMode::Lobby;
        const Reply rp = c.CallN(kind, header, args, 15, sbuf, nullptr);
        ++requests[who];
        ++it;
        if (rp.out == DlpOutcome::JoinPending) {
            CHECK(lobby_mode && kind == kFk && id == 0x0B);
            pending[who] = true;
            ++joins_begun;
            continue;
        }
        CheckFuzzReply(kind, id, rp, lobby_mode);
        CHECK_EQ(c.svc.last_id(), id);
        CHECK_EQ(c.svc.last_result(), rp.res());
        if (rp.res() == 0) {
            ++oks;
            if (lobby_mode && ((kind == kSv && (id == 0x07 || id == 0x08)) || (kind == kFk && (id == 0x07 || id == 0x08 || id == 0x09)))) {
                ++deep_ok;  // a command that reached its work: a lobby of the table or a distribution phase
            }
        }
    }
    std::printf("  fuzz: %u ok replies, %u of them deep, joins begun %u ended %u ok %u\n", oks, deep_ok, joins_begun, joins_ended, joins_ok);
    // the books
    CHECK_EQ(requests[0], a.svc.commands());
    CHECK_EQ(requests[1], b.svc.commands());
    CHECK_EQ(requests[2], p.svc.commands());
    CHECK_EQ(requests[0] + requests[1] + requests[2], 100000u);
    for (Console* c : cons) {
        CHECK(c->svc.unknown_commands() + c->svc.bad_pointer() <= c->svc.refused());
        CHECK(c->mem.Intact());
    }
    CHECK(p.svc.mode() == DlpMode::ProbeOnly);
    CHECK_EQ(p.svc.bad_pointer(), 0u);
    CHECK_EQ(p.mem.map_calls, 0u);  // a probe never reads a buffer
    CHECK(a.svc.bad_pointer() > 0 && b.svc.bad_pointer() > 0);  // the fuzz did try bad pointers
    CHECK(deep_ok > 0);
}

// =====================================================================================================================
// Gaps found by the attack and the mutation run (DESIGN-FASE1.md sez. 16). Each block kills mutants that the tests above let
// live, or pins a decision of the review.

// A way into the private saturating counter of the service, for the one test of its saturation (an explicit instantiation is
// exempt from the access check; no production code is touched).
template <typename Tag, typename Tag::type M>
struct Robber {
    friend typename Tag::type Steal(Tag) { return M; }
};
struct CommandsTag {
    using type = u32 DlpService::*;
    friend type Steal(CommandsTag);
};
template struct Robber<CommandsTag, &DlpService::commands_>;

/// A resolver that accepts ANY address, 0 included, and counts how often it was asked: a service that lets an address 0 (or a
/// wrong size) through shows at once.
struct AnyMap {
    u32 calls = 0;
    u32 last_size = 0;
    std::vector<u8> buf = std::vector<u8>(0x2000);
    static u8* Thunk(void* ctx, u32 /*addr*/, u32 size) {
        AnyMap* m = static_cast<AnyMap*>(ctx);
        ++m->calls;
        m->last_size = size;
        return size <= m->buf.size() ? m->buf.data() : nullptr;
    }
    IpcResolver Resolver() {
        IpcResolver r;
        r.ctx = this;
        r.map = &Thunk;
        return r;
    }
};

static void TestDescriptorKinds() {
    std::printf("descriptor kinds, address 0, sizes asked of the resolver\n");
    // a static-buffer descriptor whose type is not exactly "static buffer 0" (type nibble 3, 0xA, 6, 0): invalid pointer
    {
        World w(2, 9000);
        ThreeLobbiesHeard(w);
        Console& c = w[1];
        const u32 kinds[4] = {3, 0xA, 6, 0};
        for (int k = 0; k < 3; ++k) {
            const u32 size = k == 0 ? 0x298 : 0x13A8;
            const Sb good = c.NewSb(size);
            for (int j = 0; j < 4; ++j) {
                const u32 d[2] = {(size << 14) | kinds[j], good.addr};
                Reply r;
                if (k == 0) {
                    r = c.Call(kFk, 0x00070080, {MacW1(kMacA), MacW2(kMacA)}, d);
                } else if (k == 1) {
                    r = c.Call(kFk, 0x00080100, {MacW1(kMacA), MacW2(kMacA), 0x0010C000, 0x00040001}, d);
                } else {
                    r = c.Call(kFk, 0x00090040, {1}, d);
                }
                CHECK_EQ(r.res(), R_BAD_POINTER);
                CHECK(c.mem.Untouched(good.addr));
            }
        }
        World w2(4);
        HostWithClients(w2, 3);
        Console& s = w2[1];
        const Sb big = s.NewSb(0x100);
        for (int j = 0; j < 4; ++j) {
            const u32 d[2] = {(8u << 14) | kinds[j], big.addr};
            const Reply r = s.Call(kFk, 0x000D0040, {4}, d);
            CHECK_EQ(r.res(), R_BAD_POINTER);
            CHECK(s.mem.Untouched(big.addr));
        }
    }
    // the address 0 is never mapped, whatever the resolver says
    {
        World w(2, 9000);
        ThreeLobbiesHeard(w);
        Console& c = w[1];
        AnyMap any;
        const IpcResolver res = any.Resolver();
        const u32 z298[2] = {(0x298u << 14) | 2, 0};
        const u32 z13a8[2] = {(0x13A8u << 14) | 2, 0};
        CHECK_EQ(c.Call(kFk, 0x00070080, {MacW1(kMacA), MacW2(kMacA)}, z298, &res).res(), R_BAD_POINTER);
        CHECK_EQ(c.Call(kFk, 0x00080100, {MacW1(kMacA), MacW2(kMacA), 0x0010C000, 0x00040001}, z13a8, &res).res(), R_BAD_POINTER);
        CHECK_EQ(c.Call(kFk, 0x00090040, {1}, z13a8, &res).res(), R_BAD_POINTER);
        CHECK_EQ(any.calls, 0u);
        // the same resolver with a real address IS asked, with the size of the record
        const u32 g298[2] = {(0x298u << 14) | 2, 0x1000};
        const u32 g13a8[2] = {(0x13A8u << 14) | 2, 0x1000};
        CHECK_EQ(c.Call(kFk, 0x00070080, {MacW1(kMacA), MacW2(kMacA)}, g298, &res).res(), 0u);
        CHECK_EQ(any.calls, 1u);
        CHECK_EQ(any.last_size, 0x298u);
        CHECK_EQ(c.Call(kFk, 0x00080100, {MacW1(kMacA), MacW2(kMacA), 0x0010C000, 0x00040001}, g13a8, &res).res(), 0u);
        CHECK_EQ(any.last_size, 0x13A8u);
        CHECK_EQ(c.Call(kFk, 0x00090040, {1}, g13a8, &res).res(), 0u);
        CHECK_EQ(any.last_size, 0x13A8u);
        CHECK_EQ(any.calls, 3u);

        World w2(4);
        HostWithClients(w2, 3);
        Console& s = w2[1];
        Console& h = w2[0];
        AnyMap any2;
        const IpcResolver res2 = any2.Resolver();
        const u32 zn[2] = {(8u << 14) | 2, 0};
        CHECK_EQ(s.Call(kFk, 0x000D0040, {4}, zn, &res2).res(), R_BAD_POINTER);
        CHECK_EQ(h.Call(kSv, 0x000B0042, {4, (8u << 4) | 0xC, 0}, nullptr, &res2).res(), R_BAD_POINTER);
        CHECK_EQ(any2.calls, 0u);
        const u32 gn[2] = {(8u << 14) | 2, 0x1000};
        CHECK_EQ(s.Call(kFk, 0x000D0040, {4}, gn, &res2).res(), 0u);
        CHECK_EQ(any2.calls, 1u);
        CHECK_EQ(any2.last_size, 8u);  // 4 ids of 2 bytes: the size written, not the number of ids
        CHECK_EQ(h.Call(kSv, 0x000B0042, {4, (8u << 4) | 0xC, 0x1000}, nullptr, &res2).res(), 0u);
        CHECK_EQ(any2.calls, 2u);
        CHECK_EQ(any2.last_size, 8u);
    }
    // the size asked of the game's memory is the size the service is going to write
    {
        World w(4);
        HostWithClients(w, 3);
        Console& c = w[1];
        Console& h = w[0];
        Sb b = c.NewSb(8, 0, true);
        CHECK_EQ(c.FkNodes(4, b).res(), 0u);
        CHECK_EQ(c.mem.last_map_size, 8u);
        Sb big = c.NewSb(0x100);
        CHECK_EQ(c.FkNodes(2, big).res(), 0u);
        CHECK_EQ(c.mem.last_map_size, 4u);
        CHECK_EQ(c.FkNodes(3, big).res(), 0u);
        CHECK_EQ(c.mem.last_map_size, 6u);
        const u32 addr = h.mem.Alloc(0x40);
        CHECK_EQ(h.SvClients(4, 0x40, addr).res(), 0u);
        CHECK_EQ(h.mem.last_map_size, 0x40u);  // the buffer the game declared
        World w2(2, 9000);
        ThreeLobbiesHeard(w2);
        Console& d = w2[1];
        Sb s0 = d.NewSb(0x298);
        Sb t0 = d.NewSb(0x13A8);
        CHECK_EQ(d.FkServerInfo(kMacA, s0).res(), 0u);
        CHECK_EQ(d.mem.last_map_size, 0x298u);
        CHECK_EQ(d.FkTitleInfo(kMacA, t0).res(), 0u);
        CHECK_EQ(d.mem.last_map_size, 0x13A8u);
        CHECK_EQ(d.FkInOrder(1, t0).res(), 0u);
        CHECK_EQ(d.mem.last_map_size, 0x13A8u);
    }
}

static void TestServiceGaps() {
    std::printf("service gaps\n");
    // PollPending without a join the SERVICE began: the answer is the invalid state, and the lobby's outcome is not collected
    {
        World w(2);
        HostUp(w);
        Console& c = w[1];
        c.FkInit();
        c.FkScan(kFilterTd);
        w.Run(60);
        c.FkStopScan();
        const Reply j = c.FkJoin(MacOf(w[0]));
        CHECK(j.out == DlpOutcome::JoinPending);
        CHECK(c.svc.join_pending());
        c.svc.SetLobby(&c.lobby);  // the service forgets the join; the lobby goes on with it
        CHECK(!c.svc.join_pending());
        w.Run(200);
        CHECK_EQ(c.lobby.client_state(), 5u);
        bool done = false;
        const Reply r = c.Pending(&done);
        CHECK(done);
        CHECK_EQ(r.hdr(), 0x000B0040u);
        CHECK_EQ(r.res(), R_INVALID_STATE);
        u32 res = 99;
        CHECK(c.lobby.ClientJoinPoll(&res));
        CHECK_EQ(res, 0u);  // the lobby's outcome was still there: PollPending did not take it
    }
    // the counters saturate at 0xFFFFFFFF and stay
    {
        World w(1);
        Console& c = w[0];
        c.svc.*Steal(CommandsTag()) = 0xFFFFFFFEu;
        c.FkStatus();
        CHECK_EQ(c.svc.commands(), 0xFFFFFFFFu);
        c.FkStatus();
        CHECK_EQ(c.svc.commands(), 0xFFFFFFFFu);
        c.FkStatus();
        CHECK_EQ(c.svc.commands(), 0xFFFFFFFFu);
    }
    // the clash of names: the UDS engine's header and the DLP ones live in one translation unit (the plugin's uds_hle.cpp)
    CHECK_EQ(kDlpNodeInfoSize, 0x28u);
    CHECK_EQ(kNodeInfoSize, 40u);
}

/// The number of lobbies the table keeps after an Initialize with the words [1] and [2] (`with_name`: InitializeWithName).
static u32 EntriesAfterInit(bool with_name, u32 w1, u32 w2) {
    World w(2, 9000);
    for (int g = 0; g < 6; ++g) {
        const Mac mac = {0x40, 0xF4, 0x07, 0x99, 0x88, static_cast<u8>(0x70 + g)};
        w.air.AddAir(mac, MakeAnnounce(0x0010C000, kNameC, 4, 1), 2, 3);
    }
    Console& c = w[1];
    if (with_name) {
        u32 n[5];
        NameWords(kNameA, n);
        CHECK_EQ(c.Call(kFk, 0x00110203, {w1, w2, n[0], n[1], n[2], n[3], n[4], 0, 0x04000000, 0x3333, 0x4444}).res(), 0u);
    } else {
        CHECK_EQ(c.Call(kFk, 0x00010083, {w1, w2, 0x04000000, 0x1111, 0x2222}).res(), 0u);
    }
    c.FkScan(kFilterTd);
    c.lobby.Pump();
    Sb t = c.NewSb(0x13A8);
    u32 n = 0;
    Reply r = c.FkInOrder(1, t);
    while (r.res() == 0 && n < 10) {
        ++n;
        r = c.FkInOrder(0, t);
    }
    return n;
}

static void TestWidthsGaps() {
    std::printf("widths: the words that are not the one read\n");
    // FKCL Initialize / InitializeWithName: the number of beacons is the whole word [2] (u32), limited to 1..4; [1] is the size
    // of the shared memory and is not it
    struct Case {
        u32 w1, w2, want;
    };
    const Case cases[] = {{0x30000, 0x100, 4}, {0x30000, 2, 2},      {2, 4, 4},         {0x10000, 3, 3},
                          {0x30000, 0x103, 4}, {0x30000, 0xFFFFFFFFu, 4}, {0x30000, 0, 1}, {0x30000, 1, 1},
                          {4, 0x1000000, 4},   {1, 2, 2}};
    for (const Case& k : cases) {
        CHECK_EQ(EntriesAfterInit(false, k.w1, k.w2), k.want);
        CHECK_EQ(EntriesAfterInit(true, k.w1, k.w2), k.want);
    }
    // SRVR InitializeWithName: [2] is a byte (0x102 is 2 clients: 3 nodes), [4] is a byte (0x1234 is 0x34)
    {
        World w(2, 9000);
        CHECK_EQ(w[0].SvInitName(0x102, 0x1234, kNameA).res(), 0u);
        CHECK_EQ(w[0].SvAccepting(0, 6).res(), 0u);
        w.Run(40);
        Console& c = w[1];
        c.FkInit();
        c.FkScan(kFilterTd);
        w.Run(60);
        Sb s = c.NewSb(0x298);
        CHECK_EQ(c.FkServerInfo(MacOf(w[0]), s).res(), 0u);
        CHECK_EQ(c.Data(s)[8], 3u);
        Sb t = c.NewSb(0x13A8);
        CHECK_EQ(c.FkInOrder(1, t).res(), 0u);
        CHECK_EQ(LoadLE32(c.Data(t) + 4), 0x34u);
    }
    // StartAccepting: "manual" is the whole low byte of [1] (2 and 0x80 are manual, 0x100 and 0x10000 are not)
    {
        const u32 words[5] = {2, 0x80, 0x100, 0x10000, 0x03a12000};
        const u32 want[5] = {0x40, 0x40, 5, 5, 5};
        for (int k = 0; k < 5; ++k) {
            World w(2);
            Console& h = w[0];
            Console& c = w[1];
            h.SvInit(3, 0);
            CHECK_EQ(h.SvAccepting(words[k], 6).res(), 0u);
            w.Run(40);
            c.FkInit();
            c.FkScan(kFilterTd);
            w.Run(60);
            c.FkStopScan();
            CHECK(JoinThroughIpc(w, 1));
            w.Run(60);
            CHECK_EQ(h.SvClientState(2).w[2] >> 24, want[k]);
        }
    }
    // GetConnectingNodes / GetConnectingClients: the element count is the whole u16 ([1] = 0x100 is 256, not 0)
    {
        World w(4);
        HostWithClients(w, 3);
        Console& c = w[1];
        Console& h = w[0];
        const u8 all[8] = {1, 0, 2, 0, 3, 0, 4, 0};
        Sb big = c.NewSb(0x100);
        Reply r = c.FkNodes(0x100, big);
        CHECK_EQ(r.res(), 0u);
        CHECK_EQ(r.w[2], 4u);
        CHECK_EQ(r.w[3], 0x00020002u);  // 8 bytes written
        CHECK_BYTES(c.Data(big), all, 8);
        c.mem.Refill(big.addr);
        r = c.FkNodes(0x10004, big);  // 4 (the upper half is not read)
        CHECK_EQ(r.w[3], 0x00020002u);
        CHECK_BYTES(c.Data(big), all, 8);
        const u8 ids3[6] = {2, 0, 3, 0, 4, 0};
        u32 addr = h.mem.Alloc(0x40);
        r = h.SvClients(0x100, 0x40, addr);
        CHECK_EQ(r.res(), 0u);
        CHECK_EQ(r.w[2], 3u);
        CHECK_BYTES(h.mem.Find(addr, 6), ids3, 6);
        h.mem.Refill(addr);
        r = h.SvClients(0x10003, 0x40, addr);
        CHECK_EQ(r.w[2], 3u);
        CHECK_BYTES(h.mem.Find(addr, 6), ids3, 6);
    }
}

int main() {
    std::printf("sizes and helpers\n");
    TestSizesAndHelpers();
    std::printf("ProbeOnly = DlpFkclHle\n");
    TestProbeOnlyIsDlpFkclHle();
    TestProbeOnlyServer();
    TestMatrixProbeOnly();
    std::printf("unknown ids, handles, books\n");
    TestUnknownIdsInLobbyMode();
    TestHandlesIgnored();
    TestCounters();
    std::printf("matrix: searcher\n");
    TestMatrixSearcher();
    std::printf("matrix: creator\n");
    TestMatrixCreator();
    std::printf("records\n");
    TestRecordsByteForByte();
    TestNodeInfoRecords();
    TestTickArithmetic();
    TestTickAges();
    std::printf("buffers\n");
    TestStaticBuffers();
    TestConnectingNodesBuffer();
    TestConnectingClientsBuffer();
    std::printf("widths\n");
    TestWidths();
    std::printf("StartFakeSession in two steps\n");
    TestJoinFlow();
    TestJoinThreads();
    std::printf("Azahar defects not copied\n");
    DefectAzahar1_ScanChannelMaskIsNotAsserted();
    DefectAzahar2_NoState5AfterAFailedJoin();
    DefectAzahar3_ClientStateOutsideTheVector();
    DefectAzahar4_ServerFinalizeResetsTheState();
    DefectAzahar5_NoMinusOneAsAnErrorCode();
    DefectAzahar6_ChildIndexIsNotIgnored();
    DefectAzahar7_HostIsCountedInTheClientsOnPurpose();
    std::printf("session closed, names, passphrase\n");
    TestSessionClosed();
    TestNamesAndPassphraseWords();
    std::printf("complete rounds (Triple Deluxe and Battle Royale order)\n");
    TestDispatchPumps();
    TestCompleteRounds();
    std::printf("gaps found by the attack and the mutation run\n");
    TestDescriptorKinds();
    TestServiceGaps();
    TestWidthsGaps();
    std::printf("fuzz\n");
    TestFuzz();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
