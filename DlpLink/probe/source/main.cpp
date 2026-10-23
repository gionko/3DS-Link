// ChildProbe: THROWAWAY feasibility probe for the 3DS Link installer child (see DlpLink/docs/research/05-figlio-installatore.md).
// It is NOT the child. It answers, in an emulator or on a console, five questions:
//   1. does romfsInit() work in this (DLP-child-shaped, fixed-key-encrypted) title, and does a 2 MiB RomFS file read back byte-exact?
//   2. can this process open "am:net" and install a CIA that is embedded in its own RomFS (AM_StartCiaInstall(SD) ... AM_FinishCiaInstall)?
//   3. does citro2d + the system font work from such a title (the 3DS Link look)?
//   4. does the worker-thread + UI-thread split keep the UI alive while AM's first (long) write runs?
//   5. what happens on exit (return from main)?
// Everything it learns goes to sdmc:/3ds/ChildProbe/probe_log.txt.
#include <3ds.h>
#include <citro2d.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "gfx.hpp"
#include "ui_lite.hpp"

using namespace ui;

namespace {

FILE* g_log = nullptr;
u64 g_t0 = 0;

void L(const char* fmt, ...) {
    if (!g_log) return;
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(g_log, "%8llu ms  %s\n", (unsigned long long)(osGetTime() - g_t0), buf);
    fflush(g_log);
}

u32 g_crc_table[256];
void crc_init() {
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
}
u32 crc_update(u32 crc, const u8* p, size_t n) {
    crc = ~crc;
    while (n--) crc = g_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

enum Phase { PH_RUN = 0, PH_DONE = 1, PH_ERR = 2 };
enum Stage { ST_CHECK = 0, ST_PREPARE = 1, ST_WRITE = 2, ST_FINISH = 3, ST_PLUGIN = 4 };

struct Job {
    volatile int phase = PH_RUN;
    volatile int stage = ST_CHECK;
    volatile u64 done = 0, total = 0;
    volatile Result rc = 0;
    char msg[160] = "";
    u64 tid = 0;
    u16 ver = 0;
    volatile u64 sd_free = 0;
} g_job;

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u32 le32(const u8* p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
u64 al64(u64 x) { return (x + 63) & ~63ull; }

// TID and version of a CIA from its header + TMD (same offsets as FBI core/data/cia.c and DlpTest/tools/finalize_child.py).
bool parse_cia(FILE* f, u64* tid, u16* ver, u64* fsize) {
    u8 h[0x20];
    fseek(f, 0, SEEK_END);
    *fsize = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) return false;
    const u64 off = al64(le32(h)) + al64(le32(h + 8)) + al64(le32(h + 12));
    u8 t[0x400];
    fseek(f, (long)off, SEEK_SET);
    if (fread(t, 1, sizeof(t), f) != sizeof(t)) return false;
    const u32 sig = be32(t);
    u32 sz = 0;
    if (sig == 0x10000 || sig == 0x10003) sz = 0x200;
    else if (sig == 0x10001 || sig == 0x10004) sz = 0x100;
    else if (sig == 0x10002 || sig == 0x10005) sz = 0x3C;
    else return false;
    const u32 hb = 4 + sz + 0x3C;
    *tid = (u64)be32(t + hb + 0x4C) << 32 | be32(t + hb + 0x50);
    *ver = (u16)(t[hb + 0x9C] << 8 | t[hb + 0x9D]);
    return true;
}

void fail(Result rc, const char* what) {
    g_job.rc = rc;
    snprintf(g_job.msg, sizeof(g_job.msg), "%s", what);
    L("FAIL: %s -> 0x%08lX", what, (unsigned long)rc);
    g_job.phase = PH_ERR;
}

bool check_romfs_payload() {
    static u8 buf[256 * 1024];
    FILE* c = fopen("romfs:/payload.crc", "r");
    if (!c) { L("romfs:/payload.crc: fopen failed errno=%d", errno); return false; }
    unsigned want = 0; unsigned long wantlen = 0;
    if (fscanf(c, "%x %lu", &want, &wantlen) != 2) { fclose(c); return false; }
    fclose(c);
    FILE* f = fopen("romfs:/payload.bin", "rb");
    if (!f) { L("romfs:/payload.bin: fopen failed errno=%d", errno); return false; }
    u32 crc = 0; unsigned long total = 0; size_t n; int reads = 0;
    const u64 t0 = osGetTime();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) { crc = crc_update(crc, buf, n); total += n; ++reads; }
    fclose(f);
    L("RomFS payload.bin: %lu bytes in %d reads of 256 KiB, crc32 %08X (expected %08X, %lu bytes) in %llu ms -> %s", total, reads, crc, want, wantlen,
      (unsigned long long)(osGetTime() - t0), (crc == want && total == wantlen) ? "MATCH" : "MISMATCH");
    return crc == want && total == wantlen;
}

void probe_system(u64 tid) {
    u32 fw = osGetFirmVersion(), kv = osGetKernelVersion();
    s64 lv = 0;
    Result rl = svcGetSystemInfo(&lv, 0x10000, 0);
    u8 model = 0xFF, reg = 0xFF; bool n3 = false;
    CFGU_GetSystemModel(&model); CFGU_SecureInfoGetRegion(&reg); APT_CheckNew3DS(&n3);
    L("console: firmware %u.%u.%u kernel %u.%u.%u | Luma svcGetSystemInfo(0x10000) rc 0x%08lX = 0x%08llX | model %u (0 3DS,1 XL,2 N3DS,3 2DS,4 N3DSXL,5 N2DSXL) region %u new3ds %d",
      GET_VERSION_MAJOR(fw), GET_VERSION_MINOR(fw), GET_VERSION_REVISION(fw), GET_VERSION_MAJOR(kv), GET_VERSION_MINOR(kv), GET_VERSION_REVISION(kv),
      (unsigned long)rl, (unsigned long long)lv, model, reg, (int)n3);

    Handle ph = 0;
    Result rp = svcConnectToPort(&ph, "plg:ldr");
    L("svcConnectToPort(plg:ldr) -> 0x%08lX (Luma Plugin Loader reachable from this process: %s)", (unsigned long)rp, R_SUCCEEDED(rp) ? "yes" : "no");
    if (R_SUCCEEDED(rp)) svcCloseHandle(ph);

    FS_ArchiveResource nr; memset(&nr, 0, sizeof(nr));
    Result rn = FSUSER_GetArchiveResource(&nr, SYSTEM_MEDIATYPE_CTR_NAND);
    L("NAND free: %llu bytes (rc 0x%08lX)", (unsigned long long)nr.freeClusters * nr.clusterSize, (unsigned long)rn);

    AM_TitleEntry me; memset(&me, 0, sizeof(me));
    Result rm = AM_GetTitleInfo(MEDIATYPE_NAND, 1, &tid, &me);
    L("AM_GetTitleInfo(NAND, own TID) -> 0x%08lX version %u size %llu", (unsigned long)rm, me.version, (unsigned long long)me.size);
}

// Il figlio riesce a leggere dalla NAND il PROPRIO contenuto installato (NCCH cifrato a chiave zero, byte identici a quelli consegnati)?
// Se si', una console che ha ricevuto 3DS Link puo' ridistribuirlo senza ricomporre niente: copia il proprio .app e ci mette intorno la CIA.
void list_dir(FS_Archive a, const char* path, int maxn) {
    Handle d = 0;
    Result r = FSUSER_OpenDirectory(&d, a, fsMakePath(PATH_ASCII, path));
    if (R_FAILED(r)) { L("  opendir %s -> 0x%08lX", path, (unsigned long)r); return; }
    FS_DirectoryEntry e[16]; u32 n = 0; int total = 0; std::string names;
    while (R_SUCCEEDED(FSDIR_Read(d, &n, 16, e)) && n) {
        for (u32 i = 0; i < n; i++) {
            ++total;
            if (total <= maxn) { for (int k = 0; k < 24 && e[i].name[k]; k++) names += (char)e[i].name[k]; names += ' '; }
        }
    }
    FSDIR_Close(d);
    L("  opendir %s -> %d entries: %s%s", path, total, names.c_str(), total > maxn ? "..." : "");
}

void probe_nand(u64 tid) {
    FS_Archive a = 0;
    Result r = FSUSER_OpenArchive(&a, ARCHIVE_NAND_CTR_FS, fsMakePath(PATH_EMPTY, ""));
    L("FSUSER_OpenArchive(NAND CTR FS) -> 0x%08lX", (unsigned long)r);
    if (R_FAILED(r)) return;
    list_dir(a, "/", 20);
    list_dir(a, "/title", 20);
    list_dir(a, "/title/00040001", 24);
    char dir[96];
    snprintf(dir, sizeof(dir), "/title/%08lx/%08lx", (unsigned long)(tid >> 32), (unsigned long)tid);
    list_dir(a, dir, 20);
    char cdir[112]; snprintf(cdir, sizeof(cdir), "%s/content", dir);
    list_dir(a, cdir, 20);
    char app[128]; snprintf(app, sizeof(app), "%s/00000000.app", cdir);
    Handle fh = 0;
    r = FSUSER_OpenFile(&fh, a, fsMakePath(PATH_ASCII, app), FS_OPEN_READ, 0);
    L("open own content %s -> 0x%08lX", app, (unsigned long)r);
    if (R_SUCCEEDED(r)) {
        u64 sz = 0; FSFILE_GetSize(fh, &sz);
        u8 hdr[0x200]; u32 rd = 0; FSFILE_Read(fh, &rd, 0, hdr, sizeof(hdr));
        L("  size %llu, read %lu, NCCH magic '%.4s' @0x100, partitionId %016llX, flags[3]=%02X flags[7]=%02X", (unsigned long long)sz, (unsigned long)rd,
          (const char*)hdr + 0x100, (unsigned long long)*(u64*)(hdr + 0x108), hdr[0x188 + 3], hdr[0x188 + 7]);
        static u8 buf[256 * 1024];
        FILE* o = fopen("sdmc:/3ds/ChildProbe/self.app", "wb");
        u32 crc = 0; u64 off = 0; const u64 t0 = osGetTime();
        while (off < sz) {
            u32 want = (sz - off) < sizeof(buf) ? (u32)(sz - off) : (u32)sizeof(buf);
            if (R_FAILED(FSFILE_Read(fh, &rd, off, buf, want)) || rd == 0) break;
            crc = crc_update(crc, buf, rd);
            if (o) fwrite(buf, 1, rd, o);
            off += rd;
        }
        if (o) fclose(o);
        L("  copied own NCCH to sdmc:/3ds/ChildProbe/self.app: %llu of %llu bytes, crc32 %08X, %llu ms", (unsigned long long)off, (unsigned long long)sz, crc, (unsigned long long)(osGetTime() - t0));
        FSFILE_Close(fh);
    }
    FSUSER_CloseArchive(a);
}

void worker(void*) {
    L("worker started on core %d", (int)svcGetProcessorID());
    g_job.stage = ST_CHECK;
    if (!check_romfs_payload()) { fail(0xC8604C10, "RomFS payload is damaged"); return; }

    FILE* f = fopen("romfs:/inner.cia", "rb");
    if (!f) { fail(0xC8604C11, "romfs:/inner.cia missing"); return; }
    u64 tid = 0, size = 0; u16 ver = 0;
    if (!parse_cia(f, &tid, &ver, &size)) { fclose(f); fail(0xC8604C12, "inner.cia is not a CIA"); return; }
    g_job.tid = tid; g_job.ver = ver; g_job.total = size;
    L("inner.cia: %llu bytes, TID %016llX, version %u (0x%04X)", (unsigned long long)size, (unsigned long long)tid, ver, ver);

    FS_ArchiveResource ar; memset(&ar, 0, sizeof(ar));
    Result r = FSUSER_GetSdmcArchiveResource(&ar);
    const u64 freeb = (u64)ar.freeClusters * ar.clusterSize;
    g_job.sd_free = freeb;
    L("SD free: %llu bytes (rc 0x%08lX, cluster %lu)", (unsigned long long)freeb, (unsigned long)r, (unsigned long)ar.clusterSize);
    if (R_SUCCEEDED(r) && freeb < size + (16ull << 20)) { fclose(f); fail(0xC86044CD, "not enough space on the SD card"); return; }

    AM_TitleEntry cur; memset(&cur, 0, sizeof(cur));
    r = AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &cur);
    L("AM_GetTitleInfo(SD, %016llX) -> 0x%08lX (installed version %u, size %llu)", (unsigned long long)tid, (unsigned long)r, cur.version, (unsigned long long)cur.size);

    g_job.stage = ST_PREPARE;
    const bool slow = access("sdmc:/3ds/ChildProbe/slow.txt", F_OK) == 0;   // test hook: pretend AM is slow (the real AM's first write takes ~2 s)
    if (slow) svcSleepThread(2500ULL * 1000000ULL);
    if (access("sdmc:/3ds/ChildProbe/fail.txt", F_OK) == 0) { fclose(f); fail(0xC86044CD, "Not enough space on the SD card"); return; } // test hook
    Result rt = AM_DeleteTitle(MEDIATYPE_SD, tid);
    Result rk = AM_DeleteTicket(tid);
    bool avail = false;
    Result rd = AM_QueryAvailableExternalTitleDatabase(&avail);
    Result rp = AM_DeletePendingTitle(MEDIATYPE_SD, tid);
    L("prep: DeleteTitle 0x%08lX DeleteTicket 0x%08lX QueryExtDb 0x%08lX (avail %d) DeletePending 0x%08lX", (unsigned long)rt, (unsigned long)rk, (unsigned long)rd, avail, (unsigned long)rp);

    Handle h = 0;
    const u64 a0 = osGetTime();
    r = AM_StartCiaInstall(MEDIATYPE_SD, &h);
    L("AM_StartCiaInstall(SD) -> 0x%08lX handle 0x%lX in %llu ms", (unsigned long)r, (unsigned long)h, (unsigned long long)(osGetTime() - a0));
    if (R_FAILED(r)) { fclose(f); fail(r, "AM_StartCiaInstall failed"); return; }

    static u8 buf[256 * 1024];
    fseek(f, 0, SEEK_SET);
    u64 off = 0; bool first = true;
    while (off < size) {
        size_t want = (size - off) < sizeof(buf) ? (size_t)(size - off) : sizeof(buf);
        size_t n = fread(buf, 1, want, f);
        if (n == 0) { AM_CancelCIAInstall(h); fclose(f); fail(0xC8604C13, "RomFS read ended early"); return; }
        const u64 w0 = osGetTime();
        u32 written = 0;
        r = FSFILE_Write(h, &written, off, buf, (u32)n, 0);
        const u64 dt = osGetTime() - w0;
        if (first) { L("first AM write: %zu bytes, rc 0x%08lX, %llu ms", n, (unsigned long)r, (unsigned long long)dt); first = false; }
        if (R_FAILED(r) || written != n) {
            AM_CancelCIAInstall(h); fclose(f);
            L("write failed at offset %llu: rc 0x%08lX written %lu of %zu", (unsigned long long)off, (unsigned long)r, (unsigned long)written, n);
            fail(R_FAILED(r) ? r : 0xC8604C14, "AM write failed");
            return;
        }
        off += n;
        g_job.done = off;
        g_job.stage = ST_WRITE;
        if (slow) svcSleepThread(700ULL * 1000000ULL);
    }
    fclose(f);
    g_job.stage = ST_FINISH;
    const u64 f0 = osGetTime();
    r = AM_FinishCiaInstall(h);
    L("AM_FinishCiaInstall -> 0x%08lX in %llu ms", (unsigned long)r, (unsigned long long)(osGetTime() - f0));
    if (R_FAILED(r)) { AM_CancelCIAInstall(h); fail(r, "AM_FinishCiaInstall failed"); return; }

    AM_TitleEntry after; memset(&after, 0, sizeof(after));
    r = AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &after);
    L("after install: AM_GetTitleInfo -> 0x%08lX version %u size %llu", (unsigned long)r, after.version, (unsigned long long)after.size);

    // the "plugin": copy payload.bin to the SD through a temp file + rename, as the child would do with AzaharLink.3gx
    g_job.stage = ST_PLUGIN;
    FILE* in = fopen("romfs:/payload.bin", "rb");
    FILE* out = fopen("sdmc:/3ds/ChildProbe/plugin_stand_in.bin.tmp", "wb");
    bool ok = in && out;
    if (ok) { size_t n; while ((n = fread(buf, 1, sizeof(buf), in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = false; break; } }
    if (in) fclose(in);
    if (out) fclose(out);
    if (ok) { remove("sdmc:/3ds/ChildProbe/plugin_stand_in.bin"); ok = rename("sdmc:/3ds/ChildProbe/plugin_stand_in.bin.tmp", "sdmc:/3ds/ChildProbe/plugin_stand_in.bin") == 0; }
    L("plugin copy via temp+rename: %s (errno %d)", ok ? "OK" : "FAILED", errno);
    {   // il figlio vero salvera' anche il CIA di 3DS Link su SD (per poterlo ridistribuire): tempo e riuscita
        FILE* i2 = fopen("romfs:/inner.cia", "rb");
        FILE* o2 = fopen("sdmc:/3ds/ChildProbe/inner_copy.cia", "wb");
        u64 tot = 0; const u64 c0 = osGetTime(); bool ok2 = i2 && o2;
        if (ok2) { size_t n; while ((n = fread(buf, 1, sizeof(buf), i2)) > 0) { if (fwrite(buf, 1, n, o2) != n) { ok2 = false; break; } tot += n; } }
        if (i2) fclose(i2);
        if (o2) fclose(o2);
        L("save of inner.cia on the SD: %s, %llu bytes in %llu ms", ok2 ? "OK" : "FAILED", (unsigned long long)tot, (unsigned long long)(osGetTime() - c0));
    }

    snprintf(g_job.msg, sizeof(g_job.msg), "installed");
    g_job.phase = PH_DONE;
    L("worker done");
}

void draw() {
    const int phase = g_job.phase;
    const int stage = g_job.stage;
    const bool running = phase == PH_RUN;
    const float frac = g_job.total ? (float)g_job.done / (float)g_job.total : 0.0f;
    const int pct = phase == PH_DONE ? 100 : (int)(frac * 100.0f);

    gfx::BeginTop();
    DrawTitleBar(gfx::kTopW, "3DS Link", "Installer", false);
    const u32 stripe = phase == PH_ERR ? col::kOff : (phase == PH_DONE ? col::kOn : col::kAccent);
    DrawCard(10, 32, 380, 126, stripe);
    DrawDotText(24, 50, col::kOn, col::kText, "Download complete", 0.5f, 340);
    if (phase == PH_ERR) {
        DrawDotText(24, 74, col::kOff, col::kText, "Installation failed", 0.5f, 340);
    } else if (phase == PH_DONE) {
        DrawDotText(24, 74, col::kOn, col::kText, "Installation complete", 0.5f, 340);
    } else {
        DrawDotText(24, 74, col::kWarn, col::kText, "Installing 3DS Link...", 0.5f, 340);
    }
    DrawProgressBar(24, 92, 300, 14, phase == PH_DONE ? 1.0f : frac);
    gfx::DrawText(376, 99, 0.62f, col::kText, Fmt("%d%%", pct), gfx::Align::Right);
    const char* st = stage == ST_CHECK ? "Checking the package" : stage == ST_PREPARE ? "Preparing the SD card" : stage == ST_WRITE ? "Writing" : stage == ST_FINISH ? "Finishing" : "Copying the plugin";
    if (running) {
        gfx::DrawText(24, 124, 0.42f, col::kDim, st);
        const int step = (int)((osGetTime() / 250) % 4);
        for (int i = 0; i < 4; ++i) gfx::FillCircle(340.0f + 10.0f * i, 124, 3.0f, i == step ? col::kAccent : col::kTrack);
        gfx::DrawText(24, 142, 0.38f, col::kDim, "Do not turn the console off.");
    } else if (phase == PH_DONE) {
        gfx::DrawText(24, 124, 0.42f, col::kOn, "3DS Link is ready on the HOME menu.");
    } else {
        gfx::DrawText(24, 124, 0.42f, col::kOff, g_job.msg);
        gfx::DrawText(24, 142, 0.4f, col::kText, Fmt("Error code 0x%08lX", (unsigned long)g_job.rc));
    }
    DrawCard(10, 166, 380, 64, col::kLine);
    gfx::DrawText(24, 182, 0.4f, col::kDim, Fmt("Package   title %016llX  v%u", (unsigned long long)g_job.tid, g_job.ver));
    gfx::DrawText(24, 198, 0.4f, col::kDim, Fmt("SD card   %.1f MiB free", (double)g_job.sd_free / 1048576.0));
    gfx::DrawText(24, 214, 0.4f, col::kDim, Fmt("Size   %.2f MiB", (double)g_job.total / 1048576.0));
    DrawFrameBorder(gfx::kTopW);

    gfx::BeginBottom();
    DrawTitleBar(gfx::kBottomW, "Installation", "", running);
    const char* body = running ? "3DS Link is being installed on this console. HOME and sleep are off until it is done."
                       : phase == PH_DONE ? "Done. Press any button: the console goes back to the HOME menu, where the new 3DS Link is waiting."
                                          : "Something went wrong. Nothing was changed on the console except what is written above. Press any button to leave.";
    gfx::DrawWrapped(16, 40, 288, 0.46f, col::kText, body, 6);
    DrawFooterBackground();
    if (phase == PH_ERR) {
        DrawButton(8, 216, 150, 20, "B", "Leave", true);
        DrawButton(162, 216, 150, 20, "A", "Details", true);
    } else {
        DrawButton(8, 216, 304, 20, "A", phase == PH_DONE ? "Go to HOME" : "Please wait...", phase == PH_DONE);
    }
    DrawFrameBorder(gfx::kBottomW);
}

} // namespace

int main(int, char**) {
    g_t0 = osGetTime();
    gfxInitDefault();
    crc_init();
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ChildProbe", 0777);
    g_log = fopen("sdmc:/3ds/ChildProbe/probe_log.txt", "a");   // append: one file collects every delivery
    { time_t tt = time(nullptr); struct tm* lt = localtime(&tt); if (g_log && lt) fprintf(g_log, "\n===== ChildProbe run %04d-%02d-%02d %02d:%02d:%02d =====\n", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec); }
    L("ChildProbe start");
    u64 mytid = 0;
    Result r = APT_GetProgramID(&mytid);
    L("APT_GetProgramID -> 0x%08lX %016llX", (unsigned long)r, (unsigned long long)mytid);
    L("envIsHomebrew = %d", (int)envIsHomebrew());

    Result rcfg = cfguInit();
    L("cfguInit -> 0x%08lX", (unsigned long)rcfg);
    Result rrom = romfsInit();
    L("romfsInit -> 0x%08lX", (unsigned long)rrom);
    Result ram = amInit();
    L("amInit -> 0x%08lX", (unsigned long)ram);
    {   // APT:GetWirelessRebootInfo (cmd 0x0045, 3dbrew): 1 normal param = size, the data comes back in static buffer 0. libctru has no wrapper, so it goes through aptSendCommand.
        u8 wr[0x10];
        memset(wr, 0xAA, sizeof(wr));
        u32* sb = getThreadStaticBuffers();
        sb[0] = IPC_Desc_StaticBuffer(sizeof(wr), 0);
        sb[1] = (u32)wr;
        u32 cb[16];
        memset(cb, 0, sizeof(cb));
        cb[0] = IPC_MakeHeader(0x45, 1, 0);
        cb[1] = sizeof(wr);
        Result rw = aptSendCommand(cb);
        L("APT GetWirelessRebootInfo(0x10) -> send 0x%08lX, reply result 0x%08lX; bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
          (unsigned long)rw, (unsigned long)cb[1], wr[0], wr[1], wr[2], wr[3], wr[4], wr[5], wr[6], wr[7], wr[8], wr[9], wr[10], wr[11], wr[12], wr[13], wr[14], wr[15]);
    }
    probe_system(mytid);
    probe_nand(mytid);
    const bool gok = gfx::Init();
    L("gfx::Init (citro3d+citro2d+system font) -> %d %s", (int)gok, gok ? "" : gfx::InitError());
    L("memory: APPLICATION region size %lu free %lu | linear free %lu | heap size %lu | system mode (svcGetSystemInfo) see APPMEMALLOC",
      (unsigned long)osGetMemRegionSize(MEMREGION_APPLICATION), (unsigned long)osGetMemRegionFree(MEMREGION_APPLICATION), (unsigned long)linearSpaceFree(), (unsigned long)envGetHeapSize());

    aptSetHomeAllowed(false);
    aptSetSleepAllowed(false);

    bool auto_exit = false;
    if (FILE* a = fopen("sdmc:/3ds/ChildProbe/auto.txt", "r")) { auto_exit = true; fclose(a); }

    Thread th = nullptr;
    if (R_FAILED(rrom) || R_FAILED(ram)) {
        fail(R_FAILED(rrom) ? rrom : ram, R_FAILED(rrom) ? "romfsInit failed" : "amInit failed");
    } else {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        th = threadCreate(worker, nullptr, 64 * 1024, prio < 0x3F ? prio + 1 : prio, -2, false);
        L("threadCreate -> %p", (void*)th);
    }

    u64 done_at = 0;
    u64 frames = 0, slow = 0, last = osGetTime();
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (g_job.phase != PH_RUN) {
            if (!done_at) { done_at = osGetTime(); L("UI sees phase %d after %llu frames (%llu frames slower than 100 ms)", (int)g_job.phase, (unsigned long long)frames, (unsigned long long)slow); }
            if (down || (auto_exit && osGetTime() - done_at > 15000)) break;
        }
        if (gok) {
            if (gfx::BeginFrame()) { draw(); gfx::EndFrame(); } else gspWaitForVBlank();
        } else {
            gspWaitForVBlank();
        }
        const u64 now = osGetTime();
        if (now - last > 100) ++slow;
        last = now; ++frames;
    }
    if (th) { threadJoin(th, U64_MAX); threadFree(th); }
    L("leaving: frames %llu, slow frames %llu", (unsigned long long)frames, (unsigned long long)slow);
    aptSetHomeAllowed(true);
    aptSetSleepAllowed(true);
    if (gok) gfx::Shutdown();
    if (R_SUCCEEDED(ram)) amExit();
    if (R_SUCCEEDED(rrom)) romfsExit();
    if (R_SUCCEEDED(rcfg)) cfguExit();
    L("calling gfxExit and returning from main");
    if (g_log) fclose(g_log);
    gfxExit();
    return 0;
}
