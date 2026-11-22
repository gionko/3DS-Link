// The kernel of the engine of the Download Play page (1.1.4, route B: analysis/piano-1.1.4-2026-11-15/specifiche/8-INGRESSO-DA-3DSLINK.md 3.1): the
// engine's KernelOps on libctru's events, as the plugin's CtrKernelOps (Includes/ctr/kernel_ops.hpp) but for the app - there is no swap of a plugin's
// block here, so GateClosing is never true. A wait is signalled only when svcWaitSynchronization says exactly 0: its timeout (0x09401BFE) is not
// negative, and R_SUCCEEDED would take a host that never answered for a connection.
#pragma once

#include <3ds.h>

#include "azl/uds_engine.hpp"

namespace dlpjoin {

class AppKernel final : public azl::KernelOps {
public:
    azl::u32 CreateEvent() override;
    azl::u32 DuplicateForGame(azl::u32 handle) override;
    void SignalEvent(azl::u32 handle) override;
    void ClearEvent(azl::u32 handle) override;
    bool WaitEvent(azl::u32 handle, azl::s64 timeout_ns) override;
    void CloseEvent(azl::u32 handle) override;
    void SleepNs(azl::s64 ns) override;
    bool GateClosing() override {
        return false;  // (no plugin's block to swap in the app)
    }
};

}  // namespace dlpjoin
