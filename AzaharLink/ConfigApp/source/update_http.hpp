// The HTTPS of the update: libcurl and nothing of the console (Tests/update_http_test.sh builds this file on a PC, with the same curl and mbedTLS, against a server of its own and
// against GitHub). Plain requests: the certificate, the dates and the host name are checked as libcurl checks them, against the public list of roots handed to SetCaBundle.
// The calls block: they run on the update's thread, never on the thread that draws.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "update_logic.hpp"

namespace update {

// Once, before any request: the PEM text of the roots (it must outlive the requests).
void SetCaBundle(const void* pem, std::size_t size);

struct HttpControl {
    const std::atomic<bool>* cancel = nullptr;    // set by another thread: the request stops at its next step
    std::atomic<std::uint32_t>* done = nullptr;   // the bytes that came so far
    std::atomic<std::uint32_t>* total = nullptr;  // how many are coming (0 = not known yet)
    long connect_seconds = 30;                    // the name, the connection and TLS (a handshake takes seconds on an Old 3DS)
    long stall_seconds = 20;                      // no byte for this long: the connection is given up
    long total_seconds = 0;                       // the whole request (0 = no limit)
};

// The file a request brought, in memory (malloc: a big file must be able to fail).
class ByteBuffer {
public:
    ByteBuffer() = default;
    ~ByteBuffer();
    ByteBuffer(const ByteBuffer&) = delete;
    ByteBuffer& operator=(const ByteBuffer&) = delete;
    const std::uint8_t* data() const {
        return data_;
    }
    std::size_t size() const {
        return size_;
    }
    void Clear();
    bool Append(const char* bytes, std::size_t count);

private:
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0, capacity_ = 0;
};

// Must be called once before the first request of a process (curl_global_init).
bool HttpInit();
void HttpExit();

// Asks for `url` without following it: `location` gets where a redirect points to. Ok only for a redirect.
NetResult GetRedirectTarget(const std::string& url, const HttpControl& control, std::string& location);

// Fetches `url` into `out`, following redirects (HTTPS only, at most 5). Ok only for a whole 200.
NetResult Download(const std::string& url, const HttpControl& control, std::size_t max_bytes, ByteBuffer& out);

} // namespace update
