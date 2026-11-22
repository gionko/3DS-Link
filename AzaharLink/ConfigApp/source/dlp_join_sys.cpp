#include "dlp_join_sys.hpp"

#include <cstring>

namespace dlpjoin {

Result SetWirelessRebootInfo(const u8 (&info)[16]) {
    Handle h = 0;
    Result rc = srvGetServiceHandle(&h, "ns:s");
    if (R_FAILED(rc)) {
        return rc;
    }
    u8 buf[16];  // (the static buffer of this request: on this stack, wiped below)
    std::memcpy(buf, info, sizeof(buf));
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(6, 1, 2);
    cmdbuf[1] = 16;
    cmdbuf[2] = IPC_Desc_StaticBuffer(16, 0);
    cmdbuf[3] = (u32)buf;
    rc = svcSendSyncRequest(h);
    volatile u8* const wipe = buf;  // (a store the compiler cannot leave out: the passphrase and the host's MAC)
    for (std::size_t i = 0; i < sizeof(buf); ++i) {
        wipe[i] = 0;
    }
    if (R_SUCCEEDED(rc)) {
        rc = static_cast<Result>(cmdbuf[1]);
    }
    svcCloseHandle(h);
    return rc;
}

}  // namespace dlpjoin
