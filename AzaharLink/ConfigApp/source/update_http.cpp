#include "update_http.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>

namespace update {

namespace {

const void* g_ca_pem = nullptr;
std::size_t g_ca_size = 0;

struct Sink {
    ByteBuffer* out = nullptr;
    std::size_t max_bytes = 0;
    NetResult failure = NetResult::Ok;
};

std::size_t OnBytes(char* bytes, std::size_t size, std::size_t count, void* user) {
    Sink* sink = static_cast<Sink*>(user);
    const std::size_t n = size * count;
    if (sink->out->size() + n > sink->max_bytes) {
        sink->failure = NetResult::TooBig;
        return 0; // (curl stops: CURLE_WRITE_ERROR)
    }
    if (!sink->out->Append(bytes, n)) {
        sink->failure = NetResult::NoMemory;
        return 0;
    }
    return n;
}

int OnProgress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    const HttpControl* control = static_cast<const HttpControl*>(user);
    if (control->done) {
        control->done->store(static_cast<std::uint32_t>(dlnow));
    }
    if (control->total) {
        control->total->store(static_cast<std::uint32_t>(dltotal));
    }
    return control->cancel && control->cancel->load() ? 1 : 0;
}

// What every request shares. `error` is curl's text of the failure (it says which check the certificate failed).
void SetCommon(CURL* curl, const std::string& url, const HttpControl& control, char* error) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // (no signals on this console, and this is a thread)
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "3DSLink");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, control.connect_seconds);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, control.stall_seconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, control.total_seconds);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &control);
    // The roots are in memory: no file of the console is looked for.
    curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
    curl_easy_setopt(curl, CURLOPT_CAPATH, nullptr);
    curl_blob roots;
    roots.data = const_cast<void*>(g_ca_pem);
    roots.len = g_ca_size;
    roots.flags = CURL_BLOB_NOCOPY;
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &roots);
}

NetResult MapCurlResult(CURLcode rc, const char* error, const Sink* sink) {
    switch (rc) {
    case CURLE_OK:
        return NetResult::Ok;
    case CURLE_ABORTED_BY_CALLBACK:
        return NetResult::Cancelled;
    case CURLE_WRITE_ERROR:
        return sink && sink->failure != NetResult::Ok ? sink->failure : NetResult::Failed;
    case CURLE_OUT_OF_MEMORY:
        return NetResult::NoMemory;
    case CURLE_PEER_FAILED_VERIFICATION:
        return CertificateFailure(error);
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
        return NetResult::Offline;
    case CURLE_TOO_MANY_REDIRECTS:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return NetResult::Server;
    default:
        return NetResult::Failed;
    }
}

} // namespace

ByteBuffer::~ByteBuffer() {
    Clear();
}

void ByteBuffer::Clear() {
    std::free(data_);
    data_ = nullptr;
    size_ = capacity_ = 0;
}

bool ByteBuffer::Append(const char* bytes, std::size_t count) {
    if (size_ + count > capacity_) {
        std::size_t wanted = capacity_ ? capacity_ * 2 : 256 * 1024;
        while (wanted < size_ + count) {
            wanted *= 2;
        }
        std::uint8_t* grown = static_cast<std::uint8_t*>(std::realloc(data_, wanted));
        if (!grown) {
            return false;
        }
        data_ = grown;
        capacity_ = wanted;
    }
    std::memcpy(data_ + size_, bytes, count);
    size_ += count;
    return true;
}

void SetCaBundle(const void* pem, std::size_t size) {
    g_ca_pem = pem;
    g_ca_size = size;
}

bool HttpInit() {
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void HttpExit() {
    curl_global_cleanup();
}

NetResult GetRedirectTarget(const std::string& url, const HttpControl& control, std::string& location) {
    location.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        return NetResult::NoMemory;
    }
    char error[CURL_ERROR_SIZE] = "";
    SetCommon(curl, url, control, error);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    NetResult result = MapCurlResult(curl_easy_perform(curl), error, nullptr);
    if (result == NetResult::Ok) {
        long status = 0;
        char* target = nullptr;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &target);
        if ((status == 301 || status == 302) && target) {
            location = target;
        } else {
            result = NetResult::Server;
        }
    }
    curl_easy_cleanup(curl);
    return result;
}

NetResult Download(const std::string& url, const HttpControl& control, std::size_t max_bytes, ByteBuffer& out) {
    out.Clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        return NetResult::NoMemory;
    }
    char error[CURL_ERROR_SIZE] = "";
    Sink sink;
    sink.out = &out;
    sink.max_bytes = max_bytes;
    SetCommon(curl, url, control, error);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // (the body of a 404 is not the file)
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const CURLcode rc = curl_easy_perform(curl);
    NetResult result = rc == CURLE_HTTP_RETURNED_ERROR ? NetResult::Server : MapCurlResult(rc, error, &sink);
    if (result == NetResult::Ok) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status != 200) {
            result = NetResult::Server;
        }
    }
    curl_easy_cleanup(curl);
    if (result != NetResult::Ok) {
        out.Clear();
    }
    return result;
}

} // namespace update
