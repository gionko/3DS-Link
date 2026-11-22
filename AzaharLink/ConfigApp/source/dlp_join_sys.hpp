// The one system call of route B (1.1.4: analysis/piano-1.1.4-2026-11-15/specifiche/8-INGRESSO-DA-3DSLINK.md D-B2 and 3.4, approved by the user on
// 2026-11-16): ns:s SetWirelessRebootInfo, what the system's Download Play application sends before its jump to a child. The prepared child then asks
// APT for GetWirelessRebootInfo and the SYSTEM answers with these bytes: no hook, no plg:ldr, no file on the SD card. A file of its own, so that the
// pins of system.cpp (plg:ldr 2 and 3) and of svcConnectToPort stay as they are.
#pragma once

#include <3ds.h>

namespace dlpjoin {

/// ns:s SetWirelessRebootInfo (command 6, 0x00060042), as dlplay sends it (modalita-download/RAPPORTO.md 4.3: [1]=16, [2]=(16<<14)|2,
/// [3]=pointer). The 16 bytes (the host's MAC, the passphrase, 0: ComposeRebootInfo) go in a buffer of this call, the IPC's static buffer 0, wiped
/// (volatile) right after the request. The result of the command: anything but 0 means no jump.
Result SetWirelessRebootInfo(const u8 (&info)[16]);

}  // namespace dlpjoin
