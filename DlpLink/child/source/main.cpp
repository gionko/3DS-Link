// 3DS Link Installer ("Powered by Gionko"): the child that Download Play hands to another console. Started by the system after the download
// (a temporary NAND title), it installs 3DS Link on the console it runs on:
//   romfs:/3dslink.cia      the 3DS Link app (a NoCrypto CIA, installed to the SD card with AM; and left as it is at sdmc:/3ds/3DSLink/dlp/3dslink.cia)
//   romfs:/AzaharLink.3gx   the plugin, written to sdmc:/3ds/3DSLink/AzaharLink.3gx (where the app looks for it)
//   romfs:/manifest.txt     version and sizes of the two (tools/make_dlp_child.sh)
// What it does, in order: reads and checks the package, looks at what is installed (policy in installer_logic.hpp: install, update, leave alone),
// checks the SD card, writes the plugin (temp file + rename), installs the CIA (AM_StartCiaInstall ... AM_FinishCiaInstall), checks that AM now
// has the title with the right version, leaves a copy of the CIA for the app (the app composes the Download Play child from it on a console that has only
// Luma: DlpLink/docs/COMPOSITORE.md; same version kept: the copy too; a copy that cannot be written is logged, never a failure), and leaves with a return from
// main (the console goes back to HOME, where 3DS Link waits wrapped like a gift).
// When the job went well and the SD card has no boot9.bin (sharing needs it, using 3DS Link does not), the finished screens say so and a key (Y) opens the
// guide "How to get boot9.bin" (AzaharLink/ConfigApp/guide, the same module the app uses): a notice, never a wall; with boot9.bin there the installer is silent.
// What it never does: write 3DSLink.cfg (the app's first start is the user's first look at it), touch /luma/plugins, use the network.
// HOME and sleep are off for the whole job (a press of HOME during an AM write would suspend the app mid-install). The screens run in this
// thread, the work in another: AM's first write takes about 2 s on hardware and the screens must not stand still.
// Everything it learns goes to sdmc:/3ds/3DSLink/installer_log.txt (small, appended, cut at 64 KB): the way to know what happened on a console
// that is not ours.
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
#include "guide.hpp"
#include "installer_logic.hpp"
#include "safe_write.hpp"
#include "ui_lite.hpp"

using namespace ui;

// installer_logic.hpp names the buttons with the numbers of libctru's: they must be the same ones.
static_assert(inst::kPadA == KEY_A && inst::kPadB == KEY_B && inst::kPadSelect == KEY_SELECT && inst::kPadStart == KEY_START && inst::kPadRight == KEY_DRIGHT &&
                  inst::kPadLeft == KEY_DLEFT && inst::kPadUp == KEY_DUP && inst::kPadDown == KEY_DDOWN && inst::kPadR == KEY_R && inst::kPadL == KEY_L &&
                  inst::kPadX == KEY_X && inst::kPadY == KEY_Y,
              "installer_logic.hpp's button bits differ from libctru's");

namespace {

using inst::OUT_NONE, inst::OUT_INSTALLED, inst::OUT_UPDATED, inst::OUT_CURRENT, inst::OUT_NEWER, inst::OUT_LEFT, inst::OUT_DOWNGRADED, inst::OUT_REPAIRED;

constexpr const char* kDataDir = "sdmc:/3ds/3DSLink";
constexpr const char* kPluginPath = "sdmc:/3ds/3DSLink/AzaharLink.3gx";
constexpr const char* kPluginTmp = "sdmc:/3ds/3DSLink/AzaharLink.3gx.part";
constexpr const char* kPluginBak = "sdmc:/3ds/3DSLink/AzaharLink.3gx.old"; // the plugin that was there, for the few moments of the swap
constexpr const char* kDlpDir = "sdmc:/3ds/3DSLink/dlp";
constexpr const char* kCopyPath = "sdmc:/3ds/3DSLink/dlp/3dslink.cia";      // the app's CIA, left for the app (COMPOSITORE.md, source (a))
constexpr const char* kCopyTmp = "sdmc:/3ds/3DSLink/dlp/3dslink.cia.part";
constexpr const char* kCopyBak = "sdmc:/3ds/3DSLink/dlp/3dslink.cia.old";   // the copy that was there, for the few moments of the swap
constexpr const char* kLogPath = "sdmc:/3ds/3DSLink/installer_log.txt";
constexpr size_t kBlock = inst::kBlock;
constexpr u64 kLogMaxBytes = 64 * 1024;

// ---------------------------------------------------------------------------------------------
// The log
// ---------------------------------------------------------------------------------------------
FILE* g_log = nullptr;
u64 g_t0 = 0;

void L(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void L(const char* fmt, ...) {
    if (!g_log) {
        return;
    }
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(g_log, "%8llu ms  %s\n", static_cast<unsigned long long>(osGetTime() - g_t0), buf);
    fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// The job: shared between the worker and the screens. Plain volatile words (one writer each, a screen that reads a stale value draws it once).
// ---------------------------------------------------------------------------------------------
enum Phase { PH_RUN = 0, PH_DONE = 1, PH_ERR = 2, PH_ASK = 3 }; // PH_ASK: the worker waits for the user (the screens show the question)
enum Stage { ST_CHECK = 0, ST_PLUGIN, ST_PREPARE, ST_WRITE, ST_FINISH, ST_VERIFY, ST_COPY };
enum AskKind { AK_NONE = 0, AK_UPDATE, AK_DOWNGRADE, AK_REPAIR };

struct Job {
    volatile int phase = PH_RUN;
    volatile int stage = ST_CHECK;
    volatile int outcome = OUT_NONE;
    volatile int action = -1;            // inst::Action once known, -1 before
    volatile u32 done_kb = 0, total_kb = 0; // KiB of work (plugin + CIA + the copy of the CIA when it is written) done and to do: 32 bits, one word on the ARM11
    volatile Result rc = 0;
    volatile bool cancel = false;        // the system is closing the app
    char msg[160] = "";
    volatile bool has_package = false;   // the package was read: package_version and package_kb mean something
    volatile u32 package_version = 0;
    volatile int installed_version = -1; // the version AM has now (-1 = not installed)
    volatile int console_known = 0;      // 0 not asked yet, 1 AM answered, 2 AM could not say (then nothing is deleted)
    volatile bool sd_known = false;      // sd_free_kb is the card's real free space
    volatile int ask_kind = AK_NONE;     // what the decision screen asks (PH_ASK)
    volatile int decision = 0;           // the user's answer to it: 0 not yet, 1 go ahead, 2 leave it as it is
    volatile bool downgrade = false;     // the user chose to replace a newer 3DS Link by this older package
    volatile bool touched = false;       // an installed 3DS Link has been taken off (in this run, a retry included): if the job fails now, there is none
    volatile u32 sd_free_kb = 0;
    volatile u32 package_kb = 0;
};
Job g_job;
bool g_can_retry = false; // the work could start (RomFS and AM are there): "Try again" means something
bool g_keys_open = false; // the screens' keys count (the first moments after a screen appears they do not: a key pressed for something else must not answer it)
// The boot9.bin guide (the screens' thread only): looked up once per finished screen, opened with Y while the notice is up.
guide::Viewer g_viewer;
bool g_guide_open = false;     // the guide is the screen (its keys count at once; the result screen's lockout starts again when it closes)
bool g_guide_missing = false;  // Open() said no (the pictures are not in this build, or no memory): the notice says where to find the guide instead, no key
bool g_boot9_checked = false;  // g_boot9_found was read for this finished screen
bool g_boot9_found = true;     // true until looked up: no notice before the answer is known
u64 g_work_done = 0; // bytes of work done: the worker's own count, published to the screens as KiB
void Advance(u64 bytes) {
    g_work_done += bytes;
    g_job.done_kb = static_cast<u32>(g_work_done >> 10);
}

void Reset() {
    g_job.phase = PH_RUN;
    g_job.stage = ST_CHECK;
    g_job.outcome = OUT_NONE;
    g_job.action = -1;
    g_work_done = 0;
    g_job.done_kb = g_job.total_kb = 0;
    g_job.has_package = false;
    g_job.rc = 0;
    g_job.msg[0] = 0;
    g_job.installed_version = -1;
    g_job.console_known = 0;
    g_job.sd_known = false;
    g_job.ask_kind = AK_NONE;
    g_job.decision = 0;
    g_job.downgrade = false;
    // (g_job.touched is NOT reset: "the old 3DS Link was taken off" stays true across a "Try again", because the second attempt finds nothing installed)
}

void Fail(Result rc, const char* what) {
    g_job.rc = rc;
    snprintf(g_job.msg, sizeof(g_job.msg), "%s", inst::ErrorText(static_cast<inst::u32>(rc)).c_str());
    L("FAIL: %s -> 0x%08lX (%s)", what, static_cast<unsigned long>(rc), g_job.msg);
    g_job.phase = PH_ERR;
}

// The same sequence as the title delivery receiver and FBI: what a failed or interrupted install leaves behind is taken away. When AM could not say what
// was installed (g_scrub_known false) the title and its ticket are left alone: only the pending import goes.
bool g_scrub_known = true;
void Scrub(u64 tid) {
    if (g_scrub_known) {
        AM_DeleteTitle(MEDIATYPE_SD, tid);
        AM_DeleteTicket(tid);
    }
    AM_DeletePendingTitle(MEDIATYPE_SD, tid);
}

// Waits for the user. The screens show the question while the phase is PH_ASK and put the answer in `decision` (1 go ahead, 2 leave it as it is); the phase
// is set LAST, after the question is complete. False when the user leaves it, or when the system is closing the app.
bool Ask(int kind) {
    g_job.decision = 0;
    g_job.ask_kind = kind;
    L("asking the user: %s", kind == AK_UPDATE ? "update" : (kind == AK_DOWNGRADE ? "install the older version" : "restore the plugin"));
    g_job.phase = PH_ASK;
    while (g_job.decision == 0 && !g_job.cancel) {
        svcSleepThread(20ULL * 1000000ULL);
    }
    const bool go = g_job.decision == 1 && !g_job.cancel;
    L("the user said: %s", go ? "go ahead" : "leave it as it is");
    g_job.phase = PH_RUN;
    return go;
}

static u8 g_buf[kBlock]; // .bss: the stack is 0x40000 and the worker's 64 KiB

// ---------------------------------------------------------------------------------------------
// Pieces of the work
// ---------------------------------------------------------------------------------------------
using inst::FileSize;

bool ReadManifest(inst::Manifest* out) {
    FILE* f = fopen("romfs:/manifest.txt", "rb");
    if (!f) {
        L("romfs:/manifest.txt: fopen failed, errno %d", errno);
        return false;
    }
    char text[2048];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    *out = inst::ParseManifest(text);
    return out->complete();
}

// The file code (inst:: in safe_write.hpp, tested on the PC) asks the job record for three things: the log, the progress and "the system is closing".
struct JobHooks : inst::Hooks {
    void Log(const char* line) override { L("%s", line); }
    void Advance(inst::u64 bytes) override { ::Advance(bytes); }
    bool Cancelled() override { return g_job.cancel; }
};
JobHooks g_hooks;

// Is `path` the same bytes as the (open) file of the RomFS (`size` bytes: the plugin, the app's CIA)? Both are read from the start.
bool SameFileAs(FILE* romfs_file, const char* path, u64 size) {
    return inst::SameFileAs(romfs_file, path, size, g_buf, sizeof(g_buf));
}

// The two files the installer puts on the SD card whole: the plugin and the copy of the app's CIA go the same way (.part, rename: safe_write.hpp).
constexpr inst::Target kPluginTarget = {"plugin", kPluginPath, kPluginTmp, kPluginBak, false};
constexpr inst::Target kCopyTarget = {"app copy", kCopyPath, kCopyTmp, kCopyBak, true};

bool WriteWhole(FILE* in, u64 size, const inst::Target& t) {
    return inst::WriteWhole(in, size, t, g_buf, sizeof(g_buf), g_hooks);
}

// The copy of the app's CIA for the app (sdmc:/3ds/3DSLink/dlp/3dslink.cia: the app composes the Download Play child from it on a console that has only Luma):
// the CIA of the RomFS, written whole and read back against the RomFS from its final place. False on any failure, logged with its errno.
bool SaveAppCopy(u64 size) {
    return inst::SaveCopy("romfs:/3dslink.cia", kDlpDir, size, kCopyTarget, g_buf, sizeof(g_buf), g_hooks);
}

// What the job does about the copy once the app is on the console (installed, updated, replaced, or the same version kept). Never a failure of the job: the app is
// there; the copy is a courtesy to the app, which can still build the child from the installed title or tell the user what to do.
void LeaveAppCopy(u64 size, bool copy_same) {
    if (copy_same) {
        L("app copy: %s is already this one, not written", kCopyPath);
        return;
    }
    g_job.stage = ST_COPY;
    if (SaveAppCopy(size)) {
        L("app copy: %s written and read back, %llu bytes", kCopyPath, static_cast<unsigned long long>(size));
    } else {
        L("app copy: NOT saved at %s (the app is installed; the job goes on)", kCopyPath);
    }
}

// The whole job. Runs on its own thread; ends by setting the phase to PH_DONE or PH_ERR.
void Worker(void*) {
    L("worker started on core %d", static_cast<int>(svcGetProcessorID()));
    g_job.stage = ST_CHECK;

    // ---- the package
    inst::Manifest man;
    if (!ReadManifest(&man)) {
        Fail(inst::kErrPayload, "manifest.txt is missing or incomplete");
        return;
    }
    L("manifest: version %s (%u), title %016llX, 3dslink.cia %llu bytes, plugin %llu bytes", man.version.c_str(), static_cast<unsigned>(man.version16),
      static_cast<unsigned long long>(man.tid), static_cast<unsigned long long>(man.cia_size), static_cast<unsigned long long>(man.plugin_size));
    // This installer installs 3DS Link and nothing else: the job deletes the title it names (and its ticket) before it installs, so a manifest that names another
    // title is refused before anything is read or touched.
    if (!inst::NamesApp(man)) {
        L("the manifest names title %016llX: this installer installs only %016llX (3DS Link) and refuses any other", static_cast<unsigned long long>(man.tid),
          static_cast<unsigned long long>(inst::kAppTid));
        Fail(inst::kErrNotThis, "the manifest names a title that is not 3DS Link's");
        return;
    }

    FILE* cia = fopen("romfs:/3dslink.cia", "rb");
    if (!cia) {
        L("romfs:/3dslink.cia: fopen failed, errno %d", errno);
        Fail(inst::kErrNoCia, "romfs:/3dslink.cia missing");
        return;
    }
    u64 cia_size = 0;
    if (!FileSize(cia, &cia_size) || cia_size != man.cia_size) {
        L("3dslink.cia is %llu bytes, the manifest says %llu", static_cast<unsigned long long>(cia_size), static_cast<unsigned long long>(man.cia_size));
        fclose(cia);
        Fail(inst::kErrPayload, "3dslink.cia has the wrong size");
        return;
    }
    static u8 head[0x4000];
    const size_t head_len = fread(head, 1, sizeof(head), cia);
    inst::CiaInfo info;
    if (!inst::ParseCia(head, head_len, cia_size, &info)) {
        fclose(cia);
        Fail(inst::kErrNotCia, "3dslink.cia is not a CIA");
        return;
    }
    if (info.tid != man.tid) {
        L("the CIA is title %016llX, the manifest names %016llX", static_cast<unsigned long long>(info.tid), static_cast<unsigned long long>(man.tid));
        fclose(cia);
        Fail(inst::kErrNotThis, "3dslink.cia is not the title the manifest names");
        return;
    }
    if (info.version != man.version16) {
        L("the CIA says version %u, the manifest %u", static_cast<unsigned>(info.version), static_cast<unsigned>(man.version16));
        fclose(cia);
        Fail(inst::kErrPayload, "the manifest and the CIA disagree on the version");
        return;
    }
    // A title of the application family, and never the one that is running (AM_DeleteTitle on ourselves would be the end of the job).
    u64 own = 0;
    APT_GetProgramID(&own);
    if ((info.tid >> 32) != 0x00040000 || info.tid == own) {
        L("the CIA is title %016llX (we are %016llX): refused", static_cast<unsigned long long>(info.tid), static_cast<unsigned long long>(own));
        fclose(cia);
        Fail(inst::kErrNotCia, "3dslink.cia is not an application");
        return;
    }
    g_job.package_version = info.version;
    g_job.package_kb = static_cast<u32>((cia_size + man.plugin_size) >> 10);
    g_job.has_package = true;
    L("3dslink.cia: TID %016llX version %u (%s), %u content(s)", static_cast<unsigned long long>(info.tid), static_cast<unsigned>(info.version),
      inst::VersionText(info.version).c_str(), static_cast<unsigned>(info.content_count));

    FILE* plugin = fopen("romfs:/AzaharLink.3gx", "rb");
    u64 plugin_size = 0;
    u8 magic[4] = {};
    if (!plugin || !FileSize(plugin, &plugin_size) || plugin_size != man.plugin_size || fread(magic, 1, 4, plugin) != 4 ||
        !inst::PluginHeaderOk(magic, 4)) {
        L("plugin: %s, size %llu (manifest %llu)", plugin ? "present" : "fopen failed", static_cast<unsigned long long>(plugin_size),
          static_cast<unsigned long long>(man.plugin_size));
        if (plugin) {
            fclose(plugin);
        }
        fclose(cia);
        Fail(inst::kErrNoPlugin, "the plugin is missing or damaged");
        return;
    }

    // ---- the console
    const Result rdb = AM_InitializeExternalTitleDatabase(false); // non destructive; makes the SD card's title database if the card has none (FBI does it at start)
    L("AM_InitializeExternalTitleDatabase -> 0x%08lX", static_cast<unsigned long>(rdb));
    AM_TitleEntry cur;
    memset(&cur, 0, sizeof(cur));
    const Result rg = AM_GetTitleInfo(MEDIATYPE_SD, 1, &info.tid, &cur);
    const bool installed = R_SUCCEEDED(rg);
    // "not installed" is one precise answer (0xD8A083FA). Any other failure means AM could not say: the job goes on (a first install must not be blocked by a
    // console that answers oddly) but deletes nothing, because what is there is not known.
    const bool unknown = R_FAILED(rg) && !inst::IsTitleNotFound(static_cast<inst::u32>(rg));
    g_job.installed_version = installed ? static_cast<int>(cur.version) : -1;
    g_job.console_known = unknown ? 2 : 1;
    g_scrub_known = !unknown;
    L("AM_GetTitleInfo(SD, %016llX) -> 0x%08lX (installed: %d, version %u, size %llu%s)", static_cast<unsigned long long>(info.tid),
      static_cast<unsigned long>(rg), installed ? 1 : 0, static_cast<unsigned>(cur.version), static_cast<unsigned long long>(cur.size),
      unknown ? "; AM could not say" : "");
    inst::Action action = inst::Decide(installed, cur.version, info.version);
    g_job.action = static_cast<int>(action);

    FS_ArchiveResource ar;
    memset(&ar, 0, sizeof(ar));
    const Result rs = FSUSER_GetSdmcArchiveResource(&ar);
    const u64 free_bytes = static_cast<u64>(ar.freeClusters) * ar.clusterSize;
    g_job.sd_free_kb = static_cast<u32>(free_bytes >> 10);
    g_job.sd_known = R_SUCCEEDED(rs);
    L("SD card: %llu bytes free (rc 0x%08lX, cluster %lu)", static_cast<unsigned long long>(free_bytes), static_cast<unsigned long>(rs),
      static_cast<unsigned long>(ar.clusterSize));
    // When 3DS Link is already on the console the user decides, and nothing has been touched yet: a newer version is never replaced by an older package unless
    // the user says so with a key of its own (X), an update asks (A), and a same-version install whose plugin file is not the right one offers to restore it.
    if (action == inst::Action::Update && !Ask(AK_UPDATE)) {
        fclose(cia);
        fclose(plugin);
        g_job.outcome = OUT_LEFT;
        g_job.phase = PH_DONE;
        L("an older 3DS Link is installed (%u < %u): the user left it as it is", static_cast<unsigned>(cur.version), static_cast<unsigned>(info.version));
        return;
    }
    if (action == inst::Action::Skip) {
        if (!Ask(AK_DOWNGRADE)) {
            fclose(cia);
            fclose(plugin);
            g_job.outcome = OUT_NEWER;
            g_job.phase = PH_DONE;
            L("a newer 3DS Link is installed (%u > %u): left as it is", static_cast<unsigned>(cur.version), static_cast<unsigned>(info.version));
            return;
        }
        action = inst::Action::Update; // the user chose to replace the newer one by this older package: from here on it is an update that goes backwards
        g_job.action = static_cast<int>(action);
        g_job.downgrade = true;
        L("the user chose to install the older version %u over %u", static_cast<unsigned>(info.version), static_cast<unsigned>(cur.version));
    }
    // Compared ONCE, here: the same answer decides the question, the space the work needs and whether anything is written (a second look at a card that
    // answers differently must not write what nobody asked for).
    const bool plugin_same = SameFileAs(plugin, kPluginPath, plugin_size);
    if (inst::AsksRepair(action, plugin_same)) {   // (the copy of the app never asks: it is not the user's file)
        struct stat ps;
        L("the plugin file on the card is %s", stat(kPluginPath, &ps) == 0 ? "not the one of this version" : "missing");
        if (!Ask(AK_REPAIR)) {
            fclose(cia);
            fclose(plugin);
            g_job.outcome = OUT_LEFT;
            g_job.phase = PH_DONE;
            L("the plugin file is not the right one and the user left it as it is");
            return;
        }
    }
    if (R_FAILED(rs)) {
        fclose(cia);
        fclose(plugin);
        Fail(rs, "the SD card cannot be read");
        return;
    }
    // The copy of the app's CIA for the app (dlp/3dslink.cia), compared ONCE as well: it is written (after the app is installed) unless it is already the right file.
    const bool copy_same = SameFileAs(cia, kCopyPath, cia_size);
    {
        struct stat cs;
        L("the copy of the app on the card (%s) is %s", kCopyPath, copy_same ? "already this one" : (stat(kCopyPath, &cs) == 0 ? "not this one" : "missing"));
    }
    const bool write_copy = inst::WritesCopy(action, copy_same);
    const bool with_cia = inst::InstallsApp(action);
    const bool nothing_to_write = inst::NothingToWrite(action, plugin_same, copy_same); // same version, right plugin, right copy: no question, no write, no space needed
    if (!nothing_to_write && free_bytes < inst::NeedBytes(cia_size, plugin_size, action, write_copy)) {
        fclose(cia);
        fclose(plugin);
        Fail(inst::kErrSdFull, "not enough space on the SD card");
        return;
    }
    g_job.total_kb = static_cast<u32>((inst::WorkBytes(cia_size, plugin_size, action, write_copy) + 1023) >> 10);

    // ---- the folders and the plugin
    mkdir("sdmc:/3ds", 0777);
    mkdir(kDataDir, 0777);
    struct stat st;
    if (stat(kDataDir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        L("%s cannot be made, errno %d", kDataDir, errno);
        fclose(cia);
        fclose(plugin);
        Fail(inst::kErrPluginWrite, "the data folder cannot be made");
        return;
    }
    g_job.stage = ST_PLUGIN;
    bool plugin_written = false;
    if (plugin_same) {
        L("the plugin on the card is already this one");
        Advance(plugin_size);
    } else if (!WriteWhole(plugin, plugin_size, kPluginTarget)) {
        fclose(cia);
        fclose(plugin);
        Fail(inst::kErrPluginWrite, "the plugin could not be written");
        return;
    } else {
        plugin_written = true;
        L("plugin written: %llu bytes", static_cast<unsigned long long>(plugin_size));
    }
    fclose(plugin);

    if (!with_cia) {
        fclose(cia);
        LeaveAppCopy(cia_size, copy_same);   // (the app is there at this version: the copy is brought up to date like the plugin)
        g_job.outcome = plugin_written ? OUT_REPAIRED : OUT_CURRENT;
        g_job.phase = PH_DONE;
        L("the same version is installed: the app was not touched");
        return;
    }

    // ---- the app
    g_job.stage = ST_PREPARE;
    if (g_job.cancel) {   // the questions promised "nothing has been changed yet": a close request after the answer must not find the old app already deleted
        fclose(cia);
        Fail(inst::kErrCancelled, "cancelled before the app was touched");
        return;
    }
    Result rt = -1, rk = -1;
    if (!unknown) {
        if (installed) {
            g_job.touched = true; // from here on, if the install fails, the old 3DS Link is gone
            g_job.installed_version = -1;
        }
        rt = AM_DeleteTitle(MEDIATYPE_SD, info.tid);
        rk = AM_DeleteTicket(info.tid);
    } else {
        L("what is installed is not known: the title and its ticket are not deleted");
    }
    bool avail = false;
    const Result rd = AM_QueryAvailableExternalTitleDatabase(&avail);
    const Result rp = AM_DeletePendingTitle(MEDIATYPE_SD, info.tid);
    L("prep: DeleteTitle 0x%08lX DeleteTicket 0x%08lX QueryExternalDb 0x%08lX (available %d) DeletePending 0x%08lX", static_cast<unsigned long>(rt),
      static_cast<unsigned long>(rk), static_cast<unsigned long>(rd), avail ? 1 : 0, static_cast<unsigned long>(rp));

    Handle h = 0;
    const u64 a0 = osGetTime();
    Result r = AM_StartCiaInstall(MEDIATYPE_SD, &h);
    L("AM_StartCiaInstall(SD) -> 0x%08lX handle 0x%lX in %llu ms", static_cast<unsigned long>(r), static_cast<unsigned long>(h),
      static_cast<unsigned long long>(osGetTime() - a0));
    if (R_FAILED(r)) {
        fclose(cia);
        Fail(r, "AM_StartCiaInstall failed");
        return;
    }
    g_job.stage = ST_WRITE;
    if (fseek(cia, 0, SEEK_SET) != 0) {
        AM_CancelCIAInstall(h);
        fclose(cia);
        Scrub(info.tid);
        Fail(inst::kErrShortRead, "seek on the CIA failed");
        return;
    }
    u64 off = 0;
    bool first = true;
    while (off < cia_size) {
        if (g_job.cancel) {
            AM_CancelCIAInstall(h);
            fclose(cia);
            Scrub(info.tid);
            Fail(inst::kErrCancelled, "cancelled");
            return;
        }
        const size_t want = cia_size - off < kBlock ? static_cast<size_t>(cia_size - off) : kBlock;
        const size_t n = fread(g_buf, 1, want, cia);
        if (n != want) {
            AM_CancelCIAInstall(h);
            fclose(cia);
            Scrub(info.tid);
            Fail(inst::kErrShortRead, "the RomFS ended early");
            return;
        }
        const u64 w0 = osGetTime();
        u32 written = 0;
        r = FSFILE_Write(h, &written, off, g_buf, static_cast<u32>(n), 0);
        if (first) {
            L("first AM write: %zu bytes, rc 0x%08lX, %llu ms", n, static_cast<unsigned long>(r), static_cast<unsigned long long>(osGetTime() - w0));
            first = false;
        }
        if (R_FAILED(r) || written != n) {
            L("AM write failed at offset %llu: rc 0x%08lX, written %lu of %zu", static_cast<unsigned long long>(off), static_cast<unsigned long>(r),
              static_cast<unsigned long>(written), n);
            AM_CancelCIAInstall(h);
            fclose(cia);
            Scrub(info.tid);
            Fail(R_FAILED(r) ? r : inst::kErrShortWrite, "AM write failed");
            return;
        }
        off += n;
        Advance(n);
    }
    fclose(cia);
    g_job.stage = ST_FINISH;
    const u64 f0 = osGetTime();
    r = AM_FinishCiaInstall(h);
    L("AM_FinishCiaInstall -> 0x%08lX in %llu ms", static_cast<unsigned long>(r), static_cast<unsigned long long>(osGetTime() - f0));
    if (R_FAILED(r)) {
        Scrub(info.tid); // (the handle is gone after Finish: no AM_CancelCIAInstall)
        Fail(r, "AM_FinishCiaInstall failed");
        return;
    }

    // ---- is it there?
    g_job.stage = ST_VERIFY;
    AM_TitleEntry after;
    memset(&after, 0, sizeof(after));
    r = AM_GetTitleInfo(MEDIATYPE_SD, 1, &info.tid, &after);
    L("after the install: AM_GetTitleInfo -> 0x%08lX version %u size %llu", static_cast<unsigned long>(r), static_cast<unsigned>(after.version),
      static_cast<unsigned long long>(after.size));
    if (R_FAILED(r) || after.version != info.version || after.size == 0) {
        g_job.console_known = 2; // AM finished but does not show the title as it should: what is on the console is not known
        Fail(R_FAILED(r) ? r : inst::kErrNotVerified, "the title is not there after the install");
        return;
    }
    g_job.installed_version = static_cast<int>(after.version);
    g_job.console_known = 1;
    LeaveAppCopy(cia_size, copy_same);   // the app is verified: from here on nothing may turn a good installation into a failure screen (the copy only logs)
    g_job.outcome = g_job.downgrade ? OUT_DOWNGRADED : (action == inst::Action::Update ? OUT_UPDATED : OUT_INSTALLED);
    g_job.phase = PH_DONE;
    L("done: 3DS Link %s is installed", inst::VersionText(info.version).c_str());
}

// ---------------------------------------------------------------------------------------------
// The screens
// ---------------------------------------------------------------------------------------------
const char* StageText(int stage) {
    switch (stage) {
    case ST_CHECK:
        return "Checking the package";
    case ST_PLUGIN:
        return "Copying the plugin";
    case ST_PREPARE:
        return "Preparing the SD card";
    case ST_WRITE:
        return "Installing";
    case ST_FINISH:
        return "Finishing";
    case ST_COPY:
        return "Saving a copy of 3DS Link";
    default:
        return "Checking the result";
    }
}

// Nothing was changed on the console (the user left it as it is, or a newer 3DS Link stays): the screens draw no bar for it.
bool Unchanged(int outcome) {
    return outcome == OUT_NEWER || outcome == OUT_LEFT;
}

std::string DoneLine(int outcome) {
    switch (outcome) {
    case OUT_UPDATED:
        return "3DS Link was updated";
    case OUT_DOWNGRADED:
        return "3DS Link was replaced by the older version";
    case OUT_REPAIRED:
        return "The plugin file was restored";
    case OUT_CURRENT:
        return "3DS Link is already installed";
    case OUT_NEWER:
        return "A newer 3DS Link is already installed";
    case OUT_LEFT:
        return "Nothing was changed";
    default:
        return "3DS Link is installed";
    }
}

// What the failure screen says about the console: only what is known. (The std::string lives until the end of the statement that draws it.)
std::string ErrorBody() {
    const bool touched = g_job.touched;
    const bool unknown = g_job.console_known == 2;
    const char* what = unknown && touched ? "Something went wrong. The old 3DS Link was taken off and the new one could not be checked."
                       : unknown          ? "Something went wrong. What 3DS Link is on this console could not be checked."
                       : touched          ? "Something went wrong and the old 3DS Link was taken off this console."
                                          : "Something went wrong. 3DS Link was not touched on this console.";
    return std::string(what) + (g_can_retry ? " A tries again, B leaves." : " Press B to leave.");
}

// The question of the decision screen, from what the worker found: the headline, two detail lines, the body of the bottom screen and the button that says yes.
// (The other button is always B, Leave. The step that takes a newer 3DS Link off is X, not A: the key that confirms everything else is not the one that
// confirms this.)
struct AskText {
    std::string headline, line1, line2, body;
    const char* key = "A";
    const char* label = "OK";
    u32 color = col::kWarn;
};

AskText AskScreen() {
    const std::string have = g_job.installed_version >= 0 ? inst::VersionText(static_cast<unsigned>(g_job.installed_version)) : std::string("?");
    const std::string pkg = inst::VersionText(g_job.package_version);
    AskText a;
    switch (g_job.ask_kind) {
    case AK_DOWNGRADE:
        a.headline = "An OLDER version than yours";
        a.color = col::kOff;
        a.line1 = "This console has 3DS Link " + have + ". This package is " + pkg + ".";
        a.line2 = "Installing it takes the newer one off.";
        a.body = "This console already has 3DS Link " + have + ", which is NEWER than this package (" + pkg +
                 "). Installing the package takes the newer version off. Most likely you want to leave it as it is.";
        a.key = "X";
        a.label = "Install older";
        break;
    case AK_REPAIR:
        a.headline = "3DS Link " + have + " is already installed";
        a.color = col::kAccent;
        a.line1 = "Its plugin file is missing or different.";
        a.line2 = "The package has the right one.";
        a.body = "3DS Link " + have + " is installed, but its plugin file (AzaharLink.3gx) is missing or is not the one of this version. Restore it?";
        a.key = "A";
        a.label = "Restore plugin";
        break;
    default:
        a.headline = "Update available";
        a.color = col::kWarn;
        a.line1 = "3DS Link " + have + "  ->  " + pkg;
        a.line2 = std::string("This package is ") + inst::ChangeText(inst::Compare(static_cast<unsigned>(g_job.installed_version), g_job.package_version)) + ".";
        a.body = "3DS Link " + have + " is installed on this console. This package has " + pkg + " (" +
                 inst::ChangeText(inst::Compare(static_cast<unsigned>(g_job.installed_version), g_job.package_version)) + "). Update now?";
        a.key = "A";
        a.label = "Update";
        break;
    }
    return a;
}

// The footer offers the guide (and Y opens it) only when the notice is up, the screens are citro2d ones (the viewer draws with them) and the viewer could be
// opened the last time it was tried: a key that does nothing is not shown.
bool GuideOffered(const inst::Boot9Notice& notice, bool graphics) {
    return notice.key && graphics && !g_guide_missing;
}

// The heading of the card: where boot9.bin was looked for, which are the two folders guide::Boot9Found looks in (the guide and the app say the same: Tests/policy_check.sh).
std::string Boot9Heading() {
    return std::string("boot9.bin was not found in ") + guide::kBoot9AppDir + " or " + guide::kBoot9GodModeDir;
}

// What the bottom screen says about boot9.bin. The words are the user's decision (DlpLink/docs/GUIDA-BOOT9.md): it is not needed to use 3DS Link, only to share;
// later is fine, and so is right now, by turning the console off (the guide starts there).
std::string Boot9Explanation() {
    return std::string("3DS Link works without it. It is only needed to share 3DS Link and titles with other consoles. You can do it later, or right now: ") +
           (g_guide_missing ? "the guide is in the 3DS Link app (\"How to get boot9.bin\"), and it starts with turning this console off."
                            : "the guide shows how, starting with turning this console off.");
}

void Draw() {
    const int phase = g_job.phase;
    const int stage = g_job.stage;
    const int outcome = g_job.outcome;
    const inst::Boot9Notice notice = inst::Boot9NoticeFor(phase == PH_DONE, outcome, g_boot9_found);
    const bool guide_on = GuideOffered(notice, true);   // (Draw runs only with citro2d up)
    const bool running = phase == PH_RUN;
    const bool asking = phase == PH_ASK;
    const u32 total = g_job.total_kb, done = g_job.done_kb;
    const float frac = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
    const bool nothing = phase == PH_DONE && Unchanged(outcome); // nothing was done: no bar to fill
    const int pct = nothing ? 0 : (phase == PH_DONE ? 100 : static_cast<int>(frac * 100.0f));
    const bool keep = g_job.action == static_cast<int>(inst::Action::Keep);
    const bool update = g_job.action == static_cast<int>(inst::Action::Update);
    const AskText ask = asking ? AskScreen() : AskText();

    gfx::BeginTop();
    DrawTitleBar(gfx::kTopW, "3DS Link Installer", "Powered by Gionko", false);
    const u32 stripe = phase == PH_ERR ? col::kOff : (asking ? ask.color : (phase == PH_DONE ? (Unchanged(outcome) ? col::kWarn : col::kOn) : col::kAccent));
    DrawCard(10, 32, 380, 126, stripe);
    DrawDotText(24, 50, col::kOn, col::kText, "Download complete", 0.5f, 340);
    if (asking) {
        DrawDotText(24, 74, ask.color, col::kText, ask.headline, 0.5f, 340);
        gfx::DrawFitted(24, 100, 0.44f, 0.34f, 350, col::kText, gfx::Clean(ask.line1));
        gfx::DrawFitted(24, 120, 0.42f, 0.34f, 350, col::kDim, gfx::Clean(ask.line2));
        gfx::DrawText(24, 142, 0.38f, col::kDim, "Nothing has been changed yet.");
    } else if (phase == PH_ERR) {
        DrawDotText(24, 74, col::kOff, col::kText, "Installation failed", 0.5f, 340);
    } else if (phase == PH_DONE) {
        DrawDotText(24, 74, Unchanged(outcome) ? col::kWarn : col::kOn, col::kText, DoneLine(outcome), 0.5f, 340);
    } else {
        DrawDotText(24, 74, col::kWarn, col::kText,
                    g_job.action < 0 ? "Checking this console..."
                                     : (g_job.downgrade ? "Installing the older version..." : (update ? "Updating 3DS Link..." : (keep ? "Refreshing the plugin..." : "Installing 3DS Link..."))),
                    0.5f, 340);
    }
    if (!asking) {
        DrawProgressBar(24, 92, 300, 14, nothing ? 0.0f : (phase == PH_DONE ? 1.0f : frac));
        if (!nothing) {
            gfx::DrawText(376, 99, 0.62f, col::kText, Fmt("%d%%", pct), gfx::Align::Right);
        }
    }
    if (running) {
        gfx::DrawText(24, 124, 0.42f, col::kDim, StageText(stage));
        const int step = static_cast<int>((osGetTime() / 250) % 4);
        for (int i = 0; i < 4; ++i) {
            gfx::FillCircle(340.0f + 10.0f * static_cast<float>(i), 124, 3.0f, i == step ? col::kAccent : col::kTrack);
        }
        gfx::DrawText(24, 142, 0.38f, col::kDim, "Do not turn the console off.");
    } else if (phase == PH_DONE) {
        gfx::DrawText(24, 124, 0.42f, Unchanged(outcome) ? col::kWarn : col::kOn,
                      Unchanged(outcome) ? "Nothing was changed on this console." : (outcome == OUT_REPAIRED ? "3DS Link is ready on the HOME menu." : "3DS Link is on the HOME menu."));
        if (notice.top) {
            gfx::DrawFitted(24, 142, 0.38f, 0.34f, 350, col::kWarn, "boot9.bin not found: only needed to share. See the lower screen.");
        }
    } else if (phase == PH_ERR) {
        gfx::DrawWrapped(24, 114, 350, 0.40f, col::kOff, gfx::Clean(g_job.msg), 2); // (the sentence that says what to do is the long one: it wraps, it is not cut)
        gfx::DrawText(24, 148, 0.4f, col::kText, Fmt("Error code 0x%08lX", static_cast<unsigned long>(g_job.rc)));
    }
    DrawCard(10, 166, 380, 64, col::kLine);
    gfx::DrawText(24, 182, 0.4f, col::kDim,
                  g_job.has_package ? Fmt("Package   3DS Link %s", inst::VersionText(g_job.package_version).c_str())
                                    : std::string(phase == PH_ERR ? "Package   (not read)" : "Package   (reading)"));
    const int known = g_job.console_known;
    const int iv = g_job.installed_version;   // read once: the worker changes it while the app is taken off
    gfx::DrawText(24, 198, 0.4f, col::kDim,
                  known == 0 ? std::string(phase == PH_ERR ? "On this console   (not read)" : "On this console   (checking)")
                             : (known == 2 ? std::string("On this console   (could not be read)")
                                           : (iv < 0 ? std::string("On this console   not installed")
                                                     : Fmt("On this console   %s", inst::VersionText(static_cast<unsigned>(iv)).c_str()))));
    gfx::DrawText(24, 214, 0.4f, col::kDim,
                  std::string(g_job.sd_known ? Fmt("SD card   %.1f MiB free", static_cast<double>(g_job.sd_free_kb) / 1024.0)
                                             : std::string(phase == PH_ERR ? "SD card   (not read)" : "SD card   (checking)")) +
                      (g_job.has_package ? Fmt("      Size   %.2f MiB", static_cast<double>(g_job.package_kb) / 1024.0) : std::string()));
    DrawFrameBorder(gfx::kTopW);

    gfx::BeginBottom();
    DrawTitleBar(gfx::kBottomW, asking ? "Your choice" : "Installation", "", running);
    const std::string body = asking ? ask.body
                             : running ? (g_job.action < 0 ? "This console is being checked. HOME and sleep are off until it is done."
                                          : keep           ? "The plugin of 3DS Link is being refreshed on this console. HOME and sleep are off until it is done."
                                                           : "3DS Link is being installed on this console. HOME and sleep are off until it is done.")
                             : phase == PH_DONE
                                 ? (Unchanged(outcome) ? "Nothing was changed on this console. Press A to go back to the HOME menu."
                                    : notice.bottom    ? "Done. Press A: the console goes back to the HOME menu, where 3DS Link is waiting."
                                                       : "Done. Press A: the console goes back to the HOME menu, where 3DS Link is waiting.")
                                 : ErrorBody();
    const float body_h = gfx::DrawWrapped(16, 40, 288, 0.46f, col::kText, body, notice.bottom ? 3 : 7);
    if (notice.bottom) {   // a card of its own under the sentence, from its real height (the console's font is taller than an emulator's) down to the footer
        const float card_y = 40.0f + body_h + 6.0f, card_h = 208.0f - card_y;
        DrawCard(8, card_y, 304, card_h, col::kWarn);
        // The heading names the two places the search looked in (guide::Boot9Found: kBoot9Dirs): it is longer than one line, so it wraps (two lines at most) beside the dot,
        // and the explanation takes what is left of the card from the real height of the heading.
        gfx::FillCircle(24.0f, card_y + 15.0f, 4.0f, col::kWarn);
        const float head_h = gfx::DrawWrapped(34, card_y + 8.0f, 270, 0.44f, col::kText, Boot9Heading(), 2);
        const float text_y = card_y + 8.0f + head_h + 4.0f;
        const int room = static_cast<int>((card_y + card_h - 4.0f - text_y) / gfx::LineHeight(0.40f));
        gfx::DrawWrapped(20, text_y, 284, 0.40f, col::kText, Boot9Explanation(), room > 1 ? room : 1);
    }
    DrawFooterBackground();
    const bool keys = g_keys_open;   // the buttons look pressable only when the keys count (not in the first 800 ms of a screen, not after the answer)
    if (asking) {
        const bool open = keys && g_job.decision == 0;
        DrawButton(8, 216, 120, 20, "B", "Leave", open);
        DrawButton(132, 216, 180, 20, ask.key, ask.label, open);
    } else if (phase == PH_ERR) {
        DrawButton(8, 216, g_can_retry ? 150 : 304, 20, "B", "Leave", keys);
        if (g_can_retry) {
            DrawButton(162, 216, 150, 20, "A", "Try again", keys);
        }
    } else if (phase == PH_DONE && guide_on) {   // the guide on the left, "Go to HOME" on the right (B leaves too; the other buttons do nothing)
        DrawButton(8, 216, 150, 20, "Y", "boot9.bin guide", keys);
        DrawButton(162, 216, 150, 20, "A", "Go to HOME", keys);
    } else {
        DrawButton(8, 216, 304, 20, "A", phase == PH_DONE ? "Go to HOME" : "Please wait...", phase == PH_DONE && keys);
    }
    DrawFrameBorder(gfx::kBottomW);
}

// When citro2d or the system font cannot start (memory): the same words on the text console, which the project has run on hardware.
void DrawConsole() {
    consoleClear();
    printf("3DS Link Installer - Powered by Gionko\n\n");
    const int phase = g_job.phase;
    if (phase == PH_ASK) {
        const AskText a = AskScreen();
        printf("%s\n%s\n%s\n\nB leaves, %s: %s.\n", a.headline.c_str(), a.line1.c_str(), a.line2.c_str(), a.key, a.label);
    } else if (phase == PH_RUN) {
        const u32 total = g_job.total_kb, done = g_job.done_kb;
        printf("%s...\n", StageText(g_job.stage));
        printf("[%-20.*s] %d%%\n", total ? static_cast<int>(20ull * done / total) : 0, "####################", total ? static_cast<int>(100ull * done / total) : 0);
        printf("\nDo not turn the console off.\n");
    } else if (phase == PH_DONE) {
        printf("%s.\n\nPress A to go back to the HOME menu.\n", DoneLine(g_job.outcome).c_str());
        if (inst::Boot9NoticeFor(true, g_job.outcome, g_boot9_found).any()) {   // (no pictures on this screen: the guide itself is in the 3DS Link app)
            printf("\nboot9.bin was not found in %s\nor %s.\n3DS Link works without it; it is only needed\nto share 3DS Link and titles with other\nconsoles. The guide is in the 3DS Link app.\n", guide::kBoot9AppDir,
                   guide::kBoot9GodModeDir);
        }
    } else {
        printf("Installation failed.\n%s\nError code 0x%08lX\n\n%s\n", g_job.msg, static_cast<unsigned long>(g_job.rc), g_can_retry ? "B leaves, A tries again." : "B leaves.");
    }
}

Thread StartWorker() {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // core 0: the child's affinity mask is 1 (the Download Play child template has no core 1)
    return threadCreate(Worker, nullptr, 64 * 1024, prio < 0x3F ? prio + 1 : prio, -2, false);
}

} // namespace

int main(int, char**) {
    g_t0 = osGetTime();
    gfxInitDefault();
    aptSetHomeAllowed(false); // from the first moment: a press of HOME during an AM write would suspend the app in the middle of the install
    aptSetSleepAllowed(false);
    bool power_held = true;

    mkdir("sdmc:/3ds", 0777);
    mkdir(kDataDir, 0777);
    {   // the log is cut when it has grown past 64 KB: one file collects the deliveries, it must not grow for ever
        struct stat st;
        const bool big = stat(kLogPath, &st) == 0 && static_cast<u64>(st.st_size) > kLogMaxBytes;
        g_log = fopen(kLogPath, big ? "w" : "a");
    }
    {
        time_t tt = time(nullptr);
        struct tm* lt = localtime(&tt);
        if (g_log && lt) {
            fprintf(g_log, "\n===== 3DS Link Installer run %04d-%02d-%02d %02d:%02d:%02d =====\n", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                    lt->tm_hour, lt->tm_min, lt->tm_sec);
        }
    }
    u64 mytid = 0;
    const Result rpid = APT_GetProgramID(&mytid);
    L("3DS Link Installer start: APT_GetProgramID 0x%08lX %016llX, envIsHomebrew %d", static_cast<unsigned long>(rpid), static_cast<unsigned long long>(mytid),
      static_cast<int>(envIsHomebrew()));

    const Result rcfg = cfguInit();
    L("cfguInit -> 0x%08lX", static_cast<unsigned long>(rcfg));
    Result rrom = romfsInit();
    L("romfsInit -> 0x%08lX", static_cast<unsigned long>(rrom));
    if (R_FAILED(rrom) && mytid != 0) {
        rrom = romfsMountFromTitle(mytid, MEDIATYPE_NAND, "romfs"); // the other way to read our own RomFS
        L("romfsMountFromTitle(NAND) -> 0x%08lX", static_cast<unsigned long>(rrom));
    }
    // am:net by name first: libctru's amInit() takes am:net, then am:u, then am:sys and says yes to the first, so a refusal of am:net would hide
    // behind am:u and show up only at AM_StartCiaInstall.
    Handle probe = 0;
    const Result rnet = srvGetServiceHandle(&probe, "am:net");
    if (R_SUCCEEDED(rnet)) {
        svcCloseHandle(probe);
    }
    L("srvGetServiceHandle(am:net) -> 0x%08lX", static_cast<unsigned long>(rnet));
    const Result ram = R_SUCCEEDED(rnet) ? amInit() : rnet;
    L("amInit -> 0x%08lX", static_cast<unsigned long>(ram));

    const bool gok = gfx::Init();
    L("gfx::Init -> %d %s", static_cast<int>(gok), gok ? "" : gfx::InitError());
    if (!gok) {
        consoleInit(GFX_TOP, nullptr);
    }
    L("memory: APPLICATION region free %lu | linear free %lu | heap size %lu", static_cast<unsigned long>(osGetMemRegionFree(MEMREGION_APPLICATION)),
      static_cast<unsigned long>(linearSpaceFree()), static_cast<unsigned long>(envGetHeapSize()));

    Thread th = nullptr;
    const bool can_work = R_SUCCEEDED(rrom) && R_SUCCEEDED(ram);
    g_can_retry = can_work;
    if (!can_work) {
        Fail(R_FAILED(rrom) ? rrom : ram, R_FAILED(rrom) ? "romfs unavailable" : "am unavailable");
    } else {
        th = StartWorker();
        L("threadCreate -> %p", static_cast<void*>(th));
        if (!th) {
            Fail(inst::kErrNoMemory, "the worker thread could not be made");
        }
    }

    inst::Lockout lock;   // the keys of a screen count 800 ms after it appeared; ANY change of screen starts the count again (installer_logic.hpp, tested on the PC)
    bool closing = false;
    bool leave_by_key = false;   // the loop ended because the user pressed A or B on a finished screen (the log says which: the system closing the app looks the same otherwise)
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        const int phase = g_job.phase;
        // HOME and sleep are off while the work runs and the user's again whenever the screens are waiting for them (a question, a result)
        if ((phase == PH_RUN) != power_held) {
            aptSetHomeAllowed(phase != PH_RUN);
            aptSetSleepAllowed(phase != PH_RUN);
            power_held = phase == PH_RUN;
        }
        // boot9.bin is looked up once for each finished screen (a few file opens; the card does not change under a running app)
        if (phase != PH_DONE) {
            g_boot9_checked = false;
        } else if (!g_boot9_checked) {
            g_boot9_found = guide::Boot9Found();
            g_boot9_checked = true;
            L("boot9.bin on the SD card: %s", g_boot9_found ? "found" : "not found");
        }
        const inst::Boot9Notice notice = inst::Boot9NoticeFor(phase == PH_DONE, g_job.outcome, g_boot9_found);
        // (a question answered with B goes to its result in microseconds: the screens never see the work in between, so the phase has to be compared, not "RUN seen")
        // The guide is a screen of its own for the lockout: when it closes the result screen is new again and its keys wait 800 ms (the B that closed the guide
        // must not also be the press that leaves).
        const bool lock_open = lock.Open(g_guide_open ? inst::kScreenGuide : phase, osGetTime());
        g_keys_open = lock_open && phase != PH_RUN && !g_guide_open;
        if (lock.changed && phase != PH_RUN && !g_guide_open) {
            L("the screens see phase %d", phase);
        }
        if (g_guide_open) {   // the viewer's keys (L/R, D-pad up/down, B) count at once: nobody was asked anything, and a step is not an answer
            int tap_x = -1, tap_y = -1;   // (the viewer's footer buttons answer a tap as they answer the keys)
            if (down & KEY_TOUCH) {
                touchPosition touch;
                hidTouchRead(&touch);
                tap_x = touch.px;
                tap_y = touch.py;
            }
            if (!g_viewer.Update(down, tap_x, tap_y)) {
                g_viewer.Close();
                g_guide_open = false;
                L("the boot9.bin guide closed");
            }
        } else if (g_keys_open) {
            if (phase == PH_ASK) {
                const bool older = g_job.ask_kind == AK_DOWNGRADE; // the step that takes a newer 3DS Link off has a key of its own: X
                g_job.decision = inst::NextDecision(g_job.decision, (down & KEY_B) != 0, (down & (older ? KEY_X : KEY_A)) != 0);
            } else if (phase == PH_ERR && can_work && (down & KEY_A)) {
                if (th) {   // "Try again": the worker is over (it set the phase last), a new one starts from the top
                    threadJoin(th, U64_MAX);
                    threadFree(th);
                    th = nullptr;
                }
                Reset();
                lock.Forget();   // the next frame is a new screen whatever it shows (the worker may reach a question or a failure before it)
                L("try again");
                th = StartWorker();
                if (!th) {
                    Fail(inst::kErrNoMemory, "the worker thread could not be made");
                }
            } else {   // a finished screen or a failure that cannot be tried again: every button leaves, except the guide's own key while the notice is up
                const inst::ResultKey key = inst::ResultKeyFor(down, phase == PH_DONE, GuideOffered(notice, gok));
                if (key == inst::ResultKey::Leave) {
                    L("leave: the user pressed %s (buttons down 0x%08lX, phase %d)", (down & KEY_A) ? "A" : "B", static_cast<unsigned long>(down), phase);
                    leave_by_key = true;
                    break;
                }
                if (key == inst::ResultKey::Guide) {
                    if (g_viewer.Open()) {
                        g_guide_open = true;
                        L("the boot9.bin guide opened");
                    } else {
                        g_guide_missing = true;   // no dead key: the footer drops it and the notice says where else the guide is
                        L("the boot9.bin guide could not be opened (the pictures are not in this build, or there is no memory for them)");
                    }
                }
            }
        }
        if (gok) {
            if (gfx::BeginFrame()) {
                if (g_guide_open) {
                    gfx::BeginTop();
                    g_viewer.DrawTop();
                    gfx::BeginBottom();
                    g_viewer.DrawBottom();
                } else {
                    Draw();
                }
                gfx::EndFrame();
            } else {
                gspWaitForVBlank();
            }
        } else {
            DrawConsole();
            gspWaitForVBlank();
        }
    }
    if (!leave_by_key) {
        L("leave: aptMainLoop() returned false (the system asks the app to close)");
    }
    if (g_job.phase == PH_RUN || g_job.phase == PH_ASK) {   // the system asked the app to close in the middle of the work (or of a question): the worker cancels the install between two blocks
        closing = true;
        g_job.cancel = true;
        L("the system asks the app to close during the work");
    }
    if (th) {
        threadJoin(th, U64_MAX); // (U64_MAX never times out: a timeout would read as success)
        threadFree(th);
    }
    L("leaving: closing %d, phase %d, rc 0x%08lX", closing ? 1 : 0, static_cast<int>(g_job.phase), static_cast<unsigned long>(g_job.rc));
    aptSetHomeAllowed(true);
    aptSetSleepAllowed(true);
    if (g_guide_open) {   // (the system asked the app to close with the guide on the screen: its textures go before the graphics do)
        g_viewer.Close();
        g_guide_open = false;
    }
    if (gok) {
        gfx::Shutdown();
    }
    if (R_SUCCEEDED(ram)) {
        amExit();
    }
    if (R_SUCCEEDED(rrom)) {
        romfsExit();
    }
    if (R_SUCCEEDED(rcfg)) {
        cfguExit();
    }
    L("calling gfxExit and returning from main");
    if (g_log) {
        fclose(g_log);
    }
    gfxExit();
    return 0;
}
