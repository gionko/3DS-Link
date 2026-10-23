// Minimal stand-in for <3ds.h> so rawhost/source/main.c can run on a PC against a simulated Download Play client.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
typedef int LightLock;
static inline void LightLock_Init(LightLock *l) { *l = 1; }
static inline void LightLock_Lock(LightLock *l) { (void)l; }
static inline void LightLock_Unlock(LightLock *l) { (void)l; }
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t s64; typedef int32_t s32;
typedef u32 Result; typedef u32 Handle; typedef unsigned int socklen_t_;
#define R_FAILED(r) ((int)(r) < 0)
#define R_SUCCEEDED(r) ((int)(r) >= 0)
typedef struct { int dummy; } PrintConsole;
#define GFX_TOP 0
#define KEY_START 1
#define KEY_A 2
#define KEY_B 4
#define KEY_SELECT 64
#define KEY_DUP 8
#define KEY_DDOWN 16
#define SOL_CONFIG 0xfffe
#define NETOPT_MAC_ADDRESS 0x1004
#define UDS_BROADCAST_NETWORKNODEID 0xFFFF
#define UDS_CHECK_SENDTO_FATALERROR(x) (R_FAILED(x) && x!=0xC86113F0)
#define PS_ALGORITHM_CTR_ENC 0
#define PS_KEYSLOT_39_DLP 0x39
typedef struct { u32 BindNodeID; Handle event; bool spectator; } udsBindContext;
typedef struct { u32 status; u16 cur_NetworkNodeID; u8 total_nodes; u8 max_nodes; u16 node_bitmask; } udsConnectionStatus;
typedef struct { u8 host_macaddress[6]; u8 channel; u8 unk_x15; u32 wlancommID; u8 id8; u8 max_nodes; u8 total_nodes; u16 appdata_size; u8 appdata[200]; } udsNetworkStruct;
#define UDS_MAXNODES 16
typedef struct { char name[24]; } udsNodeInfo;
typedef struct { udsNetworkStruct network; udsNodeInfo nodes[UDS_MAXNODES]; } udsNetworkScanInfo;
#define UDSCONTYPE_Client 1
static inline bool udsCheckNodeInfoInitialized(const udsNodeInfo *n) { (void)n; return false; }
static inline Result udsGetNodeInfoUsername(const udsNodeInfo *n, char *o) { memcpy(o, n->name, 24); return 0; }
static inline Result udsScanBeacons(void *b, size_t m, udsNetworkScanInfo **n, size_t *t, u32 id, u8 id8, const u8 *mac, bool c) { (void)b; (void)m; (void)id; (void)id8; (void)mac; (void)c; *n = NULL; *t = 0; return 0; }
static inline Result udsConnectNetwork(const udsNetworkStruct *n, const void *p, size_t ps, udsBindContext *c, u16 r, int type, u8 ch, u32 rb) { (void)n; (void)p; (void)ps; (void)c; (void)r; (void)type; (void)ch; (void)rb; return (Result)-1; }
static inline Result udsDisconnectNetwork(void) { return 0; }
Result udsEjectClient(u16 n);
Result udsSetNewConnectionsBlocked(bool b, bool c, bool f);
static inline Result udsBind(udsBindContext *c, u16 n, bool sp, u8 ch, u32 rb) { (void)c; (void)n; (void)sp; (void)ch; (void)rb; return 0; }
Result udsGetNodeInformation(u16 id, udsNodeInfo *o);
static inline Result udsGetChannel(u8 *c) { *c = 11; return 0; }
static inline Result udsSetApplicationData(const void *b, size_t n) { (void)b; (void)n; return 0; }
static inline u32 osGetFirmVersion(void) { return (11u << 24) | (17u << 16); } static inline u32 osGetKernelVersion(void) { return (2u << 24) | (54u << 16); }
#define GET_VERSION_MAJOR(v) ((v) >> 24) 
#define GET_VERSION_MINOR(v) (((v) >> 16) & 0xFF)
#define GET_VERSION_REVISION(v) (((v) >> 8) & 0xFF)
static inline Result svcGetSystemInfo(s64 *o, u32 t, s32 p) { (void)t; (void)p; *o = 0; return 0; }
static inline Result CFGU_GetSystemModel(u8 *m) { *m = 2; return 0; } static inline Result CFGU_SecureInfoGetRegion(u8 *r) { *r = 1; return 0; }


static inline void consoleInit(int s, PrintConsole *c) { (void)s; (void)c; }
static inline void consoleSelect(PrintConsole *c) { (void)c; }
static inline void gfxInitDefault(void) {} static inline void gfxExit(void) {}
static inline void gfxFlushBuffers(void) {} static inline void gfxSwapBuffers(void) {} static inline void gspWaitForVBlank(void) {}
static inline void hidScanInput(void) {} static inline u32 hidKeysDown(void) { return 0; }
static inline bool aptMainLoop(void) { return false; }
static inline Result cfguInit(void) { return 0; } static inline void cfguExit(void) {}
static inline Result CFGU_GetSystemLanguage(u8 *l) { *l = 4; return 0; }
static inline Result psInit(void) { return 0; } static inline void psExit(void) {}
static inline Result PS_GenerateRandomBytes(void *b, size_t n) { for (size_t i = 0; i < n; i++) ((u8 *)b)[i] = (u8)(0x40 + i); return 0; }
static inline Result PS_EncryptDecryptAes(u32 size, u8 *in, u8 *out, int alg, int slot, u8 *iv) { (void)size; (void)in; (void)alg; (void)slot; (void)iv; static const u8 k[4] = { 0x11, 0x22, 0x33, 0x44 }; for (int i = 0; i < 16; i++) out[i] = i < 4 ? k[i] : 0; return 0; }
static inline Result socInit(u32 *b, u32 n) { (void)b; (void)n; return 0; } static inline void socExit(void) {}
typedef unsigned int socklen_t;
static inline Result SOCU_GetNetworkOpt(int l, int o, void *v, socklen_t *len) { (void)l; (void)o; (void)len; static const u8 m[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 } /* PLACEHOLDER MAC: to compare with a capture of a real console, set the MAC of that console here */; for (int i = 0; i < 6; i++) ((u8 *)v)[i] = m[i]; return 0; }
// hooks implemented by the test
u64 osGetTime(void);
void svcSleepThread(s64 ns);
Result udsInit(size_t s, const char *u); void udsExit(void);
void udsGenerateDefaultNetworkStruct(udsNetworkStruct *n, u32 id, u8 id8, u8 max);
Result udsCreateNetwork(const udsNetworkStruct *n, const void *p, size_t ps, udsBindContext *c, u8 ch, u32 rb);
Result udsUnbind(udsBindContext *c); Result udsDestroyNetwork(void);
Result udsSendTo(u16 dst, u8 ch, u8 flags, const void *buf, size_t size);
bool udsWaitDataAvailable(const udsBindContext *c, bool next, bool wait);
Result udsPullPacket(const udsBindContext *c, void *buf, size_t size, size_t *actual, u16 *src);
Result udsGetConnectionStatus(udsConnectionStatus *o);
