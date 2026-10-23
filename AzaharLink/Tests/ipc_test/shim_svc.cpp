// Fake kernel for the host IPC test. Handles are small integers mapped to shared objects, so
// svcDuplicateHandle / svcCloseHandle behave like the real thing for events.
#include <3ds.h>
extern "C" {
#include <3ds/srv.h>
#include <3ds/services/cfgu.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "ctr/uds_hle.hpp"

namespace {

struct EventObject {
    bool signaled = false;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::map<Handle, std::shared_ptr<EventObject>> g_handles;
Handle g_next_handle = 0x100;

thread_local u32 g_cmdbuf[64];
thread_local u32 g_static[16];

constexpr Result kErrInvalidHandle = 0xD8E007F7;

} // namespace

// The HLE the "game" talks to (set by the test).
azl::ctr::UdsHle* g_test_hle = nullptr;
std::atomic<unsigned> g_fake_friend_seed_low{0x11111111};
std::atomic<bool> g_fail_create_event{false}; // a test: svcCreateEvent fails
// svcSleepThread is counted (the throttle of a game thread that polls a failing command sleeps 0.5 ms per call: a test
// that makes thousands of them turns the real sleeping off and reads the totals).
std::atomic<unsigned long long> g_test_sleep_calls{0};
std::atomic<unsigned long long> g_test_sleep_ns{0};
std::atomic<bool> g_test_real_sleep{true};

extern "C" {

u32* getThreadCommandBuffer(void) {
    return g_cmdbuf;
}
u32* getThreadStaticBuffers(void) {
    return g_static;
}

Result svcCreateEvent(Handle* event, ResetType) {
    if (g_fail_create_event.load()) {
        return 0xC860180A; // the process is out of kernel objects (OutOfResource)
    }
    std::lock_guard<std::mutex> l(g_mutex);
    *event = g_next_handle++;
    g_handles[*event] = std::make_shared<EventObject>();
    return 0;
}
Result svcSignalEvent(Handle h) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_handles.find(h);
    if (it == g_handles.end()) {
        return kErrInvalidHandle;
    }
    it->second->signaled = true;
    g_cv.notify_all();
    return 0;
}
Result svcClearEvent(Handle h) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_handles.find(h);
    if (it == g_handles.end()) {
        return kErrInvalidHandle;
    }
    it->second->signaled = false;
    return 0;
}
Result svcWaitSynchronization(Handle h, s64 ns) {
    std::unique_lock<std::mutex> l(g_mutex);
    auto it = g_handles.find(h);
    if (it == g_handles.end()) {
        return kErrInvalidHandle;
    }
    std::shared_ptr<EventObject> obj = it->second;
    const bool ok = ns < 0 ? (g_cv.wait(l, [&] { return obj->signaled; }), true)
                           : g_cv.wait_for(l, std::chrono::nanoseconds(ns), [&] { return obj->signaled; });
    if (!ok) {
        return 0x09401BFE; // timeout
    }
    obj->signaled = false; // one-shot
    return 0;
}
Result svcCloseHandle(Handle h) {
    std::lock_guard<std::mutex> l(g_mutex);
    return g_handles.erase(h) ? 0 : kErrInvalidHandle;
}
Result svcDuplicateHandle(Handle* out, Handle original) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_handles.find(original);
    if (it == g_handles.end()) {
        return kErrInvalidHandle;
    }
    *out = g_next_handle++;
    g_handles[*out] = it->second;
    return 0;
}
void svcSleepThread(s64 ns) {
    ++g_test_sleep_calls;
    g_test_sleep_ns += static_cast<unsigned long long>(ns);
    if (g_test_real_sleep.load()) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
}
Result svcGetThreadPriority(s32* out, Handle) {
    *out = 0x30;
    return 0;
}
Result svcGetThreadId(u32* out, Handle) {
    *out = 0x1234; // (the "game thread": the first-caller line of NoteCommand names it)
    return 0;
}
Result svcGetProcessId(u32* out, Handle) {
    *out = 0x2D; // the "game"; UdsHle serves every ndm:u request as this process
    return 0;
}
Result svcCreateMemoryBlock(Handle* memblock, u32, u32, MemPerm, MemPerm) {
    return svcCreateEvent(memblock, RESET_ONESHOT); // any object with a handle will do
}

// Every request goes to the HLE first, exactly like the patched svc stub in the game; whatever it
// does not claim would reach the real kernel, which does not know our handles.
Result svcSendSyncRequest(Handle h) {
    Result r = 0;
    if (g_test_hle && g_test_hle->HandleSendSyncRequest(h, &r)) {
        return r;
    }
    return kErrInvalidHandle;
}

// libctru's srvGetServiceHandle() builds this request; do the same so that the interception of
// srv:GetServiceHandle is exercised.
Result srvGetServiceHandle(Handle* out, const char* name) {
    u32* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0x5, 4, 0);
    std::memset(&cmd[1], 0, 8);
    std::strncpy(reinterpret_cast<char*>(&cmd[1]), name, 8);
    cmd[3] = static_cast<u32>(std::strlen(name));
    cmd[4] = 0;
    const Handle srv_session = 0x1; // not one of ours: the game's own srv: handle
    Result rc = svcSendSyncRequest(srv_session);
    if (R_FAILED(rc)) {
        return 0xD8E06406; // srv: "service not found / not allowed"
    }
    rc = cmd[1];
    if (R_SUCCEEDED(rc)) {
        *out = cmd[3];
    }
    return rc;
}

Result cfguInit(void) {
    return 0;
}
void cfguExit(void) {}
Result CFGU_GetConfigInfoBlk2(u32 size, u32 blkID, void* outData) {
    std::memset(outData, 0, size);
    if (blkID == 0x00090000 && size >= 8) { // local friend code seed
        const u32 low = g_fake_friend_seed_low.load();
        std::memcpy(outData, &low, 4);
        const u32 high = 0x00000001;
        std::memcpy(static_cast<u8*>(outData) + 4, &high, 4);
    }
    return 0;
}

} // extern "C"
