// The lists of ids of the console, READ ONLY (1.1.4, the build 2h: dlp_guest_ids.hpp): the titles of the SD card and of the NAND, the tickets, the
// game card, the titles AM has begun (SD and NAND), the processes that run. The ONE file of the app that names MEDIATYPE_NAND, and only in the calls
// that list (AM_GetTitleCount, AM_GetTitleList, AM_GetNumPendingTitles, AM_GetPendingTitleList): nothing here writes, deletes or installs anything.
// The processes are read as zombie_census.cpp reads them: svcGetProcessList, then each process opened only to read its title id (Luma's
// svcGetProcessInfo 0x10001) and closed at once - never svcGetThreadList (it keeps a reference on the process for ever).
// On an emulator (Azahar: svcGetSystemInfo 0x20000) a list of the tickets, of the processes or of the titles begun that the emulator does not give
// is an EMPTY list, not one "not read": its AM and its pm are not a console's (no closed game stays in memory there, nothing can be broken), and
// the tests of the build there would stop at the guard. On a console every needed list must be read whole, or nothing is written (R1).
#include <3ds.h>

#include <vector>

#include "dlp_guest_am.hpp"
#include "dlp_guest_ids.hpp"

namespace dlpguest {

namespace {

constexpr u32 kMaxListed = 65536; // (a list longer than this is not read: nothing is decided on a part of it. The review of the build 2h, R5: the
                                  // packs of "every eShop ticket" hold more than 16384 tickets - a list cut there stopped every write)
constexpr s32 kMaxProcesses = 256; // (a console runs about 40 - closed games kept in memory too; a list that fills the buffer is not read whole)

u64 NowMsOfTicks() {
    return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

bool OnEmulator() {
    s64 v = 0;
    svcGetSystemInfo(&v, 0x20000, 0); // (DPAPP-2k: 0 on a console's Luma, 1 on Citra, 2 on Azahar - never its Result: Luma's is 1, positive)
    return v != 0;
}

bool ListTitles(FS_MediaType media, std::vector<u64>* ids) {
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(media, &count)) || count > kMaxListed) {
        return false;
    }
    ids->assign(count, 0);
    u32 got = 0;
    if (count > 0 && R_FAILED(AM_GetTitleList(&got, media, count, ids->data()))) {
        return false;
    }
    return got == count; // (the review of the build 2h, R5: fewer ids than AM counted is a list not read whole - never taken for the whole one)
}

bool ListPending(FS_MediaType media, std::vector<u64>* ids) {
    u32 count = 0;
    ids->clear();
    if (R_FAILED(AM_GetNumPendingTitles(&count, media)) || count > kMaxListed) {
        return OnEmulator(); // (an emulator that does not give it: empty)
    }
    ids->assign(count, 0);
    u32 got = 0;
    if (count > 0 && R_FAILED(AM_GetPendingTitleList(&got, ids->data(), count, media))) {
        return false;
    }
    if (got != count) {
        ids->resize(got < count ? got : count);
        return OnEmulator(); // (the review of the build 2h, R5: on a console a list shorter than its count is not read whole)
    }
    return true;
}

bool ListTickets(std::vector<u64>* ids) {
    u32 count = 0;
    ids->clear();
    if (R_FAILED(AM_GetTicketCount(&count)) || count > kMaxListed) {
        return OnEmulator(); // (an emulator that does not give it: empty)
    }
    ids->assign(count, 0);
    u32 got = 0;
    if (count > 0 && R_FAILED(AM_GetTicketList(&got, count, 0, ids->data()))) {
        return false;
    }
    if (got != count) {
        ids->resize(got < count ? got : count);
        return OnEmulator(); // (the review of the build 2h, R5: on a console a list shorter than its count is not read whole)
    }
    return true;
}

bool ListCard(std::vector<u64>* ids) {
    ids->clear();
    bool inserted = false;
    if (R_FAILED(FSUSER_CardSlotIsInserted(&inserted))) {
        return false;
    }
    if (!inserted) {
        return true; // (an empty slot: an empty list)
    }
    return ListTitles(MEDIATYPE_GAME_CARD, ids);
}

// The title ids of the processes. On a console every process must give its id (Luma's 0x10001), or the list is not read (R1: nothing is written);
// an emulator that does not give it is read without that process (Azahar's own processes are not a console's).
bool ListProcesses(std::vector<u64>* ids) {
    ids->clear();
    s32 n = 0;
    u32 pids[kMaxProcesses] = {};
    const bool emulator = OnEmulator();
    if (R_FAILED(svcGetProcessList(&n, pids, kMaxProcesses)) || n < 0 || n > kMaxProcesses) {
        return emulator; // (an emulator that does not give it: empty)
    }
    if (n == kMaxProcesses) {
        return false; // (the review of the build 2h, R5: a full buffer may be a list cut short - svcGetProcessList does not say there are more)
    }
    for (s32 i = 0; i < n; ++i) {
        Handle h = 0;
        if (R_FAILED(svcOpenProcess(&h, pids[i]))) {
            if (!emulator) {
                return false;
            }
            continue;
        }
        s64 tid = 0;
        const Result rc = svcGetProcessInfo(&tid, h, 0x10001); // (Luma: the title id of the process's code set - the ACI's)
        svcCloseHandle(h);
        if (R_FAILED(rc)) {
            if (!emulator) {
                return false;
            }
            continue;
        }
        ids->push_back(static_cast<u64>(tid));
    }
    return true;
}

struct ConsoleIdReader final : IdReader {
    bool List(int source, std::vector<u64>* ids) override {
        switch (source) {
        case kSrcSd: return ListTitles(MEDIATYPE_SD, ids);
        case kSrcNand: return ListTitles(MEDIATYPE_NAND, ids);
        case kSrcTicket: return ListTickets(ids);
        case kSrcCard: return ListCard(ids);
        case kSrcProcess: return ListProcesses(ids);
        case kSrcPendingSd: return ListPending(MEDIATYPE_SD, ids);
        case kSrcPendingNand: return ListPending(MEDIATYPE_NAND, ids);
        default: return false;
        }
    }
    u64 NowMs() override {
        return NowMsOfTicks();
    }
};

} // namespace

bool ReadConsoleIds(IdCensus* out, RefreshReport* report) {
    *out = IdCensus();
    ConsoleIdReader reader;
    return RefreshCensus(out, reader, report);
}

bool RefreshConsoleIds(IdCensus* census, RefreshReport* report) {
    ConsoleIdReader reader;
    return RefreshCensus(census, reader, report);
}

} // namespace dlpguest
