// The Download Play child the app prepares with boot9.bin (1.1.4: analysis/piano-1.1.4-2026-11-15/specifiche/6-APP-boot9.md 3 and 4.2, with its
// critic 6-APP-boot9-CRITICO.md and the contract 7-CONTRATTO.md K9): the pure part. The child's CIA (the one file of the RomFS of a game's content 2)
// carries a content under two layers, the title key (AES-CBC, the key made of the common key 0) and the NCCH layer (AES-CTR, key slot 0x2C, method 0;
// or method 1, slot 0x2C for some parts and slot 0x25 for the others: the build 2e slot25, 2026-11-18, below FindSlot25 - the KeyX of slot 0x25 is
// not in boot9.bin, the app finds it in the boot.firm the console starts from, read only, and keeps it in RAM for the work alone).
// The app takes both off, gives the exheader the id of the prepared title (azl::title_rules::PreparedOf of the game: the id of the PROCESS, which
// Luma3DS and the plugin read) and the ticket, the TMD and the NCCH's 0x108/0x118 the id of AM (the build 2f, 2026-11-19: two ids, AmTitleFor of
// dlp_guest_store.hpp - 00040001|low in the hidden form, so that HOME drops the title by its category; the prepared id itself in the old form), and
// turns off the "visible in HOME" bit of the icon (the user's decision: a prepared child is never in HOME). Since the build 2e (2026-11-18) the NCCH
// has the form makerom gives an application of the SD card (HeaderPatches). What it makes is, byte for byte, what the PC tools made and checked
// (analysis/island-tour-2026-11-13: cia_apri.py, ncch_chiaro.py, componi_figlio.py; the form makerom: the variants V2 and V2vis of
// analysis/piano-1.1.4-2026-11-15/home-figlio-2026-11-18/varianti; the form M3c1 of the build 2f: IT-M3c1.cia of
// analysis/piano-1.1.4-2026-11-15/prova-2e-2026-11-19/prova-pc/varianti): Tests/settings_test/dlp_guest_test.cpp holds it to them by their md5 when the
// private files are there.
// Header only, plain C++ with TitleShare's aes.c (CTR) and sha256.c and our aes_cbc.cpp: no libctru, no heap, no file. Nothing here writes a log, and
// every buffer that held a key is wiped by its owner (Wipe). The keys are never printed, anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "aes_cbc.hpp"

// TitleShare's primitives are C and their headers have no C++ guards. They are used as they are.
extern "C" {
#include "../titleshare/common/aes.h"
#include "../titleshare/common/sha256.h"
}

namespace dlpguest {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// ---- bytes ---------------------------------------------------------------------------------------------------------------------------

inline u16 LoadBe16(const u8* p) {
    return static_cast<u16>((p[0] << 8) | p[1]);
}
inline u32 LoadBe32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}
inline u64 LoadBe64(const u8* p) {
    return (static_cast<u64>(LoadBe32(p)) << 32) | LoadBe32(p + 4);
}
inline u32 LoadLe32(const u8* p) {
    return (static_cast<u32>(p[3]) << 24) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[1]) << 8) | p[0];
}
inline u64 LoadLe64(const u8* p) {
    return (static_cast<u64>(LoadLe32(p + 4)) << 32) | LoadLe32(p);
}
inline void StoreBe16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v >> 8);
    p[1] = static_cast<u8>(v);
}
inline void StoreBe64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<u8>(v >> (56 - 8 * i));
    }
}
inline void StoreLe32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<u8>(v >> (8 * i));
    }
}
inline void StoreLe64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<u8>(v >> (8 * i));
    }
}

/// Zeroes a buffer that held a key (volatile: the compiler may not drop it because nobody reads the buffer afterwards).
inline void Wipe(void* p, std::size_t n) {
    volatile u8* v = static_cast<volatile u8*>(p);
    while (n-- != 0) {
        *v++ = 0;
    }
}

// ---- keys ----------------------------------------------------------------------------------------------------------------------------

/// The four 16-byte values the composition needs: two KeyX of boot9's protected half, the common KeyY 0 and the key generator's constant (the
/// last two are built-in values: titleshare_host/builtin_keys.h, read by ComposeKeys in dlp_guest_compose.cpp). Only in RAM, wiped after the work.
struct Keys {
    u8 ncch_x[16];     // KeyX of slot 0x2C: the NCCH layer of method 0 (the same value as sdcrypt.c sd_ncch_keyx)
    u8 common_x[16];   // KeyX of slot 0x3D: the common keys of the tickets
    u8 common_y0[16];  // KeyY of the common key 0 (retail eShop titles)
    u8 generator[16];  // the hardware key generator's constant
};

constexpr std::size_t kBoot9Whole = 0x10000;     // boot9.bin as the console dumps it: the protected half is the second
constexpr std::size_t kBoot9Protected = 0x8000;  // the protected half alone
constexpr std::size_t kKeyX2CAt = 0x59D0;        // in the protected half
constexpr std::size_t kKeyX3DAt = 0x5A20;

/// Fills ncch_x and common_x from boot9.bin (whole, or its protected half); false for any other size (the other two values are not touched).
inline bool KeysFromBoot9(const u8* boot9, std::size_t size, Keys* out) {
    if (boot9 == nullptr || (size != kBoot9Whole && size != kBoot9Protected)) {
        return false;
    }
    const u8* prot = boot9 + (size == kBoot9Whole ? kBoot9Protected : 0);
    std::memcpy(out->ncch_x, prot + kKeyX2CAt, 16);
    std::memcpy(out->common_x, prot + kKeyX3DAt, 16);
    return true;
}

namespace detail {
struct U128 {
    u64 hi, lo;
};
inline U128 Load128(const u8* p) {
    return {LoadBe64(p), LoadBe64(p + 8)};
}
inline U128 Rol128(U128 v, unsigned n) {
    n %= 128;
    if (n >= 64) {
        v = {v.lo, v.hi};
        n -= 64;
    }
    if (n == 0) {
        return v;
    }
    return {(v.hi << n) | (v.lo >> (64 - n)), (v.lo << n) | (v.hi >> (64 - n))};
}
} // namespace detail

/// The hardware's key generator: key = ROL128((ROL128(X, 2) ^ Y) + C, 87), the three as big-endian 128-bit numbers. C is a parameter (never written here).
inline void Scramble(const u8 x[16], const u8 y[16], const u8 c[16], u8 key[16]) {
    using detail::U128;
    U128 t = detail::Rol128(detail::Load128(x), 2);
    const U128 yy = detail::Load128(y), cc = detail::Load128(c);
    t.hi ^= yy.hi;
    t.lo ^= yy.lo;
    U128 s;
    s.lo = t.lo + cc.lo;
    s.hi = t.hi + cc.hi + (s.lo < t.lo ? 1 : 0);
    s = detail::Rol128(s, 87);
    StoreBe64(key, s.hi);
    StoreBe64(key + 8, s.lo);
    Wipe(&t, sizeof(t));  // (the halves of the key on the way: not left on the stack)
    Wipe(&s, sizeof(s));
}

// ---- the CIA -------------------------------------------------------------------------------------------------------------------------

constexpr u32 kCiaHeader = 0x2020;
constexpr u32 kMetaSmdhAt = 0x400;
constexpr u32 kSmdhSize = 0x36C0;
constexpr u32 kMetaMax = 0x3AC0;           // dependencies 0x180, core version, padding, then the SMDH at 0x400
constexpr u32 kSmdhFlagsAt = 0x2028;       // in the SMDH: the flags word, bit 0 = visible in HOME
constexpr u64 kChildCategory = 0x00040001; // a Download Play child's high word

inline u64 Align64(u64 x) {
    return (x + 63) & ~63ull;
}

/// The parts of a CIA: each starts at a multiple of 64 bytes, the meta block last and ending the file.
struct CiaLayout {
    u32 header, cert, ticket, tmd, meta;
    u64 content;
    u64 cert_at, ticket_at, tmd_at, content_at, meta_at, total;
};

/// The CIA's first 0x20 bytes against its length. A child's CIA has a meta block with an SMDH (FindChildPackage asks the same).
inline bool ReadLayout(const u8 head[0x20], u64 file_size, CiaLayout* out) {
    CiaLayout l{};
    l.header = LoadLe32(head);
    l.cert = LoadLe32(head + 0x08);
    l.ticket = LoadLe32(head + 0x0C);
    l.tmd = LoadLe32(head + 0x10);
    l.meta = LoadLe32(head + 0x14);
    l.content = LoadLe64(head + 0x18);
    if (l.header != kCiaHeader || l.cert == 0 || l.ticket == 0 || l.tmd == 0 || l.content == 0 || l.content > 0xFFFFFFFFull ||
        l.meta < kMetaSmdhAt + kSmdhSize || l.meta > kMetaMax) {
        return false;
    }
    l.cert_at = Align64(l.header);
    l.ticket_at = l.cert_at + Align64(l.cert);
    l.tmd_at = l.ticket_at + Align64(l.ticket);
    l.content_at = l.tmd_at + Align64(l.tmd);
    l.meta_at = l.content_at + Align64(l.content);
    l.total = l.meta_at + l.meta;
    if (l.total != file_size) {
        return false;
    }
    *out = l;
    return true;
}

/// Where the body of a signed blob (ticket, TMD) starts: 4 bytes of type, the signature, its padding. 0 = a type we do not know.
inline u32 SignedBody(u32 type) {
    switch (type) {
    case 0x10000: case 0x10003: return 4 + 0x23C;  // RSA-4096
    case 0x10001: case 0x10004: return 4 + 0x13C;  // RSA-2048
    case 0x10002: case 0x10005: return 4 + 0x7C;   // ECDSA
    default: return 0;
    }
}

struct TicketFacts {
    u32 body;
    u64 tid;
    u8 enc_key[16];  // the title key, under the common key (not a secret by itself: it is in the file)
    u8 key_index;    // which common key
};

constexpr u32 kTicketKeyAt = 0x7F, kTicketTidAt = 0x9C, kTicketIndexAt = 0xB1;

inline bool ReadTicket(const u8* t, std::size_t n, TicketFacts* out) {
    if (n < 4) {
        return false;
    }
    const u32 body = SignedBody(LoadBe32(t));
    if (body == 0 || n < body + kTicketIndexAt + 1) {
        return false;
    }
    out->body = body;
    out->tid = LoadBe64(t + body + kTicketTidAt);
    std::memcpy(out->enc_key, t + body + kTicketKeyAt, 16);
    out->key_index = t[body + kTicketIndexAt];
    return true;
}

struct TmdFacts {
    u32 body;
    u64 tid;
    u16 version;
    u32 id;
    u16 index, type;
    u64 size;
    u8 hash[32];
    u32 info_at, chunk_at;  // in the TMD
};

constexpr u32 kTmdTidAt = 0x4C, kTmdVersionAt = 0x9C, kTmdCountAt = 0x9E, kTmdInfoHashAt = 0xA4, kTmdInfoAt = 0xC4;
constexpr u32 kTmdInfoBytes = 64 * 0x24;
constexpr u32 kTmdChunkBytes = 0x30;

/// A child's TMD: ONE content, the first info record (0, 1), and the chain header -> info records -> content record holds.
inline bool ReadTmd(const u8* t, std::size_t n, TmdFacts* out) {
    if (n < 4) {
        return false;
    }
    const u32 body = SignedBody(LoadBe32(t));
    if (body == 0) {
        return false;
    }
    const u32 info = body + kTmdInfoAt, chunk = info + kTmdInfoBytes;
    if (n < chunk + kTmdChunkBytes || LoadBe16(t + body + kTmdCountAt) != 1 || LoadBe16(t + info) != 0 || LoadBe16(t + info + 2) != 1) {
        return false;
    }
    u8 h[32];
    sha256(t + info, kTmdInfoBytes, h);
    if (std::memcmp(h, t + body + kTmdInfoHashAt, 32) != 0) {
        return false;
    }
    sha256(t + chunk, kTmdChunkBytes, h);
    if (std::memcmp(h, t + info + 4, 32) != 0) {
        return false;
    }
    out->body = body;
    out->tid = LoadBe64(t + body + kTmdTidAt);
    out->version = LoadBe16(t + body + kTmdVersionAt);
    out->id = LoadBe32(t + chunk);
    out->index = LoadBe16(t + chunk + 4);
    out->type = LoadBe16(t + chunk + 6);
    out->size = LoadBe64(t + chunk + 8);
    std::memcpy(out->hash, t + chunk + 0x10, 32);
    out->info_at = info;
    out->chunk_at = chunk;
    return true;
}

/// The title key: the common key 0 (Scramble of KeyX 0x3D and KeyY 0) opens the ticket's key, AES-CBC with the IV = title id (big-endian) and 8 zeros.
/// false = another common key (not a retail eShop title: not ours to open).
inline bool TitleKey(const Keys& keys, const TicketFacts& t, u8 out[16]) {
    if (t.key_index != 0) {
        return false;
    }
    u8 normal[16];
    Scramble(keys.common_x, keys.common_y0, keys.generator, normal);
    AesDecKey dk;
    AesDecSetKey(&dk, normal);
    u8 iv[16] = {0};
    StoreBe64(iv, t.tid);
    std::memcpy(out, t.enc_key, 16);
    AesCbcDecrypt(dk, iv, out, 16);
    Wipe(normal, sizeof(normal));
    Wipe(&dk, sizeof(dk));
    return true;
}

/// The IV of a content under its title key: the content's index (big-endian, 2 bytes), then 14 zeros.
inline void ContentIv(u16 index, u8 iv[16]) {
    std::memset(iv, 0, 16);
    StoreBe16(iv, index);
}

// ---- the NCCH ------------------------------------------------------------------------------------------------------------------------

constexpr u32 kMediaUnit = 0x200;
constexpr u32 kExheaderAt = 0x200;
constexpr u32 kExheaderHashed = 0x400;  // the exheader proper: what 0x160 is the SHA-256 of
constexpr u32 kPlainHead = 0xA00;       // the NCCH header, the exheader and the access descriptor

enum class NcchWhy : u8 { Ok, Magic, Size, Method, Seed, FixedKey, ExheaderSize, ExheaderHash, Regions, NoIcon, IconHash, ExefsHash, HashRegion, Patches };

/// A part of the content under the secondary key of method 1 (a file of the ExeFS, to its end rounded up to 0x200).
struct ExefsRange {
    u64 at, size;
};

struct NcchPlan {
    bool plain_source;  // 0x18F has NoCrypto already: no CTR
    bool two_keys;      // method 1 (0x18B = 1): a secondary key, KeyX of slot 0x25 (FindSlot25), for the ExeFS's files but icon and banner and the RomFS
    u64 partition;      // the source's 0x108 (the counter's first half of the layer taken off; the output's 0x108 is the prepared id: HeaderPatches)
    u8 key[16];         // the NCCH layer's key (method 0; method 1's primary): Scramble(KeyX 0x2C, the first 16 bytes of the header, C)
    u64 exefs_at, exefs_size, exefs_hash_size, romfs_at, romfs_size;
    ExefsRange secondary[10];  // (method 1, ExefsKeys) the ExeFS's files under the secondary key
    u8 n_secondary;
    u64 code_at, code_size;    // (method 1, ExefsKeys) the ExeFS's .code: its SHA-256 in the ExeFS header confirms the secondary key after the stream
    u8 code_hash[32];
};

/// The NCCH header (plain: the NCCH layer never covers it) of a content of `content_size` bytes.
inline NcchWhy ReadNcch(const u8 head[0x200], u64 content_size, const Keys& keys, NcchPlan* out) {
    if (std::memcmp(head + 0x100, "NCCH", 4) != 0) {
        return NcchWhy::Magic;
    }
    if (static_cast<u64>(LoadLe32(head + 0x104)) * kMediaUnit != content_size || head[0x18E] != 0) {
        return NcchWhy::Size;  // (0x18E: the media unit is 0x200 << it; every NCCH of a CIA has 0)
    }
    const u8 flags = head[0x18F];
    const bool plain = (flags & 0x04) != 0;
    if (!plain) {
        if (head[0x18B] != 0 && head[0x18B] != 1) {
            return NcchWhy::Method;  // 0x0A/0x0B (the KeyX of slot 0x18/0x1B, the New 3DS's: in no file 3DS Link reads) or a value nobody knows
        }
        if (flags & 0x20) {
            return NcchWhy::Seed;
        }
        if (flags & 0x01) {
            return NcchWhy::FixedKey;
        }
    }
    if (LoadLe32(head + 0x180) != kExheaderHashed) {
        return NcchWhy::ExheaderSize;
    }
    NcchPlan p{};
    p.plain_source = plain;
    p.two_keys = !plain && head[0x18B] == 1;  // (method 1: the secondary key is the caller's to find, FindSlot25; its round keys go to NcchDecrypt)
    p.partition = LoadLe64(head + 0x108);
    p.exefs_at = static_cast<u64>(LoadLe32(head + 0x1A0)) * kMediaUnit;
    p.exefs_size = static_cast<u64>(LoadLe32(head + 0x1A4)) * kMediaUnit;
    p.exefs_hash_size = static_cast<u64>(LoadLe32(head + 0x1A8)) * kMediaUnit;
    p.romfs_at = static_cast<u64>(LoadLe32(head + 0x1B0)) * kMediaUnit;
    p.romfs_size = static_cast<u64>(LoadLe32(head + 0x1B4)) * kMediaUnit;
    const auto inside = [content_size](u64 at, u64 size) { return size == 0 || (at >= kPlainHead && at <= content_size && size <= content_size - at); };
    if (!inside(p.exefs_at, p.exefs_size) || !inside(p.romfs_at, p.romfs_size) || p.exefs_hash_size > p.exefs_size) {
        return NcchWhy::Regions;
    }
    if (!plain) {
        Scramble(keys.ncch_x, head, keys.generator, p.key);
    }
    *out = p;
    Wipe(p.key, sizeof(p.key));  // (the copy is the caller's to wipe: this one does not stay in a dead stack frame)
    return NcchWhy::Ok;
}

/// The counter of a region of the NCCH layer at its start: the partition id byte-reversed (its little-endian bytes read backwards), the region's
/// type (1 exheader + access descriptor, 2 ExeFS, 3 RomFS), 7 zeros. It grows by one every 16 bytes from the region's start.
inline void NcchCtr(u64 partition, u8 type, u8 ctr[16]) {
    std::memset(ctr, 0, 16);
    StoreBe64(ctr, partition);
    ctr[8] = type;
}

/// Takes the NCCH layer off the parts of the three regions that fall in [off, off + len) of the content (off a multiple of 16; `rk` is
/// aes_set_key of the plan's key, `rk2` of the secondary key of method 1, read only when plan.two_keys). The rest (header, logo, plain region,
/// padding) is plain already and stays as it is: ncch_chiaro.py.
/// Which key, method 1 (Azahar src/core/hle/service/am/am.cpp, the install of a CIA: CryptoRegion EXHEADER, EXEFS_HDR and EXEFS_PRI - the ExeFS's
/// files "icon" and "banner" - under the primary key, EXEFS_SEC - every other file, to its end rounded up to 0x200 - and ROMFS under the secondary;
/// 3dbrew NCCH, the same; seen on Luigi's Mansion's child, analysis/piano-1.1.4-2026-11-15/luigi-308-2026-11-18): the exheader with the access
/// descriptor, the ExeFS's header, its icon and banner, and any byte of the ExeFS no file covers under the primary key; the ExeFS's other
/// files (.code, logo...) and the whole RomFS under the secondary. Both share the counter of their region.
inline void NcchDecrypt(const NcchPlan& plan, const u32 rk[44], const u32 rk2[44], u64 off, u8* buf, std::size_t len) {
    if (plan.plain_source) {
        return;
    }
    const struct {
        u8 type;
        u64 at, size;
    } regions[3] = {{1, kExheaderAt, kPlainHead - kExheaderAt}, {2, plan.exefs_at, plan.exefs_size}, {3, plan.romfs_at, plan.romfs_size}};
    for (const auto& r : regions) {
        const u64 a = off > r.at ? off : r.at;
        const u64 b = (off + len) < (r.at + r.size) ? (off + len) : (r.at + r.size);
        if (r.size == 0 || a >= b) {
            continue;
        }
        u8 ctr[16];
        NcchCtr(plan.partition, r.type, ctr);
        if (!plan.two_keys || r.type == 1) {
            aes_ctr_crypt(rk, ctr, a - r.at, buf + (a - off), static_cast<std::size_t>(b - a));  // (method 0; and method 1's exheader: the primary key)
            continue;
        }
        if (r.type == 3) {
            aes_ctr_crypt(rk2, ctr, a - r.at, buf + (a - off), static_cast<std::size_t>(b - a));  // method 1's RomFS: the secondary key
            continue;
        }
        // method 1's ExeFS, piece by piece: a file of plan.secondary under the secondary key, the rest (header, icon, banner) under the primary
        for (u64 p = a; p < b;) {
            const u32* key = rk;
            u64 end = b;
            for (u8 i = 0; i < plan.n_secondary; ++i) {
                const ExefsRange& s = plan.secondary[i];
                if (p >= s.at && p < s.at + s.size) {
                    key = rk2;
                    end = (s.at + s.size) < end ? (s.at + s.size) : end;
                } else if (s.at > p && s.at < end) {
                    end = s.at;
                }
            }
            aes_ctr_crypt(key, ctr, p - r.at, buf + (p - off), static_cast<std::size_t>(end - p));
            p = end;
        }
    }
}

/// (method 1) Which files of the ExeFS are under the secondary key - every file but "icon" and "banner", named as Azahar compares them (am.cpp: the
/// first 4 and 6 bytes) - each to its end rounded up to 0x200 (never past the ExeFS), and where .code and its SHA-256 are. `exefs_head` is the
/// ExeFS's plain header (under the primary key: read before any file). Regions: a file outside the ExeFS, not on a 16-byte boundary (the counter
/// moves by blocks), two files of the secondary key over each other, or one's span (rounded up to 0x200) over icon or banner (the review of DPAPP-2e
/// slot25: the icon would come out under the wrong key).
inline NcchWhy ExefsKeys(const u8 exefs_head[0x200], NcchPlan* plan) {
    plan->n_secondary = 0;
    plan->code_at = plan->code_size = 0;
    ExefsRange primary[10];  // icon and banner, as they are
    u8 n_primary = 0;
    for (unsigned i = 0; i < 10; ++i) {
        const u8* e = exefs_head + 16 * i;
        const u32 off = LoadLe32(e + 8), size = LoadLe32(e + 12);
        if (size == 0) {
            continue;  // (an empty entry)
        }
        if (off % 16 != 0 || 0x200ull + off + size > plan->exefs_size) {
            return NcchWhy::Regions;
        }
        if (std::memcmp(e, "icon", 4) == 0 || std::memcmp(e, "banner", 6) == 0) {
            primary[n_primary++] = {plan->exefs_at + 0x200 + off, size};
            continue;  // (the primary key)
        }
        const u64 at = plan->exefs_at + 0x200 + off;
        const u64 room = plan->exefs_at + plan->exefs_size - at;
        const u64 span = (static_cast<u64>(size) + kMediaUnit - 1) / kMediaUnit * kMediaUnit;
        plan->secondary[plan->n_secondary++] = {at, span < room ? span : room};
        if (std::memcmp(e, ".code\0\0\0", 8) == 0) {
            plan->code_at = at;
            plan->code_size = size;
            std::memcpy(plan->code_hash, exefs_head + 0xC0 + (9 - i) * 32, 32);
        }
    }
    for (u8 i = 0; i < plan->n_secondary; ++i) {
        for (u8 j = 0; j < plan->n_secondary; ++j) {
            const ExefsRange &x = plan->secondary[i], &y = plan->secondary[j];
            if (i != j && x.at < y.at + y.size && y.at < x.at + x.size) {
                return NcchWhy::Regions;
            }
        }
        for (u8 j = 0; j < n_primary; ++j) {
            const ExefsRange &x = plan->secondary[i], &y = primary[j];
            if (x.at < y.at + y.size && y.at < x.at + x.size) {
                return NcchWhy::Regions;  // (a span of the secondary key over icon or banner)
            }
        }
    }
    return NcchWhy::Ok;
}

// ---- method 1: the KeyX of slot 0x25 in boot.firm (the build 2e slot25, 2026-11-18: analysis/piano-1.1.4-2026-11-15/luigi-308-2026-11-18/
// CAUSA-E-PROGETTO.md, the way H) ----------------------------------------------------------------------------------------------------------
// The console decrypts a child of method 1 by itself: slot 0x25 is loaded at every start. With Luma3DS it is loaded from a constant of Luma's ARM9
// code (Luma3DS arm9/source/crypto.c setupKeyslots, 16 bytes aligned to 4), so it is in the boot.firm the console starts from. The app cannot read
// a key slot: it reads boot.firm (read only) and tries every 16 bytes aligned to 4 of its sections, the ARM9's first, as the KeyX. A candidate is the
// key when the first block of the RomFS decrypts to "IVFC" and version 0x10000, and is CONFIRMED by the SHA-256 of the RomFS's superblock at 0x1E0
// here, then by the SHA-256 of .code in the ExeFS header after the stream (Compose). The KeyX is never written anywhere, never printed: the log says
// the offset where it was, nothing of it. Its copies are wiped (Wipe) by their owners.

/// How the search went (the line of the log says it by Slot25StepName).
enum class Slot25Step : u8 { None, Found, FirmMissing, FirmBad, NoKey, NoRomfs, RomfsHash, CodeHash, RomfsLarge };

inline const char* Slot25StepName(Slot25Step s) {
    switch (s) {
    case Slot25Step::Found: return "found";
    case Slot25Step::FirmMissing: return "firm missing";  // (no boot.firm read: none on the SD card or in the CTRNAND, or no memory for it)
    case Slot25Step::FirmBad: return "firm bad";          // (not a FIRM, a section outside the file, a section's SHA-256 wrong, too big)
    case Slot25Step::NoKey: return "no key";              // (no candidate gives "IVFC")
    case Slot25Step::NoRomfs: return "no romfs";          // (a child with no RomFS: nothing to know the key by)
    case Slot25Step::RomfsHash: return "romfs hash";      // (a candidate gave "IVFC", none the SHA-256 at 0x1E0)
    case Slot25Step::CodeHash: return "code hash";        // (the key found does not give .code's SHA-256)
    case Slot25Step::RomfsLarge: return "romfs too large";  // (the superblock 0x1B8 names is larger than the room to check it: no candidate tried)
    default: return "none";
    }
}

constexpr std::size_t kFirmHeaderBytes = 0x200;   // the FIRM header (3dbrew FIRM): the sections start after it
constexpr std::size_t kFirmMaxBytes = 0x400000;   // a boot.firm larger than 4 MiB is not read (Luma's: about 300 KB)
constexpr u32 kArm9MemFrom = 0x08000000, kArm9MemTo = 0x08180000;  // the ARM9's internal memory (1 MiB on the Old 3DS, 1.5 MiB on the New)
constexpr u32 kRomfsMagicVersion = 0x10000;       // the IVFC header of a RomFS: "IVFC", then 0x00010000

struct FirmSection {
    u32 offset, load, size;
};

/// boot.firm's header (3dbrew FIRM): "FIRM" at 0, the ARM9 entry point at 0x0C, four section headers of 0x30 bytes from 0x40 (offset, load
/// address, size, copy method, SHA-256). Every section with a size lies after the header, inside the file, and has its SHA-256 right; one at least
/// has a size. false: not a FIRM to trust (FirmBad).
inline bool ReadFirm(const u8* firm, std::size_t n, FirmSection out[4], u32* arm9_entry) {
    if (firm == nullptr || n < kFirmHeaderBytes || n > kFirmMaxBytes || std::memcmp(firm, "FIRM", 4) != 0) {
        return false;
    }
    *arm9_entry = LoadLe32(firm + 0x0C);
    int sized = 0;
    for (int i = 0; i < 4; ++i) {
        const u8* h = firm + 0x40 + 0x30 * i;
        const FirmSection s{LoadLe32(h), LoadLe32(h + 4), LoadLe32(h + 8)};
        if (s.size != 0) {
            if (s.offset < kFirmHeaderBytes || s.offset > n || s.size > n - s.offset) {
                return false;
            }
            u8 hash[32];
            sha256(firm + s.offset, s.size, hash);
            if (std::memcmp(hash, h + 0x10, 32) != 0) {
                return false;
            }
            ++sized;
        }
        out[i] = s;
    }
    return sized > 0;
}

/// A section of the ARM9: loaded in the ARM9's internal memory, or holding the ARM9 entry point.
inline bool Arm9Section(const FirmSection& s, u32 arm9_entry) {
    return (s.load >= kArm9MemFrom && s.load < kArm9MemTo) || (arm9_entry >= s.load && arm9_entry - s.load < s.size);
}

/// The SHA-256 of `n` bytes of the RomFS (from its start) with the NCCH layer taken off by `rk`, against `want`: 64 bytes at a time, no copy.
inline bool RomfsHashHolds(const u32 rk[44], const u8 ctr[16], const u8* cipher, std::size_t n, const u8 want[32]) {
    sha256_ctx c;
    sha256_init(&c);
    u8 piece[64];
    for (std::size_t done = 0; done < n;) {
        const std::size_t m = n - done < sizeof(piece) ? n - done : sizeof(piece);
        std::memcpy(piece, cipher + done, m);
        aes_ctr_crypt(rk, ctr, done, piece, m);
        sha256_update(&c, piece, m);
        done += m;
    }
    Wipe(piece, sizeof(piece));  // (plain bytes of a game)
    u8 h[32];
    sha256_final(&c, h);
    return std::memcmp(h, want, 32) == 0;
}

struct Slot25Search {
    Slot25Step step;
    u32 at;          // where the KeyX is in boot.firm (Found), or the last candidate that gave "IVFC" (RomfsHash): an offset, never the key
    u32 candidates;  // how many were tried
};

/// The KeyX of slot 0x25 in boot.firm (`firm`, `n` bytes). `ncch_head` is the child's NCCH header (its first 16 bytes are the KeyY, 0x1E0 the
/// SHA-256 of the RomFS's superblock), `partition` its 0x108, `romfs` the first `hashed` bytes of its RomFS with the title key off and the NCCH layer
/// ON (hashed = 0x1B8 in units, a multiple of 16, at least 16). Every 16 bytes aligned to 4 of the ARM9's sections, then of the others
///: the normal key Scramble(candidate, KeyY, C) must decrypt the first block to "IVFC" and 0x00010000, then the superblock to the
/// SHA-256 at 0x1E0. Found: the KeyX in `x_out` (the caller wipes it). The keys of the way are wiped here.
inline Slot25Search FindSlot25(const u8* firm, std::size_t n, const u8 ncch_head[0x200], u64 partition, const u8* romfs, std::size_t hashed,
                               const u8 generator[16], u8 x_out[16]) {
    Slot25Search r{Slot25Step::FirmBad, 0, 0};
    FirmSection sec[4];
    u32 entry = 0;
    if (!ReadFirm(firm, n, sec, &entry)) {
        return r;
    }
    r.step = Slot25Step::NoKey;
    u8 ctr[16];
    NcchCtr(partition, 3, ctr);
    u8 key[16], block[16];
    u32 rk[44];
    for (int pass = 0; pass < 2 && r.step != Slot25Step::Found; ++pass) {  // pass 0 the ARM9's sections, pass 1 the others
        for (int i = 0; i < 4 && r.step != Slot25Step::Found; ++i) {
            if (sec[i].size < 16 || Arm9Section(sec[i], entry) != (pass == 0)) {
                continue;
            }
            const u32 last = sec[i].offset + sec[i].size - 16;
            for (u32 at = sec[i].offset + ((0u - sec[i].load) & 3u); at <= last; at += 4) {  // (aligned to 4 where it is loaded: Luma's constant is aligned(4))
                ++r.candidates;
                Scramble(firm + at, ncch_head, generator, key);
                aes_set_key(rk, key);
                std::memcpy(block, romfs, 16);
                aes_ctr_crypt(rk, ctr, 0, block, 16);
                if (std::memcmp(block, "IVFC", 4) != 0 || LoadLe32(block + 4) != kRomfsMagicVersion) {
                    continue;
                }
                r.at = at;
                if (!RomfsHashHolds(rk, ctr, romfs, hashed, ncch_head + 0x1E0)) {
                    r.step = Slot25Step::RomfsHash;  // (the superblock's SHA-256 does not hold: not this one, the search goes on)
                    continue;
                }
                std::memcpy(x_out, firm + at, 16);
                r.step = Slot25Step::Found;
                break;
            }
        }
    }
    Wipe(key, sizeof(key));
    Wipe(block, sizeof(block));
    Wipe(rk, sizeof(rk));
    return r;
}

// ---- the changes, applied while the content streams ------------------------------------------------------------------------------

struct Patch {
    u64 at;  // in the content
    u8 len;
    u8 bytes[32];
};

struct PatchList {
    Patch p[16];
    u8 n = 0;
    bool lost = false; // an Add refused (the list full, a length out of 1..32): the changes are not all there and nothing may be made of them
    bool Add(u64 at, const u8* bytes, u8 len) {
        if (n >= 16 || len == 0 || len > 32) {
            lost = true;
            return false;
        }
        p[n].at = at;
        p[n].len = len;
        std::memcpy(p[n].bytes, bytes, len);
        ++n;
        return true;
    }
};

/// Writes the parts of the patches that fall in [off, off + len) (a patch may straddle two chunks).
inline void ApplyPatches(const PatchList& list, u64 off, u8* buf, std::size_t len) {
    for (u8 i = 0; i < list.n; ++i) {
        const Patch& q = list.p[i];
        const u64 a = off > q.at ? off : q.at;
        const u64 b = (off + len) < (q.at + q.len) ? (off + len) : (q.at + q.len);
        if (a < b) {
            std::memcpy(buf + (a - off), q.bytes + (a - q.at), static_cast<std::size_t>(b - a));
        }
    }
}

/// SHA-256 of `len` bytes that sit at `at` of the content, as the patches leave them (64 bytes at a time: no copy of the whole).
inline void ShaPatched(const u8* data, u64 at, std::size_t len, const PatchList& list, u8 out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    u8 piece[64];
    for (std::size_t done = 0; done < len;) {
        const std::size_t n = len - done < sizeof(piece) ? len - done : sizeof(piece);
        std::memcpy(piece, data + done, n);
        ApplyPatches(list, at + done, piece, n);
        sha256_update(&c, piece, n);
        done += n;
    }
    sha256_final(&c, out);
}

// The form makerom gives an application of the SD card (the build 2e, 2026-11-18; CAUSA-E-RIMEDIO.md R5 of home-figlio-2026-11-18), in the content:
constexpr u32 kSciFlagsAt = 0x20D;         // the exheader's 0x0D: the SCI's flags, bit 1 SDApplication
constexpr u8 kSciSdApplication = 0x02;
constexpr u32 kAciArm9At = 0x5F0;          // the exheader's 0x3F0: the ACI's ARM9 capabilities (u32 LE), bit 8 SdApplication
constexpr u32 kDescArm9At = 0x9F0;         // the exheader's 0x7F0: the same word of the access descriptor, what the ACI may ask
constexpr u32 kArm9SdApplication = 0x100;

/// The changes of the header and the exheader (rifai_ncch + the flags of ncch_chiaro.py, and since the build 2e the form makerom gives an application
/// of the SD card: the variant V2 of home-figlio-2026-11-18/varianti): the jump id (0x3C8), the program of the ACI (0x400, the one Luma reads) and of
/// the access descriptor (0x800) become the prepared id - the id of the PROCESS, NEVER the id of AM: Luma3DS makes the codeset with 0x400
/// (loader.c:430-437) and the plugin knows the child by it -; the program id (0x118) and the partition id (0x108, as makerom writes it: it is the
/// counter of the NCCH layer, which the output has no more: NoCrypto) become `am_title`, the id AM installs the title by (the build 2f: AmTitleFor;
/// both are outside the exheader, so its hash at 0x160 is the same in the two forms); the SCI's flags say SDApplication; the ACI's ARM9 capabilities say
/// SdApplication, ONLY when the access descriptor lets them (an ACI that asks more than its descriptor is refused when the title starts: without
/// that bit in the descriptor the ACI stays as the child has it, the form that already started on the console); 0x160 the SHA-256 of the new
/// exheader; the content says NoCrypto (0x18B method 0, 0x18F NoCrypto on, fixed key off). Why (a hypothesis, never seen on the console): HOME
/// would read the icon by {tid, SD} (ARCHIVE_SAVEDATA_AND_CONTENT, as FBI does), and the child as it was is not in the form of the SD card's
/// applications (the open question of the empty place in HOME: the log of the children says what that way reads, dlp_guest.cpp HomeIconLine).
/// `plain_head` is the first 0xA00 bytes with the NCCH layer off. A change the list refuses (full) is said: Patches, never a child without it.
inline NcchWhy HeaderPatches(const u8 plain_head[0xA00], u64 prepared, u64 am_title, PatchList* list) {
    u8 h[32];
    sha256(plain_head + kExheaderAt, kExheaderHashed, h);
    if (std::memcmp(h, plain_head + 0x160, 32) != 0) {
        return NcchWhy::ExheaderHash;  // the NCCH key is wrong (or the content is damaged)
    }
    u8 am[8];
    StoreLe64(am, am_title);
    list->Add(0x108, am, 8);
    list->Add(0x118, am, 8);
    u8 id[8];
    StoreLe64(id, prepared);  // (0x400 is the id Luma gives the process and the plugin knows: NEVER the id of AM)
    list->Add(0x3C8, id, 8);
    list->Add(0x400, id, 8);
    list->Add(0x800, id, 8);
    const u8 sci = static_cast<u8>(plain_head[kSciFlagsAt] | kSciSdApplication);
    list->Add(kSciFlagsAt, &sci, 1);
    if ((LoadLe32(plain_head + kDescArm9At) & kArm9SdApplication) != 0) {
        u8 arm9[4];
        StoreLe32(arm9, LoadLe32(plain_head + kAciArm9At) | kArm9SdApplication);
        list->Add(kAciArm9At, arm9, 4);
    }
    ShaPatched(plain_head + kExheaderAt, kExheaderAt, kExheaderHashed, *list, h);
    list->Add(0x160, h, 32);
    const u8 method = 0;
    list->Add(0x18B, &method, 1);
    const u8 flags = static_cast<u8>((plain_head[0x18F] | 0x04) & ~0x01);
    list->Add(0x18F, &flags, 1);
    return list->lost ? NcchWhy::Patches : NcchWhy::Ok; // (a change refused: the hashes above were made without it)
}

/// Where the ExeFS's file "icon" is (in the content) and where its SHA-256 sits in the ExeFS header.
struct IconPlace {
    u64 at, hash_at;
};

inline NcchWhy FindIcon(const u8 exefs_head[0x200], u64 exefs_at, u64 exefs_size, IconPlace* out) {
    for (unsigned i = 0; i < 10; ++i) {
        const u8* e = exefs_head + 16 * i;
        if (std::memcmp(e, "icon\0\0\0\0", 8) != 0) {
            continue;
        }
        const u32 off = LoadLe32(e + 8), size = LoadLe32(e + 12);
        if (size != kSmdhSize || 0x200ull + off + size > exefs_size) {
            return NcchWhy::NoIcon;
        }
        out->at = exefs_at + 0x200 + off;
        out->hash_at = exefs_at + 0xC0 + (9 - i) * 32;
        return NcchWhy::Ok;
    }
    return NcchWhy::NoIcon;
}

/// The hidden child (componi_figlio.py nascondi): bit 0 of the icon's flags off, the icon's SHA-256 in the ExeFS header and the ExeFS's at 0x1C0
/// made again. `hashed` is the plain ExeFS from its start, the `hashed_size` bytes 0x1C0 covers; `icon` the plain icon. Both are checked against
/// the hashes they have before anything is changed: a child whose icon or hashes do not hold is not hidden (and is never installed visible).
inline NcchWhy HidePatches(const u8 ncch_head[0x200], u64 exefs_at, const u8* hashed, std::size_t hashed_size, const IconPlace& icon_place, const u8* icon,
                           PatchList* list, u32* flags_before) {
    if (hashed_size < 0x200 || icon_place.hash_at + 32 > exefs_at + hashed_size) {
        return NcchWhy::HashRegion;
    }
    u8 h[32];
    sha256(hashed, hashed_size, h);
    if (std::memcmp(h, ncch_head + 0x1C0, 32) != 0) {
        return NcchWhy::ExefsHash;
    }
    sha256(icon, kSmdhSize, h);
    if (std::memcmp(icon, "SMDH", 4) != 0 || std::memcmp(h, hashed + (icon_place.hash_at - exefs_at), 32) != 0) {
        return NcchWhy::IconHash;
    }
    const u32 before = LoadLe32(icon + kSmdhFlagsAt);
    *flags_before = before;
    u8 word[4];
    StoreLe32(word, before & ~1u);
    list->Add(icon_place.at + kSmdhFlagsAt, word, 4);
    ShaPatched(icon, icon_place.at, kSmdhSize, *list, h);
    list->Add(icon_place.hash_at, h, 32);
    ShaPatched(hashed, exefs_at, hashed_size, *list, h);
    list->Add(0x1C0, h, 32);
    return list->lost ? NcchWhy::Patches : NcchWhy::Ok; // (the same: a hidden child without one of its changes does not hold)
}

// ---- the front of the CIA (rifai_ticket, rifai_tmd) and its meta ----------------------------------------------------------------

/// In place on [0, content_at) of the CIA: the paddings zeroed, the ticket's title id, the TMD's title id (both `am_title`, the id AM installs the
/// title by: the build 2f), the content's type without the title key (bit 0), its SHA-256 the new content's, the hash of the content records and
/// the hash of the info records made again. The certificates and everything else stay as they are (the signatures no longer hold: Luma's patches
/// let them through, as for every CIA of makerom).
inline bool MakeFront(u8* front, const CiaLayout& lay, const TicketFacts& tik, const TmdFacts& tmd, u64 am_title, const u8 new_hash[32]) {
    if (tik.body + kTicketTidAt + 8 > lay.ticket || tmd.chunk_at + kTmdChunkBytes > lay.tmd) {
        return false;
    }
    const auto zero = [front](u64 from, u64 to) {
        if (to > from) {
            std::memset(front + from, 0, static_cast<std::size_t>(to - from));
        }
    };
    zero(lay.header, lay.cert_at);
    zero(lay.cert_at + lay.cert, lay.ticket_at);
    zero(lay.ticket_at + lay.ticket, lay.tmd_at);
    zero(lay.tmd_at + lay.tmd, lay.content_at);
    StoreBe64(front + lay.ticket_at + tik.body + kTicketTidAt, am_title);
    u8* t = front + lay.tmd_at;
    StoreBe64(t + tmd.body + kTmdTidAt, am_title);
    StoreBe16(t + tmd.chunk_at + 6, static_cast<u16>(tmd.type & ~1u));
    std::memcpy(t + tmd.chunk_at + 0x10, new_hash, 32);
    sha256(t + tmd.chunk_at, kTmdChunkBytes, t + tmd.info_at + 4);
    sha256(t + tmd.info_at, kTmdInfoBytes, t + tmd.body + kTmdInfoHashAt);
    return true;
}

/// The meta block: copied; hidden, the same bit of the SMDH it carries goes off too (AM does not read it: for coherence, as componi_figlio.py).
inline void MakeMeta(u8* meta, u32 size, bool hide) {
    if (hide && size >= kMetaSmdhAt + kSmdhSize && std::memcmp(meta + kMetaSmdhAt, "SMDH", 4) == 0) {
        u8* flags = meta + kMetaSmdhAt + kSmdhFlagsAt;
        StoreLe32(flags, LoadLe32(flags) & ~1u);
    }
}

// ---- what is made --------------------------------------------------------------------------------------------------------------------

/// kHide: the user's decision, every prepared child (the console's worker passes nothing else; Compose refuses a visible one outside the tests).
/// kVisible exists for Tests/settings_test/dlp_guest_test.cpp alone, to hold the composition to the md5 of the PC's plain P2a.
/// kFixCrr (P2c: the CRR's id for the RO module) is not made by this version: it waits for the console's S1.
enum Variant : u8 { kVisible = 0, kHide = 1, kFixCrr = 2 };
constexpr u8 kDefaultVariant = kHide;

// ---- results -------------------------------------------------------------------------------------------------------------------------

constexpr int kErrNoBoot9 = 300, kErrBoot9NoKeys = 301, kErrNoSource = 302, kErrContainerCrypto = 303, kErrNoChild = 304, kErrNotCia = 305,
              kErrCommonKey = 306, kErrTitleKey = 307, kErrChildCrypto = 308, kErrNcch = 309, kErrSourceHash = 310, kErrNoSpace = 311,
              kErrIdTaken = 312, kErrIdIsApp = 313, kErrTemp = 314, kErrInstall = 315, kErrInstallCheck = 316, kErrPlugin = 317,
              kErrTooMany = 318, kErrCancel = 319, kErrNoMemory = 320, kErrCannotHide = 321, kErrVariant = 322, kErrNoMovable = 323;
/// (the build 2f) the system did not take the hidden form (dlp_guest_store.hpp kFormDlpChild): the child was installed in the old form, it works,
/// and HOME shows it as a black icon (D2 of the plan of the build 2f). Not a fault of the moment: CanTryAgain says no.
constexpr int kErrShownInHome = 324;
/// (the build 2h, the guard of the ids: dlp_guest_ids.hpp) an id of the slot is a title of the system (the NAND), a ticket of another title's, a
/// process that is still in memory; the console's titles changed between the look and the write; the system put the child in the NAND.
constexpr int kErrIdSystem = 325, kErrIdTicket = 326, kErrIdRunning = 327, kErrIdStale = 328, kErrPutInNand = 329;
/// (DPAPP-2j, R-D) AM or FS answered a write or the Finish of the install with an error of the SD card (FS, FSI, SDMC, or fewer bytes taken than
/// given): nothing was installed, and it is NOT a refusal of the form - the Result is in the log.
constexpr int kErrSdIo = 330;
constexpr int kErrFirst = kErrNoBoot9, kErrLast = kErrSdIo;

// ---- (DPAPP-2j, R-D) what AM's or FS's Result says of a write or a Finish that failed ----------------------------------------------------
/// libctru's result.h: R_MODULE (bits 10-17) and R_DESCRIPTION (bits 0-9); RM_FS 17, RM_FSI 24, RM_AM 32, RM_SDMC 61. The descriptions FBI names
/// (FBI/source/core/ui/error.c): FS 210 "Partition full", AM 4 "Wrong installation state".
constexpr u32 kModuleFs = 17, kModuleFsi = 24, kModuleAm = 32, kModuleSdmc = 61;
constexpr u32 kDescPartitionFull = 210, kDescWrongInstallState = 4;
/// dlp_guest_am.cpp's own Result when AM took fewer bytes than it was given (module AM, description 107: never AM's own, said here first).
constexpr u32 kResultShortWrite = 0xD8E0806Bu;
enum class InstallFault : u8 {
    None,        ///< not a Result of AM or FS (0, a stop, the work file): nothing to say of the install
    SdIo,        ///< FS, FSI, SDMC (not a full card), or a short write: the SD card - kErrSdIo, never a refusal of the form
    NoSpace,     ///< FS 210: the card is full - kErrNoSpace
    FormRefused, ///< AM said no (not its state): the only case that is a refusal of the form
    Busy,        ///< AM 4, its state: a fault of the moment
    Unknown,     ///< any other module: kErrInstall, never a refusal of the form, the Result said
};
constexpr u32 ResultModule(u32 rc) {
    return (rc >> 10) & 0xFFu;
}
constexpr u32 ResultDescription(u32 rc) {
    return rc & 0x3FFu;
}
constexpr u32 ResultSummary(u32 rc) {
    return (rc >> 21) & 0x3Fu;
}
constexpr InstallFault ClassifyAm(u32 rc) {
    return rc == 0 ? InstallFault::None
           : rc == kResultShortWrite ? InstallFault::SdIo
           : (ResultModule(rc) == kModuleFs || ResultModule(rc) == kModuleFsi || ResultModule(rc) == kModuleSdmc)
               ? (ResultDescription(rc) == kDescPartitionFull ? InstallFault::NoSpace : InstallFault::SdIo)
           : ResultModule(rc) == kModuleAm ? (ResultDescription(rc) == kDescWrongInstallState ? InstallFault::Busy : InstallFault::FormRefused)
                                            : InstallFault::Unknown;
}
static_assert(ClassifyAm(0xC8804478u) == InstallFault::SdIo && ClassifyAm(0xC86044D2u) == InstallFault::NoSpace && ClassifyAm(kResultShortWrite) == InstallFault::SdIo &&
                  ClassifyAm(0xC8A08025u) == InstallFault::FormRefused && ClassifyAm(0xC8A08004u) == InstallFault::Busy && ClassifyAm(0x13Bu) == InstallFault::Unknown &&
                  ClassifyAm(0) == InstallFault::None && ClassifyAm(0xD8A0F7BFu) == InstallFault::SdIo,
              "FS not found and FS partition full, the short write, AM's Invalid NCCH and Wrong state, a number of ours, SDMC");
inline const char* FaultWord(InstallFault f) {
    switch (f) {
    case InstallFault::SdIo: return "an error of the SD card";
    case InstallFault::NoSpace: return "the SD card is full";
    case InstallFault::FormRefused: return "AM refused it";
    case InstallFault::Busy: return "AM was busy";
    case InstallFault::Unknown: return "not known";
    case InstallFault::None: break;
    }
    return "none";
}
/// `digits` hex digits of `v`, upper case (no printf here: the composer's files never print).
inline std::string HexDigits(u64 v, int digits) {
    std::string s(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i, v >>= 4) {
        s[static_cast<std::size_t>(i)] = "0123456789ABCDEF"[v & 0xF];
    }
    return s;
}
/// "guest: install write 000400010F8F8200: Result 0xC8A08025 (module 32, summary 5, description 37) at 1048576 of 2949120 bytes: AM refused it"
/// (a line of the log: AM's or FS's own Result, never lost again - the 2i's 315 said nothing of it).
inline std::string FaultLine(const char* step, u64 tid, u32 result, u64 at, u64 total, InstallFault fault) {
    return std::string("guest: ") + step + " " + HexDigits(tid, 16) + ": Result 0x" + HexDigits(result, 8) + " (module " + std::to_string(ResultModule(result)) +
           ", summary " + std::to_string(ResultSummary(result)) + ", description " + std::to_string(ResultDescription(result)) + ") at " + std::to_string(at) +
           " of " + std::to_string(total) + " bytes: " + FaultWord(fault);
}

/// A game CARD's content 2 refused by route F (the only route there), from FS's Result: "not found" (summary 4, module FS 17, description 120,
/// whatever the level) - the card has no partition 2, a game with no Download Play - is kErrNoChild, which the look remembers in children.cache;
/// anything else is kErrNoSource, tried again at the next look (a card taken out, a read that failed). The final review of 1.1.4: before, a card
/// with no Download Play was looked at again at every reason to look, and its sheet said that its data could not be read.
constexpr int CardRouteFRefused(u32 fs_result) {
    return ((fs_result >> 21) & 0x3F) == 4 && ((fs_result >> 10) & 0xFF) == 17 && (fs_result & 0x3FF) == 120 ? kErrNoChild : kErrNoSource;
}
static_assert(CardRouteFRefused(0xC8804478) == kErrNoChild && CardRouteFRefused(0xC8A04478) != kErrNoChild, "FS's not found, and only it");

/// What went wrong, for the user (one line).
inline const char* Phrase(int err) {
    switch (err) {
    case 0: return "Ready.";
    case kErrNoBoot9: return "boot9.bin was not found.";
    case kErrBoot9NoKeys: return "boot9.bin has no keys in it.";
    case kErrNoSource: return "The game's Download Play data could not be read.";
    case kErrContainerCrypto: return "The game's data is locked in a way 3DS Link cannot open.";
    case kErrNoChild: return "This game has no Download Play guest.";
    case kErrNotCia: return "The game's Download Play guest is not in a known form.";
    case kErrCommonKey: return "The guest is locked with a key 3DS Link does not have.";
    case kErrTitleKey: return "boot9.bin is not valid for this console.";
    case kErrChildCrypto: return "The guest is locked in a way 3DS Link could not open (see the log).";  // (DPAPP-2e slot25 rev: the case is in the log's line)
    case kErrNcch: return "The guest's data does not check out (boot9.bin, or a damaged game).";
    case kErrSourceHash: return "The game's data is damaged: nothing was installed.";
    case kErrNoSpace: return "Not enough free space on the SD card.";
    case kErrIdTaken: return "Another title already uses the guest's number.";
    case kErrIdIsApp: return "This game cannot have a guest (its number is 3DS Link's).";
    case kErrTemp: return "The SD card could not hold the work file.";
    case kErrInstall: return "The system did not install the guest.";
    case kErrInstallCheck: return "The guest installed is not the one made.";
    case kErrPlugin: return "The plugin could not be put in the guest's folder.";
    case kErrTooMany: return "Too many Download Play guests (16 at most).";
    case kErrCancel: return "Stopped.";
    case kErrNoMemory: return "Not enough memory.";
    case kErrCannotHide: return "The guest could not be hidden from HOME, so it was not installed.";
    case kErrVariant: return "This kind of guest is not made by this version.";
    case kErrNoMovable: return "Needs movable.sed: the system did not give the game's Download Play data.";
    case kErrShownInHome: return "Installed the old way: HOME shows it as a black icon.";
    case kErrIdSystem: return "A system title already uses the guest's number.";
    case kErrIdTicket: return "Another title's ticket uses the guest's number.";
    case kErrIdRunning: return "A closed game is still in memory.";
    case kErrIdStale: return "The console's titles changed while 3DS Link looked.";
    case kErrPutInNand: return "The system put the guest in the internal memory.";
    case kErrSdIo: return "Error reading or writing the SD card: nothing was installed.";
    default: return "Something went wrong.";
    }
}

/// What the user can do about it (one line).
inline const char* Advice(int err) {
    switch (err) {
    case 0: return "";
    case kErrNoBoot9:
    case kErrBoot9NoKeys:
    case kErrTitleKey: return "Dump boot9.bin of THIS console again with GodMode9 (see the guide).";
    case kErrNoSource: return "Check that the game is on the SD card or in the slot, then try again.";
    case kErrChildCrypto: return "Start Luma3DS from sd:/boot.firm and try again; if it fails, join with your own copy.";
    case kErrContainerCrypto:
    case kErrCommonKey:
    case kErrNotCia:
    case kErrNoChild:
    case kErrVariant: return "This game's guest cannot be prepared: join with your own copy of the game.";
    case kErrNcch: return "Check boot9.bin, then try again; if it happens again, the game may be damaged.";
    case kErrSourceHash: return "Check the game in Data Management, then try again.";
    case kErrNoSpace: return "Free some space on the SD card, then try again.";
    case kErrIdTaken: return "Remove that title with FBI if it is not needed.";
    case kErrIdIsApp: return "Nothing to do: this game plays with its own copy.";
    case kErrTemp: return "Check the SD card, then try again.";
    case kErrInstall:
    case kErrInstallCheck: return "Restart the console, then try again.";
    case kErrPlugin: return "Check the SD card, then try again.";
    case kErrTooMany: return "Remove some guests in Download Play > Enable games.";
    case kErrCancel: return "Prepare it again when you like.";
    case kErrNoMemory: return "Close other software, then try again.";
    case kErrCannotHide: return "Nothing to do: this game plays with its own copy.";
    case kErrNoMovable: return "Dump movable.sed of THIS console with GodMode9 and put it next to boot9.bin.";
    // (the review of the build 2f: the memo of the session sends a game straight back to the old form until 3DS Link starts again)
    case kErrShownInHome: return "It works. To try hiding it again: restart 3DS Link, disable and enable the game.";
    // (the build 2h: never "remove that title" - a title of the system, or another title's ticket, is never 3DS Link's to take away, nor the user's)
    case kErrIdSystem:
    case kErrIdTicket: return "Nothing to do: this game plays with its own copy.";
    case kErrIdRunning: return "Restart the console, then try again.";
    case kErrIdStale: return "Try again: 3DS Link read them again.";
    case kErrPutInNand: return "3DS Link does not touch it: tell the developers.";
    case kErrSdIo: return "Check the SD card (a PC's disk check), then try again.";
    default: return "Try again.";
    }
}

/// Whether trying again may help (a fault of the moment, not of the game or of the keys).
inline bool CanTryAgain(int err) {
    switch (err) {
    case kErrNoSource:
    case kErrSourceHash:
    case kErrNoSpace:
    case kErrTemp:
    case kErrInstall:
    case kErrInstallCheck:
    case kErrPlugin:
    case kErrCancel:
    case kErrNoMemory:
    case kErrIdRunning:
    case kErrIdStale:
    case kErrSdIo:
    case kErrNcch: return true;
    default: return false;
    }
}

} // namespace dlpguest
