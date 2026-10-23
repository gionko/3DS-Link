// dlph_wire: pure wire format of the Download Play host (no libctru, no I/O). PROTOTYPE written for the report
// DlpLink/docs/research/03-rawhost-a-libreria.md; every layout below is the one rawhost/source/main.c puts on the air.
#ifndef DLPH_WIRE_H
#define DLPH_WIRE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DLPH_FRAG_SIZE   1440u                       // bytes of CIA per DATA fragment
#define DLPH_BLOCK_FRAGS 182u                        // fragments per block
#define DLPH_CHUNK_SIZE  (DLPH_FRAG_SIZE * DLPH_BLOCK_FRAGS)   // 0x3FFC0, beacon +0x1C
#define DLPH_HDR         0x10u
#define DLPH_BC0_LEN     768u                        // beacon frame 0
#define DLPH_BCI_LEN     1448u                       // beacon frames 1..3 (icon)
#define DLPH_BC4_LEN     1464u                       // beacon frame 4 (system update list)
#define DLPH_ICON_BYTES  4608u                       // 48x48 RGB565
#define DLPH_NAME_BYTES  128u                        // 64 x UTF-16
#define DLPH_DESC_BYTES  256u                        // 128 x UTF-16
#define DLPH_MAX_FRAME   1468u                       // 28 + 1440: the biggest unicast packet
#define DLPH_MAX_SYS     90u                         // entries of the system update list (3dbrew: 0x5A)

enum { DLPH_T_BEACON = 1, DLPH_T_AUTH = 2, DLPH_T_STARTDIST = 3, DLPH_T_DATA = 4, DLPH_T_FINISH = 5, DLPH_T_BEGIN = 6 };

typedef struct { uint64_t tid; uint16_t version; uint32_t size; } dlph_sysent;

// Everything that goes into the 5 beacon frames. The "k*" fields are the bytes whose meaning is NOT known: they carry
// the value captured from Zelda TFH (dlph_beacon_zelda_constants) because that is what the real client was shown.
typedef struct {
    uint64_t tid;                    // frame0 +0x10 (BE)
    uint16_t version;                // frame0 +0x18 (BE): title version in the TMD of the CIA
    uint8_t  k1a, k1b;               // +0x1A,+0x1B   Zelda: 01 02
    uint32_t chunk_size;             // +0x1C (BE)    0x3FFC0
    uint8_t  max_nodes;              // +0x20
    uint8_t  k21;                    // +0x21         Zelda: 02
    uint16_t cver_version;           // +0x22 (BE)    0x2D10 in the capture = 11.17.0 = version of the HOST's CVer title (see dlph_beacon_set_host_cver)
    uint8_t  k24[4];                 // +0x24         Zelda: 01 00 00 00
    uint8_t  k28[16];                // +0x28..0x37   Zelda: 00 00 00 86 87 00 86 87 80 00..; rawhost sends zeros
    uint32_t transfer_size;          // +0x38 (BE)    what the client downloads (CIA up to the meta block)
    uint32_t required_size;          // +0x3C (BE)    whole CIA
    const uint8_t *name_le;          // 128 B, SMDH "short description" of the chosen language (UTF-16LE as in the SMDH)
    const uint8_t *desc_le;          // 256 B, SMDH "long description"
    const uint8_t *icon_le;          // 4608 B, SMDH large icon (RGB565 LE as in the SMDH)
    uint8_t  k2f8[8];                // +0x2F8..0x2FF Zelda: 0B 11 00 00 45 0B 11 00 (the client ignores them when hdr+6 < 0x101)
    // frame 4 (system update list). n_sys < 0 : no frame 4 at all (4 frames, total_frames = 4): NEVER TRIED ON HARDWARE.
    // In the capture: 2 identical entries = the host's CVer title (000400DB00017102 = CVer EUR, version 0x2D10 = 11.17.0), size 0.
    int      n_sys;
    uint16_t sys_h[4];               // payload u16 x4: Zelda 0001 0002 0000 0001 (3dbrew: unk_x0, total_entries, out_entryindex, unk_x6)
    dlph_sysent sys[DLPH_MAX_SYS];
} dlph_beacon_desc;

// CVer = the "system version" title. TID 0x000400DB000171xx..176xx per region (3dbrew Title_list): J 7202, U 7302, E 7102, C 7402, K 7502, T 7602.
// Title version = (major << 10) | (minor << 4) | build, e.g. 11.17.0 -> 0x2D10. region is the letter of OS_VersionBin / version.bin.
uint64_t dlph_cver_tid(char region);                                     // 0 if the letter is unknown
uint16_t dlph_cver_version(unsigned major, unsigned minor, unsigned build);
// Fills cver_version, sys_h, and the 2 list entries from the host's own system version (returns 0, or -1 for an unknown region letter).
int  dlph_beacon_set_host_cver(dlph_beacon_desc *d, char region, unsigned major, unsigned minor, unsigned build);

// No CVer at all: +0x22 = 0 and no frame 4 (4 frames, total_frames = 4). The remedy for clients older than the host (report 07 section 2.5). Proven on hardware 2026-10-14..20 on old and new consoles (this is what dlph uses).
void dlph_beacon_set_no_cver(dlph_beacon_desc *d);

void dlph_beacon_zelda_constants(dlph_beacon_desc *d);     // fills every k* field and the CVer fields with the values of the capture (host = EUR 11.17.0)
int  dlph_beacon_frames(const dlph_beacon_desc *d, const uint8_t seed[4], uint8_t out[5][DLPH_BC4_LEN], size_t len[5]);   // returns the frame count (4 or 5)

// checksum (identical to rawhost main.c checksum_of; 3dbrew "Download Play#Checksum")
uint32_t dlph_checksum(const uint8_t seed[4], const uint8_t *frame, size_t len);
void     dlph_seal(const uint8_t seed[4], uint8_t *frame, size_t len);    // writes the checksum at +8
int      dlph_check(const uint8_t seed[4], const uint8_t *frame, size_t len);

// unicast (host -> client, data channel 2). `o` must hold DLPH_MAX_FRAME bytes. All return the length.
size_t dlph_pk_auth(uint8_t *o, const uint8_t seed[4]);
size_t dlph_pk_startdist(uint8_t *o, const uint8_t seed[4], uint8_t seq, const uint8_t rid[2]);
size_t dlph_pk_data(uint8_t *o, const uint8_t seed[4], uint32_t block, uint16_t frag, const uint8_t *src, uint16_t n);
size_t dlph_pk_finish_req(uint8_t *o, const uint8_t seed[4], uint8_t seq, const uint8_t rid[2], uint32_t next_ack);
size_t dlph_pk_begin_ping(uint8_t *o, const uint8_t seed[4], uint8_t seq, const uint8_t rid[2]);
size_t dlph_pk_begin_final(uint8_t *o, const uint8_t seed[4], uint8_t seq, const uint8_t rid[2], const uint8_t pass[9]);

#ifdef __cplusplus
}
#endif

#endif
