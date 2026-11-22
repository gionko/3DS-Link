// The prepared titles out of the delivery module's catalog (1.1.4, piece APP-STORE: 6-APP-boot9.md 5 and its critic 4.6). titles.cpp calls it on the
// catalog catalog_build made (ts_catalog: its groups hold indexes into its items), between catalog_build and UseAppNames: a title of ours is no
// longer a member of any group, and a group left with no member is no longer a group, so Titles Delivery never lists, sends or names it. The items
// stay where they are (the module's indexes into them stay right). A template on the shape of ts_game, so a PC tries it on a made-up catalog
// (Tests/settings_test/dlp_guest_store_test.cpp) without the module's C headers.
#pragma once

#include <cstdint>

namespace dlpguest {

/// Takes out of `games[0..*n_games)` every member `is_ours(tid_of(member))` says, then every group left empty, keeping the order of the rest. Returns
/// how many members went.
template <class Group, class TidOf, class IsOurs>
int DropOwnedMembers(Group* games, int* n_games, TidOf tid_of, IsOurs is_ours) {
    int dropped = 0;
    int kept_groups = 0;
    for (int g = 0; g < *n_games; ++g) {
        Group& group = games[g];
        int kept = 0;
        for (int k = 0; k < group.nMembers; ++k) {
            if (is_ours(tid_of(group.members[k]))) {
                ++dropped;
            } else {
                group.members[kept++] = group.members[k];
            }
        }
        group.nMembers = kept;
        if (kept > 0) {
            if (kept_groups != g) {
                games[kept_groups] = group;
            }
            ++kept_groups;
        }
    }
    *n_games = kept_groups;
    return dropped;
}

} // namespace dlpguest
