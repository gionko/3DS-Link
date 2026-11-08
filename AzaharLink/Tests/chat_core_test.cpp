// The chat of the room (azl/chat_core.hpp), run by Tests/run.sh (with ASan/UBSan, and again with ThreadSanitizer for the seqlock):
// the cleaning of what the room relays (printable ASCII, a UTF-8 character as one '?', controls and escapes as a space, spaces
// collapsed, nothing left = dropped), the cut at 128 with "...", a 5000-character message that costs what 128 do (the scan never
// reads past its bound: ASan would stop the test), malformed packets, the ring of 5, the seqlock between a writer and a reader
// thread, the rate limit (50 a second -> 4, of one member -> 2: a friend's messages pass a flood), the outgoing box (one a second, only while joined, our own line kept) and no
// allocation in any of it (malloc is wrapped by the link, operator new goes through it).
// The same code against the real room: Tests/chat_room_probe.cpp, run by Tests/integration.sh.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "azl/chat_core.hpp"

using namespace azl;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                                          \
    } while (0)
#define CHECK_STR(a, b)                                                                            \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const std::string got_ = (a);                                                              \
        if (got_ != (b)) {                                                                         \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: '%s' is not '%s'\n", __FILE__, __LINE__, got_.c_str(), std::string(b).c_str()); \
        }                                                                                          \
    } while (0)

// ---- every allocation of the process is counted (the link wraps malloc; operator new is replaced to go through it) ----
static std::atomic<unsigned> g_allocs{0};
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return __real_malloc(n);
}
void* operator new(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) {
        std::abort();
    }
    return p;
}
void* operator new[](std::size_t n) {
    return operator new(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

// Statics, not the stack or the heap: the cases below must allocate nothing themselves.
static char g_out[chat::kTextMax + 1];
static u8 g_packet[8192];

static std::string Clean(const std::string& in, u32 cap = chat::kTextMax, bool ellipsis = true) {
    const u32 n = chat::Sanitize(reinterpret_cast<const u8*>(in.data()), in.size(), g_out, cap, ellipsis);
    CHECK(std::strlen(g_out) == n);
    return std::string(g_out, n);
}

/// [0x06][str nick][str user][str msg] into g_packet; the size.
static std::size_t Packet(const char* nick, std::size_t nick_n, const char* user, std::size_t user_n, const char* msg, std::size_t msg_n) {
    std::size_t p = 0;
    g_packet[p++] = chat::kIdChatMessage;
    StoreBE32(g_packet + p, static_cast<u32>(nick_n));
    std::memcpy(g_packet + p + 4, nick, nick_n);
    p += 4 + nick_n;
    StoreBE32(g_packet + p, static_cast<u32>(user_n));
    std::memcpy(g_packet + p + 4, user, user_n);
    p += 4 + user_n;
    StoreBE32(g_packet + p, static_cast<u32>(msg_n));
    std::memcpy(g_packet + p + 4, msg, msg_n);
    p += 4 + msg_n;
    return p;
}
static std::size_t Packet(const char* nick, const char* user, const char* msg) {
    return Packet(nick, std::strlen(nick), user, std::strlen(user), msg, std::strlen(msg));
}

static void TestSanitize() {
    std::printf("the cleaning: printable ASCII, UTF-8 as one '?', controls and escapes as a space, spaces collapsed\n");
    CHECK_STR(Clean("Hello, room! 1+1=2 ~{}"), "Hello, room! 1+1=2 ~{}");
    CHECK_STR(Clean("h\xC3\xA9llo"), "h?llo");                       // 2 bytes
    CHECK_STR(Clean("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB"), "???"); // three characters of 3 bytes
    CHECK_STR(Clean("ok \xF0\x9F\x98\x80!"), "ok ?!");               // 4 bytes
    CHECK_STR(Clean("a\x80\x80" "b"), "a??b");                        // stray continuation bytes: a '?' each
    CHECK_STR(Clean("ab\xE2\x82"), "ab?");                           // a sequence cut by the end
    CHECK_STR(Clean("a\tb\r\n\nc"), "a b c");
    CHECK_STR(Clean("x\x7Fy"), "x y");                               // DEL
    CHECK_STR(Clean("\x1B[1;31mred\x1B[0m text"), "red text");        // a CSI sequence as one space
    CHECK_STR(Clean("a\x1B" "Db"), "a b");                            // a two-byte escape
    CHECK_STR(Clean("a\x1B"), "a");                                   // ESC at the end
    CHECK_STR(Clean("a     b"), "a b");
    CHECK_STR(Clean("   lead and trail   "), "lead and trail");
    CHECK_STR(Clean(std::string("a\0b", 3)), "a b");                  // NUL is a control
    CHECK(Clean("") == "");
    CHECK(Clean("   \t\r\n \x1B[0m ") == "");                         // nothing left: dropped
    CHECK_STR(Clean("Gionko-3DS_x.y"), "Gionko-3DS_x.y");
}

static void TestTruncation() {
    std::printf("the cut: 128 characters at most, 125 and \"...\" when the message was longer; the nick cut at 20\n");
    const std::string a128(128, 'a');
    CHECK_STR(Clean(a128), a128);
    CHECK_STR(Clean(std::string(129, 'b')), std::string(125, 'b') + "...");
    CHECK_STR(Clean(a128 + "     "), a128); // (trailing spaces are no text)
    CHECK_STR(Clean(std::string(124, 'c') + " dddd"), std::string(124, 'c') + "...");
    const std::string big(5000, 'z');
    const std::string cut = Clean(big);
    CHECK(cut.size() == 128);
    CHECK(cut.compare(125, 3, "...") == 0 && cut.substr(0, 125) == std::string(125, 'z'));
    CHECK(Clean(std::string(5000, ' ')) == ""); // (bounded too: a field of spaces is dropped, whatever its length)
    // a UTF-8 text past 128 characters: 200 of 3 bytes
    std::string jp;
    for (int i = 0; i < 200; ++i) {
        jp += "\xE3\x81\x82";
    }
    CHECK_STR(Clean(jp), std::string(125, '?') + "...");
    CHECK_STR(Clean("ABCDEFGHIJKLMNOPQRSTUVWXYZ", chat::kNickMax, false), "ABCDEFGHIJKLMNOPQRST");
    CHECK_STR(Clean("Exactly20Characters_", chat::kNickMax, false), "Exactly20Characters_");
}

static void TestScanBound() {
    std::printf("a 5000-character message costs what 128 do: the scan never reads past cap * 4 + 16 bytes\n");
    // The buffer holds only the bytes the scan may read; the length says 5000. A read past them is a heap overflow that ASan reports.
    const std::size_t bound = chat::kTextMax * chat::kScanPerChar + 16;
    std::vector<u8> head(bound, ' ');
    head[0] = 'h';
    head[1] = 'i';
    const u32 n = chat::Sanitize(head.data(), 5000, g_out, chat::kTextMax, true);
    CHECK(n == 5 && std::string(g_out) == "hi..."); // (input left unread: the reader is told the message was not read to its end)
    std::vector<u8> nick_head(chat::kNickMax * chat::kScanPerChar + 16, 0x01);
    CHECK(chat::Sanitize(nick_head.data(), 1u << 30, g_out, chat::kNickMax, false) == 0);
}

static void TestDecode() {
    std::printf("the room's chat packet decoded by hand: the nick and the text cleaned, the username skipped, bad lengths refused\n");
    chat::Line line;
    std::size_t n = Packet("Gionko", "gionko_user", "hello there");
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Ok);
    CHECK_STR(std::string(line.nick, line.nick_len), "Gionko");
    CHECK_STR(std::string(line.text, line.text_len), "hello there");
    CHECK(line.nick[line.nick_len] == '\0' && line.text[line.text_len] == '\0' && line.own == 0);
    // an empty username, and one of 3000 bytes
    n = Packet("Pippo", "", "a");
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Ok && line.text_len == 1);
    static char user[3000];
    std::memset(user, 'u', sizeof(user));
    n = Packet("Pippo", 5, user, sizeof(user), "after a big user", 16);
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Ok);
    CHECK_STR(std::string(line.text, line.text_len), "after a big user");
    // a nick of 20 and one longer (the room would refuse it: cut all the same)
    n = Packet("TwentyCharactersNick", "", "x");
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Ok && line.nick_len == 20);
    n = Packet("TwentyCharactersNickAndMore", "", "x");
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Ok && line.nick_len == 20);
    CHECK_STR(std::string(line.nick, line.nick_len), "TwentyCharactersNick");
    // empty after the cleaning
    n = Packet("Gionko", "", "  \r\n ");
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Empty);
    n = Packet("\x01\x02", "", "text");
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Empty);
    // malformed: no packet, another id, every cut of a good packet, lengths past the end
    CHECK(chat::DecodeChatMessage(g_packet, 0, &line) == chat::Decoded::Malformed);
    n = Packet("Gionko", "user", "hi");
    for (std::size_t cut = 1; cut < n; ++cut) {
        CHECK(chat::DecodeChatMessage(g_packet, cut, &line) == chat::Decoded::Malformed);
    }
    g_packet[0] = 14; // IdStatusMessage is not the chat's
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Malformed);
    g_packet[0] = chat::kIdChatMessage;
    StoreBE32(g_packet + 1, 0xFFFFFFFFu); // a nick "of 4 GB"
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Malformed);
    n = Packet("Gionko", "user", "hi");
    StoreBE32(g_packet + 1 + 4 + 6, 0xFFFFFFFCu); // a username that wraps the position round
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Malformed);
    n = Packet("Gionko", "user", "hi");
    StoreBE32(g_packet + n - 2 - 4, 3); // the message one byte longer than what came
    CHECK(chat::DecodeChatMessage(g_packet, n, &line) == chat::Decoded::Malformed);
    // bytes after the message are ignored (a later version of the room may add a field)
    n = Packet("Gionko", "user", "hi");
    g_packet[n] = 0xAB;
    CHECK(chat::DecodeChatMessage(g_packet, n + 1, &line) == chat::Decoded::Ok);
}

static void TestRing() {
    std::printf("the ring: 5 messages in all, the oldest goes; the reader keeps the copy it shows\n");
    static chat::ChatRing ring;
    static chat::ChatReader reader;
    CHECK(reader.Refresh(ring).count == 0);
    chat::Line line;
    for (u32 i = 0; i < 7; ++i) {
        line.t_ms = 1000 + i;
        line.text_len = static_cast<u8>(std::snprintf(line.text, sizeof(line.text), "message %u", i));
        line.nick_len = static_cast<u8>(std::snprintf(line.nick, sizeof(line.nick), "N%u", i));
        ring.Push(line);
        const chat::View& v = reader.Refresh(ring);
        CHECK(v.count == (i + 1 < chat::kLines ? i + 1 : chat::kLines));
        CHECK(v.lines[v.count - 1].t_ms == 1000 + i); // the newest last
    }
    const chat::View& v = reader.Refresh(ring);
    CHECK(v.count == 5);
    for (u32 i = 0; i < 5; ++i) {
        char want[16];
        std::snprintf(want, sizeof(want), "message %u", i + 2);
        CHECK(std::strcmp(v.lines[i].text, want) == 0);
    }
    CHECK(&reader.Refresh(ring) == &v); // nothing new: the same copy, no read
}

static void TestSeqlock() {
    std::printf("the seqlock: a writer thread and a reader thread, every copy whole and in order\n");
    static chat::ChatRing ring;
    static chat::ChatReader reader;
    constexpr u32 kWrites = 200000;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        chat::Line line;
        for (u32 i = 1; i <= kWrites; ++i) {
            // the line says its number three times: a torn copy disagrees with itself
            line.t_ms = i;
            line.nick_len = 8;
            for (int k = 0; k < 8; ++k) {
                line.nick[k] = static_cast<char>('A' + ((i >> (k * 4)) & 0xF));
            }
            line.text_len = chat::kTextMax;
            std::memset(line.text, 'a' + static_cast<int>(i % 26), chat::kTextMax);
            line.text[chat::kTextMax] = 0;
            ring.Push(line);
            if ((i & 63) == 0) {
                std::this_thread::yield(); // (windows for the reader: a writer that never pauses leaves it few whole copies)
            }
        }
        done.store(true);
    });
    u32 reads = 0, bad = 0, last_newest = 0, changes = 0;
    while (!done.load() || reads == 0) {
        const chat::View& v = reader.Refresh(ring);
        ++reads;
        if (v.count == 0) {
            continue;
        }
        const u32 newest = v.lines[v.count - 1].t_ms;
        if (newest < last_newest) {
            ++bad; // the reader went back in time
        }
        if (newest != last_newest) {
            ++changes;
        }
        last_newest = newest;
        for (u32 j = 0; j < v.count; ++j) {
            const chat::Line& l = v.lines[j];
            const u32 i = l.t_ms;
            bool ok = l.nick_len == 8 && l.text_len == chat::kTextMax && l.text[chat::kTextMax] == 0 && i == newest - (v.count - 1 - j);
            for (int k = 0; k < 8 && ok; ++k) {
                ok = l.nick[k] == static_cast<char>('A' + ((i >> (k * 4)) & 0xF));
            }
            for (u32 k = 0; k < chat::kTextMax && ok; ++k) {
                ok = l.text[k] == static_cast<char>('a' + static_cast<int>(i % 26));
            }
            bad += ok ? 0 : 1;
        }
    }
    writer.join();
    const chat::View& v = reader.Refresh(ring);
    CHECK(bad == 0);
    CHECK(v.count == 5 && v.lines[4].t_ms == kWrites);
    std::printf("  %u refreshes, %u of them saw new lines\n", reads, changes);
    CHECK(changes > 1);
}

static void TestAdmit() {
    std::printf("the rate limit: more than 4 in any sliding second are dropped before the decoding, more than 2 of one member\n");
    chat::RateLimit r;
    u32 taken = 0;
    for (u32 i = 0; i < 50; ++i) {
        taken += r.Admit(i * 20, 100 + i) ? 1 : 0; // 50 in one second, each of another member
    }
    CHECK(taken == 4);
    // three seconds at 50 a second: 4 in each
    chat::RateLimit r3;
    taken = 0;
    for (u32 i = 0; i < 150; ++i) {
        taken += r3.Admit(5000 + i * 20, 100 + i) ? 1 : 0;
    }
    CHECK(taken == 12);
    // sliding: 0, 100, 200, 300 in; 400 and 999 out; 1000 in (the one of 0 is a second old), 1050 out, 1100 in
    chat::RateLimit s;
    CHECK(s.Admit(0, 1) && s.Admit(100, 2) && s.Admit(200, 3) && s.Admit(300, 4));
    CHECK(!s.Admit(400, 5) && !s.Admit(999, 6));
    CHECK(s.Admit(1000, 7) && !s.Admit(1050, 8) && s.Admit(1100, 9));
    // the u32 clock wraps
    chat::RateLimit w;
    const u32 t0 = 0xFFFFFE00u;
    CHECK(w.Admit(t0, 1) && w.Admit(t0 + 100, 2) && w.Admit(t0 + 200, 3) && w.Admit(t0 + 300, 4));
    CHECK(!w.Admit(t0 + 700, 5)); // (0x100 after the wrap: 700 ms after the first)
    CHECK(w.Admit(t0 + 1000, 6) && w.Admit(t0 + 1100, 7));
    // one member: 2 in any sliding second, the other 2 places left to the others
    chat::RateLimit m;
    taken = 0;
    for (u32 i = 0; i < 50; ++i) {
        taken += m.Admit(i * 20, 7) ? 1 : 0;
    }
    CHECK(taken == 2);
    chat::RateLimit q;
    CHECK(q.Admit(0, 1) && q.Admit(100, 1) && !q.Admit(500, 1)); // the member's third in the second
    CHECK(q.Admit(600, 2) && q.Admit(700, 2) && !q.Admit(800, 2));
    CHECK(!q.Admit(999, 3));                                     // 4 in the second: nobody else either
    CHECK(q.Admit(1000, 1) && !q.Admit(1050, 1));                // the one of 0 is a second old; 100 and 1000 are the member's 2
    CHECK(!q.Admit(1099, 3) && q.Admit(1100, 3));                // the second place of 100 frees at 1100
    CHECK(q.Admit(1600, 2) && !q.Admit(1650, 2));                // 600 left, 700 still in
    // a flood of 20 a second from one member, 2 friends at 1 a second, a minute: every one of the friends' passes, the flood 2 a second,
    // never more than 4 in any second (the times of the taken ones kept: every window of a second checked)
    chat::RateLimit f;
    static u32 times[400];
    u32 n_taken = 0, friends = 0, flood = 0;
    for (u32 t = 0; t < 60000; t += 50) {
        if (f.Admit(t, 666)) {
            ++flood;
            times[n_taken++ % 400] = t;
        }
        if (t % 1000 == 300 && f.Admit(t + 1, 11)) {
            ++friends;
            times[n_taken++ % 400] = t + 1;
        }
        if (t % 1000 == 750 && f.Admit(t + 2, 12)) {
            ++friends;
            times[n_taken++ % 400] = t + 2;
        }
    }
    CHECK(friends == 120 && flood <= 120 && flood >= 118 && n_taken <= 400);
    u32 worst = 0;
    for (u32 i = 0; i < n_taken && n_taken <= 400; ++i) {
        u32 in = 0;
        for (u32 k = 0; k < n_taken; ++k) {
            in += (times[k] >= times[i] && times[k] - times[i] < chat::kRateWindowMs) ? 1 : 0;
        }
        worst = in > worst ? in : worst;
    }
    CHECK(worst == 4);
    std::printf("  a flood at 20 a second and 2 friends at 1: the friends' %u of 120, the flood's %u, at most %u in a second\n", friends, flood, worst);
    // the sender is read before the decoding: the raw nickname, not the username or the text; 0 when it is not in the packet
    std::size_t n = Packet("Alice", "alice", "one");
    const u32 alice = chat::SenderTag(g_packet, n);
    n = Packet("Alice", "other", "a second text, longer");
    CHECK(chat::SenderTag(g_packet, n) == alice && alice != 0);
    n = Packet("Alicf", "alice", "one");
    CHECK(chat::SenderTag(g_packet, n) != alice);
    n = Packet("Alice", "alice", "one");
    CHECK(chat::SenderTag(g_packet, 0) == 0 && chat::SenderTag(g_packet, 3) == 0 && chat::SenderTag(g_packet, 9) == 0);
    CHECK(chat::SenderTag(g_packet, 10) == alice); // (the nickname's field is whole: the rest may be cut, the decoding sees it)
    // the sink counts the drops, and keeps a friend's message through a flood
    static ChatSink sink;
    taken = 0;
    for (u32 i = 0; i < 50; ++i) {
        taken += sink.Admit(7000 + i * 20, 100 + i) ? 1 : 0;
    }
    CHECK(taken == 4 && sink.counters.rate_drops.load() == 46);
    static ChatSink flooded;
    u32 friend_lines = 0;
    for (u32 t = 0; t < 10000; t += 50) {
        n = Packet("Spammer", "spam", "buy now");
        if (flooded.Admit(t, chat::SenderTag(g_packet, n))) {
            flooded.OnPacket(g_packet, n, t);
        }
        if (t % 1000 == 500) {
            n = Packet("Friend", "friend", "are you there");
            if (flooded.Admit(t, chat::SenderTag(g_packet, n)) && flooded.OnPacket(g_packet, n, t)) {
                ++friend_lines;
            }
        }
    }
    CHECK(friend_lines == 10 && flooded.counters.taken.load() <= 30 && flooded.counters.rate_drops.load() >= 200 - 20);
}

static void TestSink() {
    std::printf("the block: packets kept, the outgoing box one a second and only while joined, our own line kept\n");
    static ChatSink sink;
    static chat::ChatReader& reader = sink.reader;
    std::size_t n = Packet("Alice", "alice", "hi \xC3\xA8 all");
    CHECK(sink.OnPacket(g_packet, n, 100));
    n = Packet("Bob", "", " \r\n");
    CHECK(!sink.OnPacket(g_packet, n, 110));
    CHECK(!sink.OnPacket(g_packet, 3, 120));
    CHECK(sink.counters.taken.load() == 1 && sink.counters.empty.load() == 1 && sink.counters.malformed.load() == 1);
    const chat::View& v = reader.Refresh(sink.ring);
    CHECK(v.count == 1 && std::strcmp(v.lines[0].nick, "Alice") == 0 && std::strcmp(v.lines[0].text, "hi ? all") == 0 && v.lines[0].t_ms == 100);

    u8 packet[chat::kPacketMax];
    CHECK(sink.TakeOutgoing(1000, true, packet) == 0); // an empty box
    CHECK(!sink.Post("   ", 3));                      // nothing left of it
    CHECK(sink.Post("first\tone", 9));
    CHECK(sink.OutboxBusy() && !sink.Post("second", 6)); // the box holds one: the keyboard keeps its draft
    // not in the room: refused, the box free again
    CHECK(sink.TakeOutgoing(1000, false, packet) == 0);
    CHECK(!sink.OutboxBusy() && sink.counters.refused.load() == 1);
    CHECK(sink.Post("first\tone", 9));
    u32 size = sink.TakeOutgoing(1000, true, packet);
    CHECK(size == 5 + 9 && packet[0] == 6 && LoadBE32(packet + 1) == 9 && std::memcmp(packet + 5, "first one", 9) == 0);
    sink.SentOutgoing("Gionko", 6, 1000, true);
    CHECK(!sink.OutboxBusy() && sink.counters.sent.load() == 1);
    const chat::View& v2 = reader.Refresh(sink.ring);
    CHECK(v2.count == 2 && v2.lines[1].own == 1 && std::strcmp(v2.lines[1].nick, "Gionko") == 0 &&
          std::strcmp(v2.lines[1].text, "first one") == 0);
    // one a second: the next waits in the box
    CHECK(sink.Post("second", 6));
    CHECK(sink.TakeOutgoing(1500, true, packet) == 0 && sink.OutboxBusy());
    CHECK(sink.TakeOutgoing(1999, true, packet) == 0);
    size = sink.TakeOutgoing(2000, true, packet);
    CHECK(size == 5 + 6);
    sink.SentOutgoing("Gionko", 6, 2000, true);
    // a long one is cut like the room's; the packet is 133 bytes at most
    static char long_text[300];
    std::memset(long_text, 'L', sizeof(long_text));
    CHECK(sink.Post(long_text, sizeof(long_text)));
    size = sink.TakeOutgoing(3000, true, packet);
    CHECK(size == chat::kPacketMax && LoadBE32(packet + 1) == 128 && std::memcmp(packet + 5 + 125, "...", 3) == 0);
    sink.SentOutgoing("Gionko", 6, 3000, false); // ENet had no memory: counted, the box free, no line
    CHECK(sink.counters.refused.load() == 2 && !sink.OutboxBusy() && reader.Refresh(sink.ring).count == 3);
    // the clock wraps between two of ours
    static ChatSink wrap;
    CHECK(wrap.Post("a", 1) && wrap.TakeOutgoing(0xFFFFFF00u, true, packet) == 6);
    wrap.SentOutgoing("N", 1, 0xFFFFFF00u, true);
    CHECK(wrap.Post("b", 1) && wrap.TakeOutgoing(0x00000100u, true, packet) == 0); // 512 ms later
    CHECK(wrap.TakeOutgoing(0x000002E8u, true, packet) == 6);                     // 1000 ms later
}

// Who came and who left, a line of its own (2026-11-08, asked by the testers): no nickname (drawn like a notice), the nickname
// cleaned like the room's, never longer than a line.
static void TestPresence() {
    std::printf("presence: '<nick> joined the room' / '<nick> left the room', a line with no nickname\n");
    static ChatSink sink;
    sink.OnPresence("Friend-A", 8, true, 10);
    sink.OnPresence("Friend-PC", 9, false, 20);
    sink.OnPresence(" \r\n", 3, true, 30); // (nothing left of the nickname: no line)
    const chat::View& v = sink.reader.Refresh(sink.ring);
    CHECK(v.count == 2 && sink.counters.presence.load() == 2);
    CHECK(v.lines[0].nick_len == 0 && std::strcmp(v.lines[0].text, "Friend-A joined the room") == 0 && v.lines[0].own == 0);
    CHECK(v.lines[0].text_len == std::strlen("Friend-A joined the room") && v.lines[0].t_ms == 10);
    CHECK(v.lines[1].nick_len == 0 && std::strcmp(v.lines[1].text, "Friend-PC left the room") == 0);
    // a nickname past the room's 20 characters is cut to 20 (no "..."), escapes and UTF-8 cleaned like a message's
    static ChatSink cut;
    cut.OnPresence("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26, true, 1);
    cut.OnPresence("a\x1b[31mb\xC3\xA8", 9, false, 2);
    const chat::View& c = cut.reader.Refresh(cut.ring);
    CHECK(c.count == 2 && std::strcmp(c.lines[0].text, "ABCDEFGHIJKLMNOPQRST joined the room") == 0);
    CHECK(std::strcmp(c.lines[1].text, "a b? left the room") == 0);
}

int main() {
    // (each case once before the count: the first call of printf and of the C++ runtime allocate their own buffers)
    std::printf("chat_core_test\n");
    TestSeqlock(); // threads allocate: outside the count
    TestScanBound(); // (its buffers are vectors)
    const unsigned before = g_allocs.load();
    TestSanitize();
    TestTruncation();
    TestDecode();
    TestRing();
    TestAdmit();
    TestSink();
    TestPresence();
    // CHECK_STR builds std::strings on the test's side: the count is taken by the cases that do not use it as well
    const unsigned with_test_strings = g_allocs.load() - before;
    std::printf("the chat allocates nothing: the decoding, the ring, the rate limit and the box\n");
    const unsigned quiet_before = g_allocs.load();
    {
        static ChatSink sink;
        std::size_t n = 0;
        for (u32 i = 0; i < 100; ++i) {
            n = Packet((i & 1) != 0 ? "Carol" : "Alice", "alice", "hello"); // (two members: 2 of each)
            if (sink.Admit(i * 10, chat::SenderTag(g_packet, n))) {
                sink.OnPacket(g_packet, n, i * 10);
            }
        }
        static char big[5000];
        std::memset(big, 'q', sizeof(big));
        n = Packet("Bob", 3, "", 0, big, sizeof(big));
        sink.Admit(5000, chat::SenderTag(g_packet, n));
        sink.OnPacket(g_packet, n, 5000);
        sink.OnPacket(g_packet, 7, 5001);
        u8 packet[chat::kPacketMax];
        sink.Post(big, sizeof(big));
        const u32 size = sink.TakeOutgoing(6000, true, packet);
        sink.SentOutgoing("Gionko", 6, 6000, size != 0);
        const chat::View& v = sink.reader.Refresh(sink.ring);
        CHECK(v.count == 5 && v.lines[3].text_len == 128 && v.lines[4].own == 1);
    }
    CHECK(g_allocs.load() == quiet_before);
    std::printf("  (the cases with the test's own strings allocated %u times, all of them the test's)\n", with_test_strings);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
