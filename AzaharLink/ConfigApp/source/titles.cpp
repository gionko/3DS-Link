#include "titles.hpp"

#include <sys/stat.h>

#include <strings.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>

#include "../titleshare_host/delivery.h"
#include "ncch_icon.hpp"
#include "paths.hpp"

// The delivery module is C and its headers have no C++ guards: they are included as C. Its files are used as they are, apart from the
// documented patches (see ConfigApp/titleshare/PATCHES.md); everything this app adds around them is in this file and in ncch_icon.cpp.
extern "C" {
#include "../titleshare/source/catalog.h"
#include "../titleshare/source/ctx.h"
}

namespace titles {

namespace {

// Asks the system for the "icon" file of a title (its SMDH, 0x36C0 bytes) in the ExeFS, as TitleShare's catalog does and as FBI does:
// no key is needed, but a 3dsx is often refused (see ncch_icon.hpp). False when the title has no readable icon this way.
bool ReadSmdhFromSystem(u64 tid, FS_MediaType media, u8* buffer) {
#ifdef AZL_TEST_SYSTEM_REFUSES
    (void)tid, (void)media, (void)buffer;
    return false; // (a build that behaves as the real consoles of TitleShare's logs do: the system refuses a 3dsx)
#endif
    u32 archive_path[4] = {static_cast<u32>(tid & 0xFFFFFFFFu), static_cast<u32>(tid >> 32), static_cast<u32>(media), 0};
    static const u32 file_path[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000}; // ExeFS, "icon"
    const FS_Path archive = {PATH_BINARY, sizeof(archive_path), archive_path};
    const FS_Path file = {PATH_BINARY, sizeof(file_path), file_path};
    Handle handle = 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&handle, ARCHIVE_SAVEDATA_AND_CONTENT, archive, file, FS_OPEN_READ, 0))) {
        return false;
    }
    u32 got = 0;
    const Result rc = FSFILE_Read(handle, &got, 0, buffer, setup::kSmdhSize);
    FSFILE_Close(handle);
    return R_SUCCEEDED(rc) && got == setup::kSmdhSize;
}

} // namespace

struct Scanner::Impl {
    std::atomic<int> state{0}; // 0 not started, 1 running, 2 done
    std::atomic<int> done{0};
    std::atomic<int> total{0};
    std::atomic<int> phase{0};    // setup::ScanProgress::phase
    std::atomic<int> titles_n{0}; // the titles the system lists (the names step counts up to it)
    std::atomic<int> apps_n{0};   // the applications (the games step counts up to it)
    std::atomic<bool> cancel{false}; // the app is closing (~Scanner): the worker stops at its next step instead of finishing the scan
    int language = 1;
    Thread thread = nullptr;
    // TitleShare's own state for the scan. Titles Delivery works on it too (dl_attach hands it the context and the catalog, and takes them back when
    // the scanner is destroyed).
    ts_ctx ctx;
    ts_catalog catalog;
    bool ctx_ready = false;
    bool catalog_ready = false;
    ScanReport report;
    std::vector<std::unique_ptr<Game>> games;
};

namespace {

Scanner::Impl* g_running = nullptr; // catalog_build's callback has no argument to carry this

// A build made only to look at the progress bar (make EXTRA_CFLAGS=-DAZL_TEST_SCAN_DELAY_MS=1500) waits a little at every step.
void SlowDownForLooking() {
#ifdef AZL_TEST_SCAN_DELAY_MS
    svcSleepThread(static_cast<s64>(AZL_TEST_SCAN_DELAY_MS) * 1000000LL);
#endif
}

// AM gives the version (and size) of every title in one call. One broken title (an installation that was interrupted, before the app of 2026-10-19
// cleaned up after itself) makes it refuse the whole list, and without versions the caches of the names and the games are of no use: then every
// title is asked alone, and only the ones AM cannot describe are left out (and named in `broken`). False when nothing could be asked.
bool ReadTitleInfos(const std::vector<u64>& ids, std::vector<AM_TitleEntry>& infos, std::vector<u64>& broken) {
    infos.assign(ids.size(), AM_TitleEntry{});
    if (ids.empty()) {
        return false;
    }
    std::vector<u64> copy = ids; // (AM wants a writable list)
    if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, static_cast<u32>(copy.size()), copy.data(), infos.data()))) {
        return true;
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        u64 tid = ids[i];
        if (R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &infos[i]))) {
            infos[i] = AM_TitleEntry{};
            broken.push_back(ids[i]);
        }
    }
    return true;
}

// The name of the game a title belongs to, as the delivery module's catalog groups them ("" when it has none).
std::string NameInCatalog(const Scanner::Impl* impl, u64 tid) {
    if (!impl->catalog_ready) {
        return std::string();
    }
    const ts_catalog& cat = impl->catalog;
    for (int g = 0; g < cat.nGames; ++g) {
        for (int k = 0; k < cat.games[g].nMembers; ++k) {
            if (cat.items[cat.games[g].members[k]].tid == tid) {
                return cat.games[g].name;
            }
        }
    }
    return std::string();
}

// The titles that are of no use (the Installation check's row): the installations the system lists as begun and never finished, the titles AM could
// not describe (`broken`), and the DLC whose ticket grants contents that are not on the card.
void ReadTitlesHealth(Scanner::Impl* impl, const std::vector<u64>& broken) {
    setup::TitlesHealth& health = impl->report.health;
    health.checked = true;
    health.titles = static_cast<int>(impl->ctx.titleCount);
    u32 pending = 0;
    if (R_SUCCEEDED(AM_GetNumPendingTitles(&pending, MEDIATYPE_SD)) && pending > 0 && pending < 1024) {
        std::vector<u64> ids(pending);
        u32 got = 0;
        if (R_SUCCEEDED(AM_GetPendingTitleList(&got, ids.data(), pending, MEDIATYPE_SD))) {
            for (u32 i = 0; i < got && i < pending; ++i) {
                setup::UnusableTitle t;
                t.kind = setup::UnusableTitle::Kind::Unfinished;
                t.tid = ids[i];
                t.name = NameInCatalog(impl, ids[i]);
                health.unusable.push_back(t);
            }
        }
    }
    for (const u64 tid : broken) {
        setup::UnusableTitle t;
        t.kind = setup::UnusableTitle::Kind::Broken;
        t.tid = tid;
        t.name = NameInCatalog(impl, tid);
        health.unusable.push_back(t);
    }
    for (u32 i = 0; i < impl->ctx.titleCount; ++i) {
        const u64 tid = impl->ctx.titleIds[i];
        if (static_cast<u32>(tid >> 32) != 0x0004008Cu || std::find(broken.begin(), broken.end(), tid) != broken.end()) {
            continue;
        }
        u32 n = 0;
        if (R_FAILED(AM_GetNumExistingContentInfos(&n, tid, MEDIATYPE_SD)) || n == 0 || n > 4096) {
            continue;
        }
        std::vector<AM_ContentInfo> infos(n);
        u32 got = 0;
        if (R_FAILED(AM_ListExistingContentInfos(&got, infos.data(), 0, n, tid, MEDIATYPE_SD))) {
            continue;
        }
        int installed = 0;
        int missing = 0;
        for (u32 k = 0; k < got && k < n; ++k) {
            const bool there = (infos[k].flags & AM_CONTENT_INSTALLED) != 0;
            const bool owned = (infos[k].flags & AM_CONTENT_OWNED) != 0;
            installed += there ? 1 : 0;
            missing += owned && !there ? 1 : 0;
        }
        if (missing > 0) {
            setup::UnusableTitle t;
            t.kind = setup::UnusableTitle::Kind::PartialDlc;
            t.tid = tid;
            t.name = NameInCatalog(impl, tid);
            t.installed = installed;
            t.missing = missing;
            health.unusable.push_back(t);
        }
    }
}

// catalog_build calls this as it goes; it reports (i, n) and ends with (n, n).
void OnProgress(int done, int total) {
    (void)total;
    if (g_running) {
        g_running->done.store(done);
    }
    SlowDownForLooking();
}

const setup::GameInfo* FindInCache(const std::vector<setup::GameInfo>& cache, u64 tid, u16 version) {
    for (const setup::GameInfo& entry : cache) {
        if (entry.tid == tid && entry.version == version) {
            return &entry;
        }
    }
    return nullptr;
}

// The name of a title, cut to what the delivery module's name field holds (47 bytes) on a character boundary.
void CopyName(char* out, std::size_t size, const std::string& name) {
    std::size_t n = std::min(name.size(), size - 1);
    while (n > 0 && n < name.size() && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) {
        --n; // (not in the middle of a character)
    }
    std::memcpy(out, name.data(), n);
    out[n] = '\0';
}

// The module's catalog read the SHORT title of every title; this app reads the long one (the one the lists show). Titles Delivery lists and
// sends the same names the rest of the app shows: the catalog takes them over (the games in it are sorted again by them).
void UseAppNames(Scanner::Impl* impl) {
    if (!impl->catalog_ready) {
        return;
    }
    std::map<u64, const std::string*> names;
    for (const std::unique_ptr<Game>& game : impl->games) {
        names[game->tid] = &game->name;
    }
    ts_catalog& cat = impl->catalog;
    for (int g = 0; g < cat.nGames; ++g) {
        ts_game& group = cat.games[g];
        for (int k = 0; k < group.nMembers; ++k) {
            const auto found = names.find(cat.items[group.members[k]].tid);
            if (found == names.end()) {
                continue;
            }
            CopyName(group.name, sizeof(group.name), *found->second);
            for (int m = 0; m < group.nMembers; ++m) {
                CopyName(cat.items[group.members[m]].name, sizeof(cat.items[0].name), *found->second);
            }
            break;
        }
    }
    std::sort(cat.games, cat.games + cat.nGames, [](const ts_game& a, const ts_game& b) { return strcasecmp(a.name, b.name) < 0; });
}

void RunScan(Scanner::Impl* impl) {
    // catalog_build saves its cache in the data folder (hostcfg.h) but does not make it: the app made it when it started, and this only covers
    // a folder that went missing since.
    setup::MakeDirs(setup::CurrentPaths().datadir);

    char message[128];
    ctx_init(&impl->ctx, message, sizeof(message)); // without keys it still lists the titles; the message only says why not more
    impl->ctx_ready = true;
    const bool have_keys = impl->ctx.haveKeys && impl->ctx.haveNcchKey;

    std::vector<u64> applications;
    for (u32 i = 0; i < impl->ctx.titleCount; ++i) {
        if (setup::IsApplicationTid(impl->ctx.titleIds[i])) {
            applications.push_back(impl->ctx.titleIds[i]);
        }
    }
    impl->report.applications = static_cast<int>(applications.size());
    impl->report.have_keys = have_keys;
    impl->done.store(0);
    impl->total.store(static_cast<int>(impl->ctx.titleCount) + static_cast<int>(applications.size()));
    impl->titles_n.store(static_cast<int>(impl->ctx.titleCount));
    impl->apps_n.store(static_cast<int>(applications.size()));
    impl->phase.store(0);

    // TitleShare's scan: the name of every title, from its cache or its icon, and the cache saved again when something changed.
    g_running = impl;
    impl->catalog_ready = catalog_build(&impl->ctx, &impl->catalog, OnProgress) == 0;
    g_running = nullptr;
    if (impl->cancel.load()) {
        return; // (the app is closing)
    }
    impl->phase.store(1);

    // The games: an application title with a readable name and icon (the SMDH), nothing else. The cache of the games has what an earlier
    // start read; a game is read again only when it is new or has a new version. The versions are asked for every title the system lists (the
    // titles' health below needs the ones AM cannot describe).
    const std::vector<u64> all_titles(impl->ctx.titleIds, impl->ctx.titleIds + impl->ctx.titleCount);
    std::vector<AM_TitleEntry> all_infos;
    std::vector<u64> broken;
    const bool have_versions = ReadTitleInfos(all_titles, all_infos, broken);
    std::map<u64, u16> versions;
    for (std::size_t i = 0; i < all_titles.size(); ++i) {
        versions[all_titles[i]] = static_cast<u16>(all_infos[i].version);
    }
    std::vector<setup::GameInfo> cached;
    if (have_versions) {
        setup::LoadGameCache(setup::CurrentPaths().games_cache.c_str(), impl->language, cached);
    }
    std::vector<setup::GameInfo> now; // the games as they are now: the next cache
    std::vector<u8> buffer(setup::kSmdhSize);
    setup::SdKeys keys;
    keys.sd_round_keys = impl->ctx.rk;
    keys.ncch_keyx = impl->ctx.keyX2C;
    keys.ctr_root = impl->ctx.ctrRoot;
    int system_works = -1; // -1 not tried yet, 0 the system refuses, 1 it answers
    int fresh = 0; // games read from their files this time (not taken from the cache)
    int done = static_cast<int>(impl->ctx.titleCount);
    for (std::size_t i = 0; i < applications.size(); ++i) {
        if (impl->cancel.load()) {
            return; // (the app is closing: the cache is not written this time, the next start reads what is missing)
        }
        const u64 tid = applications[i];
        if (std::find(broken.begin(), broken.end(), tid) != broken.end()) {
            impl->done.store(++done); // (a title the system cannot describe has no readable icon either: it is in the titles' health, not in the games)
            continue;
        }
        const u16 version = have_versions ? versions[tid] : 0;
        std::unique_ptr<Game> game(new Game);
        bool recognised = false;
        if (const setup::GameInfo* hit = have_versions ? FindInCache(cached, tid, version) : nullptr) {
            static_cast<setup::GameInfo&>(*game) = *hit;
            recognised = true;
        } else {
            if (system_works != 0) {
                const bool answered = ReadSmdhFromSystem(tid, MEDIATYPE_SD, buffer.data());
                if (answered) {
                    system_works = 1;
                } else if (system_works < 0 && have_keys) {
                    system_works = 0; // refused, and there is another way: stop asking (as TitleShare's catalog does)
                }
                if (answered && setup::ParseSmdh(buffer.data(), setup::kSmdhSize, impl->language, *game)) {
                    recognised = true;
                }
            }
            if (!recognised && have_keys && setup::ReadSmdhKeyed(keys, tid, buffer.data(), buffer.size()) == 0 &&
                setup::ParseSmdh(buffer.data(), setup::kSmdhSize, impl->language, *game)) {
                recognised = true;
            }
            fresh += recognised ? 1 : 0;
        }
        if (recognised) {
#ifdef AZL_TEST_LONG_NAME
            game->name += " - a subtitle that makes the whole name too long for one line of the list"; // (a build made to look at a long name)
#endif
            game->tid = tid;
            game->version = version;
            now.push_back(*game);
            impl->games.push_back(std::move(game));
        }
        impl->done.store(++done);
        SlowDownForLooking();
    }
    // The cache is written again only when it changed: a game read this time, or a game gone (a title that cannot be read at all is
    // asked again at every start, but costs no write).
    if (have_versions && (fresh > 0 || now.size() != cached.size())) {
        setup::SaveGameCache(setup::CurrentPaths().games_cache.c_str(), impl->language, now);
    }
    impl->report.recognised = static_cast<int>(impl->games.size());
    std::sort(impl->games.begin(), impl->games.end(), [](const std::unique_ptr<Game>& a, const std::unique_ptr<Game>& b) {
        if (a->name != b->name) {
            return setup::NameLess(a->name, b->name);
        }
        return a->tid < b->tid;
    });
    UseAppNames(impl);
    if (impl->cancel.load()) {
        return; // (the app is closing)
    }
    impl->phase.store(2);
    ReadTitlesHealth(impl, broken);
    impl->done.store(impl->total.load());
    impl->phase.store(3);
    impl->state.store(2);
}

void ThreadMain(void* arg) {
    RunScan(static_cast<Scanner::Impl*>(arg));
}

Scanner* g_scanner = nullptr;

} // namespace

Scanner::Scanner() : impl_(new Impl) {}

Scanner::~Scanner() {
    impl_->cancel.store(true); // (a worker that is still reading stops at its next step: the wait below is short)
    dl_attach(nullptr, nullptr); // (Titles Delivery holds this scan's context and catalog: it must not keep them when they are freed)
    if (!Finish(10000)) {
        (void)impl_.release(); // the worker is still running: what it uses is left to the end of the program, not freed under it
        return;
    }
    if (impl_->catalog_ready) {
        catalog_free(&impl_->catalog);
    }
    if (impl_->ctx_ready) {
        ctx_exit(&impl_->ctx);
    }
}

bool Scanner::CacheExists() {
    struct stat st;
    return stat(setup::CurrentPaths().catalog_cache.c_str(), &st) == 0 && !S_ISDIR(st.st_mode) && st.st_size > 0;
}

void Scanner::Start(int language) {
    if (impl_->state.load() != 0) {
        return;
    }
    impl_->language = language;
#ifdef AZL_TEST_SKIP_SCAN
    impl_->state.store(2); // (a build made to look at the screens on an emulator that crashes in TitleShare's key code when boot9.bin is there)
    return;
#endif
    impl_->state.store(1);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    // A little below the thread that draws, so the bar keeps moving.
    impl_->thread = threadCreate(ThreadMain, impl_.get(), 64 * 1024, priority + 1, -2, false);
    if (!impl_->thread) {
        RunScan(impl_.get()); // (no thread: the scan is done here, and the screen waits)
    }
}

bool Scanner::started() const {
    return impl_->state.load() != 0;
}

bool Scanner::done() const {
    return impl_->state.load() == 2;
}

int Scanner::progress_done() const {
    return impl_->done.load();
}

int Scanner::progress_total() const {
    return impl_->total.load();
}

setup::ScanProgress Scanner::progress() const {
    setup::ScanProgress p;
    p.phase = impl_->state.load() == 2 ? 3 : impl_->phase.load();
    p.titles = impl_->titles_n.load();
    const int done = impl_->done.load();
    if (p.phase == 0) {
        p.done = done;
        p.total = p.titles;
    } else if (p.phase == 1) {
        p.done = done - p.titles;
        p.total = impl_->apps_n.load();
    }
    if (p.phase == 3) {
        p.games = impl_->report.recognised;
    }
    return p;
}

const std::vector<std::unique_ptr<Game>>& Scanner::games() const {
    return impl_->games;
}

ScanReport Scanner::report() const {
    return impl_->report;
}

void* Scanner::module_context() const {
    return impl_->state.load() == 2 && impl_->ctx_ready ? static_cast<void*>(&impl_->ctx) : nullptr;
}

void* Scanner::module_catalog() const {
    return impl_->state.load() == 2 && impl_->catalog_ready ? static_cast<void*>(&impl_->catalog) : nullptr;
}

bool Scanner::Finish(u64 timeout_ms) {
    if (!impl_->thread) {
        return true;
    }
    // svcWaitSynchronization answers a timeout with 0x09401BFE, a code that is not negative: R_SUCCEEDED() takes it for a success. It did until
    // 2026-10-21: START, Exit while the scan was still running waited ten seconds, took the worker for finished, freed the state it was
    // still writing in, and the worker crashed in free() when it ended (three identical Luma dumps, `3dsx_app`, `_free_r`). Only 0 is success.
    if (threadJoin(impl_->thread, timeout_ms * 1000000ULL) == 0) {
        threadFree(impl_->thread);
        impl_->thread = nullptr;
        return true;
    }
    return false;
}

static int g_generation = 0;

Scanner& Scan() {
    if (!g_scanner) {
        g_scanner = new Scanner;
        ++g_generation;
    }
    return *g_scanner;
}

int ScanGeneration() {
    return g_generation;
}

void Shutdown() {
    delete g_scanner;
    g_scanner = nullptr;
}

bool ReadCardGame(int language, Game& out) {
    bool inserted = false;
    if (R_FAILED(FSUSER_CardSlotIsInserted(&inserted)) || !inserted) {
        return false;
    }
    FS_CardType type;
    if (R_FAILED(FSUSER_GetCardType(&type)) || type != CARD_CTR) {
        return false; // (a DS card has no 3DS game on it)
    }
    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_GAME_CARD, &count)) || count == 0 || count > 8) {
        return false;
    }
    u64 ids[8];
    u32 got = 0;
    if (R_FAILED(AM_GetTitleList(&got, MEDIATYPE_GAME_CARD, count, ids))) {
        return false;
    }
    std::vector<u8> buffer(setup::kSmdhSize);
    for (u32 i = 0; i < got && i < 8; ++i) {
        // (Only the system can read a card: the SD keys do not open it.)
        if (setup::IsApplicationTid(ids[i]) && ReadSmdhFromSystem(ids[i], MEDIATYPE_GAME_CARD, buffer.data()) &&
            setup::ParseSmdh(buffer.data(), setup::kSmdhSize, language, out)) {
            out.tid = ids[i];
            out.version = 0;
            out.card = true;
            return true;
        }
    }
    return false;
}

int DeleteUnusable(const std::vector<setup::UnusableTitle>& titles, int* failed) {
    int gone = 0;
    int refused = 0;
    for (const setup::UnusableTitle& t : titles) {
        Result rc = 0;
        switch (t.kind) {
        case setup::UnusableTitle::Kind::Unfinished:
            rc = AM_DeletePendingTitle(MEDIATYPE_SD, t.tid); // (the ticket stays: it may be the one of a title that works, which this never finished replacing)
            break;
        case setup::UnusableTitle::Kind::Broken:
            rc = AM_DeleteTitle(MEDIATYPE_SD, t.tid);
            AM_DeleteTicket(t.tid);
            AM_DeletePendingTitle(MEDIATYPE_SD, t.tid);
            break;
        case setup::UnusableTitle::Kind::PartialDlc:
            rc = AM_DeleteTitle(MEDIATYPE_SD, t.tid);
            AM_DeleteTicket(t.tid);
            break;
        }
        if (R_SUCCEEDED(rc)) {
            ++gone;
        } else {
            ++refused;
        }
    }
    if (failed) {
        *failed = refused;
    }
    return gone;
}

} // namespace titles
