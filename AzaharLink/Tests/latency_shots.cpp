// Pictures of the top screen with the chat open and the latency of the other players, drawn by the plugin's own code (azl/chat_host.hpp,
// azl/chat_draw.hpp, azl/chat_latency.hpp) on a fake framebuffer: for looking at, not a test (Tests/run.sh does not build it).
//   g++ -std=c++17 -O1 -IIncludes Tests/latency_shots.cpp -o latency_shots && ./latency_shots <folder>     (writes <folder>/*.bmp, 400 x 240)
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "azl/chat_host.hpp"
#include "azl/chat_latency.hpp"

using namespace azl;

namespace azl {
namespace chat {
Signals g_signals;
}
} // namespace azl

static bool g_bright = false; // a bright scene behind (clouds, snow, a white room): where a text without a background is hardest to read

struct Fb {
    static constexpr std::size_t kGuard = 64;
    std::vector<u8> mem;
    gfx::Surface s;
    Fb() {
        const u32 bpp = 3, rows = 240, w = 400;
        mem.assign(kGuard * 2 + static_cast<std::size_t>(w) * rows * bpp, 0);
        s.left = mem.data() + kGuard;
        s.stride = rows * bpp;
        s.bpp = static_cast<u8>(bpp);
        s.format = static_cast<u8>(gfx::kBGR8);
        s.w = 400;
        s.h = 240;
    }
    void Put(int x, int y, u8 r, u8 g, u8 b) {
        u8* p = s.left + static_cast<std::size_t>(x) * s.stride + static_cast<std::size_t>(239 - y) * 3;
        p[0] = b;
        p[1] = g;
        p[2] = r;
    }
    void Get(int x, int y, u8* r, u8* g, u8* b) const {
        const u8* p = s.left + static_cast<std::size_t>(x) * s.stride + static_cast<std::size_t>(239 - y) * 3;
        *b = p[0];
        *g = p[1];
        *r = p[2];
    }
    // "a game": a sky, a field with a grid of blocks, like a puzzle game's well
    void Paint() {
        for (int y = 0; y < 240; ++y) {
            for (int x = 0; x < 400; ++x) {
                u8 r = static_cast<u8>(30 + y / 4), g = static_cast<u8>(70 + y / 3), b = static_cast<u8>(150 + y / 5);
                if (g_bright) {
                    r = static_cast<u8>(215 + y / 12);
                    g = static_cast<u8>(225 + y / 20);
                    b = static_cast<u8>(235 + y / 40);
                    if (((x / 24) + (y / 24)) % 2 == 0) {
                        r = static_cast<u8>(r - 25);
                        g = static_cast<u8>(g - 20);
                    }
                }
                if (x > 150 && x < 250 && y > 20 && y < 230) {
                    const bool cell = ((x - 150) / 10 + (y - 20) / 10) % 2 == 0;
                    r = cell ? 40 : 55;
                    g = cell ? 40 : 55;
                    b = cell ? 60 : 75;
                    if (y > 150 && ((x - 150) / 10) % 3 != 1) {
                        r = static_cast<u8>(200 - ((x - 150) / 10) * 12);
                        g = static_cast<u8>(90 + ((y - 150) / 10) * 12);
                        b = 80;
                    }
                }
                Put(x, y, r, g, b);
            }
        }
    }
    void WriteBmp(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
            return;
        }
        const unsigned row = 400 * 3, size = 54 + row * 240;
        u8 head[54] = {'B', 'M'};
        const auto put32 = [&](int at, unsigned v) {
            for (int i = 0; i < 4; ++i) head[at + i] = static_cast<u8>(v >> (8 * i));
        };
        put32(2, size);
        put32(10, 54);
        put32(14, 40);
        put32(18, 400);
        put32(22, 240);
        head[26] = 1;
        head[28] = 24;
        put32(34, row * 240);
        std::fwrite(head, 1, sizeof(head), f);
        std::vector<u8> line(row);
        for (int y = 239; y >= 0; --y) {
            for (int x = 0; x < 400; ++x) {
                u8 r, g, b;
                Get(x, y, &r, &g, &b);
                line[x * 3] = b;
                line[x * 3 + 1] = g;
                line[x * 3 + 2] = r;
            }
            std::fwrite(line.data(), 1, row, f);
        }
        std::fclose(f);
    }
};

static std::size_t PacketOf(u8* out, const char* nick, const char* msg) {
    std::size_t p = 0;
    out[p++] = chat::kIdChatMessage;
    const char* fields[3] = {nick, "user", msg};
    for (const char* f : fields) {
        const u32 n = static_cast<u32>(std::strlen(f));
        StoreBE32(out + p, n);
        std::memcpy(out + p + 4, f, n);
        p += 4 + n;
    }
    return p;
}

struct Player {
    int node;
    int rtt;
    const char* name;
};

// The node of the player who writes under a nick (the plugin finds it through the room: nickname -> MAC -> node); here from the list of the picture.
static const Player* g_players = nullptr;
static int g_player_count = 0;
static u8 NodeOfShot(const char* nick, u32 len) {
    for (int i = 0; i < g_player_count; ++i) {
        if (std::strlen(g_players[i].name) == len && std::memcmp(g_players[i].name, nick, len) == 0) return static_cast<u8>(g_players[i].node);
    }
    return 0;
}

// One picture: the chat in `corner` with five messages of THREE lines (the most a message takes), the keyboard open (the latency shows only then), the latency of `players` (the
// others of the lobby; node 1 is the host) and a notification of the plugin (the opaque black strip) on top. `chat_players` = who can write, `speak` = the five that write.
static void Shot(const std::string& dir, const char* file, int corner, const Player* players, int n, const Player* chat_players, int chat_n, const int* speak,
                 const char* notice) {
    std::unique_ptr<ChatSink> sink_box(new ChatSink()); // (a new chat for every picture: the messages of the one before are not in it)
    ChatSink& sink = *sink_box;
    g_players = chat_players;
    g_player_count = chat_n;
    sink.SetNodeSource(&NodeOfShot);
    u8 packet[256];
    const u32 now = 500000;
    static const char* const long_text[5] = {
        "i just found a much better route through the second level, if we all go left at the bridge and wait for the others to catch up we can skip the fight",
        "that is a very long message from someone who really likes to type a lot, and then a bit more so it fills the lines of the chat completely, you see?",
        "okay, everybody listen: the plan is simple, we meet at the fountain, then we split in two groups, one takes the bridge and the other one takes the tunnel",
        "let's start the match now, i found a good spot near the bridge and the boss is already waiting for us there so we should hurry up before it moves away",
        "hold on a second, my battery is at ten percent and i have to find the charger, it is somewhere in the other room under the pile of games and cables"};
    u32 t = 1000;
    for (int i = 0; i < 5; ++i) {
        sink.OnPacket(packet, PacketOf(packet, chat_players[speak[i]].name, long_text[i]), t += 2000);
    }

    latency::NodeRtt nr[8];
    char names[9][chat::kNickMax + 1] = {};
    bool has[9] = {};
    for (int i = 0; i < 8; ++i) nr[i].node = i + 1;
    for (int k = 0; k < n; ++k) {
        latency::NodeRtt& o = nr[players[k].node - 1];
        o.age_ms = 5;
        o.rtt_ms = players[k].rtt;
        has[players[k].node] = true;
        std::strncpy(names[players[k].node], players[k].name, chat::kNickMax);
    }
    chat::Line rows[chat::kLines];
    const u32 count = chat::BuildLatencyRows(nr, names, has, now, rows);

    chat::Overlay o;
    o.sink.store(&sink);
    o.on.store(1);
    o.corner.store(static_cast<u32>(corner));
    o.keyboard.store(1); // the keyboard is open: the user is writing
    o.lat.Publish(rows, count);
    if (notice != nullptr) {
        o.board.Show(notice, now);
    }
    Fb fb;
    fb.Paint();
    chat::DrawOverlay(o, fb.s, true, now);
    fb.WriteBmp(dir + "/" + file + ".bmp");
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    g_bright = argc > 2 && std::strcmp(argv[2], "bright") == 0;
    const char* sd = "AzaharLink: SD card 98% full";
    // a lobby of 4: me (node 3) and three others (node 1 is the host), one with a long nickname
    const Player four[] = {{1, 28, "Ana"}, {2, 64, "Carlo Magno 2"}, {4, 120, "Bob"}, {3, 0, "Tu"}};
    const int say4[5] = {1, 0, 3, 2, 1}; // Carlo Magno 2, Ana, us, Bob, Carlo Magno 2
    // a lobby of 8: me (node 3) and seven others, "Carlo Magno 2" among them
    const Player eight[] = {{1, 22, "Ana"}, {2, 45, "Carlo Magno 2"}, {4, 80, "Bob"}, {5, 120, "Dan"}, {6, 200, "Eva"}, {7, -1, "Fabio"}, {8, 12, "Gina"}, {3, 0, "Tu"}};
    const int say8[5] = {1, 3, 7, 4, 1}; // Carlo Magno 2, Dan, us, Eva, Carlo Magno 2
    const char* where[4] = {"in_alto_a_sinistra", "in_alto_a_destra", "in_basso_a_sinistra", "in_basso_a_destra"};
    for (int corner = 0; corner < 4; ++corner) {
        std::string a = std::string("L4_chat_") + where[corner];
        std::string b = std::string("L8_chat_") + where[corner];
        Shot(dir, a.c_str(), corner, four, 3, four, 4, say4, sd);
        Shot(dir, b.c_str(), corner, eight, 7, eight, 8, say8, sd);
    }
    return 0;
}