// What Download Play's host library (DlpLink/host/dlph.c, the copy in AzaharLink/ConfigApp/dlph/ is the same file) says about a child file: the
// acceptance conditions of dlph_open (size, TMD, Title ID, the emulator variant refused, the SMDH) and what it would announce (name, version, bytes).
// dlph.c is built here against the stand-in for <3ds.h> that its own test uses (DlpLink/host/tests/3ds.h); nothing of the network is run, only dlph_open,
// dlph_get_stats and dlph_close. The hooks the stand-in declares are empty: dlph_open touches none of them.
#include <3ds.h>

u64 osGetTime(void) { return 0; }
void svcSleepThread(s64 ns) { (void)ns; }
Result udsInit(size_t s, const char *u) { (void)s; (void)u; return 0; }
void udsExit(void) {}
void udsGenerateDefaultNetworkStruct(udsNetworkStruct *n, u32 id, u8 id8, u8 max) { memset(n, 0, sizeof(*n)); n->wlancommID = id; n->id8 = id8; n->max_nodes = max; }
Result udsCreateNetwork(const udsNetworkStruct *n, const void *p, size_t ps, udsBindContext *c, u8 ch, u32 rb) { (void)n; (void)p; (void)ps; (void)c; (void)ch; (void)rb; return 0; }
Result udsUnbind(udsBindContext *c) { (void)c; return 0; }
Result udsDestroyNetwork(void) { return 0; }
Result udsSetNewConnectionsBlocked(bool b, bool c, bool f) { (void)b; (void)c; (void)f; return 0; }
Result udsGetNodeInformation(u16 id, udsNodeInfo *o) { (void)id; memset(o, 0, sizeof(*o)); return 0; }
Result udsEjectClient(u16 n) { (void)n; return 0; }
bool udsWaitDataAvailable(const udsBindContext *c, bool next, bool wait) { (void)c; (void)next; (void)wait; return false; }
Result udsPullPacket(const udsBindContext *c, void *buf, size_t size, size_t *actual, u16 *src) { (void)c; (void)buf; (void)size; *actual = 0; *src = 0; return 0; }
Result udsGetConnectionStatus(udsConnectionStatus *o) { memset(o, 0, sizeof(*o)); return 0; }
Result udsSendTo(u16 dst, u8 ch, u8 flags, const void *buf, size_t size) { (void)dst; (void)ch; (void)flags; (void)buf; (void)size; return 0; }

#include "../host/dlph.c"
#include "../host/dlph_wire.c"

// 1 = dlph_open accepted the file (the fields are filled), 0 = refused.
int dlpc_glue_dlph(const char *path, char *name, size_t name_size, unsigned *version, unsigned long long *tid, unsigned long *child_bytes,
                   unsigned long *transfer_bytes, unsigned long *blocks)
{
    dlph_cfg cfg;
    static const u8 mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 } /* PLACEHOLDER MAC: to compare with a capture of a real console, set the MAC of that console here */;
    memset(&cfg, 0, sizeof(cfg));
    memset(cfg.iv, 0x5A, 16);                                 // a made-up IV: the acceptance conditions do not use it
    cfg.child_path = path;
    cfg.lang = 1;                                             // English: the name that the Download Play list shows
    cfg.gap_us = 3000;
    cfg.max_nodes = 2;
    memcpy(cfg.mac, mac, 6);
    if (!dlph_open(&cfg)) return 0;
    dlph_stats st = dlph_get_stats();
    snprintf(name, name_size, "%s", st.child_name);
    *version = st.child_version;
    *tid = st.child_tid;
    *child_bytes = st.child_bytes;
    *transfer_bytes = st.transfer_bytes;
    *blocks = (st.transfer_bytes + DLPH_CHUNK_SIZE - 1) / DLPH_CHUNK_SIZE;
    dlph_close();
    return 1;
}
