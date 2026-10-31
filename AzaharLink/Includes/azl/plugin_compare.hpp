// Is the plugin that runs in this game the one 3DS Link keeps in /3ds/3DSLink/? (the app puts its plugin there and copies it into the folder of every enabled game.)
//
// The plugin looks once, a good while after the game has started, at the two files: its own copy (sd:/luma/plugins/<title id>/AzaharLink.3gx, which Luma loaded) and the plugin of
// 3DS Link's folder (sd:/3ds/3DSLink/AzaharLink.3gx). When the second is not the first, the plugin says on the screen that 3DS Link has to be opened to update the games.
//
// What this finds, and what it does not: it finds a game whose copy was left behind (the app was updated and its pass over the games was cut short, or this game was enabled before),
// and a plugin file that was put in 3DS Link's folder by hand. It does NOT find a CIA that was installed and never opened: the file in /3ds/3DSLink/ is only written by the app, when it runs, so until
// then both files are the old plugin and they are the same.
//
// The pure part is here (the decisions, tested on a PC: Tests/plugin_compare_test.cpp); the reading of the two files is in plugin.cpp.
#pragma once
#include "azl/common.hpp"

namespace azl {

/// The version word of a .3gx header: bytes 8..11, little endian = major << 24 | minor << 16 | revision << 8. 0 when `head` (the first 12 bytes of a file) does not begin with the "3GX$" magic.
inline u32 PluginHeaderVersionWord(const u8* head, u32 size) {
    if (!head || size < 12 || head[0] != '3' || head[1] != 'G' || head[2] != 'X' || head[3] != '$') {
        return 0;
    }
    return static_cast<u32>(head[8]) | static_cast<u32>(head[9]) << 8 | static_cast<u32>(head[10]) << 16 | static_cast<u32>(head[11]) << 24;
}

/// "1.0.8" -> 0x01000800 (the same word). 0 when the text is not X.Y.Z with three numbers below 256 and nothing after them.
inline u32 PluginVersionWordFromText(const char* text) {
    if (!text) {
        return 0;
    }
    u32 parts[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (*text < '0' || *text > '9') {
            return 0;
        }
        u32 value = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + static_cast<u32>(*text - '0');
            if (value > 255) {
                return 0;
            }
            ++text;
        }
        parts[i] = value;
        if (i < 2) {
            if (*text != '.') {
                return 0;
            }
            ++text;
        }
    }
    if (*text != '\0') {
        return 0;
    }
    return parts[0] << 24 | parts[1] << 16 | parts[2] << 8;
}

/// How the plugin of 3DS Link's folder (the master) stands to the one that runs.
enum class MasterPlugin : u8 {
    Same,       ///< the same bytes: nothing to say
    Newer,      ///< a later version: 3DS Link was updated and this game was not
    OtherBuild, ///< the same version and other bytes (another build of it)
    Older,      ///< an earlier version: this copy is a newer one put by hand; nothing to say
    Unreadable  ///< the file is not there or is not a plugin: nothing to say
};

/// `own` and `master` are version words (0 = unknown); `bytes_equal` only matters when the two versions are the same.
inline MasterPlugin RelateMaster(u32 own, u32 master, bool bytes_equal) {
    if (master == 0 || own == 0) {
        return MasterPlugin::Unreadable;
    }
    if (master > own) {
        return MasterPlugin::Newer;
    }
    if (master < own) {
        return MasterPlugin::Older;
    }
    return bytes_equal ? MasterPlugin::Same : MasterPlugin::OtherBuild;
}

/// Does this relation call for the warning "open 3DS Link to update your games"?
inline bool MasterPluginWarns(MasterPlugin relation) {
    return relation == MasterPlugin::Newer || relation == MasterPlugin::OtherBuild;
}

} // namespace azl
