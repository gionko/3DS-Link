// Tests for the rule that says which IPC calls of the game go to the file system (Includes/azl/game_card_calls.hpp, IsFileSystemCall):
// what the svc hook counts with the log on, so that the log's writer keeps the capture off the SD card while the game is at its files.
// The headers of the file system are the ones of the debug capture of a real console over one evening (2026-11-11, 1.1.3 with the log
// on, seven launches: New Super Mario Bros. 2, Donkey Kong Country Returns 3D, Mario Kart 7 and Tekken 3D Prime Edition among them -
// every `cmd=0x08......` of it) and the ones libctru's fs.c sends; the headers that
// must NOT count are the ones a game sends all the time to the other services (gsp, dsp, srv, cfg:u, APT, hid, nwm::UDS, soc:U ...).
// The last test plays the hook's two lines (PassThroughPre / PassThroughPost) with real calls around a watch of the writer.
//   game_card_calls_test
#include <cstdio>

#include "azl/capture_pace.hpp"
#include "azl/game_card_calls.hpp"

using namespace azl;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

// (the rule is a constant expression: the hook's `if` is a shift and a compare, and a compiler that could not fold it here would say so)
static_assert(IsFileSystemCall(0x08030204), "fs:USER OpenFileDirectly, the call of Tekken 3D that waited 26 s, is a file call");
static_assert(IsFileSystemCall(0x080200C2), "FSFILE_Read, the whole rule of 2026-11-10, still is one");
static_assert(!IsFileSystemCall(0x000C0000), "gsp::Gpu 0x000C is not");
static_assert(kFsCommandFamily == 0x08, "the family of the file system's commands");

namespace {

struct Header {
    u32 word;
    const char* name;
};

// Every fs header of that capture with the calls it has of it (the calls the diet of the IPC lines folded included), named from
// libctru's fs.c: 9036 reads and 221 calls that are not reads.
const Header kSeenInTheLogs[] = {
    {0x080200C2, "FSFILE_Read (9036)"},
    {0x08080000, "FSFILE_Close (54)"},
    {0x080201C2, "FSUSER_OpenFile (32)"},
    {0x08030204, "FSUSER_OpenFileDirectly (28): the call of Tekken 3D that came back after 26.4 s"},
    {0x080C00C2, "FSUSER_OpenArchive (24)"},
    {0x080E0080, "FSUSER_CloseArchive (19)"},
    {0x08040000, "FSFILE_GetSize (13)"},
    {0x08030102, "FSFILE_Write (10)"},
    {0x080B0102, "FSUSER_OpenDirectory (7)"},
    {0x08630000, "FSUSER_GetPriority (5)"},
    {0x080D0144, "FSUSER_ControlArchive (5)"},
    {0x08010042, "FSDIR_Read (5)"},
    {0x08020000, "FSDIR_Close (3)"},
    {0x08620040, "FSUSER_SetPriority (2)"},
    {0x08610042, "FSUSER_InitializeWithSdkVersion (2)"},
    {0x08080202, "FSUSER_CreateFile (2)"},
    {0x08050080, "FSFILE_SetSize (2)"},
    {0x08040142, "FSUSER_DeleteFile (2)"},
    {0x08010002, "FSUSER_Initialize (2)"},
    {0x08180000, "FSUSER_IsSdmcWritable (1)"},
    {0x08170000, "FSUSER_IsSdmcDetected (1)"},
    {0x080C0000, "FSFILE_OpenLinkFile (1)"},
    {0x080A0040, "FSFILE_SetPriority (1)"},
};

// Others of libctru's fs.c that a game may send (the words are the ones in the library the plugin is built with).
const Header kOtherFsCalls[] = {
    {0x08010100, "FSFILE_OpenSubFile"},
    {0x08060000, "FSFILE_GetAttributes"},
    {0x08070040, "FSFILE_SetAttributes"},
    {0x08090000, "FSFILE_Flush"},
    {0x080B0000, "FSFILE_GetPriority"},
    {0x08030040, "FSDIR_SetPriority"},
    {0x08050244, "FSUSER_RenameFile"},
    {0x08060142, "FSUSER_DeleteDirectory"},
    {0x08070142, "FSUSER_DeleteDirectoryRecursively"},
    {0x08090182, "FSUSER_CreateDirectory"},
    {0x080A0244, "FSUSER_RenameDirectory"},
    {0x08120080, "FSUSER_GetFreeBytes"},
    {0x08130000, "FSUSER_GetCardType"},
    {0x08210000, "FSUSER_CardSlotIsInserted"},
    {0x084500C2, "FSUSER_GetFormatInfo"},
    {0x084C0242, "FSUSER_FormatSaveData"},
    {0x08510242, "FSUSER_CreateExtSaveData"},
    {0x08520100, "FSUSER_DeleteExtSaveData"},
    {0x085A00C0, "FSUSER_SetArchivePriority"},
    {0x08650140, "FSUSER_SetSaveDataSecureValue"},
    {0x086600C0, "FSUSER_GetSaveDataSecureValue"},
    {0x08680000, "FSUSER_GetMediaType"},
};

// What a game sends to the services that are not the file system: none of these may keep the capture off the card.
const Header kNotFileCalls[] = {
    {0x000C0000, "gsp::Gpu TriggerCmdReqQueue (101 times in 2 s of the Tekken capture)"},
    {0x00080082, "gsp::Gpu FlushDataCache: the 0x08 is the LOW byte of the command id"},
    {0x00050200, "gsp::Gpu SetBufferSwap"},
    {0x00130042, "gsp::Gpu RegisterInterruptRelayQueue"},
    {0x001F0000, "dsp::DSP GetHeadphoneStatus (every 156 ms from Tekken's audio thread)"},
    {0x000D0082, "dsp::DSP WriteProcessPipe"},
    {0x00050100, "srv: GetServiceHandle"},
    {0x000B0000, "srv: ReceiveNotification"},
    {0x00010002, "srv: RegisterClient"},
    {0x00010082, "cfg:u GetConfigInfoBlk2"},
    {0x00020000, "cfg:u SecureInfoGetRegion"},
    {0x000B0040, "APT:U InquireNotification"},
    {0x000D0080, "APT:U ReceiveParameter"},
    {0x003E0080, "APT:U ReplySleepQuery"},
    {0x00430040, "APT:U NotifyToWait"},
    {0x004B00C2, "APT:U AppletUtility"},
    {0x01020000, "a command 0x0102 (APT CheckNew3DS)"},
    {0x000A0000, "hid:USER GetIPCHandles"},
    {0x001B0302, "nwm::UDS InitializeWithVersion"},
    {0x001400C0, "nwm::UDS PullPacket"},
    {0x00170182, "nwm::UDS SendTo"},
    {0x000F0404, "nwm::UDS RecvBeaconBroadcastData"},
    {0x00080102, "soc:U recvfrom"},
    {0x000A0106, "soc:U sendto"},
    {0x00140084, "soc:U poll"},
    {0x00010800, "err:f ThrowFatalError"},
    {0x00010040, "APT:U GetLockHandle"},
    {0x00010042, "ndm:u EnterExclusiveState"},
    {0x04010082, "a command 0x0401 (cfg:s GetConfigInfoBlk4)"},
    {0x040A0000, "a command 0x040A (ptm:sysm CheckNew3DS)"},
    {0x04060080, "a command 0x0406 (fs:REG CheckHostLoadId: the registry of fs, not a game's and not the card)"},
    {0x10010080, "a command 0x1001 (the family of am:app)"},
    {0x10050084, "a command 0x1005 (the family of am:app)"},
    {0x00000000, "no command"},
    {0xFFFFFFFF, "all ones"},
    // one bit away from the family, and the family's byte in every other place
    {0x00010008, "0x08 in the low byte of the header"},
    {0x00030800, "0x08 in the second byte"},
    {0x00080000, "0x08 in the low byte of the command id"},
    {0x07FF0000, "the command before the family"},
    {0x09000000, "the command after the family"},
    {0x09010002, "0x0901"},
    {0x0C020000, "0x0C02 (0x08 with a bit more)"},
    {0x18010002, "0x1801"},
    {0x28010002, "0x2801"},
    {0x48010002, "0x4801"},
    {0x88010002, "0x8801 (the top bit set)"},
    {0x00010002, "0x0001 (the family's byte cleared)"},
    {0x80000000, "the top bit alone"},
};

// The commands 0x08xx of services that are not the file system, as libctru sends them: the rule cannot tell them from a file call (it
// has the header and nothing else), and says so here. None is a service a game's exheader opens (cfg:i, am:net, ptm:sysm, qtm:sp are
// the system's); were one to come through the hook, the capture would step aside for it - within pace::kSkipForceMs like everything.
const Header kOtherServicesSameFamily[] = {
    {0x08010082, "cfg:i GetConfigInfoBlk8"},
    {0x08030000, "cfg:i UpdateConfigSavegame"},
    {0x08010000, "am:net InstallTicketBegin"},
    {0x08040100, "am:net InstallTitleBegin"},
    {0x08180040, "ptm:sysm ConfigureNew3DSCPU"},
    {0x08160000, "ptm:sysm GetRtcTime"},
    {0x08010040, "qtm:sp NotifyTopLcdModeChange"},
};

void TestTheCallsOfTheLogs() {
    std::printf("every file-system call of the console's captures counts, and the calls to the other services do not\n");
    for (const Header& h : kSeenInTheLogs) {
        CHECK(IsFileSystemCall(h.word));
        if (!IsFileSystemCall(h.word)) {
            std::fprintf(stderr, "    0x%08lx %s\n", static_cast<unsigned long>(h.word), h.name);
        }
    }
    for (const Header& h : kOtherFsCalls) {
        CHECK(IsFileSystemCall(h.word));
        if (!IsFileSystemCall(h.word)) {
            std::fprintf(stderr, "    0x%08lx %s\n", static_cast<unsigned long>(h.word), h.name);
        }
    }
    for (const Header& h : kNotFileCalls) {
        CHECK(!IsFileSystemCall(h.word));
        if (IsFileSystemCall(h.word)) {
            std::fprintf(stderr, "    0x%08lx %s\n", static_cast<unsigned long>(h.word), h.name);
        }
    }
    // the rule of 2026-11-10 was this one word: the opens, the writes and the closes are the news
    int more_than_the_read = 0;
    for (const Header& h : kSeenInTheLogs) {
        if (h.word != 0x080200C2 && IsFileSystemCall(h.word)) {
            ++more_than_the_read;
        }
    }
    CHECK(more_than_the_read == 22);
}

void TestTheWholeCommandSpace() {
    std::printf("the rule over every command id: 0x0800..0x08FF and nothing else, whatever the sizes in the low half of the header\n");
    // (the sizes a header can carry: every count of normal words with every count of translate words would be 4096 a command; the
    // corners and a walking bit say the same of a rule that must not look at them at all)
    const u32 kSizes[] = {0x0000, 0x0001, 0x0002, 0x0040, 0x0042, 0x0080, 0x00C2, 0x0102, 0x0204, 0x0800, 0x0808, 0x8000, 0xFFFF,
                          0x0004, 0x0008, 0x0010, 0x0020, 0x0100, 0x0200, 0x0400, 0x1000, 0x2000, 0x4000};
    u32 counted = 0;
    u32 wrong = 0;
    for (u32 command = 0; command <= 0xFFFF; ++command) {
        const bool file_system = command >= 0x0800 && command <= 0x08FF;
        for (const u32 sizes : kSizes) {
            if (IsFileSystemCall(command << 16 | sizes) != file_system) {
                ++wrong;
            }
        }
        if (IsFileSystemCall(command << 16)) {
            ++counted;
        }
    }
    CHECK(wrong == 0);
    CHECK(counted == 256); // (the whole byte is the family's: fs:USER alone goes from 0x0801 to 0x0868 in libctru, and a newer one is a file call too)
    // every header whose high byte is not the family's: none counts, one bit at a time and all at once
    for (u32 high = 0; high <= 0xFF; ++high) {
        CHECK(IsFileSystemCall(high << 24 | 0x00030204) == (high == 0x08));
        CHECK(IsFileSystemCall(high << 24 | 0x00FFFFFF) == (high == 0x08));
        CHECK(IsFileSystemCall(high << 24) == (high == 0x08));
    }
}

void TestWhatTheRuleCannotTell() {
    std::printf("the commands 0x08xx of the system's own services count too (known: no game opens them)\n");
    for (const Header& h : kOtherServicesSameFamily) {
        CHECK(IsFileSystemCall(h.word));
    }
}

// The hook's two lines, as Sources/ctr/hook.cpp has them (Tests/policy_check.sh pins the lines themselves): pre counts a call that
// begins, post one that ended, and both ask the same question of the same header.
struct HookModel {
    u32 begun = 0;
    u32 done = 0;
    void Pre(u32 header) {
        if (IsFileSystemCall(header)) {
            ++begun;
        }
    }
    void Post(u32 header) {
        if (IsFileSystemCall(header)) {
            ++done;
        }
    }
};

void TestAnOpenKeepsTheCaptureOffTheCard() {
    std::printf("an open, a write or a close of the game under way keeps the capture off the card as a read does; a call to gsp does not\n");
    typedef pace::GameReadWatch W;
    // every file call of the captures, one at a time: under way -> not now; ended since the last look -> not now; quiet -> the card is free
    for (const Header& h : kSeenInTheLogs) {
        HookModel hook;
        W watch;
        u64 t = 1000;
        CHECK(int(watch.Look(hook.begun, hook.done, t, true)) == int(W::kPass));
        hook.Pre(h.word);
        CHECK(int(watch.Look(hook.begun, hook.done, t += 50, true)) == int(W::kSkip));
        CHECK(int(watch.Look(hook.begun, hook.done, t += 50, true)) == int(W::kSkip)); // (still under way)
        hook.Post(h.word);
        CHECK(int(watch.Look(hook.begun, hook.done, t += 50, true)) == int(W::kSkip)); // (it ended since the last look)
        CHECK(int(watch.Look(hook.begun, hook.done, t += 50, true)) == int(W::kPass));
        CHECK(watch.reads == 1u);
        CHECK(hook.begun == 1u && hook.done == 1u);
    }
    // the calls to the other services, as many as a game makes in a frame: the two words never move and the capture writes
    {
        HookModel hook;
        W watch;
        u64 t = 1000;
        for (int frame = 0; frame < 100; ++frame) {
            for (const Header& h : kNotFileCalls) {
                hook.Pre(h.word);
                CHECK(int(watch.Look(hook.begun, hook.done, t += 1, true)) == int(W::kPass));
                hook.Post(h.word);
            }
            CHECK(int(watch.Look(hook.begun, hook.done, t += 16, true)) == int(W::kPass));
        }
        CHECK(hook.begun == 0u && hook.done == 0u);
        CHECK(watch.reads == 0u);
    }
    // Tekken 3D at the start of a match (2026-11-11): the sequence of the capture - cfg:u twice, then the opens, each with its reads
    // and its close - with looks 50 ms apart. From the first open to the last close no look gives the card to the capture; with the
    // rule of 2026-11-10 (the reads alone) the look that falls inside an open did, and the game's next call waited behind that write.
    {
        HookModel hook;
        W watch;
        u64 t = 190332;
        int passes = 0;
        int looks = 0;
        const auto look = [&](u64 at) {
            ++looks;
            if (watch.Look(hook.begun, hook.done, at, true) == W::kPass) {
                ++passes;
            }
        };
        hook.Pre(0x00020000); // cfg:u
        hook.Post(0x00020000);
        hook.Pre(0x00010082);
        hook.Post(0x00010082);
        look(t); // (nothing of the file system yet: the capture may write - on the console this is the write the open then waits behind)
        CHECK(passes == 1);
        for (int file = 0; file < 12; ++file) {
            hook.Pre(0x08030204); // an open that takes 100 ms: two looks fall inside it
            look(t += 50);
            look(t += 50);
            hook.Post(0x08030204);
            hook.Pre(0x08040000); // the size
            hook.Post(0x08040000);
            for (int block = 0; block < 4; ++block) {
                hook.Pre(0x080200C2);
                look(t += 50);
                hook.Post(0x080200C2);
            }
            hook.Pre(0x08080000); // the close
            hook.Post(0x08080000);
            look(t += 50);
        }
        CHECK(looks == 1 + 12 * 7);
        CHECK(passes == 1); // (none after the first: 4.2 s of looks, every one of them with the game at its files)
        CHECK(hook.begun == 12u * 7 && hook.done == hook.begun);
        look(t += 50); // quiet since the look that saw the last close end: the card is the capture's again
        CHECK(passes == 2);
        CHECK(watch.realigned == 0u);
    }
    // an open whose post never runs (the swap of the plugin's block on an Old 3DS during the call) is put right like a read's: after
    // pace::kReadStuckMs of neither word moving the capture has the card back
    {
        HookModel hook;
        W watch;
        u64 t = 5000;
        (void)watch.Look(hook.begun, hook.done, t, true);
        hook.Pre(0x08030204);
        int skips = 0;
        W::Verdict verdict = W::kSkip;
        for (int i = 0; i < 100 && verdict == W::kSkip; ++i) {
            verdict = watch.Look(hook.begun, hook.done, t += 50, true, 1);
            if (verdict == W::kSkip) {
                ++skips;
            }
        }
        CHECK(int(verdict) == int(W::kPass));
        CHECK(skips == 60); // (3 s of looks 50 ms apart)
        CHECK(watch.realigned == 1u);
    }
}

} // namespace

int main() {
    TestTheCallsOfTheLogs();
    TestTheWholeCommandSpace();
    TestWhatTheRuleCannotTell();
    TestAnOpenKeepsTheCaptureOffTheCard();
    std::printf("game_card_calls_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
