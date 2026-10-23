// dlph.c - see dlph.h. Everything on the air follows DlpTest/rawhost (proven on hardware 2026-10-14..20); what changed is only the
// organisation: one Download Play console at a time, the information a server screen needs (name of the console, progress, speed, time
// left, history) and two ways of keeping the room clean:
//   * room of 2 (persistent = false): the room is re-made after every console and after a stuck download;
//   * persistent room (persistent = true): nothing is kicked; a seat used by a console is closed for ~45 s (it heals by itself).
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlph.h"
#include "dlph_wire.h"

#define WLANCOMM_ID     0x2810
#define NET_ID8         0x55
#define BCAST_CHANNEL   1                 // data channel of the title broadcast
#define CLIENT_CHANNEL  2                 // data channel of the host <-> client conversation
#define RECV_BUF        0x3C00
#define SENDTO_NOROOM   0xC86113F0u

#define STALL_MS        3000              // client silent this long while downloading: send the block again
#define REMAKE_MS       9000              // ... this long: the network is stuck, re-make it
#define HANDSHAKE_MS    10000             // room of 2: auth / start of distribution not answered
#define NOTOURS_MS      3000              // persistent room: a node that does not answer auth is not a Download Play console
#define FINAL_MS        6000              // final packet delivered but the client has not left
#define QUARANTINE_MS   45000             // a used seat heals in 44-49 s
#define FOREIGN_RETRY_MS 10000            // persistent room: a node that did not answer is asked again after this long (a console on a dirty seat answers once the seat is clean)

static const char PASSPHRASE[] = "0km@tsa$uhmy1a0sa";                          // sent with its final NUL (18 bytes), like the real one
#if __has_include("../../../DlpLink/host/dlph_iv.local.h")
#include "../../../DlpLink/host/dlph_iv.local.h"   /* yours, from your own console (not in this repository): static const u8 DLP_IV[16] = { ... }; */
#else
#warning "dlph_iv.local.h is missing: PLACEHOLDER IV (all zero), 3DS Link Delivery will not work until you put your own there"
static const u8 DLP_IV[16] = { 0 };   /* PLACEHOLDER */
#endif

typedef enum { CS_NONE, CS_AUTH, CS_DIST_ACK, CS_CONTENT, CS_NOCONTENT, CS_DONE, CS_FINAL } CState;

static struct {
    bool opened, up, persistent;
    dlph_cfg cfg;
    u8 seed[4];
    u8 max_nodes;
    u32 quarantine_ms;

    // the child
    u8 *cia;
    u32 cia_size, transfer_size;
    u64 tid;
    u16 ver;
    u8 name[DLPH_NAME_BYTES], desc[DLPH_DESC_BYTES], icon[DLPH_ICON_BYTES];      // SMDH, little endian as in the file
    u8 bc[5][DLPH_BC4_LEN];
    size_t bclen[5];
    int nbc;

    // the room
    udsBindContext bind;
    u8 pass9[9];
    u64 lastBcast;
    bool blocked;                      // persistent room: new connections closed because the lowest free seat is dirty
    u16 foreign_mask;                  // nodes that did not answer as Download Play consoles (3DS Link receivers): left alone, asked again now and then
    u64 foreign_at[10];
    u64 dirty_until[10];               // per seat

    // the Download Play console (one at a time)
    CState st;
    int node;                          // its seat (2..8), 0 when none
    u8 seq, rid[2];
    u32 block, next_ack, total_frags, retries;
    bool can_next, sent_req;
    u64 lastTx, since, lastRx, wdAt, doneAt;
    u32 pings, finals;
    u64 finalAt;
    u64 selAt;
    u8 lastBm[64], lastBmLen, selSent, selFail, selOff;

    // numbers for the app
    int percent;
    u32 served, lost, remade, bytes_sent;
    int gap_us;

    // what a server screen shows
    char cname[32];                    // user name of the console being served
    u64 name_try;
    int name_tries;
    u64 joined_at;                     // 0 = nobody has answered as a Download Play console yet
    u64 content_at, room_at;
    u32 hi, frags_first, frags_sent;   // highest fragment index sent + 1, fragments sent for the first time, all sent
    u64 spd_at;
    u32 spd_bytes;
    float mibps;
    u32 visit_no;
    char child_name[64];
    bool remaking, had_version, want_up;
    u64 retry_at;
    u32 refused;
} G;

// shared with the other threads (the snapshot for the screen, the history, the kick request)
static LightLock g_lock;
static bool g_lock_ready;
static dlph_stats g_snap;
static dlph_visit g_hist[8];
static int g_nhist;
static volatile int g_kick;

static u8 g_tx[DLPH_MAX_FRAME + 16];
static u8 g_rx[RECV_BUF];

static u64 now_ms(void) { return osGetTime(); }

static void L(const char *fmt, ...)
{
    if (!G.cfg.log) return;
    char line[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    G.cfg.log(line, G.cfg.user);
}

static void ev(dlph_event e, int arg) { if (G.cfg.event) G.cfg.event(e, arg, G.cfg.user); }

static void utf16le_to_utf8(const u8 *src, size_t units, char *dst, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < units && o + 4 < cap; i++) {
        u32 c = (u32)src[2 * i] | ((u32)src[2 * i + 1] << 8);
        if (!c) break;
        if (c >= 0xD800 && c < 0xE000) c = '?';
        if (c < 0x20) c = '?';
        if (c < 0x80) dst[o++] = (char)c;
        else if (c < 0x800) { dst[o++] = (char)(0xC0 | (c >> 6)); dst[o++] = (char)(0x80 | (c & 0x3F)); }
        else { dst[o++] = (char)(0xE0 | (c >> 12)); dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); dst[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    dst[o] = 0;
}

static u32 rd_be16(const u8 *p) { return (u32)((p[0] << 8) | p[1]); }
static u32 rd_be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 rd_le32(const u8 *p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static dlph_state state_of(void);

// the snapshot the screen reads (any thread): called by the radio thread after every poll and every 16 fragments
static void publish(void)
{
    if (!g_lock_ready) return;
    u64 t = now_ms();
    if (!G.spd_at) { G.spd_at = t; G.spd_bytes = G.bytes_sent; }
    else if (t - G.spd_at >= 1000) {
        G.mibps = (float)(G.bytes_sent - G.spd_bytes) / 1048576.0f / ((float)(t - G.spd_at) / 1000.0f);
        G.spd_at = t; G.spd_bytes = G.bytes_sent;
    }
    dlph_stats s;
    memset(&s, 0, sizeof(s));
    s.state = state_of();
    s.door_closed = G.blocked;
    s.remaking = G.remaking || (G.want_up && !G.up);
    s.served = G.served; s.lost = G.lost; s.remade = G.remade;
    s.room_ms = (G.up && G.room_at && t > G.room_at) ? (u32)(t - G.room_at) : 0;
    s.bytes_sent = G.bytes_sent;
    s.child_version = G.ver; s.child_tid = G.tid; s.child_bytes = G.cia_size; s.transfer_bytes = G.transfer_size;
    memcpy(s.child_name, G.child_name, sizeof(s.child_name));
    s.eta_s = -1;
    if (G.up && G.node && G.joined_at) {
        s.seat = G.node;
        memcpy(s.client_name, G.cname, sizeof(s.client_name));
        s.percent = G.percent;
        s.client_ms = t > G.joined_at ? (u32)(t - G.joined_at) : 0;
        s.silent_ms = (G.lastRx && t > G.lastRx) ? (u32)(t - G.lastRx) : 0;
        s.mibps = G.mibps;
        s.frags_total = G.total_frags;
        s.frags_sent = G.frags_sent;
        s.frags_repeated = G.frags_sent > G.frags_first ? G.frags_sent - G.frags_first : 0;
        s.frags_refused = G.refused;
        s.already_has = G.had_version;
        if (G.st == CS_CONTENT && G.content_at && G.total_frags) {
            double done = (double)G.next_ack * DLPH_BLOCK_FRAGS / (double)G.total_frags;
            double elapsed = (double)(t - G.content_at) / 1000.0;
            if (done > 1.0) done = 1.0;
            if (done >= 0.03 && elapsed >= 1.5) s.eta_s = (int)(elapsed * (1.0 - done) / done + 0.5);
        }
    }
    LightLock_Lock(&g_lock);
    g_snap = s;
    LightLock_Unlock(&g_lock);
}

// ---------------------------------------------------------------------------------------------------------
// seed of the checksum: first 4 bytes of the keystream (ps:ps key slot 0x39) with counter = IV xor MAC (verified on real frames)
static bool compute_seed(void)
{
    u8 ctr[16], zero[16] = { 0 }, ks[16] = { 0 };
    for (unsigned i = 0; i < 16; i++) ctr[i] = DLP_IV[i] ^ G.cfg.mac[i % 6];
    Result rc = PS_EncryptDecryptAes(16, zero, ks, PS_ALGORITHM_CTR_ENC, PS_KEYSLOT_39_DLP, ctr);
    if (R_FAILED(rc)) { L("PS_EncryptDecryptAes failed 0x%08lX", (unsigned long)rc); return false; }
    memcpy(G.seed, ks, 4);
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// the child CIA -> what the beacon announces
static bool load_child(void)
{
    FILE *f = fopen(G.cfg.child_path, "rb");
    if (!f) { L("cannot open %s", G.cfg.child_path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0x2040 || n > 0x02000000) { fclose(f); L("child CIA size %ld is not valid", n); return false; }
    G.cia = (u8 *)malloc((size_t)n);
    if (!G.cia) { fclose(f); L("not enough memory for the child (%ld bytes)", n); return false; }
    size_t got = fread(G.cia, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { L("short read of the child"); free(G.cia); G.cia = NULL; return false; }
    G.cia_size = (u32)n;

    const u8 *c = G.cia;
    // the section offsets are computed in 64 bits: with a corrupted header (a file that is not a CIA) 32-bit sums wrap, the meta check passes
    // and the TMD is read far outside the buffer (data abort in the radio thread). Every offset below is checked against the file size.
    u64 hdr = rd_le32(c), cert = rd_le32(c + 8), tik = rd_le32(c + 12), tmd = rd_le32(c + 16), meta = rd_le32(c + 20), content = rd_le32(c + 24);
    if (rd_le32(c + 28) != 0) { L("the child CIA claims a content of 4 GiB or more: it is not a Download Play child"); return false; }
    u64 oCert = (hdr + 63ull) & ~63ull, oTik = (oCert + cert + 63ull) & ~63ull, oTmd = (oTik + tik + 63ull) & ~63ull;
    u64 oContent = (oTmd + tmd + 63ull) & ~63ull, oMeta = (oContent + content + 63ull) & ~63ull;      // oMeta >= oTmd: one check bounds both
    if (!meta || oMeta + 0x400 + 0x24C0 + 0x1200 > G.cia_size) { L("the child CIA has no usable meta/SMDH"); return false; }
    u32 offTmd = (u32)oTmd, offMeta = (u32)oMeta, offContent = (u32)oContent;
    G.transfer_size = offMeta;                          // what the client really downloads: everything before the meta block

    u32 sig = rd_be32(c + offTmd) & 0xFFFF;
    u32 sigsz = (sig == 0x0000 || sig == 0x0003) ? 0x240 : (sig == 0x0001 || sig == 0x0004) ? 0x140 : 0x80;
    u32 th = offTmd + sigsz;
    if (sig > 5 || (u64)th + 0x9C4 + 0x30 > G.cia_size) { L("the child CIA has no usable TMD (signature type %lu)", (unsigned long)sig); return false; }
    G.tid = 0;
    for (int i = 0; i < 8; i++) G.tid = (G.tid << 8) | c[th + 0x4C + i];
    G.ver = (u16)((c[th + 0x9C] << 8) | c[th + 0x9D]);
    // a damaged child.cia (seen 2026-10-19: 3.7 KB wrong in the ticket/TMD area) announced a random title id and the clients saw nothing
    if (sig > 5 || (G.tid >> 32) != 0x00040001ULL) {
        L("the child CIA is DAMAGED or is not a Download Play child: title id %08lX%08lX, signature type %lu", (unsigned long)(G.tid >> 32), (unsigned long)G.tid, (unsigned long)sig);
        return false;
    }

    // The emulator variant of the child (NCCH flag NoCrypto, made for Azahar) must never be offered to real consoles: they would get a child that is not
    // encrypted with the zero key. The flag is readable only when the content is not encrypted with the title key (bit 0 of the type of the first TMD chunk).
    u32 ctype = (u32)((c[th + 0x9C4 + 6] << 8) | c[th + 0x9C4 + 7]);
    if (!(ctype & 1) && content >= 0x200 && !memcmp(c + offContent + 0x100, "NCCH", 4) && (c[offContent + 0x18F] & 0x04)) {
        L("the child CIA is the emulator variant (NCCH NoCrypto): it is not for real consoles");
        return false;
    }

    const u8 *smdh = c + offMeta + 0x400;
    if (memcmp(smdh, "SMDH", 4)) { L("the meta block does not hold an SMDH"); return false; }
    u8 lang = G.cfg.lang;
    if (lang == 0xFF) {
        lang = 1;                                        // CFG language order equals the SMDH title order
        if (R_SUCCEEDED(cfguInit())) { CFGU_GetSystemLanguage(&lang); cfguExit(); }
    }
    if (lang > 11) lang = 1;
    const u8 *ti = smdh + 8 + lang * 0x200;
    memcpy(G.name, ti, DLPH_NAME_BYTES);
    utf16le_to_utf8(ti, 0x40, G.child_name, sizeof(G.child_name));
    memcpy(G.desc, ti + 0x80, DLPH_DESC_BYTES);
    memcpy(G.icon, smdh + 0x24C0, DLPH_ICON_BYTES);

    // the beacon: clean (no host system version, no update list: 4 frames). The beacon of Zelda announced "system 11.17.0" and
    // consoles with an older system asked to update; with version 0 and no list every console tried so far downloads.
    dlph_beacon_desc d;
    memset(&d, 0, sizeof(d));
    dlph_beacon_zelda_constants(&d);
    dlph_beacon_set_no_cver(&d);
    d.tid = G.tid;
    d.version = G.ver;
    d.max_nodes = G.max_nodes;
    d.transfer_size = G.transfer_size;
    d.required_size = G.cia_size;
    d.name_le = G.name; d.desc_le = G.desc; d.icon_le = G.icon;
    G.nbc = dlph_beacon_frames(&d, G.seed, G.bc, G.bclen);
    L("child %08lX%08lX v%u, %lu bytes (%lu to download), beacon of %d frames, %u nodes, %s room", (unsigned long)(G.tid >> 32), (unsigned long)G.tid, G.ver,
      (unsigned long)G.cia_size, (unsigned long)G.transfer_size, G.nbc, G.max_nodes, G.persistent ? "persistent" : "re-made");
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// the room
static void net_stop(void)
{
    if (!G.up) return;
    udsUnbind(&G.bind);
    udsDestroyNetwork();
    G.up = false;
}

static bool net_start(void)
{
    if (G.up) return true;
    udsNetworkStruct ns;
    udsGenerateDefaultNetworkStruct(&ns, WLANCOMM_ID, NET_ID8, G.max_nodes);
    ns.unk_x15 = 0x01;                                   // seen in the real beacon
    Result rc = udsCreateNetwork(&ns, PASSPHRASE, sizeof(PASSPHRASE), &G.bind, CLIENT_CHANNEL, RECV_BUF);
    if (R_FAILED(rc)) { L("udsCreateNetwork -> 0x%08lX", (unsigned long)rc); return false; }
    PS_GenerateRandomBytes(G.pass9, sizeof(G.pass9));
    G.up = true;
    G.st = CS_NONE; G.node = 0;
    G.lastBcast = 0;
    G.percent = 0;
    G.blocked = false; G.foreign_mask = 0; memset(G.foreign_at, 0, sizeof(G.foreign_at));
    G.cname[0] = 0; G.joined_at = 0; G.content_at = 0; G.hi = G.frags_first = G.frags_sent = 0; G.room_at = now_ms();
    G.refused = 0; G.had_version = false;
    memset(G.dirty_until, 0, sizeof(G.dirty_until));     // a new network has every seat clean
    G.selSent = G.selFail = G.selOff = 0;
    return true;
}

static void remake(bool stalled)
{
    L("re-making the room%s", stalled ? " (the client was stuck)" : "");
    G.st = CS_NONE; G.node = 0; G.joined_at = 0; G.cname[0] = 0; G.percent = 0; G.had_version = false;      // the console is gone: the screen must not show it for the next 1-4 s
    G.remaking = true;
    publish();
    net_stop();
    bool ok = net_start();
    G.remaking = false;
    if (ok) { G.remade++; ev(DLPH_EV_ROOM_REMADE, stalled ? 1 : 0); ev(DLPH_EV_ROOM_OPEN, 0); }
    else { L("the room could not be re-made: trying again in 2 s"); G.retry_at = now_ms(); }
    publish();
}

// one console less in the room: it goes into the history for the screen
static void visit_end(dlph_result r)
{
    if (!G.joined_at) return;
    dlph_visit v;
    memset(&v, 0, sizeof(v));
    memcpy(v.name, G.cname, sizeof(v.name));
    v.result = r;
    v.percent = G.percent;
    v.already_had = G.had_version;
    u64 t = now_ms();
    v.duration_ms = t > G.joined_at ? (u32)(t - G.joined_at) : 0;
    v.number = ++G.visit_no;
    L("console #%lu '%s': %s, %d%%, %lu s, %lu of %lu fragments sent (%lu again, %lu refused by the radio)%s", (unsigned long)v.number, v.name,
      r == DLPH_V_SERVED ? "served" : r == DLPH_V_LOST ? "lost" : "sent away", v.percent, (unsigned long)(v.duration_ms / 1000), (unsigned long)G.frags_sent,
      (unsigned long)G.total_frags, (unsigned long)(G.frags_sent > G.frags_first ? G.frags_sent - G.frags_first : 0), (unsigned long)G.refused,
      G.had_version ? ", it already had this version" : "");
    if (g_lock_ready) {
        LightLock_Lock(&g_lock);
        memmove(&g_hist[1], &g_hist[0], sizeof(g_hist) - sizeof(g_hist[0]));
        g_hist[0] = v;
        if (g_nhist < 8) g_nhist++;
        LightLock_Unlock(&g_lock);
    }
    G.joined_at = 0;
}

// a console that had answered as a Download Play console is gone without the app (or was sent away)
static void lost_event(dlph_result r)
{
    if (!G.joined_at) return;
    G.lost++;
    visit_end(r);
    ev(DLPH_EV_CLIENT_LOST, 0);
}

// the Download Play console is gone (or must go): room of 2 -> new room; persistent room -> its seat is dirty for a while
static void client_gone(bool dirty)
{
    G.cname[0] = 0; G.joined_at = 0; G.content_at = 0; G.hi = G.frags_first = G.frags_sent = 0;
    if (!G.persistent) { remake(false); return; }
    if (dirty && G.node) G.dirty_until[G.node] = now_ms() + G.quarantine_ms;
    G.st = CS_NONE; G.node = 0; G.percent = 0; G.lastBcast = 0;
}

// send the console away now: room of 2 -> new room; persistent room -> ejected, its seat is dirty, and it is ignored until it is really gone
static void kick_client(void)
{
    if (!G.persistent) { remake(false); return; }
    udsEjectClient((u16)G.node);
    G.foreign_mask |= (u16)(1u << (G.node - 1)); G.foreign_at[G.node] = now_ms() + 3600000ULL;
    client_gone(true);
}

// persistent room: while the lowest free seat is dirty a console that arrives would land on it and stay half-connected for ~45 s.
// Close the door (and the title, which is only sent while the door is open) until that seat is clean. Consoles already inside are not touched.
static void manage_seats(u16 mask, u64 t)
{
    int low = 0;
    for (int k = 2; k <= G.max_nodes; k++) if (!((mask >> (k - 1)) & 1)) { low = k; break; }
    bool want = low && G.dirty_until[low] > t;
    if (want == G.blocked) return;
    Result rc = udsSetNewConnectionsBlocked(want, true, false);
    G.blocked = want;
    if (want) L("seat %d was used %llu s ago: closed to new consoles until it is clean (rc 0x%08lX)", low, (unsigned long long)((G.quarantine_ms - (G.dirty_until[low] - t)) / 1000), (unsigned long)rc);
    else L("the lowest free seat is clean: open to new consoles again (rc 0x%08lX)", (unsigned long)rc);
}

// ---------------------------------------------------------------------------------------------------------
// sending
static Result send_to_client(size_t len)
{
    Result rc = udsSendTo((u16)G.node, CLIENT_CHANNEL, 0, g_tx, len);
    if (rc == SENDTO_NOROOM) { svcSleepThread(2000000LL); rc = udsSendTo((u16)G.node, CLIENT_CHANNEL, 0, g_tx, len); }   // radio queue full: once more
    return rc;
}

static void send_broadcast(void)
{
    for (int k = 0; k < G.nbc; k++) {
        memcpy(g_tx, G.bc[k], G.bclen[k]);
        udsSendTo(UDS_BROADCAST_NETWORKNODEID, BCAST_CHANNEL, 0, g_tx, G.bclen[k]);
    }
}

static void send_frag(u32 blk, u32 k)
{
    u32 idx = blk * DLPH_BLOCK_FRAGS + k;
    if (idx >= G.total_frags) return;
    u32 off = idx * DLPH_FRAG_SIZE;
    u32 n = G.transfer_size - off;
    if (n > DLPH_FRAG_SIZE) n = DLPH_FRAG_SIZE;
    size_t len = dlph_pk_data(g_tx, G.seed, blk, (u16)k, G.cia + off, (u16)n);
    if (R_FAILED(send_to_client(len))) G.refused++;                  // the radio queue was full twice: counted as sent, the console will ask again
    G.bytes_sent += n;
    G.frags_sent++;
    if (idx >= G.hi) { G.hi = idx + 1; G.frags_first++; }
    if ((G.frags_sent & 15) == 0) publish();
    if (G.gap_us > 0) svcSleepThread((s64)G.gap_us * 1000LL);       // the radio share we give away
}

static void send_block(void)
{
    for (u32 k = 0; k < DLPH_BLOCK_FRAGS; k++) {
        if (G.block * DLPH_BLOCK_FRAGS + k >= G.total_frags) break;
        send_frag(G.block, k);
    }
}

static void set_percent(int p)
{
    if (p > 100) p = 100;
    if (p == G.percent) return;
    G.percent = p;
    ev(DLPH_EV_PROGRESS, p);
}

// The client, when a block is incomplete, sends an ack (type 5) with a bitmap of 4*ceil(n/32) bytes (length at +0x1A): a set bit is a
// missing fragment, byte k bit b = fragment 8k+b (confirmed on hardware 2026-10-19). Only those are sent again. If it has no effect
// several times in a row the whole block is sent again, as before.
static bool selective_resend(const u8 *p, size_t len)
{
    u32 blen = rd_be16(p + 0x1A);
    if (blen < 4 || blen > 64 || 0x1C + blen > len) return false;
    u64 t = now_ms();
    if (G.selSent && t - G.selAt < 150) return true;                  // an ack written before we finished sending: old news
    u32 blk = G.block - 1;
    u32 nfrag = G.total_frags - blk * DLPH_BLOCK_FRAGS;
    if (nfrag > DLPH_BLOCK_FRAGS) nfrag = DLPH_BLOCK_FRAGS;
    if (G.selSent && blen == G.lastBmLen && !memcmp(p + 0x1C, G.lastBm, blen)) {      // nothing changed after the last send
        if (++G.selFail >= 4) { G.selOff = 1; L("selective resend gives up on block %lu", (unsigned long)blk); return false; }
    } else if (G.selSent) G.selFail = 0;
    u16 list[DLPH_BLOCK_FRAGS];
    u32 nl = 0;
    for (u32 k = 0; k < blen; k++)
        for (int b = 0; b < 8; b++)
            if (((p[0x1C + k] >> b) & 1) && 8 * k + b < nfrag && nl < DLPH_BLOCK_FRAGS) list[nl++] = (u16)(8 * k + b);
    if (!nl) return false;
    for (u32 x = 0; x < nl; x++) send_frag(blk, list[x]);
    G.selAt = now_ms();
    memcpy(G.lastBm, p + 0x1C, blen); G.lastBmLen = (u8)blen;
    G.selSent = 1;
    G.retries = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// what the client sends
static void on_packet(const u8 *p, size_t len)
{
    if (len < 0x10 || !dlph_check(G.seed, p, len)) return;
    G.lastRx = now_ms();
    u8 type = p[0];

    if (type == 2) {                                                  // answer to our auth: it is in, start the distribution
        if (G.st != CS_AUTH) return;
        G.rid[0] = p[0x16]; G.rid[1] = p[0x17];
        G.seq++;
        size_t n = dlph_pk_startdist(g_tx, G.seed, G.seq, G.rid);
        send_to_client(n);
        G.st = CS_DIST_ACK;
        G.lastTx = now_ms();
        L("seat %d is a Download Play console: distribution started", G.node);
        G.joined_at = now_ms(); G.cname[0] = 0; G.name_tries = 0; G.name_try = 0;
        G.hi = G.frags_first = G.frags_sent = 0; G.content_at = 0; G.percent = 0; G.refused = 0; G.had_version = false;
        ev(DLPH_EV_CLIENT_JOINED, G.node);
    } else if (type == 3) {                                           // does it need the content?
        if (G.st != CS_DIST_ACK || len < 0x18) return;
        G.seq++;
        G.block = 0; G.sent_req = false; G.next_ack = 0; G.retries = 0;
        G.total_frags = (G.transfer_size + DLPH_FRAG_SIZE - 1) / DLPH_FRAG_SIZE;
        G.lastRx = now_ms(); G.wdAt = 0;
        if (rd_be16(p + 0x14) == 0x20) { G.can_next = true; G.st = CS_CONTENT; G.content_at = now_ms(); set_percent(1); }
        else { G.can_next = false; G.st = CS_NOCONTENT; G.had_version = true; L("the client already has this version (field 0x%04lX): nothing to send", (unsigned long)rd_be16(p + 0x14)); }
    } else if (type == 5) {                                           // finish ack: how far is the client
        if (G.st != CS_CONTENT && G.st != CS_NOCONTENT) return;
        if (len < 0x1C) return;
        u8 finished = p[0x14], needs = p[0x15];
        u32 ackSeq = rd_be32(p + 0x16);
        if (!needs) {
            G.seq++;
            bool had = G.st == CS_NOCONTENT;
            G.st = CS_DONE; G.sent_req = false; G.pings = 0; G.doneAt = now_ms();
            if (!had) set_percent(100);                                   // a console that already had the app received nothing: no 100%
            L(had ? "nothing to deliver: the console already has this version" : "content delivered");
            return;
        }
        if (G.st == CS_NOCONTENT) return;                                 // nothing to send, so no block to acknowledge (speculative: only 0x20 and 0 were ever seen)
        G.seq++;
        if (G.block != ackSeq) {                                      // the block in flight is not complete
            G.sent_req = false;
            G.lastTx = now_ms();
            if (!G.selOff && G.block > 0 && ackSeq == G.block - 1 && finished != 1 && len >= 0x20 && selective_resend(p, len)) return;
            if (++G.retries > 10 && G.block > 0) { G.block--; G.retries = 0; G.can_next = true; }      // old rule: ten acks without change -> the whole block
            return;
        }
        G.selSent = G.selFail = G.selOff = 0;
        G.can_next = true;
        G.next_ack++;
        G.retries = 0;
        if (G.total_frags) set_percent((int)((u64)G.next_ack * DLPH_BLOCK_FRAGS * 100 / G.total_frags));
    } else if (type == 6) {
        if (G.st == CS_DONE || G.st == CS_FINAL) G.seq++;
    }
}

static void pump_rx(void)
{
    size_t got = 0;
    u16 src = 0;
    for (int i = 0; i < 64; i++) {                                    // until the queue is empty (the bind event is one-shot: one pull per signal would strand packets)
        got = 0;
        if (R_FAILED(udsPullPacket(&G.bind, g_rx, sizeof(g_rx), &got, &src)) || got == 0) break;
        if (G.node && src == G.node) on_packet(g_rx, got);
    }
}

// ---------------------------------------------------------------------------------------------------------
static u64 silent_ms(void)
{
    u64 n = now_ms();
    return (G.lastRx && n > G.lastRx) ? n - G.lastRx : 0;     // never negative: lastRx can be newer than the `t` taken at the top of poll
}

static dlph_state state_of(void)
{
    if (!G.up) return DLPH_ROOM_DOWN;
    switch (G.st) {
    case CS_NONE: return DLPH_ROOM_OPEN;
    case CS_AUTH: case CS_DIST_ACK: return DLPH_CLIENT_JOINING;
    case CS_CONTENT: case CS_NOCONTENT: return DLPH_CLIENT_DOWNLOADING;
    default: return DLPH_CLIENT_FINISHING;
    }
}

static void poll_inner(void)
{
    if (!G.up) {
        if (!G.want_up || now_ms() < G.retry_at + 2000) return;                   // the room was lost (a failed re-make, the system took the network down): try again now and then
        G.retry_at = now_ms();
        if (net_start()) { L("the room is back"); G.remade++; ev(DLPH_EV_ROOM_REMADE, 0); ev(DLPH_EV_ROOM_OPEN, 0); }
        else L("the room could not be made again: trying again in 2 s");
        return;
    }
    udsConnectionStatus cs;
    if (R_FAILED(udsGetConnectionStatus(&cs))) return;
    u64 t = now_ms();
    u16 mask = cs.node_bitmask;                                                   // bit k-1 = node k (bit 0 is this console)
    G.foreign_mask &= mask;                                                       // a foreign node that left is forgotten

    pump_rx();
    t = now_ms();                                                                 // the packets just handled stamped times newer than the `t` above: take it again
    if (g_kick) {                                                                 // "send this console away" from the screen
        g_kick = 0;
        if (G.st != CS_NONE && G.node) {
            L("seat %d sent away on request", G.node);
            lost_event(DLPH_V_KICKED);
            kick_client();
            return;
        }
    }
    if (G.persistent) manage_seats(mask, t);

    if (G.st == CS_NONE) {
        int cand = 0;                                                             // the first node that is neither ours nor known as foreign
        for (int k = 2; k <= G.max_nodes; k++) if (((mask >> (k - 1)) & 1) && !((G.foreign_mask >> (k - 1)) & 1)) { cand = k; break; }
        if (!cand)                                                                // nobody new: a foreign node that has been quiet for long gets another try
            for (int k = 2; k <= G.max_nodes; k++)
                if (((G.foreign_mask >> (k - 1)) & 1) && t - G.foreign_at[k] >= FOREIGN_RETRY_MS) { cand = k; G.foreign_mask &= (u16)~(1u << (k - 1)); break; }
        if (!cand) {
            if (!G.blocked && t - G.lastBcast >= 150) { send_broadcast(); G.lastBcast = t; }
            return;
        }
        G.node = cand; G.st = CS_AUTH; G.seq = 0; G.lastTx = 0; G.since = t; G.lastRx = t;
        G.selSent = G.selFail = G.selOff = 0;
        L("a console entered seat %d", cand);
    }

    bool present = (mask >> (G.node - 1)) & 1;
    if (!present) {                                                               // it left: delivered (it is going to install) or lost
        bool served = (G.st == CS_DONE || G.st == CS_FINAL);
        bool ours = (G.st != CS_AUTH);                                            // it answered auth: it was a Download Play console
        L(served ? "the client left with the app" : ours ? "the client left before the end" : "the node left before answering");
        if (served) { G.served++; visit_end(DLPH_V_SERVED); ev(DLPH_EV_CLIENT_SERVED, 0); } else if (ours) lost_event(DLPH_V_LOST);
        client_gone(ours);
        if (G.persistent) manage_seats(mask, now_ms());                           // close the door at once if the lowest free seat is the one it used
        return;
    }

    if (G.joined_at && !G.cname[0] && G.name_tries < 12 && t - G.name_try >= 500) {             // the user name of the console
        udsNodeInfo ni;
        char un[32];
        memset(un, 0, sizeof(un));
        G.name_try = t; G.name_tries++;
        if (R_SUCCEEDED(udsGetNodeInformation((u16)G.node, &ni)) && R_SUCCEEDED(udsGetNodeInfoUsername(&ni, un)) && un[0]) {
            for (int i = 0; i < 31 && un[i]; i++) if ((unsigned char)un[i] < 0x20) un[i] = '?';
            memcpy(G.cname, un, 31); G.cname[31] = 0;
            L("seat %d is '%s'", G.node, G.cname);
            ev(DLPH_EV_CLIENT_NAME, G.node);
        }
    }

    switch (G.st) {
    case CS_AUTH:
        if (G.persistent) {
            if (t - G.since >= NOTOURS_MS) {                                      // somebody else's node (a 3DS Link receiver): leave it alone
                G.foreign_mask |= (u16)(1u << (G.node - 1)); G.foreign_at[G.node] = t;
                L("node %d does not answer as a Download Play console: not ours (asked again in %d s)", G.node, FOREIGN_RETRY_MS / 1000);
                G.st = CS_NONE; G.node = 0;
                return;
            }
        } else if (t - G.since >= HANDSHAKE_MS) { remake(true); return; }
        if (t - G.lastTx >= 100) { send_to_client(dlph_pk_auth(g_tx, G.seed)); G.lastTx = t; }
        break;
    case CS_DIST_ACK:
        if (t - G.since >= HANDSHAKE_MS) { lost_event(DLPH_V_LOST); remake(true); return; }
        if (t >= G.lastTx && t - G.lastTx >= 400) { send_to_client(dlph_pk_startdist(g_tx, G.seed, G.seq, G.rid)); G.lastTx = t; }
        break;
    case CS_CONTENT:
        if (silent_ms() >= STALL_MS && now_ms() >= G.wdAt + STALL_MS) {           // the client has gone quiet
            G.wdAt = now_ms();
            if (silent_ms() >= REMAKE_MS) { L("the client was silent for %llu ms", (unsigned long long)silent_ms()); lost_event(DLPH_V_LOST); remake(true); return; }
            if (G.block > 0) { L("the client was silent for %llu ms: sending the block again", (unsigned long long)silent_ms()); G.block--; G.can_next = true; G.sent_req = false; }
        }
        if (!G.can_next) {
            if (!G.sent_req) {
                if (t - G.lastTx >= 150) { send_to_client(dlph_pk_finish_req(g_tx, G.seed, G.seq, G.rid, G.next_ack)); G.sent_req = true; G.lastTx = t; }
            } else if (t - G.lastTx >= 500) G.sent_req = false;                   // no answer: ask again
        } else {
            send_block();
            G.can_next = false; G.sent_req = false;
            G.block++;
            G.lastTx = now_ms();
        }
        break;
    case CS_NOCONTENT:
        if (silent_ms() >= REMAKE_MS) { L("the client was silent for %llu ms after saying it has the app", (unsigned long long)silent_ms()); lost_event(DLPH_V_LOST); remake(true); return; }
        if (t - G.lastTx >= 100) { send_to_client(dlph_pk_finish_req(g_tx, G.seed, G.seq, G.rid, 0)); G.lastTx = t; }
        break;
    case CS_DONE:                                                                 // BeginGame is the host's decision: a few pings, then the final packet
        if (t - G.lastTx >= 100) {
            send_to_client(dlph_pk_begin_ping(g_tx, G.seed, G.seq, G.rid));
            G.lastTx = t; G.pings++;
            if (G.pings >= 3) {
                send_to_client(dlph_pk_begin_final(g_tx, G.seed, G.seq, G.rid, G.pass9));
                G.finals = 1; G.finalAt = now_ms();
                G.st = CS_FINAL;
            }
        }
        break;
    case CS_FINAL:
        if (G.finals < 4 && t - G.finalAt >= 500) { send_to_client(dlph_pk_begin_final(g_tx, G.seed, G.seq, G.rid, G.pass9)); G.finals++; G.finalAt = t; }
        if (t - G.doneAt >= FINAL_MS) {                                           // it has the app but does not leave: put it out
            G.served++; visit_end(DLPH_V_SERVED); ev(DLPH_EV_CLIENT_SERVED, 0);
            kick_client();
            return;
        }
        break;
    default: break;
    }
}

void dlph_poll(void)
{
    poll_inner();
    publish();
}

// ---------------------------------------------------------------------------------------------------------
bool dlph_open(const dlph_cfg *cfg)
{
    if (G.opened) dlph_close();
    if (!g_lock_ready) { LightLock_Init(&g_lock); g_lock_ready = true; }
    LightLock_Lock(&g_lock);
    memset(&g_snap, 0, sizeof(g_snap)); memset(g_hist, 0, sizeof(g_hist)); g_nhist = 0;
    LightLock_Unlock(&g_lock);
    g_kick = 0;
    memset(&G, 0, sizeof(G));
    G.cfg = *cfg;
    G.gap_us = cfg->gap_us;
    G.persistent = cfg->persistent;
    G.max_nodes = cfg->max_nodes < 2 ? 2 : cfg->max_nodes > 8 ? 8 : cfg->max_nodes;
    G.quarantine_ms = cfg->quarantine_ms ? cfg->quarantine_ms : QUARANTINE_MS;
    if (R_FAILED(psInit())) { L("psInit failed"); return false; }
    if (!compute_seed() || !load_child()) { psExit(); free(G.cia); G.cia = NULL; return false; }
    G.opened = true;
    publish();
    return true;
}

bool dlph_start(void)
{
    if (!G.opened) return false;
    if (!net_start()) return false;
    G.want_up = true;
    L("room open (title %08lX%08lX)", (unsigned long)(G.tid >> 32), (unsigned long)G.tid);
    ev(DLPH_EV_ROOM_OPEN, 0);
    publish();
    return true;
}

void dlph_stop(void) { G.want_up = false; net_stop(); G.st = CS_NONE; G.node = 0; G.joined_at = 0; G.cname[0] = 0; publish(); }

void dlph_close(void)
{
    if (!G.opened) return;
    net_stop();
    free(G.cia);
    psExit();
    memset(&G, 0, sizeof(G));
    publish();
}

void dlph_set_gap_us(int gap_us) { G.gap_us = gap_us < 0 ? 0 : gap_us; }

dlph_stats dlph_get_stats(void)
{
    dlph_stats s;
    memset(&s, 0, sizeof(s));
    if (!g_lock_ready) return s;
    LightLock_Lock(&g_lock);
    s = g_snap;
    LightLock_Unlock(&g_lock);
    return s;
}

int dlph_history(dlph_visit *out, int max)
{
    if (!g_lock_ready || !out || max <= 0) return 0;
    LightLock_Lock(&g_lock);
    int n = g_nhist < max ? g_nhist : max;
    memcpy(out, g_hist, (size_t)n * sizeof(dlph_visit));
    LightLock_Unlock(&g_lock);
    return n;
}

void dlph_request_kick(void) { g_kick = 1; }

const char *dlph_state_text(dlph_stats s)
{
    if (s.remaking) return "Re-making the room: one moment";
    switch (s.state) {
    case DLPH_ROOM_DOWN: return "Room closed";
    case DLPH_ROOM_OPEN: return s.door_closed ? "Room open: getting ready for the next console" : "Room open: waiting for a console";
    case DLPH_CLIENT_JOINING: return "A console is joining";
    case DLPH_CLIENT_DOWNLOADING: return s.already_has ? "This console already has this version" : "Sending 3DS Link";
    default: return s.already_has ? "This console already has this version" : "Delivered: the console is leaving to install";
    }
}
