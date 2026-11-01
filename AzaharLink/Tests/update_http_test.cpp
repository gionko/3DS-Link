// The requests of the update (ConfigApp/source/update_http.cpp, the very file, with libcurl 8.4.0 and mbedTLS 2.28 as on the console). Tests/update_http_test.sh runs it.
//   mock <ca.pem> <first port>   against Tests/update_mock_server.py: redirects, a file cut short, a 404, a file too big, a stop from another thread, a server that goes silent, and
//                                certificates out of their dates, of another host and of another authority
//   github <cacert.pem> ok       against GitHub, with the list of roots the app carries: the whole way of an update
//   github <cacert.pem> clock    the same with a clock that is wrong (the script runs it under faketime): it must say so
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "update_http.hpp"

using namespace update;

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

static std::string ReadFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static double SecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static std::string Url(int port, const char* path) {
    return "https://localhost:" + std::to_string(port) + path;
}

static const char* Name(NetResult r) {
    switch (r) {
    case NetResult::Ok: return "Ok";
    case NetResult::Cancelled: return "Cancelled";
    case NetResult::Offline: return "Offline";
    case NetResult::Clock: return "Clock";
    case NetResult::Certificate: return "Certificate";
    case NetResult::Server: return "Server";
    case NetResult::TooBig: return "TooBig";
    case NetResult::NoMemory: return "NoMemory";
    case NetResult::Failed: return "Failed";
    }
    return "?";
}

#define CHECK_RESULT(expr, want)                                                                   \
    do {                                                                                           \
        const NetResult got_ = (expr);                                                             \
        ++g_checks;                                                                                \
        if (got_ != (want)) {                                                                      \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s gave %s, not %s\n", __FILE__, __LINE__, #expr, Name(got_), Name(want)); \
        }                                                                                          \
    } while (0)

static void RunMock(int port) {
    const int good = port, expired = port + 1, future = port + 2, other_host = port + 3, other_ca = port + 4, silent = port + 5;
    constexpr std::size_t kFileBytes = 1024 * 1024;
    HttpControl control;
    control.connect_seconds = 10;
    control.stall_seconds = 10;
    std::string location;
    ByteBuffer out;

    // The question: one redirect, not followed.
    CHECK_RESULT(GetRedirectTarget(Url(good, "/latest"), control, location), NetResult::Ok);
    CHECK(location == "https://github.com/gionko/3DS-Link/releases/tag/v1.2.3");
    CHECK(VersionFromLocation(location) == "1.2.3");
    CHECK_RESULT(GetRedirectTarget(Url(good, "/file"), control, location), NetResult::Server); // (a 200 is no redirect)
    CHECK(location.empty());
    CHECK_RESULT(GetRedirectTarget(Url(good, "/404"), control, location), NetResult::Server);

    // The file, whole and to the byte, with its progress.
    std::atomic<std::uint32_t> done{0}, total{0};
    control.done = &done;
    control.total = &total;
    CHECK_RESULT(Download(Url(good, "/file"), control, kMaxFileBytes, out), NetResult::Ok);
    CHECK(out.size() == kFileBytes);
    CHECK(done.load() == kFileBytes && total.load() == kFileBytes);
    bool same = out.size() == kFileBytes;
    for (std::size_t i = 0; same && i < kFileBytes; ++i) {
        same = out.data()[i] == static_cast<std::uint8_t>(i * 7);
    }
    CHECK(same);
    control.done = nullptr;
    control.total = nullptr;

    // Redirects: followed, not for ever, and never to plain HTTP.
    CHECK_RESULT(Download(Url(good, "/redirect"), control, kMaxFileBytes, out), NetResult::Ok);
    CHECK(out.size() == kFileBytes);
    CHECK_RESULT(Download(Url(good, "/loop"), control, kMaxFileBytes, out), NetResult::Server);
    CHECK(out.size() == 0);
    CHECK_RESULT(Download(Url(good, "/to-http"), control, kMaxFileBytes, out), NetResult::Server);
    CHECK(out.size() == 0);
    CHECK_RESULT(Download("http://localhost:" + std::to_string(good) + "/file", control, kMaxFileBytes, out), NetResult::Server);

    // What is not the file leaves nothing behind.
    CHECK_RESULT(Download(Url(good, "/404"), control, kMaxFileBytes, out), NetResult::Server);
    CHECK(out.size() == 0);
    CHECK_RESULT(Download(Url(good, "/500"), control, kMaxFileBytes, out), NetResult::Server);
    CHECK_RESULT(Download(Url(good, "/truncated"), control, kMaxFileBytes, out), NetResult::Failed);
    CHECK(out.size() == 0);
    CHECK_RESULT(Download(Url(good, "/file"), control, kFileBytes - 1, out), NetResult::TooBig);
    CHECK(out.size() == 0);
    CHECK_RESULT(Download(Url(good, "/file"), control, kFileBytes, out), NetResult::Ok); // (exactly the limit is allowed)

    // A stop from another thread comes through at once, a server that goes silent is given up.
    {
        std::atomic<bool> cancel{false};
        HttpControl stoppable = control;
        stoppable.cancel = &cancel;
        const auto start = std::chrono::steady_clock::now();
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cancel.store(true);
        });
        CHECK_RESULT(Download(Url(good, "/slow"), stoppable, kMaxFileBytes, out), NetResult::Cancelled);
        stopper.join();
        CHECK(SecondsSince(start) < 5.0);
        CHECK(out.size() == 0);
    }
    {
        HttpControl impatient = control;
        impatient.stall_seconds = 2;
        const auto start = std::chrono::steady_clock::now();
        CHECK_RESULT(Download(Url(good, "/stall"), impatient, kMaxFileBytes, out), NetResult::Offline);
        CHECK(SecondsSince(start) < 15.0); // (curl looks at the speed over about 5 s, so 2 s of silence take about 8 s; the server stays silent for 30)
    }
    {
        HttpControl impatient = control;
        impatient.connect_seconds = 2;
        const auto start = std::chrono::steady_clock::now();
        CHECK_RESULT(GetRedirectTarget(Url(silent, "/latest"), impatient, location), NetResult::Offline); // (TCP answers, TLS never does)
        CHECK(SecondsSince(start) < 8.0);
    }
    {
        HttpControl total_limit = control;
        total_limit.total_seconds = 2;
        const auto start = std::chrono::steady_clock::now();
        CHECK_RESULT(Download(Url(good, "/slow"), total_limit, kMaxFileBytes, out), NetResult::Offline);
        CHECK(SecondsSince(start) < 8.0);
    }

    // Nobody there.
    CHECK_RESULT(GetRedirectTarget(Url(port + 9, "/latest"), control, location), NetResult::Offline);
    CHECK_RESULT(GetRedirectTarget("https://no-such-host.invalid/latest", control, location), NetResult::Offline);

    // Certificates: out of their dates (which for GitHub's means the console's clock), of another host, of an authority that is not on the list. Nothing is taken from any of them.
    CHECK_RESULT(GetRedirectTarget(Url(expired, "/latest"), control, location), NetResult::Clock);
    CHECK_RESULT(Download(Url(expired, "/file"), control, kMaxFileBytes, out), NetResult::Clock);
    CHECK(out.size() == 0);
    CHECK_RESULT(GetRedirectTarget(Url(future, "/latest"), control, location), NetResult::Clock);
    CHECK_RESULT(Download(Url(future, "/file"), control, kMaxFileBytes, out), NetResult::Clock);
    CHECK(out.size() == 0);
    CHECK_RESULT(GetRedirectTarget(Url(other_host, "/latest"), control, location), NetResult::Certificate);
    CHECK_RESULT(Download(Url(other_host, "/file"), control, kMaxFileBytes, out), NetResult::Certificate);
    CHECK(out.size() == 0);
    CHECK_RESULT(GetRedirectTarget(Url(other_ca, "/latest"), control, location), NetResult::Certificate);
    CHECK_RESULT(Download(Url(other_ca, "/file"), control, kMaxFileBytes, out), NetResult::Certificate);
    CHECK(out.size() == 0);
    // A redirect from the good server to a bad one: the bad one's certificate still counts.
    CHECK_RESULT(Download(Url(good, "/to-other-ca"), control, kMaxFileBytes, out), NetResult::Certificate);
    CHECK(out.size() == 0);
}

static void RunGithubOk() {
    HttpControl control;
    std::string location;
    auto start = std::chrono::steady_clock::now();
    CHECK_RESULT(GetRedirectTarget(kLatestUrl, control, location), NetResult::Ok);
    const std::string version = VersionFromLocation(location);
    std::printf("the question: %s -> v%s in %.2f s\n", location.c_str(), version.c_str(), SecondsSince(start));
    CHECK(!version.empty());
    Version parsed;
    CHECK(ParseVersion(version, parsed));

    ByteBuffer file;
    start = std::chrono::steady_clock::now();
    CHECK_RESULT(Download(AssetUrl(version, Target::Cia), control, kMaxFileBytes, file), NetResult::Ok);
    std::printf("the CIA: %zu bytes in %.2f s\n", file.size(), SecondsSince(start));
    CiaIdentity id;
    CHECK(ReadCiaIdentity(file.data(), file.size(), id));
    CHECK(id.title_id == 0x000400000F83D100ull);
    CHECK(id.version == TmdVersion(parsed));

    CHECK_RESULT(Download(AssetUrl(version, Target::File3dsx), control, kMaxFileBytes, file), NetResult::Ok);
    std::printf("the 3DSX: %zu bytes\n", file.size());
    CHECK(Looks3dsx(file.data(), file.size()));

    CHECK_RESULT(Download(ChangelogUrl(version), control, kMaxChangelogBytes, file), NetResult::Ok);
    const std::string news = WhatsNew(std::string(reinterpret_cast<const char*>(file.data()), file.size()), version);
    std::printf("what's new: %s\n", news.c_str());
    CHECK(!news.empty());

    CHECK_RESULT(Download("https://github.com/gionko/3DS-Link/releases/download/v0.0.0/none.cia", control, kMaxFileBytes, file), NetResult::Server);
}

static void RunGithubClock() {
    HttpControl control;
    std::string location;
    ByteBuffer file;
    CHECK_RESULT(GetRedirectTarget(kLatestUrl, control, location), NetResult::Clock);
    CHECK(location.empty());
    CHECK_RESULT(Download(AssetUrl("1.0.13", Target::Cia), control, kMaxFileBytes, file), NetResult::Clock);
    CHECK(file.size() == 0);
    CHECK_RESULT(Download(ChangelogUrl("1.0.13"), control, kMaxChangelogBytes, file), NetResult::Clock);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: %s mock <ca.pem> <first port> | github <cacert.pem> ok|clock\n", argv[0]);
        return 2;
    }
    const std::string roots = ReadFile(argv[2]);
    SetCaBundle(roots.data(), roots.size());
    CHECK(HttpInit());
    const auto start = std::chrono::steady_clock::now();
    if (std::strcmp(argv[1], "mock") == 0) {
        RunMock(std::atoi(argv[3]));
    } else if (std::strcmp(argv[3], "ok") == 0) {
        RunGithubOk();
    } else {
        RunGithubClock();
    }
    HttpExit();
    std::printf("update_http_test %s %s: %d checks, %d failures (%.1f s)\n", argv[1], argv[3], g_checks, g_failures, SecondsSince(start));
    return g_failures ? 1 : 0;
}
