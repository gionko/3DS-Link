#include "dlp_guest_compose.hpp"

#include <cstring>

#include "../titleshare_host/builtin_keys.h"

namespace dlpguest {

namespace {

// Everything of a key Compose holds, wiped on every way out of it
struct Secrets {
    u8 title_key[16];
    AesDecKey dk;
    u32 rk[44];  // aes.c's round keys of the NCCH layer's key
    u32 rk2[44]; // (method 1) ... of its secondary key, KeyX of slot 0x25
    u8 iv[16];   // the CBC chain of the stream
    ~Secrets() {
        Wipe(title_key, sizeof(title_key));
        Wipe(&dk, sizeof(dk));
        Wipe(rk, sizeof(rk));
        Wipe(rk2, sizeof(rk2));
        Wipe(iv, sizeof(iv));
    }
};

struct Source {
    const ComposeIn* in;
    u64 content_at, content;
    bool encrypted;
    const AesDecKey* dk;
    const u8* content_iv;
    const NcchPlan* plan;  // null until the NCCH header is read: then only the title key's layer comes off
    const u32* rk;
    const u32* rk2;
};

// The plain bytes [at, at + len) of the content, read at 16-byte boundaries with the 16 cipher bytes before them (their CBC IV: any block of a CBC
// stream opens alone). `space` holds len + 48 bytes. Returns where the bytes start in `space` (writable), or null when the read failed.
u8* ReadPlain(const Source& s, u64 at, std::size_t len, u8* space) {
    if (at > s.content || len > s.content - at) {
        return nullptr;
    }
    const u64 a0 = at & ~15ull;
    const u64 a1 = (at + len + 15) & ~15ull;  // (the content is a whole number of media units: a1 stays inside)
    const std::size_t n = static_cast<std::size_t>(a1 - a0);
    u8* plain = space + 16;
    if (s.encrypted && a0 >= 16) {
        if (!s.in->read(s.in->rctx, s.in->cia_at + s.content_at + a0 - 16, space, static_cast<u32>(n + 16))) {
            return nullptr;
        }
        u8 iv[16];
        std::memcpy(iv, space, 16);
        AesCbcDecrypt(*s.dk, iv, plain, n);
    } else {
        if (!s.in->read(s.in->rctx, s.in->cia_at + s.content_at + a0, plain, static_cast<u32>(n))) {
            return nullptr;
        }
        if (s.encrypted) {
            u8 iv[16];
            std::memcpy(iv, s.content_iv, 16);
            AesCbcDecrypt(*s.dk, iv, plain, n);
        }
    }
    if (s.plan != nullptr) {
        NcchDecrypt(*s.plan, s.rk, s.rk2, a0, plain, n);
    }
    return plain + (at - a0);
}

// (method 1, the build 2e slot25) The KeyX of slot 0x25 found in boot.firm, its normal key's round keys in `rk2`: 0, kErrNoSource, or kErrChildCrypto
// with the step in out->slot25. `head` is the NCCH header (plain), `space` holds kSmdhSize + 48 bytes. The RomFS's superblock is read with the title
// key off and the NCCH layer ON (`src` has no plan yet). The KeyX and its normal key are wiped before it returns. The time of the line is the search's
// alone: taken after boot.firm is read (the review of DPAPP-2e slot25: the first child of a run read ~300 KB from the SD card inside it), 0 before.
int SecondaryKey(const ComposeIn& in, const Source& src, const NcchPlan& plan, const u8* head, u8* space, u32 rk2[44], Composed* out) {
    u64 t0 = 0;
    bool timed = false;
    const auto done = [&](Slot25Step step, int rc) {
        out->slot25 = step;
        out->slot25_ms = timed ? static_cast<u32>(in.clock_ms() - t0) : 0;
        return rc;
    };
    const u64 hashed = static_cast<u64>(LoadLe32(head + 0x1B8)) * kMediaUnit;  // (the superblock 0x1E0 covers)
    if (plan.romfs_size == 0 || hashed == 0 || hashed > plan.romfs_size) {
        return done(Slot25Step::NoRomfs, kErrChildCrypto);  // (no RomFS: no "IVFC" to know the key by)
    }
    if (hashed > kSmdhSize) {
        return done(Slot25Step::RomfsLarge, kErrChildCrypto);  // (a superblock larger than the room to check it: never seen, never guessed)
    }
    const u8* firm = nullptr;
    std::size_t firm_size = 0;
    if (in.firm == nullptr || !in.firm(in.fctx, &firm, &firm_size)) {
        return done(Slot25Step::FirmMissing, kErrChildCrypto);
    }
    if (in.clock_ms != nullptr) {
        t0 = in.clock_ms();  // (after boot.firm is read: the ms of the line are the search's, never the SD card's or the CTRNAND's)
        timed = true;
    }
    const u8* romfs = ReadPlain(src, plan.romfs_at, static_cast<std::size_t>(hashed), space);
    if (romfs == nullptr) {
        return done(Slot25Step::None, kErrNoSource);
    }
    u8 x[16];
    const Slot25Search s = FindSlot25(firm, firm_size, head, plan.partition, romfs, static_cast<std::size_t>(hashed), in.keys->generator, x);
    out->slot25_at = s.at;
    out->slot25_candidates = s.candidates;
    if (s.step != Slot25Step::Found) {
        Wipe(x, sizeof(x));
        return done(s.step, kErrChildCrypto);
    }
    u8 key[16];
    Scramble(x, head, in.keys->generator, key);  // the secondary key: KeyX 0x25, the same KeyY (the header's first 16 bytes), C
    aes_set_key(rk2, key);
    Wipe(key, sizeof(key));
    Wipe(x, sizeof(x));
    return done(Slot25Step::Found, 0);
}

// The text of CryptoLine, made without printf (the composer writes nothing: Tests/policy_check.sh)
struct Text {
    char* p;
    std::size_t n, len;
    void Put(const char* s) {
        while (*s != '\0' && len + 1 < n) {
            p[len++] = *s++;
        }
        p[len] = '\0';
    }
    void Hex(u64 v, int digits) {
        char d[17];
        for (int i = digits - 1; i >= 0; --i) {
            d[i] = "0123456789ABCDEF"[v & 15];
            v >>= 4;
        }
        d[digits] = '\0';
        Put(d);
    }
    void Dec(u32 v) {
        char d[11];
        int i = 10;
        d[i] = '\0';
        do {
            d[--i] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0);
        Put(d + i);
    }
};

} // namespace

int ComposeKeys(const u8* boot9, std::size_t size, Keys* out) {
    std::memset(out, 0, sizeof(*out));
    if (!KeysFromBoot9(boot9, size, out) || bk_get(BK_GENERATOR_CONSTANT, out->generator) != 0 || bk_get(BK_COMMON_KEY_Y0, out->common_y0) != 0) {
        Wipe(out, sizeof(*out));
        return kErrBoot9NoKeys;
    }
    return 0;
}

int Compose(const ComposeIn& in, Composed* out) {
    if (out == nullptr) {
        return kErrNoMemory;
    }
    std::memset(out, 0, sizeof(*out));  // (first, before any way out: the run's line reads slot25 of THIS child, never one of before)
    if (in.read == nullptr || in.keys == nullptr || in.temp == nullptr || in.buf == nullptr || in.buf_size < 16 || in.buf_size % 16 != 0 ||
        in.buf_size > 0x40000000u || in.work == nullptr || in.work_size < kWorkBytes) {
        return kErrNoMemory;
    }
#ifndef DLPGUEST_TEST_VISIBLE
    if ((in.variant & kHide) == 0) {
        return kErrCannotHide;  // the user's decision: a prepared child is never installed visible (the plain P2a is made by Tests/ alone, for its md5)
    }
#endif
    if ((in.variant & kFixCrr) != 0) {
        return kErrVariant;
    }
    const bool hide = (in.variant & kHide) != 0;
    Secrets sec;

    // ---- the CIA's parts: the layout against its length, the front (header, certificates, ticket, TMD), one content of the child
    u8 head[0x20];
    if (in.cia_size < kCiaHeader || !in.read(in.rctx, in.cia_at, head, sizeof(head))) {
        return kErrNoSource;
    }
    CiaLayout& lay = out->lay;
    if (!ReadLayout(head, in.cia_size, &lay)) {
        return kErrNotCia;
    }
    if (lay.content_at > sizeof(out->front)) {
        return kErrNotCia;  // (a front larger than any child's: the usual one is 0x3900, a TMD signed with RSA-4096 adds 0x100)
    }
    if (!in.read(in.rctx, in.cia_at, out->front, static_cast<u32>(lay.content_at))) {
        return kErrNoSource;
    }
    if (!ReadTicket(out->front + lay.ticket_at, lay.ticket, &out->tik) || !ReadTmd(out->front + lay.tmd_at, lay.tmd, &out->tmd)) {
        return kErrNotCia;
    }
    const TmdFacts& tmd = out->tmd;
    if (tmd.tid != out->tik.tid || (tmd.tid >> 32) != kChildCategory || tmd.size != lay.content || lay.content < kPlainHead || lay.content % kMediaUnit != 0) {
        return kErrNotCia;
    }
    const bool encrypted = (tmd.type & 1) != 0;
    if (encrypted) {
        if (!TitleKey(*in.keys, out->tik, sec.title_key)) {
            return kErrCommonKey;
        }
        AesDecSetKey(&sec.dk, sec.title_key);
    }
    u8 content_iv[16];
    ContentIv(tmd.index, content_iv);
    Source src{&in, lay.content_at, lay.content, encrypted, &sec.dk, content_iv, nullptr, sec.rk, sec.rk2};
    if (in.step != nullptr && !in.step(in.sctx, kStageRead, 0, lay.content)) {
        return kErrCancel;
    }

    // ---- before the stream: the header and the exheader (the changes and the new 0x160), and for the hidden child the ExeFS's icon and hashes.
    // The magic after the title key says at once whether boot9.bin is right; the exheader's hash says it of the NCCH layer.
    u8* const part_a = in.work;
    u8* const part_b = in.work + kHashedMax + 48;
    u8* plain_head = ReadPlain(src, 0, kPlainHead, part_a);
    if (plain_head == nullptr) {
        return kErrNoSource;
    }
    if (std::memcmp(plain_head + 0x100, "NCCH", 4) != 0) {
        return encrypted ? kErrTitleKey : kErrNcch;
    }
    std::memcpy(out->ncch_flags, plain_head + 0x188, sizeof(out->ncch_flags));  // (plain: the line of the log says them, CryptoLine)
    NcchPlan plan;
    out->why = ReadNcch(plain_head, lay.content, *in.keys, &plan);
    switch (out->why) {
    case NcchWhy::Ok: break;
    case NcchWhy::Method:
    case NcchWhy::Seed:
    case NcchWhy::FixedKey: return kErrChildCrypto;
    default: return kErrNcch;
    }
    aes_set_key(sec.rk, plan.key);
    Wipe(plan.key, sizeof(plan.key));
    NcchDecrypt(plan, sec.rk, sec.rk2, 0, plain_head, kPlainHead);  // (the exheader: the primary key for both methods - rk2 is not read here)
    u8 ncch_head[0x200];
    std::memcpy(ncch_head, plain_head, sizeof(ncch_head));
    PatchList patches;
    if (HeaderPatches(plain_head, in.prepared, in.am_title != 0 ? in.am_title : in.prepared, &patches) != NcchWhy::Ok) {
        return kErrNcch;
    }
    if (plan.two_keys) {
        // (the review of DPAPP-2e slot25) after the exheader's hash held with the primary key: a damaged content is 309, its boot.firm never read
        const int keyed = SecondaryKey(in, src, plan, plain_head, part_b, sec.rk2, out);  // (method 1: the KeyX of slot 0x25, from boot.firm)
        if (keyed != 0) {
            return keyed;
        }
    }
    src.plan = &plan;
    if (plan.two_keys && plan.exefs_size >= 0x200) {
        // (method 1) which files of the ExeFS are under which key: its header (under the primary key) before any of them is read
        const u8* exefs_head = ReadPlain(src, plan.exefs_at, 0x200, part_b);
        if (exefs_head == nullptr) {
            return kErrNoSource;
        }
        if (ExefsKeys(exefs_head, &plan) != NcchWhy::Ok) {
            return kErrNcch;
        }
    }
    if (hide) {
        if (plan.exefs_size < 0x200 || plan.exefs_hash_size < 0x200 || plan.exefs_hash_size > kHashedMax) {
            return kErrCannotHide;
        }
        const u8* exefs_head = ReadPlain(src, plan.exefs_at, 0x200, part_b);
        if (exefs_head == nullptr) {
            return kErrNoSource;
        }
        IconPlace icon;
        if (FindIcon(exefs_head, plan.exefs_at, plan.exefs_size, &icon) != NcchWhy::Ok) {
            return kErrCannotHide;  // no icon file: the bit that hides it is not there to turn off
        }
        const u8* hashed = ReadPlain(src, plan.exefs_at, static_cast<std::size_t>(plan.exefs_hash_size), part_a);
        const u8* icon_bytes = hashed == nullptr ? nullptr : ReadPlain(src, icon.at, kSmdhSize, part_b);
        if (icon_bytes == nullptr) {
            return kErrNoSource;
        }
        if (HidePatches(ncch_head, plan.exefs_at, hashed, static_cast<std::size_t>(plan.exefs_hash_size), icon, icon_bytes, &patches, &out->icon_flags_before) !=
            NcchWhy::Ok) {
            return kErrCannotHide;
        }
    }

    // ---- the stream: CBC (the IV carried from chunk to chunk), the hash of what the TMD names, CTR, the changes, the new hash, the work file
    sha256_ctx orig, fresh, code;
    sha256_init(&orig);
    sha256_init(&fresh);
    sha256_init(&code);
    std::memcpy(sec.iv, content_iv, 16);
    for (u64 off = 0; off < lay.content;) {
        const u32 n = static_cast<u32>(lay.content - off < in.buf_size ? lay.content - off : in.buf_size);
        if (!in.read(in.rctx, in.cia_at + lay.content_at + off, in.buf, n)) {
            return kErrNoSource;
        }
        if (encrypted) {
            AesCbcDecrypt(sec.dk, sec.iv, in.buf, n);
        }
        sha256_update(&orig, in.buf, n);
        NcchDecrypt(plan, sec.rk, sec.rk2, off, in.buf, n);
        const u64 ca = off > plan.code_at ? off : plan.code_at;  // (method 1: .code in the clear, before any change - none falls on it)
        const u64 cb = (off + n) < (plan.code_at + plan.code_size) ? (off + n) : (plan.code_at + plan.code_size);
        if (plan.code_size != 0 && ca < cb) {
            sha256_update(&code, in.buf + (ca - off), static_cast<std::size_t>(cb - ca));
        }
        ApplyPatches(patches, off, in.buf, n);
        sha256_update(&fresh, in.buf, n);
        if (!in.temp(in.tctx, off, in.buf, n)) {
            return kErrTemp;
        }
        off += n;
        if (in.step != nullptr && !in.step(in.sctx, kStageDecrypt, off, lay.content)) {
            return kErrCancel;
        }
    }
    u8 orig_hash[32];
    sha256_final(&orig, orig_hash);
    sha256_final(&fresh, out->new_hash);
    if (std::memcmp(orig_hash, tmd.hash, 32) != 0) {
        return kErrSourceHash;  // the content is not the one its TMD names: nothing is installed
    }
    if (plan.code_size != 0) {
        u8 code_hash[32];
        sha256_final(&code, code_hash);
        if (std::memcmp(code_hash, plan.code_hash, 32) != 0) {
            out->slot25 = Slot25Step::CodeHash;
            return kErrChildCrypto;  // (method 1) the KeyX found does not open .code: the second confirmation fails, nothing is installed
        }
    }

    // ---- the front and the meta block of the new CIA
    if (!MakeFront(out->front, lay, out->tik, tmd, in.am_title != 0 ? in.am_title : in.prepared, out->new_hash)) {
        return kErrNotCia;
    }
    if (!in.read(in.rctx, in.cia_at + lay.meta_at, out->meta, lay.meta)) {
        return kErrNoSource;
    }
    MakeMeta(out->meta, lay.meta, hide);
    out->hidden = hide;
    return 0;
}

void CryptoLine(const Composed& c, u64 game, char* out, std::size_t n) {
    if (out == nullptr || n == 0) {
        return;
    }
    Text t{out, n, 0};
    t.Put("guest: child crypto ");
    t.Hex(c.tmd.tid, 16);
    t.Put(" game ");
    t.Hex(game, 16);
    t.Put(" flags ");
    for (int i = 0; i < 8; ++i) {
        t.Hex(c.ncch_flags[i], 2);
    }
    const u8 method = c.ncch_flags[3], flags = c.ncch_flags[7];
    if (c.slot25 == Slot25Step::Found) {
        t.Put(" slot 0x25 key found at 0x");
        t.Hex(c.slot25_at, 8);
    } else if (c.slot25 != Slot25Step::None || (c.why == NcchWhy::Method && method == 1)) {
        t.Put(" why Method (0x18B=01: slot 0x25) step ");
        t.Put(Slot25StepName(c.slot25));
        if (c.slot25_at != 0) {
            t.Put(" at 0x");
            t.Hex(c.slot25_at, 8);
        }
    } else if (c.why == NcchWhy::Method) {
        t.Put(" why Method (0x18B=");
        t.Hex(method, 2);
        t.Put(method == 0x0A ? ": slot 0x18, not supported)" : (method == 0x0B ? ": slot 0x1B, not supported)" : ": unknown)"));
    } else if (c.why == NcchWhy::Seed || c.why == NcchWhy::FixedKey) {
        t.Put(c.why == NcchWhy::Seed ? " why Seed (0x18F=" : " why FixedKey (0x18F=");
        t.Hex(flags, 2);
        t.Put(")");
    } else {
        t.Put(" why other");
    }
    if (c.slot25 != Slot25Step::None) {
        t.Put(", ");
        t.Dec(c.slot25_candidates);
        t.Put(" candidates, ");
        t.Dec(c.slot25_ms);
        t.Put(" ms");
    }
}

int Emit(const Composed& c, ReadFn temp_read, void* t, WriteFn sink, void* s, StepFn step, void* sc, u8* buf, std::size_t n) {
    if (temp_read == nullptr || sink == nullptr || buf == nullptr || n == 0) {
        return kErrNoMemory;
    }
    const CiaLayout& lay = c.lay;
    const u64 chunk = n < 0x40000000u ? n : 0x40000000u;
    if (!sink(s, 0, c.front, static_cast<u32>(lay.content_at))) {
        return kErrInstall;
    }
    for (u64 off = 0; off < lay.content;) {
        const u32 m = static_cast<u32>(lay.content - off < chunk ? lay.content - off : chunk);
        if (!temp_read(t, off, buf, m)) {
            return kErrTemp;
        }
        if (!sink(s, lay.content_at + off, buf, m)) {
            return kErrInstall;
        }
        off += m;
        if (step != nullptr && !step(sc, kStageInstall, off, lay.content)) {
            return kErrCancel;
        }
    }
    static const u8 kZeros[64] = {0};
    const u64 end = lay.content_at + lay.content;
    if (lay.meta_at > end && !sink(s, end, kZeros, static_cast<u32>(lay.meta_at - end))) {
        return kErrInstall;
    }
    if (!sink(s, lay.meta_at, c.meta, lay.meta)) {
        return kErrInstall;
    }
    if (step != nullptr) {
        step(sc, kStageFinish, lay.total, lay.total);
    }
    return 0;
}

} // namespace dlpguest
