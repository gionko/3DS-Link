#include "dlp_guest_ids.hpp"

#include <algorithm>

#include "azl/dlp_children.hpp"
#include "dlp_guest_paths.hpp"
#include "fileio.hpp"
#include "paths.hpp"

namespace dlpguest {

namespace {

constexpr const char* kIdsHeader = "#dlp-ids 1";
constexpr const char* kIdsFileName = "ids.cache";
constexpr std::size_t kIdsMaxBytes = 2 * 1024 * 1024; // (20 bytes an id: about a hundred thousand ids - the 65536 tickets of dlp_guest_ids_am.cpp and the titles)
constexpr const char* kSourceWords[kSrcCount] = {"sd", "nand", "ticket", "card", "process", "pending-sd", "pending-nand"};
constexpr u64 kFnvBasis = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime = 0x100000001B3ull;

// ---- text (no printf family in the dlp_guest files) ----
std::string Hex16(u64 v) {
    char p[16];
    azl::dlp_children::PutHex16(p, v);
    return std::string(p, 16);
}

std::string Hex2(unsigned v) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    return std::string(1, kDigits[(v >> 4) & 0xF]) + kDigits[v & 0xF];
}

std::string Dec(u32 v) {
    char d[10];
    int n = 0;
    do {
        d[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0 && n < 10);
    std::string s;
    while (n > 0) {
        s += d[--n];
    }
    return s;
}

bool ReadDec(const std::string& s, u32* out) {
    if (s.empty() || s.size() > 10) {
        return false;
    }
    u64 v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<u64>(c - '0');
    }
    if (v > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<u32>(v);
    return true;
}

bool ReadHexN(const std::string& s, std::size_t digits, u64* out) {
    if (s.size() != digits) {
        return false;
    }
    u64 v = 0;
    for (const char c : s) {
        u32 d;
        if (c >= '0' && c <= '9') {
            d = static_cast<u32>(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<u32>(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<u32>(c - 'a' + 10);
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

u64 FnvOfText(const std::string& t) {
    u64 h = kFnvBasis;
    for (const char c : t) {
        h ^= static_cast<u8>(c);
        h *= kFnvPrime;
    }
    return h;
}

std::vector<std::string> Words(const std::string& line) {
    std::vector<std::string> w;
    std::string cur;
    for (const char c : line) {
        if (c == ' ') {
            if (!cur.empty()) {
                w.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        w.push_back(cur);
    }
    return w;
}

void SortUnique(std::vector<u64>* v) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
}

bool Holds(const std::vector<u64>& sorted, u64 id) {
    return std::binary_search(sorted.begin(), sorted.end(), id);
}

// a process whose id, without its low byte, is `id`'s (pm's own search, process_data.c:37)
bool RunsAs(const std::vector<u64>& processes, u64 id) {
    if (id == 0) {
        return false;
    }
    for (const u64 p : processes) {
        if ((p & ~0xFFull) == (id & ~0xFFull)) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* SourceWord(int source) {
    return source >= 0 && source < kSrcCount ? kSourceWords[source] : "?";
}

u64 FnvOfIds(const std::vector<u64>& sorted) {
    u64 h = kFnvBasis;
    for (const u64 id : sorted) {
        for (int b = 0; b < 8; ++b) {
            h ^= static_cast<u8>(id >> (8 * b));
            h *= kFnvPrime;
        }
    }
    return h;
}

bool RefreshCensus(IdCensus* census, IdReader& reader, RefreshReport* report) {
    RefreshReport r;
    const u64 began = reader.NowMs();
    for (int s = 0; s < kSrcCount; ++s) {
        const u64 t0 = reader.NowMs();
        std::vector<u64> ids;
        const bool read = reader.List(s, &ids);
        r.ms[s] = static_cast<u32>(reader.NowMs() - t0);
        SourceState& st = census->src[s];
        if (!read) {
            r.unread |= SrcBit(s);
            if (st.read || !census->valid) {
                r.changed |= SrcBit(s); // (a list known before and not now: nothing is known of it any more)
            }
            st = SourceState();
            st.ms = r.ms[s];
            census->ids[s].clear();
            continue;
        }
        SortUnique(&ids);
        const u64 fnv = FnvOfIds(ids);
        if (!census->valid || !st.read || st.count != ids.size() || st.fnv != fnv) {
            r.changed |= SrcBit(s); // (only this source is put in again)
            census->ids[s] = ids;
            st.read = true;
            st.count = static_cast<u32>(ids.size());
            st.fnv = fnv;
        }
        st.ms = r.ms[s];
    }
    census->valid = true;
    r.total_ms = static_cast<u32>(reader.NowMs() - began);
    if (report != nullptr) {
        *report = r;
    }
    return (r.unread & kNeededSources) == 0;
}

std::string RefreshLine(const IdCensus& census, const RefreshReport& report, const char* when) {
    std::string t = std::string("ids (") + when + "):";
    for (int s = 0; s < kSrcCount; ++s) {
        t += std::string(" ") + kSourceWords[s] + " ";
        t += census.src[s].read ? Dec(census.src[s].count) : std::string("NOT READ");
        t += " (" + Dec(report.ms[s]) + " ms)";
    }
    t += " - changed:";
    bool any = false;
    for (int s = 0; s < kSrcCount; ++s) {
        if (report.changed & SrcBit(s)) {
            t += std::string(any ? ", " : " ") + kSourceWords[s];
            any = true;
        }
    }
    t += any ? "" : " none";
    t += "; " + Dec(report.total_ms) + " ms in all";
    return t;
}

std::string CensusText(const IdCensus& census) {
    std::string body;
    for (int s = 0; s < kSrcCount; ++s) {
        const SourceState& st = census.src[s];
        body += std::string("source ") + kSourceWords[s] + " " + (st.read ? "1" : "0") + " " + Dec(st.count) + " " + Hex16(st.fnv) + " " + Dec(st.ms) + "\n";
    }
    // one line per id, its sources together
    std::vector<std::pair<u64, unsigned>> all;
    for (int s = 0; s < kSrcCount; ++s) {
        for (const u64 id : census.ids[s]) {
            all.emplace_back(id, SrcBit(s));
        }
    }
    std::sort(all.begin(), all.end());
    for (std::size_t i = 0; i < all.size();) {
        unsigned mask = 0;
        const u64 id = all[i].first;
        while (i < all.size() && all[i].first == id) {
            mask |= all[i].second;
            ++i;
        }
        body += Hex16(id) + ":" + Hex2(mask) + "\n";
    }
    return std::string(kIdsHeader) + "\nseal " + Hex16(FnvOfText(body)) + "\n" + body;
}

bool ParseCensus(const std::string& text, IdCensus* out) {
    const std::string head = std::string(kIdsHeader) + "\nseal ";
    if (text.compare(0, head.size(), head) != 0 || text.size() < head.size() + 17 || text[head.size() + 16] != '\n') {
        return false;
    }
    u64 seal = 0;
    if (!ReadHexN(text.substr(head.size(), 16), 16, &seal)) {
        return false;
    }
    const std::string body = text.substr(head.size() + 17);
    if (FnvOfText(body) != seal) {
        return false; // (a file cut or edited: read again from the console, never trusted)
    }
    IdCensus c;
    bool seen[kSrcCount] = {};
    std::size_t at = 0;
    while (at < body.size()) {
        const std::size_t end = body.find('\n', at);
        if (end == std::string::npos) {
            return false;
        }
        const std::string line = body.substr(at, end - at);
        at = end + 1;
        if (line.compare(0, 7, "source ") == 0) {
            const std::vector<std::string> w = Words(line);
            int s = -1;
            for (int i = 0; w.size() == 6 && i < kSrcCount; ++i) {
                s = w[1] == kSourceWords[i] ? i : s;
            }
            u32 count = 0, ms = 0;
            u64 fnv = 0;
            if (s < 0 || seen[s] || (w[2] != "0" && w[2] != "1") || !ReadDec(w[3], &count) || !ReadHexN(w[4], 16, &fnv) || !ReadDec(w[5], &ms)) {
                return false;
            }
            seen[s] = true;
            c.src[s].read = w[2] == "1";
            c.src[s].count = count;
            c.src[s].fnv = fnv;
            c.src[s].ms = ms;
            continue;
        }
        u64 id = 0, mask = 0;
        if (line.size() != 19 || line[16] != ':' || !ReadHexN(line.substr(0, 16), 16, &id) || !ReadHexN(line.substr(17, 2), 2, &mask) ||
            mask == 0 || mask >= (1u << kSrcCount)) {
            return false;
        }
        for (int s = 0; s < kSrcCount; ++s) {
            if (mask & SrcBit(s)) {
                c.ids[s].push_back(id);
            }
        }
    }
    for (int s = 0; s < kSrcCount; ++s) {
        SortUnique(&c.ids[s]);
        if (!seen[s] || c.ids[s].size() != c.src[s].count || FnvOfIds(c.ids[s]) != (c.src[s].read ? c.src[s].fnv : FnvOfIds({})) ||
            (!c.src[s].read && !c.ids[s].empty())) {
            return false; // (the lists are not the ones their lines say)
        }
    }
    c.valid = true;
    *out = c;
    return true;
}

std::string IdsCachePath() {
    const std::string dir = RecordsDir(); // (DPAPP-2j, R-B: the census is THIS console's - in its folder; "" when its ID0 is not known)
    return dir.empty() ? std::string() : dir + "/" + kIdsFileName;
}

bool LoadCensus(IdCensus* out) {
    std::string text;
    IdCensus c;
    if (setup::ReadTextFile(IdsCachePath(), kIdsMaxBytes, text) != setup::LoadResult::Loaded || !ParseCensus(text, &c)) {
        *out = IdCensus();
        return false;
    }
    *out = c;
    return true;
}

bool SaveCensus(const IdCensus& census) {
    if (!census.valid || RecordsDir().empty()) {
        return false;
    }
    setup::MakeDirs(RecordsDir());
    return setup::WriteTextFile(IdsCachePath(), CensusText(census));
}

LiveSlot SlotIn(const IdCensus& census, u64 slot) {
    LiveSlot l;
    for (int s = 0; s < kSrcCount; ++s) {
        l.read |= census.src[s].read ? SrcBit(s) : 0u;
    }
    if (!census.valid) {
        l.read = 0;
        return l;
    }
    for (const u8 f : kForms) {
        const u64 am = AmTitleFor(slot, f);
        const u64 process = ProcessIdFor(slot, f);
        unsigned m = 0;
        for (const int s : {kSrcSd, kSrcNand, kSrcTicket, kSrcCard, kSrcPendingSd, kSrcPendingNand}) {
            m |= am != 0 && Holds(census.ids[s], am) ? SrcBit(s) : 0u;
        }
        m |= RunsAs(census.ids[kSrcProcess], am) || RunsAs(census.ids[kSrcProcess], process) ? SrcBit(kSrcProcess) : 0u;
        l.mask[f] = m;
    }
    return l;
}

bool SameSlot(const LiveSlot& a, const LiveSlot& b) {
    if (a.read != b.read) {
        return false;
    }
    for (u8 f = 0; f < kFormCount; ++f) {
        if (a.mask[f] != b.mask[f]) {
            return false;
        }
    }
    return true;
}

std::string SlotText(const LiveSlot& s) {
    static constexpr const char* kFormWords[kFormCount] = {"app", "dlp", "sys"};
    std::string t;
    for (const u8 f : kForms) {
        t += t.empty() ? "" : ", ";
        t += std::string(kFormWords[f]) + " ";
        std::string m;
        for (int src = 0; src < kSrcCount; ++src) {
            if (s.mask[f] & SrcBit(src)) {
                m += (m.empty() ? "" : "+") + std::string(kSourceWords[src]);
            }
        }
        t += m.empty() ? "-" : m;
    }
    if ((s.read & kNeededSources) != kNeededSources) {
        t += " (lists not read)";
    }
    return t;
}

const char* TakenWord(IdTaken t) {
    switch (t) {
    case IdTaken::Free: return "free";
    case IdTaken::Unknown: return "a list of AM not read";
    case IdTaken::Nand: return "in the NAND";
    case IdTaken::Running: return "a process runs with it";
    case IdTaken::Card: return "on the game card";
    case IdTaken::Sd: return "a title of the SD card that is not ours";
    case IdTaken::Ticket: return "a ticket that is not ours";
    case IdTaken::Pending: return "a title being installed that is not ours";
    }
    return "?";
}

IdTaken JudgeInstall(const LiveSlot& live, u8 form, const Ownership& own) {
    if ((live.read & kNeededSources) != kNeededSources) {
        return IdTaken::Unknown; // R1
    }
    for (const u8 f : kForms) {
        if (live.mask[f] & (SrcBit(kSrcNand) | SrcBit(kSrcPendingNand))) {
            return IdTaken::Nand; // R2: an id of the slot in the NAND - nothing of the slot is ever written
        }
    }
    for (const u8 f : kForms) {
        if (live.mask[f] & SrcBit(kSrcProcess)) {
            return IdTaken::Running; // R3
        }
    }
    for (const u8 f : kForms) {
        if (live.mask[f] & SrcBit(kSrcCard)) {
            return IdTaken::Card; // R4
        }
    }
    for (const u8 f : kForms) {
        if ((live.mask[f] & SrcBit(kSrcSd)) && !(own.sd_ours & FormBit(f))) {
            return IdTaken::Sd; // R5: a title that is not ours at any id of the slot
        }
    }
    if ((live.mask[form] & SrcBit(kSrcTicket)) && !own.ticket_ours) {
        return IdTaken::Ticket; // R5: the ticket of the id written is somebody else's
    }
    if ((live.mask[form] & SrcBit(kSrcPendingSd)) && !own.record) {
        return IdTaken::Pending; // R5: a title begun at the id that no run of ours began
    }
    return IdTaken::Free;
}

IdTaken JudgeDelete(const LiveSlot& live, u8 form, const Ownership& own) {
    if ((live.read & kNeededSources) != kNeededSources) {
        return IdTaken::Unknown; // R1
    }
    if (live.mask[form] & (SrcBit(kSrcNand) | SrcBit(kSrcPendingNand))) {
        return IdTaken::Nand; // R2
    }
    if (live.mask[form] & SrcBit(kSrcProcess)) {
        return IdTaken::Running; // R3: never a title whose program pm still holds
    }
    if (live.mask[form] & SrcBit(kSrcCard)) {
        return IdTaken::Card; // R4
    }
    if ((live.mask[form] & SrcBit(kSrcSd)) && !(own.sd_ours & FormBit(form))) {
        return IdTaken::Sd; // R6: only our title
    }
    if (!own.record) {
        return IdTaken::Sd; // R6: never without our record
    }
    return IdTaken::Free;
}

bool AmMayWrite(const IdCensus& live, u64 am, u64 process, bool install, bool ticket_expected, bool title_expected) {
    const unsigned needed = kNeededSources;
    unsigned read = 0;
    for (int s = 0; s < kSrcCount; ++s) {
        read |= live.src[s].read ? SrcBit(s) : 0u;
    }
    if (!live.valid || (read & needed) != needed || am == 0 || SlotKeyOf(am) == 0) {
        return false;
    }
    if (!SlotIsSound(SlotKeyOf(am))) {
        return false; // (the review of the build 2h, R5: R0 in the second guard too - never 3DS Link's id, with or without its low byte)
    }
    if (Holds(live.ids[kSrcNand], am) || Holds(live.ids[kSrcPendingNand], am) || Holds(live.ids[kSrcCard], am)) {
        return false;
    }
    if (RunsAs(live.ids[kSrcProcess], am) || RunsAs(live.ids[kSrcProcess], process)) {
        return false;
    }
    if (install && Holds(live.ids[kSrcTicket], am) && !ticket_expected) {
        return false;
    }
    if (install && Holds(live.ids[kSrcSd], am) && !title_expected) {
        return false; // (a title at the id that the caller's guard did not judge ours: AM would put the child over it at the Finish)
    }
    return true;
}

} // namespace dlpguest
