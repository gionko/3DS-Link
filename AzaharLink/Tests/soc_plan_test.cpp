#include "azl/soc_plan.hpp"

// Host test of the socket buffer placement arithmetic and of the receive-ring policy (rcvbuf.h), run by
// Tests/run.sh.
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace azl::ctr::socplan;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                      \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

static void Dump(const char* name, const Inputs& in, uint32_t size) {
    Candidate c[kMaxCandidates];
    PlanInfo pi;
    const int n = Build(in, size, c, kMaxCandidates, &pi);
    std::printf("%s: size 0x%x known=%d provable=%d room=%u KB heap_reach=0x%08x proof_floor=0x%08x, %d candidates\n",
                name, size, pi.known, pi.provable, pi.room / 1024, pi.heap_reach, pi.proof_floor, n);
    for (int i = 0; i < n; ++i) {
        if (i < 3 || i == n - 1 || c[i].window != c[i - 1].window) {
            std::printf("   [%2d] 0x%08x..0x%08x %s\n", i, c[i].base, c[i].base + size, WindowName(c[i].window));
        }
    }
}

// Invariants every plan must keep, whatever the inputs.
static void CheckPlan(const Candidate* c, int n, const PlanInfo& pi, uint32_t size, uint32_t free_end) {
    for (int i = 0; i < n; ++i) {
        CHECK(c[i].base % kPage == 0);
        CHECK(c[i].base >= kRegionBase && static_cast<uint64_t>(c[i].base) + size <= kRegionEnd);
        for (int j = 0; j < i; ++j) {
            CHECK(c[i].base != c[j].base);
        }
        if (c[i].window == Window::HeapAreaTop || c[i].window == Window::LimitTop) {
            CHECK(pi.provable);
            CHECK(c[i].base >= pi.proof_floor); // never inside the heap's reach
            CHECK(static_cast<uint64_t>(c[i].base) + size <= kHeapAreaEnd);
        }
        if (c[i].window == Window::ReserveTop) {
            const uint32_t top = free_end != 0 ? free_end : kDefaultTopEnd;
            CHECK(c[i].base >= kHeapAreaEnd && static_cast<uint64_t>(c[i].base) + size <= top);
        }
    }
}

// A model of what the soc service does with SO_RCVBUF on a UDP socket (hardware
// tests #5 and #6): the ring starts at 4416 bytes, a request of 8 KB..64 KB is applied, 64 KB + 1 up to
// 128 KB answers ENOMEM and changes nothing, and an option it does not implement answers errno 109.
struct SocModel {
    int ring = 4416;
    int max_ok = 65536;     // larger requests answer ENOMEM
    int fixed_errno = 0;    // every call fails with this errno (109 = ENOPROTOOPT) and changes nothing
    bool readable = true;   // getsockopt works
    bool apply_on_error = false; // the call fails with ENOMEM but the ring is already the size asked
    bool ignores = false;   // rc 0 but the ring does not change
    int calls = 0;
    int asked[8] = {};
    int log_calls = 0;
};

static int ModelSet(void* ctx, int size, int* err) {
    SocModel* m = static_cast<SocModel*>(ctx);
    if (m->calls < 8) {
        m->asked[m->calls] = size;
    }
    ++m->calls;
    *err = 0;
    if (m->fixed_errno) {
        *err = m->fixed_errno;
        return -1;
    }
    if (size > m->max_ok) {
        if (m->apply_on_error) {
            m->ring = size;
        }
        *err = AZL_RCVBUF_ENOMEM;
        return -1;
    }
    if (!m->ignores) {
        m->ring = size;
    }
    return 0;
}
static int ModelGet(void* ctx) {
    const SocModel* m = static_cast<const SocModel*>(ctx);
    return m->readable ? m->ring : -1;
}
static void ModelLog(void* ctx, int, int, int, int) {
    ++static_cast<SocModel*>(ctx)->log_calls;
}
static int Negotiate(SocModel* m, int want, unsigned block) {
    azl_rcvbuf_io io{ModelSet, ModelGet, ModelLog, m};
    const int asked = azl_rcvbuf_limit(want, block);
    return asked > 0 ? azl_rcvbuf_negotiate(&io, asked) : m->ring;
}

static void TestReceiveRing() {
    // The hard limit: never more than 64 KB, whatever is asked (above 128 KB the soc process crashes).
    CHECK(AZL_RCVBUF_MAX == 65536);
    for (int want : {65537, 100000, 131072, 131073, 262144, 1 << 20, 0x7FFFFFFF}) {
        CHECK(azl_rcvbuf_limit(want, 0) == 65536);
        CHECK(azl_rcvbuf_limit(want, 0x20000) == 65536);
        CHECK(azl_rcvbuf_limit(want, 0x60000) == 65536);
    }
    CHECK(azl_rcvbuf_limit(65536, 0) == 65536);
    CHECK(azl_rcvbuf_limit(-5, 0) == 0);

    // The ring budget of a block: 64 KB in 0x20000 (and in anything bigger), 32 KB in 0x10000, no ring below.
    CHECK(azl_rcvbuf_ring_budget(0x20000) == 0x10000);
    CHECK(azl_rcvbuf_ring_budget(0x60000) == 0x10000);
    CHECK(azl_rcvbuf_ring_budget(0x1F000) == 0x8000);
    CHECK(azl_rcvbuf_ring_budget(0x10000) == 0x8000);
    CHECK(azl_rcvbuf_ring_budget(0xF000) == 0);
    CHECK(azl_rcvbuf_ring_budget(0) == 0);
    CHECK(azl_rcvbuf_limit(65536, 0x20000) == 65536); // the old block/4 rule would have said 32 KB
    CHECK(azl_rcvbuf_limit(65536, 0x10000) == 32768);
    CHECK(azl_rcvbuf_limit(65536, 0xF000) == 0);
    CHECK(azl_rcvbuf_limit(16384, 0x20000) == 16384); // a request below the budget stays
    // The block ladder and the budget go together.
    CHECK(kBlockSizes[0] == kSocBlockSize && kBlockSizes[1] == 0x10000);
    CHECK(sizeof(kBlockSizes) / sizeof(kBlockSizes[0]) == 2);
    for (uint32_t block : kBlockSizes) {
        CHECK(block % kPage == 0 && block >= 0x10000); // soc:U refuses less than 0x10000 (0xD8E073EC)
        CHECK(azl_rcvbuf_limit(AZL_RCVBUF_MAX, block) <= AZL_RCVBUF_MAX);
        CHECK(azl_rcvbuf_limit(AZL_RCVBUF_MAX, block) * 2 <= static_cast<int>(block)); // a ring needs room beside it
    }

    // The ini key: 8, 16, 32, 64 only; it lowers, it never raises.
    CHECK(azl_rcvbuf_from_kb(8) == 8192 && azl_rcvbuf_from_kb(16) == 16384);
    CHECK(azl_rcvbuf_from_kb(32) == 32768 && azl_rcvbuf_from_kb(64) == 65536);
    for (int kb : {-1, 0, 1, 4, 7, 9, 24, 48, 63, 65, 96, 128, 256, 1024, 65536, 0x7FFFFFFF}) {
        CHECK(azl_rcvbuf_from_kb(kb) == 0); // ignored: the ring stays 64 KB
    }
    for (int kb : {8, 16, 32, 64}) {
        CHECK(azl_rcvbuf_limit(azl_rcvbuf_from_kb(kb), 0x20000) <= 65536);
        CHECK(azl_rcvbuf_limit(azl_rcvbuf_from_kb(kb), 0x20000) == kb * 1024);
    }

    // The ladder: 64 -> 32 -> 16 KB, then nothing.
    CHECK(azl_rcvbuf_next(65536) == 32768 && azl_rcvbuf_next(32768) == 16384);
    CHECK(azl_rcvbuf_next(16384) == 0 && azl_rcvbuf_next(8192) == 0 && azl_rcvbuf_next(0) == 0);

    // Retry only when the service ran out of memory AND the ring is really smaller than the candidate.
    CHECK(azl_rcvbuf_should_retry(-1, AZL_RCVBUF_ENOMEM, 4416, 65536));
    CHECK(azl_rcvbuf_should_retry(-1, AZL_RCVBUF_ENOMEM, -1, 65536));       // unreadable counts as smaller
    CHECK(!azl_rcvbuf_should_retry(-1, AZL_RCVBUF_ENOMEM, 65536, 65536));   // refused but the ring is as asked
    CHECK(!azl_rcvbuf_should_retry(-1, AZL_RCVBUF_ENOMEM, 131072, 65536));
    CHECK(!azl_rcvbuf_should_retry(-1, 109, 4416, 65536));                  // another error: not a size problem
    CHECK(!azl_rcvbuf_should_retry(0, 0, 4416, 65536));                     // success is not retried even if smaller
    CHECK(azl_rcvbuf_degraded(65536, 32768) && azl_rcvbuf_degraded(65536, 4416) && azl_rcvbuf_degraded(32768, 16384));
    CHECK(!azl_rcvbuf_degraded(65536, 65536) && !azl_rcvbuf_degraded(32768, 65536) && !azl_rcvbuf_degraded(65536, -1));

    // Negotiation against the model.
    SocModel healthy; // takes 64 KB at once
    CHECK(Negotiate(&healthy, 65536, 0x20000) == 65536);
    CHECK(healthy.calls == 1 && healthy.asked[0] == 65536 && healthy.log_calls == 1);

    SocModel never_above_32; // 64 KB answers ENOMEM, 32 KB is taken
    never_above_32.max_ok = 32768;
    CHECK(Negotiate(&never_above_32, 65536, 0x20000) == 32768);
    CHECK(never_above_32.calls == 2 && never_above_32.asked[0] == 65536 && never_above_32.asked[1] == 32768);

    SocModel never_above_16; // only 16 KB
    never_above_16.max_ok = 16384;
    CHECK(Negotiate(&never_above_16, 65536, 0x20000) == 16384);
    CHECK(never_above_16.calls == 3 && never_above_16.asked[2] == 16384);

    SocModel nothing; // every size answers ENOMEM: the ladder ends at 16 KB and the default ring stays
    nothing.max_ok = 4096;
    CHECK(Negotiate(&nothing, 65536, 0x20000) == 4416);
    CHECK(nothing.calls == 3 && nothing.asked[0] == 65536 && nothing.asked[1] == 32768 && nothing.asked[2] == 16384);
    for (int i = 0; i < nothing.calls; ++i) {
        CHECK(nothing.asked[i] <= 65536); // nothing above 64 KB is ever sent
    }

    SocModel odd_error; // ENOPROTOOPT: one attempt only
    odd_error.fixed_errno = 109;
    CHECK(Negotiate(&odd_error, 65536, 0x20000) == 4416);
    CHECK(odd_error.calls == 1);

    SocModel already_there; // refused with ENOMEM but the ring is what was asked: no second attempt
    already_there.max_ok = 4096;
    already_there.apply_on_error = true;
    CHECK(Negotiate(&already_there, 65536, 0x20000) == 65536);
    CHECK(already_there.calls == 1);

    SocModel unreadable; // the readback fails: a refusal is retried, the result is "unknown"
    unreadable.readable = false;
    unreadable.max_ok = 32768;
    CHECK(Negotiate(&unreadable, 65536, 0x20000) == -1);
    CHECK(unreadable.calls == 2 && unreadable.asked[1] == 32768);

    SocModel ignoring; // rc 0 but the ring does not change: accepted as it is, the plugin reports the shortfall
    ignoring.ignores = true;
    const int got = Negotiate(&ignoring, 65536, 0x20000);
    CHECK(ignoring.calls == 1 && got == 4416 && azl_rcvbuf_degraded(65536, got));

    // A smaller block and the ini key cap the first request, never raise it.
    SocModel small_block;
    CHECK(Negotiate(&small_block, 65536, 0x10000) == 32768 && small_block.asked[0] == 32768);
    SocModel lowered;
    CHECK(Negotiate(&lowered, azl_rcvbuf_from_kb(16), 0x20000) == 16384 && lowered.calls == 1);
    SocModel absurd; // a hostile ini value reaches the service as 64 KB at the most
    CHECK(Negotiate(&absurd, 1 << 30, 0x20000) == 65536 && absurd.asked[0] == 65536);

    // The names of the Initialize refusals.
    CHECK(InitFailureName(0xC8A0700B) && std::strstr(InitFailureName(0xC8A0700B), "already registered"));
    CHECK(InitFailureName(0xC8A07004) && std::strstr(InitFailureName(0xC8A07004), "already initialised"));
    CHECK(InitFailureName(0) == nullptr && InitFailureName(0xD8E073EC) == nullptr);
}

int main() {
    TestReceiveRing();
    Candidate c[kMaxCandidates];
    PlanInfo pi;
    constexpr uint32_t kNew = 124u << 20; // application region of a New 3DS
    constexpr uint32_t kOld = 64u << 20;  // application region of an Old 3DS / 2DS
    // FFE on the console: limit 64 MiB, commit 8692 KB at start, stack 0x40000 -> free span ends 0x0FFC0000.
    Inputs ffe{65536u * 1024, 8692u * 1024, 0x0FFC0000, kNew};
    int n = Build(ffe, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(n > 0 && pi.known && pi.provable);
    CHECK(kSocBlockSize == 0x20000);
    CHECK(c[0].base == 0x0DFE0000 && c[0].window == Window::HeapAreaTop); // (0.1.12-0.1.14 chose this place with a 0x60000 block: 0x0DFA0000)
    CHECK(pi.heap_reach == 0x0B763000);                                    // 56844 - 128 = 56716 KB above 0x08000000
    CHECK(pi.proof_floor == 0x0B863000);
    CHECK(c[15].window == Window::HeapAreaTop && c[15].base == 0x0DFE0000 - 15 * kStep);
    CHECK(c[16].window == Window::LimitTop && c[16].base == 0x0BFE0000);   // top of 64 MiB of room
    CHECK(c[23].window == Window::LimitTop && c[23].base == 0x0B8E0000);   // the last one above the proof floor
    CHECK(c[24].window == Window::ReserveTop && c[24].base == 0x0FFA0000); // top of the reserve, under the stack
    CheckPlan(c, n, pi, kSocBlockSize, 0x0FFC0000);
    Dump("FFE on a New 3DS (legacy 64 MiB)", ffe, kSocBlockSize);

    // The same title on an Old 3DS: the top of the commit room goes first (nothing has run there yet).
    Inputs o3ds{65536u * 1024, 8692u * 1024, 0x0FFC0000, kOld};
    n = Build(o3ds, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(pi.provable && n > 0);
    CHECK(c[0].window == Window::LimitTop && c[0].base == 0x0BFE0000);
    CHECK(c[0].base + kSocBlockSize <= 0x0C000000);
    CHECK(c[8].window == Window::HeapAreaTop && c[8].base == 0x0DFE0000);
    CheckPlan(c, n, pi, kSocBlockSize, 0x0FFC0000);
    Dump("FFE on an Old 3DS (64 MiB)", o3ds, kSocBlockSize);

    // Old 3DS, the commit in use unreadable (the caller then passes the region size as the limit and 0 as
    // the use): the heap could reach the whole limit, so nothing under it is provable and the plan falls
    // back to the top of the heap area.
    Inputs o3ds_region_only{kOld, 0, 0x0FFC0000, kOld};
    n = Build(o3ds_region_only, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(pi.known && pi.provable && n > 0);
    CHECK(c[0].window == Window::HeapAreaTop && c[0].base == 0x0DFE0000);
    for (int i = 0; i < n; ++i) {
        CHECK(c[i].window != Window::LimitTop);
    }
    CheckPlan(c, n, pi, kSocBlockSize, 0x0FFC0000);

    // Extended-memory title: limit 124 MiB, 12 MB already committed.
    Inputs ext{126976u * 1024, 12000u * 1024, 0x0FFC0000, kNew};
    n = Build(ext, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(n > 0 && pi.known && !pi.provable);
    CHECK(pi.heap_reach == kHeapAreaEnd);                       // capped by the 96 MiB heap area
    CHECK(c[0].window == Window::ReserveTop && c[0].base == 0x0FFA0000);
    bool margin = false;
    for (int i = 0; i < n; ++i) {
        CHECK(c[i].window != Window::HeapAreaTop && c[i].window != Window::LimitTop);
        margin |= c[i].window == Window::HeapAreaMargin;
    }
    CHECK(margin);
    CHECK(c[n - 1].window == Window::HeapAreaMargin && c[n - 1].base == kHeapAreaEnd - kSocBlockSize - 3 * kStep);
    CheckPlan(c, n, pi, kSocBlockSize, 0x0FFC0000);
    Dump("ext 124 MiB", ext, kSocBlockSize);

    // Dev unit, 96 MiB, 10 MiB used: room 86 MiB -> still provable under 0x0E000000.
    Inputs dev{96u << 20, 10u << 20, 0x0FFC0000, 96u << 20};
    n = Build(dev, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(pi.provable);
    CHECK(c[0].window == Window::LimitTop && c[0].base == 0x0DFE0000); // region < 100 MiB: old layout order
    CheckPlan(c, n, pi, kSocBlockSize, 0x0FFC0000);
    Dump("dev 96 MiB", dev, kSocBlockSize);

    // Commit data unavailable: no proof, reserve first.
    Inputs unk{0, 0, 0, 0};
    n = Build(unk, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(!pi.known && !pi.provable && n > 0 && c[0].window == Window::ReserveTop && c[0].base == 0x0FEE0000);
    CheckPlan(c, n, pi, kSocBlockSize, 0);
    Dump("unknown commit", unk, kSocBlockSize);

    // Boundary: provable exactly while room <= 95 MiB (96 MiB cap minus the 1 MiB safety slack).
    Inputs edge_ok{95u << 20, 0, 0x0FFC0000, kNew};
    n = Build(edge_ok, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(pi.provable && c[0].base == 0x0DFE0000);
    Inputs edge_no{(95u << 20) + 0x1000, 0, 0x0FFC0000, kNew};
    n = Build(edge_no, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(!pi.provable && c[0].window == Window::ReserveTop);

    // Degenerate inputs must not wrap or crash.
    CHECK(Build(ffe, 0, c, kMaxCandidates, &pi) == 0);
    CHECK(Build(ffe, 0x1234, c, kMaxCandidates, &pi) == 0);       // not page aligned
    CHECK(Build(ffe, 0x7000000, c, kMaxCandidates, &pi) == 0);    // bigger than the heap area
    n = Build(ffe, 0x6000000, c, kMaxCandidates, &pi);            // exactly the whole heap area
    CHECK(!pi.provable);
    for (int i = 0; i < n; ++i) {
        CHECK(c[i].base >= kRegionBase);                          // nothing below the heap area
    }
    n = Build(ffe, 0x5F00000, c, kMaxCandidates, &pi);            // nearly the whole heap area: no wrap below it
    for (int i = 0; i < n; ++i) {
        CHECK(c[i].base >= kRegionBase);
    }
    Inputs full{65536u * 1024, 65536u * 1024, 0x0FFC0000, kNew};   // no commit left at all
    n = Build(full, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(pi.known && pi.room == 0 && pi.provable);               // the game cannot grow: top of the area is safe
    Inputs over{65536u * 1024, 70000u * 1024, 0x0FFC0000, kNew};   // used > limit: treated as unknown
    n = Build(over, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(!pi.known);
    Inputs tiny_span{65536u * 1024, 8692u * 1024, 0x0E010000, kNew}; // stack starts almost at 0x0E000000
    n = Build(tiny_span, kSocBlockSize, c, kMaxCandidates, &pi);
    for (int i = 0; i < n; ++i) {
        CHECK(c[i].window != Window::ReserveTop);                  // 0x10000 is smaller than the block
    }
    Inputs big_stack{126976u * 1024, 12000u * 1024, 0x0FC00000, kNew}; // 4 MiB stack
    n = Build(big_stack, kSocBlockSize, c, kMaxCandidates, &pi);
    CHECK(n > 0 && c[0].base == 0x0FC00000 - kSocBlockSize);
    Candidate small[3];
    CHECK(Build(ffe, kSocBlockSize, small, 3, &pi) == 3);          // capacity respected

    // What the start-up says on screen: nothing for the normal block, a line when only the smaller one could be
    // reserved (it holds a 32 KiB ring). There is no line about the memory margin any more ("Memory margin low" fired for every
    // game that is given its whole budget and never uses it - Tri Force Heroes with 56,320 KB free all session; what the game has
    // left is told from what it uses: azl/screen_text.hpp). Each block of the ladder is either the normal one or the reduced one.
    CHECK(StartupIssue(kSocBlockSize, false) == "");
    CHECK(StartupIssue(kSocBlockSize, true) == "");
    CHECK(StartupIssue(0, false) == "");                                      // no block (the plugin is off: it says so itself)
    CHECK(StartupIssue(0, true) == "");
    CHECK(StartupIssue(0x10000, false).find("Socket buffer reduced to 64 KB") != std::string::npos);
    CHECK(StartupIssue(0x10000, true).find("Socket buffer reduced to 64 KB") != std::string::npos);
    for (const bool log_on : {false, true}) {
        const std::string text = StartupIssue(0x10000, log_on);
        CHECK(text.find("argin") == std::string::npos);
        CHECK(text.find("AzaharLink.log") == std::string::npos); // (never sends the player to a file that may not be there)
        CHECK(text.size() > 40);                                  // a reason, not a bare label
    }
    for (const uint32_t size : kBlockSizes) {
        const bool reduced = size < kSocBlockSize;
        CHECK((StartupIssue(size, false)[0] != '\0') == reduced);
        CHECK(azl_rcvbuf_ring_budget(size) == (reduced ? 0x8000 : 0x10000)); // the warning is about this ring
    }
    // With debug=0 there is no log to look in: the text carries its reason and says how to get the details. With debug=1 the
    // details are being written already and the hint is dropped (it said "set debug=1" to a player who has it on).
    CHECK(StartupIssue(0x10000, false).find("set debug=1 for details") != std::string::npos);
    CHECK(StartupIssue(0x10000, true).find("debug=1") == std::string::npos);
    CHECK(StartupIssue(0x10000, true).find("details") == std::string::npos);
    CHECK(std::strcmp(azl::DebugHint(false), " (set debug=1 for details)") == 0);
    CHECK(std::strcmp(azl::DebugHint(true), "") == 0);

    std::printf("%s (%d checks)\n", g_fail ? "FAILED" : "all checks passed", g_checks);
    return g_fail != 0;
}
