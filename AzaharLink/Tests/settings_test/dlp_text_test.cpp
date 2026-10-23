// The words and the small decisions of 3DS Link Delivery (ConfigApp/source/dlp_text.hpp, the very file, plain C++), run by Tests/run.sh. What it
// pins: the version of the child as the TMD holds it, the name of a console the library has not read yet, what each end of a visit is called
// and how it is coloured (a console that already had the app is never "Delivered"), the time left while the library does not know it, the address
// of this console (all zero would make no console see the room), the question before closing (only with a console in the room), and that every
// way the room can fail has a title and a text.
#include <cstdio>
#include <cstring>
#include <string>

#include "dlp_text.hpp"

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                          \
    } while (0)

// A rough model of gfx::WrapText for the tests of what a note holds: the paragraphs are cut at every '\n' (an empty one is a blank line, which takes a line of its
// own) and the words are packed greedily into lines of `cols` characters. How many lines the text takes; a word longer than a line takes a line by itself.
static int WrapLines(const std::string& text, std::size_t cols) {
    int lines = 0;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', pos);
        const std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (para.empty()) {
            ++lines;
        } else {
            std::size_t used = 0;
            bool open = false;
            std::size_t at = 0;
            while (at < para.size()) {
                const std::size_t sp = para.find(' ', at);
                const std::size_t len = (sp == std::string::npos ? para.size() : sp) - at;
                if (len > 0) {
                    if (!open) {
                        ++lines;
                        open = true;
                        used = len;
                    } else if (used + 1 + len <= cols) {
                        used += 1 + len;
                    } else {
                        ++lines;
                        used = len;
                    }
                }
                at = sp == std::string::npos ? para.size() : sp + 1;
            }
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return lines;
}

static void TestVersion() {
    CHECK(dlp::VersionText(1024) == "1.0.0"); // the first version: 1 << 10
    CHECK(dlp::VersionText((1u << 10) | (2u << 4) | 3u) == "1.2.3");
    CHECK(dlp::VersionText((2u << 10) | (15u << 4) | 15u) == "2.15.15");
    CHECK(dlp::VersionText(0).empty()); // no child read: nothing to say
}

static void TestConsoleName() {
    CHECK(dlp::ConsoleName("Anna", 2) == "Anna");
    CHECK(dlp::ConsoleName("", 2) == "Console (seat 2)"); // the first 26-70 ms after it came in
    CHECK(dlp::ConsoleName(nullptr, 3) == "Console (seat 3)");
    CHECK(dlp::ConsoleName("", 0) == "Console");
}

static void TestVisits() {
    CHECK(std::strcmp(dlp::VisitWord(0, false), "Delivered") == 0);
    CHECK(std::strcmp(dlp::VisitWord(1, false), "Lost") == 0);
    CHECK(std::strcmp(dlp::VisitWord(2, false), "Sent away") == 0);
    CHECK(std::strcmp(dlp::VisitWord(9, false), "?") == 0);
    // a console that already had this version received nothing: whatever the library called it, the screen never says "Delivered"
    for (int result = 0; result <= 2; ++result) {
        CHECK(std::strcmp(dlp::VisitWord(result, true), "Had it already") == 0);
        CHECK(dlp::VisitTone(result, true) == dlp::Tone::Dim);
    }
    CHECK(dlp::VisitTone(0, false) == dlp::Tone::Good);
    CHECK(dlp::VisitTone(1, false) == dlp::Tone::Bad);
    CHECK(dlp::VisitTone(2, false) == dlp::Tone::Warn);
}

static void TestNumbers() {
    CHECK(dlp::TimeLeftText(-1) == "working out the time"); // the library does not know it for the first second and a half
    CHECK(dlp::TimeLeftText(0) == "about 1 s left");
    CHECK(dlp::TimeLeftText(8) == "about 8 s left");
    CHECK(dlp::TimeLeftText(89) == "about 89 s left");
    CHECK(dlp::TimeLeftText(90) == "about 2 min left");
    CHECK(dlp::TimeLeftText(600) == "about 10 min left");

    CHECK(dlp::DurationText(0) == "0 s");
    CHECK(dlp::DurationText(12400) == "12 s");
    CHECK(dlp::DurationText(59600) == "1 min 00 s"); // (it rounds to the second first: 60 s is a minute)
    CHECK(dlp::DurationText(65000) == "1 min 05 s");
    CHECK(dlp::DurationText(3723000) == "1 h 02 min");

    CHECK(dlp::SpeedText(0.456f) == "0.46 MiB/s");
    CHECK(dlp::SpeedText(-1.0f) == "0.00 MiB/s");
    CHECK(dlp::RepeatedText(3, 182) == "3 of 182 packets sent again");
}

static void TestMac() {
    const unsigned char good[6] = {0xDC, 0x68, 0xEB, 0x00, 0x00, 0x01};
    const unsigned char zero[6] = {0, 0, 0, 0, 0, 0};
    const unsigned char ones[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char one_zero[6] = {0, 0, 0, 0, 0, 1};
    CHECK(dlp::MacUsable(good));
    CHECK(!dlp::MacUsable(zero));
    CHECK(!dlp::MacUsable(ones));
    CHECK(dlp::MacUsable(one_zero));
    CHECK(dlp::MacText(good) == "DC:68:EB:00:00:01");
}

static void TestClose() {
    CHECK(!dlp::CloseNeedsQuestion(0)); // nobody in the room: nobody is cut off
    for (int seat = 2; seat <= 8; ++seat) {
        CHECK(dlp::CloseNeedsQuestion(seat));
    }
    CHECK(std::strcmp(dlp::kCloseQuestion, "A console that has not finished will stop receiving.") == 0);
}

static void TestFailures() {
    const dlp::Failure all[] = {dlp::Failure::NoChild, dlp::Failure::BadChild, dlp::Failure::NoMac, dlp::Failure::Wireless,
                                dlp::Failure::Library, dlp::Failure::Room,     dlp::Failure::Thread};
    for (const dlp::Failure f : all) {
        CHECK(std::strlen(dlp::FailureTitle(f)) > 0);
        CHECK(!dlp::FailureText(f, "").empty());
        const std::string with = dlp::FailureText(f, "sdmc:/x/child.cia");
        CHECK(with.size() > dlp::FailureText(f, "").size() && with.find("sdmc:/x/child.cia") != std::string::npos);
        CHECK(with.back() != ' ' && with.front() != ' ');
    }
    // the ways the package that is handed out can fail to be there: each has a title and a text, and none is silent
    const dlp::Failure before[] = {dlp::Failure::SwitchedOff, dlp::Failure::NoBoot9, dlp::Failure::NoModel,    dlp::Failure::NoPlugin,
                                   dlp::Failure::NoSource,    dlp::Failure::NoKeys,  dlp::Failure::BadAppFile, dlp::Failure::Prepare};
    for (const dlp::Failure f : before) {
        CHECK(std::strlen(dlp::FailureTitle(f)) > 0);
        CHECK(dlp::FailureText(f, "the detail of it.").find("the detail of it.") != std::string::npos);
        CHECK(dlp::FailureText(f, "x").back() != ' ' && dlp::FailureText(f, "x").front() != ' ');
    }
    CHECK(dlp::FailureText(dlp::Failure::SwitchedOff, "").find("Settings") != std::string::npos);
    // the plugin is inside the app: a build without it says that, and the way out is the CIA (no file is asked for "next to 3DS Link")
    CHECK(std::string(dlp::FailureTitle(dlp::Failure::NoPlugin)) == "This build has no plugin");
    CHECK(dlp::FailureText(dlp::Failure::NoPlugin, "").find("without the plugin inside") != std::string::npos);
    CHECK(dlp::FailureText(dlp::Failure::NoPlugin, "").find("Install the 3DS Link CIA again") != std::string::npos);
    CHECK(dlp::FailureText(dlp::Failure::NoPlugin, "").find("next to 3DS Link") == std::string::npos && dlp::FailureText(dlp::Failure::NoPlugin, "").find("not found") == std::string::npos);
    // boot9.bin: honest about what it is for, and the key that opens the guide is named
    CHECK(dlp::FailureText(dlp::Failure::NoBoot9, "").find("works without it") != std::string::npos);
    CHECK(dlp::FailureText(dlp::Failure::NoBoot9, "").find("share") != std::string::npos && dlp::FailureText(dlp::Failure::NoBoot9, "").find("Press Y") != std::string::npos);
    // the two ways the app's own copy can be out of reach say exactly what is missing and where to put the file (the folder is part of the sentence)
    const std::string source = dlp::FailureText(dlp::Failure::NoSource, "sdmc:/3ds/3DSLink/dlp");
    CHECK(source.find("3DSLink.cia") != std::string::npos && source.find("sdmc:/3ds/3DSLink/dlp") != std::string::npos && source.find("boot9.bin") != std::string::npos);
    CHECK(dlp::FailureText(dlp::Failure::NoSource, "").find("dlp folder") != std::string::npos);
    const std::string keys = dlp::FailureText(dlp::Failure::NoKeys, "sdmc:/3ds/3DSLink/dlp");
    CHECK(keys.find("movable.sed") != std::string::npos && keys.find("boot9.bin") != std::string::npos && keys.find("key") != std::string::npos);
    CHECK(keys.find("sdmc:/3ds/3DSLink/dlp") != std::string::npos);
    // the file that is there and cannot be used is NAMED (its path, what is wrong with it, its version when it is known) and the two ways out follow; the key is not what
    // the sentence is about (the old words, "the key of the SD card is missing", sent the user to look at the wrong thing)
    const std::string file = "sdmc:/3ds/3DSLink/dlp/3dslink.cia";
    const std::string damaged = dlp::BadAppFileText(file, "", "1.0.0");
    CHECK(damaged.find(file) != std::string::npos && damaged.find("damaged") != std::string::npos && damaged.find("another version") != std::string::npos);
    CHECK(damaged.find("Put the 3DSLink.cia of this version there") != std::string::npos && damaged.find("boot9.bin") != std::string::npos);
    CHECK(damaged.find("missing") == std::string::npos && damaged.find("movable.sed") == std::string::npos);
    const std::string old = dlp::BadAppFileText(file, "0.9.0", "1.0.0");
    CHECK(old.find(file) != std::string::npos && old.find("0.9.0") != std::string::npos && old.find("1.0.0") != std::string::npos && old.find("damaged") == std::string::npos);
    CHECK(old.find("Put the 3DSLink.cia of this version there") != std::string::npos && old.find("boot9.bin") != std::string::npos);
    CHECK(dlp::BadAppFileText(file, "1.0.0", "1.0.0") == damaged);   // (the version it says is this one: it is the composer that refused it, so it is damaged)
    CHECK(dlp::FailureText(dlp::Failure::BadAppFile, old) == old);   // (the page hands over the whole sentence, as it does for Prepare)
    CHECK(std::string(dlp::FailureTitle(dlp::Failure::BadAppFile)).find("3DSLink.cia") != std::string::npos);
    CHECK(std::string(dlp::FailureTitle(dlp::Failure::BadAppFile)) != std::string(dlp::FailureTitle(dlp::Failure::NoKeys)));
    CHECK(dlp::FailureText(dlp::Failure::Prepare, "").empty()); // (the page puts the sentence of the failure in the detail)
    CHECK(dlp::FailureText(dlp::Failure::BadChild, "").find("A makes it again") != std::string::npos);
    CHECK(std::strlen(dlp::FailureTitle(dlp::Failure::None)) == 0 && dlp::FailureText(dlp::Failure::None, "").empty());
    CHECK(dlp::FailureText(dlp::Failure::None, "only this") == "only this"); // (no stray space before a detail that stands alone)
    CHECK(std::string(dlp::FailureTitle(dlp::Failure::NoChild)) == "child.cia not found");
}

static void TestSteps() {
    CHECK(dlp::kStepCount == 5);
    for (int i = 0; i < dlp::kStepCount; ++i) {
        CHECK(!dlp::StepText(i, "Alice").empty());
    }
    CHECK(dlp::StepText(dlp::kStepCount, "x").empty() && dlp::StepText(-1, "x").empty());
    CHECK(dlp::StepText(1, "Alice").find("from Alice") != std::string::npos); // the console to pick in the list
    CHECK(dlp::StepText(1, "").find(" from ") == std::string::npos);              // without a name the sentence still reads
    CHECK(dlp::StepText(0, "").find("Download Play") != std::string::npos);
    CHECK(dlp::StepText(2, "").find("Luma3DS") != std::string::npos);
}

// The entry on the main page: open, switched off by Settings > Download Play delivery, or shut for the lack of boot9.bin (as Titles Delivery's Send Titles is).
static void TestGate() {
    // the decision, for every pair: the switch comes first, then boot9.bin
    CHECK(dlp::GateOf(true, true) == dlp::Gate::Open);
    CHECK(dlp::GateOf(true, false) == dlp::Gate::NoBoot9);
    CHECK(dlp::GateOf(false, true) == dlp::Gate::Off);
    CHECK(dlp::GateOf(false, false) == dlp::Gate::Off); // (switched off, nothing else about the entry is looked at)
    // an open entry says nothing at its right and tells what it does; the shut ones say why
    CHECK(std::strlen(dlp::GateValue(dlp::Gate::Open)) == 0);
    CHECK(std::string(dlp::GateValue(dlp::Gate::Off)) == "switched off" && std::string(dlp::GateValue(dlp::Gate::NoBoot9)) == "needs boot9.bin");
    CHECK(std::string(dlp::GateNoteTitle(dlp::Gate::Open)) == "3DS Link Delivery");
    CHECK(std::string(dlp::GateNoteTitle(dlp::Gate::NoBoot9)) == "boot9.bin is missing"); // (the same words as Send Titles' note)
    CHECK(std::string(dlp::GateNoteTitle(dlp::Gate::Off)).find("switched off") != std::string::npos);
    // the note without boot9.bin: what the file is for (sharing), that 3DS Link works without it, and the key of the guide
    const std::string no_boot9 = dlp::GateNote(dlp::Gate::NoBoot9);
    CHECK(no_boot9.find("boot9.bin") != std::string::npos && no_boot9.find("Sharing 3DS Link needs boot9.bin") != std::string::npos && no_boot9.find("works without it") != std::string::npos);
    CHECK(no_boot9.find("key file of this console") != std::string::npos && no_boot9.find("Press Y") != std::string::npos);
    CHECK(no_boot9.find("\n\nPress Y") != std::string::npos);  // (the Y sentence is a paragraph of its own, last: the part of the note that must stay in view)
    // the note when it is switched off: where, and that nothing of it runs
    const std::string off = dlp::GateNote(dlp::Gate::Off);
    CHECK(off.find("Settings (Download Play delivery)") != std::string::npos && off.find("Nothing of it runs") != std::string::npos);
    CHECK(off.find("read or written") != std::string::npos);
    // the note of an open entry is honest about the first time
    const std::string open = dlp::GateNote(dlp::Gate::Open);
    CHECK(open.find("Download Play") != std::string::npos && open.find("One console at a time") != std::string::npos);
    CHECK(open.find("The first time") != std::string::npos && open.find("how long") != std::string::npos);
    // The notes of the main page are shown whole: the note's box has four lines, and a blank line between two paragraphs is one of them. The emulator's stand-in font is the
    // wider one (a line of the note holds 69-70 characters there; the console's holds more): a line of 60 is the margin, and the Y sentence must still be on the screen.
    for (const dlp::Gate g : {dlp::Gate::Open, dlp::Gate::Off, dlp::Gate::NoBoot9}) {
        CHECK(WrapLines(dlp::GateNote(g), 60) <= 4);
    }
    CHECK(WrapLines(no_boot9.substr(0, no_boot9.find("\n\n")), 60) <= 2);       // (the first paragraph takes no more than two lines: then the blank one, then the Y sentence)
    CHECK(WrapLines(no_boot9.substr(no_boot9.find("\n\n") + 2), 60) == 1);
    CHECK(WrapLines(open.substr(0, open.find("\n\n")), 60) <= 2);
    CHECK(WrapLines(open.substr(open.find("\n\n") + 2), 60) == 1);              // (and the second paragraph one: it is the last line of the four)
    // A on a shut entry answers in one line that points the way
    CHECK(std::string(dlp::GateToast(dlp::Gate::Off)).find("Settings > Download Play delivery") != std::string::npos);
    CHECK(std::string(dlp::GateToast(dlp::Gate::NoBoot9)).find("press Y") != std::string::npos);
    CHECK(std::string(dlp::kGuideHint) == "Y guide" && std::string(dlp::kGuideLabel) == "Guide" && std::strlen(dlp::kNoGuideToast) > 10);
    // Settings: one row, with its explanation
    CHECK(std::string(dlp::kSwitchLabel) == "Download Play delivery");
    CHECK(std::string(dlp::kSwitchNote).find("ON:") != std::string::npos && std::string(dlp::kSwitchNote).find("OFF:") != std::string::npos);
    CHECK(std::string(dlp::kSwitchNote).find("rest of 3DS Link is not affected") != std::string::npos);
    CHECK(std::string(dlp::SwitchToast(true)) != std::string(dlp::SwitchToast(false)));
}

// The preparing screen's words.
static void TestPrepareWords() {
    CHECK(std::string(dlp::kPrepareTitle) == "Preparing the package");
    CHECK(std::string(dlp::kPrepareKeepAwake).find("B stops it") != std::string::npos && std::string(dlp::kPrepareKeepAwake).find("HOME and sleep are off") != std::string::npos);
    CHECK(std::string(dlp::kCancelPrepareQuestion).find("Nothing is left half done") != std::string::npos);
    CHECK(std::string(dlp::kCancelPrepareQuestion).find("Stop now") == std::string::npos); // (the popup's title is "Stop now?": the text does not ask it twice)
    CHECK(std::string(dlp::kCancelPrepareQuestion).find('?') == std::string::npos);
    CHECK(std::string(dlp::kPrepareWaitScanNote).find("key") != std::string::npos && std::strlen(dlp::kPrepareCopyStage) > 10);
}

int main() {
    TestGate();
    TestPrepareWords();
    TestVersion();
    TestConsoleName();
    TestVisits();
    TestNumbers();
    TestMac();
    TestClose();
    TestFailures();
    TestSteps();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
