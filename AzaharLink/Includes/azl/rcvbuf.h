/* The receive buffer of the room socket (SO_RCVBUF), as pure functions: the ENet platform layer is C,
   the arithmetic is tested on the PC (Tests/test_main.cpp).

   What the soc service does with the request (as the soc service behaves, 2026-10-09; it does not check it
   against the block given to socInit):
     * the receive buffer is a BYTE ring, payload + 32 bytes per datagram, newest datagram dropped when
       it is full; sizes are powers of two of pages, at most 64 KiB;
     * 65537..131072 answers ENOMEM (12); above 131072 a signed-order bug hands out ONE 4 KiB page that
       is declared as the big size, and the soc PROCESS corrupts memory and crashes (hardware test #5,
       a 256 KB request).
   So 64 KiB is a HARD limit that nothing may raise, and a block of 0x20000 bytes is the least that
   holds one 64 KiB ring (chunk 0: metadata + the default ring, chunk 1: the ring), one of 0x10000 only
   a 32 KiB one. 64 KiB is 0.9 s of a 4-player session (238 datagrams/s of ~280 B) and as much as ENet's
   reliable window, so more would be useless anyway. */
#ifndef AZL_RCVBUF_H
#define AZL_RCVBUF_H

#define AZL_RCVBUF_MAX 65536   /* never ask for more than this */
#define AZL_RCVBUF_FLOOR 16384 /* the retry ladder stops here */
#define AZL_RCVBUF_WARN 32768  /* a readback below this is shown on screen */
#define AZL_RCVBUF_ENOMEM 12   /* newlib's ENOMEM */

/* The largest ring a socket block holds (0 = the block cannot hold a sensible one). */
static inline int azl_rcvbuf_ring_budget(unsigned block) {
    return block >= 0x20000u ? 0x10000 : (block >= 0x10000u ? 0x8000 : 0);
}

/* What may be asked for: at most 64 KiB, and at most the ring the block holds (block 0 = unknown: no
   budget, the hard limit only). Never negative. */
static inline int azl_rcvbuf_limit(int want, unsigned block) {
    if (want > AZL_RCVBUF_MAX) {
        want = AZL_RCVBUF_MAX;
    }
    if (block != 0 && want > azl_rcvbuf_ring_budget(block)) {
        want = azl_rcvbuf_ring_budget(block);
    }
    return want < 0 ? 0 : want;
}

/* The ini key rcvbuf_kb: 8, 16, 32 or 64 (it can only lower the ring); anything else is 0 = ignored. */
static inline int azl_rcvbuf_from_kb(int kb) {
    return (kb == 8 || kb == 16 || kb == 32 || kb == 64) ? kb * 1024 : 0;
}

/* The next smaller size to try after `candidate` was refused: 64 -> 32 -> 16 KiB, then none (0). */
static inline int azl_rcvbuf_next(int candidate) {
    return candidate / 2 >= AZL_RCVBUF_FLOOR ? candidate / 2 : 0;
}

/* Ask again with a smaller size? Only when the service ran out of memory AND the ring really is smaller
   than the candidate (readback < 0 = unknown): a refusal that left the ring as asked needs no change. */
static inline int azl_rcvbuf_should_retry(int rc, int err, int readback, int candidate) {
    return rc != 0 && err == AZL_RCVBUF_ENOMEM && readback < candidate;
}

/* Is the ring the service reports smaller than what was asked? (Unknown readback: no claim.) */
static inline int azl_rcvbuf_degraded(int asked, int readback) {
    return readback >= 0 && readback < asked;
}

/* The calls the negotiation makes, so that it can be run against a model of the soc service on the PC. */
typedef struct azl_rcvbuf_io {
    int (*set)(void* ctx, int size, int* err);                          /* setsockopt: 0, or -1 with errno in *err */
    int (*get)(void* ctx);                                              /* getsockopt: the ring in bytes, -1 if unreadable */
    void (*log)(void* ctx, int candidate, int rc, int err, int got);    /* optional */
    void* ctx;
} azl_rcvbuf_io;

/* Asks for `asked` bytes (already limited by azl_rcvbuf_limit) and returns the ring the service ends up
   with, -1 if it cannot be read. Every attempt is reported to io->log. */
static inline int azl_rcvbuf_negotiate(const azl_rcvbuf_io* io, int asked) {
    int candidate = asked, rc, err, got;
    for (;;) {
        err = 0;
        rc = io->set(io->ctx, candidate, &err);
        got = io->get(io->ctx);
        if (io->log) {
            io->log(io->ctx, candidate, rc, err, got);
        }
        if (!azl_rcvbuf_should_retry(rc, err, got, candidate)) {
            break;
        }
        candidate = azl_rcvbuf_next(candidate);
        if (candidate <= 0) {
            break;
        }
    }
    return got;
}

#endif
