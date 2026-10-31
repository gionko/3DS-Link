// Emulation of the "dlp:FKCL" service for the one thing some games ask of it: whether the wireless is on.
//
// Kirby Battle Royale starts its local multiplayer
// with  dlp:FKCL InitializeWithName  followed 1 ms later by  Finalize ; the system's answer decides whether the game
// goes on to nwm::UDS (the wireless mode, with each player's own copy) or shows "wireless communication must be
// turned on". The real module cannot start while the console stays on its access point for the tunnel (ndm:u is
// faked for that), and answers 0xC940AC04 (ResultWirelessOff). Nothing else of Download Play is used in that mode
// (a game of two Azahar instances, whose DLP answers success, shows exactly these two calls and then UDS only).
//
// So: Initialize, InitializeWithName and Finalize succeed, every other command of the service is refused with
// "invalid state" (the real module's answer to a command out of its state) and counted. The Download Play proper
// (scan, join, host) is not carried by the tunnel.
//
// Kirby Triple Deluxe and Kirby Planet Robobot are NOT served by this: after Initialize their wireless mode asks GetChannels and goes on to scan and join, and the
// answer "invalid state" ends in the game's own error screen (0xE0A0AC01, seen on 2026-10-28). They need the Download Play client carried by the room.
#pragma once
#include "azl/common.hpp"

namespace azl {

class DlpFkclHle {
public:
    static constexpr u32 kInitialize = 0x01;         // [0x00010083]
    static constexpr u32 kFinalize = 0x02;           // [0x00020000]
    static constexpr u32 kInitializeWithName = 0x11; // [0x00110203]
    static constexpr u32 kLastCommand = 0x11;
    // Result codes (module 43, DLP): Usage / InvalidState / description 1, and the dispatcher's own answer for an id the
    // service does not have.
    static constexpr u32 kResInvalidState = 0xE0A0AC01;
    static constexpr u32 kResBadHeader = 0xD9001830;

    /// Handles one request; the reply replaces the request words. No locking: the caller serialises the calls.
    void Dispatch(u32* cmd) {
        const u32 id = cmd[0] >> 16;
        if (id == kInitialize || id == kInitializeWithName) {
            ++initializes_;
            Reply(cmd, id, 0);
        } else if (id == kFinalize) {
            ++finalizes_;
            Reply(cmd, id, 0);
        } else if (id >= 1 && id <= kLastCommand) {
            ++refused_;
            Reply(cmd, id, kResInvalidState);
        } else {
            ++refused_;
            Reply(cmd, 0, kResBadHeader);
        }
    }

    u32 initializes() const {
        return initializes_;
    }
    u32 finalizes() const {
        return finalizes_;
    }
    /// Commands answered with an error: the game asked for more than the wireless check.
    u32 refused() const {
        return refused_;
    }

private:
    static void Reply(u32* cmd, u32 id, u32 result) {
        cmd[0] = (id << 16) | (1u << 6); // one normal word, no translate words
        cmd[1] = result;
    }
    u32 initializes_ = 0;
    u32 finalizes_ = 0;
    u32 refused_ = 0;
};

} // namespace azl
