// PC test of the Download Play host library (dlph.c) against a simulated stock client and a simulated nwm.
// The client answers like the real one seen in the hardware logs: auth reply, "I need the content", fragments with loss, the
// "finish ack" with a bitmap of the missing fragments, the final BeginGame packet and then it LEAVES. It can also go silent.
// The simulated nwm has the behaviour measured on hardware 2026-10-19/20: a seat freed by a Download Play console is DIRTY for 44 s - a console
// that lands on it stays half-connected (it cannot talk) until then.
//   ROOM OF 2 (persistent = false): healthy downloads one after another with the room re-made, loss 1% and 3%, a client that goes silent
//   PERSISTENT ROOM OF 8: nothing is kicked; the seat of a console that left is closed for 45 s; a console that arrives at once is served
//   afterwards; without the quarantine it would land on the dirty seat; a node that is not a Download Play console is left alone.
// Run from the repo root: gcc -Wall -I DlpLink/host/tests -I DlpLink/host -o t DlpLink/host/tests/dlph_test.c && ./t DlpLink/probe/out/ChildProbe.cia
#include "../dlph.c"
#include "../dlph_wire.c"

#include <assert.h>

static u64 g_now = 1000000;
u64 osGetTime(void) { return g_now; }
void svcSleepThread(s64 ns) { g_now += (u64)(ns / 1000000); }
Result udsInit(size_t s, const char *u) { (void)s; (void)u; return 0; }
void udsExit(void) {}
void udsGenerateDefaultNetworkStruct(udsNetworkStruct *n, u32 id, u8 id8, u8 max) { memset(n, 0, sizeof(*n)); n->wlancommID = id; n->id8 = id8; n->max_nodes = max; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

// ---- the "air"
static bool net_up, client_joined, net_blocked;
static int client_node = 2, foreign_node, rooms_created, expect_nodes = 2, blocked_calls;
static u64 last_leave[10];
static struct { u8 d[RECV_BUF]; size_t n; u16 src; } rxq[256];
static int rxq_h, rxq_t;
static bool rx_signal;                                        // the bind event: one-shot, set when a packet arrives, cleared by a successful wait
static int fail_create_n;                                     // make the next udsCreateNetwork calls fail
static bool g_has_version, g_mute_all;                        // the console says it already has the app / never answers
static void client_reset(void);
static u64 clean_at(int node);

Result udsCreateNetwork(const udsNetworkStruct *n, const void *p, size_t ps, udsBindContext *c, u8 ch, u32 rb)
{
    (void)c; (void)rb;
    CHECK(n->wlancommID == 0x2810 && n->id8 == 0x55 && n->unk_x15 == 1 && n->max_nodes == expect_nodes, "network identity (id 0x%X id8 0x%X unk %u nodes %u)", n->wlancommID, n->id8, n->unk_x15, n->max_nodes);
    CHECK(ps == 18 && !memcmp(p, "0km@tsa$uhmy1a0sa\0", 18) && ch == 2, "passphrase / data channel");
    g_now += 1070;                                            // udsCreateNetwork takes ~1.07 s on the console
    if (fail_create_n > 0) { fail_create_n--; return 0xC8A10001u; }
    net_up = true; net_blocked = false; rooms_created++;
    memset(last_leave, 0, sizeof(last_leave));                // a new network has every seat clean
    return 0;
}
Result udsUnbind(udsBindContext *c) { (void)c; return 0; }
Result udsDestroyNetwork(void) { net_up = false; client_joined = false; foreign_node = 0; rxq_h = rxq_t = 0; rx_signal = false; client_reset(); return 0; }
Result udsSetNewConnectionsBlocked(bool b, bool c, bool f) { (void)c; (void)f; net_blocked = b; blocked_calls++; return 0; }
static const char *g_client_name = "Marco";
Result udsGetNodeInformation(u16 id, udsNodeInfo *o)
{
    memset(o, 0, sizeof(*o));
    if (!(client_joined && id == client_node) || g_now < clean_at(client_node)) return 0xC90113FAu;       // a half-connected console has no name yet
    snprintf(o->name, sizeof(o->name), "%s", g_client_name);
    return 0;
}
Result udsEjectClient(u16 n) { if (n == client_node) { client_joined = false; last_leave[n] = g_now; } return 0; }
bool udsWaitDataAvailable(const udsBindContext *c, bool next, bool wait) { (void)c; (void)next; (void)wait; bool r = rx_signal && rxq_h != rxq_t; rx_signal = false; return r; }
Result udsPullPacket(const udsBindContext *c, void *buf, size_t size, size_t *actual, u16 *src)
{
    (void)c; (void)size;
    if (rxq_h == rxq_t) { *actual = 0; *src = 0; return 0; }
    int k = rxq_h++ % 256;
    g_now += 3;                                               // time passes while a packet is read, as on the console
    memcpy(buf, rxq[k].d, rxq[k].n); *actual = rxq[k].n; *src = rxq[k].src;
    return 0;
}
Result udsGetConnectionStatus(udsConnectionStatus *o)
{
    memset(o, 0, sizeof(*o));
    o->node_bitmask = (u16)(0x1 | (client_joined ? 1u << (client_node - 1) : 0) | (foreign_node ? 1u << (foreign_node - 1) : 0));
    return 0;
}

// ---- the simulated client
static u8 *ref, *got_data;
static u32 xfer, total_frags;
static bool downloading, complete;
static u32 blocks_done, dropped, acks, mute_after, loss_permille;
static u32 rng = 1;
static u8 got[3000];
static u8 rid[3];
static u64 next_ack_at;
static int unicast_count[8];

static u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static u32 nfrag_of(u32 blk) { u32 first = blk * DLPH_BLOCK_FRAGS; if (first >= total_frags) return 0; return total_frags - first < DLPH_BLOCK_FRAGS ? total_frags - first : DLPH_BLOCK_FRAGS; }
static u64 clean_at(int node) { return last_leave[node] ? last_leave[node] + 44000 : 0; }       // when the previous occupant's registration is forgotten

static void client_reset(void) { downloading = complete = false; blocks_done = dropped = acks = 0; memset(got, 0, sizeof(got)); next_ack_at = 0; }
static void push_from(const u8 *d, size_t n, u16 src) { int k = rxq_t++ % 256; memcpy(rxq[k].d, d, n); rxq[k].n = n; rxq[k].src = src; rx_signal = true; }
static void push(const u8 *d, size_t n)
{
    u8 junk[40];
    for (int i = 0; i < 40; i++) junk[i] = (u8)(0x55 + i * 7);
    push_from(junk, sizeof(junk), client_node == 5 ? 6 : 5);             // a packet from another node, ahead of the real one in the queue
    push_from(d, n, (u16)client_node);
}

static void client_send(u8 type, size_t size, void (*fill)(u8 *))
{
    u8 b[0x40];
    memset(b, 0, sizeof(b));
    b[0] = type; b[1] = 2; b[4] = (u8)(size >> 8); b[5] = (u8)size; b[6] = 2; memcpy(b + 13, rid, 3);
    if (fill) fill(b);
    dlph_seal(G.seed, b, size);
    push(b, size);
}
static void fill_auth(u8 *b) { b[0x10] = 1; b[0x16] = 0x01; b[0x17] = 0x02; }
static void fill_need(u8 *b) { b[0x10] = 1; b[0x14] = 0; b[0x15] = g_has_version ? 0x00 : 0x20; b[0x18] = 1; }
static void fill_ack(u8 *b)
{
    u32 nf = nfrag_of(blocks_done), missing = 0;
    bool done = nf == 0, started = false;
    for (u32 f = 0; f < nf; f++) if (got[blocks_done * DLPH_BLOCK_FRAGS + f]) { started = true; break; }
    size_t blen = 4 * ((nf + 31) / 32);
    b[0x10] = 1;
    b[0x15] = done ? 0 : 1;
    b[0x16] = (u8)(blocks_done >> 24); b[0x17] = (u8)(blocks_done >> 16); b[0x18] = (u8)(blocks_done >> 8); b[0x19] = (u8)blocks_done;
    for (u32 f = 0; f < nf && started; f++)
        if (!got[blocks_done * DLPH_BLOCK_FRAGS + f]) { missing++; b[0x1C + f / 8] |= (u8)(1u << (f % 8)); }
    b[0x14] = missing ? 2 : 1;
    if (missing) { b[0x1A] = (u8)(blen >> 8); b[0x1B] = (u8)blen; }
}
static void send_ack(void)
{
    if (g_mute_all || (mute_after && acks >= mute_after)) return;
    u32 nf = nfrag_of(blocks_done);
    bool missing = false, started = false;
    for (u32 f = 0; f < nf; f++) { if (got[blocks_done * DLPH_BLOCK_FRAGS + f]) started = true; else missing = true; }
    size_t size = (missing && started) ? 0x1C + 4 * ((nf + 31) / 32) : 0x1C;
    client_send(5, size, fill_ack);
    acks++;
}

static void client_rx(const u8 *p, size_t n)
{
    CHECK(n >= 16 && dlph_check(G.seed, p, n), "packet from the host: checksum/size (type %u len %u)", p[0], (unsigned)n);
    CHECK(((u32)p[4] << 8 | p[5]) == n, "size field");
    if (g_now < clean_at(client_node)) return;                // a seat that is still dirty: the console cannot talk yet
    unicast_count[p[0] & 7]++;
    memcpy(rid, p + 13, 3);
    if (p[0] == 2) { client_reset(); client_send(2, 0x18, fill_auth); }
    else if (p[0] == 3) { downloading = true; blocks_done = 0; total_frags = g_has_version ? 0 : (xfer + DLPH_FRAG_SIZE - 1) / DLPH_FRAG_SIZE; next_ack_at = g_now + 100; client_send(3, 0x38, fill_need); }
    else if (p[0] == 4) {
        u32 blk = (u32)p[0x14] << 24 | (u32)p[0x15] << 16 | (u32)p[0x16] << 8 | p[0x17], fi = (u32)p[0x18] << 8 | p[0x19], fs = (u32)p[0x1A] << 8 | p[0x1B];
        CHECK(n == 28 + fs, "fragment size");
        if (rnd() % 1000 < loss_permille) { dropped++; return; }
        u32 idx = blk * DLPH_BLOCK_FRAGS + fi;
        if (idx < total_frags && !got[idx]) { got[idx] = 1; memcpy(got_data + (size_t)idx * DLPH_FRAG_SIZE, p + 0x1C, fs); }
        while (nfrag_of(blocks_done)) {
            u32 nf = nfrag_of(blocks_done), ok = 0;
            for (u32 f = 0; f < nf; f++) ok += got[blocks_done * DLPH_BLOCK_FRAGS + f];
            if (ok != nf) break;
            blocks_done++;
        }
    } else if (p[0] == 5) send_ack();
    else if (p[0] == 6) { if (p[0x1D] == 9) { complete = true; downloading = false; } }
}

Result udsSendTo(u16 dst, u8 ch, u8 flags, const void *buf, size_t size)
{
    (void)flags;
    const u8 *p = buf;
    if (dst == 0xFFFF) {
        CHECK(ch == 1 && dlph_check(G.seed, p, size), "beacon frame channel/checksum");
        CHECK(!net_blocked, "the title must not be announced while the door is closed");
        CHECK(p[0] == 1 && p[13] == 4, "beacon header (total frames %u, must be 4: no system update list)", p[13]);
        if (p[12] == 0) {
            xfer = (u32)p[0x38] << 24 | (u32)p[0x39] << 16 | (u32)p[0x3A] << 8 | p[0x3B];
            CHECK(p[0x20] == expect_nodes, "frame 0 max nodes %u", p[0x20]);
            CHECK(p[0x22] == 0 && p[0x23] == 0, "frame 0 host system version must be 0");
            CHECK(((u64)p[0x10] << 56 | (u64)p[0x11] << 48 | (u64)p[0x12] << 40 | (u64)p[0x13] << 32 | (u64)p[0x14] << 24 | (u64)p[0x15] << 16 | (u64)p[0x16] << 8 | p[0x17]) == G.tid, "frame 0 title id");
        }
        CHECK(p[12] < 4, "frame %u: there must be 4 frames only", p[12]);
        return 0;
    }
    if (foreign_node && dst == foreign_node) return 0;        // somebody else's node: nobody answers
    CHECK(ch == 2 && client_joined && dst == client_node, "unicast to node %u channel %u (client in the room: %d, its seat %d)", dst, ch, client_joined, client_node);
    client_rx(p, size);
    return 0;
}

// ---- driver
static int ev_count[16];
static struct { int max_pct, seat, kick_pct; bool kicked; float max_mibps; int eta_seen, eta_min; bool name_seen; char name[32]; uint32_t rep_max, tot; int state_seen[8]; bool text_ok; int already, remaking_seen; char remaking_text[48]; } obs;
static void observe(void)
{
    dlph_stats st = dlph_get_stats();
    if (st.seat) { obs.seat = st.seat; if (st.client_name[0] && !obs.name_seen) { obs.name_seen = true; snprintf(obs.name, sizeof(obs.name), "%s", st.client_name); } }
    if (st.percent > obs.max_pct) obs.max_pct = st.percent;
    if (st.mibps > obs.max_mibps) obs.max_mibps = st.mibps;
    if (st.eta_s >= 0) { obs.eta_seen++; if (obs.eta_min < 0 || st.eta_s < obs.eta_min) obs.eta_min = st.eta_s; }
    if (st.frags_repeated > obs.rep_max) obs.rep_max = st.frags_repeated;
    if (st.frags_total) obs.tot = st.frags_total;
    obs.state_seen[st.state]++;
    if (st.already_has) obs.already++;
    if (st.remaking) { obs.remaking_seen++; snprintf(obs.remaking_text, sizeof(obs.remaking_text), "%s", dlph_state_text(st)); }
    if (!dlph_state_text(st)[0]) obs.text_ok = false;
    if (obs.kick_pct && !obs.kicked && st.percent >= obs.kick_pct) { dlph_request_kick(); obs.kicked = true; }
}
static void on_event(dlph_event e, int arg, void *u) { (void)u; (void)arg; ev_count[e]++; }
static void on_log(const char *line, void *u) { (void)u; if (getenv("DLPH_VERBOSE")) printf("   %s\n", line); }

static int lowest_free(void) { for (int k = 2; k <= 8; k++) if (k != foreign_node && !(client_joined && client_node == k)) return k; return 0; }

// serve one console. It tries to join from the moment it "arrives"; `stop_when_open` ends the run when the host is ready for the next one.
static bool run_client(const char *label, u32 permille, u32 mute, int max_iter, double *secs, double *join_after_s)
{
    loss_permille = permille; mute_after = mute; rng = 99 + permille;
    client_reset(); memset(got_data, 0, G.cia_size);
    { int kp = obs.kick_pct; memset(&obs, 0, sizeof(obs)); obs.kick_pct = kp; obs.eta_min = -1; obs.text_ok = true; }
    int served0 = (int)G.served;
    u64 t0 = g_now, join_at = 0, leave_at = 0;
    bool left = false;
    for (int it = 0; it < max_iter; it++) {
        g_now += 16;
        if (net_up && !client_joined && !left && !complete && !net_blocked) {         // the console tries to connect (it cannot while the door is closed)
            client_node = lowest_free(); client_joined = true; join_at = g_now; complete = false;
        }
        if (downloading && g_now >= next_ack_at) { send_ack(); next_ack_at = g_now + 100; }
        dlph_poll();
        observe();
        if (complete && !leave_at) leave_at = g_now + 100;
        if (leave_at && g_now >= leave_at && client_joined) { last_leave[client_node] = g_now; client_joined = false; left = true; }
        if (left && net_up && dlph_get_stats().state == DLPH_ROOM_OPEN) break;       // ready for the next console
        if (obs.kicked && !client_joined && net_up && dlph_get_stats().state == DLPH_ROOM_OPEN) break;
        if (mute && G.remade && net_up && !client_joined && it > 100 && !G.persistent) { if (dlph_get_stats().state == DLPH_ROOM_OPEN) break; }
    }
    if (secs) *secs = (double)(g_now - t0) / 1000.0;
    if (join_after_s) *join_after_s = join_at ? (double)(join_at - t0) / 1000.0 : -1.0;
    bool served = (int)G.served > served0;
    printf("%-52s served=%d  time %.1f s  joined after %.1f s  fragments %d\n", label, served, secs ? *secs : 0.0, join_after_s ? *join_after_s : 0.0, unicast_count[4]);
    return served;
}

static void open_lib(bool persistent, u8 nodes, u32 quarantine)
{
    dlph_cfg cfg; memset(&cfg, 0, sizeof(cfg)); memset(cfg.iv, 0x5A, 16);
    cfg.child_path = getenv("DLPH_CHILD") ? getenv("DLPH_CHILD") : "DlpLink/probe/out/ChildProbe.cia";
    cfg.lang = 1; cfg.gap_us = 3000; cfg.log = on_log; cfg.event = on_event; cfg.persistent = persistent; cfg.max_nodes = nodes; cfg.quarantine_ms = quarantine;
    static const u8 mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 } /* PLACEHOLDER MAC: to compare with a capture of a real console, set the MAC of that console here */;
    memcpy(cfg.mac, mac, 6);
    expect_nodes = nodes ? nodes : 2;
    memset(ev_count, 0, sizeof(ev_count));
    rooms_created = 0; blocked_calls = 0; memset(last_leave, 0, sizeof(last_leave)); foreign_node = 0; client_joined = false; net_up = false; rxq_h = rxq_t = 0;
    assert(dlph_open(&cfg));
    ref = G.cia;
    free(got_data); got_data = calloc(1, G.cia_size + 4096);
    assert(dlph_start() && net_up);
    dlph_poll();                                              // the first beacon goes out: a console has seen it before it joins
    CHECK(xfer == G.transfer_size, "beacon announces %lu bytes, child has %lu", (unsigned long)xfer, (unsigned long)G.transfer_size);
}

int main(int argc, char **argv)
{
    const char *child = argc > 1 ? argv[1] : "DlpLink/probe/out/ChildProbe.cia";
    setenv("DLPH_CHILD", child, 1);
    double s, ja;

    // ---- a damaged child is refused
    {
        FILE *f = fopen(child, "rb");
        if (!f) { printf("cannot open %s\n", child); return 2; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        u8 *b = malloc((size_t)n); size_t r = fread(b, 1, (size_t)n, f); (void)r; fclose(f);
        for (int i = 0x2DC0; i < 0x2DC0 + 0x400; i++) b[i] = (u8)(i * 7);          // trash the TMD, like the 2026-10-19 file
        FILE *o = fopen("damaged_child.tmp", "wb"); fwrite(b, 1, (size_t)n, o); fclose(o); free(b);
        dlph_cfg c; memset(&c, 0, sizeof(c)); memset(c.iv, 0x5A, 16); c.child_path = "damaged_child.tmp"; c.lang = 1; c.gap_us = 3000; c.log = on_log;
        CHECK(!dlph_open(&c), "a damaged child.cia must be refused");
        remove("damaged_child.tmp");
        printf("damaged child refused: %s\n", failures ? "NO" : "yes");
    }

    // ---- no IV in the configuration: refused, and the library says why (it does not carry the IV itself)
    {
        dlph_cfg c; memset(&c, 0, sizeof(c)); c.child_path = child; c.lang = 1; c.gap_us = 3000; c.log = on_log;
        static const u8 mac2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
        memcpy(c.mac, mac2, 6);
        CHECK(!dlph_open(&c), "a configuration without the IV must be refused");
        memset(c.iv, 0x5A, 16);
        CHECK(dlph_open(&c), "the same configuration with an IV opens");
        dlph_close();
        printf("no IV refused, with an IV opens: %s\n", failures ? "NO" : "yes");
    }

    // ---- files that are not a valid child: no crash (run with -fsanitize=address,undefined), refused, and the emulator variant is never offered
    {
        FILE *f = fopen(child, "rb");
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        u8 *orig = malloc((size_t)n), *b = malloc((size_t)n); size_t r = fread(orig, 1, (size_t)n, f); (void)r; fclose(f);
        int accepted = 0, refused = 0;
        // the header of a CIA that is not one: 32-bit sums of the sections used to wrap (cert normal, ticket + 0x10000000, content - 0x10000000)
        memcpy(b, orig, (size_t)n);
        { u32 tik = (u32)b[12] | (u32)b[13] << 8 | (u32)b[14] << 16 | (u32)b[15] << 24, ct = (u32)b[24] | (u32)b[25] << 8 | (u32)b[26] << 16 | (u32)b[27] << 24;
          tik += 0x10000000u; ct -= 0x10000000u;
          b[12] = (u8)tik; b[13] = (u8)(tik >> 8); b[14] = (u8)(tik >> 16); b[15] = (u8)(tik >> 24);
          b[24] = (u8)ct; b[25] = (u8)(ct >> 8); b[26] = (u8)(ct >> 16); b[27] = (u8)(ct >> 24); }
        { FILE *o = fopen("wrap_child.tmp", "wb"); fwrite(b, 1, (size_t)n, o); fclose(o); }
        { dlph_cfg c; memset(&c, 0, sizeof(c)); memset(c.iv, 0x5A, 16); c.child_path = "wrap_child.tmp"; c.lang = 1; c.log = on_log;
          CHECK(!dlph_open(&c), "a CIA whose section sizes wrap 32 bits must be refused, not read outside the buffer"); }
        // random corruption of the 32 bytes of the header
        u32 seed = 12345;
        for (int it = 0; it < 3000; it++) {
            memcpy(b, orig, (size_t)n);
            for (int k = 0; k < 1 + (int)(seed % 4); k++) {
                seed = seed * 1664525u + 1013904223u;
                b[(seed >> 8) % 32] = (u8)(seed >> 3);
            }
            FILE *o = fopen("fuzz_child.tmp", "wb"); fwrite(b, 1, (size_t)n, o); fclose(o);
            dlph_cfg c; memset(&c, 0, sizeof(c)); memset(c.iv, 0x5A, 16); c.child_path = "fuzz_child.tmp"; c.lang = 1; c.log = NULL;
            if (dlph_open(&c)) { accepted++; dlph_close(); } else refused++;
        }
        printf("header fuzz: 3000 corrupted headers, %d refused, %d accepted (same offsets), no crash\n", refused, accepted);
        remove("wrap_child.tmp"); remove("fuzz_child.tmp");
        free(orig); free(b);

        // the emulator variant of the real child (NoCrypto): only if the file exists next to the real one
        const char *emu = getenv("DLPH_EMU_CHILD");
        if (emu && *emu) {
            dlph_cfg c; memset(&c, 0, sizeof(c)); memset(c.iv, 0x5A, 16); c.child_path = emu; c.lang = 1; c.log = on_log;
            CHECK(!dlph_open(&c), "the emulator variant (NCCH NoCrypto) of the child must be refused");
            printf("emulator variant refused: %s\n", emu);
        } else printf("emulator variant: not tested (set DLPH_EMU_CHILD)\n");
    }

    // =========================================================================================================== ROOM OF 2
    printf("\n== room of 2, re-made after every console ==\n");
    open_lib(false, 0, 0);
    dlph_stats st = dlph_get_stats();
    CHECK(st.state == DLPH_ROOM_OPEN, "state after start");
    printf("child %08lX%08lX version %u, to download %lu bytes\n", (unsigned long)(st.child_tid >> 32), (unsigned long)st.child_tid, st.child_version, (unsigned long)G.transfer_size);
    for (int i = 1; i <= 2; i++) {
        char lab[64]; snprintf(lab, sizeof(lab), "healthy download, console %d", i);
        g_client_name = i == 1 ? "Marco" : "Giulia";
        CHECK(run_client(lab, 0, 0, 20000, &s, &ja), "%s: not served", lab);
        CHECK(!memcmp(got_data, ref, xfer), "%s: downloaded bytes differ from the child", lab);
        CHECK(obs.name_seen && !strcmp(obs.name, g_client_name), "%s: the screen must show the name '%s' (got '%s')", lab, g_client_name, obs.name);
        CHECK(obs.seat == 2 && obs.max_pct == 100, "%s: seat %d, max percent %d", lab, obs.seat, obs.max_pct);
        CHECK(obs.max_mibps > 0.1f && obs.max_mibps < 2.0f, "%s: speed %.2f MiB/s out of range", lab, obs.max_mibps);
        CHECK(obs.eta_seen > 0 && obs.eta_min >= 0 && obs.eta_min <= 6, "%s: time left was never shown or never got near zero (seen %d, min %d)", lab, obs.eta_seen, obs.eta_min);
        CHECK(obs.tot == (xfer + DLPH_FRAG_SIZE - 1) / DLPH_FRAG_SIZE && obs.rep_max == 0, "%s: fragments total %lu, repeated %lu (a clean download repeats none)", lab, (unsigned long)obs.tot, (unsigned long)obs.rep_max);
        CHECK(obs.state_seen[DLPH_CLIENT_DOWNLOADING] > 0 && obs.state_seen[DLPH_CLIENT_JOINING] > 0 && obs.text_ok, "%s: states seen", lab);
        dlph_visit h[8];
        int nh = dlph_history(h, 8);
        CHECK(nh == i && h[0].result == DLPH_V_SERVED && !strcmp(h[0].name, g_client_name) && h[0].percent == 100 && h[0].number == (uint32_t)i && h[0].duration_ms > 3000,
              "%s: history (n %d, newest '%s' result %d percent %d number %u)", lab, nh, h[0].name, h[0].result, h[0].percent, h[0].number);
    }
    { dlph_stats st = dlph_get_stats(); CHECK(st.served == 2 && st.lost == 0 && st.remade == 2 && st.child_bytes == G.cia_size && st.child_name[0] && st.room_ms < 5000, "stats after two consoles (served %lu lost %lu remade %lu name '%s' room %lu ms)", (unsigned long)st.served, (unsigned long)st.lost, (unsigned long)st.remade, st.child_name, (unsigned long)st.room_ms); }
    CHECK(ev_count[DLPH_EV_CLIENT_NAME] == 2, "name events %d", ev_count[DLPH_EV_CLIENT_NAME]);
    CHECK(rooms_created == 3, "rooms created %d (1 + one re-made per console = 3)", rooms_created);
    static const u32 LOSS[] = { 10, 30 };
    for (unsigned i = 0; i < 2; i++) {
        char lab[64]; snprintf(lab, sizeof(lab), "loss %u.%u%%", LOSS[i] / 10, LOSS[i] % 10);
        memset(unicast_count, 0, sizeof(unicast_count));
        CHECK(run_client(lab, LOSS[i], 0, 40000, &s, &ja), "%s: not served", lab);
        CHECK(!memcmp(got_data, ref, xfer), "%s: downloaded bytes differ", lab);
        CHECK(obs.rep_max > 0 && obs.rep_max < total_frags / 8, "%s: repeated fragments shown %lu", lab, (unsigned long)obs.rep_max);
        CHECK((u32)unicast_count[4] <= total_frags + total_frags / 8, "%s: %d fragments sent for %lu needed (selective resend must stay within +12%%)", lab, unicast_count[4], (unsigned long)total_frags);
        CHECK(s < 25.0, "%s: took %.1f s", lab, s);
    }
    {
        int lost0 = ev_count[DLPH_EV_CLIENT_LOST];
        CHECK(!run_client("client that goes silent (mute after 6 acks)", 0, 6, 12000, &s, &ja), "a silent client must not count as served");
        CHECK(ev_count[DLPH_EV_CLIENT_LOST] == lost0 + 1, "the silent client must produce one CLIENT_LOST event");
        CHECK(s >= 8.5 && s <= 16.0, "the room must be re-made after ~9 s of silence, it took %.1f s", s);
        dlph_visit h[8];
        CHECK(dlph_history(h, 8) >= 1 && h[0].result == DLPH_V_LOST && h[0].percent > 0 && h[0].percent < 100, "history of the silent console: result %d percent %d", h[0].result, h[0].percent);
        CHECK(dlph_get_stats().lost == 1, "lost count %lu", (unsigned long)dlph_get_stats().lost);
        CHECK(run_client("next console after the stuck one", 0, 0, 20000, &s, &ja) && !memcmp(got_data, ref, xfer), "the console after a stuck one must be served");
        // the screen sends a console away at 30%: history says KICKED, the room is re-made, the next console is served
        int remade0 = rooms_created;
        obs.kick_pct = 30;
        CHECK(!run_client("console sent away from the screen at 30%", 0, 0, 20000, &s, &ja), "a console that was sent away must not count as served");
        CHECK(dlph_history(h, 8) >= 1 && h[0].result == DLPH_V_KICKED && h[0].percent >= 30 && h[0].percent < 100, "history of the console sent away: result %d percent %d", h[0].result, h[0].percent);
        CHECK(rooms_created == remade0 + 1, "the room must be re-made once after a kick (%d -> %d)", remade0, rooms_created);
        obs.kick_pct = 0;
        CHECK(run_client("next console after the one sent away", 0, 0, 20000, &s, &ja) && !memcmp(got_data, ref, xfer), "the console after a kick must be served");

        // a console that already has this version: served at once, nothing sent, and the screen says so (no false green)
        int frags0 = unicast_count[4];
        g_has_version = true;
        CHECK(run_client("console that already has this version", 0, 0, 20000, &s, &ja), "a console that already has the app is 'served' (it leaves)");
        g_has_version = false;
        CHECK(unicast_count[4] == frags0, "no fragment must be sent to a console that already has the app (%d sent)", unicast_count[4] - frags0);
        CHECK(obs.already > 0 && obs.max_pct == 0, "the screen must say 'already has' (seen %d times) and never show a percentage (%d%%)", obs.already, obs.max_pct);
        CHECK(dlph_history(h, 8) >= 1 && h[0].already_had && h[0].percent == 0 && h[0].result == DLPH_V_SERVED, "history: already_had %d percent %d", h[0].already_had, h[0].percent);

        // ... and one that says so and then goes silent does not keep the room for ever
        g_has_version = true; g_mute_all = true;
        CHECK(!run_client("console that has the app and goes silent", 0, 100000, 12000, &s, &ja), "a silent console must not count as served");
        g_has_version = false; g_mute_all = false;
        CHECK(s >= 8.5 && s <= 16.0, "silent after 'already has': the room must be re-made after ~9 s, it took %.1f s", s);
        CHECK(run_client("next console", 0, 0, 20000, &s, &ja) && !memcmp(got_data, ref, xfer), "the console after a silent one must be served");

        // the room cannot be re-made (the network service refuses twice): the library keeps trying, tells the screen, and the next console is served
        int created0 = rooms_created, remade0b = (int)dlph_get_stats().remade;
        fail_create_n = 2;
        CHECK(run_client("console served, then the re-make fails twice", 0, 0, 30000, &s, &ja) && !memcmp(got_data, ref, xfer), "console served before the failing re-make");
        CHECK(obs.remaking_seen > 0 && !strcmp(obs.remaking_text, "Re-making the room: one moment"), "the screen must say the room is being re-made (seen %d, text '%s')", obs.remaking_seen, obs.remaking_text);
        CHECK(rooms_created == created0 + 1 && (int)dlph_get_stats().remade == remade0b + 1, "one room made in the end (%d -> %d)", created0, rooms_created);
        CHECK(run_client("console after the room came back", 0, 0, 20000, &s, &ja) && !memcmp(got_data, ref, xfer), "the console after a failed re-make must be served");
    }
    dlph_close();

    // =========================================================================================================== PERSISTENT ROOM
    printf("\n== persistent room of 8: nothing is kicked, a used seat is closed for 45 s ==\n");
    open_lib(true, 8, 0);
    CHECK(run_client("console 1 (clean seat)", 0, 0, 20000, &s, &ja) && !memcmp(got_data, ref, xfer), "persistent: console 1 not served");
    CHECK(rooms_created == 1, "persistent: the room must not be re-made (created %d)", rooms_created);
    CHECK(net_blocked, "persistent: the door must be closed right after a console left (its seat is dirty)");
    // console 2 arrives at once: it must wait outside until the seat is clean, then be served at the first try
    CHECK(run_client("console 2 arrives at once (must wait ~45 s)", 0, 0, 20000, &s, &ja) && !memcmp(got_data, ref, xfer), "persistent: console 2 not served");
    CHECK(ja >= 44.0 && ja <= 47.0, "persistent: console 2 must get in after the seat healed (45 s), it joined after %.1f s", ja);
    CHECK(s - ja < 15.0, "persistent: console 2 must download in one go once in (%.1f s)", s - ja);
    CHECK(rooms_created == 1, "persistent: the room must still be the first one (created %d)", rooms_created);
    // a node that is not a Download Play console takes the lowest free seat: it is left alone, the console goes to the next seat
    foreign_node = 2;
    CHECK(run_client("a 3DS Link receiver sits on seat 2, console 3 on seat 3", 0, 0, 20000, &s, &ja) && !memcmp(got_data, ref, xfer), "persistent: a foreign node must not stop the delivery");
    CHECK(client_node == 3, "persistent: the console should have gone to seat 3 (went to %d)", client_node);
    CHECK(rooms_created == 1, "persistent: the room must still be the first one (created %d)", rooms_created);
    dlph_close();

    // negative control: without quarantine the next console lands on the dirty seat and is not served in time
    open_lib(true, 8, 1);
    CHECK(run_client("control: console 1", 0, 0, 20000, &s, &ja), "control: console 1 not served");
    CHECK(!run_client("control: no quarantine, console 2 lands on a dirty seat", 0, 0, 1500, &s, &ja), "the simulation says a dirty seat must stall the console: the quarantine is what avoids it");
    dlph_close();

    printf(failures ? "\nRESULT: %d FAILURE(S)\n" : "\nRESULT: ALL OK\n", failures);
    return failures != 0;
}
