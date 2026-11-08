// The pure logic of the zombie probe (ctr/zombie_probe.hpp), run by Tests/run.sh with ASan/UBSan. The console reads
// (svcs) are not here; only the parts a PC can hold:
// - the scheduling-mask classification: a thread is notable with 0x20 (Luma hold), 0x40, or a low nibble that is not 0
//   (runnable), 1 (running) or 2 (terminating); a terminating thread is not a hold;
// - the memory-map merge: adjacent runs of equal state and perm coalesce, non-adjacent equal runs do not, and a crafted
//   MHXX-like layout gives the runs expected;
// - the IPC reply handle-transfer decode: a copy (0x00) or move (0x10) descriptor yields the handle and its kind, a
//   buffer/static section is skipped, an empty translate section yields nothing, a malformed one is refused;
// - the startup-occupancy decision: a zombie in the books or in the region is foreign with `own` from the process, and
//   the margins' edges;
// - the memory-map walk: merges, skips FREE, never loops (no progress, a wrap past the top, size 0), marks the cap's cut;
// - the handle classes (KServerSession, not KSession) and the owned-object sum the refcount line subtracts.
#include <cstdio>
#include <vector>

#include "ctr/zombie_probe.hpp"

using namespace azl;
using namespace azl::zombie;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)

static void TestSchedMask() {
    // runnable / running / terminating are not notable
    CHECK(!SchedMaskNotable(0x00)); // runnable
    CHECK(!SchedMaskNotable(0x01)); // running
    CHECK(!SchedMaskNotable(0x02)); // terminating
    // a hold is notable whatever the run state
    CHECK(SchedMaskNotable(0x20));
    CHECK(SchedMaskNotable(0x21));
    CHECK(SchedMaskNotable(0x22)); // held even while terminating
    CHECK(SchedMaskNotable(0x40));
    // an out-of-the-ordinary low nibble is notable, a plain terminating one is not
    CHECK(SchedMaskNotable(0x03));
    CHECK(SchedMaskNotable(0x0F));
    CHECK(!SchedMaskNotable(0x02));
}

// Feeds a list of blocks through the merger and returns the runs it emits (Flush included).
static std::vector<MergedRun> Merge(const std::vector<MergedRun>& blocks) {
    MapMerger m;
    std::vector<MergedRun> out;
    MergedRun r;
    for (const MergedRun& b : blocks) {
        if (m.Add(b.base, b.size, b.state, b.perm, &r)) {
            out.push_back(r);
        }
    }
    if (m.Flush(&r)) {
        out.push_back(r);
    }
    return out;
}

static void TestMapMerge() {
    // two adjacent blocks of equal state and perm merge into one
    auto a = Merge({{0x100000, 0x1000, 3, 5}, {0x101000, 0x1000, 3, 5}});
    CHECK(a.size() == 1 && a[0].base == 0x100000 && a[0].size == 0x2000);
    // adjacent but different state: two runs
    auto b = Merge({{0x100000, 0x1000, 3, 5}, {0x101000, 0x1000, 4, 5}});
    CHECK(b.size() == 2 && b[0].state == 3 && b[1].state == 4);
    // non-adjacent (a gap), same state: two runs, NOT merged
    auto c = Merge({{0x100000, 0x1000, 3, 5}, {0x200000, 0x1000, 3, 5}});
    CHECK(c.size() == 2 && c[0].base == 0x100000 && c[1].base == 0x200000);
    // a crafted MHXX-like layout: code, static, several heap runs that coalesce, then linear
    auto d = Merge({
        {0x00100000, 0x80000, 4, 5},   // .text
        {0x00180000, 0x10000, 3, 3},   // .rodata
        {0x08000000, 0x100000, 5, 3},  // heap
        {0x08100000, 0x200000, 5, 3},  // more heap, same attrs -> merges
        {0x08300000, 0x100000, 5, 3},  // still heap
        {0x14000000, 0x400000, 7, 3},  // linear
    });
    CHECK(d.size() == 4);
    CHECK(d[0].base == 0x00100000 && d[0].size == 0x80000);
    CHECK(d[2].base == 0x08000000 && d[2].size == 0x400000); // the three heap runs became one
    CHECK(d[3].base == 0x14000000);
    // nothing fed: nothing emitted
    CHECK(Merge({}).empty());
}

// A reply command buffer: header, then normal params, then the translate section.
static void TestIpcDecode() {
    // srv:GetServiceHandle reply: header 0x00050042 (1 normal, 2 translate), result, move descriptor 0x10, handle
    {
        u32 cmd[4] = {0x00050042, 0x00000000, 0x00000010, 0xDEAD0001};
        HandleXfer x = DecodeReplyHandleXfer(cmd, 4);
        CHECK(x.found && x.move && x.handle == 0xDEAD0001);
    }
    // a copy-handle reply: descriptor 0x00
    {
        u32 cmd[4] = {0x00010042, 0x00000000, 0x00000000, 0xBEEF0002};
        HandleXfer x = DecodeReplyHandleXfer(cmd, 4);
        CHECK(x.found && !x.move && x.handle == 0xBEEF0002);
    }
    // no translate section: nothing
    {
        u32 cmd[2] = {0x00010040, 0x00000000}; // 1 normal, 0 translate
        HandleXfer x = DecodeReplyHandleXfer(cmd, 2);
        CHECK(!x.found);
    }
    // a static-buffer descriptor (low bits 0b10) before a handle descriptor: the buffer is skipped, the handle found
    {
        // header: 1 normal, 4 translate -> begin at word 2, end at word 6
        u32 cmd[6] = {0x00010044, 0x00000000, 0x00000002 /*static buf desc*/, 0x08000000 /*ptr*/, 0x00000010 /*move*/, 0xCAFE0003};
        HandleXfer x = DecodeReplyHandleXfer(cmd, 6);
        CHECK(x.found && x.move && x.handle == 0xCAFE0003);
    }
    // a translate section the header claims longer than the buffer: refused, no read past the end
    {
        u32 cmd[2] = {0x0001004A, 0x00000000}; // claims 10 translate words, buffer has 2
        HandleXfer x = DecodeReplyHandleXfer(cmd, 2);
        CHECK(!x.found);
    }
    // a two-handle copy descriptor: (count-1)<<26 = 1<<26, the first handle is reported
    {
        u32 cmd[5] = {0x00010043, 0x00000000, (1u << 26) | 0x0, 0x11110001, 0x11110002};
        HandleXfer x = DecodeReplyHandleXfer(cmd, 5);
        CHECK(x.found && !x.move && x.handle == 0x11110001);
    }
    // null / zero length: nothing, no crash
    {
        CHECK(!DecodeReplyHandleXfer(nullptr, 4).found);
        u32 cmd[1] = {0x00010040};
        CHECK(!DecodeReplyHandleXfer(cmd, 0).found);
    }
}

// The startup-occupancy decision (Plugin::CheckStartupOccupancy). KB figures from the console-63 logs of 2026-11-07.
static void TestStartupForeign() {
    const s64 K = 1024;
    const s64 plugin = 5120 * K;
    // A clean launch of Monster Hunter XX (console 63): books = its commit, own (type 2) a few KB over, the region = own + the plugin's block.
    CHECK(StartupForeign(6232 * K, 11352 * K, 6236 * K, plugin) == 0);
    // The relaunch after a stuck close: the zombie's 82048 KB commit in the books and its 82040 KB of pages in the region. Both say so.
    const s64 books = (82048 + 6232) * K;
    const s64 used = (82040 + 6232 + 5120) * K;
    CHECK(StartupForeign(books, used, 6236 * K, plugin) == (kStartupForeignBooks | kStartupForeignRegion));
    // The first version took `own` from the limit's current: the zombie cancels on both sides and nothing fires (the review's blocker).
    CHECK(StartupForeign(books, used, books, plugin) == 0);
    // 3DS Link's own reading of that state (3DSLink-heap.log: current 87340, used 87332, own 5304 KiB, no plugin): foreign in both.
    CHECK(StartupForeign(87340 * K, 87332 * K, 5304 * K, 0) == (kStartupForeignBooks | kStartupForeignRegion));
    // The margins' edges: equal is not foreign, one page over is.
    const s64 own = 6236 * K;
    CHECK(StartupForeign(own + kStartupMargin, 0, own, plugin) == 0);
    CHECK(StartupForeign(own + kStartupMargin + 4096, 0, own, plugin) == kStartupForeignBooks);
    CHECK(StartupForeign(0, own + plugin + kStartupMargin, own, plugin) == 0);
    CHECK(StartupForeign(0, own + plugin + kStartupMargin + 4096, own, plugin) == kStartupForeignRegion);
    // The books only (a zombie's pages not in the region's count): still said.
    CHECK(StartupForeign(books, 11352 * K, own, plugin) == kStartupForeignBooks);
    // own unknown (the svc failed, or 0): no decision.
    CHECK(StartupForeign(books, used, 0, plugin) == 0);
    CHECK(StartupForeign(books, used, -1, plugin) == 0);
}

// The memory-map walk over a fake address space.
struct FakeSpace {
    std::vector<MapBlock> blocks;
    int queries = 0;
    bool operator()(u32 addr, MapBlock* b) {
        ++queries;
        for (const MapBlock& m : blocks) {
            if (addr >= m.base && addr - m.base < m.size) {
                *b = m;
                return true;
            }
        }
        return false;
    }
};

static void TestWalkMap() {
    // Two adjacent equal runs merge, a FREE gap is skipped, the last run is flushed.
    {
        FakeSpace f;
        f.blocks = {{0x00100000, 0x1000, 5, 5}, {0x00101000, 0x1000, 5, 5}, {0x00102000, 0x10000, kMemStateFree, 0},
                    {0x00112000, 0x2000, 9, 3}};
        std::vector<MergedRun> out;
        const WalkEnd w = WalkMap(0x00100000, 0x00114000, 128, f, [&](const MergedRun& r) { out.push_back(r); });
        CHECK(!w.truncated && w.runs == 2 && out.size() == 2);
        CHECK(out.size() == 2 && out[0].base == 0x00100000 && out[0].size == 0x2000 && out[1].base == 0x00112000);
    }
    // The cap: three separate runs, a cap of two: two emitted, the cut said, at the third's base.
    {
        FakeSpace f;
        f.blocks = {{0x1000, 0x1000, 5, 5}, {0x2000, 0x1000, 6, 5}, {0x3000, 0x1000, 7, 5}};
        int emitted = 0;
        const WalkEnd w = WalkMap(0x1000, 0x4000, 2, f, [&](const MergedRun&) { ++emitted; });
        CHECK(w.truncated && w.at == 0x3000 && w.runs == 2 && emitted == 2);
    }
    // The cap cutting in the middle: four runs, a cap of two: the third is where it was cut.
    {
        FakeSpace f;
        f.blocks = {{0x1000, 0x1000, 5, 5}, {0x2000, 0x1000, 6, 5}, {0x3000, 0x1000, 7, 5}, {0x4000, 0x1000, 8, 5}};
        int emitted = 0;
        const WalkEnd w = WalkMap(0x1000, 0x5000, 2, f, [&](const MergedRun&) { ++emitted; });
        CHECK(w.truncated && w.at == 0x3000 && emitted == 2);
    }
    // Never loops: a block that does not move the address forward (base + size <= addr) stops the walk.
    {
        // A query that always answers a block ending below the address asked.
        int calls = 0;
        const auto stuck = [&](u32, MapBlock* b) {
            ++calls;
            *b = MapBlock{0x1000, 0x1000, 5, 5};
            return true;
        };
        const WalkEnd w = WalkMap(0x5000, 0x9000, 128, stuck, [](const MergedRun&) {});
        CHECK(calls == 1 && !w.truncated && w.runs == 1);
    }
    // A wrap past the top (base + size overflows to 0) stops the walk.
    {
        int calls = 0;
        const auto top = [&](u32, MapBlock* b) {
            ++calls;
            *b = MapBlock{0xFFFFF000u, 0x1000, 5, 5};
            return true;
        };
        const WalkEnd w = WalkMap(0xFFFFF000u, 0xFFFFFFFFu, 128, top, [](const MergedRun&) {});
        CHECK(calls == 1 && w.runs == 1);
    }
    // A block of size 0 and a failed query stop it; nothing in the space: nothing emitted.
    {
        const auto zero = [](u32 addr, MapBlock* b) {
            *b = MapBlock{addr, 0, 5, 5};
            return true;
        };
        CHECK(WalkMap(0x1000, 0x9000, 128, zero, [](const MergedRun&) {}).runs == 0);
        FakeSpace empty;
        CHECK(WalkMap(0x1000, 0x9000, 128, empty, [](const MergedRun&) {}).runs == 0 && empty.queries == 1);
    }
}

// The handle classes and the owned-object sum.
static void TestHandleClassesAndOwned() {
    CHECK(HandleClassIndex(0x1F) == 0);  // event
    CHECK(HandleClassIndex(0xA5) == 8);  // client session
    CHECK(HandleClassIndex(0x95) == 7);  // server session (a KSession 0x70 is never in a handle table)
    CHECK(HandleClassIndex(0x70) == kHandleClasses); // (other)
    CHECK(HandleClassIndex(0xC5) == kHandleClasses); // KProcess: other
    CHECK(HandleClassIndex(0) == kHandleClasses);
    const s64 cur[8] = {6232 * 1024, 12, 40, 9, 3, 2, 4, 1};
    CHECK(OwnedObjects(cur, 1, 8) == 12 + 40 + 9 + 3 + 2 + 4 + 1);
    const s64 bad[8] = {0, 12, -1, 9, 3, 2, 4, 1};
    CHECK(OwnedObjects(bad, 1, 8) == -1); // (a class the svc did not give: no residual)
}

int main() {
    TestSchedMask();
    TestMapMerge();
    TestIpcDecode();
    TestStartupForeign();
    TestWalkMap();
    TestHandleClassesAndOwned();
    std::printf("zombie_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
