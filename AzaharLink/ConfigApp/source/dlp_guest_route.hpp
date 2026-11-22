// Route K of the source of a Download Play child the app prepares (1.1.4, piece APP-WORKER: 6-APP-boot9.md 3.1, its critic 6-APP-boot9-CRITICO.md 4.5
// and K9.4 of 7-CONTRATTO.md): a game's content 2 read through the keys instead of the system. The delivery module gives the virtual CIA of an installed
// title (titleshare_host/delivery.h dl_cia_*: the SD layer off, every content there, each padded to 64 bytes, the content type's title-key bit cleared);
// its content 2 is the "child container", an NCCH whose RomFS holds one CIA per child. This file finds that content in the CIA, reads the container's
// header, and gives its RomFS as azl::dlp_real::FindChildPackage and Compose read it: as it is when the container says NoCrypto (Island Tour), through
// the NCCH layer (AES-CTR, key slot 0x2C, method 0, counter type 3) otherwise. Any other container is kErrContainerCrypto.
// And the facts of a child's CIA that the app keeps without composing it (ReadChildFacts: children.cache, the check of another version).
// Pure, header only: no libctru, no heap, no file, no log. Tests/settings_test/dlp_guest_run_test.cpp reads made-up CIAs with it. The round keys of the
// NCCH layer live in the KeyedRomfs the caller owns, and CloseKeyedRomfs wipes them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "dlp_guest_compose.hpp"
#include "dlp_guest_store.hpp"

namespace dlpguest {

constexpr u16 kChildContentIndex = 2; // the content of a game that carries its Download Play children

/// Where a content of a CIA is. The contents follow the TMD's order, each padded to 64 bytes, and only the ones the header's bitmap names are in
/// the file (all of them in a virtual CIA).
struct ContentAt {
    u64 at = 0;   // in the CIA
    u64 size = 0;
    u16 type = 0;
};

/// The content of `index` in the CIA `read` gives (`cia_size` bytes): 0, kErrNotCia (not a CIA, or one that lies) or kErrNoChild (no such content:
/// the game has no Download Play child).
inline int FindContent(ReadFn read, void* ctx, u64 cia_size, u16 index, ContentAt* out) {
    u8 head[0x20];
    if (cia_size < kCiaHeader || !read(ctx, 0, head, sizeof(head))) {
        return kErrNotCia;
    }
    const u32 cert = LoadLe32(head + 0x08), ticket = LoadLe32(head + 0x0C), tmd = LoadLe32(head + 0x10);
    const u64 content = LoadLe64(head + 0x18);
    if (LoadLe32(head) != kCiaHeader || cert == 0 || ticket == 0 || tmd < 4 || content == 0) {
        return kErrNotCia;
    }
    const u64 tmd_at = Align64(kCiaHeader) + Align64(cert) + Align64(ticket);
    const u64 content_at = tmd_at + Align64(tmd);
    if (content_at > cia_size || content > cia_size - content_at) {
        return kErrNotCia;
    }
    u8 b[0x30];
    if (!read(ctx, tmd_at, b, 4)) {
        return kErrNotCia;
    }
    const u32 body = SignedBody(LoadBe32(b));
    const u64 chunks_at = static_cast<u64>(body) + kTmdInfoAt + kTmdInfoBytes;
    if (body == 0 || chunks_at > tmd || !read(ctx, tmd_at + body + kTmdCountAt, b, 2)) {
        return kErrNotCia;
    }
    const u32 count = LoadBe16(b);
    if (count == 0 || chunks_at + static_cast<u64>(count) * kTmdChunkBytes > tmd) {
        return kErrNotCia;
    }
    u64 at = content_at;
    for (u32 i = 0; i < count; ++i) {
        if (!read(ctx, tmd_at + chunks_at + static_cast<u64>(i) * kTmdChunkBytes, b, kTmdChunkBytes)) {
            return kErrNotCia;
        }
        const u16 idx = LoadBe16(b + 4);
        const u64 size = LoadBe64(b + 8);
        u8 bit = 0;
        if (!read(ctx, 0x20 + idx / 8, &bit, 1)) {
            return kErrNotCia;
        }
        if ((bit & (0x80u >> (idx % 8))) == 0) {
            continue; // (not in this file)
        }
        if (size > cia_size || at > cia_size - size) {
            return kErrNotCia;
        }
        if (idx == index) {
            out->at = at;
            out->size = size;
            out->type = LoadBe16(b + 6);
            return 0;
        }
        at += Align64(size);
    }
    return kErrNoChild;
}

/// The RomFS of a game's content 2 inside its virtual CIA.
struct KeyedRomfs {
    ReadFn read = nullptr; // the virtual CIA
    void* ctx = nullptr;
    u64 romfs_at = 0;      // in the CIA
    u64 romfs_size = 0;
    bool plain = true;     // the container says NoCrypto: read as it is
    u64 partition = 0;     // the counter's first half (0x108)
    u32 rk[44] = {};       // aes_set_key of the NCCH layer's key (wiped by CloseKeyedRomfs)
    u8* scratch = nullptr; // a multiple of 16, at least 32: the aligned reads of the layer
    std::size_t scratch_size = 0;
};

/// Opens the RomFS of content 2 of the virtual CIA `read` gives: 0, kErrNotCia, kErrNoChild (no content 2, or one with no RomFS), kErrNcch (not an NCCH),
/// kErrContainerCrypto (a method other than 0, a seed, a fixed key: not keys of boot9.bin). `scratch` stays the caller's.
inline int OpenKeyedRomfs(ReadFn read, void* ctx, u64 cia_size, const Keys& keys, u8* scratch, std::size_t scratch_size, KeyedRomfs* out) {
    if (scratch == nullptr || scratch_size < 32 || scratch_size % 16 != 0) {
        return kErrNoMemory;
    }
    ContentAt c;
    const int found = FindContent(read, ctx, cia_size, kChildContentIndex, &c);
    if (found != 0) {
        return found;
    }
    u8 head[0x200];
    if (c.size < sizeof(head) || !read(ctx, c.at, head, sizeof(head))) {
        return kErrNotCia;
    }
    if (std::memcmp(head + 0x100, "NCCH", 4) != 0 || static_cast<u64>(LoadLe32(head + 0x104)) * kMediaUnit != c.size) {
        return kErrNcch;
    }
    const u8 flags = head[0x18F];
    const bool plain = (flags & 0x04) != 0;
    if (!plain && (head[0x18B] != 0 || (flags & 0x20) != 0 || (flags & 0x01) != 0)) {
        return kErrContainerCrypto;
    }
    const u64 romfs_at = static_cast<u64>(LoadLe32(head + 0x1B0)) * kMediaUnit;
    const u64 romfs_size = static_cast<u64>(LoadLe32(head + 0x1B4)) * kMediaUnit;
    if (romfs_size == 0) {
        return kErrNoChild;
    }
    if (romfs_at < 0x200 || romfs_at > c.size || romfs_size > c.size - romfs_at) {
        return kErrNcch;
    }
    KeyedRomfs k;
    k.read = read;
    k.ctx = ctx;
    k.romfs_at = c.at + romfs_at;
    k.romfs_size = romfs_size;
    k.plain = plain;
    k.partition = LoadLe64(head + 0x108);
    k.scratch = scratch;
    k.scratch_size = scratch_size;
    if (!plain) {
        u8 key[16];
        Scramble(keys.ncch_x, head, keys.generator, key); // KeyX 0x2C and the container's first 16 bytes
        aes_set_key(k.rk, key);
        Wipe(key, sizeof(key));
    }
    *out = k;
    Wipe(k.rk, sizeof(k.rk)); // (the copy is out's: this one does not stay on the stack)
    return 0;
}

/// A ReadFn over an open KeyedRomfs (`ctx` = the KeyedRomfs): `len` bytes at `off` of the RomFS image.
inline bool ReadKeyedRomfs(void* ctx, u64 off, u8* buf, u32 len) {
    KeyedRomfs* k = static_cast<KeyedRomfs*>(ctx);
    if (off > k->romfs_size || len > k->romfs_size - off) {
        return false;
    }
    if (k->plain) {
        return k->read(k->ctx, k->romfs_at + off, buf, len);
    }
    // The layer's counter runs from the RomFS's start, 16 bytes at a time: every read is made of whole blocks, in the scratch buffer.
    const u64 end = off + len;
    const u64 end16 = (end + 15) & ~15ull;
    u64 pos = off;
    u8* to = buf;
    while (pos < end) {
        const u64 a = pos & ~15ull;
        const u64 n = end16 - a < k->scratch_size ? end16 - a : k->scratch_size;
        if (!k->read(k->ctx, k->romfs_at + a, k->scratch, static_cast<u32>(n))) {
            return false;
        }
        u8 ctr[16];
        NcchCtr(k->partition, 3, ctr);
        aes_ctr_crypt(k->rk, ctr, a, k->scratch, static_cast<std::size_t>(n));
        const u64 take = end - pos < a + n - pos ? end - pos : a + n - pos;
        std::memcpy(to, k->scratch + (pos - a), static_cast<std::size_t>(take));
        to += take;
        pos += take;
    }
    return true;
}

inline void CloseKeyedRomfs(KeyedRomfs* k) {
    Wipe(k->rk, sizeof(k->rk));
    if (k->scratch != nullptr) {
        Wipe(k->scratch, k->scratch_size); // (plain bytes of a game: not left in a buffer that goes back to the heap)
    }
    k->read = nullptr;
}

// ---- what the app keeps of a child without composing it -----------------------------------------------------------------------------

/// A child's CIA as children.cache and a check of another version want it: its id, its TMD's version and content hash, its product code.
struct ChildFacts {
    u64 child = 0;
    u16 version = 0;
    u8 hash8[8] = {};
    char product[kProductBytes] = {}; // (the NCCH's 0x150: never changed by the composer; "" when it could not be read)
};

/// Reads the facts of the child's CIA at `cia_at` (`cia_size` bytes) of what `read` gives. The product code is under the title key: the first 0x200
/// bytes of the content are opened (and wiped) to read it. 0, kErrNoSource, kErrNotCia, kErrCommonKey or kErrTitleKey (boot9.bin is not this console's).
inline int ReadChildFacts(ReadFn read, void* ctx, u64 cia_at, u64 cia_size, const Keys& keys, ChildFacts* out) {
    u8 head[0x20];
    if (!read(ctx, cia_at, head, sizeof(head))) {
        return kErrNoSource;
    }
    CiaLayout lay;
    if (!ReadLayout(head, cia_size, &lay) || lay.ticket > 0x1000 || lay.tmd > 0x1000) {
        return kErrNotCia;
    }
    u8 t[0x1000];
    TicketFacts tik;
    if (!read(ctx, cia_at + lay.ticket_at, t, lay.ticket) || !ReadTicket(t, lay.ticket, &tik)) {
        return kErrNotCia;
    }
    TmdFacts tmd;
    if (!read(ctx, cia_at + lay.tmd_at, t, lay.tmd) || !ReadTmd(t, lay.tmd, &tmd) || tmd.tid != tik.tid || (tmd.tid >> 32) != kChildCategory ||
        tmd.size != lay.content || lay.content < 0x200) {
        return kErrNotCia;
    }
    u8 ncch[0x200];
    if (!read(ctx, cia_at + lay.content_at, ncch, sizeof(ncch))) {
        return kErrNoSource;
    }
    const bool encrypted = (tmd.type & 1) != 0;
    if (encrypted) {
        u8 title_key[16];
        if (!TitleKey(keys, tik, title_key)) {
            return kErrCommonKey;
        }
        AesDecKey dk;
        AesDecSetKey(&dk, title_key);
        u8 iv[16];
        ContentIv(tmd.index, iv);
        AesCbcDecrypt(dk, iv, ncch, sizeof(ncch));
        Wipe(title_key, sizeof(title_key));
        Wipe(&dk, sizeof(dk));
    }
    const bool magic = std::memcmp(ncch + 0x100, "NCCH", 4) == 0;
    ChildFacts f;
    f.child = tmd.tid;
    f.version = tmd.version;
    std::memcpy(f.hash8, tmd.hash, sizeof(f.hash8));
    if (magic) {
        std::memcpy(f.product, ncch + 0x150, 16); // (f.product keeps its NUL)
    }
    Wipe(ncch, sizeof(ncch));
    if (!magic) {
        return encrypted ? kErrTitleKey : kErrNcch;
    }
    *out = f;
    return 0;
}

} // namespace dlpguest
