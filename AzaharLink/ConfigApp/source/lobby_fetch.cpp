#include "lobby_fetch.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace lobby {

namespace {

constexpr std::size_t kMaxBody = 256 * 1024; // a list with a hundred rooms and their players is about 60 KB
constexpr int kMaxRedirects = 3;

std::string Trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// One request. Returns 0 on success (body filled), else a reason. `redirect` is set when the server asked to go elsewhere.
const char* Get(const std::string& url, std::string& body, std::string& redirect, char* why, std::size_t why_size) {
    httpcContext ctx;
    Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url.c_str(), 1);
    if (R_FAILED(rc)) {
        std::snprintf(why, why_size, "Bad address (0x%08lX).", static_cast<unsigned long>(rc));
        return why;
    }
    httpcAddRequestHeaderField(&ctx, "User-Agent", "3DSLink");
    httpcAddRequestHeaderField(&ctx, "Accept", "application/json");
    if (url.compare(0, 8, "https://") == 0) {
        // The 3DS does not carry today's root certificates, so a check would fail on most servers. What comes back is a public room list that
        // the user still has to choose a room from and that is tested before it is used: nothing secret is sent or trusted.
        httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    }
    rc = httpcBeginRequest(&ctx);
    if (R_FAILED(rc)) {
        httpcCloseContext(&ctx);
        std::snprintf(why, why_size, "No connection to the server (0x%08lX). Is WiFi on?", static_cast<unsigned long>(rc));
        return why;
    }
    u32 status = 0;
    rc = httpcGetResponseStatusCode(&ctx, &status);
    if (R_FAILED(rc)) {
        httpcCloseContext(&ctx);
        std::snprintf(why, why_size, "No answer from the server (0x%08lX).", static_cast<unsigned long>(rc));
        return why;
    }
    if (status >= 300 && status < 400) {
        char loc[512] = {0};
        if (R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Location", loc, sizeof(loc) - 1))) {
            redirect = Trim(loc);
        }
        httpcCloseContext(&ctx);
        if (redirect.empty()) {
            std::snprintf(why, why_size, "The server moved (HTTP %u) without saying where.", static_cast<unsigned>(status));
            return why;
        }
        return nullptr;
    }
    if (status != 200) {
        httpcCloseContext(&ctx);
        std::snprintf(why, why_size, status == 404 ? "HTTP 404: this address has no /lobby." : "The server answered HTTP %u.",
                      static_cast<unsigned>(status));
        return why;
    }
    std::vector<char> chunk(0x4000);
    for (;;) {
        u32 got = 0;
        rc = httpcDownloadData(&ctx, reinterpret_cast<u8*>(chunk.data()), static_cast<u32>(chunk.size()), &got);
        body.append(chunk.data(), got);
        if (body.size() > kMaxBody) {
            httpcCloseContext(&ctx);
            std::snprintf(why, why_size, "The room list is too big (over %u KB).", static_cast<unsigned>(kMaxBody / 1024));
            return why;
        }
        if (rc != static_cast<Result>(HTTPC_RESULTCODE_DOWNLOADPENDING)) {
            break;
        }
    }
    httpcCloseContext(&ctx);
    if (R_FAILED(rc)) {
        std::snprintf(why, why_size, "The download broke off (0x%08lX).", static_cast<unsigned long>(rc));
        return why;
    }
    return nullptr;
}

} // namespace

std::string LobbyUrl(const std::string& base_in) {
    std::string base = Trim(base_in);
    if (base.empty()) {
        return std::string();
    }
    if (base.compare(0, 7, "http://") != 0 && base.compare(0, 8, "https://") != 0) {
        base = "http://" + base;
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.size() >= 6 && base.compare(base.size() - 6, 6, "/lobby") == 0) {
        return base; // (the user pasted the whole address)
    }
    return base + "/lobby";
}

void Fetch::Start(const std::string& base_url) {
    if (state_) {
        state_->abandoned = true;
    }
    state_ = std::make_shared<State>();
    state_->url = LobbyUrl(base_url);
    state_->started_ms = osGetTime();
    if (state_->url.empty()) {
        state_->error = "No lobby address: press A on the first row.";
        state_->phase = 2;
        return;
    }
    auto* share = new std::shared_ptr<State>(state_);
    auto entry = [](void* arg) {
        std::shared_ptr<State> st = *static_cast<std::shared_ptr<State>*>(arg);
        delete static_cast<std::shared_ptr<State>*>(arg);
        char why[160] = {0};
        std::string body;
        std::string error;
        Result rc = httpcInit(0x1000);
        if (R_FAILED(rc)) {
            std::snprintf(why, sizeof(why), "The http service is not available (0x%08lX).", static_cast<unsigned long>(rc));
            error = why;
        } else {
            std::string url = st->url;
            for (int hop = 0; hop <= kMaxRedirects && error.empty(); ++hop) {
                std::string redirect;
                body.clear();
                const char* bad = Get(url, body, redirect, why, sizeof(why));
                if (bad) {
                    error = bad;
                } else if (!redirect.empty()) {
                    url = redirect;
                    if (hop == kMaxRedirects) {
                        error = "The server redirects too many times.";
                    }
                } else {
                    break;
                }
            }
            httpcExit();
        }
        if (!st->abandoned) {
            if (error.empty()) {
                st->body = std::move(body);
                st->phase = 1;
            } else {
                st->error = std::move(error);
                st->phase = 2;
            }
        }
    };
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    Thread thread = threadCreate(entry, share, 0x10000, priority, -2, true); // detached: nobody joins it
    if (!thread) {
        delete share;
        state_->error = "Could not start the download.";
        state_->phase = 2;
    }
}

void Fetch::Abandon() {
    if (state_) {
        state_->abandoned = true;
        state_.reset();
    }
}

Fetch::Phase Fetch::phase() const {
    if (!state_) {
        return Phase::Idle;
    }
    switch (state_->phase.load()) {
    case 1: return Phase::Done;
    case 2: return Phase::Failed;
    default: return Phase::Loading;
    }
}

const std::string& Fetch::body() const {
    static const std::string empty;
    return state_ ? state_->body : empty;
}

const std::string& Fetch::error() const {
    static const std::string empty;
    return state_ ? state_->error : empty;
}

u64 Fetch::elapsed_ms() const {
    return state_ ? osGetTime() - state_->started_ms : 0;
}

} // namespace lobby
