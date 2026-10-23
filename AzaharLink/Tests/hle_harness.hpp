// Shared parts of the tests that drive the real IPC front-end (ctr/uds_hle.cpp) the way a game does:
// a console = engine + UdsHle on a counting kernel, sessions opened through the intercepted
// srv:GetServiceHandle, requests built word by word with the headers of the spec (T9).
// Built like Tests/ipc_test (-m32 and the fake kernel in ipc_test/shim_svc.cpp): the IPC words carry
// 32-bit pointers.
#pragma once

#include <atomic>
#include <deque>
#include <initializer_list>

#include <3ds.h>

#include "azl/stats.hpp"
#include "ctr/uds_hle.hpp"

#include "azahar_peer.hpp"
#include "mock_room.hpp"

static_assert(sizeof(void*) == 4, "the HLE tests are 32-bit: IPC words carry 32-bit pointers");

extern azl::ctr::UdsHle* g_test_hle; // the HLE the fake srvGetServiceHandle() talks to (shim_svc.cpp)
extern "C" Result srvGetServiceHandle(Handle* out, const char* name);

// ---------------------------------------------------------------------------------------------
// Result codes of the firmware (spec T1)
// ---------------------------------------------------------------------------------------------
namespace fw {
constexpr u32 kW = 0xC8A113EA;      // wrong state
constexpr u32 kWO = 0xC9411002;     // wireless off
constexpr u32 kBadId = 0xE10113EA;  // unknown bind id / node 1 can't be kicked
constexpr u32 kBig = 0xE10113E9;    // too large
constexpr u32 kArg = 0xE10113FD;    // wrong argument
constexpr u32 kOom = 0xC86113F3;    // out of resource (bind)
constexpr u32 kAgg = 0xC86113F0;    // aggregate full
constexpr u32 kNodeNotFound = 0xC90113FA;
constexpr u32 kNoHost = 0xC88113FA;
constexpr u32 kConnFailed = 0xC92113FB;
constexpr u32 kConnTimeout = 0xC92113FE;
constexpr u32 kNetFull = 0xC8611001;
constexpr u32 kBadBeacon = 0xE1211005;
constexpr u32 kInitFail = 0xE06113F9;
constexpr u32 kBadCmd = 0xD900182F;
constexpr u32 kBadHeader = 0xD9001830;
} // namespace fw

// ---------------------------------------------------------------------------------------------
// A kernel that counts how often every event was signalled, really sleeps (a scan takes its 300 ms,
// unless fast_sleep) and can hold or cut short ConnectToNetwork's 5 s wait so that a test can look at
// the engine in the "connecting" state.
// ---------------------------------------------------------------------------------------------
class CountingKernel : public FakeKernel { // (FakeKernel::Signals counts the signals)
public:
    void SleepNs(s64 ns) override {
        if (!fast_sleep) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        }
    }
    /// What the 5 s wait of ConnectToNetwork does: really wait (kNormal), stay in it until the mode
    /// changes and then time out (kHold: lets a test look at the "connecting" state), or time out at
    /// once (kFail).
    enum ConnectMode { kNormal, kHold, kFail };
    bool WaitEvent(u32 h, s64 ns) override {
        if (ns < 1000000000LL || connect_mode.load() == kNormal) { // only ConnectToNetwork waits that long
            return FakeKernel::WaitEvent(h, ns);
        }
        held.fetch_add(1);
        bool signalled = false;
        while (connect_mode.load() == kHold && !(signalled = FakeKernel::WaitEvent(h, 2000000))) {
        }
        held.fetch_sub(1);
        return signalled; // released without an answer: the host never replied
    }

    bool fast_sleep = false;
    std::atomic<int> connect_mode{kNormal};
    std::atomic<int> held{0};
};

// ---------------------------------------------------------------------------------------------
// The words of one request / reply, and the builders of the requests
// ---------------------------------------------------------------------------------------------
struct Reply {
    u32 w[64];
    u32 result() const {
        return w[1];
    }
    /// Words 2..13 of GetConnectionStatus.
    ConnectionStatus Status() const {
        ConnectionStatus s;
        std::memcpy(&s, &w[2], sizeof(s));
        return s;
    }
};

namespace req {

inline std::vector<u32> Make(u32 id, unsigned normal, unsigned translate) {
    std::vector<u32> v(1 + normal + translate, 0);
    v[0] = IPC_MakeHeader(static_cast<u16>(id), normal, translate);
    return v;
}
inline u32 Ptr(const void* p) {
    return reinterpret_cast<u32>(p);
}

inline std::vector<u32> Initialize(const NodeInfo& node, u32 shared_memory_size, u16 version) {
    std::vector<u32> v = Make(0x1B, 12, 2);
    v[1] = shared_memory_size;
    node.Store(reinterpret_cast<u8*>(&v[2]));
    v[12] = version;
    v[13] = IPC_Desc_SharedHandles(1);
    v[14] = 0x00130003; // the game's shared memory block (never touched)
    return v;
}
inline std::vector<u32> Shutdown() {
    return Make(0x03, 0, 0);
}
inline std::vector<u32> BeginHosting(const NetworkInfo* network) {
    static const char kPassphrase[] = "passphrase";
    std::vector<u32> v = Make(0x1D, 1, 4);
    v[1] = sizeof(kPassphrase);
    v[2] = IPC_Desc_StaticBuffer(sizeof(NetworkInfo), 1);
    v[3] = Ptr(network);
    v[4] = IPC_Desc_StaticBuffer(sizeof(kPassphrase), 0);
    v[5] = Ptr(kPassphrase);
    return v;
}
inline std::vector<u32> Connect(u8 type, const NetworkInfo* network) {
    static const char kPassphrase[] = "passphrase";
    std::vector<u32> v = Make(0x1E, 2, 4);
    v[1] = type;
    v[2] = 0x66; // unused word, as FFE sends it
    v[3] = IPC_Desc_StaticBuffer(sizeof(NetworkInfo), 1);
    v[4] = Ptr(network);
    v[5] = IPC_Desc_StaticBuffer(0x66, 0);
    v[6] = Ptr(kPassphrase);
    return v;
}
inline std::vector<u32> Disconnect() {
    return Make(0x0A, 0, 0);
}
inline std::vector<u32> Destroy() {
    return Make(0x08, 0, 0);
}
inline std::vector<u32> GetConnectionStatus() {
    return Make(0x0B, 0, 0);
}
inline std::vector<u32> Eject(u16 node) {
    std::vector<u32> v = Make(0x05, 1, 0);
    v[1] = node;
    return v;
}
inline std::vector<u32> GetNodeInformation(u16 node) {
    std::vector<u32> v = Make(0x0D, 1, 0);
    v[1] = node;
    return v;
}
/// RecvBeaconBroadcastData with the scan input words FFE sends: passive scan, mode 2, channels
/// 1/6/11, 110 ms, any BSSID. `cancel` is the game's event handle in word 18.
inline std::vector<u32> Scan(u8* buffer, u32 size, u32 wlancommid, u8 id8, u32 cancel) {
    std::vector<u32> v = Make(0x0F, 16, 4);
    v[1] = size;
    v[2] = 0x00020001;
    v[3] = 0x006e0421;
    v[4] = 0xffffffff;
    v[5] = 0x0000ffff;
    v[15] = wlancommid;
    v[16] = id8;
    v[17] = IPC_Desc_SharedHandles(1);
    v[18] = cancel;
    v[19] = (size << 4) | 0xC;
    v[20] = Ptr(buffer);
    return v;
}
inline std::vector<u32> ScanOnConnection(u8* buffer, u32 size, u32 wlancommid, u8 id8) {
    std::vector<u32> v = Make(0x22, 16, 2);
    v[1] = size;
    v[2] = 0x00020001;
    v[3] = 0x006e0421;
    v[4] = 0xffffffff;
    v[5] = 0x0000ffff;
    v[15] = wlancommid;
    v[16] = id8;
    v[17] = (size << 4) | 0xC;
    v[18] = Ptr(buffer);
    return v;
}
inline std::vector<u32> DecryptBeacon(const NetworkInfo* network, const u8* tag0, u32 tag0_size, const u8* tag1,
                                      u32 tag1_size) {
    std::vector<u32> v = Make(0x1F, 0, 6);
    v[1] = IPC_Desc_StaticBuffer(sizeof(NetworkInfo), 1);
    v[2] = Ptr(network);
    v[3] = IPC_Desc_StaticBuffer(tag0_size, 2);
    v[4] = Ptr(tag0);
    v[5] = IPC_Desc_StaticBuffer(tag1_size, 3);
    v[6] = Ptr(tag1);
    return v;
}
inline std::vector<u32> SetApplicationData(const u8* data, u32 size) {
    std::vector<u32> v = Make(0x10, 1, 2);
    v[1] = size;
    v[2] = IPC_Desc_StaticBuffer(size, 4);
    v[3] = Ptr(data);
    return v;
}
inline std::vector<u32> GetApplicationData(u32 capacity) {
    std::vector<u32> v = Make(0x11, 1, 0);
    v[1] = capacity;
    return v;
}
inline std::vector<u32> Bind(u32 id, u32 recv_size, u8 channel, u16 src) {
    std::vector<u32> v = Make(0x12, 4, 0);
    v[1] = id;
    v[2] = recv_size;
    v[3] = channel;
    v[4] = src;
    return v;
}
inline std::vector<u32> Unbind(u32 id) {
    std::vector<u32> v = Make(0x13, 1, 0);
    v[1] = id;
    return v;
}
inline std::vector<u32> Pull(u32 id, u32 aligned_words = 0x172, u32 max_size = 0x5C6) {
    std::vector<u32> v = Make(0x14, 3, 0);
    v[1] = id;
    v[2] = aligned_words;
    v[3] = max_size;
    return v;
}
inline std::vector<u32> SendTo(u16 dest, u8 channel, u8 flags, const u8* data, u32 size) {
    std::vector<u32> v = Make(0x17, 6, 2);
    v[2] = dest;
    v[3] = channel;
    v[4] = (size + 3) / 4;
    v[5] = size;
    v[6] = flags;
    v[7] = IPC_Desc_StaticBuffer(size, 5);
    v[8] = Ptr(data);
    return v;
}
/// A command that takes one or two plain words after the header.
inline std::vector<u32> Words(u32 id, unsigned normal, u32 a = 0, u32 b = 0) {
    std::vector<u32> v = Make(id, normal, 0);
    if (normal > 0) {
        v[1] = a;
    }
    if (normal > 1) {
        v[2] = b;
    }
    return v;
}

// ndm:u
inline std::vector<u32> NdmMask(u32 id, u32 mask) { // 0x06 SuspendDaemons, 0x14 OverrideDefaultDaemons ...
    return Words(id, 1, mask);
}
// (`pid_word` false = libctru's way: it writes the descriptor and leaves the PID word as the buffer
// held it, because the kernel fills it in; the harness pre-fills the buffer with 0xA5A5A5A5.)
inline std::vector<u32> NdmEnter(u32 state, bool pid_word = true) {
    std::vector<u32> v = Make(0x01, 1, 2);
    v[1] = state;
    v[2] = IPC_Desc_CurProcessId();
    if (!pid_word) {
        v.resize(3);
    }
    return v;
}
inline std::vector<u32> NdmLeave(bool pid_word = true) {
    std::vector<u32> v = Make(0x02, 0, 2);
    v[1] = IPC_Desc_CurProcessId();
    if (!pid_word) {
        v.resize(2);
    }
    return v;
}
inline std::vector<u32> NdmLock(u32 id, bool pid_word = true) { // 0x04 LockState, 0x05 UnlockState
    std::vector<u32> v = Make(id, 0, 2);
    v[1] = IPC_Desc_CurProcessId();
    if (!pid_word) {
        v.resize(2);
    }
    return v;
}

} // namespace req

// ---------------------------------------------------------------------------------------------
// A console of the test: Console (engine on the mock room) + UdsHle
// ---------------------------------------------------------------------------------------------
class HleConsole {
public:
    HleConsole(MockRoom& room, u8 mac_last) : console(room, mac_last, &kernel) {
        hle.reset(new azl::ctr::UdsHle(console.engine.get()));
    }

    /// srv:GetServiceHandle through the interception: returns the fake session ("nwm::UDS", "ndm:u").
    Handle Open(const char* name) {
        g_test_hle = hle.get();
        Handle h = 0;
        const Result rc = srvGetServiceHandle(&h, name);
        CHECK_EQ(rc, 0);
        return h;
    }
    void Close(Handle h) {
        hle->NoteHandleClosed(h);
        svcCloseHandle(h);
    }

    /// Runs one request through UdsHle on the calling thread. `out_static` / `out_size` prepare the
    /// output static buffer of the thread (word 0x180 of the TLS on a console).
    Reply Call(Handle h, const std::vector<u32>& request, void* out_static = nullptr, u32 out_size = 0) {
        g_test_hle = hle.get();
        u32* cmd = getThreadCommandBuffer();
        std::memset(cmd, 0xA5, 64 * sizeof(u32)); // stale words, like a real TLS
        std::memcpy(cmd, request.data(), request.size() * sizeof(u32));
        u32* sb = getThreadStaticBuffers();
        sb[0] = IPC_Desc_StaticBuffer(out_size, 0);
        sb[1] = req::Ptr(out_static);
        Result rc = -1;
        const bool handled = hle->HandleSendSyncRequest(h, &rc);
        Reply r;
        std::memcpy(r.w, cmd, sizeof(r.w));
        if (!handled || rc != 0) {
            std::fprintf(stderr, "  HLE did not handle request 0x%08lx (handled=%d rc=0x%lx)\n",
                         static_cast<unsigned long>(request[0]), handled, static_cast<unsigned long>(rc));
            ++g_failures;
        }
        ++g_checks;
        return r;
    }

    CountingKernel kernel;
    Console console;
    std::unique_ptr<azl::ctr::UdsHle> hle;
};

// ---------------------------------------------------------------------------------------------
// The debug capture, read back as the lines the plugin would write
// ---------------------------------------------------------------------------------------------
class DebugLines {
public:
    DebugLines() : before_(g_debug_level) {
        CHECK(DebugInit());
        g_debug_level = 1;
        // what an earlier scope of this process left in the ring is not this one's
        DebugRecord stale;
        u8 buf[1700];
        while (DebugPop(&stale, buf, sizeof(buf))) {
        }
    }
    ~DebugLines() {
        g_debug_level = before_;
    }
    /// Moves what the capture holds into the list (each line without the "     0.000 " time).
    void Drain() {
        DebugRecord rec;
        u8 buf[1700];
        while (DebugPop(&rec, buf, sizeof(buf))) {
            std::string line;
            DebugFormat(rec, buf, rec.ms, &line);
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            raw_.push_back(line);
            lines_.push_back(line.size() > 11 ? line.substr(11) : line);
        }
    }
    /// Everything captured so far, with the times, as AzaharLink.debug.log would hold it.
    bool WriteFile(const char* path) {
        Drain();
        FILE* f = std::fopen(path, "w");
        if (!f) {
            return false;
        }
        for (const std::string& l : raw_) {
            std::fputs(l.c_str(), f);
            std::fputc(10, f);
        }
        std::fclose(f);
        return true;
    }
    /// The newest line that starts with `prefix`, or "".
    std::string Last(const std::string& prefix) {
        Drain();
        for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
            if (it->compare(0, prefix.size(), prefix) == 0) {
                return *it;
            }
        }
        return "";
    }
    int Count(const std::string& prefix) {
        Drain();
        int n = 0;
        for (const std::string& l : lines_) {
            if (l.compare(0, prefix.size(), prefix) == 0) {
                ++n;
            }
        }
        return n;
    }
    /// Words between "req" and "->" and after "rep" of a command line: {request words, reply words}.
    static std::pair<int, int> WordCounts(const std::string& line) {
        std::vector<std::string> words;
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && line[i] == ' ') {
                ++i;
            }
            std::size_t j = i;
            while (j < line.size() && line[j] != ' ') {
                ++j;
            }
            if (j > i) {
                words.push_back(line.substr(i, j - i));
            }
            i = j;
        }
        int req_at = -1, arrow = -1, rep_at = -1;
        for (std::size_t k = 0; k < words.size(); ++k) {
            if (words[k] == "req" && req_at < 0) {
                req_at = static_cast<int>(k);
            } else if (words[k] == "->" && arrow < 0) {
                arrow = static_cast<int>(k);
            } else if (words[k] == "rep" && rep_at < 0 && arrow >= 0) {
                rep_at = static_cast<int>(k);
            }
        }
        if (req_at < 0 || arrow < 0 || rep_at < 0) {
            return {-1, -1};
        }
        return {arrow - req_at - 1, static_cast<int>(words.size()) - rep_at - 1};
    }

private:
    int before_; ///< the level it gives back
    std::deque<std::string> lines_;
    std::vector<std::string> raw_;
};

// ---------------------------------------------------------------------------------------------
// A test that hangs must fail loudly, not stall the pipeline: say where and abort.
// ---------------------------------------------------------------------------------------------
class Watchdog {
public:
    explicit Watchdog(int seconds) : limit_ms_(seconds * 1000), thread_([this] { Run(); }) {}
    ~Watchdog() {
        stop_ = true;
        thread_.join();
    }
    /// Starts a new unit of work; the clock restarts.
    void Step(const std::string& what) {
        std::lock_guard<std::mutex> l(m_);
        what_ = what;
        since_ = std::chrono::steady_clock::now();
    }

private:
    void Run() {
        while (!stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> l(m_);
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since_);
            if (!what_.empty() && waited.count() > limit_ms_) {
                std::fprintf(stderr, "  HANG: '%s' did not finish within %d s\n", what_.c_str(), limit_ms_ / 1000);
                std::fflush(stderr);
                std::abort();
            }
        }
    }
    int limit_ms_;
    std::atomic<bool> stop_{false};
    std::mutex m_;
    std::string what_;
    std::chrono::steady_clock::time_point since_ = std::chrono::steady_clock::now();
    std::thread thread_;
};
