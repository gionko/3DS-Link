// soc_send_test: the count of the datagrams the network thread hands to the room socket (azl/stats.hpp, soc_send_*; 2026-11-11,
// Fantasy Life: a client's broadcast frames above ENet's fragment length never reached the room, and a send the socket refuses
// is taken by ENet as "0 bytes sent", in silence). The file under test is the plugin's own ENet platform layer,
// Sources/enet/enet_3ds.c, built for the PC with the shim of Tests/enet3ds (the one libctru call it makes), and the plugin's own
// counters (Sources/azl/log.cpp: azl_c_soc_send, StatsSocSend, TakeStats). No room and no ENet host: enet_socket_send is called
// by hand on a real UDP socket of the loopback, and the link wraps the socket call under it (-Wl,--wrap=sendto, --wrap=send) so
// that the test decides what the socket answers: it takes the datagram whole, refuses it (EWOULDBLOCK), fails with another
// error, fails with the kernel's raw result (libctru's answer when the request to the soc service itself fails), or takes fewer
// bytes. For each answer: what enet_socket_send returns, the errno it leaves (ENet reads it), what the
// layer tells the counters and what TakeStats() gives.
// The two hooks of the layer are wrapped too (--wrap=azl_c_soc_call, --wrap=azl_c_soc_send): the wrappers note the arguments,
// call the real ones and then leave ANOTHER errno behind, so that a layer that did not put the socket's errno back is seen here
// (on the console the hooks touch no errno: the layer does not rely on it).
//
// Build: Tests/run.sh (with the sanitizers) and Tests/enet3ds/build.sh (the family of the tests against the plugin's layer).
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <enet/enet.h>

#include "azl/platform.hpp"
#include "azl/stats.hpp"
#include "support.hpp"

using namespace azl;

// The numbers enet_3ds.c passes (a C file: it cannot read the enum) are the enum's.
static_assert(kSocSendTaken == 0 && kSocSendRefused == 1 && kSocSendFailed == 2 && kSocSendShort == 3, "enet_3ds.c passes 0, 1, 2, 3");
static_assert(kSocSendBig == 1024, "the tests below stand on both sides of 1024");

// ---- the socket under the layer, answered by the test --------------------------------------------------------------------------
enum class Answer { Real, Refuse, Fail, Raw, Short };
static Answer g_answer = Answer::Real;
static int g_fail_errno = 0;        ///< the errno of Answer::Fail
static int g_raw_result = 0;        ///< what Answer::Raw answers: a kernel result, negative and never -1 ...
static int g_raw_errno = ENODEV;    ///< ... and its errno (ENODEV is what libctru leaves)
static std::size_t g_short_take = 0; ///< how many bytes Answer::Short takes (really sent, so that the other end can read them)
static int g_socket_calls = 0;      ///< sendto / send calls that reached the wrapper
static std::size_t g_last_len = 0;  ///< the length of the last one (one datagram: the layer gathers ENet's buffers)

struct Note {
    unsigned bytes;
    int outcome;
};
static std::vector<Note> g_notes;      ///< what the layer told azl_c_soc_send
static std::vector<int> g_call_kinds;  ///< what the layer told azl_c_soc_call
constexpr int kHookErrno = ERANGE;     ///< the errno the wrapped hooks leave behind (never one the socket gives here)

/// One call of the socket: the answer the test set, or the real call (`real`, which sends the first `n` bytes).
static ssize_t Answered(std::size_t len, ssize_t (*real)(std::size_t n, void* ctx), void* ctx) {
    ++g_socket_calls;
    g_last_len = len;
    switch (g_answer) {
    case Answer::Refuse:
        errno = EWOULDBLOCK;
        return -1;
    case Answer::Fail:
        errno = g_fail_errno;
        return -1;
    case Answer::Raw:
        errno = g_raw_errno;
        return g_raw_result;
    case Answer::Short:
        return real(g_short_take < len ? g_short_take : len, ctx);
    case Answer::Real:
        break;
    }
    return real(len, ctx);
}

extern "C" {
ssize_t __real_sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
ssize_t __real_send(int, const void*, size_t, int);
void __real_azl_c_soc_send(unsigned, int);
void __real_azl_c_soc_call(int, unsigned long long);

ssize_t __wrap_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* to, socklen_t tolen) {
    struct Ctx {
        int fd;
        const void* buf;
        int flags;
        const struct sockaddr* to;
        socklen_t tolen;
    } c{fd, buf, flags, to, tolen};
    return Answered(len, [](std::size_t n, void* p) {
        const Ctx* x = static_cast<const Ctx*>(p);
        return __real_sendto(x->fd, x->buf, n, x->flags, x->to, x->tolen);
    }, &c);
}

ssize_t __wrap_send(int fd, const void* buf, size_t len, int flags) {
    struct Ctx {
        int fd;
        const void* buf;
        int flags;
    } c{fd, buf, flags};
    return Answered(len, [](std::size_t n, void* p) {
        const Ctx* x = static_cast<const Ctx*>(p);
        return __real_send(x->fd, x->buf, n, x->flags);
    }, &c);
}

void __wrap_azl_c_soc_send(unsigned bytes, int outcome) {
    g_notes.push_back({bytes, outcome});
    __real_azl_c_soc_send(bytes, outcome);
    errno = kHookErrno;
}

void __wrap_azl_c_soc_call(int kind, unsigned long long ticks) {
    g_call_kinds.push_back(kind);
    __real_azl_c_soc_call(kind, ticks);
    errno = kHookErrno;
}
} // extern "C"

// ---- the two ends ------------------------------------------------------------------------------------------------------------
static ENetSocket g_out = ENET_SOCKET_NULL; ///< the layer's socket (enet_socket_create: what the room client's host uses)
static int g_in = -1;                       ///< the other end: a plain socket of the loopback
static ENetAddress g_to;                    ///< its address, as ENet holds one
static unsigned char g_pattern[8192];

/// The datagram that reached the other end (empty: none within `wait_ms`).
static std::vector<unsigned char> Arrived(int wait_ms) {
    struct pollfd p = {g_in, POLLIN, 0};
    std::vector<unsigned char> v;
    if (poll(&p, 1, wait_ms) > 0) {
        v.resize(8192);
        const ssize_t n = recv(g_in, v.data(), v.size(), 0);
        v.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    }
    return v;
}

struct Sent {
    int ret;   ///< what enet_socket_send returned
    int err;   ///< the errno it left
    int calls; ///< how many times it reached the socket
    std::vector<Note> notes;
    std::vector<int> kinds;
};
constexpr int kErrnoBefore = ENOTTY; ///< the errno at the call (a send that succeeds sets none: ENet finds what was there)

/// One enet_socket_send of a datagram made of `parts` (ENet hands the header and the commands as separate buffers), to `to` (null: the
/// socket's own peer).
static Sent Send(const ENetAddress* to, std::initializer_list<std::size_t> parts) {
    ENetBuffer buffers[8];
    std::size_t count = 0, at = 0;
    for (std::size_t len : parts) {
        buffers[count].data = g_pattern + at;
        buffers[count].dataLength = len;
        at += len;
        ++count;
    }
    g_notes.clear();
    g_call_kinds.clear();
    const int before = g_socket_calls;
    errno = kErrnoBefore;
    const int ret = enet_socket_send(g_out, to, buffers, count);
    const int err = errno;
    return {ret, err, g_socket_calls - before, g_notes, g_call_kinds};
}

/// The layer told the counters once, `bytes` and `outcome`, after it told the timing (kind 2, a send).
#define CHECK_TOLD(sent, want_bytes, want_outcome)                    \
    do {                                                              \
        CHECK_EQ((sent).calls, 1);                                    \
        CHECK_EQ((sent).kinds.size(), 1u);                            \
        CHECK((sent).kinds.size() == 1 && (sent).kinds[0] == kSocSend); \
        CHECK_EQ((sent).notes.size(), 1u);                            \
        if ((sent).notes.size() == 1) {                               \
            CHECK_EQ((sent).notes[0].bytes, (want_bytes));            \
            CHECK_EQ((sent).notes[0].outcome, (want_outcome));        \
        }                                                             \
    } while (0)

/// The window TakeStats() gives now: the six counters of the sends and the timed send calls.
#define CHECK_WINDOW(want_refused, want_refused_big, want_failed, want_short, want_big, want_max, want_calls) \
    do {                                                                                                     \
        const Stats w = TakeStats();                                                                         \
        CHECK_EQ(w.soc_send_refused, static_cast<u32>(want_refused));                                        \
        CHECK_EQ(w.soc_send_refused_big, static_cast<u32>(want_refused_big));                                \
        CHECK_EQ(w.soc_send_failed, static_cast<u32>(want_failed));                                          \
        CHECK_EQ(w.soc_send_short, static_cast<u32>(want_short));                                            \
        CHECK_EQ(w.soc_send_big, static_cast<u32>(want_big));                                                \
        CHECK_EQ(w.soc_send_max_bytes, static_cast<u32>(want_max));                                          \
        CHECK_EQ(w.soc_calls[kSocSend], static_cast<u32>(want_calls));                                       \
    } while (0)

// ---- the socket takes the datagram whole -------------------------------------------------------------------------------------
static void TestTakenWhole() {
    std::printf("taken whole\n");
    (void)TakeStats();
    g_answer = Answer::Real;
    u32 big = 0, largest = 0, calls = 0;
    // (1400 is ENet's MTU here, a full datagram of fragments; 4096 the largest datagram the layer gathers)
    for (std::size_t size : {std::size_t(1), std::size_t(172), std::size_t(1023), std::size_t(1024), std::size_t(1025), std::size_t(1400),
                             std::size_t(4096)}) {
        const Sent s = Send(&g_to, {size});
        CHECK_EQ(s.ret, static_cast<int>(size)); // the bytes
        CHECK_EQ(s.err, kErrnoBefore);           // no error: the errno of before the call, not the hooks'
        CHECK_TOLD(s, static_cast<unsigned>(size), static_cast<int>(kSocSendTaken));
        const std::vector<unsigned char> got = Arrived(1000);
        CHECK_EQ(got.size(), size);
        CHECK(got.size() == size && std::memcmp(got.data(), g_pattern, size) == 0);
        big += size >= kSocSendBig;
        largest = size > largest ? static_cast<u32>(size) : largest;
        ++calls;
        // every window on its own: the largest is this datagram, "big" only from 1024 bytes
        CHECK_WINDOW(0, 0, 0, 0, size >= 1024 ? 1 : 0, size, 1);
    }
    CHECK_EQ(big, 4u); // (1024, 1025, 1400, 4096: the loop did stand on both sides)
    // the datagram is the SUM of ENet's buffers: 4 + 1020 bytes are one datagram of 1024 (big), 4 + 1019 one of 1023 (not)
    Sent s = Send(&g_to, {4, 1020});
    CHECK_EQ(s.ret, 1024);
    CHECK_TOLD(s, 1024u, static_cast<int>(kSocSendTaken));
    std::vector<unsigned char> got = Arrived(1000);
    CHECK(got.size() == 1024 && std::memcmp(got.data(), g_pattern, 1024) == 0); // gathered in order
    s = Send(&g_to, {4, 1019});
    CHECK_EQ(s.ret, 1023);
    CHECK_TOLD(s, 1023u, static_cast<int>(kSocSendTaken));
    CHECK_EQ(Arrived(1000).size(), 1023u);
    s = Send(&g_to, {4, 8, 160});
    CHECK_EQ(s.ret, 172);
    CHECK_EQ(Arrived(1000).size(), 172u);
    // one window for the three: one big, and the largest is not the last
    CHECK_WINDOW(0, 0, 0, 0, 1, 1024, 3);
    // TakeStats cleared them
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
    // a datagram of no bytes (ENet builds none): taken, counted nowhere
    s = Send(&g_to, {});
    CHECK_EQ(s.ret, 0);
    CHECK_EQ(s.err, kErrnoBefore);
    CHECK_TOLD(s, 0u, static_cast<int>(kSocSendTaken));
    CHECK_EQ(Arrived(1000).size(), 0u);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 1);
}

// ---- the socket refuses it (EWOULDBLOCK): "0 bytes sent" for ENet, and a count -------------------------------------------------
static void TestRefused() {
    std::printf("refused\n");
    (void)TakeStats();
    g_answer = Answer::Refuse;
    Sent s = Send(&g_to, {1023});
    CHECK_EQ(s.ret, 0);            // ENet goes on as if nothing were sent
    CHECK_EQ(s.err, EWOULDBLOCK);  // the socket's errno, not the hooks'
    CHECK_TOLD(s, 1023u, static_cast<int>(kSocSendRefused));
    CHECK_WINDOW(1, 0, 0, 0, 0, 0, 1); // 1023 bytes: refused, not a big one
    s = Send(&g_to, {1024});
    CHECK_EQ(s.ret, 0);
    CHECK_EQ(s.err, EWOULDBLOCK);
    CHECK_TOLD(s, 1024u, static_cast<int>(kSocSendRefused));
    CHECK_WINDOW(1, 1, 0, 0, 0, 0, 1); // 1024 bytes: a big one
    // a run, in one window: 5 refused, 3 of them big (the sum of the buffers again); nothing "taken", no largest
    for (std::size_t size : {std::size_t(1), std::size_t(1023), std::size_t(1400), std::size_t(4096)}) {
        s = Send(&g_to, {size});
        CHECK_EQ(s.ret, 0);
        CHECK_EQ(s.err, EWOULDBLOCK);
        CHECK_TOLD(s, static_cast<unsigned>(size), static_cast<int>(kSocSendRefused));
    }
    s = Send(&g_to, {24, 1000});
    CHECK_EQ(s.ret, 0);
    CHECK_TOLD(s, 1024u, static_cast<int>(kSocSendRefused));
    CHECK_WINDOW(5, 3, 0, 0, 0, 0, 5);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(Arrived(50).size(), 0u); // nothing left the console
    g_answer = Answer::Real;
}

// ---- the socket fails with another error: -1 for ENet (its service call fails), and a count ------------------------------------
static void TestFailed() {
    std::printf("failed\n");
    (void)TakeStats();
    g_answer = Answer::Fail;
    u32 failed = 0;
    for (int err : {ENETUNREACH, ENOBUFS, EMSGSIZE, ENOMEM, EINTR, EBADF}) {
        g_fail_errno = err;
        const Sent s = Send(&g_to, {failed % 2 ? std::size_t(1400) : std::size_t(172)});
        CHECK_EQ(s.ret, -1);
        CHECK_EQ(s.err, err); // the socket's errno
        CHECK_TOLD(s, failed % 2 ? 1400u : 172u, static_cast<int>(kSocSendFailed));
        ++failed;
    }
    // (three of them were 1400 bytes: a failed send is in no "big" count and is no largest datagram)
    CHECK_WINDOW(0, 0, failed, 0, 0, 0, failed);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
    // EAGAIN is EWOULDBLOCK on Linux and in the console's libc: a refusal, not a failure
    CHECK_EQ(EAGAIN, EWOULDBLOCK);
    CHECK_EQ(Arrived(50).size(), 0u);
    g_answer = Answer::Real;
}

// ---- the request to the soc service itself fails: libctru answers the kernel's raw result, not -1 (2026-11-11) ------------------
// libctru's sendto and send, when svcSendSyncRequest fails (the plugin's soc:U session is gone: 0xD8E007F7, an invalid handle; 0xC920181A,
// the session closed by the service), set errno to ENODEV and return the kernel's result as it is: negative, never -1. ENet takes any
// negative value for an error, and the count must too: taken for "a datagram the socket took whole", a send that never left the console
// would be read in the log as one that left it and was lost after it.
static void TestRawKernelResult() {
    std::printf("raw kernel result\n");
    (void)TakeStats();
    g_answer = Answer::Raw;
    u32 failed = 0;
    for (const int raw : {static_cast<int>(0xD8E007F7u), static_cast<int>(0xC920181Au), -2}) {
        g_raw_result = raw;
        CHECK(raw < -1);
        const Sent s = Send(&g_to, {failed % 2 ? std::size_t(172) : std::size_t(1400)});
        CHECK_EQ(s.ret, raw);    // what ENet was given before the count was there (negative: an error for it)
        CHECK_EQ(s.err, ENODEV); // libctru's errno, not the hooks'
        CHECK_TOLD(s, failed % 2 ? 172u : 1400u, static_cast<int>(kSocSendFailed));
        ++failed;
    }
    // (two of them were 1400 bytes: failed, in no "big" count and no largest datagram - and not short either)
    CHECK_WINDOW(0, 0, failed, 0, 0, 0, failed);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
    // a raw result with EWOULDBLOCK in the errno is a failure all the same: a refusal is -1 with that errno, nothing else
    g_raw_result = static_cast<int>(0xD8E007F7u);
    g_raw_errno = EWOULDBLOCK;
    const Sent s = Send(&g_to, {1024});
    CHECK_EQ(s.ret, g_raw_result);
    CHECK_EQ(s.err, EWOULDBLOCK);
    CHECK_TOLD(s, 1024u, static_cast<int>(kSocSendFailed));
    CHECK_WINDOW(0, 0, 1, 0, 0, 0, 1);
    CHECK_EQ(Arrived(50).size(), 0u);
    g_raw_errno = ENODEV;
    g_answer = Answer::Real;
}

// ---- the socket takes fewer bytes than the datagram held -----------------------------------------------------------------------
static void TestShort() {
    std::printf("short\n");
    (void)TakeStats();
    g_answer = Answer::Short;
    g_short_take = 1000;
    Sent s = Send(&g_to, {1400});
    CHECK_EQ(s.ret, 1000);         // the bytes the socket took
    CHECK_EQ(s.err, kErrnoBefore); // no error
    CHECK_TOLD(s, 1400u, static_cast<int>(kSocSendShort));
    CHECK_EQ(g_last_len, std::size_t(1400)); // (the layer did ask for the whole datagram)
    CHECK_EQ(Arrived(1000).size(), 1000u);
    CHECK_WINDOW(0, 0, 0, 1, 0, 0, 1); // short: not "big taken whole", not the largest
    g_short_take = 1399;               // one byte less is short
    s = Send(&g_to, {4, 1396});
    CHECK_EQ(s.ret, 1399);
    CHECK_TOLD(s, 1400u, static_cast<int>(kSocSendShort));
    CHECK_EQ(Arrived(1000).size(), 1399u);
    g_short_take = 0; // no byte at all and no error: short too (the errno says nothing, so it is not a refusal)
    s = Send(&g_to, {172});
    CHECK_EQ(s.ret, 0);
    CHECK_EQ(s.err, kErrnoBefore);
    CHECK_TOLD(s, 172u, static_cast<int>(kSocSendShort));
    CHECK_EQ(Arrived(1000).size(), 0u); // (what arrives is a datagram of no bytes)
    g_short_take = 1400; // all of it: not short
    s = Send(&g_to, {1400});
    CHECK_EQ(s.ret, 1400);
    CHECK_TOLD(s, 1400u, static_cast<int>(kSocSendTaken));
    CHECK_EQ(Arrived(1000).size(), 1400u);
    CHECK_WINDOW(0, 0, 0, 2, 1, 1400, 3);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
    g_answer = Answer::Real;
}

// ---- one window of a game: every answer, each in its own counter ---------------------------------------------------------------
static void TestOneWindow() {
    std::printf("one window\n");
    (void)TakeStats();
    const auto one = [](Answer a, std::size_t size, int err = 0, std::size_t take = 0) {
        g_answer = a;
        g_fail_errno = err;
        g_short_take = take;
        const Sent s = Send(&g_to, {size});
        g_answer = Answer::Real;
        return s.ret;
    };
    CHECK_EQ(one(Answer::Real, 172), 172);
    CHECK_EQ(one(Answer::Real, 1392), 1392);
    CHECK_EQ(one(Answer::Refuse, 1400), 0);          // larger than every datagram taken: not the largest
    CHECK_EQ(one(Answer::Refuse, 1400), 0);
    CHECK_EQ(one(Answer::Refuse, 60), 0);
    CHECK_EQ(one(Answer::Real, 1024), 1024);
    CHECK_EQ(one(Answer::Fail, 4000, ENOBUFS), -1);
    CHECK_EQ(one(Answer::Fail, 30, ENETUNREACH), -1);
    CHECK_EQ(one(Answer::Short, 3000, 0, 2999), 2999);
    CHECK_EQ(one(Answer::Real, 1023), 1023);
    g_raw_result = static_cast<int>(0xC920181Au);
    CHECK_EQ(one(Answer::Raw, 1400), g_raw_result); // (larger than every datagram taken, and 1024 bytes or more: failed, nothing else)
    while (!Arrived(50).empty()) {
    }
    CHECK_WINDOW(3, 2, 3, 1, 2, 1392, 11);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
}

// ---- the statistics are the network thread's own: a send of any other thread is counted nowhere --------------------------------
static void TestNotTheNetworkThread() {
    std::printf("not the network thread\n");
    (void)TakeStats();
    CHECK(platform::IsNetThread());
    // another thread (the PC tests run a room server in the process, with this layer; on the console no other thread sends): the layer
    // works the same - the return value, the errno, the hooks told - and no counter moves
    std::thread other([] {
        CHECK(!platform::IsNetThread());
        g_answer = Answer::Real;
        Sent s = Send(&g_to, {1400});
        CHECK_EQ(s.ret, 1400);
        CHECK_EQ(s.err, kErrnoBefore);
        CHECK_TOLD(s, 1400u, static_cast<int>(kSocSendTaken));
        g_answer = Answer::Refuse;
        s = Send(&g_to, {1400});
        CHECK_EQ(s.ret, 0);
        CHECK_EQ(s.err, EWOULDBLOCK);
        CHECK_TOLD(s, 1400u, static_cast<int>(kSocSendRefused));
        g_answer = Answer::Fail;
        g_fail_errno = ENOBUFS;
        s = Send(&g_to, {1400});
        CHECK_EQ(s.ret, -1);
        CHECK_EQ(s.err, ENOBUFS);
        CHECK_TOLD(s, 1400u, static_cast<int>(kSocSendFailed));
        g_answer = Answer::Short;
        g_short_take = 10;
        s = Send(&g_to, {1400});
        CHECK_EQ(s.ret, 10);
        CHECK_TOLD(s, 1400u, static_cast<int>(kSocSendShort));
        g_answer = Answer::Real;
    });
    other.join();
    CHECK_EQ(Arrived(1000).size(), 1400u);
    CHECK_EQ(Arrived(1000).size(), 10u);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
    // ... nor of this thread once it is no longer the network thread
    platform::ForgetNetThread();
    CHECK(!platform::IsNetThread());
    Sent s = Send(&g_to, {1400});
    CHECK_EQ(s.ret, 1400);
    CHECK_EQ(Arrived(1000).size(), 1400u);
    g_answer = Answer::Refuse;
    s = Send(&g_to, {1400});
    CHECK_EQ(s.ret, 0);
    g_answer = Answer::Real;
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
    // ... and of a thread that is: the one that marked itself last
    std::thread net([] {
        const platform::NetThreadMark mark;
        g_answer = Answer::Refuse;
        const Sent refused = Send(&g_to, {1400});
        CHECK_EQ(refused.ret, 0);
        g_answer = Answer::Real;
        const Sent taken = Send(&g_to, {1100});
        CHECK_EQ(taken.ret, 1100);
    });
    net.join();
    CHECK_EQ(Arrived(1000).size(), 1100u);
    CHECK_WINDOW(1, 1, 0, 0, 1, 1100, 2);
    platform::MarkNetThread();
    CHECK(platform::IsNetThread());
}

// ---- a datagram the layer cannot gather (above ENet's largest, 4096 bytes: ENet builds none) never reaches the socket ----------
static void TestOverTheGatherBuffer() {
    std::printf("over the gather buffer\n");
    (void)TakeStats();
    g_answer = Answer::Real;
    const Sent s = Send(&g_to, {4096, 1});
    CHECK_EQ(s.ret, -1);
    CHECK_EQ(s.err, kErrnoBefore); // (no socket call, no errno of one)
    CHECK_EQ(s.calls, 0);
    CHECK_EQ(s.notes.size(), 0u);  // not a send: nothing is counted, nothing is timed
    CHECK_EQ(s.kinds.size(), 0u);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(Arrived(50).size(), 0u);
}

// ---- the other branch of the layer: a connected socket, no address (send instead of sendto) - the same count --------------------
static void TestConnectedSocket() {
    std::printf("connected socket\n");
    CHECK_EQ(enet_socket_connect(g_out, &g_to), 0);
    (void)TakeStats();
    g_answer = Answer::Real;
    Sent s = Send(nullptr, {4, 1020});
    CHECK_EQ(s.ret, 1024);
    CHECK_EQ(s.err, kErrnoBefore);
    CHECK_TOLD(s, 1024u, static_cast<int>(kSocSendTaken));
    CHECK_EQ(Arrived(1000).size(), 1024u);
    g_answer = Answer::Refuse;
    s = Send(nullptr, {1024});
    CHECK_EQ(s.ret, 0);
    CHECK_EQ(s.err, EWOULDBLOCK);
    CHECK_TOLD(s, 1024u, static_cast<int>(kSocSendRefused));
    g_answer = Answer::Fail;
    g_fail_errno = ECONNREFUSED;
    s = Send(nullptr, {100});
    CHECK_EQ(s.ret, -1);
    CHECK_EQ(s.err, ECONNREFUSED);
    CHECK_TOLD(s, 100u, static_cast<int>(kSocSendFailed));
    g_answer = Answer::Short;
    g_short_take = 99;
    s = Send(nullptr, {100});
    CHECK_EQ(s.ret, 99);
    CHECK_TOLD(s, 100u, static_cast<int>(kSocSendShort));
    CHECK_EQ(Arrived(1000).size(), 99u);
    g_answer = Answer::Raw;
    g_raw_result = static_cast<int>(0xD8E007F7u);
    s = Send(nullptr, {1400});
    CHECK_EQ(s.ret, g_raw_result);
    CHECK_EQ(s.err, ENODEV);
    CHECK_TOLD(s, 1400u, static_cast<int>(kSocSendFailed));
    g_answer = Answer::Real;
    CHECK_WINDOW(1, 1, 2, 1, 1, 1024, 5);
    CHECK_WINDOW(0, 0, 0, 0, 0, 0, 0);
}

int main() {
    for (std::size_t i = 0; i < sizeof(g_pattern); ++i) {
        g_pattern[i] = static_cast<unsigned char>(i * 7 + (i >> 8));
    }
    // the other end: a socket of the loopback on a port of its own
    g_in = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in {};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t in_len = sizeof(in);
    if (g_in < 0 || bind(g_in, reinterpret_cast<struct sockaddr*>(&in), sizeof(in)) != 0 ||
        getsockname(g_in, reinterpret_cast<struct sockaddr*>(&in), &in_len) != 0) {
        std::printf("no loopback socket\n");
        return 1;
    }
    CHECK_EQ(enet_address_set_host_ip(&g_to, "127.0.0.1"), 0);
    g_to.port = ntohs(in.sin_port);
    // this end: the layer's own socket, as ENet's host makes it (non-blocking)
    g_out = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    CHECK(g_out != ENET_SOCKET_NULL);
    CHECK_EQ(enet_socket_set_option(g_out, ENET_SOCKOPT_NONBLOCK, 1), 0);
    platform::MarkNetThread(); // this thread is the network thread (RoomClient::Run marks itself the same way)

    TestTakenWhole();
    TestRefused();
    TestFailed();
    TestRawKernelResult();
    TestShort();
    TestOneWindow();
    TestNotTheNetworkThread();
    TestOverTheGatherBuffer();
    TestConnectedSocket();

    CHECK(g_socket_calls > 40); // (the answers above were the wrapper's: a link without --wrap=sendto would not have asked it)
    enet_socket_destroy(g_out);
    close(g_in);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
