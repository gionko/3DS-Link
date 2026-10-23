// Shared helpers for the host-side tests.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "azl/stats.hpp"
#include "azl/uds_engine.hpp"
#include "azl/uds_frames.hpp"

using namespace azl;

/// The log on for a scope. Log() returns at once at level 0, which is where every test starts: a test that reads
/// the lines the plugin would write turns it on first (and gives the level back).
struct LogOn {
    int before;
    LogOn() : before(g_debug_level) {
        g_debug_level = 1;
    }
    ~LogOn() {
        g_debug_level = before;
    }
};

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

[[maybe_unused]] static std::string Show(const std::string& s) {
    return s;
}
template <typename T>
static std::string Show(T v) {
    return std::to_string(static_cast<unsigned long long>(v));
}

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const auto va = (a);                                                                       \
        const auto vb = (b);                                                                       \
        if (!(va == vb)) {                                                                         \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (%s vs %s)\n", __FILE__, __LINE__, #a,    \
                         #b, Show(va).c_str(), Show(vb).c_str());                                  \
        }                                                                                          \
    } while (0)

[[maybe_unused]] static std::string Hex(const u8* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

[[maybe_unused]] static std::vector<u8> FromHex(const char* s) {
    std::vector<u8> v;
    for (; s[0] && s[1]; s += 2) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        v.push_back(static_cast<u8>((nib(s[0]) << 4) | nib(s[1])));
    }
    return v;
}

class FakeKernel : public KernelOps {
public:
    u32 CreateEvent() override {
        std::lock_guard<std::mutex> l(m_);
        const u32 id = next_++;
        events_[id] = false;
        return id;
    }
    u32 DuplicateForGame(u32 h) override {
        return h;
    }
    void SignalEvent(u32 h) override {
        std::lock_guard<std::mutex> l(m_);
        ++signals_[h];
        if (events_.count(h)) {
            events_[h] = true;
        }
        cv_.notify_all();
    }
    /// How often the event was signalled (also when it was closed or never opened).
    u32 Signals(u32 h) {
        std::lock_guard<std::mutex> l(m_);
        return signals_.count(h) ? signals_[h] : 0;
    }
    void ClearEvent(u32 h) override {
        std::lock_guard<std::mutex> l(m_);
        if (events_.count(h)) {
            events_[h] = false;
        }
    }
    bool WaitEvent(u32 h, s64 timeout_ns) override {
        std::unique_lock<std::mutex> l(m_);
        const bool ok = cv_.wait_for(l, std::chrono::nanoseconds(timeout_ns),
                                     [&] { return !events_.count(h) || events_[h]; });
        if (ok && events_.count(h)) {
            events_[h] = false; // one-shot
            return true;
        }
        return false;
    }
    bool IsSignaled(u32 h) {
        std::lock_guard<std::mutex> l(m_);
        return events_.count(h) && events_[h];
    }
    void CloseEvent(u32 h) override {
        std::lock_guard<std::mutex> l(m_);
        events_.erase(h);
        cv_.notify_all();
    }
    void SleepNs(s64) override {} // keep tests fast

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::map<u32, bool> events_;
    std::map<u32, u32> signals_;
    u32 next_ = 1;
};

/// PullPacket the way the IPC front-end calls it: the game's buffer is the 0x5C8 bytes the SDK prepares (the
/// reply descriptor of an unrestricted pull). `*out` is that buffer, zeroed first; the frame is in its first
/// `*size` bytes.
inline u32 PullInto(UdsEngine& engine, u32 bind_id, std::vector<u8>* out, u32* size, u16* src) {
    out->assign(0x5C8, 0);
    return engine.PullPacket(bind_id, 0x5C6, out->data(), static_cast<u32>(out->size()), size, src);
}

/// The UDS block of the game the engine tests pretend to be (FFE declares 0x14000).
constexpr u32 kTestSharedMemory = 0x14000;
constexpr u16 kTestVersion = 0x400; // InitializeWithVersion as libctru and FFE send it

[[maybe_unused]] static NetworkInfo MakeNetworkInfo(u8 max_nodes, u8 app_size) {
    NetworkInfo ni;
    std::memset(&ni, 0, sizeof(ni));
    ni.channel = 0;
    ni.max_nodes = max_nodes;
    StoreBE32(ni.wlan_comm_id_be, 0x0010CAFE);
    ni.id = 1;
    StoreBE16(ni.attributes_be, 0x0002);
    StoreBE32(ni.network_id_be, 0xDEADBEEF);
    ni.application_data_size = app_size;
    for (int i = 0; i < app_size; ++i) {
        ni.application_data[i] = static_cast<u8>(0xA0 + i);
    }
    return ni;
}

[[maybe_unused]] static NodeInfo MakeNode(u64 seed, const char* name, u16 id) {
    NodeInfo n;
    n.friend_code_seed = seed;
    for (int i = 0; name[i] && i < 10; ++i) {
        n.username[i] = static_cast<u16>(name[i]);
    }
    n.network_node_id = id;
    return n;
}

