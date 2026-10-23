// What 3DS Link Delivery does BEFORE its room opens, the decisions that need no console: is the package that other consoles download (child.cia) good, which
// source the composer builds it from, how long the one-time work takes and how far it is, the marker that tells a run that died in the middle, and what each
// way it can fail asks of the user. Plain C++ with no libctru in it, so Tests/settings_test/dlp_prepare_test.cpp builds this very file on a PC (against real
// CIAs when the tree has them). dlp_prepare.cpp is the part that talks to the console (the card, the threads, the composer, the module's CIA stream); the words
// of the screens are in dlp_text.hpp; the page is pages_dlp.cpp. The design is DlpLink/docs/COMPOSITORE.md.
//
// The CIA offsets are the ones of DlpLink/child/source/installer_logic.hpp (ParseCia), which are FBI's and ctrtool's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "../dlpc/dlpc.h"

namespace dlp {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The title of the app itself: the child installs this one, and the CIA the composer takes in must be it.
constexpr u64 kAppTid = 0x000400000F83D100ull;
// The library that sends the child takes no package bigger than this (dlph_open); an empty file is no package either.
constexpr u64 kMaxChildBytes = 32ull << 20;
// The app's own CIA is about 2 MB; one that would not fit in a child is not it.
constexpr u64 kMaxAppCiaBytes = 30ull << 20;

// "1.0.0" -> 1024: the version as the TMD holds it, major << 10 | minor << 4 | micro. 0 when the text is not three numbers or does not fit the TMD (major and
// minor up to 63, micro up to 15: the same limits Tests/policy_check.sh puts on VERSION).
inline unsigned Version16(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    unsigned part[3] = {0, 0, 0};
    int index = 0;
    bool digits = false;
    for (const char* p = text;; ++p) {
        if (*p >= '0' && *p <= '9') {
            part[index] = part[index] * 10 + static_cast<unsigned>(*p - '0');
            digits = true;
            if (part[index] > 63) {
                return 0;
            }
        } else if (*p == '\0' && digits) {
            return index == 2 && part[2] < 16 ? (part[0] << 10 | part[1] << 4 | part[2]) : 0;
        } else if (*p == '.' && digits && index < 2) {
            ++index;
            digits = false;
        } else {
            return 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// A CIA: the title and the version its TMD holds
// ---------------------------------------------------------------------------------------------

inline u32 Be32(const u8* p) {
    return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3];
}
inline u32 Le32(const u8* p) {
    return p[0] | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24;
}
inline u64 Le64(const u8* p) {
    return static_cast<u64>(Le32(p)) | static_cast<u64>(Le32(p + 4)) << 32;
}
inline u64 Align64(u64 x) {
    return (x + 63) & ~static_cast<u64>(63);
}

struct CiaFacts {
    bool parsed = false; // a CIA whose TMD could be read
    u64 tid = 0;
    u16 version = 0;
    u16 contents = 0;
    u64 end = 0;         // where its header says the file ends (the contents and the meta included)
};

// Reads `len` bytes at `off` of the file being looked at. True when it did.
using ReadAt = std::function<bool(u64 off, u8* buf, std::size_t len)>;

constexpr u64 kCiaHeaderBytes = 0x2020;
// How much of the TMD is looked at: the signature (up to 0x200 and its padding) and the header (0xC4 bytes; the title version at +0x9C, the content count at +0x9E).
constexpr std::size_t kTmdNeeded = 0x400;

// Where the TMD starts, from the first 0x20 bytes (0 when they are not a CIA header).
inline u64 TmdOffset(const u8* head, std::size_t len) {
    if (len < 0x20 || Le32(head) != kCiaHeaderBytes) {
        return 0;
    }
    const u32 cert = Le32(head + 8), ticket = Le32(head + 12), tmd = Le32(head + 16);
    if (cert == 0 || ticket == 0 || tmd == 0 || cert > 0x10000 || ticket > 0x10000 || tmd > 0x100000) {
        return 0;
    }
    return Align64(kCiaHeaderBytes) + Align64(cert) + Align64(ticket);
}

// Reads the header of the CIA and its TMD (two small reads). `file_size` is the size of the file.
inline CiaFacts InspectCia(const ReadAt& read, u64 file_size) {
    CiaFacts facts;
    u8 head[0x20];
    if (file_size < sizeof(head) || !read(0, head, sizeof(head))) {
        return facts;
    }
    const u64 tmd = TmdOffset(head, sizeof(head));
    if (tmd == 0 || tmd + kTmdNeeded > file_size) {
        return facts;
    }
    u8 buf[kTmdNeeded];
    if (!read(tmd, buf, sizeof(buf))) {
        return facts;
    }
    u32 sig_size = 0, pad = 0x3C; // RSA signatures are followed by 0x3C bytes of padding, ECDSA by 0x40
    switch (Be32(buf)) {
    case 0x10000:
    case 0x10003:
        sig_size = 0x200;
        break;
    case 0x10001:
    case 0x10004:
        sig_size = 0x100;
        break;
    case 0x10002:
    case 0x10005:
        sig_size = 0x3C;
        pad = 0x40;
        break;
    default:
        return facts;
    }
    const u8* body = buf + 4 + sig_size + pad;
    facts.tid = static_cast<u64>(Be32(body + 0x4C)) << 32 | Be32(body + 0x50);
    facts.version = static_cast<u16>(body[0x9C] << 8 | body[0x9D]);
    facts.contents = static_cast<u16>(body[0x9E] << 8 | body[0x9F]);
    const u32 tmd_size = Le32(head + 16), meta = Le32(head + 20);
    facts.end = tmd + Align64(tmd_size) + Align64(Le64(head + 0x18)) + meta;
    facts.parsed = facts.contents != 0 && facts.tid != 0 && facts.end <= file_size + 0x40;
    return facts;
}

// In memory (the tests, and anything that already holds the bytes).
inline CiaFacts InspectCiaBytes(const u8* data, std::size_t size) {
    return InspectCia(
        [&](u64 off, u8* buf, std::size_t len) {
            if (off + len > size) {
                return false;
            }
            std::memcpy(buf, data + off, len);
            return true;
        },
        size);
}

enum class Verdict {
    Good,
    Missing,      // no such file
    BadSize,      // empty, or bigger than a package can be
    NotCia,       // it does not read as a CIA, or its header says more than the file holds
    WrongTitle,   // a CIA of another title
    WrongVersion  // the right title of another version of the app
};

// child.cia, the package other consoles download: a title of the Download Play kind (00040001: what dlph_open takes) of THIS version of the app (the composer
// puts the app's version in it). It is not made again until the version changes or the file is missing (COMPOSITORE.md).
inline Verdict JudgeChild(bool exists, u64 size, const CiaFacts& facts, unsigned version16) {
    if (!exists) {
        return Verdict::Missing;
    }
    if (size == 0 || size > kMaxChildBytes) {
        return Verdict::BadSize;
    }
    if (!facts.parsed) {
        return Verdict::NotCia;
    }
    if ((facts.tid >> 32) != 0x00040001u) {
        return Verdict::WrongTitle;
    }
    return facts.version == version16 ? Verdict::Good : Verdict::WrongVersion;
}

// 3dslink.cia, the app's own CIA that the installer of the child leaves (or a user puts) in the dlp folder: the app's title, THIS version.
inline Verdict JudgeAppCia(bool exists, u64 size, const CiaFacts& facts, unsigned version16) {
    if (!exists) {
        return Verdict::Missing;
    }
    if (size == 0 || size > kMaxAppCiaBytes) {
        return Verdict::BadSize;
    }
    if (!facts.parsed) {
        return Verdict::NotCia;
    }
    if (facts.tid != kAppTid) {
        return Verdict::WrongTitle;
    }
    return facts.version == version16 ? Verdict::Good : Verdict::WrongVersion;
}

// ---------------------------------------------------------------------------------------------
// Where the composer takes the app's CIA from
// ---------------------------------------------------------------------------------------------

enum class Source {
    File,     // <data folder>/dlp/3dslink.cia is a good CIA of the app (the installer of the child leaves it; a user may put it)
    Title,    // the virtual CIA of the installed title (needs boot9.bin and this console's movable.sed, through the scan's context)
    WaitScan, // the file is no good and the key comes from the scan of the titles, which is not done
    None      // neither: the page says what to do
};

inline Source ChooseSource(Verdict app_file, bool scan_done, bool keys) {
    if (app_file == Verdict::Good) {
        return Source::File;
    }
    if (!scan_done) {
        return Source::WaitScan;
    }
    return keys ? Source::Title : Source::None;
}

// When there is no source at all, what the page names first: a file that is there and cannot be used (damaged, not a CIA, another title or another version) is
// the thing to look at, and a page that only said "the key is missing" would leave the user looking at the wrong thing. A file that is not there (or a good one
// that the composer refused: the page knows that by itself) is the plain case.
inline bool FileIsToBlame(Verdict app_file) {
    return app_file != Verdict::Missing && app_file != Verdict::Good;
}

// The card needs the finished package, the copy of the app's CIA when it is made from the installed title, and room for the card's own bookkeeping.
constexpr u64 kSpareBytes = 2ull << 20;
inline u64 SpaceNeeded(bool copy_of_the_app, u64 app_bytes, u64 child_bytes) {
    return child_bytes + (copy_of_the_app ? app_bytes : 0) + kSpareBytes;
}

// ---------------------------------------------------------------------------------------------
// The time
// ---------------------------------------------------------------------------------------------

// What a stage does, by kind of work, in bytes (the composer's dlpc_stage_work says it for its stages; the copy of the app's CIA is ours).
struct Work {
    u64 sd_read = 0;
    u64 sd_write = 0;
    u64 sha = 0;
    u64 aes = 0;
};

// The speeds the time is worked out with, in MiB a second: what the console does in software (AES-CTR and SHA-256 of the module's own code) and with the card.
// They are the figures measured on the consoles, taken a little low: a time that is longer than the work is better than one that is shorter.
struct Speeds {
    double card_read = 3.0;
    double card_write = 3.0;
    double sha = 2.0;
    double aes = 0.7;
};

// The stages in the order they run: 0 is ours (the app's CIA is copied from its installation), 1..6 are the composer's (DLPC_STAGE_*).
constexpr int kStageCopy = 0;
constexpr int kStages = 7;

struct Plan {
    double seconds[kStages] = {};
    double total = 0.0;
};

constexpr double kMiBd = 1048576.0;

inline double WorkSeconds(const Work& w, const Speeds& s) {
    return (static_cast<double>(w.sd_read) / s.card_read + static_cast<double>(w.sd_write) / s.card_write + static_cast<double>(w.sha) / s.sha +
            static_cast<double>(w.aes) / s.aes) /
           kMiBd;
}

// The copy of the app's CIA from its installation: read from the card, decrypted from the SD layer (AES), written to the card, and then composed from there.
inline Work CopyWork(u64 app_bytes) {
    Work w;
    w.sd_read = app_bytes;
    w.sd_write = app_bytes;
    w.aes = app_bytes;
    return w;
}

// The plan from what each stage does: `work[kStageCopy]` is the copy (all zero when there is none), `work[1..6]` the composer's.
inline Plan MakePlanFromWork(const Work (&work)[kStages], const Speeds& speed = Speeds()) {
    Plan plan;
    for (int i = 0; i < kStages; ++i) {
        plan.seconds[i] = WorkSeconds(work[i], speed);
        plan.total += plan.seconds[i];
    }
    return plan;
}

// The same from the sizes alone, when the composer cannot say what its stages do: the RomFS holds the app and the plugin; the package is `child_bytes`
// (dlpc_output_size). The stages read and hash the app and the plugin, build the hash tree, encrypt and hash, write, read back and check.
inline Plan MakePlan(bool copy, u64 app_bytes, u64 plugin_bytes, u64 child_bytes, const Speeds& speed = Speeds()) {
    const u64 rom = app_bytes + plugin_bytes;
    Work work[kStages];
    if (copy) {
        work[kStageCopy] = CopyWork(app_bytes);
    }
    work[DLPC_STAGE_READ_APP].sd_read = app_bytes;
    work[DLPC_STAGE_READ_APP].sha = app_bytes;
    work[DLPC_STAGE_READ_PLUGIN].sha = plugin_bytes; // (the plugin is a resource of the app: hashed, but not read from the card)
    work[DLPC_STAGE_TREE].sha = rom;
    work[DLPC_STAGE_SEAL].aes = rom;
    work[DLPC_STAGE_SEAL].sha = rom;
    work[DLPC_STAGE_WRITE].sd_write = child_bytes;
    work[DLPC_STAGE_VERIFY].sd_read = child_bytes;
    work[DLPC_STAGE_VERIFY].sha = child_bytes;
    work[DLPC_STAGE_VERIFY].aes = child_bytes;
    return MakePlanFromWork(work, speed);
}

// A job that only counts: the composer lays the package out from the model, the sizes of the two payloads and the manifest, which says the version (so
// `version_text` must be there and agree with `version16`: without it the composer says it cannot lay the job out and every size it gives is 0). No callback, no
// path: dlpc_output_size, dlpc_memory_needed and dlpc_stage_work do no I/O.
inline dlpc_job SizesJob(const u8* model, std::size_t model_size, u64 app_bytes, u64 plugin_bytes, unsigned version16, const char* version_text) {
    dlpc_job job;
    std::memset(&job, 0, sizeof(job));
    job.model = model;
    job.model_size = model_size;
    job.app_tid = kAppTid;
    job.version16 = static_cast<std::uint16_t>(version16);
    job.version_text = version_text;
    job.app_size = app_bytes;
    job.plugin_size = plugin_bytes;
    return job;
}

// The plan for a job (SizesJob or the real one): the composer says what each of its stages does (dlpc_stage_work: bytes of card read and written, of SHA-256 and
// of AES) and the time is those over the speeds of the console. `copy`: the app's CIA is first copied from its installation. When the composer cannot say (the job
// cannot be laid out), the plan is worked out from the sizes alone, with the parts the package is made of.
inline Plan PlanFor(const dlpc_job& job, bool copy, const Speeds& speed = Speeds()) {
    Work work[kStages];
    bool known = false;
    for (int stage = DLPC_STAGE_READ_APP; stage <= DLPC_STAGE_VERIFY; ++stage) {
        const dlpc_work w = dlpc_stage_work(&job, stage);
        work[stage].sd_read = w.sd_read;
        work[stage].sd_write = w.sd_write;
        work[stage].sha = w.sha;
        work[stage].aes = w.aes;
        known = known || w.sd_read + w.sd_write + w.sha + w.aes != 0;
    }
    work[DLPC_STAGE_READ_PLUGIN].sd_read = 0; // (the composer counts a plugin read from a file: ours is a resource of the app, in memory, and costs no card)
    if (copy) {
        work[kStageCopy] = CopyWork(job.app_size);
    }
    if (known) {
        return MakePlanFromWork(work, speed);
    }
    const u64 child = dlpc_output_size(&job);
    return MakePlan(copy, job.app_size, job.plugin_size, child != 0 ? child : job.model_size + job.app_size + job.plugin_size, speed);
}

// How far the work is, 0..0.99 (the bar is never full before the end: a stage that is slower than the plan must not leave it at 100% while it works). `stage` is
// 0 (the copy) or a DLPC_STAGE_*; `done` of `total` bytes of that stage.
inline double Fraction(const Plan& plan, int stage, u64 done, u64 total) {
    if (plan.total <= 0.0 || stage < 0 || stage >= kStages) {
        return 0.0;
    }
    double before = 0.0;
    for (int i = 0; i < stage; ++i) {
        before += plan.seconds[i];
    }
    double within = 0.0;
    if (total > 0) {
        within = plan.seconds[stage] * (done >= total ? 1.0 : static_cast<double>(done) / static_cast<double>(total));
    }
    const double f = (before + within) / plan.total;
    return f < 0.0 ? 0.0 : (f > 0.99 ? 0.99 : f);
}

// The plan's speeds are guesses (nobody has measured them on a console); the pace of the work itself is trusted once a seventh of it is done and two seconds
// have passed (a clock that has run for less says nothing). `elapsed_s` is the time since the plan began: when the plan is made again (the file route that the
// composer refused, and the copy from the installed title that follows) the clock starts again with it.
inline bool PaceKnown(double fraction, double elapsed_s) {
    return fraction >= 0.15 && elapsed_s >= 2.0;
}

// How long the whole work takes: the plan's total until the pace is known, and the pace's (the time so far over the part done) after that.
inline double WholeSeconds(const Plan& plan, double fraction, double elapsed_s) {
    return PaceKnown(fraction, elapsed_s) ? elapsed_s / fraction : plan.total;
}

// Seconds left, from the plan until a seventh of the work is done and from the pace measured so far after that (the stages differ in speed, the plan weighs them;
// the measured pace corrects the plan's speeds, which are those of an average card). At least 1 while the work goes on; -1 = nothing to say.
inline int SecondsLeft(const Plan& plan, double fraction, double elapsed_s) {
    if (plan.total <= 0.0) {
        return -1;
    }
    const double left = WholeSeconds(plan, fraction, elapsed_s) * (1.0 - fraction);
    return left < 1.0 ? 1 : static_cast<int>(left + 0.5);
}

// ---------------------------------------------------------------------------------------------
// The marker: written before the child is built and removed after (win or lose). One that is there when the page opens means the last run died in the middle.
// ---------------------------------------------------------------------------------------------

inline std::string MarkerText(int attempt, const std::string& version) {
    return "attempt=" + std::to_string(attempt) + "\nversion=" + version + "\n";
}

// How many runs in a row have started and not ended, counting the one the marker was written for (a marker that cannot be read counts as one).
inline int MarkerAttempts(const std::string& text) {
    const std::size_t at = text.find("attempt=");
    if (at == std::string::npos) {
        return 1;
    }
    const long n = std::strtol(text.c_str() + at + 8, nullptr, 10);
    return n < 1 ? 1 : (n > 99 ? 99 : static_cast<int>(n));
}

// ---------------------------------------------------------------------------------------------
// What a failure asks of the user
// ---------------------------------------------------------------------------------------------

// Our own results, beside the composer's (dlpc_result): the copy of the app's CIA and what is checked before the composer starts.
constexpr int kPrepNoCopy = 100;      // the installed 3DS Link could not be read or copied
constexpr int kPrepAppVersion = 101;  // the installed 3DS Link is not this version
constexpr int kPrepNoSpace = 102;     // the card has no room for the work
constexpr int kPrepNoMemory = 103;    // the heap has no room for the composer
constexpr int kPrepNoThread = 104;    // the thread of the work could not be made
constexpr int kPrepNoModel = 105;     // this build has no installer model (the placeholder is in data/)

// The composer's own words, or ours, for a result: a phrase, with no capital and no full stop (dlpc_result_text's form).
inline const char* PrepPhrase(int result) {
    switch (result) {
    case kPrepNoCopy:
        return "the installed 3DS Link could not be read";
    case kPrepAppVersion:
        return "the installed 3DS Link is not the version of this app";
    case kPrepNoSpace:
        return "the SD card has no room for the package";
    case kPrepNoMemory:
        return "there is not enough memory";
    case kPrepNoThread:
        return "the work could not be started";
    case kPrepNoModel:
        return "this build has no installer";
    default:
        return nullptr; // (a dlpc_result: dlpc_result_text has it)
    }
}

// What to do about it, one sentence.
inline const char* PrepAdvice(int result) {
    switch (result) {
    case DLPC_E_MODEL:
    case kPrepNoModel:
        return "This build of 3DS Link cannot make the package. Install the 3DS Link CIA again.";
    case DLPC_E_VERSION:
        return "The installer inside this app is from another build than the app. Install the 3DS Link CIA again.";
    case DLPC_E_READ_APP:
    case kPrepNoCopy:
        return "Check that the SD card is not locked, and try again.";
    case DLPC_E_READ_PLUGIN:
        return "The plugin inside this app could not be read. Install the 3DS Link CIA again.";
    case DLPC_E_APP_BAD:
    case kPrepAppVersion:
        return "Put the 3DSLink.cia of this version in the dlp folder of 3DS Link, or try again.";
    case DLPC_E_PLUGIN_BAD:
        return "The plugin inside this app is not a good one. Install the 3DS Link CIA again.";
    case DLPC_E_NOMEM:
    case kPrepNoMemory:
    case kPrepNoThread:
        return "Close 3DS Link, open it again, and try once more.";
    case DLPC_E_TOOBIG:
        return "The package would be too big for Download Play (32 MiB).";
    case DLPC_E_WRITE:
    case kPrepNoSpace:
        return "Is the SD card full or locked? Free some space and try again.";
    case DLPC_E_VERIFY:
        return "The file that was written did not check out, so it was removed. Try again; if it repeats, the card may be failing.";
    case DLPC_E_CANCEL:
        return "It was stopped: nothing was changed.";
    default:
        return "Try again.";
    }
}

// Whether trying again can come out differently: a build that has no installer, an installer or a plugin of another build, or a package that cannot fit stay as they are.
inline bool CanTryAgain(int result) {
    return result != DLPC_E_MODEL && result != DLPC_E_VERSION && result != DLPC_E_TOOBIG && result != kPrepNoModel && result != DLPC_E_CANCEL &&
           result != DLPC_E_PLUGIN_BAD && result != DLPC_E_READ_PLUGIN; // (the plugin is part of the app: it is the same on the next try)
}

// A phrase with a capital: "reading the plugin" -> "Reading the plugin" (the name of a stage, which dlpc_stage_text gives in the form of a phrase).
inline std::string Capitalized(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') {
        s[0] = static_cast<char>(s[0] - 'a' + 'A');
    }
    return s;
}

// A phrase made a sentence: "the card refused the file" -> "The card refused the file."
inline std::string Sentence(const std::string& phrase) {
    if (phrase.empty()) {
        return std::string();
    }
    std::string s = Capitalized(phrase);
    if (s.back() != '.' && s.back() != '!' && s.back() != '?') {
        s += '.';
    }
    return s;
}

// The sentence for a failed run: the phrase of the result made a sentence, and what the composer or the module said besides (`extra`, "" = nothing). The composer
// often says the phrase again and adds what it found in a bracket ("The app's installation file is not valid for this version (the hash of ... differs)."): the
// card must not read the phrase twice, so the extra that starts with the phrase stands alone (it holds the phrase, once), one that is the phrase says nothing new
// and is dropped, and anything else follows the phrase.
inline std::string FailureSentence(const std::string& phrase, const std::string& extra) {
    const std::string first = Sentence(phrase);
    const std::string more = Sentence(extra);
    if (more.empty()) {
        return first;
    }
    if (first.empty()) {
        return more;
    }
    // Each without its closing mark (the bracket comes before the full stop: "...for this version (why)." starts with "...for this version").
    const std::string a = first.substr(0, first.size() - 1);
    const std::string b = more.substr(0, more.size() - 1);
    if (b.compare(0, a.size(), a) == 0) {
        return more; // (the extra says the phrase, and perhaps more: it stands alone)
    }
    if (a.compare(0, b.size(), b) == 0) {
        return first; // (the phrase says everything the extra does)
    }
    return first + " " + more;
}

} // namespace dlp
