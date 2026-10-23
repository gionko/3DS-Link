// dlpc.h -- the composer of the Download Play child of 3DS Link (child.cia), run by the app on the console.
//
// 3DS Link is ONE CIA. The child that Download Play consoles download carries the app's CIA and the plugin, so it cannot be inside the app's CIA (the child
// would hold the app and the app the child). The app builds it instead, the first time the 3DS Link Delivery page needs it: the program of the installer
// (about 250 KB, the "model": DlpLink/tools/make_model.py makes it from the finished child, without its RomFS) is a resource of the app; this library puts
// the payload (the app's CIA, the plugin, a manifest) into a RomFS, seals the NCCH and the TMD and writes the finished child.cia.
//
// Portable C99: no libctru, no I/O besides the callbacks and the output file, so the same code runs on the console and in the PC tests
// (DlpLink/tests/dlpc_test.c). The design and the rules are in DlpLink/docs/COMPOSITORE.md.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads `len` bytes at `off` of a payload file. 0 = done, anything else = failed.
typedef int (*dlpc_read_fn)(void* user, uint64_t off, uint8_t* buf, size_t len);

// The stages, in the order they run (the screen says what the app is doing and the estimate follows the weights of DlpLink/docs/COMPOSITORE.md).
enum {
    DLPC_STAGE_READ_APP = 1,    // the app's CIA is read (and hashed)
    DLPC_STAGE_READ_PLUGIN = 2, // the plugin is read (and hashed)
    DLPC_STAGE_TREE = 3,        // the RomFS and its hash tree
    DLPC_STAGE_SEAL = 4,        // encryption of the RomFS, NCCH header, TMD chain
    DLPC_STAGE_WRITE = 5,       // the file goes to the card
    DLPC_STAGE_VERIFY = 6       // the file is read back and checked
};

// Called often (about every 64 KB of work): `done` of `total` bytes of the stage. Return nonzero to cancel (the output is not left behind).
// The units are the bytes that the passes of the stage go through (a pass that reads and hashes a file counts it once; a stage with two passes counts the
// file twice), so done/total is a fair fraction of the stage but not of its TIME: the stages cost very differently per byte. For the time use
// dlpc_stage_work() below. The first call of a stage has done = 0 and the last one done = total; `total` does not change inside a stage.
typedef int (*dlpc_progress_fn)(void* user, int stage, uint64_t done, uint64_t total);

typedef struct {
    const uint8_t* model;        // the resource inside the app (the child without its RomFS: DlpLink/tools/make_model.py)
    size_t model_size;
    uint64_t app_tid;            // 000400000F83D100: the title that the child installs (the manifest says it)
    uint16_t version16;          // TMD version of the app: major << 10 | minor << 4 | micro (1024 = 1.0.0); the model must carry the same
    const char* version_text;    // "1.0.0": major.minor.micro, and it must say what version16 says (it goes into manifest.txt)
    dlpc_read_fn read_app;       // the app's CIA (the same bytes that AM installs); read once, in order, in pieces of 64 KB
    void* app_user;
    uint64_t app_size;           // the size of that file, at most 32 MiB (a CIA that lacks the padding of its last content is fine, as for the installer)
    dlpc_read_fn read_plugin;    // the plugin (AzaharLink.3gx): a .3gx of version16 (the version the Installation check reads: bytes 9..11 of its header)
    void* plugin_user;
    uint64_t plugin_size;
    const char* out_path;        // the finished child; written as "<out_path>.part" and renamed when it is whole and verified (its folder must be there: nothing makes it)
    int emu_variant;             // 1 = the NoCrypto variant that only Azahar installs (tests; the model must be the emulator model); 0 = the child that is delivered
    dlpc_progress_fn progress;   // may be NULL
    void* progress_user;
} dlpc_job;

typedef enum {
    DLPC_OK = 0,
    DLPC_E_MODEL,        // the resource is not a model of this library (magic, sizes, a CIA that does not parse)
    DLPC_E_VERSION,      // the model's version is not job->version16 (the app and its model are from different builds), or version_text is not major.minor.micro of it
    DLPC_E_READ_APP,     // read_app failed
    DLPC_E_READ_PLUGIN,  // read_plugin failed
    DLPC_E_APP_BAD,      // the bytes of the app are not a CIA of app_tid with version16 whose contents match its TMD (a stale, foreign or damaged file)
    DLPC_E_PLUGIN_BAD,   // the plugin is not a .3gx of version16
    DLPC_E_NOMEM,
    DLPC_E_TOOBIG,       // the child would pass 32 MiB (the limit of dlph_open)
    DLPC_E_WRITE,        // the card refused the file
    DLPC_E_VERIFY,      // the file read back is not a valid child (the output is removed)
    DLPC_E_CANCEL
} dlpc_result;

// Composes the child. On failure `err` (if not NULL) gets a short English sentence and nothing is left at out_path or "<out_path>.part". A file that is
// already at out_path is removed when the job starts (the job is there because that file is missing or is not the right one): a failure leaves NO child.
// The child is built whole in one block of memory, the size of the finished file (about 2 MB for 3DS Link 1.0, see dlpc_memory_needed), and the callbacks
// are called from the calling thread, one at a time.
dlpc_result dlpc_compose(const dlpc_job* job, char* err, size_t err_size);

// What the finished file will weigh and how much memory dlpc_compose will take (for the space check and the estimate; no I/O). Both are 0 when the job
// cannot be laid out (the model is not a model); a payload past the limit gives a size past 32 MiB (dlpc_compose then says DLPC_E_TOOBIG).
uint64_t dlpc_output_size(const dlpc_job* job);
size_t dlpc_memory_needed(const dlpc_job* job);

// What a stage costs, by kind of work, for an honest estimate: seconds = sd_read / (card read speed) + sd_write / (card write speed) + sha / (SHA-256
// speed) + aes / (AES-CTR speed), each speed measured on the console (the software SHA-256 and AES of titleshare/common/). No I/O; all zero for a job that
// cannot be laid out and for a stage that does not exist.
typedef struct {
    uint64_t sd_read, sd_write, sha, aes;   // bytes
} dlpc_work;
dlpc_work dlpc_stage_work(const dlpc_job* job, int stage);

// A short English phrase for a result and for a stage.
const char* dlpc_result_text(dlpc_result r);
const char* dlpc_stage_text(int stage);

// Is `model` a model that this library composes from (the whole check of dlpc_compose, its SHA-256 included: about 220 KB hashed), and which version does it
// carry (0 when it is not)?
uint16_t dlpc_model_version(const uint8_t* model, size_t model_size);

#ifdef __cplusplus
}
#endif
