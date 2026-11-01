// The decisions and the words of the update from GitHub (ConfigApp/source/update_logic.hpp, the very file, plain C++): the versions, the redirect of GitHub, which file a run replaces,
// the CIA that came (a made-up one, and with AZL_REAL_CIA a real one), the summary out of a CHANGELOG, the clock, the certificate's words and when the question and the popup come.
// The requests themselves are Tests/update_http_test.sh.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "appcfg.hpp"
#include "update_logic.hpp"

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

static bool Parses(const char* text) {
    update::Version v;
    return update::ParseVersion(text, v);
}

static void TestVersions() {
    update::Version v;
    CHECK(update::ParseVersion("1.0.14", v) && v.major == 1 && v.minor == 0 && v.micro == 14);
    CHECK(update::ParseVersion("0.0.0", v) && v.major == 0 && v.micro == 0);
    CHECK(update::ParseVersion("12.34.567", v) && v.major == 12 && v.minor == 34 && v.micro == 567);
    for (const char* bad : {"", ".", "1", "1.0", "1.0.", ".1.0", "1..0", "1.0.0.0", "v1.0.0", "1.0.14-test", "1.0.1a", " 1.0.0", "1.0.0 ", "1.0.1000", "-1.0.0", "1,0,0"}) {
        CHECK(!Parses(bad));
    }
    // Numbers, not letters: 1.0.9 comes before 1.0.13.
    CHECK(update::IsNewer("1.0.13", "1.0.9"));
    CHECK(!update::IsNewer("1.0.9", "1.0.13"));
    CHECK(update::IsNewer("1.1.0", "1.0.15"));
    CHECK(update::IsNewer("2.0.0", "1.63.15"));
    CHECK(!update::IsNewer("1.0.13", "1.0.13"));
    CHECK(!update::IsNewer("1.0.12", "1.1.0"));
    CHECK(!update::IsNewer("1.0.14-test", "1.0.13"));
    CHECK(!update::IsNewer("1.0.14", "not a version"));
    // As DlpLink/tools/version.py puts it in the TMD.
    update::ParseVersion("1.0.13", v);
    CHECK(update::TmdVersion(v) == 1037);
    update::ParseVersion("1.2.3", v);
    CHECK(update::TmdVersion(v) == ((1 << 10) | (2 << 4) | 3));
}

static void TestRedirect() {
    CHECK(update::VersionFromLocation("https://github.com/gionko/3DS-Link/releases/tag/v1.0.13") == "1.0.13");
    CHECK(update::VersionFromLocation("https://github.com/gionko/3DS-Link/releases/tag/v1.0.14-test").empty());
    CHECK(update::VersionFromLocation("https://github.com/gionko/3DS-Link/releases/tag/1.0.13").empty());
    CHECK(update::VersionFromLocation("https://github.com/gionko/3DS-Link/releases").empty()); // (no release at all)
    CHECK(update::VersionFromLocation("https://github.com/someone/else/releases/tag/v9.9.9").empty());
    CHECK(update::VersionFromLocation("https://evil.example/gionko/3DS-Link/releases/tag/v9.9.9").empty());
    CHECK(update::VersionFromLocation("http://github.com/gionko/3DS-Link/releases/tag/v9.9.9").empty());
    CHECK(update::VersionFromLocation("").empty());
    CHECK(update::AssetUrl("1.0.14", update::Target::Cia) == "https://github.com/gionko/3DS-Link/releases/download/v1.0.14/3DSLink.cia");
    CHECK(update::AssetUrl("1.0.14", update::Target::File3dsx) == "https://github.com/gionko/3DS-Link/releases/download/v1.0.14/3DSLink.3dsx");
    CHECK(update::ChangelogUrl("1.0.14") == "https://raw.githubusercontent.com/gionko/3DS-Link/v1.0.14/CHANGELOG.md");
}

static void TestTarget() {
    using about::Origin;
    using update::Target;
    CHECK(update::ChooseTarget(Origin::Installed, "") == Target::Cia);
    CHECK(update::ChooseTarget(Origin::File, "sdmc:/3ds/3DSLink/3DSLink.3dsx") == Target::File3dsx);
    CHECK(update::ChooseTarget(Origin::File, "sdmc:/3ds/my.name.3dsx") == Target::File3dsx);
    CHECK(update::ChooseTarget(Origin::File, "3dslink:/3DSLink.3dsx") == Target::None); // (a network load: there is no file to replace)
    CHECK(update::ChooseTarget(Origin::File, "sdmc:/3ds/3DSLink.elf") == Target::None);
    CHECK(update::ChooseTarget(Origin::File, "sdmc:/.3dsx") == Target::File3dsx);
    CHECK(update::ChooseTarget(Origin::NoPath, "") == Target::None);
    CHECK(update::ChooseTarget(Origin::NoPath, "sdmc:/3ds/3DSLink.3dsx") == Target::None);
}

// A CIA with nothing but the fields that are read.
static std::vector<std::uint8_t> MakeCia(std::uint32_t certs, std::uint32_t ticket, std::uint32_t sig_type, std::uint32_t header, std::uint64_t tid, std::uint16_t version) {
    const auto align = [](std::uint64_t v) { return (v + 63) / 64 * 64; };
    const std::uint64_t tmd_at = align(align(align(0x2020) + certs) + ticket);
    const std::uint32_t tmd_size = header + 0xC4 + 0x30;
    std::vector<std::uint8_t> cia(tmd_at + tmd_size);
    const auto le32 = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            cia[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    };
    le32(0, 0x2020);
    le32(8, certs);
    le32(0xC, ticket);
    le32(0x10, tmd_size);
    for (int i = 0; i < 4; ++i) {
        cia[tmd_at + i] = static_cast<std::uint8_t>(sig_type >> (24 - 8 * i));
    }
    for (int i = 0; i < 8; ++i) {
        cia[tmd_at + header + 0x4C + i] = static_cast<std::uint8_t>(tid >> (56 - 8 * i));
    }
    cia[tmd_at + header + 0x9C] = static_cast<std::uint8_t>(version >> 8);
    cia[tmd_at + header + 0x9D] = static_cast<std::uint8_t>(version);
    return cia;
}

static void TestCia() {
    const std::uint64_t tid = 0x000400000F83D100ull;
    update::CiaIdentity id;
    std::vector<std::uint8_t> cia = MakeCia(0xA00, 0x350, 0x00010004, 0x140, tid, 1037);
    CHECK(update::ReadCiaIdentity(cia.data(), cia.size(), id) && id.title_id == tid && id.version == 1037);
    cia = MakeCia(0xA01, 0x351, 0x00010003, 0x240, tid, 7); // (sizes that are not multiples of 64, the other kinds of signature)
    CHECK(update::ReadCiaIdentity(cia.data(), cia.size(), id) && id.title_id == tid && id.version == 7);
    cia = MakeCia(0, 0, 0x00010005, 0x80, tid, 9);
    CHECK(update::ReadCiaIdentity(cia.data(), cia.size(), id) && id.version == 9);

    // What is not a whole CIA, at every length; nothing is read outside the bytes (the test runs with the address sanitizer).
    cia = MakeCia(0xA00, 0x350, 0x00010004, 0x140, tid, 1037);
    const std::size_t whole_up_to = cia.size() - 0x30; // (the header ends here; the rest of the TMD is not read)
    for (std::size_t n = 0; n < whole_up_to; n += 37) {
        std::vector<std::uint8_t> cut(cia.begin(), cia.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(!update::ReadCiaIdentity(cut.data(), cut.size(), id));
    }
    CHECK(!update::ReadCiaIdentity(nullptr, 0, id));
    std::vector<std::uint8_t> wrong = cia;
    wrong[0] = 0x21; // (another header size)
    CHECK(!update::ReadCiaIdentity(wrong.data(), wrong.size(), id));
    wrong = MakeCia(0xA00, 0x350, 0x00010009, 0x140, tid, 1); // (an unknown signature)
    CHECK(!update::ReadCiaIdentity(wrong.data(), wrong.size(), id));
    wrong = cia;
    for (int i = 0; i < 4; ++i) {
        wrong[8 + i] = 0xFF; // (sizes that point past the end)
    }
    CHECK(!update::ReadCiaIdentity(wrong.data(), wrong.size(), id));
    wrong = cia;
    for (int i = 0; i < 4; ++i) {
        wrong[0x10 + i] = 0; // (a TMD of size 0)
    }
    CHECK(!update::ReadCiaIdentity(wrong.data(), wrong.size(), id));

    const std::uint8_t good[] = {'3', 'D', 'S', 'X', 0x20};
    const std::uint8_t bad[] = {'3', 'D', 'S', 'Y', 0x20};
    CHECK(update::Looks3dsx(good, sizeof(good)));
    CHECK(!update::Looks3dsx(bad, sizeof(bad)));
    CHECK(!update::Looks3dsx(good, 4));
    CHECK(!update::Looks3dsx(nullptr, 0));

    if (const char* path = std::getenv("AZL_REAL_CIA")) { // (Tests/update_http_test.sh: the CIA of a release, as GitHub gave it)
        if (std::FILE* f = std::fopen(path, "rb")) {
            std::vector<std::uint8_t> real(4 * 1024 * 1024);
            real.resize(std::fread(real.data(), 1, real.size(), f));
            std::fclose(f);
            CHECK(update::ReadCiaIdentity(real.data(), real.size(), id) && id.title_id == tid && id.version != 0);
        }
    }
}

static void TestWhatsNew() {
    const std::string log =
        "# Changelog\n\n"
        "## 1.0.14 - 2026-11-02\r\n\r\n"
        "**The update comes by *itself*. See `Settings`.**\r\n\r\n"
        "### At a glance\n\n| a | b |\n\n"
        "## 1.0.13 - 2026-11-01\n\n"
        "**Monster Hunter XX no longer closes at launch.**\n\n"
        "> a note\n\n"
        "## 1.0.1\n\n"
        "### At a glance\n\n"
        "## 1.0.0 - 2026-10-21\n\n";
    CHECK(update::WhatsNew(log, "1.0.14") == "The update comes by itself. See Settings.");
    CHECK(update::WhatsNew(log, "1.0.13") == "Monster Hunter XX no longer closes at launch.");
    CHECK(update::WhatsNew(log, "1.0.1").empty());   // (a section with no sentence, and not the one of 1.0.13 or 1.0.14 that starts alike)
    CHECK(update::WhatsNew(log, "1.0.0").empty());   // (nothing under it)
    CHECK(update::WhatsNew(log, "1.0.15").empty());
    CHECK(update::WhatsNew("", "1.0.14").empty());
    const std::string long_line = "## 2.0.0\n\n" + std::string(100, 'a') + std::string(300, ' ') + "b\n";
    CHECK(update::WhatsNew(long_line, "2.0.0").size() <= 283);
    std::string many = "## 2.0.0\n\n";
    for (int i = 0; i < 100; ++i) {
        many += "word ";
    }
    const std::string cut = update::WhatsNew(many, "2.0.0");
    CHECK(cut.size() <= 283 && cut.compare(cut.size() - 3, 3, "...") == 0 && cut.find("wor...") == std::string::npos);
}

static void TestClockAndCertificate() {
    CHECK(update::ClockTooOld(2011));
    CHECK(update::ClockTooOld(2025));
    CHECK(update::ClockTooOld(0));
    CHECK(!update::ClockTooOld(2026));
    CHECK(!update::ClockTooOld(2040));
    CHECK(update::ClockPopupDue(true, false));
    CHECK(!update::ClockPopupDue(true, true));  // (once: consoles kept at an old date on purpose are not asked at every start)
    CHECK(!update::ClockPopupDue(false, false));
    CHECK(update::CertificateFailure("Cert verify failed: BADCERT_FUTURE") == update::NetResult::Clock);
    CHECK(update::CertificateFailure("Cert verify failed: BADCERT_EXPIRED") == update::NetResult::Clock);
    CHECK(update::CertificateFailure("Cert verify failed: BADCERT_NOT_TRUSTED") == update::NetResult::Certificate);
    CHECK(update::CertificateFailure("Cert verify failed: BADCERT_CN_MISMATCH") == update::NetResult::Certificate);
    CHECK(update::CertificateFailure("") == update::NetResult::Certificate);
}

static void TestAsking() {
    CHECK(update::AskAtStart("1.0.14", "1.0.13", ""));
    CHECK(!update::AskAtStart("1.0.14", "1.0.13", "1.0.14")); // "Not now" holds for that version ...
    CHECK(update::AskAtStart("1.0.15", "1.0.13", "1.0.14"));  // ... and not for the next one
    CHECK(!update::AskAtStart("1.0.13", "1.0.13", ""));
    CHECK(!update::AskAtStart("1.0.12", "1.0.13", ""));
    CHECK(!update::AskAtStart("", "1.0.13", ""));
}

// The console's font has no tilde, and the texts are read on a small screen.
static void CheckWords(const std::string& text) {
    CHECK(!text.empty());
    CHECK(text.find('~') == std::string::npos);
    for (const char c : text) {
        CHECK(c == '\n' || (c >= ' ' && c < 127));
    }
}

static void TestWords() {
    for (const char* text : {update::kCheckRowNote, update::kAtStartRowNote, update::kClockText, update::kWrongFileText, update::kBusyText}) {
        CheckWords(text);
    }
    for (const update::NetResult r : {update::NetResult::Offline, update::NetResult::Clock, update::NetResult::Certificate, update::NetResult::Server, update::NetResult::TooBig,
                                      update::NetResult::NoMemory, update::NetResult::Failed}) {
        CheckWords(update::NetFailureText(r));
    }
    CHECK(std::strlen(update::NetFailureText(update::NetResult::Ok)) == 0);
    CHECK(std::strlen(update::NetFailureText(update::NetResult::Cancelled)) == 0);
    CheckWords(update::PromptText("1.0.14", "1.0.13"));
    CHECK(update::PromptText("1.0.14", "1.0.13").find("v1.0.14") != std::string::npos && update::PromptText("1.0.14", "1.0.13").find("v1.0.13") != std::string::npos);
    CheckWords(update::UpToDateText("1.0.13"));
    CheckWords(update::NoFileText("1.0.14"));
    CheckWords(update::InstallFailureText(0xC8A044DCu));
    CHECK(update::InstallFailureText(0xC8A044DCu).find("0xC8A044DC") != std::string::npos);
    CheckWords(update::FileFailureText("the old file could not be replaced"));
    // Whoever updates is told about the plugin: the whole point of waiting for the app's message.
    CHECK(update::PromptTopText("").find("plugin") != std::string::npos);
    CHECK(update::PromptTopText("New things.").compare(0, 11, "New things.") == 0);
    for (const update::Target t : {update::Target::Cia, update::Target::File3dsx}) {
        CheckWords(update::DoneText(t, "1.0.14"));
        CHECK(update::DoneText(t, "1.0.14").find("plugin") != std::string::npos);
        CHECK(update::DoneText(t, "1.0.14").find("v1.0.14") != std::string::npos);
    }
    CHECK(update::DoneText(update::Target::Cia, "1.0.14").find("HOME Menu") != std::string::npos);
    CHECK(update::DoneText(update::Target::File3dsx, "1.0.14").find("Homebrew Launcher") != std::string::npos);
}

static void TestConfig() {
    setup::AppConfig cfg = setup::ParseAppConfig("first_start_done=1\n");
    CHECK(cfg.update_check && cfg.update_not_now.empty() && !cfg.clock_warned); // an older file: the check is on
    cfg = setup::ParseAppConfig("update_check=0\nupdate_not_now=1.0.14\nclock_warned=1\n");
    CHECK(!cfg.update_check && cfg.update_not_now == "1.0.14" && cfg.clock_warned);
    const setup::AppConfig again = setup::ParseAppConfig(setup::SerialiseAppConfig(cfg));
    CHECK(!again.update_check && again.update_not_now == "1.0.14" && again.clock_warned);
    cfg = setup::ParseAppConfig("update_check=maybe\n");
    CHECK(cfg.update_check); // (a slip is the default)
    setup::AppConfig plain;
    const std::string text = setup::SerialiseAppConfig(plain);
    CHECK(text.find("update_check=1") != std::string::npos);
    CHECK(text.find("update_not_now") == std::string::npos && text.find("clock_warned") == std::string::npos);
}

int main() {
    TestVersions();
    TestRedirect();
    TestTarget();
    TestCia();
    TestWhatsNew();
    TestClockAndCertificate();
    TestAsking();
    TestWords();
    TestConfig();
    std::printf("update_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
