// DlpLinkHostDemo: the smallest program that uses the host library (dlph). It is also the hardware test of the library and of the
// information a "3DS Link Delivery" server screen has to show (the bottom screen here is the plain-text version of that screen):
//   A = ROOM OF 2 (3DS Link 1.0): one console at a time, the room re-made after every console and after a stuck download.
//   B = PERSISTENT ROOM OF 8 (not in 1.0): nothing is kicked; a seat used by a Download Play console is closed to new consoles for 45 s.
// gap_us = 3000 (10 s per download on hardware; 0 was slower). SELECT = send the console being served away (what "kick" will be on the real screen). START = exit
// (the room is closed). Files: sdmc:/3ds/DlpTest/child.cia (the child to deliver), log sdmc:/3ds/DlpTest/dlph_demo_log.txt.
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "dlph.h"
#include "../../../../AzaharLink/ConfigApp/titleshare_host/builtin_keys.h"   // the IV of the checksum, as the app takes it (keys_glue.c compiles the module here)

static FILE *g_log;
static PrintConsole g_top, g_bot;

static void logline(const char *line, void *user)
{
    (void)user;
    consoleSelect(&g_top);
    printf("%s\n", line);
    if (g_log) { fprintf(g_log, "[%7llu] %s\n", (unsigned long long)(osGetTime() % 10000000ULL), line); fflush(g_log); }
}

static void on_event(dlph_event e, int arg, void *user)
{
    static const char *NAME[] = { "ROOM_OPEN", "CLIENT_JOINED", "PROGRESS", "CLIENT_SERVED", "CLIENT_LOST", "ROOM_REMADE", "CLIENT_NAME" };
    (void)user;
    if (e == DLPH_EV_PROGRESS) { if (arg % 10 == 0 || arg >= 99) { char b[48]; snprintf(b, sizeof(b), "event PROGRESS %d%%", arg); logline(b, NULL); } return; }
    char b[64];
    snprintf(b, sizeof(b), "event %s %d", NAME[e], arg);
    logline(b, NULL);
}

static bool read_mac(u8 mac[6])
{
    u32 *buf = memalign(0x1000, 0x100000);
    if (!buf) return false;
    Result rc = socInit(buf, 0x100000);
    if (R_FAILED(rc)) { free(buf); return false; }
    socklen_t len = 6;
    rc = SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_MAC_ADDRESS, mac, &len);
    socExit();
    free(buf);
    return R_SUCCEEDED(rc);
}

// the bottom screen: what the server screen of 3DS Link Delivery shows (the real one is drawn by the app)
static void draw_status(bool persistent)
{
    static const char *RES[] = { "served", "lost", "sent away" };
    dlph_stats s = dlph_get_stats();
    dlph_visit h[8];
    int nh = dlph_history(h, 8);
    consoleSelect(&g_bot);
    consoleClear();
    printf("3DS Link Delivery - %s\n\n", persistent ? "persistent room (B)" : "room of 2 (A)");
    printf("%s\n\n", dlph_state_text(s));
    printf("Offered: %s v%u, %.2f MiB\n", s.child_name, (unsigned)s.child_version, (double)s.child_bytes / 1048576.0);
    printf("Room: %lu s old, re-made %lu x\n", (unsigned long)(s.room_ms / 1000), (unsigned long)s.remade);
    printf("Served %lu   Lost %lu\n\n", (unsigned long)s.served, (unsigned long)s.lost);
    if (s.seat) {
        printf("Console: %s (seat %d)\n", s.client_name[0] ? s.client_name : "(name not known yet)", s.seat);
        printf("In the room %lu s, last packet %.1f s ago\n", (unsigned long)(s.client_ms / 1000), (double)s.silent_ms / 1000.0);
        if ((s.state == DLPH_CLIENT_DOWNLOADING || s.state == DLPH_CLIENT_FINISHING) && !s.already_has) {
            printf("Download %d%%   %.2f MiB/s\n", s.percent, (double)s.mibps);
            if (s.eta_s >= 0) printf("About %d s left\n", s.eta_s); else printf("Time left: not known yet\n");
            printf("Repeated %lu of %lu, refused by radio %lu\n", (unsigned long)s.frags_repeated, (unsigned long)s.frags_total, (unsigned long)s.frags_refused);
        }
    } else {
        printf("Nobody in the room\n");
    }
    printf("\nLast consoles:\n");
    for (int i = 0; i < nh && i < 5; i++)
        printf(" #%lu %s: %s%s, %d%%, %lu s\n", (unsigned long)h[i].number, h[i].name[0] ? h[i].name : "(no name)", RES[h[i].result], h[i].already_had ? " (had it)" : "", h[i].percent, (unsigned long)(h[i].duration_ms / 1000));
    printf("\nSELECT = send the console away\nSTART = close the room and exit\n");
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &g_top);
    consoleInit(GFX_BOTTOM, &g_bot);
    consoleSelect(&g_top);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/DlpTest", 0777);
    g_log = fopen("sdmc:/3ds/DlpTest/dlph_demo_log.txt", "a");
    logline("=== DlpLinkHostDemo (dlph library) ===", NULL);
    consoleSelect(&g_bot);
    printf("\nA = room of 2 (re-made after every console)\n    this is 3DS Link 1.0\nB = persistent room of 8 (nothing is kicked,\n    a used seat is closed for 45 s)\nSTART = exit\n");
    bool persistent = false, chosen = false;
    while (aptMainLoop() && !chosen) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_START) { gfxExit(); return 0; }
        if (k & KEY_A) { persistent = false; chosen = true; }
        if (k & KEY_B) { persistent = true; chosen = true; }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    if (!chosen) { gfxExit(); return 0; }
    logline(persistent ? "mode B: persistent room of 8, quarantine 45 s" : "mode A: room of 2, re-made after every console", NULL);

    dlph_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.child_path = "sdmc:/3ds/DlpTest/child.cia";
    cfg.lang = 0xFF;
    cfg.gap_us = 3000;                                      // measured: 3000 = 10 s per download; 0 = 17 s and 2.4 x the bytes (the radio is the limit)
    cfg.persistent = persistent;
    cfg.max_nodes = persistent ? 8 : 2;
    cfg.log = logline;
    cfg.event = on_event;
    bool ok = read_mac(cfg.mac);
    bk_get(BK_DLP_CHECKSUM_IV, cfg.iv);
    if (!ok) logline("cannot read the MAC address", NULL);
    if (ok) ok = R_SUCCEEDED(udsInit(0x4000, NULL));
    if (ok) ok = dlph_open(&cfg);
    if (ok) ok = dlph_start();
    logline(ok ? "SELECT = send the console away, START = exit" : "NOT STARTED (see the lines above). START = exit", NULL);

    u64 lastStat = 0, lastDraw = 0;
    u16 lastMask = 0xFFFF;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;
        if (ok) {
            if (down & KEY_SELECT) dlph_request_kick();
            dlph_poll();
            udsConnectionStatus cs;
            if (R_SUCCEEDED(udsGetConnectionStatus(&cs)) && cs.node_bitmask != lastMask) {      // who is in the room, for the analysis of the log
                char b[64];
                lastMask = cs.node_bitmask;
                snprintf(b, sizeof(b), "nodes in the room: mask 0x%04X", (unsigned)lastMask);
                logline(b, NULL);
            }
            u64 t = osGetTime();
            if (t - lastDraw >= 250) { lastDraw = t; draw_status(persistent); }
            if (t - lastStat >= 10000) {
                lastStat = t;
                dlph_stats s = dlph_get_stats();
                char b[128];
                snprintf(b, sizeof(b), "stats: state %d, %d%%, served %lu, lost %lu, room re-made %lu, %lu KB sent, %.2f MiB/s", (int)s.state, s.percent,
                         (unsigned long)s.served, (unsigned long)s.lost, (unsigned long)s.remade, (unsigned long)(s.bytes_sent / 1024), (double)s.mibps);
                logline(b, NULL);
            }
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    if (ok) { dlph_stop(); dlph_close(); udsExit(); }
    logline("=== end ===", NULL);
    if (g_log) fclose(g_log);
    gfxExit();
    return 0;
}
