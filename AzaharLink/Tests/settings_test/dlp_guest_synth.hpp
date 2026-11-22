// A made-up Download Play child, for Tests/settings_test/dlp_guest_test.cpp (1.1.4, the child the app prepares with boot9.bin): no byte of Nintendo
// and no real key. It builds a plain NCCH (header, an exheader with the three ids, an ExeFS with a .code and an icon SMDH, a small RomFS, every hash
// right), puts the NCCH layer on it (method 0, with keys made up here), the title key layer (AES-CBC), and a ticket, a TMD, certificates and a meta
// block around it: the child's CIA, as a game's content 2 carries it, optionally in a RomFS of one file (the shape of Tests/dlp_title_block_test.cpp).
// Method 1 too (the build 2e slot25): the primary key for the exheader, the ExeFS header, icon and banner, the secondary one - a KeyX of "slot 0x25"
// MADE UP here (MadeKeys::slot25_x, from its own generator: never a real key) - for the other files and the RomFS; and a boot.firm made up around
// that KeyX (MakeFirm), with false candidates beside it.
// Nothing here shares code with what it tests: its own key generator (a 128-bit integer of the compiler), its own counters and regions, CBC
// encryption made of aes.c's CTR (one block of CTR over 16 zeros with the counter = the plain block is the block encrypted).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../../ConfigApp/titleshare/common/aes.h"
#include "../../ConfigApp/titleshare/common/sha256.h"
}

namespace synth {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Bytes = std::vector<u8>;

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    u8 Byte() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<u8>(s >> 24);
    }
    void Fill(u8* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            p[i] = Byte();
        }
    }
};

inline void Le32(Bytes& b, std::size_t at, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b[at + i] = static_cast<u8>(v >> (8 * i));
    }
}
inline void Le64(Bytes& b, std::size_t at, u64 v) {
    for (int i = 0; i < 8; ++i) {
        b[at + i] = static_cast<u8>(v >> (8 * i));
    }
}
inline void Be16(Bytes& b, std::size_t at, u16 v) {
    b[at] = static_cast<u8>(v >> 8);
    b[at + 1] = static_cast<u8>(v);
}
inline void Be32(Bytes& b, std::size_t at, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b[at + i] = static_cast<u8>(v >> (24 - 8 * i));
    }
}
inline void Be64(Bytes& b, std::size_t at, u64 v) {
    for (int i = 0; i < 8; ++i) {
        b[at + i] = static_cast<u8>(v >> (56 - 8 * i));
    }
}
inline void Sha(const u8* p, std::size_t n, u8* out) {
    sha256(p, n, out);
}
inline u64 Up(u64 x, u64 a) {
    return (x + a - 1) / a * a;
}

// ---- its own crypto (independent of ConfigApp/source/dlp_guest_logic.hpp) --------------------------------------------------------------

using u128 = unsigned __int128;
inline u128 Get128(const u8* p) {
    u128 v = 0;
    for (int i = 0; i < 16; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}
inline void Put128(u8* p, u128 v) {
    for (int i = 15; i >= 0; --i) {
        p[i] = static_cast<u8>(v);
        v >>= 8;
    }
}
inline u128 Rol(u128 v, unsigned n) {
    return (v << n) | (v >> (128 - n));
}
inline void KeyOf(const u8 x[16], const u8 y[16], const u8 c[16], u8 key[16]) {
    Put128(key, Rol((Rol(Get128(x), 2) ^ Get128(y)) + Get128(c), 87));
}

/// AES-128 encryption of one block with aes.c alone: CTR with the counter = the block, over 16 zeros
inline void EncBlock(const u32 rk[44], const u8 in[16], u8 out[16]) {
    aes_init();  // (aes.c's tables: once, the test is one thread)
    std::memset(out, 0, 16);
    aes_ctr_crypt(rk, in, 0, out, 16);
}
inline void CbcEncrypt(const u8 key[16], const u8 iv0[16], u8* data, std::size_t len) {
    aes_init();
    u32 rk[44];
    aes_set_key(rk, key);
    u8 iv[16], x[16];
    std::memcpy(iv, iv0, 16);
    for (std::size_t at = 0; at + 16 <= len; at += 16) {
        for (int i = 0; i < 16; ++i) {
            x[i] = static_cast<u8>(data[at + i] ^ iv[i]);
        }
        EncBlock(rk, x, data + at);
        std::memcpy(iv, data + at, 16);
    }
}
/// The NCCH layer of one region (method 0): the counter is the partition id as it reads in big-endian order, the type, zeros
inline void CtrRegion(const u8 key[16], u64 partition, u8 type, u8* data, std::size_t len) {
    aes_init();
    u32 rk[44];
    aes_set_key(rk, key);
    u8 ctr[16] = {0};
    for (int i = 0; i < 8; ++i) {
        ctr[i] = static_cast<u8>(partition >> (56 - 8 * i));
    }
    ctr[8] = type;
    aes_ctr_crypt(rk, ctr, 0, data, len);
}

// ---- the child -------------------------------------------------------------------------------------------------------------------------

struct MadeKeys {
    u8 ncch_x[16], common_x[16], common_y0[16], generator[16];
    u8 slot25_x[16];  // (method 1) the made-up KeyX of "slot 0x25": its own generator, so the children of method 0 stay byte for byte as before
};

struct Opts {
    u64 child = 0x00040001000F8200ull;
    u64 partition = 0x00040001000F8277ull;  // (another id than the program's, as in a real child)
    u16 version = 0x0410;
    bool title_key = true;   // the content's type has bit 0 (the CIA's layer)
    bool ncch_layer = true;  // method 0; false = the container says NoCrypto already
    u8 method = 0;
    bool seed = false, fixed_key = false;
    u32 exheader_size = 0x400;
    bool icon = true;
    u32 icon_flags = 0x145;  // bit 0: visible
    bool desc_sd = true;     // the access descriptor lets the ACI say SdApplication (bit 8 of its ARM9 word, the exheader's 0x7F0), as a real child's
    bool sci_sd = false;     // the SCI's flags say SDApplication already (a child in the form of the SD card: the |= leaves it)
    bool aci_sd = false;     // the ACI's ARM9 says SdApplication already (with desc_sd false: an ACI above its descriptor, left as it is)
    u8 key_index = 0;
    u32 tik_sig = 0x10004, tmd_sig = 0x10004;
    u16 contents = 1;
    u16 info_count = 1;  // the first info record's count
    u32 code_bytes = 5 * 1024;
    u32 romfs_units = 0x30;  // 0: no RomFS
    bool banner = false;     // (method 1) a file "banner" in the ExeFS: under the primary key
    bool logo = false;       // (method 1) a file "logo" in the ExeFS: under the secondary key, like .code
    bool bad_romfs_hash = false;  // 0x1E0 is not the superblock's SHA-256 (the TMD's hash still holds)
    bool bad_code_hash = false;   // the ExeFS header holds a wrong SHA-256 for .code (0x1C0 made over it: only .code's own check can tell)
    u32 romfs_hashed_units = 1;   // 0x1B8: the units of the RomFS's superblock 0x1E0 covers (0x1E0 is made over the first one only)
    u64 rng = 1;
};

struct Made {
    Bytes cia;
    Bytes plain;  // the content with both layers off, the flags as the source had them
    Bytes cert, ticket, tmd, meta;
    u64 cert_at, ticket_at, tmd_at, content_at, meta_at;
    u64 exefs_at, exefs_size, romfs_at, romfs_size, icon_at, icon_hash_at;
    u64 code_at, banner_at, logo_at;  // (banner_at, logo_at: 0 when there is none)
    MadeKeys keys;
    u8 title_key[16];
};

inline Bytes MakeSmdh(Rng& r, u32 flags) {
    Bytes s(0x36C0, 0);
    std::memcpy(s.data(), "SMDH", 4);
    const char16_t* name = u"Made-up Party";
    for (int e = 0; e < 16; ++e) {
        std::size_t at = 8 + 0x200 * static_cast<std::size_t>(e);
        for (const char16_t* c = name; *c; ++c) {
            s[at++] = static_cast<u8>(*c & 0xFF);
            s[at++] = static_cast<u8>(*c >> 8);
        }
    }
    Le32(s, 0x2028, flags);
    r.Fill(s.data() + 0x24C0, 0x1200);
    return s;
}

inline Made Make(const Opts& o) {
    Rng r(o.rng);
    Made m{};
    r.Fill(m.keys.ncch_x, 16);
    r.Fill(m.keys.common_x, 16);
    r.Fill(m.keys.common_y0, 16);
    r.Fill(m.keys.generator, 16);
    r.Fill(m.title_key, 16);
    Rng(o.rng ^ 0x2525).Fill(m.keys.slot25_x, 16);  // (a generator of its own: r's sequence, and every child of method 0, as before)

    // the plain NCCH
    const u64 code_units = Up(o.code_bytes, 0x200) / 0x200;
    m.exefs_at = 0xA00;
    m.exefs_size = 0x200 + code_units * 0x200 + 0x3800 + (o.banner ? 0x400 : 0) + (o.logo ? 0x200 : 0);
    m.romfs_at = Up(m.exefs_at + m.exefs_size, 0x200);
    m.romfs_size = static_cast<u64>(o.romfs_units) * 0x200;
    const u64 size = m.romfs_at + m.romfs_size;
    Bytes& p = m.plain;
    p.assign(size, 0);
    r.Fill(p.data(), 0x100);  // the signature: its first 16 bytes are the layer's KeyY
    std::memcpy(&p[0x100], "NCCH", 4);
    Le32(p, 0x104, static_cast<u32>(size / 0x200));
    Le64(p, 0x108, o.partition);
    Le64(p, 0x118, o.child);
    std::memcpy(&p[0x150], "CTR-P-ZZZZ", 10);
    Le32(p, 0x180, o.exheader_size);
    p[0x18B] = o.method;
    p[0x18C] = 1;
    p[0x18D] = 0x03;
    p[0x18F] = static_cast<u8>((o.ncch_layer ? 0 : 0x04) | (o.seed ? 0x20 : 0) | (o.fixed_key ? 0x01 : 0));
    Le32(p, 0x1A0, static_cast<u32>(m.exefs_at / 0x200));
    Le32(p, 0x1A4, static_cast<u32>(m.exefs_size / 0x200));
    Le32(p, 0x1A8, 1);
    Le32(p, 0x1B0, o.romfs_units != 0 ? static_cast<u32>(m.romfs_at / 0x200) : 0);
    Le32(p, 0x1B4, o.romfs_units);
    Le32(p, 0x1B8, o.romfs_units != 0 ? o.romfs_hashed_units : 0);
    // the exheader and the access descriptor
    r.Fill(&p[0x200], 0x800);
    p[0x200 + 0x0D] = static_cast<u8>(o.sci_sd ? (p[0x200 + 0x0D] | 0x02) : (p[0x200 + 0x0D] & ~0x02));  // the SCI's flags: SDApplication only with o.sci_sd (a child's of the NAND: 0x01)
    p[0x200 + 0x3F1] = static_cast<u8>(o.aci_sd ? (p[0x200 + 0x3F1] | 0x01) : (p[0x200 + 0x3F1] & ~0x01));  // the ACI's ARM9: SdApplication (bit 8) only with o.aci_sd
    p[0x200 + 0x7F1] = static_cast<u8>(o.desc_sd ? (p[0x200 + 0x7F1] | 0x01) : (p[0x200 + 0x7F1] & ~0x01));  // ... the descriptor's: o.desc_sd
    Le64(p, 0x200 + 0x1C8, o.child);              // the jump id
    Le64(p, 0x200 + 0x200, o.child);              // the ACI's program
    Le64(p, 0x200 + 0x600, 0x00040001FFFFFFFFull);  // the access descriptor's
    Sha(&p[0x200], 0x400, &p[0x160]);
    // the ExeFS: .code and icon (method 1 may add banner and logo after them)
    const std::size_t eo = static_cast<std::size_t>(m.exefs_at);
    m.code_at = m.exefs_at + 0x200;
    r.Fill(&p[eo + 0x200], o.code_bytes);
    std::memcpy(&p[eo], ".code\0\0\0", 8);
    Le32(p, eo + 8, 0);
    Le32(p, eo + 12, o.code_bytes);
    Sha(&p[eo + 0x200], o.code_bytes, &p[eo + 0xC0 + 9 * 32]);
    const Bytes smdh = MakeSmdh(r, o.icon_flags);
    const u32 icon_off = static_cast<u32>(code_units * 0x200);
    m.icon_at = m.exefs_at + 0x200 + icon_off;
    m.icon_hash_at = m.exefs_at + 0xC0 + 8 * 32;
    std::memcpy(&p[m.icon_at], smdh.data(), smdh.size());
    if (o.icon) {
        std::memcpy(&p[eo + 16], "icon\0\0\0\0", 8);
        Le32(p, eo + 16 + 8, icon_off);
        Le32(p, eo + 16 + 12, 0x36C0);
        Sha(smdh.data(), smdh.size(), &p[m.icon_hash_at]);
    }
    u32 next_off = icon_off + 0x3800;
    m.banner_at = m.logo_at = 0;
    if (o.banner) {  // (entry 2, 0x400 bytes)
        m.banner_at = m.exefs_at + 0x200 + next_off;
        r.Fill(&p[m.banner_at], 0x400);
        std::memcpy(&p[eo + 32], "banner\0\0", 8);
        Le32(p, eo + 32 + 8, next_off);
        Le32(p, eo + 32 + 12, 0x400);
        Sha(&p[m.banner_at], 0x400, &p[eo + 0xC0 + 7 * 32]);
        next_off += 0x400;
    }
    if (o.logo) {  // (entry 3, 0x1F0 bytes: rounded up to 0x200 under its key)
        m.logo_at = m.exefs_at + 0x200 + next_off;
        r.Fill(&p[m.logo_at], 0x1F0);
        std::memcpy(&p[eo + 48], "logo\0\0\0\0", 8);
        Le32(p, eo + 48 + 8, next_off);
        Le32(p, eo + 48 + 12, 0x1F0);
        Sha(&p[m.logo_at], 0x1F0, &p[eo + 0xC0 + 6 * 32]);
    }
    if (o.bad_code_hash) {
        p[eo + 0xC0 + 9 * 32] ^= 0x01;  // (.code's hash in the ExeFS header; 0x1C0 below is made over it)
    }
    Sha(&p[eo], 0x200, &p[0x1C0]);
    // the RomFS (only its first unit is hashed here: nothing reads its tree)
    const std::size_t ro = static_cast<std::size_t>(m.romfs_at);
    if (m.romfs_size != 0) {
        r.Fill(&p[ro], static_cast<std::size_t>(m.romfs_size));
        std::memcpy(&p[ro], "IVFC", 4);
        if (o.method != 0) {
            Le32(p, ro + 4, 0x10000);  // (the IVFC version a real RomFS has: what the search in boot.firm knows the KeyX by)
        }
        Sha(&p[ro], 0x200, &p[0x1E0]);
        if (o.bad_romfs_hash) {
            p[0x1E0] ^= 0x01;
        }
    }

    // the NCCH layer (method 1: the secondary key for the ExeFS's files but icon and banner, and for the RomFS)
    Bytes content = p;
    if (o.ncch_layer) {
        u8 key[16], key2[16];
        KeyOf(m.keys.ncch_x, &p[0], m.keys.generator, key);
        KeyOf(o.method == 1 ? m.keys.slot25_x : m.keys.ncch_x, &p[0], m.keys.generator, key2);
        CtrRegion(key, o.partition, 1, &content[0x200], 0x800);
        CtrRegion(key, o.partition, 2, &content[eo], static_cast<std::size_t>(m.exefs_size));
        if (o.method == 1) {
            Bytes second(p.begin() + static_cast<std::ptrdiff_t>(eo), p.begin() + static_cast<std::ptrdiff_t>(eo + m.exefs_size));
            CtrRegion(key2, o.partition, 2, second.data(), second.size());
            const auto take = [&](u64 at, u64 size) {  // (a file under the secondary key, to its end rounded up to 0x200)
                for (u64 i = at; i < at + Up(size, 0x200); ++i) {
                    content[i] = second[i - eo];
                }
            };
            take(m.code_at, o.code_bytes);
            if (o.logo) {
                take(m.logo_at, 0x1F0);
            }
        }
        if (m.romfs_size != 0) {
            CtrRegion(key2, o.partition, 3, &content[ro], static_cast<std::size_t>(m.romfs_size));
        }
    }
    u8 content_hash[32];
    Sha(content.data(), content.size(), content_hash);
    if (o.title_key) {
        const u8 iv[16] = {0, 0};  // (index 0)
        CbcEncrypt(m.title_key, iv, content.data(), content.size());
    }

    // the ticket
    const auto body = [](u32 type) -> u32 {
        switch (type) {
        case 0x10000: case 0x10003: return 4 + 0x23C;
        case 0x10001: case 0x10004: return 4 + 0x13C;
        case 0x10002: case 0x10005: return 4 + 0x7C;
        default: return 4 + 0x13C;
        }
    };
    const u32 bt = body(o.tik_sig);
    m.ticket.assign(bt + 0x210, 0);
    r.Fill(m.ticket.data(), m.ticket.size());
    Be32(m.ticket, 0, o.tik_sig);
    u8 normal[16], enc_key[16];
    KeyOf(m.keys.common_x, m.keys.common_y0, m.keys.generator, normal);
    std::memcpy(enc_key, m.title_key, 16);
    u8 tiv[16] = {0};
    for (int i = 0; i < 8; ++i) {
        tiv[i] = static_cast<u8>(o.child >> (56 - 8 * i));
    }
    CbcEncrypt(normal, tiv, enc_key, 16);
    std::memcpy(&m.ticket[bt + 0x7F], enc_key, 16);
    Be64(m.ticket, bt + 0x9C, o.child);
    m.ticket[bt + 0xB1] = o.key_index;

    // the TMD
    const u32 b = body(o.tmd_sig);
    const u32 info = b + 0xC4, chunk = info + 0x900;
    m.tmd.assign(chunk + 0x30 * o.contents, 0);
    r.Fill(m.tmd.data(), b);
    Be32(m.tmd, 0, o.tmd_sig);
    Be64(m.tmd, b + 0x4C, o.child);
    Be16(m.tmd, b + 0x9C, o.version);
    Be16(m.tmd, b + 0x9E, o.contents);
    for (u16 i = 0; i < o.contents; ++i) {
        const std::size_t c = chunk + 0x30 * static_cast<std::size_t>(i);
        Be32(m.tmd, c, 0x00000000u + i);
        Be16(m.tmd, c + 4, i);
        Be16(m.tmd, c + 6, o.title_key ? 1 : 0);
        Be64(m.tmd, c + 8, size);
        std::memcpy(&m.tmd[c + 0x10], content_hash, 32);
    }
    Be16(m.tmd, info, 0);
    Be16(m.tmd, info + 2, o.info_count);
    Sha(&m.tmd[chunk], 0x30 * static_cast<std::size_t>(o.contents), &m.tmd[info + 4]);
    Sha(&m.tmd[info], 0x900, &m.tmd[b + 0xA4]);

    // the certificates and the meta block (with the icon's copy at 0x400)
    m.cert.assign(0xA00, 0);
    r.Fill(m.cert.data(), m.cert.size());
    m.meta.assign(0x3AC0, 0);
    r.Fill(m.meta.data(), 0x180);
    std::memcpy(&m.meta[0x400], smdh.data(), smdh.size());

    // the CIA
    m.cert_at = Up(0x2020, 64);
    m.ticket_at = m.cert_at + Up(m.cert.size(), 64);
    m.tmd_at = m.ticket_at + Up(m.ticket.size(), 64);
    m.content_at = m.tmd_at + Up(m.tmd.size(), 64);
    m.meta_at = m.content_at + Up(content.size(), 64);
    Bytes& cia = m.cia;
    cia.assign(m.meta_at + m.meta.size(), 0);
    Le32(cia, 0, 0x2020);
    Le32(cia, 8, static_cast<u32>(m.cert.size()));
    Le32(cia, 0x0C, static_cast<u32>(m.ticket.size()));
    Le32(cia, 0x10, static_cast<u32>(m.tmd.size()));
    Le32(cia, 0x14, static_cast<u32>(m.meta.size()));
    Le64(cia, 0x18, content.size());
    cia[0x20] = 0x80;  // content 0 is in the file
    std::memcpy(&cia[m.cert_at], m.cert.data(), m.cert.size());
    std::memcpy(&cia[m.ticket_at], m.ticket.data(), m.ticket.size());
    std::memcpy(&cia[m.tmd_at], m.tmd.data(), m.tmd.size());
    std::memcpy(&cia[m.content_at], content.data(), content.size());
    std::memcpy(&cia[m.meta_at], m.meta.data(), m.meta.size());
    return m;
}

// ---- a boot.firm made up around the made-up KeyX of slot 0x25 (the build 2e slot25) ---------------------------------------------------

struct FirmOpts {
    bool key = true;           // the KeyX is in the file
    bool in_arm9 = true;       // ... in the ARM9's section (false: in the ARM11's only: the search's second pass)
    bool unaligned = false;    // ... at an offset that is not a multiple of 4 only (never a candidate)
    bool bad_hash = false;     // a section's SHA-256 is wrong
    bool magic = true;         // "FIRM"
    u32 arm9_load = 0x08006000u;  // where the ARM9's section is loaded (0x08006002: its 4-byte boundaries are 2 off the file's)
    u64 rng = 9;
};

struct Firm {
    Bytes bytes;
    u32 key_at = 0;  // where the KeyX is (0: nowhere)
};

/// A FIRM of three sections - ARM9 (0x08006000, 32 KB), ARM11 (0x1FF80000, 16 KB), another (0x18180000, 4 KB) - of random bytes, every SHA-256 right
/// (unless bad_hash), the KeyX `x` put where FirmOpts says, and false candidates around it: the "dev" KeyX of Luma right after it (another value),
/// the KeyX with one bit flipped at aligned places, the KeyX at an offset not aligned to 4.
inline Firm MakeFirm(const u8 x[16], const FirmOpts& f) {
    Rng r(f.rng);
    Firm out;
    const u32 at[3] = {0x200, 0x8200, 0xC200}, load[3] = {f.arm9_load, 0x1FF80000u, 0x18180000u}, size[3] = {0x8000, 0x4000, 0x1000};
    Bytes& b = out.bytes;
    b.assign(0xD200, 0);
    r.Fill(b.data() + 0x200, b.size() - 0x200);
    std::memcpy(b.data(), f.magic ? "FIRM" : "FIRN", 4);
    Le32(b, 0x08, 0x1FF80000u);  // the ARM11 entry
    Le32(b, 0x0C, 0x08006000u);  // the ARM9 entry
    for (int i = 0; i < 2; ++i) {  // false candidates: the KeyX with a bit flipped, aligned, in both sections
        u8 near[16];
        std::memcpy(near, x, 16);
        near[(i * 5) % 16] ^= 0x10;
        std::memcpy(&b[at[i] + 0x1000], near, 16);
    }
    if (f.key) {
        out.key_at = f.unaligned ? at[0] + 0x5002 : (f.in_arm9 ? at[0] + 0x5000 : at[1] + 0x100);
        std::memcpy(&b[out.key_at], x, 16);
        r.Fill(&b[out.key_at + 16], 16);  // (Luma keeps the dev unit's KeyX right after the retail one: another value)
    }
    for (int i = 0; i < 3; ++i) {
        u8* h = &b[0x40 + 0x30 * static_cast<std::size_t>(i)];
        for (int k = 0; k < 4; ++k) {
            h[k] = static_cast<u8>(at[i] >> (8 * k));
            h[4 + k] = static_cast<u8>(load[i] >> (8 * k));
            h[8 + k] = static_cast<u8>(size[i] >> (8 * k));
        }
        Sha(&b[at[i]], size[i], h + 0x10);
    }
    if (f.bad_hash) {
        b[at[1] + 0x20] ^= 0x01;
    }
    return out;
}

/// A level-3 RomFS whose root directory holds one file, `name`, with `data` (a game's content 2 as the system gives it); `data_at` = where the file is.
inline Bytes InRomFs(const Bytes& data, const std::string& name, u64* data_at) {
    Bytes meta(0x20 + ((name.size() * 2 + 3) & ~std::size_t(3)), 0);
    Le32(meta, 0x00, 0);
    Le32(meta, 0x04, 0xFFFFFFFFu);
    Le64(meta, 0x08, 0);
    Le64(meta, 0x10, data.size());
    Le32(meta, 0x18, 0xFFFFFFFFu);
    Le32(meta, 0x1C, static_cast<u32>(name.size() * 2));
    for (std::size_t k = 0; k < name.size(); ++k) {
        meta[0x20 + 2 * k] = static_cast<u8>(name[k]);
    }
    const u32 dir_at = 0x28 + 4, file_hash_at = dir_at + 0x18, meta_at = file_hash_at + 4;
    const u32 file_data_at = (meta_at + static_cast<u32>(meta.size()) + 15) & ~15u;
    Bytes img(file_data_at + data.size(), 0);
    Le32(img, 0x00, 0x28);
    Le32(img, 0x04, 0x28);
    Le32(img, 0x08, 4);
    Le32(img, 0x0C, dir_at);
    Le32(img, 0x10, 0x18);
    Le32(img, 0x14, file_hash_at);
    Le32(img, 0x18, 4);
    Le32(img, 0x1C, meta_at);
    Le32(img, 0x20, static_cast<u32>(meta.size()));
    Le32(img, 0x24, file_data_at);
    Le32(img, dir_at + 0x00, 0);
    Le32(img, dir_at + 0x04, 0xFFFFFFFFu);
    Le32(img, dir_at + 0x08, 0xFFFFFFFFu);
    Le32(img, dir_at + 0x0C, 0);
    Le32(img, dir_at + 0x10, 0xFFFFFFFFu);
    Le32(img, dir_at + 0x14, 0xFFFFFFFFu);
    std::memcpy(&img[meta_at], meta.data(), meta.size());
    std::memcpy(&img[file_data_at], data.data(), data.size());
    *data_at = file_data_at;
    return img;
}

} // namespace synth
