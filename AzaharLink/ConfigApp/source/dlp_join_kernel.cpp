#include "dlp_join_kernel.hpp"

namespace dlpjoin {

azl::u32 AppKernel::CreateEvent() {
    Handle h = 0;
    if (R_FAILED(svcCreateEvent(&h, RESET_ONESHOT))) {
        return 0;
    }
    return h;
}

azl::u32 AppKernel::DuplicateForGame(azl::u32 handle) {
    // (the engine keeps its own handle: whoever is given one may close it)
    Handle dup = 0;
    if (R_FAILED(svcDuplicateHandle(&dup, handle))) {
        return 0;
    }
    return dup;
}

void AppKernel::SignalEvent(azl::u32 handle) {
    svcSignalEvent(handle);
}

void AppKernel::ClearEvent(azl::u32 handle) {
    svcClearEvent(handle);
}

bool AppKernel::WaitEvent(azl::u32 handle, azl::s64 timeout_ns) {
    return svcWaitSynchronization(handle, timeout_ns) == 0;  // (exactly 0: the timeout 0x09401BFE is not negative)
}

void AppKernel::CloseEvent(azl::u32 handle) {
    svcCloseHandle(handle);
}

void AppKernel::SleepNs(azl::s64 ns) {
    svcSleepThread(ns);
}

}  // namespace dlpjoin
