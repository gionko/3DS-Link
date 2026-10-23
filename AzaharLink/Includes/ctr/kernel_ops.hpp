// KernelOps implementation on top of real 3DS kernel events (svc*).
#pragma once

#include <3ds.h>

#include "azl/uds_engine.hpp"

namespace azl {
namespace ctr {

class CtrKernelOps : public KernelOps {
public:
    u32 CreateEvent() override {
        Handle h = 0;
        if (R_FAILED(svcCreateEvent(&h, RESET_ONESHOT))) {
            return 0;
        }
        return h;
    }
    u32 DuplicateForGame(u32 handle) override {
        // The engine keeps its own handle: the game closes whatever we give it (libctru's
        // udsUnbind closes the event *before* the Unbind request), and a recycled handle number
        // must never be signalled by mistake.
        Handle dup = 0;
        if (R_FAILED(svcDuplicateHandle(&dup, handle))) {
            return 0;
        }
        return dup;
    }
    void SignalEvent(u32 handle) override {
        svcSignalEvent(handle);
    }
    void ClearEvent(u32 handle) override {
        svcClearEvent(handle);
    }
    bool WaitEvent(u32 handle, s64 timeout_ns) override {
        // Exactly 0 = signalled. The timeout code (0x09401BFE) is not negative, so R_SUCCEEDED()
        // would report a host that never answered as a successful connection.
        return svcWaitSynchronization(handle, timeout_ns) == 0;
    }
    void CloseEvent(u32 handle) override {
        svcCloseHandle(handle);
    }
    void SleepNs(s64 ns) override {
        svcSleepThread(ns);
    }
};

} // namespace ctr
} // namespace azl
