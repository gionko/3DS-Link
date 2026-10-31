// The adapter of the fake Download Play lobby to the UDS engine: LobbyNet (azl/dlp_lobby.hpp) realised on a UdsEngine. The
// lobby is a UDS network of its own - the Download Play's wlancommid 0x00002810 and id8 0x55, NEVER the game's ids, or the
// beacon would be listed by the game's own scan - that the creator's plugin hosts and the searcher's joins. Its beacon carries the
// announcement (azl/dlp_types.hpp, 29 bytes) as application data; admission, node ids, names and goodbyes are the engine's, the four
// messages of the creator's phases are frames on a data channel of their own (UdsEngine::kLobbyChannel) that the engine keeps in a
// ring and never gives to a bind. Design: analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE2.md, DESIGN-FASE1.md sez. 2 and 14.
//
// ONE ENGINE, TWO USERS. The engine is the game's nwm::UDS as well, and has one state. The lobby takes it only when it is free
// (UdsEngine::InitializeForLobby; a game's Initialize before ours = HostOpen / ConnectBegin answer false, ScanOpen too) and gives
// it back whole when the lobby ends (HostClose / Disconnect = Shutdown), so that the game's own Initialize finds a clean service and
// not the lobby's node, name and seed. The caller must run DlpLobby::OnGameUdsStarting before the game's Initialize reaches the
// engine (phase 3: UdsHle::Dispatch, first thing in cases 0x01 / 0x1B).
//
// THREADS. As the lobby: nothing here is thread-safe, the caller serialises every call with ONE mutex; the engine takes its own
// lock inside each call (never held across two). Nothing blocks. No allocation, no log, no state but three words.
#pragma once

#include "azl/dlp_lobby.hpp"
#include "azl/uds_engine.hpp"

namespace azl {

// >>> game-thread code (Tests/policy_check.sh: no printf family, no Log()/DebugEvent() with a format, no big buffer)

class EngineLobbyNet final : public LobbyNet {
public:
    /// A lobby is listed while its last beacon is younger than this (a host beacons every 102 ms; the room does not always carry
    /// all of them: the lobby's own table keeps an entry 1500 ms after the last time it was listed).
    static constexpr u32 kHeardFreshMs = 400;
    /// UDSNETATTR_DISABLE_CONNECT: the host admits nobody while it is set.
    static constexpr u16 kBlockClients = 0x2;
    static_assert(kAnnounceSize <= UdsEngine::kHeardDataMax, "an announcement fits in what the engine copies out of a beacon");
    static_assert(kMaxGroups <= UdsEngine::kHeardLobbies, "the engine remembers as many lobbies as the searcher keeps");
    static_assert(kMaxMessage <= UdsEngine::kLobbyMessageMax, "a message fits in what the engine's ring keeps");

    /// `engine` outlives the adapter. Cost: two pointers and a flag.
    explicit EngineLobbyNet(UdsEngine* engine) : engine_(engine) {}
    EngineLobbyNet(const EngineLobbyNet&) = delete;
    EngineLobbyNet& operator=(const EngineLobbyNet&) = delete;

    /// True while the lobby holds the engine (it initialised it and has not given it back).
    bool engine_held() const {
        return held_;
    }

    u64 NowMs() override;
    u64 NowTicks() override;
    // creator
    bool HostOpen(const HostAnnounce& a, u8 channel, const Identity& me) override;
    void HostUpdate(const HostAnnounce& a) override;
    void HostClose() override;
    void Kick(u8 node) override;
    // searcher
    bool ScanOpen() override;
    void ScanClose() override;
    bool ScanRead(u32 index, HeardGroup* out) override;
    bool ConnectBegin(const Mac& host, const Identity& me) override;
    int ConnectPoll() override;
    void Disconnect() override;
    // both
    u8 MyNode() override;
    u32 Members(u8 (&nodes)[kMaxPlayers]) override;
    bool ReadNode(u8 node, NodeEntry* out) override;
    bool Send(u8 to_node, const u8* data, u32 size) override;
    bool Receive(u8* from_node, u8* data, u32 capacity, u32* size) override;

private:
    /// Takes the engine if nobody has it (our identity becomes the node's); false = a game's session is up. Held already: true.
    bool Acquire(const Identity& me);
    /// Gives it back: the inbox off, the service shut down (it ends the network we are in, with the Deauthentications).
    void Release();

    UdsEngine* engine_;
    bool held_ = false;
};

// <<< game-thread code

} // namespace azl
