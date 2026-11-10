#!/bin/bash
# The defaults and the switches that decide what the plugin does to a game on a console nobody can test here, read
# from the sources: flipping one by accident (or adding a knob) fails this check. It is a plain text scan, so it
# also runs where the plugin cannot be built. Called by Tests/run.sh.
cd "$(dirname "$0")/.."
FAILED=0
# a clone that was never built (the build needs items of your own console, see the README) has none of the build products: their checks are skipped
BUILT=1
[ -f dist/3DSLink.3dsx ] && [ -f ConfigApp/data/AzaharLink.3gx ] && [ -f ConfigApp/data/child_model.bin ] || BUILT=0
[ "$BUILT" = 1 ] || echo "NOTE: this tree has not been built: the checks of the build products (dist/, ConfigApp/data/) are skipped"
fail() {
  echo "policy check FAILED: $1"
  FAILED=1
}
# $1 = file, $2 = what must be in it (a fixed string), $3 = why
need() {
  grep -F -q -- "$2" "$1" || fail "$1 no longer has '$2' ($3)"
}
# $1 = file, $2 = a whole line that must be in it (no more, no less than the text), $3 = why
need_line() {
  tr -d '\r' < "$1" | grep -F -x -q -- "$2" || fail "$1 no longer has the line '$2' ($3)"
}
# $1 = file, $2 = a line that starts a function, $3 = what must follow within the next 3 lines, $4 = why
need_after() {
  tr -d '\r' < "$1" | grep -F -A3 -- "$2" | grep -F -q -- "$3" || fail "$1: '$2' is no longer followed by '$3' ($4)"
}
# $1 = file, $2 = a fixed string, $3 = another that must come LATER in the file (the first line that has $2 is above the first line that has $3), $4 = why
need_order() {
  local a b
  a=$(tr -d '\r' < "$1" | grep -F -n -m1 -- "$2" | cut -d: -f1)
  b=$(tr -d '\r' < "$1" | grep -F -n -m1 -- "$3" | cut -d: -f1)
  { [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; } || fail "$1: '$2' does not come before '$3' ($4)"
}
# $1 = file, $2 = a statement that must open a line after its indentation (a commented-out copy, '// x;', does not count), $3 = why
need_stmt() {
  tr -d '\r' < "$1" | awk -v s="$2" '{ sub(/^[ \t]+/, ""); if (index($0, s) == 1) found = 1 } END { exit !found }' || fail "$1 no longer has the statement '$2' ($3)"
}

# The ini defaults: the sleep action is off until the lid test, the receive ring is the 64 KB the soc service grants, and
# these two live in ONE place (Includes/azl/ini_values.hpp) that the plugin's Config and the Setup app's Settings both take
# their defaults from
need Includes/azl/ini_values.hpp "constexpr int kSleepStopDefault = 0;" "the sleep action is off until the lid test (0 off, 1 a client, 2 a host too)"
need Includes/azl/ini_values.hpp "constexpr int kRcvbufKbDefault = 64;" "the receive ring is the 64 KB the soc service grants"
need Includes/ctr/plugin.hpp "int sleep_stop = ini::kSleepStopDefault;" "the plugin takes its default from the shared header"
need Includes/ctr/plugin.hpp "int rcvbuf_kb = ini::kRcvbufKbDefault;" "the plugin takes its default from the shared header"
need ConfigApp/source/settings.hpp "int sleep_stop = azl::ini::kSleepStopDefault;" "the Setup app takes its default from the shared header"
need ConfigApp/source/settings.hpp "int rcvbuf_kb = azl::ini::kRcvbufKbDefault;" "the Setup app takes its default from the shared header"
need Includes/ctr/plugin.hpp "int debug = 0;" "the debug capture is off by default"
need Includes/ctr/plugin.hpp "bool enabled = true;" "the plugin is on by default"
# ... and the README says the same default as the code (a default that is flipped must be flipped in the table too)
SLEEP_DEFAULT=$(grep -o 'kSleepStopDefault = [0-9]*;' Includes/azl/ini_values.hpp | sed 's/.*= //; s/;//')
tr -d '\r' < README.md | grep -q "^| \`sleep_stop\` | $SLEEP_DEFAULT |" || fail "README.md does not give $SLEEP_DEFAULT as the default of sleep_stop"

# No knob beyond these (a new ini key is a decision, not a side effect). They are 18: the 13 of 0.1.14 plus
# rcvbuf_kb and sleep_stop, chat and chat_key (1.1.1: the switch and the chord of the chat), and chat_pos (the corner of the chat's
# lines on the top screen, which came with the chat itself).
# menu_key and debug are old keys: 0.1.15 only gave them values (menu_key=off, debug 0 or 1).
KEYS=$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -o 'key == "[a-z_]*"' | sed 's/key == "//; s/"//' | sort | tr '\n' ' ')
WANTED="autoconnect chat chat_key chat_pos debug enabled game_name host menu_key net_thread_core nickname password port radio_like rcvbuf_kb sleep_stop socket_buffers tunnel "
[ "$KEYS" = "$WANTED" ] || fail "the ini keys read by the plugin are '$KEYS', expected '$WANTED'"

# Both programs write all 18 keys, in one place: they fill every field of ini::Fields and call ini::Render, which writes the 18
# lines (a writer that forgets a key, or a third writer with its own list, fails here). Before 0.1.16 the plugin left out
# rcvbuf_kb and sleep_stop unless they were set and the app did not know them: a file made by either had no line for them and a
# hand edit needed the key names.
FIELDS="autoconnect chat chat_key chat_pos debug enabled game_name host menu_key net_thread_core nickname password port radio_like rcvbuf_kb sleep_stop socket_buffers tunnel "
for f in Sources/ctr/plugin.cpp ConfigApp/source/settings.cpp; do
  GOT=$(tr -d '\r' < "$f" | grep -o 'fields\.[a-z_]* = ' | sed 's/fields\.//; s/ = //' | sort -u | tr '\n' ' ')
  [ "$GOT" = "$FIELDS" ] || fail "$f fills the ini fields '$GOT', expected '$FIELDS'"
  tr -d '\r' < "$f" | grep -q 'ini::Render(fields' || fail "$f does not write the ini through ini::Render"
done
RENDERED=$(tr -d '\r' < Includes/azl/ini_values.hpp | sed -n '/^inline std::string Render/,/^}/p' | grep -o '"[a-z_]*=' | tr -d '="' | sort | tr '\n' ' ')
[ "$RENDERED" = "$WANTED" ] || fail "ini::Render writes '$RENDERED', expected '$WANTED'"
! tr -d '\r' < Sources/ctr/plugin.cpp | grep -q 'WriteLine' || fail "plugin.cpp writes a line of the ini by itself (it must go through ini::Render)"
# The switches (enabled, tunnel, autoconnect, socket_buffers, chat) are read by the shared ParseBool in both programs: 1/on/true/yes
# and 0/off/false/no, and a slip is the key's default, never "on". radio_like has three values (0, 1, 2: ini::ParseRadioLike,
# which takes the words of a switch for 0 and 1) since the performance round of 2026-10-19.
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c 'ini::ParseBool(value, true)')" = "5" ] || fail "the plugin reads a switch of the ini without ini::ParseBool"
[ "$(tr -d '\r' < ConfigApp/source/settings.cpp | grep -c 'azl::ini::ParseBool(value, true)')" = "5" ] || fail "the Setup app reads a switch of the ini without ini::ParseBool"
need Sources/ctr/plugin.cpp "config_.radio_like = ini::ParseRadioLike(value);" "the plugin reads radio_like's three values with the shared parser"
need ConfigApp/source/settings.cpp "cfg.radio_like = azl::ini::ParseRadioLike(value);" "the Setup app reads radio_like's three values with the shared parser"
need Includes/azl/ini_values.hpp 'if (Word(value) == "2") {' "radio_like=2 is the third value"
need Includes/azl/ini_values.hpp "return known ? (on ? 1 : 0) : 1;" "the words of a switch mean 0 and 1 for radio_like, a slip is 1"
# (radio_like and sleep_stop are no rows of Settings since 1.1.1, the user's choice: the app carries their values, a new file has the defaults)
need ConfigApp/source/settings.cpp "    fields.radio_like = cfg.radio_like;" "the app writes radio_like back as it read it (no row, never lost)"
if tr -d '\r' < Sources/ctr/plugin.cpp | grep -n 'value != "0"'; then
  fail "the plugin reads a switch of the ini as 'anything but 0 is on' again (ini::ParseBool)"
fi
if tr -d '\r' < ConfigApp/source/settings.cpp | grep -n 'value != "0"'; then
  fail "the Setup app reads a switch of the ini as 'anything but 0 is on' again (ini::ParseBool)"
fi
# The 3DS Link app (the Setup app) has a row for every key that is not an advanced one (socket_buffers, net_thread_core, game_name,
# rcvbuf_kb, and since 1.1.1 radio_like and sleep_stop, are in the file, never lost, but are not rows; the menu chord has its row Menu keys,
# the chat its three rows: In-game chat, Chat keys, Chat position). The main page
# has the 3DS Link switch (enabled + tunnel); the room (host, port, and its own password) is chosen on the Azahar Emulator Room page; the nickname
# and the other switches are on the Settings page.
for field in enabled tunnel; do
  grep -E -q "g\.cfg\.$field([^a-z_]|$)" ConfigApp/source/pages_main.cpp || fail "the main page of the Setup app has no row for $field"
done
for field in host port password; do
  grep -E -q "g\.cfg\.$field = " ConfigApp/source/pages_servers.cpp || fail "the Setup app has no page that sets $field (a room brings its own)"
done
for field in nickname autoconnect debug menu_chord chat chat_key chat_pos; do
  grep -E -q "g\.cfg\.$field([^a-z_]|$)" ConfigApp/source/pages_config.cpp || fail "the Settings page of the Setup app has no row for $field"
done
need ConfigApp/source/pages_servers.cpp 'pass.label = "Password";' "the form of a room has its password row (each room keeps its own)"
need ConfigApp/source/settings.hpp "std::string password;" "a saved room has a password"
need ConfigApp/source/settings.cpp "    fields.sleep_stop = cfg.sleep_stop;" "the app writes sleep_stop back as it read it (no row, never lost)"
need Includes/azl/ini_values.hpp 'constexpr const char* kDefaultChatKey = "L+Left+Select";' "the chat's chord is L + Left + Select, the mirror of the menu's"
need ConfigApp/source/pages_config.cpp "if (ChordsClash(chord, g.cfg.chat_key)) {" "the menu's keys cannot be the chat's"
# ... nor hold them or be held by them (review of the chat, defect 47: the app refused only the same chord, so a menu on L+Select took the
# chat's L+Left+Select, the menu looked at first; and the plugin left Select out of chat_key, so chat_key=L+R took the game's own keys)
need ConfigApp/source/pages_config.cpp "if (ChordsClash(chord, setup::MenuOn(g.cfg) ? g.cfg.menu_key : g.cfg.menu_chord)) {" "the chat's keys cannot be the menu's"
need_after ConfigApp/source/pages_config.cpp "bool ChordsClash(const std::string& a, const std::string& b) {" "    return azl::ini::ChordsNested(a, b);" "the app refuses a chord that holds the other or is held by it"
need_after Includes/azl/ini_values.hpp "inline bool ChordsNested(const std::string& a, const std::string& b) {" "    return (x & y) == x || (x & y) == y;" "two chords clash when one holds every key of the other"
need_after Includes/azl/ini_values.hpp "inline unsigned ChordBits(const std::string& chord) {" "    unsigned bits = 1u; // (Select)" "the app counts Select in every chord, as the plugin adds it"
need_line Sources/main.cpp "    g_chat_keys = ParseKeys(cfg.chat_key) | Key::Select;" "Select is always in the chat's chord, as in the menu's"
need_line Sources/main.cpp "    const auto clash = [](u32 a, u32 b) { return !g_menu_off && ((a & b) == a || (a & b) == b); };" "the plugin finds the chords that hold one another (only with the menu on)"
need_after Sources/main.cpp "    if (g_chat_keys == Key::Select || clash(g_chat_keys, g_menu_keys)) {" "        g_chat_keys = ParseKeys(azl::ini::kDefaultChatKey) | Key::Select;" "a chat chord of Select alone or that clashes takes the default"
need_after Sources/main.cpp "    if (clash(g_chat_keys, g_menu_keys)) {" "        g_menu_keys = ParseKeys(azl::ini::kDefaultMenuKey) | Key::Select;" "a menu chord that still clashes takes its default"
need_order Sources/main.cpp "    g_menu_keys = ParseKeys(cfg.menu_key) | Key::Select;" "    if (g_chat_keys == Key::Select || clash(g_chat_keys, g_menu_keys)) {" "the clash is looked for with the menu's chord read"
need_line Tests/log_files_test.cpp '    CHECK(ini::ChordsNested("L+Left+Select", "l+left") && ini::ChordsNested("L+Select", ini::kDefaultChatKey));' "the ini test checks the nested chords"
# The chat is ON by default (the user's decision, 2026-11-04: it can be switched off in the app and in the in-game menu), and its lines are
# at the bottom left of the top screen until the user moves them. The word in the file is one of four; anything else is the default, read
# by the one shared parser in both programs, and the plugin's Config and the app's Settings take the default from the shared header.
need Includes/azl/ini_values.hpp "bool chat = true;" "the chat is on in a new file"
need Includes/ctr/plugin.hpp "bool chat = true;" "the plugin's chat is on when the file does not say otherwise"
need ConfigApp/source/settings.hpp "bool chat = true;" "the app's row In-game chat is on when the file does not say otherwise"
need Includes/azl/ini_values.hpp "enum ChatPos : int { kChatPosTopLeft = 0, kChatPosTopRight = 1, kChatPosBottomLeft = 2, kChatPosBottomRight = 3, kChatPosCount = 4 };" "the four corners of chat_pos, in the order the rows cycle"
need Includes/azl/ini_values.hpp 'constexpr const char* kChatPosWords[kChatPosCount] = {"top-left", "top-right", "bottom-left", "bottom-right"};' "the words of chat_pos in the file, in the order of the corners"
need Includes/azl/ini_values.hpp "constexpr int kChatPosDefault = kChatPosBottomLeft;" "the chat's lines are at the bottom left by default"
need_line Includes/azl/ini_values.hpp "    return kChatPosDefault;" "a word of chat_pos that is none of the four is the default"
need Includes/azl/ini_values.hpp 'out += std::string("chat_pos=") + ChatPosWord(f.chat_pos) + "\n";' "the file holds the word of the corner, never past the table"
need Includes/ctr/plugin.hpp "int chat_pos = ini::kChatPosDefault;" "the plugin takes the chat's corner from the shared header"
need ConfigApp/source/settings.hpp "int chat_pos = azl::ini::kChatPosDefault;" "the app takes the chat's corner from the shared header"
need Sources/ctr/plugin.cpp "config_.chat_pos = ini::ParseChatPos(value);" "the plugin reads chat_pos with the shared parser"
need ConfigApp/source/settings.cpp "cfg.chat_pos = azl::ini::ParseChatPos(value);" "the app reads chat_pos with the shared parser"
need ConfigApp/source/pages_config.cpp "chat_pos.dim = !g.cfg.chat;" "the row Chat position is dim while the chat is off"
tr -d '\r' < README.md | grep -q '^| `chat` | 1 |' || fail "README.md does not give 1 as the default of chat"
tr -d '\r' < README.md | grep -q '^| `chat_pos` | `bottom-left` |' || fail "README.md does not give bottom-left as the default of chat_pos"
# ... and the in-game menu switches the chat and moves it with the game paused: saved to the ini and taken at once through the atomics the
# chat reads on the other threads.
need Sources/game_menu.cpp 'DrawRow(scr, y, sel, "In-game chat", "", kDim, false);' "the in-game menu has the chat's switch"
need Sources/game_menu.cpp "plugin.SetChatPos(ini::NextChatPos(plugin.chat_pos(), right ? 1 : -1));" "left and right move the chat's corner in the in-game menu"
need_order Sources/game_menu.cpp "    rows.push_back(kChat);" "    rows.push_back(kDebug);" "the chat's rows come before the debug log"
need Sources/ctr/plugin.cpp "chat_on_.store(on, std::memory_order_relaxed);" "the chat's switch of the in-game menu is taken at once"
need Sources/ctr/plugin.cpp "chat_pos_.store(pos, std::memory_order_relaxed);" "the chat's corner of the in-game menu is taken at once"
# What the file says reaches the same atomics: without these two a chat=0 in the ini would leave the chat ON for the network thread and
# the game's (its memory and its work with the chat off, against the decision), and the corner of the file would never be used.
need_order Sources/ctr/plugin.cpp "config_.chat_pos = ini::ParseChatPos(value);" "    chat_on_.store(config_.chat, std::memory_order_relaxed);" "LoadConfig hands the chat's switch it read to the other threads"
need_order Sources/ctr/plugin.cpp "config_.chat_pos = ini::ParseChatPos(value);" "    chat_pos_.store(config_.chat_pos, std::memory_order_relaxed);" "LoadConfig hands the chat's corner it read to the other threads"
# The chat of the room (Includes/azl/chat_core.hpp, through RoomClient::SetChatSink). Its numbers are the user's decisions (2026-11-04): 5
# messages kept in all, 128 characters each (125 and "..." past that), a nick of 20, more than 4 a second dropped before the decoding, one of
# ours a second. The header allocates nothing and does not use the reader of room_proto.hpp (PacketReader::String mallocs a std::string per
# field): the packet's lengths are read by hand.
need Includes/azl/chat_core.hpp "constexpr u32 kLines = 5; " "the chat keeps 5 messages in all"
need Includes/azl/chat_core.hpp "constexpr u32 kTextMax = 128; " "a message is kept to 128 characters"
need Includes/azl/chat_core.hpp "constexpr u32 kNickMax = 20; " "a nick is kept to 20 characters (the room's own rule)"
need Includes/azl/chat_core.hpp "constexpr u32 kRateMax = 4; " "more than 4 messages a second are dropped"
need Includes/azl/chat_core.hpp "constexpr u32 kRateWindowMs = 1000; " "... in a sliding second"
# ... and at most 2 of them from one member, read before the decoding (review of the chat, defect 41: one ring for everybody, and a member
# flooding at 20 a second took nearly every place: a friend's messages at 1 a second were decoded 17% of the time)
need Includes/azl/chat_core.hpp "constexpr u32 kRatePerMember = 2; " "at most 2 of the 4 a second from one member: a flood leaves the others theirs"
need_after Includes/azl/chat_core.hpp "    bool Admit(u32 now_ms, u32 tag) {" "            same += (tags_[i] == tag && now_ms - ts_[i] < kRateWindowMs) ? 1 : 0;" "the rate limit counts the member's messages of the last second"
need_line Includes/azl/chat_core.hpp "        if (same >= kRatePerMember) {" "a member's message past its share of the second is dropped"
need_line Includes/azl/chat_core.hpp "        tags_[slot] = tag;" "the rate limit keeps who sent each message it took"
need Tests/chat_core_test.cpp "CHECK(friends == 120 && flood <= 120" "the chat's test runs a flood with two friends: every one of the friends' passes"
need Tests/chat_room_probe.cpp 'CHECK(std::strcmp(v.lines[3].text, "amid the flood") == 0' "a message amid a flood is kept, against the real room"
need Includes/azl/chat_core.hpp "constexpr u32 kSendGapMs = 1000; " "at most one message of ours a second"
need Includes/azl/chat_core.hpp "constexpr u32 kScanPerChar = 4;" "the cleaning stops early: 5000 characters cost what 128 do"
hit=$(tr -d '\r' < Includes/azl/chat_core.hpp | sed 's://.*$::' | grep -n -E 'std::(string|vector|function)|\bnew\b|\b(malloc|calloc|realloc)[[:space:]]*\(|PacketReader|#include <(string|vector|functional)>' | head -3)
[ -z "$hit" ] || fail "Includes/azl/chat_core.hpp allocates or reads with PacketReader (the chat is decoded by hand, with no heap): $hit"
# The room client: the chat packet is decoded only with a block (the chat off = nullptr = dropped unread, as before the chat), after the rate
# limit; the block is copied in the loop like the callbacks (no lock per packet); the room's status messages stay in the default; ours goes
# from the network thread only, built on the stack, reliable on channel 0, never through tx_, and only while joined.
need Sources/azl/room_client.cpp "                    case room::IdChatMessage:" "the room client hands the chat's packets to the chat's block"
need_after Sources/azl/room_client.cpp "                        if (chat) {" "if (chat->Admit(chat_ms, chat::SenderTag(data, size))) {" "a chat packet is decoded only after the rate limit took it, its sender read first"
if tr -d '\r' < Sources/azl/room_client.cpp | grep -n 'case room::IdStatusMessage'; then
  fail "Sources/azl/room_client.cpp handles the room's status messages: the plugin already says who comes and goes (they stay in the default)"
fi
need_after Sources/azl/room_client.cpp "            tick = tick_cb_;" "chat = chat_sink_.load(std::memory_order_acquire);" "the network loop takes the chat's block with the callbacks, when their generation changes"
need_after Sources/azl/room_client.cpp "void RoomClient::SetChatSink(ChatSink* sink) {" "cb_generation_.fetch_add(1);" "a new chat block (or none) is taken by the loop at its next round"
need Sources/azl/room_client.cpp "const u32 chat_size = chat->TakeOutgoing(static_cast<u32>(now_ms), state() == State::Joined, chat_packet);" "ours goes only while joined"
need Sources/azl/room_client.cpp "ENetPacket* packet = enet_packet_create(chat_packet, chat_size, EnetPacketFlags(true));" "ours goes reliable, like the room's own chat"
need_order Sources/azl/room_client.cpp "const u32 chat_size = chat->TakeOutgoing(" "        if (tx_pending_.load(std::memory_order_acquire) != 0) {" "ours is sent before the game's queue is taken, from the network thread"
# The plugin: with the chat on ONE block, made once and never deleted (a network thread that Stop() gave up on may still hold it); with the
# chat off none at all, and no sink.
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c 'new (std::nothrow) ChatSink()')" = "1" ] || fail "Sources/ctr/plugin.cpp makes the chat's block in more than one place, or with another gap between our messages"
if tr -d '\r' < Sources/ctr/plugin.cpp | grep -n -E 'delete[[:space:]]+(sink|chat_sink_)'; then
  fail "Sources/ctr/plugin.cpp deletes the chat's block: the network thread and the drawing may still hold it"
fi
need_after Sources/ctr/plugin.cpp "    if (config_read_early && config_.chat) {" "        WireChat(true);" "the chat's block is made at start only when the file was read and says the chat is on (a file not read yet may say chat=0: LateInit wires it)"
need_after Sources/ctr/plugin.cpp "    if (config_.chat || chat_sink_.load(std::memory_order_relaxed)) {" "        WireChat(config_.chat);" "LateInit wires the chat as the file read now says: chat=0 unhooks a block made at start, so the room no longer decodes the chat (review of the chat, defect 49)"
# A chat packet ENet refuses is freed here (enet_peer_send leaves it to the caller) and is not kept as our line.
need_after Sources/azl/room_client.cpp "                    if (enet_peer_send(server, 0, packet) == 0) {" "chat_sent = true;" "our chat line is kept only when ENet took the packet"
need_after Sources/azl/room_client.cpp "                    } else {" "enet_packet_destroy(packet);" "a chat packet ENet refuses is freed"
need Sources/azl/room_client.cpp "chat->SentOutgoing(config.nickname.c_str(), config.nickname.size(), static_cast<u32>(now_ms), chat_sent);" "our chat line is kept only when ENet took the packet"
# Our line is the room's too: a client that leaves within kChatGoodbyeMs of its last chat message waits (kGoodbyeWaitMs at most) for the room's
# acknowledgement before the disconnect, which throws away what ENet holds (and the room what it has not handled), on both ways out.
need_after Sources/azl/room_client.cpp "                        chat_sent = true;" "chat_sent_ms = now_ms;" "the goodbye knows when our last chat message went"
need Sources/azl/room_client.cpp "constexpr u32 kChatGoodbyeMs = 2000;" "a goodbye up to two seconds after our chat message waits for it"
need_line Sources/azl/room_client.cpp "        if (!to_send.empty() || (chat_sent_ms != 0 && platform::NowMs() - chat_sent_ms < kChatGoodbyeMs)) {" "the goodbye waits for the game's last frames and for our last chat message, on the quick stop (EXIT, sleep) and on Stop()"
need_order Sources/azl/room_client.cpp "        const bool quick = quick_stop_.load();" "        if (!to_send.empty() || (chat_sent_ms != 0" "... after the quick stop's frames went"
need_order Sources/azl/room_client.cpp "        if (!to_send.empty() || (chat_sent_ms != 0" "            enet_peer_disconnect_now(server, 0);" "... before the quick stop's disconnect"
need_order Sources/azl/room_client.cpp "        if (!to_send.empty() || (chat_sent_ms != 0" "            enet_peer_disconnect(server, 0);" "... and before the graceful one"
need_after Sources/azl/room_client.cpp "            for (const u64 until = platform::NowMs() + kGoodbyeWaitMs;" "platform::NowMs() < until && ReliableBacklog(server) > 0;) {" "the goodbye's wait is bounded by a deadline and ends at the acknowledgement"
need_after Sources/azl/room_client.cpp "            enet_peer_disconnect(server, 0);" "bool done = server->state == ENET_PEER_STATE_DISCONNECTED;" "a room that let us go during the wait is not waited for again"
need Tests/chat_room_probe.cpp 'std::printf("6. A sends and leaves at once' "the goodbye that waits for our last chat message is tested against the real room"
need_after Sources/ctr/plugin.cpp "    ChatSink* sink = chat_sink_.load(std::memory_order_relaxed);" "    if (on && !sink) {" "the block is made only when the chat goes on"
need Sources/ctr/plugin.cpp "    room_->SetChatSink(on ? sink : nullptr);" "the chat off hands the room client no block"
need Tests/run.sh 'Tests/chat_core_test.cpp -Wl,--wrap=malloc -o "$OUT/chat_core_test"' "the chat's test counts the allocations through the link"
need Tests/integration.sh "/tmp/azl_tests/chat_room_probe \$R1 24872" "the chat is tested against the real room"
# The chat's lines on the top screen (Includes/azl/chat_draw.hpp) and the drawing under them (Includes/azl/gfx_surface.hpp): they run on the
# game's thread inside the framework's OSD callback, so the two headers are held to the rule of the game-thread code everywhere (no heap,
# no container, no printf, no big buffer: their regions are counted with the others below), and they draw with their own stores, never with
# the framework's (DrawSysfont reads the framebuffer, blends and allocates; DrawRect writes a byte at a time; neither draws the right eye).
# The framebuffer is reached only through the word and halfword stores of gfx_surface.hpp (and its byte store): a read of that uncached
# memory costs more than a write, and a store that is not theirs is not counted by Tests/chat_draw_test.cpp.
# The user's decisions (2026-11-04): Spleen 6x12, opaque strips in the background of the app, the bottom-right corner above the framework's
# notifications; 2026-11-08: smaller and tighter, the whole width of the top screen (the 6x12 glyphs in a cell of 10 rows, lines of 62
# cells, at most 3 lines a message, so that the longest nick and 128 characters always show whole).
for h in Includes/azl/chat_draw.hpp Includes/azl/gfx_surface.hpp; do
  hit=$(tr -d '\r' < "$h" | sed 's://.*$::' | grep -n -E 'std::(string|vector|function|mutex)|\bnew\b|\b(malloc|calloc|realloc)[[:space:]]*\(|#include <(string|vector|functional|mutex|cstdio)>|DrawSysfont|DrawRect|GetTextWidth|OSD::|\bprintf\b' | head -3)
  [ -z "$hit" ] || fail "$h allocates, formats or draws with the framework (the chat draws on the game's thread with its own stores): $hit"
done
hit=$(tr -d '\r' < Includes/azl/gfx_surface.hpp | sed 's://.*$::' | grep -o -E 'reinterpret_cast<[^>]*>' | grep -v -x -E 'reinterpret_cast<(std::uintptr_t|Word\*|Half\*)>' | head -2)
[ -z "$hit" ] || fail "Includes/azl/gfx_surface.hpp reaches the framebuffer other than through Put32 / Put16 / Put8 (a read of it, or a store that is not counted): $hit"
[ "$(tr -d '\r' < Includes/azl/gfx_surface.hpp | sed 's://.*$::' | grep -c -E 'reinterpret_cast<(Word|Half)\*>')" = "2" ] || fail "Includes/azl/gfx_surface.hpp has a word or halfword access of the framebuffer other than the stores of Put32 and Put16"
need Includes/azl/gfx_surface.hpp "#define AZL_GFX_COUNT_STORE(bytes) ((void)0)" "the plugin's stores are counted by nothing (the test counts them)"
need Includes/azl/gfx_surface.hpp '#include "azl/font_spleen.inc"' "the chat's font is Spleen"
need Includes/azl/chat_draw.hpp "constexpr u32 kBlockLines = 3; " "a message takes at most 3 lines on the top screen"
need Includes/azl/chat_draw.hpp "constexpr u32 kCols = 62; " "a line of the chat is the whole width of the top screen (2026-11-08)"
need Includes/azl/chat_draw.hpp "constexpr int kGlyphW = 6, kLineH = 10; " "the chat's text is the 6x12 font in a cell of 10 rows: smaller, tighter lines"
need Includes/azl/gfx_surface.hpp "constexpr Font kFont6x10 = {&kSpleen6x10[0][0], 6, 10};" "the chat's font of 10 rows is the generated table"
need Includes/azl/chat_draw.hpp "                if (total - (at + i + 1) <= room) {" "a line breaks at a space only when the rest still fits the lines left: a message the block holds shows whole"
need_line Tests/chat_draw_test.cpp "    CHECK(whole == 2000);" "the chat's test wraps 2000 messages of 128 characters with the longest nick: every one whole"
# The geometry is held where it is made (CHAT-2, 2026-11-08): no block past the top screen's 400 columns in any corner (DrawBlock writes
# where it is told, on the game's framebuffer), every height a multiple of 4 rows (the strips go out as words), a column at most 32 rows
# (one run of RunColumnBoth: its masks are a u32, and a shift by 32 is undefined), and a guard at run time behind the asserts.
need Includes/azl/chat_draw.hpp "static_assert(kLeftX + BlockWidth(kCols) <= 400, " "a block in a left corner ends inside the top screen"
need Includes/azl/chat_draw.hpp "static_assert(kRightEdge <= 400 && kRightEdge - BlockWidth(kCols) >= kLeftX, " "a block in a right corner starts inside the top screen"
need Includes/azl/chat_draw.hpp "static_assert(BlockHeight(kBlockLines) <= 32, " "a column of a block is one run of 32 rows at most"
need Includes/azl/chat_draw.hpp "static_assert(BlockHeight(1) % 4 == 0 && BlockHeight(2) % 4 == 0 && BlockHeight(3) % 4 == 0, " "every block is a whole number of 4 rows"
need Includes/azl/chat_draw.hpp "static_assert(kNickMax + 2 + kTextMax <= kCols * kBlockLines, " "the longest nick and 128 characters fit a block"
need_after Includes/azl/chat_draw.hpp "            const int x0 = right ? kRightEdge - width : kLeftX;" "            if (x0 < 0 || x0 + width > s.w) {" "a block that would pass the screen's edge is not drawn"
need Includes/azl/chat_draw.hpp "    const u32 all = h >= 32 ? 0xFFFFFFFFu : (1u << h) - 1;" "the bar of a block of 32 rows is all 32 bits, never a shift by 32"
need Includes/azl/chat_draw.hpp "constexpr u32 kShowMs = 10000; " "the lines stay 10 s after the last message came (the user's decision)"
need Tests/chat_draw_test.cpp "    CHECK(chat::ShownLines(v, 0xFFFFFF00u + 10000u, false) == 0);" "the test holds the 10 s in numbers, not through kShowMs"
need Includes/azl/chat_draw.hpp "constexpr gfx::Rgb kBg = {13, 14, 17};" "the chat's strips are opaque, in the background of 3DS Link"
need Includes/azl/chat_draw.hpp "constexpr int kRightEdge = 390; " "the right corners end where the framework's notifications do"
need Includes/azl/chat_draw.hpp "return notices == 0 ? kBottomY : ((230 - 15 * static_cast<int>(notices) - 3) & ~3);" "the bottom-right corner climbs above the plugin's notifications"
need Includes/azl/chat_draw.hpp "            const gfx::Rgb bar = l.nick_len == 0 ? kNoticeBar : kNick[NickColour(l.nick, l.nick_len)];" "every block is drawn with its nick's colour, a notification of the plugin with its own"
need Includes/azl/chat_draw.hpp "            DrawBlock(s, x0, y0, l, w, gfx::Raw(bar, s.format), text_raw, bg_raw);" "every block is drawn with the colour of its bar"
# Their cost: the lines are drawn on the presenting game thread every frame for 10 s after a message, and the worst frame (5 blocks of two
# full lines, the 3D on) was 1.7-1.9 million instructions, 6-15 ms of an Old 3DS's frame, when every pixel called the colour's picker and
# every column divided stride by bpp (the ARM11 has no division: a call to the library's). The pixel's colour is worked out in line, the
# address with no division, a group of four background pixels is not worked out at all, and Tests/chat_draw_cost.sh (run by
# Tests/stack_usage.sh) counts the instructions of the plugin's own build of the two headers in an ARM emulator against budgets.
need Includes/azl/gfx_surface.hpp "__attribute__((always_inline)) inline u32 PickBit(u32 ma, u32 mb, u32 bit, u32 a, u32 b, u32 bg) {" "the colour of a pixel of the chat is worked out in line, never a call a pixel"
need Includes/azl/gfx_surface.hpp "    return x * s.stride + (s.h - 1u - y) * s.bpp;" "the address of a pixel is worked out with no division (the ARM11 has none)"
# The framework hands the game's format byte unmasked (0x40 the top screen in 2D, 0x20 the 3D) and works its Screen::BytesPerPixel out of it
# unmasked (3 for any value it does not know): the chat's lines and keyboard were never drawn in RGBA8 and the 16-bit formats with a high bit.
# The screen is taken through gfx::ScreenSurface (Tests/chat_host_test.cpp, TestScreenFormats): the format masked, the bytes a pixel from it.
need Includes/azl/gfx_surface.hpp "    s.format = static_cast<u8>(raw_format & 7);" "the format of a screen is its low 3 bits"
need Includes/azl/gfx_surface.hpp "    s.bpp = static_cast<u8>(BytesPerPixel(s.format));" "the bytes of a pixel of a screen come from its masked format"
need Sources/ctr/chat_overlay.cpp "    return gfx::ScreenSurface(reinterpret_cast<u8*>(scr.LeftFramebuffer)," "the chat takes the framework's screen through gfx::ScreenSurface"
need Sources/game_menu.cpp "    return gfx::ScreenSurface(reinterpret_cast<u8*>(scr.LeftFramebuffer)," "the paused menu takes the framework's screen through gfx::ScreenSurface"
hit=$(grep -rn -F 'BytesPerPixel' Sources | grep -v -F 'gfx::BytesPerPixel(' | head -2)
[ -z "$hit" ] || fail "the framework's Screen::BytesPerPixel is read (worked out of the game's format byte unmasked: 3 for 0x40 | RGBA8): $hit"
need_line Tests/chat_host_test.cpp "    TestScreenFormats();" "the chat host's test runs the screens with the high bits of the format"
[ "$(tr -d '\r' < Includes/azl/gfx_surface.hpp | grep -c -F 'if (((ma | mb) & 15) == 0) {')" = "2" ] || fail "Includes/azl/gfx_surface.hpp: a group of four background pixels is not worked out (RGBA8 and 16-bit)"
need Includes/azl/gfx_surface.hpp "            if (((ma | mb) & 15) != 0) {" "a group of four background pixels is not worked out (BGR8)"
need Includes/azl/chat_draw.hpp "            glyph[line] = &gfx::kFont6x10.cols[gfx::GlyphIndex(ch) * kGlyphW];" "a cell's glyph is looked up once for its 6 columns"
need_line Tests/stack_usage.sh "bash Tests/chat_draw_cost.sh || FAILED=1" "the instructions of the chat's lines are counted by the stack check"
need_line Tests/chat_draw_cost.py "        ok = drawn == lines and n <= budget and probe.bus <= bus_budget and same" "every frame of the chat's lines is held to its budgets of instructions and bus writes, and to the reference build byte by byte"
need Tests/chat_draw_cost.py "    ('5 long messages, 3D', 0, 1, 5, 20, 128, 721000, 30560)," "the worst frame of the chat's lines (RGBA8, the 3D on, 5 blocks of 382 x 32) stays within 721 000 instructions and 30 560 bus writes"
need Tests/chat_draw_cost.py "    ('5 long messages, 3D', 1, 1, 5, 20, 128, 792000, 30560)," "the worst frame of the chat's lines (BGR8, the 3D on) stays within 792 000 instructions and 30 560 bus writes"
need Tests/chat_draw_cost.py "    ('5 long messages, 3D', 2, 1, 5, 20, 128, 718000, 30560)," "the worst frame of the chat's lines (RGB565, the 3D on) stays within 718 000 instructions and 30 560 bus writes"
# The framebuffer is strongly ordered and uncached: every store is a bus transaction the core waits for, and one store a word was 83 440 of
# them in the worst frame of the lines (RGBA8, the 3D on). On the console a whole group of four pixels goes out in one stm an image (4, 3 or
# 2 words in r4-r7, from a word: RunColumnBoth reaches a word before its groups, gfx::Usable asks both images on words), 20 860 in that
# frame; the PC and the cost check's reference build (AZL_GFX_NO_BURST, never in the plugin's build) keep a store a word, and the check
# holds the plugin's build to the reference byte by byte.
need Includes/azl/gfx_surface.hpp "#if defined(__arm__) && !defined(__thumb__) && !defined(AZL_GFX_NO_BURST)" "a group of the chat's pixels is one block store on the console"
need Includes/azl/gfx_surface.hpp '    __asm__ volatile("stm %0, {r4-r7}" : : "r"(l + off), "r"(x0), "r"(x1), "r"(x2), "r"(x3) : "memory");' "a group of RGBA8 is one block store of four words"
need Includes/azl/gfx_surface.hpp '    __asm__ volatile("stm %0, {r4-r6}" : : "r"(l + off), "r"(x0), "r"(x1), "r"(x2) : "memory");' "a group of BGR8 is one block store of three words"
need Includes/azl/gfx_surface.hpp '    __asm__ volatile("stm %0, {r4-r5}" : : "r"(l + off), "r"(x0), "r"(x1) : "memory");' "a group of 16 bits is one block store of two words"
need Includes/azl/gfx_surface.hpp "            Put3Both(l, r, off, c0 | c1 << 24, c1 >> 8 | c2 << 16, c2 >> 16 | c3 << 8);" "a group of BGR8 goes out as one block"
[ "$(tr -d '\r' < Includes/azl/gfx_surface.hpp | grep -c -F '            Put32Both(l, r, off')" = "0" ] || fail "Includes/azl/gfx_surface.hpp: a group of pixels goes out one store a word"
! grep -r -q -F 'AZL_GFX_NO_BURST' Makefile Sources || fail "AZL_GFX_NO_BURST (the cost check's reference, a store a word) is set in the plugin's build"
need Tests/chat_draw_cost.sh "for v in probe:-UAZL_GFX_NO_BURST probe_ref:-DAZL_GFX_NO_BURST; do" "the cost check builds the plugin's probe and the reference with a store a word"
[ "$(tr -d '\r' < Tests/chat_draw_cost.py | grep -c -F -x '        same = probe.framebuffers() == ref.framebuffers()')" = "2" ] || fail "Tests/chat_draw_cost.py: the block stores of the lines and of the keyboard are not both checked byte by byte against the reference build"
need Tests/chat_draw_cost.sh "-w /src pablomk7/libctrpf:0.8.0" "the chat's lines are counted as the plugin's own compiler builds them"
# The keyboard's copy onto the bottom screen (azl/chat_host.hpp, CopyImage) runs on the same thread every frame the keyboard is open, the
# whole 320 x 240 screen: one store a word was 234 K instructions a frame in RGBA8 (several ms of an Old 3DS's frame). On the console it goes
# out in blocks of eight words (ldm/stm), only from and to words (a block access off a word is a data abort), and the same count holds it.
need_after Includes/azl/chat_host.hpp "inline void CopyImage(const Image& im, const gfx::Surface& fb) {" "#if defined(__arm__) && !defined(__thumb__)" "the keyboard's copy is the burst on the console"
need Includes/azl/chat_host.hpp '"    stm   %[dst]!, {r3-r9, r12}' "the keyboard's copy writes eight words a store"
need Includes/azl/chat_host.hpp "im.col_bytes == kImageH * im.bpp && (reinterpret_cast<std::uintptr_t>(im.pixels) & 3) == 0;" "the keyboard's picture is copied only from a word's address"
# ... and only onto a screen of its own format: RGB565, RGB5A1 and RGBA4 have the same bytes a pixel, so the bpp alone would copy a picture
# of one onto another (review of the chat, H15: one or two frames of the keyboard in the wrong colours); Tests/chat_host_test.cpp tries them
need Includes/azl/chat_host.hpp "    return gfx::Usable(fb) && fb.format == im.format && fb.bpp == im.bpp && fb.w == kImageW" "the keyboard's picture is copied only onto a screen of its own format"
need Tests/chat_host_test.cpp "            CHECK(chat::ImageFits(im16.im, fb16.s) == (fi == ff));" "the chat host's test copies no picture across the 16-bit formats"
need Tests/chat_draw_cost.py "        ok = copied == 1 and wrong == 0 and n <= budget" "the keyboard's copy is checked byte by byte and held to its budget of instructions"
need Tests/chat_draw_cost.py "    (0, 240, 44800),  # RGBA8" "the keyboard's copy (RGBA8) stays within 44 800 instructions a frame"
# The keyboard's picture is drawn on the plugin's thread, at the game's own priority on the Old 3DS's core 0 (a game's thread woken meanwhile
# waits): the whole screen is 1.5-2 million instructions, 5.6-7.4 ms at 268 MHz, and every change of the view (the blink twice a second, every
# key) drew it all. Only what changed is drawn again (osk::DrawOskOver), byte for byte the whole drawing (osk_test, and the count's check).
need Sources/ctr/chat_overlay.cpp "        osk::DrawOskOver(chat::ImageSurface(*g_image), v, g_drawn);" "the chat's keyboard is drawn again only where its view changed"
need Includes/azl/osk_draw.hpp "            if (!SameLook(look, LookOf(was, key)) || (recase && kKeys[i].kind == kChar && kKeys[i].ch >= 'a' && kKeys[i].ch <= 'z')) {" "a key is drawn again only when its look or its label changed"
need_line Tests/osk_test.cpp "    TestDrawOver();" "the keyboard drawn over its picture is checked against the whole drawing, byte by byte"
# The picture is drawn again only when the view changed (review of the chat, defect 26: osk::Same guards the redraw of chat_overlay.cpp and of
# the game menu, built by no test on the PC; a guard dropped or a Same never true drew the 333 KB picture 30 times a second, 10 MB/s, with more
# frames of mixed pixels): the guard and Same's compare are pinned, osk_test checks two views of an idle keyboard equal and every field
need_after Sources/ctr/chat_overlay.cpp "    g_osk->Snapshot(&v);" "    if (!force && g_drawn_valid && osk::Same(v, g_drawn)) {" "the chat's keyboard is drawn again only when its view changed"
need_after Sources/ctr/chat_overlay.cpp "    if (!force && g_drawn_valid && osk::Same(v, g_drawn)) {" "        return;" "the chat's keyboard with the same view leaves the redraw"
need Sources/game_menu.cpp "    if (!osk::Same(v, m.osk_drawn)) {" "the menu's keyboard is drawn again only when its view changed"
need_after Includes/azl/osk.hpp "inline bool Same(const View& a, const View& b) {" "    return std::memcmp(&a, &b, sizeof(View)) == 0;" "two views are the same when they are equal byte for byte"
need Includes/azl/osk.hpp "        std::memset(v, 0, sizeof(View));" "every byte of a view is written, its padding too (Same compares bytes)"
need_line Tests/osk_test.cpp "    TestSame();" "the keyboard's test checks the redraw's guard: idle views the same, every field changed seen"
need Tests/chat_draw_cost.py "        ok = wrong == 0 and n <= budget" "every redraw of the keyboard is checked byte by byte and held to its budget of instructions"
need Tests/chat_draw_cost.py "    (1, 0, 70700), (1, 1, 116000), (1, 2, 70000)," "the cursor's blink redraws one line of the keyboard (at most 116 K instructions)"
need Tests/chat_draw_cost.py "    (3, 0, 196900), (3, 1, 279300), (3, 2, 212100)," "a character typed redraws a line, the bar and a key (at most 280 K instructions)"
need Tests/chat_draw_cost.py "    uc.hook_add(UC_HOOK_BLOCK, on_block)" "the count's hook is there from the start (a hook added later misses the blocks already translated)"
# The font is made by its script from the BDF files of a Spleen release, with the license in it (the BSD 2-Clause asks a copy of the source to
# keep it) and in the credits (CREDITS.md, which the binary's copies go with, and the app's list: about_test holds the two the same).
head -1 Includes/azl/font_spleen.inc | grep -q "^// Generated by Tests/make_font_spleen.py from the BDF files of Spleen" || fail "Includes/azl/font_spleen.inc is not marked as generated by Tests/make_font_spleen.py (run the script on the release's files instead of editing it)"
[ -f Tests/make_font_spleen.py ] || fail "Tests/make_font_spleen.py, the script that makes the chat's font, is gone"
need Includes/azl/font_spleen.inc "// Copyright (c) 2018-2024, Frederic Cambus" "the font keeps Spleen's copyright"
need Includes/azl/font_spleen.inc "//   * Redistributions in binary form must reproduce the above copyright" "the font keeps Spleen's license"
# The fonts and the keyboard's table are inline: one copy in the plugin (a constexpr of a namespace has internal linkage, so game_menu.o and
# chat_overlay.o had a copy each of all three, 3,812 bytes of .rodata twice in the plugin's block, on the Old 3DS's 5 MiB too).
need Includes/azl/font_spleen.inc "inline constexpr u16 kSpleen6x12[95][6] = {" "the 6x12 font is in the plugin once"
need Includes/azl/font_spleen.inc "inline constexpr u16 kSpleen8x16[95][8] = {" "the 8x16 font is in the plugin once"
need Includes/azl/font_spleen.inc "inline constexpr u16 kSpleen6x10[95][6] = {" "the 6x10 font (the chat's lines) is in the plugin once"
need Tests/make_font_spleen.py "    _, t10 = table('kSpleen6x10', bdf6, crop10, 10)" "the 6x10 font is made by the script from the 6x12 BDF, not by hand"
need Tests/make_font_spleen.py "'inline constexpr u16 %s[%d][%d] = {'" "the font's script makes the tables inline"
need Includes/azl/osk.hpp "inline constexpr KeyDef kKeys[] = {" "the keyboard's table is in the plugin once"
need CREDITS.md "Copyright (c) 2018-2024, Frederic Cambus" "the credits carry Spleen's copyright (the copy of the license that goes with the binaries)"
need CREDITS.md "Redistributions in binary form must reproduce the above copyright" "the credits carry Spleen's license"
need ConfigApp/source/about_text.hpp '{"Spleen", "BSD-2-Clause", "Frederic Cambus",' "the app's credits thank Spleen"
need Tests/run.sh 'Tests/chat_draw_test.cpp -Wl,--wrap=malloc -o "$OUT/chat_draw_test"' "the chat's drawing test counts the allocations through the link"
# The plugin's one on-screen keyboard (Includes/azl/osk.hpp: the table and the state; Includes/azl/osk_draw.hpp: the drawing), the chat's in
# the game and the password's in the paused menu. No heap (the host makes it with new (std::nothrow) when it opens and deletes it when it
# closes), no framework, no printf, and the framebuffer only through gfx_surface.hpp's stores. Its edges are its own, from the keys held:
# in the game the framework's loop updates the controller every frame, so its "pressed" is nearly always 0 there (STUDIO-CHAT section 11),
# and a key held when it opens (the chord) is no press. The paused menu asks a room's password on it, never on the framework's Keyboard
# (whose look settings went from main.cpp with it).
for h in Includes/azl/osk.hpp Includes/azl/osk_draw.hpp; do
  hit=$(tr -d '\r' < "$h" | sed 's://.*$::' | grep -n -E 'std::(string|vector|function|mutex)|\bnew\b|\b(malloc|calloc|realloc)[[:space:]]*\(|#include <(string|vector|functional|mutex|cstdio)>|DrawSysfont|DrawRect|GetTextWidth|OSD::|Controller::|GetKeysPressed|hidKeysDown|\bprintf\b|reinterpret_cast' | head -3)
  [ -z "$hit" ] || fail "$h allocates, formats, reads the pad through the framework or reaches the framebuffer other than through gfx_surface.hpp: $hit"
done
need Includes/azl/osk.hpp "const u32 pressed = held & ~prev_held_;" "the keyboard makes its own edges from the keys held"
need Includes/azl/osk.hpp "u32 prev_held_ = 0xFFFFFFFFu;" "a key held when the keyboard opens is no press"
need Includes/azl/osk.hpp "bool touch_prev_ = true; " "a finger on the screen when the keyboard opens is no key"
need Includes/azl/osk.hpp "constexpr u32 kRepeatDelayMs = 400; " "a held key repeats after 400 ms"
need Includes/azl/osk.hpp "constexpr u32 kRepeatMs = 90; " "... and then every 90 ms"
need Includes/azl/osk.hpp "constexpr u32 kRevealMs = 1000; " "a password's last character stays visible 1 s"
need Sources/game_menu.cpp "m.osk = new (std::nothrow) osk::Osk(o, Plugin::Get().config().password.c_str());" "the password's keyboard is made when the room refuses (and deleted when it closes)"
need Sources/game_menu.cpp "    o.max_len = 64;" "a room's password has at most 64 characters"
need Sources/game_menu.cpp "    o.mask = true;" "the password is masked"
hit=$(tr -d '\r' < Sources/game_menu.cpp | sed 's://.*$::' | grep -n -E '\bKeyboard\b' | head -2)
[ -z "$hit" ] || fail "Sources/game_menu.cpp uses the framework's Keyboard (a room's password is typed on the plugin's own keyboard, azl/osk.hpp): $hit"
if tr -d '\r' < Sources/main.cpp | grep -n -E 'look\.Keyboard\.'; then
  fail "Sources/main.cpp sets the look of the framework's Keyboard, which the plugin no longer opens"
fi
need Tests/run.sh 'Tests/osk_test.cpp -Wl,--wrap=malloc -o "$OUT/osk_test"' "the keyboard's test counts the allocations through the link"
# The chat over the running game (Sources/ctr/chat_overlay.cpp, the console's side; Includes/azl/chat_host.hpp, the pure parts that
# Tests/chat_host_test.cpp holds). The framework's OSD callback is registered ONCE, the first time the chat is on, and never with the chat
# off; on the game's thread it passes the swap's gate with GateTryPass (never GateEnter or SwapGateScope: a game's thread parked in the
# plugin's code wakes up in memory that is gone) and only copies. The keyboard is fed from the keys held (never the framework's "pressed",
# never its sysfont, and Controller::InjectKey/InjectTouch write a page that is read-only: a data abort), and the picture it is drawn into
# is made only while it is open. It closes, its draft kept, on the notifications counted by the hook (any level of the log, the low byte)
# and on the framework's events, which main.cpp counts before Plugin::NoteProcessEvent.
for h in Includes/azl/chat_host.hpp Sources/ctr/chat_overlay.cpp; do
  hit=$(tr -d '\r' < "$h" | sed 's://.*$::' | grep -n -E 'GetKeysPressed|hidKeysDown|DrawSysfont|DrawRect|Inject(Key|Touch)|GateEnter|SwapGateScope|\bprintf\b' | head -3)
  [ -z "$hit" ] || fail "$h reads the pad's 'pressed', draws with the framework, injects input or parks a game's thread at the gate: $hit"
done
hit=$(tr -d '\r' < Includes/azl/chat_host.hpp | sed 's://.*$::' | grep -n -E 'std::(string|vector|function|mutex)|\bnew\b|\b(malloc|calloc|realloc)[[:space:]]*\(|#include <(string|vector|functional|mutex|cstdio)>|OSD::|Controller::' | head -3)
[ -z "$hit" ] || fail "Includes/azl/chat_host.hpp allocates or reaches the framework (it is pure: the console's side is chat_overlay.cpp): $hit"
hit=$(grep -rn -E 'Inject(Key|Touch)' Sources Includes | head -2)
[ -z "$hit" ] || fail "the plugin injects input through the framework (Controller::InjectKey/InjectTouch write a read-only page: a data abort): $hit"
[ "$(grep -rh -E 'OSD::Run\(' Sources | grep -v -E '^[[:space:]]*//' | wc -l)" = "1" ] || fail "OSD::Run is called in more than one place, or nowhere (one callback, the chat's, registered once)"
need_after Sources/ctr/chat_overlay.cpp "    if (on && !g_osd_running) {" "OSD::Run(OnChatOsd);" "the chat's OSD callback is registered only with the chat on, and once"
need_after Sources/ctr/chat_overlay.cpp "bool OnChatOsd(const Screen& scr) {" "if (!GateTryPass(g_swap_gate, kGateOsd)) {" "the chat's OSD callback passes the swap's gate before anything else, and never stops at it"
need_after Sources/ctr/chat_overlay.cpp "    if (!GateTryPass(g_swap_gate, kGateOsd)) {" "        return false;" "the gate closed, the chat's OSD callback goes out at once: nothing of the plugin's block is read while it goes to the SD"
need_after Sources/ctr/chat_overlay.cpp "    u32 drew = chat::kDrewNothing;" "    if (g_overlay.on.load(std::memory_order_relaxed) != 0) {" "past the gate, the chat's OSD callback looks at the switch first: off after an on (the callback stays registered), it reads no clock and makes no svc"
need_after Sources/ctr/chat_overlay.cpp "    if (g_overlay.on.load(std::memory_order_relaxed) != 0) {" "chat::DrawOverlay(g_overlay, SurfaceOf(scr)" "the chat's OSD callback draws, and reads the clock for it, only with the chat on"
need_order Sources/ctr/chat_overlay.cpp "    if (!GateTryPass(g_swap_gate, kGateOsd)) {" "g_overlay.on.load(std::memory_order_relaxed)" "the chat's OSD callback reads its switch only past the gate (g_overlay is in the plugin's block)"
need_line Sources/ctr/chat_overlay.cpp "    GateLeave(g_swap_gate, kGateOsd);" "the chat's OSD callback leaves the swap's gate it passed (else every drain of SWAP_ENTER waits its whole budget)"
need_after Sources/ctr/chat_overlay.cpp "    GateLeave(g_swap_gate, kGateOsd);" "    return drew != chat::kDrewNothing;" "the chat's OSD callback leaves the gate right before its only return after the pass"
need_order Sources/ctr/chat_overlay.cpp "chat::DrawOverlay(g_overlay, SurfaceOf(scr)" "GateLeave(g_swap_gate, kGateOsd);" "the chat's OSD callback leaves the gate only after drawing"
need Includes/azl/swap_gate.hpp "constexpr u32 kGateOsd = 1u << 24;" "the game's thread in the chat's OSD callback has a weight of its own in the swap's gate"
need Includes/azl/swap_gate.hpp "    return g.on == 0 || GateTryEnter(g, who);" "GateTryPass counts nothing on a New 3DS (GateLeave counts nothing there)"
need Includes/azl/chat_host.hpp "    return raw & 0xFFu;" "the chat reads the notification's low byte (0x608 is the power button of an Old 3DS)"
need Includes/azl/chat_host.hpp "    return (signal >= 1 && signal <= 3) || signal == 5 || (signal >= 7 && signal <= 11);" "the keyboard closes on the notifications 1, 2, 3, 5, 7, 8, 9, 10, 11"
need Includes/azl/chat_host.hpp "    return signal == 1 || signal == 2 || signal == 8 || signal == 9;" "no opening for a while after HOME and power"
need Includes/azl/chat_host.hpp "constexpr u32 kReopenHoldMs = 2000; " "no opening for 2 s after HOME, power or a swap"
need Includes/azl/chat_host.hpp "    if (rc < 0 || static_cast<s32>(result_word) < 0) {" "only a notification the game was given closes the keyboard"
need Includes/azl/chat_host.hpp "constexpr u32 kWorkPerSecond = 30; " "the keyboard's work at most 30 times a second"
need Sources/ctr/chat_overlay.cpp "constexpr u64 kWorkGapTicks = chat::WorkGap(SYSCLOCK_ARM11);" "the keyboard's pace on the console is 1/30 s"
need Includes/azl/chat_host.hpp "    return (per_second + WorkEarly(per_second) + kWorkPerSecond - 1) / kWorkPerSecond;" "the pace's gap is rounded up and holds the early allowance: no second holds a 31st job"
# The pace takes the jitter of the frame callback (review of the chat, defect 37: a job refused for coming 0.1 ms before 1/30 s after the last
# went to the third frame, 22-26 jobs a second at 60 fps, 18-23 at 30 fps): each job has a place at most 2 ms before it, the next a gap later
need_line Includes/azl/chat_host.hpp "constexpr u32 kWorkEarlyDiv = 500;  ///< a job's place in the pace may be this part of a second (2 ms) before it: the frames' own jitter" "the pace's early allowance is 2 ms: a gap of 33.4 ms, less than two frames"
need_after Includes/azl/chat_host.hpp "    bool Due(u64 now) {" "        if (started_ && now < next_) {" "the pace never takes a job before the next place"
need_line Includes/azl/chat_host.hpp "        next_ = now + gap_ - (behind < early_ ? behind : early_); // (this job's place, and a gap; early_ < gap_)" "a job's place is at most the early allowance before it"
need Sources/ctr/chat_overlay.cpp "constexpr u64 kWorkEarlyTicks = chat::WorkEarly(SYSCLOCK_ARM11);" "the keyboard's pace on the console takes 2 ms of jitter"
need_line Sources/ctr/chat_overlay.cpp "chat::WorkPacer g_pacer(kWorkGapTicks, kWorkEarlyTicks);" "the keyboard's pace on the console is given its early allowance"
need_line Tests/chat_host_test.cpp "    TestPaceJitter();" "the chat host's test runs the pace with the frames' jitter"
# The latch between two jobs keeps the edges (review of the chat, defect 23: the touch OR-ed over a job's frames merged a lift of 17-33 ms,
# and "q, one frame up, w" typed nothing): a second edge waits for the next job, the press after a short release is never folded into it
need_after Includes/azl/chat_host.hpp "    void Sample(u32 v) {" "        two ^= t & one;" "the keyboard's latch counts a second edge of a key or of the touch between two jobs"
need_after Includes/azl/chat_host.hpp "        seen ^= one;" "        one = two;" "the keyboard's latch gives the waiting edge at the next job"
need_after Includes/azl/chat_host.hpp "    osk::Input Take(u32 now_ms) {" "        in.held = keys_.Take();" "the keyboard is given the keys' edges one a job"
need_line Includes/azl/chat_host.hpp "        in.touch = touch_.Take() != 0;" "the keyboard is given the touch's edges one a job, not the touch of any frame since the last"
need_line Tests/chat_host_test.cpp "    TestLatchLifts();" "the chat host's test runs the short lifts of the stylus and of the keys"
need Sources/ctr/hook.cpp "static __attribute__((noinline)) void NoteAptForChat(Result rc, u32 result_word, u32 raw) {" "the chat's observer of the notifications is out of line"
need_order Sources/ctr/hook.cpp "        NoteAptForChat(rc, cmd[1], raw);" "        if (g_apt_observer && g_debug_level > 0 && answered) {" "the chat counts the notifications at every level of the log, before the debug log's observer"
need_order Sources/main.cpp "    azl::ctr::ChatProcessEvent(static_cast<int>(event));" "    Plugin::Get().NoteProcessEvent(static_cast<int>(event));" "the chat counts the framework's events before the plugin's swap work"
# The plugin is made in PatchProcess, on the one thread that runs it, BEFORE the event callback is registered (it calls Plugin::Get()):
# with the callback first, the framework's keep-alive thread and PatchProcess could be in the guard of the function-local static of
# Plugin::Get() at once, and the plugin is built with -fno-exceptions (abort). Decision of 2026-11-09 (the fork build 57 of klark231).
patch_process=$(tr -d '\r' < Sources/main.cpp | awk '/^void PatchProcess\(FwkSettings& settings\) \{$/ {on = 1} on {print} on && /^}$/ {exit}')
pp_get=$(printf '%s\n' "$patch_process" | grep -n -F -x '    Plugin& plugin = Plugin::Get();' | cut -d: -f1)
pp_cb=$(printf '%s\n' "$patch_process" | grep -n -F -x '    Process::SetProcessEventCallback(OnProcessEvent);' | cut -d: -f1)
pp_init=$(printf '%s\n' "$patch_process" | grep -n -F -x '    plugin.EarlyInit();' | cut -d: -f1)
{ [ -n "$pp_get" ] && [ -n "$pp_cb" ] && [ -n "$pp_init" ] && [ "$pp_get" -lt "$pp_cb" ] && [ "$pp_cb" -lt "$pp_init" ]; } || fail "Sources/main.cpp: PatchProcess must make the plugin (Plugin& plugin = Plugin::Get();) before SetProcessEventCallback, then call plugin.EarlyInit()"
[ "$(cat Sources/*.cpp Sources/ctr/*.cpp | tr -d '\r' | grep -c 'SetProcessEventCallback(')" = "1" ] || fail "the event callback is registered somewhere else than in PatchProcess"
need Sources/main.cpp "azl::ctr::ChatFrame(g_chat_keys, !g_menu_off);" "the chat's work runs every frame of the running game"
need Sources/ctr/chat_overlay.cpp "    g_osk = new (std::nothrow) osk::Osk(o, g_draft);" "the chat's keyboard is made when it opens, from the draft"
need Sources/ctr/chat_overlay.cpp "    u8* pixels = im ? new (std::nothrow) u8[chat::ImageBytes(format)] : nullptr;" "the keyboard's picture is made only while it is open"
need Sources/ctr/chat_overlay.cpp "    FreeImage(g_keeper.Freeable(g_overlay.image));" "the keyboard's picture is freed only when no copy can read it"
# Nothing of the keyboard outlives it (chat_overlay.cpp is built by no test on the PC: up to 333,440 bytes a keyboard on the Old 3DS's 5 MiB
# block): the picture's pixels and the picture freed, the keyboard deleted at its close after its picture is withdrawn, a picture the close
# could not withdraw withdrawn by the next frame, and no new picture while an old one waits (the keeper holds one: a second Retire fails)
need_after Sources/ctr/chat_overlay.cpp "void FreeImage(chat::Image* im) {" "        delete[] im->pixels;" "the keyboard's picture frees its pixels"
need_after Sources/ctr/chat_overlay.cpp "        delete[] im->pixels;" "        delete im;" "the keyboard's picture is freed with its pixels"
need_after Sources/ctr/chat_overlay.cpp "    if (g_image && g_keeper.Retire(g_overlay.image)) {" "    delete g_osk;" "the keyboard's close withdraws its picture, then deletes the keyboard"
need_after Sources/ctr/chat_overlay.cpp "    FreeImage(g_keeper.Freeable(g_overlay.image));" "    if (!g_osk && g_image && g_keeper.Retire(g_overlay.image)) {" "a picture left with the keyboard closed is withdrawn on the next frame"
need_after Sources/ctr/chat_overlay.cpp "    if (!g_keeper.Empty()) {" "        return nullptr;" "no new picture while an old one waits to be freed"
# The game's input goes to the mirror only once the keyboard's picture is published (one MirrorWant, right after the first Publish of an
# opening): a picture that cannot be made (no memory on the Old 3DS's 5 MiB block, a format it does not draw in) or a bottom screen the game
# never presents within kShowWaitMs closes the keyboard with a notification, once; before that the keys are the game's, nothing typed blind
# (review of the chat, defect 15: an invisible keyboard held the game's input, the picture's new of 150-300 KB tried again every frame)
need_after Sources/ctr/chat_overlay.cpp "    g_overlay.image.Publish(im);" "    if (!g_shown) {" "the game's input is wanted only after the keyboard's picture is published"
need_after Sources/ctr/chat_overlay.cpp "    if (!g_shown) {" "        MirrorWant();" "the first picture of an opening hands the game's input to the mirror"
n=$(tr -d '\r' < Sources/ctr/chat_overlay.cpp | sed 's://.*$::' | grep -c 'MirrorWant()')
[ "$n" = 1 ] || fail "Sources/ctr/chat_overlay.cpp asks the mirror $n times (once, after the keyboard's picture is published: never at the opening)"
need_line Sources/ctr/chat_overlay.cpp "    g_shown = false; // (the game's input stays its own until the Image is published: EnsureImage)" "every opening waits for its own picture"
need Sources/ctr/chat_overlay.cpp "constexpr u32 kShowWaitMs = 1000;" "an opened keyboard that is not on the screen after a second is closed"
need Sources/ctr/chat_overlay.cpp "    if (!cannot && !g_shown && now_ms - g_opened_ms >= kShowWaitMs) {" "the keyboard waits for the bottom screen at most kShowWaitMs"
need_after Sources/ctr/chat_overlay.cpp "    if (cannot) {" '        Close(true, "not shown");' "a keyboard that cannot be shown is closed, its draft kept"
need_after Sources/ctr/chat_overlay.cpp '        Close(true, "not shown");' "        Notify(cannot);" "a keyboard that cannot be shown says so"
need Sources/ctr/chat_overlay.cpp "    if (!g_shown || !g_pacer.Due(svcGetSystemTick())) {" "nothing is typed before the keyboard is seen"
# The keyboard's work at most 30 times a second (review of the chat, defect 25: WorkPacer is tested in chat_host_test, but chat_overlay.cpp is
# built by no test on the PC and only the gap's constant was pinned): the pace's check leaves the frame, and the keyboard's only Feed and
# only redraw of the frame's work come after it
need_after Sources/ctr/chat_overlay.cpp "    if (!g_shown || !g_pacer.Due(svcGetSystemTick())) {" "        return; // (nothing typed before the keyboard is seen" "a frame between two of the keyboard's jobs goes out at once"
n=$(tr -d '\r' < Sources/ctr/chat_overlay.cpp | sed 's://.*$::' | grep -c -E '(->|\.)Feed\(|Redraw\(false\)')
[ "$n" = 2 ] || fail "Sources/ctr/chat_overlay.cpp feeds or redraws the keyboard $n times (one Feed and one Redraw(false), both after the pace)"
need_order Sources/ctr/chat_overlay.cpp "    if (!g_shown || !g_pacer.Due(svcGetSystemTick())) {" "    const osk::Result r = g_osk->Feed(g_latch.Take(now_ms));" "the keyboard is fed only after the pace"
need_order Sources/ctr/chat_overlay.cpp "    if (!g_shown || !g_pacer.Due(svcGetSystemTick())) {" "    Redraw(false);" "the keyboard's picture is redrawn only after the pace"
need_order Sources/ctr/chat_overlay.cpp "    if (!g_keeper.Empty()) {" "        g_keeper.Retire(g_overlay.image); // (the slot shows g_image" "the picture of another format is withdrawn only with the keeper empty"
# ... and freed before the new one is made, never two on the heap (review of the chat, defect 35: the new one made at once, 230-307 KB
# more for a frame on the Old 3DS's 5 MiB block, 541,948 bytes over the base in the stress test): at once when no copy reads it, else the
# new one waits for a next frame
need_after Sources/ctr/chat_overlay.cpp "        g_keeper.Retire(g_overlay.image); // (the slot shows g_image" "        chat::Image* old = g_keeper.Freeable(g_overlay.image);" "the picture of another format is freed at once when no copy reads it"
need_after Sources/ctr/chat_overlay.cpp "        chat::Image* old = g_keeper.Freeable(g_overlay.image);" "        FreeImage(old);" "the picture of another format is freed before the new one is made"
need_after Sources/ctr/chat_overlay.cpp "        FreeImage(old);" "        if (old == nullptr) {" "a picture of another format still read waits: no new one beside it"
need_after Sources/ctr/chat_overlay.cpp "        if (old == nullptr) {" "            return nullptr; // (a copy still reads it" "a picture of another format still read waits: no new one beside it"
need_order Sources/ctr/chat_overlay.cpp "        FreeImage(old);" "    chat::Image* im = new (std::nothrow) chat::Image();" "the old picture is freed before the new one is made"
# Send asks whether the box is busy BEFORE it posts (only the network thread empties it): asked after a failed Post, a box emptied in
# between read as "nothing to send" and the draft was thrown away (review of T5, 2026-11-04)
need_after Includes/azl/chat_host.hpp "inline Sent SubmitDraft(ChatSink& sink, const char* text, u32 n) {" "    if (sink.OutboxBusy()) {" "the keyboard's Send asks the box first: a draft is never lost to the network thread emptying it"
# HOME, power, sleep or exit close an open keyboard on the next frame, its draft kept and at once (the user's fall-back: the keyboard, its
# picture on the plugin's heap and the mirror's wish never outlive HOME or the swap); CloseRules is tested in chat_host_test, but chat_overlay.cpp
# is built by no test on the PC: that ChatFrame asks it and closes on its answer is held here
need Sources/ctr/chat_overlay.cpp "    const bool must_close = g_rules.Update(chat::Look(chat::g_signals), now_ms);" "the chat's frame asks the close rules about HOME, power, sleep and exit"
need_after Sources/ctr/chat_overlay.cpp "    if (g_osk && must_close) {" '        Close(true, "HOME, power, sleep or exit", true);' "an open keyboard closes on HOME, power, sleep or exit, its draft kept and at once"
# ... and every other closing that is not a Send keeps the draft too (review of the chat, defect 51: only HOME's was held, while the Close calls
# and Close's own copy are built by no test on the PC; a closing turned to Close(false, ...) lost what was typed, in silence)
need_after Sources/ctr/chat_overlay.cpp "    } else if (g_osk && !on) {" '        Close(true, "chat off", true);' "an open keyboard closes when the chat is turned off, its draft kept and at once"
need_after Sources/ctr/chat_overlay.cpp "    } else if (g_osk && plugin.room().state() != RoomClient::State::Joined) {" '        Close(true, "left the room", true);' "an open keyboard closes when the room is left, its draft kept and at once"
need_after Sources/ctr/chat_overlay.cpp "    if (r == osk::Result::kCancel) {" '        Close(true, "cancelled");' "a keyboard cancelled keeps its draft"
need_line Sources/ctr/chat_overlay.cpp '            Close(true, "not in a room");' "a Send out of a room closes the keyboard with its draft kept"
need_after Sources/ctr/chat_overlay.cpp "    if (keep) {" "            g_draft[i] = g_osk->text()[i];" "Close with keep copies what was typed into the draft"
need_after Sources/ctr/chat_overlay.cpp "            g_draft[i] = g_osk->text()[i];" "        g_draft[n] = '\0';" "the draft kept is ended"
need Sources/ctr/chat_overlay.cpp "        const chat::Sent sent = chat::SubmitDraft(*sink, g_osk->text(), g_osk->length());" "the keyboard's Send goes through SubmitDraft"
# Our message refused after Send (review of the chat, defect 46: the keyboard closed and the draft was forgotten at once, so the room left
# before the network thread's round, or ENet not taking the packet, lost the text in silence): the refusals read before the Post, the message
# watched, and a refusal gives its text back as the draft (unless a newer one is there) and tells the player
need_order Sources/ctr/chat_overlay.cpp "        const u32 refused = sink->counters.refused.load(std::memory_order_relaxed);" "        const chat::Sent sent = chat::SubmitDraft(*sink, g_osk->text(), g_osk->length());" "the refusals are read before Send posts: the refusal of the message posted is never in the snapshot"
need_after Sources/ctr/chat_overlay.cpp "        if (sent == chat::Sent::kPosted) {" "            if (g_watch.Posted(refused, left)) {" "a message posted is watched until the network thread sent or refused it (and a refusal of the one before, in between, is told)"
need Sources/ctr/chat_overlay.cpp "    if (!g_watch.Refused(*sink)) {" "the chat asks whether our last message was refused"
need_after Sources/ctr/chat_overlay.cpp "    } else if (g_draft[0] == '\0') {" "        std::memcpy(g_draft, sink->Outbox(), sizeof g_draft);" "a message refused after Send goes back to the draft (the keyboard closed)"
need_after Sources/ctr/chat_overlay.cpp "    if (g_osk) {" "            g_osk->SetText(sink->Outbox());" "a message refused after Send goes back into the open keyboard when nothing is typed in it"
need Sources/ctr/chat_overlay.cpp 'Notify(left ? "Message not sent: not in the room" : "Message not sent: the room did not take it");' "the player is told of a message refused after Send, and why (CHAT-5)"
need Sources/ctr/chat_overlay.cpp "        TellRefused(sink); // (our last message refused by the network thread after Send forgot the draft)" "the chat's frame asks whether our last message was refused"
need_order Sources/ctr/chat_overlay.cpp "        TellRefused(sink);" "        const chat::Sent sent = chat::SubmitDraft(*sink, g_osk->text(), g_osk->length());" "a refusal that came since the frame's look is told before Send (not taken into the new message's baseline)"
need_order Sources/ctr/chat_overlay.cpp "        const u32 left = sink->counters.refused_left.load(std::memory_order_relaxed);" "        const chat::Sent sent = chat::SubmitDraft(*sink, g_osk->text(), g_osk->length());" "the reasons are read before Send posts too"
need Includes/azl/chat_host.hpp "        not_in_room_ = sink.counters.refused_left.load(std::memory_order_relaxed) != left_;" "the watch tells why the message was refused"
need_after Includes/azl/chat_core.hpp "        if (!joined) {" "            counters.refused_left.fetch_add(1, std::memory_order_relaxed);" "a message refused because we were not in the room is counted apart"
# The keyboard stays open after Send (the user's request, 2026-11-08): the text is emptied for the next message, the draft forgotten, and a
# Close key (the chat's label of the Cancel key; the room's password keeps Cancel), SELECT or B with nothing typed close it. A Send within the
# second of the last one keeps the text and says so.
need_order Sources/ctr/chat_overlay.cpp "            if (g_watch.Posted(refused, left)) {" "            g_osk->Clear();" "Send empties the keyboard and leaves it open"
[ "$(tr -d '\r' < Sources/ctr/chat_overlay.cpp | sed 's://.*$::' | grep -c 'Close(false')" = "0" ] || fail "Sources/ctr/chat_overlay.cpp closes the keyboard and forgets its text somewhere (Send's close: the keyboard stays open after Send)"
need Sources/ctr/chat_overlay.cpp '    o.cancel = "Close"; ' "the chat's keyboard has a Close key"
need Includes/azl/osk.hpp '    const char* cancel = "Cancel"; ' "the keyboard's cancel key says Cancel unless the host says otherwise (the room's password)"
need Includes/azl/osk_draw.hpp "        AddText(l, v.cancel);" "the cancel key says the host's label"
need Includes/azl/osk_draw.hpp "std::memcmp(now.cancel, was.cancel, sizeof(now.cancel)) != 0) {" "another cancel label is the whole screen drawn again"
need Sources/ctr/chat_overlay.cpp 'Notify("One message a second: Send again");' "a Send within the second keeps the text and says so"
need_line Tests/osk_test.cpp "    TestClearAndClose();" "the keyboard's test empties it after Send, puts a refused text back and holds the labels of the cancel key"
need_after Includes/azl/chat_host.hpp "    bool Refused(const ChatSink& sink) {" "        if (!armed_ || sink.OutboxBusy()) {" "a message still in the box is not refused yet"
need_after Includes/azl/chat_host.hpp "        armed_ = false;" "        return sink.counters.refused.load(std::memory_order_relaxed) != refused_;" "a refusal is told once, by the counter that moved"
need_line Tests/chat_host_test.cpp "    TestPostWatch();" "the chat host's test runs the message refused after Send"
# The plugin's notifications never cover the chat (the user's request, 2026-11-08): the chat's OSD callback draws them in the corner OPPOSITE
# the chat (bottom-left -> top-right, top-left -> bottom-right and back: the one chat_pos of 3DS Link and of the in-game menu sets both), the
# chat's stack stops where theirs begins; when the callback cannot draw them (the chat off or never on, no top screen drawn on in the last
# 500 ms: a format the layer refuses, a game that does not present, an Old 3DS swap) they go to the framework's OSD::Notify as before, whole.
need Includes/azl/chat_draw.hpp "    return 3 - corner; // (top-left <-> bottom-right, top-right <-> bottom-left)" "the plugin's notifications go to the corner opposite the chat"
need Includes/azl/chat_host.hpp "    const int across = Opposite(corner);" "the callback draws the notifications in the corner opposite the chat"
need Includes/azl/chat_host.hpp "    if (notes != 0 && DrawChatLines(fb, board, notes, across, across == kBottomRight ? young : 0, kNoLimit, &limit) != 0) {" "the notifications are drawn first, and say where their stack ends"
need Includes/azl/chat_host.hpp "    if (shown != 0 && DrawChatLines(fb, v, shown, corner, corner == kBottomRight ? young : 0, limit) != 0) {" "the chat's stack stops where the notifications' begins"
need_after Includes/azl/chat_host.hpp "    if (!TopUsable(fb)) {" "        o.top_seen.store(0, std::memory_order_relaxed);" "a top screen the layer refuses is told to the plugin's thread: its notifications go to the framework"
need Includes/azl/chat_host.hpp "constexpr u32 kTopSeenMs = 500; " "a notification goes to the chat's callback only if it drew the top screen in the last half second"
need Sources/ctr/chat_overlay.cpp "    if (!g_osd_running || g_overlay.on.load(std::memory_order_relaxed) == 0) {" "with the chat off or never on the notifications are the framework's"
need Sources/ctr/chat_overlay.cpp "    if (seen == 0 || !chat::Recent(now_ms, seen, chat::kTopSeenMs)) {" "with no top screen drawn on lately the notifications are the framework's"
need_after Sources/ctr/chat_overlay.cpp "void Notify(const char* text) {" "    if (ChatNotice(text)) {" "the chat's own notifications go to the opposite corner too"
need_after Sources/main.cpp "        if (!azl::ctr::ChatNotice(message.c_str())) {" "            OSD::Notify(message);" "a notification the chat's callback cannot show is the framework's, whole"
need_line Tests/chat_host_test.cpp "    TestNotices();" "the chat host's test draws the notifications in the opposite corner of every corner, and a refused screen told"
hit=$(tr -d '\r' < Sources/ctr/chat_overlay.cpp | sed 's://.*$::' | grep -n -E '(->|\.)(Post|OutboxBusy)\(' | head -2)
[ -z "$hit" ] || fail "Sources/ctr/chat_overlay.cpp posts to the chat's box by itself (chat::SubmitDraft asks the box first: a draft is never lost): $hit"
# On the game's thread only the copy (review of the chat, O11): the OSD callback's region never sends, feeds or redraws the keyboard nor cleans
# a text (SubmitDraft and Sanitize read up to 528 bytes at every present, and a second producer of the outgoing box breaks its one-writer pact)
hit=$(tr -d '\r' < Sources/ctr/chat_overlay.cpp | awk '/^[[:space:]]*\/\/ >>> game-thread code/{f=1} f{print} /^[[:space:]]*\/\/ <<< game-thread code/{f=0}' | sed 's://.*$::' | grep -n -E 'SubmitDraft|Sanitize|TakeOutgoing|Feed\(|Redraw\(|Type\(|g_osk|g_latch' | head -2)
[ -z "$hit" ] || fail "Sources/ctr/chat_overlay.cpp: the game-thread code (the OSD callback) does the keyboard's or the box's work, only the copy belongs there: $hit"
need Tests/run.sh 'Tests/chat_host_test.cpp -Wl,--wrap=malloc -o "$OUT/chat_host_test"' "the chat host's test counts the allocations through the link"
need_line Tests/stack_usage.sh 'budget chat_overlay "azl::ctr::{anonymous}::OnChatOsd" 64' "the chat's OSD callback takes at most 64 bytes of the game's stack"
# The callback is reached by no hook entry (the framework calls it through a pointer): the call graph check follows it as a root of its own
# (no formatter below it) and adds up its deepest chain of frames against a ceiling (a helper with a buffer of its own: every frame small)
need_line Tests/callgraph_check.py "    r'^azl::ctr::\(anonymous namespace\)::OnChatOsd\(CTRPluginFramework::Screen const&\)\$'," "the call graph check follows the chat's OSD callback for formatters"
need_line Tests/callgraph_check.py "    (r'^azl::ctr::\(anonymous namespace\)::OnChatOsd\(CTRPluginFramework::Screen const&\)\$', OSD_CHAIN_CEILING)," "the chat's OSD callback's whole chain is held to a ceiling"
need_line Tests/callgraph_check.py "OSD_CHAIN_CEILING = 480" "the chain under the chat's OSD callback is at most 480 bytes of the game's stack (392 measured)"
need_line Tests/callgraph_check.py "    for p in check_frame_roots(funcs, dem, quiet):" "the report of the call graph check runs the chain ceilings"
need Tests/callgraph_mutants.py "('O15 Log with a format in a helper of the chat" "the call graph's mutants hold a formatter below the OSD callback"
need Tests/callgraph_mutants.py "('O16 a 1.5 KB buffer in a helper of the chat" "the call graph's mutants hold a big frame below the OSD callback"
# Every call graph mutant's text is found once in the tree (it is run by hand, with Docker: a stale text stopped it at the fifth mutant in 1.1.0,
# the ones after it never ran); a text not found is a mutant not caught, and the others still run
"$(command -v python >/dev/null 2>&1 && echo python || echo python3)" Tests/callgraph_mutants.py --patterns >/dev/null || fail "a mutant of Tests/callgraph_mutants.py no longer finds its text in the tree (python Tests/callgraph_mutants.py --patterns says which)"
need_after Tests/callgraph_mutants.py "            if s.count(old) != 1:" "                failed += 1" "a call graph mutant whose text is not found is counted as not caught and the others still run (no assert)"
# The input mirror page (Sources/ctr/input_mirror.cpp, the console's side; Includes/azl/hid_mirror.hpp, the pure parts that
# Tests/hid_mirror_test.cpp holds; STUDIO-CHAT sections 2, 11 and 12). While the keyboard is open the game's HID page is a neutral copy at the
# game's own address. The framework's own mapping is never switched (the plugin reads the real input there) and must be among the pages
# found, or nothing is switched. A writable shared neighbour refuses the switch (the kernel would fold the mirror into it), the mirror must
# come out as exactly one page, the address must be free after an unmap. The game's threads are held across every change by Luma3DS's
# PROCESSOP_SCHEDULE_THREADS with a predicate that reads ONE word of the kernel's thread, the TLS address Luma3DS's own swap reads (Old 3DS
# and New), and leaves out the caller and the plugin's threads; the hold is asked twice and checked against the threads of the process.
# The page goes back synchronously at the framework's events FIRST (before Plugin::NoteProcessEvent takes the hooks out at SWAP_ENTER) and
# never falls back on the mirror at SWAP_ENTER and EXIT (the mirror's page goes with the plugin's block); on a game's thread, in the hook,
# for a notification that closes the keyboard, through a try of the lock only. svc 0xA0 is called in its first, five-argument form
# (libctrpf's stub is the later form, which a Luma3DS from before it refuses). No thread, no hook, no Scheduler task of the framework.
need_line Includes/azl/hid_mirror.hpp "constexpr u32 kPadEntries = 0x28;      ///< 8 entries of 0x10: held, pressed, released, Circle Pad (s16 x, s16 y)" "the pad's entries start at 0x28"
need_line Includes/azl/hid_mirror.hpp "constexpr u32 kTouchEntries = 0xC8;    ///< 8 entries of 8: u16 x, u16 y, u32 flag at +4" "the touch's entries start at 0xC8"
need Includes/azl/hid_mirror.hpp "    std::memset(m + kTouchRaw, 0, kTouchEnd - kTouchRaw);" "the mirror's touch entries are zero, the flag too (a zero entry with the flag set is a finger at 0,0)"
need Includes/azl/hid_mirror.hpp "constexpr u32 kQuietMs = 50; " "the game gets its page back after 50 ms with nothing held"
need Includes/azl/hid_mirror.hpp "constexpr u32 kMaxWaitMs = 1000; " "... or after 1 s"
need Includes/azl/hid_mirror.hpp "constexpr u32 kReleasedSamples = 2; " "the keys the game saw held are released for 2 samples"
need Includes/azl/hid_mirror.hpp "    return r.state == kStateShared && (r.perm & kPermW) != 0;" "a writable shared neighbour refuses the switch"
# (two of the game's mappings side by side are read-only now, but their two mirrors would be one folded block: the way back would unmap
# one page of it, the kernel panic of the neighbour check)
need Includes/azl/hid_mirror.hpp "NeighbourMerges(below) || NeighbourMerges(above) || NextToAnother(f, i)) {" "two of the game's mappings side by side refuse the switch"
need Sources/ctr/input_mirror.cpp "        RestoreLocked(true, true, kWhereNotification, &ticks);" "the way back on a game's thread spins its pauses: it never sleeps in the plugin's code"
need Sources/ctr/input_mirror.cpp "    if (g_m.closing.load(std::memory_order_seq_cst) != g_want_closing || g_m.dead.load(std::memory_order_relaxed) != 0) {" "a closing counted before the switch cancels it (under the lock), and after EXIT nothing is switched"
need Includes/azl/hid_mirror.hpp "    return r.base == va && r.size == kPageBytes && r.state == kStateShared" "the mirror is exactly one page at the address"
# (the kernel's unmap of one page of a block folded with a writable shared one the game mapped beside the mirror frees from the address over
# the whole folded size: folded below, past the block and kernelpanic(); folded above, nothing done. Only an exact page is unmapped, an
# address found free gets its real page; the test's kernel unmaps as the kernel does)
need_after Includes/azl/hid_mirror.hpp "            if (!IsFreeAt(r, va)) {" "                if (!MappedExactly(r, va) || ops.UnmapMirror(va) != 0) {" "the way back unmaps only a mirror that is exactly one page, and maps the real page at a free address"
need_line Tests/hid_mirror_test.cpp "        if (!k.Query(va, &r) || r.base + r.size > va + kPageBytes) {" "the test's unmap does nothing on a block folded above, as the kernel"
need_line Tests/hid_mirror_test.cpp "            for (u32 a = va; a < va + r.size; a += kPageBytes) {" "the test's unmap frees from the address over the folded size, as the kernel"
need_line Tests/hid_mirror_test.cpp "        CHECK(o.panics == 0 && o.foreign_unmapped == 0 && o.folded_unmaps == 0);" "a block folded below or above the mirror: no kernel panic, nothing of it freed"
need_after Includes/azl/hid_mirror.hpp "            if (!ops.Query(va, &r) || !IsFreeAt(r, va)) {" "out = Outcome::kNotFree;" "the address is free after the real page is unmapped"
need_after Includes/azl/hid_mirror.hpp "                if (p == own_va) {" "f.own_seen = true;" "the framework's own mapping is never switched"
need Sources/ctr/input_mirror.cpp "    if (!f.own_seen) {" "nothing is switched when the framework's own mapping is not among the pages found"
# (the predicate reads two words of Luma3DS's own KThread, one layout for the Old and the New 3DS: +0x98, the kernel's alias of the TLS page,
# for the mark - the word Luma3DS's swap reads - and +0x94, the TLS's user address, for the caller: what getThreadLocalStorage() gives
# (TPIDRURO) is the user address, never the alias, and Luma3DS's lock holds the current thread too, so a game's thread giving the page back
# from the hook would hold itself with the whole game. Tests/hid_mirror_test.cpp runs it inside Luma3DS's loop)
need Includes/azl/hid_mirror.hpp "constexpr u32 kKThreadTlsWord = 0x26;" "the mark is read through the word of the kernel's thread that Luma3DS's swap reads"
need Includes/azl/hid_mirror.hpp "constexpr u32 kKThreadUserTlsWord = 0x25;" "the caller is told by the TLS's user address, the value getThreadLocalStorage() gives"
hit=$(tr -d '\r' < Includes/azl/hid_mirror.hpp | sed 's://.*$::' | grep -n -E 'kthread\[' | grep -v -F -e 'kthread[kKThreadTlsWord]' -e 'kthread[kKThreadUserTlsWord]' | head -2)
[ -z "$hit" ] || fail "Includes/azl/hid_mirror.hpp reads another word of the kernel's thread (only the TLS's two are known to be Luma3DS's own on both models): $hit"
hit=$(tr -d '\r' < Sources/ctr/input_mirror.cpp | sed 's://.*$::' | grep -n -E 'kthread\[' | head -2)
[ -z "$hit" ] || fail "Sources/ctr/input_mirror.cpp reads the kernel's thread itself (the predicate is azl/hid_mirror.hpp's HoldThread, the tested one): $hit"
need_after Sources/ctr/input_mirror.cpp "__attribute__((noinline, used)) bool azl_mirror_thread_predicate(const u32* kthread) {" "    return azl::hidmirror::HoldThread(kthread, azl_mirror_pred_caller_tls, azl_mirror_pred_off);" "the kernel's predicate is the tested one"
need Includes/azl/hid_mirror.hpp "__attribute__((always_inline)) inline bool HoldThread(const u32* kthread, u32 caller_tls, u32 off) {" "the predicate makes no call: it runs in the kernel"
need_after Includes/azl/hid_mirror.hpp "inline bool HoldThread(const u32* kthread, u32 caller_tls, u32 off) {" "    if (tls == 0 || kthread[kKThreadUserTlsWord] == caller_tls) {" "the hold leaves out the caller by its user TLS"
need Includes/azl/hid_mirror.hpp "    return *reinterpret_cast<const volatile u32*>(static_cast<uintptr_t>(tls)) != kThreadVarsMagic;" "the hold leaves out the plugin's threads"
need_line Tests/hid_mirror_test.cpp "    TestPredicate();" "the predicate is run inside Luma3DS's loop"
need Includes/azl/hid_mirror.hpp "        if (!FreezeSeenAll(calls, ops.Threads(), ops.Cap())) {" "the hold is checked against the threads of the process"
need Sources/ctr/input_mirror.cpp '    asm volatile("svc 0xA0"' "the mirror is mapped with svc 0xA0's first form, which every Luma3DS takes"
hit=$(grep -rn -E '\bsvcMapProcessMemoryEx[[:space:]]*\(' Sources | head -2)
[ -z "$hit" ] || fail "the plugin calls libctrpf's svcMapProcessMemoryEx (its later form: a Luma3DS from before it takes the magic number for a handle): $hit"
need Sources/ctr/input_mirror.cpp "            state = LastState(RestoreOffGameThread(!last, static_cast<u32>(event), &ticks), kMirrorLastGivenBack);" "at SWAP_ENTER and EXIT the mirror never stays"
need_line Sources/ctr/input_mirror.cpp "    const bool last = event == chat::kEventExit || event == chat::kEventSwapEnter;" "the game's last events are EXIT and SWAP_ENTER (on an Old 3DS the net for an end with no EXIT event: a process killed, a block that does not come back)"
# (the events that reach the way back are chat::IsClosingEvent's, the list the chat's counters use and Tests/chat_host_test.cpp holds:
# without SWAP_ENTER an Old 3DS's mirror page could stay mapped at the game's HID address while the plugin's block goes to the SD)
need_line Includes/azl/chat_host.hpp "    return event == kEventHomeEnter || event == kEventSwapEnter || event == kEventSleepEnter || event == kEventExit;" "the page goes back at HOME_ENTER, SWAP_ENTER, SLEEP_ENTER and EXIT"
need_after Sources/ctr/input_mirror.cpp "void MirrorProcessEvent(int event) {" "    if (!chat::IsClosingEvent(event)) {" "the mirror's way back takes the closing events from the one tested list"
# (the keep-alive thread waits for the lock at most 1 s, looking every 1 ms, while the plugin's thread stands aside - every TryLock of the
# plugin's thread looks at event_waiting first: the longest work under the lock took 620-1007 us in 56 samples on console, so a lock not
# had in 1 s is held by a thread that no longer runs. At SWAP_ENTER Luma3DS waits for its answer (10 s): an unbounded wait for a lock whose
# holder is gone would stall the console. Nothing is changed without the lock - the holder may be between the unmap and the map of the
# game's HID page - so a lock not had leaves the way back to the plugin's thread; after the game's last event (EXIT, SWAP_ENTER) the mirror
# then stays mapped, and an alias of the plugin's memory outlives the game (console 63, 2026-11-06): the shutdown's line says so. A way
# back without the lock - every other thread held, each address put right from what the kernel says - was built and dropped on 2026-11-06:
# a holder stopped between its check and its svc ran the svc afterwards, and its state was never cleared, for a case not seen on console)
need Sources/ctr/input_mirror.cpp "constexpr u64 kEventLockTicks = SYSCLOCK_ARM11;" "the keep-alive thread waits for the lock at most 1 s"
need Sources/ctr/input_mirror.cpp "constexpr s64 kEventLockStepNs = 1000000;" "the keep-alive thread looks at the lock every 1 ms"
need_after Sources/ctr/input_mirror.cpp "    const u64 deadline = svcGetSystemTick() + (left < kEventLockTicks ? left : kEventLockTicks);" "    while (!locked && svcGetSystemTick() < deadline) {" "the keep-alive thread's wait for the lock is bounded"
need_order Sources/ctr/input_mirror.cpp "    g_m.event_waiting.fetch_add(1, std::memory_order_seq_cst);" "    const u64 deadline = svcGetSystemTick() + (left < kEventLockTicks ? left : kEventLockTicks);" "the plugin's thread stands aside before the keep-alive thread starts waiting"
need_stmt Sources/ctr/input_mirror.cpp "g_m.event_waiting.fetch_sub(1, std::memory_order_seq_cst);" "the plugin's thread takes the lock again once the event is done"
hit=$(tr -d '\r' < Sources/ctr/input_mirror.cpp | sed 's://.*$::' | awk '/^[A-Za-z].*\) \{$/{fn=$0} /g_m\.lock\.TryLock\(\)/{ if (index($0, "event_waiting.load(") == 0 && index(prev, "event_waiting.load(") == 0 && fn !~ /^void Mirror(RestoreFromGame|ProcessEvent)\(/) print FNR": "$0 } {prev=$0}' | head -2)
[ -z "$hit" ] || fail "Sources/ctr/input_mirror.cpp: the plugin's thread takes the mirror's lock without looking at event_waiting (the keep-alive's wait at the game's last event would meet it): $hit"
need_after Sources/ctr/input_mirror.cpp "        // with no EXIT event, a process killed: no line)." "        g_m.want_restore.store(1, std::memory_order_release);" "a lock not had in time leaves the way back to the plugin's thread and touches nothing"
need_after Sources/ctr/input_mirror.cpp "        g_m.want_restore.store(1, std::memory_order_release);" "        state = kMirrorLastNoLock;" "a lock not had at the game's last event is said in the shutdown's line"
need_after Sources/ctr/input_mirror.cpp "    if (last) {" "        g_last.store(state, std::memory_order_release);" "what became of the mirror at the game's last event is kept for the shutdown's line"
hit=$(cat Sources/ctr/input_mirror.cpp Includes/azl/hid_mirror.hpp | tr -d '\r' | sed 's://.*$::' | grep -n -E 'Reconcile|Seize|stolen|Gone\(|pred_all' | head -2)
[ -z "$hit" ] || fail "the input mirror changes the game's pages without the lock again (the way back without the lock was dropped on 2026-11-06): $hit"
# (the shutdown's line says what became of the mirror at the game's last event)
need Sources/ctr/plugin.cpp 'MirrorLastText(MirrorLastState())' "the shutdown's line says what became of the input mirror at the game's last event"
need_line Tests/chat_host_test.cpp "    CHECK(chat::IsClosingEvent(3) && chat::IsClosingEvent(5) && chat::IsClosingEvent(1) && chat::IsClosingEvent(0));" "the chat host's test holds the closing events"
# (the framework's pause - its menu over the game, Process::Pause - holds the game's threads with the same bit of the kernel's thread as
# the mirror's hold: libctrpf's LockGameThreads is PROCESSOP_SCHEDULE_THREADS too. A way back during it, at an event of the keep-alive
# thread (the lid closed with the menu open), turns the predicate off: our let-go would wake the game under the menu. Every way back
# that is not on a game's thread goes through RestoreOffGameThread, and the flag is 0 again after it)
need_after Sources/ctr/input_mirror.cpp "Outcome RestoreOffGameThread(bool may_fall_back, u32 where, u32* ticks) {" "    azl_mirror_pred_off = CTRPluginFramework::Process::IsPaused() ? 1u : 0u;" "a way back off the game's thread under the framework's pause holds and lets go of no thread"
need_after Sources/ctr/input_mirror.cpp "    const Outcome out = RestoreLocked(false, may_fall_back, where, ticks);" "    azl_mirror_pred_off = 0;" "the predicate is on again for the switch and the game's thread"
# (the pause's count goes up before its hold and down before its let-go, and the flag is read once: a way back of the keep-alive amid the
# start of the menu's pause read it wrong - review of the chat, defect 44. The plugin's own menu gives the page back on its thread before its
# Process::Pause, with a bounded wait for the lock, and is not opened while the mirror stays; no switch is made while it is open)
need Sources/game_menu.cpp "    if (opens && may_open && MirrorBeforePause()) {" "the in-game menu opens only with the input mirror's page given back"
need_after Sources/ctr/input_mirror.cpp "bool MirrorBeforePause() {" "    const u64 deadline = svcGetSystemTick() + kPauseLockTicks;" "the page back before the menu's pause waits for the lock ..."
need Sources/ctr/input_mirror.cpp "constexpr u64 kPauseLockTicks = SYSCLOCK_ARM11 / 10;" "... at most 100 ms"
need_after Sources/ctr/input_mirror.cpp "bool MirrorBeforePause() {" '    while (!RestoreHere("the in-game menu") && svcGetSystemTick() < deadline) {' "the page goes back before the menu's pause, on the plugin's thread, until its deadline"
need_after Sources/ctr/input_mirror.cpp "        svcSleepThread(kPauseLockStepNs);" "    return g_m.active.load(std::memory_order_acquire) == 0;" "the menu is not opened while the mirror stays"
need_after Includes/azl/hid_mirror.hpp "    if (tls == 0 || kthread[kKThreadUserTlsWord] == caller_tls) {" "    if (off != 0) {" "the predicate holds no thread under the framework's pause"
hit=$(tr -d '\r' < Sources/ctr/input_mirror.cpp | sed 's://.*$::' | grep -n -E 'RestoreLocked\(false' | grep -v -F 'RestoreLocked(false, may_fall_back, where, ticks)' | head -2)
[ -z "$hit" ] || fail "Sources/ctr/input_mirror.cpp gives the page back off the game's thread without RestoreOffGameThread (the framework's pause): $hit"
need_after Sources/ctr/chat_overlay.cpp "void ChatProcessEvent(int event) {" "    MirrorProcessEvent(event);" "the page goes back first, before the plugin's swap work"
# (a switch is cancelled by a closing its frame's look did not see: the count is taken BEFORE the look and the switch compares with it under
# the lock; every event and notification counts its signal before its closing. A count taken at the wish (MirrorWant, after the look) took
# a SWAP_ENTER that came between the look and the wish as seen: on an Old 3DS the mirror engaged after SWAP_ENTER's way back and stayed
# mapped at the game's HID address while Luma3DS freed the plugin's block)
need_order Sources/ctr/chat_overlay.cpp "    MirrorBeforeLook();" "    const bool must_close = g_rules.Update(chat::Look(chat::g_signals), now_ms);" "the closings are counted before the frame's look at the signals"
n=$(tr -d '\r' < Sources/ctr/chat_overlay.cpp | sed 's://.*$::' | grep -c 'MirrorBeforeLook()')
[ "$n" = 1 ] || fail "Sources/ctr/chat_overlay.cpp counts the closings for the look $n times (once, right before the look)"
need_after Sources/ctr/input_mirror.cpp "void MirrorBeforeLook() {" "    g_look_closing = g_m.closing.load(std::memory_order_seq_cst);" "the count before the look is the closings'"
need_after Sources/ctr/input_mirror.cpp "void MirrorWant() {" "    g_want_closing = g_look_closing;" "the switch compares with the count taken before the look"
need_after Sources/ctr/chat_overlay.cpp "void ChatProcessEvent(int event) {" "    chat::NoteProcessEvent(chat::g_signals, event);" "the keep-alive counts the signal..."
need_order Sources/ctr/chat_overlay.cpp "    chat::NoteProcessEvent(chat::g_signals, event);" "    MirrorProcessEvent(event);" "... before the closing"
need_after Sources/ctr/hook.cpp "static __attribute__((noinline)) void NoteAptForChat(Result rc, u32 result_word, u32 raw) {" "    if (chat::NoteAptNotification(chat::g_signals, rc, result_word, raw)) {" "a notification counts its signal before its closing"
need_after Sources/ctr/hook.cpp "static __attribute__((noinline)) void NoteAptForChat(Result rc, u32 result_word, u32 raw) {" "        MirrorRestoreFromGame();" "a notification that closes the keyboard gives the game its page back before the game acts on it"
need_after Sources/ctr/input_mirror.cpp "void MirrorRestoreFromGame() {" "    g_m.closing.fetch_add(1, std::memory_order_seq_cst);" "a closing notification cancels a switch that is not made yet"
need_after Sources/ctr/input_mirror.cpp "    if (!g_m.lock.TryLock()) {" "        g_m.want_restore.store(1, std::memory_order_release);" "a game's thread never waits for the lock: the plugin's thread finishes"
need Sources/ctr/chat_overlay.cpp "    MirrorFrame(now_ms);" "the mirror's work runs every frame, the chat on or off"
# ... and, once nothing maps the page any more, MirrorFrame gives it back to the heap and forgets it (review of the chat, defect 56: the
# two lines dropped, 4 KB of the plugin's heap stayed taken after the first keyboard, against "only while it is needed")
need_after Sources/ctr/input_mirror.cpp "    } else if (!g_want && g_page && g_m.event_waiting.load(std::memory_order_seq_cst) == 0 && g_m.lock.TryLock()) {" "            g_m.page.store(0, std::memory_order_seq_cst);" "the mirror's page is given back when nothing maps it"
need_after Sources/ctr/input_mirror.cpp "            g_m.page.store(0, std::memory_order_seq_cst);" "            std::free(g_page);" "the mirror's page goes back to the plugin's heap when nothing maps it"
need_after Sources/ctr/input_mirror.cpp "            std::free(g_page);" "            g_page = nullptr;" "the freed page is forgotten (Engage takes a new one)"
need Sources/ctr/input_mirror.cpp "        if (CTRPluginFramework::System::IsCitra()) {" "an emulator gets the keyboard without the mirror"
hit=$(cat Sources/ctr/input_mirror.cpp Includes/azl/hid_mirror.hpp | tr -d '\r' | sed 's://.*$::' | grep -n -E 'threadCreate|svcCreateThread|Scheduler::|InstallSvcHooks|GateEnter|SwapGateScope|svcWaitSynchronization' | head -2)
[ -z "$hit" ] || fail "the input mirror makes a thread, a hook or a task, or waits on a game's thread: $hit"
need Tests/run.sh 'Tests/hid_mirror_test.cpp -o "$OUT/hid_mirror_test"' "the input mirror's test is built"
need_line Tests/stack_usage.sh 'budget input_mirror "azl_mirror_thread_predicate" 0' "the predicate of the hold runs in the kernel, on its stack: no frame"
need_line Tests/stack_usage.sh 'budget input_mirror "azl::ctr::MirrorRestoreFromGame" 24' "the input mirror's way back on a game's thread stays small"
need ConfigApp/source/pages_config.cpp "} else if (keys == KEY_X) {" "X alone gives the default chord back"
# ... and, while the mirror is the game's page, its live parts are copied again at every frame (review of the chat, I12: the call dropped, the
# indices and ticks of the pad and the touch, the accelerometer and the gyroscope stayed frozen and the "released" were never written again)
body=$(tr -d '\r' < Sources/ctr/input_mirror.cpp | awk '/^void MirrorFrame\(u32 now_ms\) \{/{f=1} f{print} f&&/^}/{exit}' | sed 's://.*$::')
printf '%s\n' "$body" | grep -F -A3 -- "    if (g_m.active.load(std::memory_order_acquire) != 0) {" | grep -F -q -- "        hidmirror::RefreshMirror(g_page, real, g_released.Next());" || fail "Sources/ctr/input_mirror.cpp: MirrorFrame no longer refreshes the mirror's live parts (RefreshMirror with the next 'released') at every frame the mirror is mapped"

# ---- the zombie probe (ctr/zombie_probe.hpp/.cpp): the MHXX-after-chat leak of a TERMINATED-but-not-finalized process -------------
# Every console-reading entry point of the probe early-returns unless the capture is on (g_debug_level>0) AND Luma3DS's custom svcs are
# there (BackdoorOk): the four are GameRefcount, ThreadSchedDump, GameHandleCounts, MemMapWalk, with the one uniform guard. The fifth,
# ForcedResLine, uses official svcs only (svcGetResourceLimit*) and is gated on the log alone, so Azahar has a reference line too.
ZGUARD="if (g_debug_level <= 0 || !BackdoorOk()"
[ "$(tr -d '\r' < Sources/ctr/zombie_probe.cpp | grep -c -F -- "$ZGUARD")" = 4 ] || fail "Sources/ctr/zombie_probe.cpp: the four backdoor probe entry points no longer all guard on 'g_debug_level<=0 || !BackdoorOk()' (a probe that runs with the log off, or on Azahar, can brick the console)"
need_after Sources/ctr/zombie_probe.cpp "void ForcedResLine(const char* when) {" "    if (g_debug_level <= 0) {" "the forced res line runs with the log on only"
# BackdoorOk is the Azahar gate: an emulator (no Luma custom svcs) or a svc that does not answer turns every backdoor read off. IsCitra()
# is tested FIRST and short-circuits: the Luma svc 0x90 is never issued on an emulator (the same order as input_mirror's Supported()).
need_after Sources/ctr/zombie_probe.cpp "    if (g_backdoor < 0) {" "        if (CTRPluginFramework::System::IsCitra()) {" "the probe's Azahar gate tests IsCitra before anything else"
need_after Sources/ctr/zombie_probe.cpp "        if (CTRPluginFramework::System::IsCitra()) {" "            g_backdoor = 0; // an emulator: the Luma svc below is never issued there" "on an emulator the backdoor is off without a Luma svc"
need Sources/ctr/zombie_probe.cpp "            g_backdoor = svcConvertVAToPA(reinterpret_cast<const void*>(0x1FF80000), false) != 0 ? 1 : 0;" "the probe's gate checks svcConvertVAToPA on a console"
need_order Sources/ctr/zombie_probe.cpp "        if (CTRPluginFramework::System::IsCitra()) {" "svcConvertVAToPA(" "IsCitra short-circuits the Luma svc"
[ "$(tr -d '\r' < Sources/ctr/zombie_probe.cpp | grep -c -F -- 'svcConvertVAToPA(')" = 1 ] || fail "Sources/ctr/zombie_probe.cpp: svcConvertVAToPA is called outside BackdoorOk's console branch"
# GameHandleCounts: GET_ALL_HANDLES with NO class token (with one, Luma calls GetClassToken on every entry without the table's lock or a
# reference: a handle closed on the other core meanwhile is a freed object), the count in use checked against the buffer first (the
# kernel does not bound the copy), each handle classified by svcGetHandleInfo 0x10001 (the lock and a reference); KServerSession 0x95,
# not KSession 0x70 (never in a handle table).
need Sources/ctr/zombie_probe.cpp "svcControlProcess(CUR_PROCESS_HANDLE, PROCESSOP_GET_ALL_HANDLES, reinterpret_cast<u32>(g_handle_buf), 0);" "GET_ALL_HANDLES is called without a class token"
[ "$(tr -d '\r' < Sources/ctr/zombie_probe.cpp | grep -c -F -- 'PROCESSOP_GET_ALL_HANDLES')" = 1 ] || fail "Sources/ctr/zombie_probe.cpp: a second GET_ALL_HANDLES call (one with a class token walks the table unlocked)"
need Sources/ctr/zombie_probe.cpp "if (R_FAILED(svcGetProcessInfo(&in_use, CUR_PROCESS_HANDLE, 4)) || in_use < 0 ||" "the handles in use are checked against the buffer before GET_ALL_HANDLES writes"
need_after Sources/ctr/zombie_probe.cpp "if (R_FAILED(svcGetProcessInfo(&in_use, CUR_PROCESS_HANDLE, 4)) || in_use < 0 ||" "        in_use > static_cast<s64>(kHandleBufWords - kHandleBufSpare)) {" "the handles in use leave the buffer room to spare"
need_order Sources/ctr/zombie_probe.cpp "svcGetProcessInfo(&in_use, CUR_PROCESS_HANDLE, 4)" "PROCESSOP_GET_ALL_HANDLES" "the count check comes before the copy"
need Sources/ctr/zombie_probe.cpp "if (R_FAILED(svcGetHandleInfo(&token, static_cast<Handle>(g_handle_buf[i]), 0x10001))) {" "each handle is classified by GetHandleInfo 0x10001"
need Includes/ctr/zombie_probe.hpp "constexpr u32 kHandleTokens[] = {0x1F, 0xB0, 0x8D, 0x39, 0x2F, 0x35, 0x98, 0x95, 0xA5};" "the classes counted (KServerSession 0x95, not KSession 0x70)"
# No newlib formatter in the probe (the keep-alive thread's 4 KB stack): one Log() format per line (printf_lite).
! tr -d '\r' < Sources/ctr/zombie_probe.cpp | grep -q -E 'snprintf|sprintf|vsnprintf' || fail "Sources/ctr/zombie_probe.cpp formats with the printf family (newlib's 1.4 KB chain on the keep-alive thread): use one Log() format"
# The mirror's counters are read as passes and let-gos (a hold is two passes), never as a 1:1 pairing.
need Sources/ctr/zombie_probe.cpp "passes == 2 * letgos ? \"yes\" : \"NO\"" "the threads line says balanced when the hold passes are twice the let-gos"
# The refcount is read with the owned objects next to it (each owns a reference to its owner): the residual is what is compared.
need Sources/ctr/zombie_probe.cpp "const s64 owned = OwnedObjects(cur, 1, kLimitCount);" "the ref line gives the owned objects and the residual"
# The memory-map walk: the header's WalkMap (PC-tested: never loops, the cap marked).
need Sources/ctr/zombie_probe.cpp "const WalkEnd end = WalkMap(0x00100000, 0x14000000, kMapLineCap, query, emit);" "the memory-map walk goes through the PC-tested WalkMap"
need Sources/ctr/zombie_probe.cpp 'Log("vm: walk truncated at 0x%08lx' "a walk cut by the cap says so"
# The handle-transfer line in the hook reads the token with a Luma ext svc only when the backdoor is there (a blind custom svc on Azahar kills the process).
need_after Sources/ctr/hook.cpp "    if (zombie::BackdoorOk()) {" "        if (R_SUCCEEDED(svcGetHandleInfo(&flags, x.handle, 0x10001))) { // 0x10001: the object's class token flags (Luma ext)" "the ipc-xfer type read is gated by the Azahar backdoor check"
# Every handle the probe opens is closed on every path (closes >= opens in the file): svcDuplicateHandle, svcOpenThread and the resource-limit handle.
ZOPENS=$(tr -d '\r' < Sources/ctr/zombie_probe.cpp | grep -c -E 'svcDuplicateHandle\(|svcOpenThread\(|svcGetResourceLimit\(')
ZCLOSES=$(tr -d '\r' < Sources/ctr/zombie_probe.cpp | grep -c 'svcCloseHandle(')
{ [ "$ZCLOSES" -ge "$ZOPENS" ] && [ "$ZOPENS" -ge 2 ]; } || fail "Sources/ctr/zombie_probe.cpp: a handle the probe opens is not closed ($ZCLOSES close(s) for $ZOPENS open(s)): a leaked reference could resurrect the object"
# The KThread layout the backdoor reads is pinned and verified against svcGetProcessorID before any mask is trusted.
# svcGetThreadList is never called (REGISTRO phase K, 2026-11-07): the kernel's GetThreadList takes a reference on the process and never gives
# it back (kernel 11.14: "they forget to decref the process!"). The chat's input mirror called it at every switch, so a game in which the
# keyboard was used stayed TERMINATED but never finalized after HOME -> Close, with all its memory, until the console restarted. The count of
# threads comes from svcGetProcessInfo type 7 (balanced). Only the diagnostic build's boot_diag.cpp (AZL_BOOT_DIAG, never delivered) keeps it.
GTL=$(grep -rln 'svcGetThreadList(' Sources Includes ConfigApp/source | grep -v '^Sources/ctr/boot_diag.cpp$' | tr '\n' ' ')
[ -z "$GTL" ] || fail "svcGetThreadList is called in: $GTL (it leaks a reference on the process: a closed game would stay in memory)"
need Sources/ctr/input_mirror.cpp "        if (svcGetProcessInfo(&n, CUR_PROCESS_HANDLE, 7) != 0) {" "the mirror counts the threads with GetProcessInfo 7"
need Sources/ctr/zombie_probe.cpp "    if (svcGetProcessInfo(&count, CUR_PROCESS_HANDLE, 7) != 0) {" "the probe counts the threads with GetProcessInfo 7"
need ConfigApp/source/zombie_census.cpp "    const int threads = R_SUCCEEDED(svcGetProcessInfo(&v, h, 7)) ? static_cast<int>(v) : -1;" "3DS Link counts the closed game's threads with GetProcessInfo 7"
need Sources/ctr/boot_diag.cpp "#ifdef AZL_BOOT_DIAG" "boot_diag.cpp (the one svcGetThreadList left) is only in the diagnostic build"
! grep -q '    GetThreadList:' ../DlpLink/rsf/3dslink.rsf || fail "DlpLink/rsf/3dslink.rsf lists GetThreadList: 3DS Link must never call it"
need ConfigApp/source/zombie_plan.hpp "    return result == 0 ? 1 : (result == 0x09401BFEu ? 0 : -1);" "the closed game's wait: the kernel's timeout is positive, not success"
# The probe runs ONLY at the events named - never on the Old-3DS SWAP path (its waits sum 8.64 s against Rosalina's 10 s and must not grow).
if tr -d '\r' < Sources/ctr/plugin.cpp | awk '/if \(event == kEventSwapEnter\) \{/{on=1} /} else if \(event == kEventSwapExit\)/{on=0} on && /zombie::/{print}' | grep -q .; then
  fail "Sources/ctr/plugin.cpp: a zombie probe runs on the SWAP_ENTER path (Old 3DS): it must run only at EXIT, HOME and keyboard/mirror events, never on the swap"
fi
need_line Sources/ctr/plugin.cpp '            zombie::ForcedResLine("EXIT");' "the forced res line is logged at EXIT"
need_line Sources/ctr/plugin.cpp '            zombie::GameRefcount("EXIT");' "the game's refcount is read at EXIT"
need_line Sources/ctr/plugin.cpp '            zombie::ThreadSchedDump("EXIT");' "the thread scheduling dump runs at EXIT"
need_line Sources/ctr/plugin.cpp '            zombie::GameHandleCounts();' "the game's held-handle counts are read at EXIT"
need_line Sources/ctr/plugin.cpp '            zombie::MemMapWalk();' "the memory-map walk runs at EXIT"
need Sources/ctr/plugin.cpp 'zombie::ForcedResLine(event == kEventHomeEnter ? "HOME_ENTER" : "HOME_EXIT");' "the forced res line is logged at HOME_ENTER and HOME_EXIT (New 3DS)"
need_order Sources/ctr/plugin.cpp 'zombie::ForcedResLine("EXIT");' 'void Plugin::OnSwapBack()' "the EXIT probes are in NoteProcessEvent, before the writer is joined in Shutdown"
# The startup-occupancy line is the ONE probe allowed with the log off: debug-independent, one comparison and one synchronous write, called from CompareMemoryWithLastLaunch before its debug-gated return.
need_after Sources/ctr/plugin.cpp "void Plugin::CompareMemoryWithLastLaunch() {" "    CheckStartupOccupancy(); // debug-independent" "the occupancy check runs even at debug=0, before the compare's debug return"
need Sources/ctr/plugin.cpp "if (File::Open(out, kLogSyncPath, File::RWC | File::APPEND | File::SYNC) == File::SUCCESS) {" "the startup occupancy warning is written synchronously, even at debug=0"
# The game's own figure comes from the PROCESS (svcGetProcessInfo type 2), never from the shared limit (which carries the zombie too: the
# comparison would cancel it), and the decision is the header's PC-tested StartupForeign (books or region); skipped on an emulator.
need Sources/ctr/plugin.cpp "    if (R_FAILED(svcGetProcessInfo(&own, CUR_PROCESS_HANDLE, 2))) {" "the startup check reads this game's own figure from its process"
need Sources/ctr/plugin.cpp "    const u32 foreign = zombie::StartupForeign(books, used, own, plugin_block);" "the startup check decides with the PC-tested StartupForeign"
need_after Sources/ctr/plugin.cpp "void Plugin::CheckStartupOccupancy() {" "    if (CTRPluginFramework::System::IsCitra()) {" "the startup check is skipped on an emulator"
need Includes/ctr/zombie_probe.hpp "    if (books > own + kStartupMargin) {" "a zombie in the books is foreign (heap_plan's ForeignInBooks)"
need Includes/ctr/zombie_probe.hpp "    if (used > own + plugin_block + kStartupMargin) {" "a zombie in the region is foreign"
if tr -d '\r' < Sources/ctr/plugin.cpp | awk '/void Plugin::CheckStartupOccupancy\(\) \{/{on=1} on&&/^}/{c++} on&&c==1{on=0} on&&/g_debug_level/{print}' | grep -q .; then
  fail "Sources/ctr/plugin.cpp: CheckStartupOccupancy looks at g_debug_level: it must run the same with the log off (it is the only debug-0 probe)"
fi
# The forced res line and the ipc-xfer line are built the game-thread-safe way (the res line is the keep-alive thread's Log, off the game threads; the ipc-xfer line is a control-lane DebugLine).
need Sources/ctr/hook.cpp "DebugLine line;" "the ipc-xfer handle-transfer line is a control-lane DebugLine (no printf on a game thread)"
need_line Tests/stack_usage.sh 'budget input_mirror "azl_mirror_thread_predicate" 0' "the predicate stays frameless (the cross-core count is read per thread at events instead)"
# Line endings: plugin.cpp keeps its CRLF, the new files are LF.
grep -q -U $'\r' Sources/ctr/plugin.cpp || fail "Sources/ctr/plugin.cpp lost its CRLF line endings"
! grep -q -U $'\r' Sources/ctr/zombie_probe.cpp || fail "Sources/ctr/zombie_probe.cpp must keep LF line endings"
! grep -q -U $'\r' Includes/ctr/zombie_probe.hpp || fail "Includes/ctr/zombie_probe.hpp must keep LF line endings"
need Tests/run.sh 'Tests/zombie_probe_test.cpp -o "$OUT/zombie_probe_test"' "the zombie probe's pure-logic test is built"
need ConfigApp/source/ui.cpp "setup::HasAllKeys(cfg)" "the app completes a file that lacks keys"
# The app is called 3DS Link (3DSLink.3dsx) but it edits the plugin's own file: the plugin and its ini keep their names
tr -d '\r' < ConfigApp/Makefile | grep -E -q '^TARGET[[:space:]]*:=[[:space:]]*3DSLink[[:space:]]*$' || fail "ConfigApp/Makefile no longer builds 3DSLink.3dsx"
need ConfigApp/source/paths.hpp 'kIniPath = "sdmc:/3ds/3DSLink/AzaharLink.ini";' "the app edits AzaharLink.ini where the plugin reads it (sd:/3ds/3DSLink, with the logs)"
need ConfigApp/source/paths.hpp 'kOldIniPath = "sdmc:/luma/plugins/AzaharLink.ini";' "the app knows the old place of the ini, to move the file"
need ConfigApp/source/settings.cpp "const LoadResult old = ReadTextFile(kOldIniPath, kMaxReadBytes, content);" "a settings file left in the old place is read when the new one is missing..."
need ConfigApp/source/settings.cpp "std::remove(kOldIniPath);" "... and moved: the old copy goes once the new file is written"
need Sources/ctr/plugin.cpp 'kConfigPath = "/3ds/3DSLink/AzaharLink.ini";' "the plugin reads its settings where the app writes them"
need Sources/ctr/plugin.cpp 'kOldConfigPath = "/luma/plugins/AzaharLink.ini";' "the plugin still reads the old place while the new file is missing (an older app)"
need Sources/ctr/plugin.cpp 'kDataDir = "/3ds/3DSLink/";' "the plugin writes its logs with the app's files"
need Sources/ctr/plugin.cpp "log_files_.reset(new LogFiles(file_layer_.get(), kDataDir, start_ms_));" "the log files are made in that folder"
need Includes/azl/log_files.hpp "u64 log_rotate_bytes = 64ull * 1024 * 1024;" "the main log grows in blocks of 64 MB"
need Includes/azl/log_files.hpp "int keep = 49;" "50 blocks a log at most (49 besides the current file)"
need Sources/azl/log_files.cpp "const int next = block_hi_[kind] + 1;" "a rotated file takes the number after the highest block: the blocks read in order and never go back"
# The list of saved rooms: servers.txt in the app's data folder (a name and an address for each: "host[:port]|name"), 10 rooms at most
need ConfigApp/source/paths.hpp 'kServersFileName = "servers.txt";' "the saved rooms live in servers.txt in the data folder"
need ConfigApp/source/settings.hpp "static constexpr std::size_t kMax = 10;" "the list of saved rooms holds 10"
need ConfigApp/source/settings.hpp "constexpr std::size_t kMaxServerNameChars = 64;" "a room's name is 64 characters at most (the name of a public room kept as a saved one: it runs by on its row when it does not fit)"
need ConfigApp/source/settings.cpp 'out += "|" + s.name;' "the file keeps a room's name after its address, after a '|'"
need ConfigApp/source/settings.cpp "const std::size_t bar = line.find('|');" "a line of the servers file is 'host[:port]' (as in the earlier builds) with an optional '|name'"
need ConfigApp/source/pages_servers.cpp "NewProbePage(room," "a room that is added or changed is tested at once"
need ConfigApp/source/pages_servers.cpp "Could not reach the room. Use it as the active room anyway?" "a room that cannot be reached is only used when the user says so"
need ConfigApp/source/pages_servers.cpp "g.servers.Replace(static_cast<std::size_t>(index), room)" "a room is changed in the same place of the list, all its four fields at once"
need ConfigApp/source/pages_servers.cpp "g.cfg.host = server.host;" "using a room puts its address in AzaharLink.ini"
need ConfigApp/source/pages_servers.cpp "g.cfg.port = server.port;" "...and its port"
need ConfigApp/source/pages_servers.cpp "g.cfg.password = server.password;" "using a room puts its own password in AzaharLink.ini"
need ConfigApp/source/fileio.cpp "return error == ENOENT ? LoadResult::Missing : LoadResult::Unreadable;" "only a missing file is created (the list of rooms is seeded, the settings file is made), one that could not be read is never written over"
need ConfigApp/source/fileio.cpp "if (failed) {" "...and a read that went wrong half way is not a file with half its text"
need ConfigApp/source/fileio.cpp "if (std::rename(path.c_str(), previous.c_str()) != 0) {" "a file is written whole or not at all: the new one is finished first, the old one taken aside, then the new one put in its place"
need ConfigApp/source/fileio.cpp "if (!f && error == ENOENT && std::rename(previous.c_str(), path.c_str()) == 0) {" "...and a write that was cut off between the two does not lose the old file"
need ConfigApp/source/settings.cpp "return WriteTextFile(kIniPath, azl::ini::Render(fields, extra, kMaxIniBytes));" "AzaharLink.ini is written by the one careful writer"
need ConfigApp/source/settings.cpp "return WriteTextFile(path, list.Serialise());" "...and so is the list of rooms"
need ConfigApp/source/appcfg.cpp "return WriteTextFile(path, SerialiseAppConfig(cfg));" "...and so is 3DSLink.cfg"
[ "$(grep -c 'fopen' ConfigApp/source/settings.cpp ConfigApp/source/appcfg.cpp | grep -v ':0$' | wc -l)" = "0" ] || fail "settings.cpp or appcfg.cpp opens a file by itself (the files of the app go through fileio.cpp)"
need ConfigApp/source/settings.cpp "c != '|'; // no blank, no control character, no bar" "a room's address has no bar (it ends the host in the file of the rooms)"
need ConfigApp/source/ui.cpp "if (settings_loaded == setup::LoadResult::Unreadable) {" "the settings are not saved over an AzaharLink.ini that could not be read"
need ConfigApp/source/system.cpp "facts.sd_writable = SdWritable(paths.datadir);" "the card is not written to for a test at every return to the main page"
need ConfigApp/source/system.cpp "now - tested_at < 60000" "...a card that could be written a moment ago is not tested again for a minute"
need ConfigApp/source/probe.cpp "if (client->StopWithin(kStopWaitMs)) {" "a room client is freed only when its network thread has left (it uses the client)"
need ConfigApp/source/copyworker.cpp "if (thread && threadJoin(thread, 5ULL * 1000000000ULL) == 0) {" "the copy thread is freed only when it has ended (it keeps its stack until then): only 0 means the thread is over, a timeout is 0x09401BFE"
# - 2026-10-21, the crash at START -> Exit (three Luma dumps): a timeout of a kernel wait is 0x09401BFE, NOT negative, so R_SUCCEEDED / R_FAILED
#   on threadJoin or svcWaitSynchronization takes a timeout for a success; the app freed the scan's state under the running worker. Only == 0 / != 0.
need ConfigApp/source/titles.cpp "if (threadJoin(impl_->thread, timeout_ms * 1000000ULL) == 0) {" "the scanner is taken for finished only when the join returned 0 (a timeout is not a finish)"
need ConfigApp/source/titles.cpp "    impl_->cancel.store(true); // (a worker that is still reading stops at its next step: the wait below is short)" "the scanner tells its worker to stop when the app closes"
need ConfigApp/source/titles.cpp "    std::atomic<bool> cancel{false};" "... with a flag the worker looks at"
[ "$(grep -c 'impl->cancel.load()' ConfigApp/source/titles.cpp)" = "3" ] || fail "ConfigApp/source/titles.cpp: the scan worker looks at its stop flag in more or fewer than the three places (after the catalog, at each game, before the health)"
if grep -rn -E 'R_(SUCCEEDED|FAILED)\(\s*(threadJoin|svcWaitSynchronization|svcWaitSynchronizationN)' ConfigApp/source ConfigApp/titleshare ConfigApp/titleshare_host Sources Includes --include=*.cpp --include=*.c --include=*.hpp --include=*.h; then
  fail "R_SUCCEEDED / R_FAILED on a kernel wait: a timeout (0x09401BFE) is not negative, so it counts as a success; compare with == 0 / != 0"
fi

# ---- the 3DS Link app: Enable titles, the start-up flow, the installation check, the Plugin Loader, the title delivery ----
# NOTHING is a fixed place: every file of the app is worked out at run time from the folder 3DSLink.3dsx was started from (argv[0]), in
# ConfigApp/source/paths.cpp. The places that are someone else's stay: sdmc:/luma/plugins (Luma3DS) and sdmc:/gm9/out (GodMode9).
need ConfigApp/source/paths.cpp "p.plugin_beside = JoinPath(appdir, kPluginFileName);" "the plugin file is next to the app, wherever the app is"
need ConfigApp/source/paths.cpp "p.plugin_in_data = JoinPath(p.datadir, kPluginFileName);" "...or in the app's own folder (where an app installed as a CIA keeps it)"
need ConfigApp/source/paths.cpp "p.datadir = SameNameNoCase(BaseName(appdir), kDataDirName) ? appdir : JoinPath(appdir, kDataDirName);" "the app's own folder is the folder it is in when that is called 3DSLink, else a folder of that name inside it"
need_after ConfigApp/source/paths.cpp "if (!argv0 || !*argv0) {" "return kDefaultAppDir;" "a launcher that says nothing (an emulator) gives the folder of the Homebrew Launcher"
need ConfigApp/source/paths.hpp 'kDefaultAppDir = "sdmc:/3ds/3DSLink";' "the folder used when the launcher does not say where the app is (an app installed as a CIA): the app's own folder"
need ConfigApp/source/paths.hpp 'kGodModeOutDir = "sdmc:/gm9/out";' "GodMode9's own folder, the one fixed place of boot9.bin"
need ConfigApp/source/paths.hpp 'kDataDirName = "3DSLink";' "what the app keeps is in the folder 3DSLink"
need ConfigApp/source/paths.hpp 'kConfigFileName = "3DSLink.cfg";' "the first start is over when 3DSLink.cfg is in the data folder"
need ConfigApp/source/paths.hpp 'kGamesCacheFileName = "titles.cache";' "the app's own cache of the titles (name and icon), in the data folder"
need ConfigApp/source/paths.hpp 'kCatalogCacheFileName = "catalog.cache";' "the cache of the names the delivery module reads, in the data folder"
need ConfigApp/titleshare/source/catalog.c '"catalog.cache"' "the module's cache is the file the app expects, in the data folder the app tells it"
need ConfigApp/source/games.hpp 'constexpr const char* kPluginFileName = "AzaharLink.3gx";' "a title's copy of the plugin keeps the plugin's own name"
# no folder of the app's own is written in the code: the only "sdmc:/3ds" is the default of an app that was not told where it is
[ "$(grep -rn '"sdmc:/3ds' ConfigApp/source ConfigApp/titleshare ConfigApp/titleshare_host --include=*.c --include=*.h --include=*.cpp --include=*.hpp | grep -v '^ConfigApp/source/paths.hpp:' | wc -l)" = "0" ] || fail "a folder under sdmc:/3ds is written in the code outside paths.hpp (everything derives from where the app was started)"
# (three: the default folder of an app that was not told where it is, and the two fixed places of the plugin's files - its folder and its ini -
# which the plugin has to find without the app)
[ "$(grep -n '= "sdmc:/3ds' ConfigApp/source/paths.hpp | wc -l)" = "3" ] || fail "paths.hpp names a place under sdmc:/3ds that it should not (only the default folder, the plugin's folder and its ini)"
need ConfigApp/source/paths.hpp 'kPluginDataDir = "sdmc:/3ds/3DSLink";' "the plugin's folder is the one place of the app's that is fixed"
if grep -rn -E '"[^"]*(TitleShare|Enable game|Game delivery|enable game)[^"]*"' ConfigApp/source ConfigApp/titleshare_host ConfigApp/titleshare/source ConfigApp/titleshare/common --include=*.c --include=*.h --include=*.cpp --include=*.hpp | grep -v 'kCatalogCache = '; then
  fail "a text of the app or of the delivery module says TitleShare (or Enable game / Game delivery): the user knows no such app"
fi
need ConfigApp/source/pages_main.cpp 'enable.label = "Enable Titles for Azahar Emulator Net";' "the page that enables titles is called 'Enable Titles for Azahar Emulator Net'"
need ConfigApp/source/pages_main.cpp 'room.label = "Azahar Emulator Room";' "the page of the room is called 'Azahar Emulator Room' (the row shows the name of the room, not only its address)"
# Luma3DS older than 13.0 has no Plugin Loader: the Azahar side of the app is shut (the switch OFF and locked, the room and the titles faint), at every start
need ConfigApp/source/ui.cpp "luma_too_old = setup::ClassifyLuma(facts.luma, facts.loader) == setup::LumaSupport::NotWork;" "the app asks at every start and every return to the main page whether Luma3DS is too old (the red verdict of the installation check)"
need ConfigApp/source/ui.cpp "if (luma_too_old && cfg.enabled) {" "with a Luma3DS that is too old 3DS Link goes OFF by itself, once"
need ConfigApp/source/pages_main.cpp "power.dim = lock != setup::SwitchLock::None;" "the 3DS Link switch is locked with a Luma3DS that is too old (and with a plugin that is not on the card)"
need ConfigApp/source/pages_main.cpp "room.dim = g.luma_too_old;" "the room row is faint with a Luma3DS that is too old"
need ConfigApp/source/pages_main.cpp "enable.dim = g.luma_too_old;" "the row of the titles for the Azahar emulator is faint with a Luma3DS that is too old"
[ "$(grep -c 'ShowToast("This Luma is not supported"' ConfigApp/source/pages_main.cpp)" = "3" ] || fail "ConfigApp/source/pages_main.cpp: A on a shut row (the switch, the room, the titles) must say, short, that this Luma is not supported (three places)"
need ConfigApp/source/pages_main.cpp 'message = "This Luma is not supported";' "the card of the main page says it short (the long sentence was too small to read)"
need ConfigApp/source/pages_main.cpp 'return ""; // (nothing between the buttons: the keys are the usual ones)' "the main page has no hint between its two buttons"
# the copies of the plugin in the games' folders are looked at once a start (an update of the app must not leave a game on the old plugin)
need ConfigApp/source/ui.cpp "worker.SubmitRefresh(tid, paths.plugin_file);" "every folder of sd:/luma/plugins gets a quiet look at its copy of the plugin once a start"
need ConfigApp/source/games.cpp "if (ReadSlot(tid).slot != Slot::On) {" "only a folder that holds our plugin and nothing else is brought up to date"
need ConfigApp/source/games.cpp "if (same_size && !FilesDiffer(GamePluginPath(tid).c_str(), source)) {" "a copy that is the plugin file is not written again"
need ConfigApp/source/pages_main.cpp 'Plugin updated in %d title%s.' "the main page says when it made copies of the plugin again"
[ "$(grep -c 'return "Azahar Emulator Room";' ConfigApp/source/pages_servers.cpp)" = "2" ] || fail "ConfigApp/source/pages_servers.cpp: the two pages of the rooms (the list and the form) are called like their row, Azahar Emulator Room"
need ConfigApp/source/pages_enable.cpp 'return "Enable Titles for Azahar Emulator Net";' "the page of the titles is called like its row"
need ConfigApp/source/pages_main.cpp 'delivery.label = "Titles Delivery";' "the page that sends and receives titles is called 'Titles Delivery'"
need ConfigApp/titleshare/source/main.c '3DS Link - Title delivery' "the module's own menu says '3DS Link - Title delivery'"
need ConfigApp/source/games.cpp '"%08lX%08lX"' "a game's folder is its title id in 16 UPPERCASE hex digits (Luma3DS looks for it by that name)"
need ConfigApp/source/games.cpp 'return static_cast<u32>(tid >> 32) == 0x00040000u;' "only application titles are games (not updates, DLC, system titles, applets, DSiWare)"
need ConfigApp/source/games.cpp 'return std::string(kPluginsDir) + "/" + TidText(tid);' "a game's plugin folder is sd:/luma/plugins/<TID>"
need ConfigApp/source/games.cpp "if (ReadSlot(tid).slot == Slot::Other) {" "a game whose folder holds another plugin is never touched"
need ConfigApp/source/games.cpp "rmdir(GamePluginDir(tid).c_str());" "disabling removes the folder too, but rmdir only takes an empty one"
need ConfigApp/source/games.cpp 'const std::string part = std::string(dst) + ".part";' "a copy is made as .part and put in place only when complete: never half a plugin"
[ "$(grep -c 'std::remove(' ConfigApp/source/games.cpp)" = "8" ] || fail "games.cpp removes a file in a place it did not (the only files the app deletes are a game's plugin copy and its .part, the .part of a failed copy, and the cache of the games when it is replaced)"
need ConfigApp/source/titles.cpp "setup::CurrentPaths().games_cache.c_str()" "the app's own cache of the titles (name and icon) is the one in the data folder"
need ConfigApp/source/titles.cpp "setup::ReadSmdhKeyed(keys, tid, buffer.data(), buffer.size()) == 0" "when the system refuses a game's icon file (a 3dsx on the consoles of TitleShare's logs) the icon is read with the user's keys"
need ConfigApp/source/titles.cpp "if (system_works < 0 && have_keys) {" "the system is not asked again once it refused and the keys are there (as TitleShare's catalog does)"
if grep -n -E 'fopen\([^)]*"[wa]|fwrite|std::remove|mkdir|rmdir|unlink' ConfigApp/source/ncch_icon.cpp; then
  fail "ncch_icon.cpp writes or removes something (it only reads a game's files)"
fi
if grep -n -E '\b(unlink|remove_all|rmdir)\b *\(' ConfigApp/source/*.cpp | grep -v 'games.cpp'; then
  fail "the app deletes something outside games.cpp (the only things it deletes are a game's plugin copy and the empty folder)"
fi
need ConfigApp/source/pages_enable.cpp "setup::CopyIsStale(info.copy_size, source_.size)" "a copy that is not the size of the plugin file is made again when the page opens"
need ConfigApp/source/pages_enable.cpp "row.label = e.name;" "a row is the game's name, never its id"
need ConfigApp/source/pages_enable.cpp "row.icon = e.game->icon;" "a row has the game's icon"
need_after ConfigApp/source/pages_enable.cpp "if (e.slot == setup::Slot::Other) {" "if (step == 0) {" "a row with another plugin in the folder cannot be switched (it only tells why)"
need ConfigApp/source/pages_enable.cpp "worker.Submit(game->tid, setup::CopyKind::Update, g.paths.plugin_file);" "a stale copy is made again in the background, like any other"
need ConfigApp/source/pages_enable.cpp "worker.Submit(e.game->tid, setup::CopyKind::Enable, g.paths.plugin_file);" "switching a title on queues its copy: the page never waits for the card"
need ConfigApp/source/pages_enable.cpp "worker.Submit(e.game->tid, setup::CopyKind::Disable, std::string());" "switching a title off queues its removal the same way"
need ConfigApp/source/copyworker.cpp "result.enable_result = EnableGame(job.tid, job.source.c_str());" "the worker thread is what makes the copies"
need ConfigApp/source/main.cpp "FinishCopies();" "the app waits for the copies still being made before it ends"
# The start-up flow. The FIRST start (no 3DSLink.cfg in the data folder) asks first and WRITES NOTHING until the answer is Yes: no data folder,
# no settings file, no cache, no check file. Yes starts the guided sequence (scan, boot9.bin, plugin file, plugin folder, the Plugin Loader
# question, the summary), and 3DSLink.cfg is written only after it: an app closed on the way asks again next time. Every other start scans in
# the background (a screen of its own only when there is no cache yet), asks about the Plugin Loader when it is off, and opens the main page.
need ConfigApp/source/main.cpp "ui::SetRootPage(pages::NewStartupPage());" "the first page is the start-up flow, not the main page"
need ConfigApp/source/main.cpp "g.first_start = setup::IsFirstStart(g.paths);" "the first start is 'no 3DSLink.cfg in the data folder'"
need_after ConfigApp/source/main.cpp "if (!g.first_start) {" "g.OpenDataFolder();" "only a start that is not the first one opens the data folder before it has asked"
need ConfigApp/source/pages_startup.cpp "g.quit = true; // (nothing was written: the first start comes again next time)" "No on the first start's question ends the app and writes nothing"
need_after ConfigApp/source/pages_startup.cpp "void Begin() {" "g.OpenDataFolder();" "the first start makes the data folder only after the answer Yes"
[ "$(grep -rn 'OpenDataFolder();' ConfigApp/source --include=*.cpp | wc -l)" = "2" ] || fail "the data folder is opened in a place that is not the start (a start that is not the first) or the first start's Yes"
need ConfigApp/source/pages_startup.cpp "const bool first = !titles::Scanner::CacheExists();" "the scan gets the whole screen only when the catalog cache is not there yet (and the first start always)"
need ConfigApp/source/pages_startup.cpp "if (g.first_start || first) {" "the first start always shows the scan, whatever cache a migration brought"
[ "$(grep -rn 'first_start_done = true' ConfigApp/source --include=*.cpp | wc -l)" = "1" ] || fail "3DSLink.cfg is marked as done in more than one place (only at the end of the first start)"
need_after ConfigApp/source/pages_startup.cpp "void Finish() {" "if (g.first_start) {" "the end of the first start (and nothing before it) writes 3DSLink.cfg"
need ConfigApp/source/ui.cpp "if (!app_cfg.first_start_done) {" "3DSLink.cfg is never written before the first start is over"
need ConfigApp/source/pages_startup.cpp "PushPage(pages::NewInstallCheckPage([this](bool go_on) { summary_result_ = go_on ? 1 : 2; }, true));" "the first start ends with the summary: the installation check, each problem can be fixed from it; START goes on, B cancels"
need ConfigApp/source/pages_check.cpp "return summary_ ? \"Continue\" : \"Save & exit\";" "the first start's summary says Continue on START (it goes on to the main page, it does not leave the app)"
need ConfigApp/source/pages_startup.cpp "setup::ShouldOfferLoader(g.loader, g.cfg.enabled && g.cfg.tunnel)" "the Plugin Loader question is asked on the first start too, before the summary"
need ConfigApp/source/titles.cpp "catalog_build(&impl->ctx, &impl->catalog, OnProgress)" "the scan is the module's own catalog_build, with a progress count"
# The module's log is opened by its own sender and receiver, and by 3DS Link Delivery (the Download Play host has no module code of its own) through the bridge's
# dl_log_open / dl_log_close, which only the page of 3DS Link Delivery calls: no other file of the app, and no other line of the bridge, opens it.
if grep -rn -E 'ts_log_open|ts_log_close' ConfigApp/source; then
  fail "the app opens the delivery module's log (only its own sender and receiver, and the bridge's dl_log_open for 3DS Link Delivery, open it)"
fi
if grep -rn -E 'ts_log_open|ts_log_close' ConfigApp/titleshare_host | grep -v -E 'delivery\.c:[0-9]+:    ts_log_(open\(role\)|close\(\));$'; then
  fail "the bridge opens the delivery module's log somewhere other than dl_log_open / dl_log_close"
fi
if grep -rn -E 'dl_log_open|dl_log_close' ConfigApp/source | grep -v -E '^ConfigApp/source/pages_dlp\.cpp:'; then
  fail "the delivery module's log is opened from a place other than the page of 3DS Link Delivery"
fi
need ConfigApp/titleshare_host/delivery.c "void dl_log_open(const char* role) {" "the bridge opens the module's log for 3DS Link Delivery"
# The folders and the key file come from the app, by setters: the module has no place of its own (ConfigApp/titleshare_host/hostcfg.h)
need ConfigApp/source/module_config.cpp "hc_set_data_dir(paths.datadir.c_str());" "the module's data folder is the app's"
need ConfigApp/source/module_config.cpp "hc_set_boot9_path(boot9.found ? boot9.path.c_str() : \"\");" "the module's key file is the one the app found"
need ConfigApp/source/module_config.cpp "hc_set_log_enabled(cfg.debug > 0 ? 1 : 0);" "the module's log switch IS the Debug log switch (ONE switch for every log)"
need ConfigApp/titleshare/source/ctx.c "const char* path = hc_boot9_path();" "the module loads the boot9.bin the app found, from no place of its own"
need ConfigApp/titleshare/source/receiver.c "return hc_data_path(out, n, \"park\");" "the module's temporary files are in the app's data folder"
need ConfigApp/source/pages_config.cpp "g.ApplyModuleConfig(); // (title delivery follows the same switch)" "flipping the Debug log flips the delivery's log"
# The delivery's log: no file at all unless the switch is on, a name of its own, 64 MiB a file, 8 files, appended to
need ConfigApp/titleshare/source/log.c "if(!hc_log_enabled() || hc_data_dir()[0] == 0) {" "the delivery module creates no log file when the Debug log is off"
need_after ConfigApp/titleshare/source/log.c "if(!hc_log_enabled() || hc_data_dir()[0] == 0) {" "return;" "...and returns before it opens a file or starts a thread"
[ "$(grep -c 'fopen(' ConfigApp/titleshare/source/log.c)" = "1" ] || fail "ConfigApp/titleshare/source/log.c opens a file in more than one place (only log_open_current, behind the switch)"
[ "$(grep -c 'log_open_current();' ConfigApp/titleshare/source/log.c)" = "2" ] || fail "log_open_current is called from a place that is not the opening of the log (behind the switch) or a rotation"
need ConfigApp/titleshare/source/log.c 'fopen(path, "a")' "the delivery log is appended to, like the plugin's capture"
if grep -n 'log-prev\|log\.txt' ConfigApp/titleshare/source/log.c; then
  fail "the delivery log renames or writes the old log.txt again"
fi
need ConfigApp/titleshare_host/logrot.h "#define LOGROT_MAX_BYTES (64ull * 1024 * 1024)" "a delivery log file rotates at 64 MiB, like the plugin's capture"
need ConfigApp/titleshare_host/logrot.h "#define LOGROT_KEEP 7" "8 files at most: the current one and .1.log ... .7.log"
need ConfigApp/titleshare_host/logrot.c '"%s/3DSLink-TitleDelivery.log"' "the delivery log's name"
need ConfigApp/titleshare_host/logrot.c '"%s/3DSLink-TitleDelivery.%d.log"' "the older files of the delivery log"
need ConfigApp/titleshare/source/log.c "if(logrot_due(gBytes)) {" "the log rotates when a file is due"
# The two values 3DS Link carries and does not read from the console (the key generator's constant, the IV of the Download Play checksum) are Azahar's own
# defaults, obfuscated in builtin_keys_blob.h and read by builtin_keys.c; no source writes either of them inline, and the library gets the IV from its caller
need ConfigApp/titleshare_host/builtin_keys.c 'uint8_t key[16] = {0}, ctr[16] = {0};' "the built-in blob is opened with a key of zeros and a counter of zeros, like Azahar's"
need ConfigApp/titleshare_host/builtin_keys.c 'aes_ctr_crypt(rk, ctr, 0, text, sizeof(bk_blob));' "the built-in blob is opened with the AES the app already has"
[ "$(tr -d '\r' < ConfigApp/titleshare/common/sdcrypt.c | grep -c -F 'if(bk_get(BK_GENERATOR_CONSTANT, cbytes) != 0) {')" = 2 ] || fail "ConfigApp/titleshare/common/sdcrypt.c: the SD key and the NCCH key must both take the key generator's constant from the built-in values"
if grep -rn -E 'static const u128 C |DLP_IV\[' ConfigApp --include=*.c --include=*.h --include=*.cpp --include=*.hpp; then
  fail "a source carries the key generator's constant or the Download Play IV inline: they are only in builtin_keys_blob.h, obfuscated"
fi
need ConfigApp/source/pages_dlp.cpp 'bk_get(BK_DLP_CHECKSUM_IV, cfg.iv);' "the app gives the Download Play library the IV of its checksum"
need ConfigApp/dlph/dlph.c 'ctr[i] = G.cfg.iv[i] ^ G.cfg.mac[i % 6];' "the checksum's counter is the caller's IV xor the MAC"
need ConfigApp/dlph/dlph.c 'L("no checksum IV in the configuration (cfg.iv)"); return false;' "the library refuses to open without an IV"
# The Download Play child carries the descriptor makerom makes itself with the wildcard programId (tried on a console, 2026-10-23): no file of Nintendo's is needed by default
need ../DlpLink/tools/build_all.sh 'DLP_AD="${DLP_AD:-wild}" bash DlpLink/tools/build_cias.sh' "the checks and the reference child are made with the descriptor of makerom by default"
need ../DlpLink/tools/make_dlp_child.sh 'AD_ARG=wild; if [ "${DLP_AD:-wild}" = file ]' "the reference child takes the descriptor of makerom unless a file is asked for"
need ../DlpLink/tools/make_child_model.sh 'AD_ARG=wild; if [ "${DLP_AD:-wild}" = file ]' "the model of the child takes the descriptor of makerom unless a file is asked for"
need ../DlpLink/tools/finalize_child.py 'struct.pack_into("<Q", ad, 0x200, 0x00040001FFFFFFFF)' "the descriptor of makerom gets the wildcard programId"
# The Plugin Loader: the commands were checked against Luma3DS's sysmodules/rosalina/source/plugin/plgloader.c (the server), plgldr.c (its
# client) and CTRPluginFramework's plgldr.c: port "plg:ldr", command 2 = is it enabled (answer: result in word 1, state in word 2), command 3 =
# enable / disable (word 1 = state; answer: result in word 1). The app sends those two and nothing else, and asks only after a Yes.
need ConfigApp/source/system.cpp '#define AZL_PLG_PORT "plg:ldr"' "the Plugin Loader's port"
need ConfigApp/source/system.cpp "cmdbuf[0] = IPC_MakeHeader(2, 0, 0);" "plg:ldr command 2 (is the loader enabled?), as Luma3DS's plgloader.c checks it"
need ConfigApp/source/system.cpp "cmdbuf[0] = IPC_MakeHeader(3, 1, 0);" "plg:ldr command 3 (enable / disable), as Luma3DS's plgloader.c checks it"
need ConfigApp/source/system.cpp "state = cmdbuf[2] != 0 ? LoaderState::Enabled : LoaderState::Disabled;" "the state of the loader is word 2 of the answer to command 2"
need ConfigApp/source/system.cpp "cmdbuf[1] = enabled ? 1u : 0u;" "the new state of the loader is word 1 of command 3"
[ "$(grep -c 'cmdbuf\[0\] = IPC_MakeHeader(' ConfigApp/source/system.cpp)" = "2" ] || fail "system.cpp sends a request that is not one of the two it knows (plg:ldr 2 and 3): a command that was not verified must never be sent"
# The app never asks the launcher it was started from to run another program (the game delivery is TitleShare inside the app): the one
# service it connects to by name is the Plugin Loader, in its two places
[ "$(grep -rn 'svcConnectToPort(' ConfigApp/source ConfigApp/titleshare_host | wc -l)" = "2" ] || fail "the app connects to a service by name in a place it did not (only plg:ldr: reading and switching the Plugin Loader)"
if grep -rn 'hb:ldr' ConfigApp/source ConfigApp/titleshare_host; then
  fail "the app talks to hb:ldr again (the game delivery runs TitleShare inside the app; no other program is started)"
fi
[ "$(grep -rn 'SetPluginLoader(' ConfigApp/source --include=*.cpp | grep -v 'ConfigApp/source/system.cpp' | wc -l)" = "3" ] || fail "the Plugin Loader is switched in a place it was not (the question after a Yes, A on the Installation check's row, and the build for trying it on an emulator)"
need ConfigApp/source/paths.cpp 'lower.find("boot9") != std::string::npos && lower.find("boot11") == std::string::npos;' "a boot9 dump is any .bin with boot9 in its name (GodMode9 2.x: <serial>_boot9_00.bin), and the boot11 dump of the same size is never taken for it"
need ConfigApp/source/paths.cpp "const std::string hit = FindBoot9InFolder(dir);" "when no file is called boot9.bin the places the app looks in by itself are looked into for such a name"
need ConfigApp/source/pages_check.cpp "return name.size() > 4 && setup::SameNameNoCase(name.substr(name.size() - 4), \".bin\");" "the search of the card looks at every .bin: the content (or the name GodMode9 gave it) says which is boot9"
need ConfigApp/source/pages_check.cpp "setup::FindBoot9InFolder(folder)" "so does the choice of a folder"
need ConfigApp/source/search.cpp "options_.name_match ? options_.name_match(name) : Lower(name) == file_name_" "the search can test a name instead of comparing it with one"
# The fifth round of the app (the user's fourth list): the tags that touch, the sender's screen, the receivers' names, a boot9 told by its content
need ConfigApp/source/ui.cpp "x = std::floor(x - 9.0f);" "the coloured bars of the kinds (game, update, DLC) are on whole pixels (one drawn between two pixels looked a pixel off its neighbour)"
need ConfigApp/source/ui.cpp "x = std::floor(x - 13.0f);" "the bars of the legend are on whole pixels too"
need ConfigApp/source/ui.cpp "float DrawKindMarks(float right_edge, float yc, unsigned tags) {" "the kind of a title is ONE small coloured bar with no letter, everywhere (a list, the offer, the receiving list)"
! grep -rn "DrawKindTags" ConfigApp/source || fail "the plates with the letters G U D are back (DrawKindTags): the kinds are small coloured bars with no letter everywhere"
! grep -n '{kTagGame, "G"' ConfigApp/source/ui.cpp || fail "a letter G in a plate of the kinds is back"
need ConfigApp/source/delivery_run.cpp "constexpr int kShownGroups = 3;" "the sending screen has room for three lines of what is offered"
need ConfigApp/source/delivery_run.cpp "first = Clamp(at - 1, 0, total_groups - kShownGroups);" "...and they are always the ones around the title that is being sent (its green dot never goes out of sight)"
need ConfigApp/source/delivery_run.cpp 'Fmt("sending speed: %.2f MiB/s", s_.mibps)' "the room card says how fast the radio sends (not the name of the title)"
need ConfigApp/source/delivery_eta.cpp "double n = size[t] * (1.0 - Clamp01(r.down[t]));" "the time left counts, title by title, what each receiver still lacks of it (a console that joins and wants a title the others have got makes it count again)"
need ConfigApp/source/delivery_run.cpp 'DrawPair(x, y3 + 53, 0.4f, "to go",' "the details of a receiver say how many titles it asked for, has downloaded, has installed and has still to go"
need ConfigApp/source/delivery_run.cpp '"SD free"' "...and the space it has on its card"
! grep -q "DrawNowLine" ConfigApp/source/delivery_run.cpp || fail "the details of a receiver name the titles again (there can be twenty: it gets numbers, not names)"
need ConfigApp/source/delivery_run.cpp 'DrawPopupBox(box, "Leave the room?", kLeaveText);' "B on the receiving screen asks before leaving the room"
need ConfigApp/source/delivery_run.cpp 'const std::string completed = "completed: " + Percent(static_cast<int>(s.sender_next));' "the share of the whole operation is called what it is"
! grep -q '"You: ' ConfigApp/source/delivery_run.cpp || fail "the receiving screen lists this console among the others (it is the card above)"
need ConfigApp/titleshare_host/delivery.c "node_name(st->otherNode[i], s.other_name[i], sizeof(s.other_name[i]), !st->reconnecting);" "the names of the other receivers are the names their consoles have on the wireless"
need ConfigApp/titleshare/common/proto.c "if(n >= base + 1 + (size_t) c * 3 && in[base] == (uint8_t) c) {" "the roster's trailer is read only when it is there and its count matches (an older sender sends none)"
need ConfigApp/titleshare/common/proto.c "t[0] = (uint8_t) r->n;" "...and written after the entries the frame always had"
need ConfigApp/titleshare/source/sender.c "ro.node[ro.n] = st.cNode[ro.n];" "the sender says which node each receiver speaks from"
need ConfigApp/titleshare/source/receiver.c "n->otherState[n->nOthers] = ro.ext ? ro.state[i] : 255;" "a receiver that gets no trailer shows no state (255)"
need ConfigApp/source/paths.cpp '{0x10000, "2F88744FEED717856386400A44BBA4B9CA62E76A32C715D4F309C399BF28166F", Boot9Verdict::Good},' "the bootrom of a retail console is known by its SHA-256, whatever the file is called"
need ConfigApp/source/paths.cpp '{0x8000, "DF95ED5B358ED93F2738BAE9E0FC2E4EF1865A62ABA9FCC54A4E0898301F97B4", Boot9Verdict::NoKeys},' "...the lower half alone, which has the size of a boot9 and no keys, is known too"
need ConfigApp/source/paths.cpp "return FileSizeOf(path, size) && ClassifyBoot9File(path) != Boot9Verdict::NoKeys;" "a file that is the lower half of the bootrom is no boot9, even when it is called boot9.bin"
need ConfigApp/source/pages_check.cpp "setup::ClassifyBoot9File(path) == setup::Boot9Verdict::Good" "the search of the card takes a file by its content too"
need ConfigApp/source/delivery_run.cpp 'Choosing it replaces the installed copy: the title and its data are deleted first.' "what the top screen says of an installed title is what choosing it does, not that it is installed (the list says that)"
need ConfigApp/source/pages_check.cpp "const Result rc = setup::SetPluginLoader(true);" "A on the Installation check's Plugin Loader row switches it on, through the service"
need ConfigApp/source/pages_check.cpp "return row && row->kind == Kind::Link ? \"Fix\" : \"\";" "a line that is fine has no A button (nothing to fix)"
need_after ConfigApp/source/main.cpp "#ifdef AZL_TEST_DISABLE_LOADER" "setup::SetPluginLoader(false);" "the only other switch of the loader is a build made to try the question"
need ConfigApp/source/pages_startup.cpp "setup::ShouldOfferLoader(g.loader, g.cfg.enabled && g.cfg.tunnel)" "the question is asked only for a loader that is known to be off, with 3DS Link on"
need ConfigApp/source/pages_startup.cpp "const Result rc = setup::SetPluginLoader(true);" "Yes switches the loader on directly, through the service"
need ConfigApp/source/status.cpp "return state == LoaderState::Disabled && link_on;" "an unknown state never asks"
# The installation check: the plugin file (searched on the whole card when it is not next to the app), the plugin folder (made on A), the card, and
# boot9.bin (a problem the main page names, found by a search, a folder or a how-to), and nothing about the settings file, which the app makes
# by itself. default.3gx is information only: 3DS Link never uses it, and the project no longer offers one.
need ConfigApp/source/status.cpp '"boot9.bin missing"' "boot9.bin missing is a problem the main page names, with where to go"
need ConfigApp/source/status.cpp 'MakeItem("boot9.bin", "missing", CheckLevel::Warning,' "a missing boot9.bin is a WARNING, not a problem: 3DS Link works without it (only sending titles needs it); the main page and the summary name it in yellow"
need ConfigApp/source/status.cpp "default.3gx would apply to every title; 3DS Link does not use it" "default.3gx is only told about"
need ConfigApp/source/status.cpp 'CheckAction::RestorePlugin, "plugin file missing"' "a missing plugin file is fixed by putting back the plugin that is inside the app"
need ConfigApp/source/status.cpp 'CheckAction::CreatePluginsDir, "plugin folder missing"' "a missing plugin folder is made on A"
need ConfigApp/source/status.cpp 'CheckAction::FindBoot9, "boot9.bin missing"' "a missing boot9.bin is fixed by the three ways"
need ConfigApp/source/status.cpp 'constexpr const char* kWhereToGo = " - A on Installation check";' "the main page says where to go"
if grep -n 'Settings file\|ini_present\|ini_all_keys' ConfigApp/source/status.cpp ConfigApp/source/status.hpp ConfigApp/source/system.cpp; then
  fail "the installation check has a settings-file row again (the app makes AzaharLink.ini by itself)"
fi
[ ! -e dist/default.3gx ] || fail "dist/default.3gx is back (3DS Link does not use a default plugin: a plugin is enabled per title)"
need ConfigApp/source/paths.cpp "for (const std::string& place : {paths.boot9_beside, paths.boot9_in_data, paths.boot9_godmode, saved_path}) {" "boot9.bin is looked for next to the app, in the app's own folder, in GodMode9's folder, then where the user pointed to"
need ConfigApp/source/paths.cpp "list.push_back(place);" "...each place once"
need ConfigApp/source/paths.cpp "return size == 0x10000 || size == 0x8000;" "a boot9.bin is 64 KB or 32 KB"
need ConfigApp/source/ui.cpp "boot9 = setup::FindBoot9(paths, app_cfg.boot9_path);" "the key file is looked for where the app is, not in a fixed place"
need ConfigApp/source/pages_check.cpp '{"Search the whole SD card", "Choose a folder", "How to get it"}' "the popup of a missing boot9.bin has its three choices"
need ConfigApp/source/pages_check.cpp "if (setup::MakeDirs(setup::kPluginsDir)) {" "A on a missing plugin folder makes it"
need ConfigApp/source/search.cpp 'lower == "nintendo 3ds"' "the search never goes into the console's own Nintendo 3DS folder"
need ConfigApp/source/search.cpp "queue_.emplace_back(JoinPath(dir_path_, name), dir_depth_ + 1);" "the search goes through the folders breadth first"
need ConfigApp/source/pages_enable.cpp "g.paths.plugin_file" "Enable Titles copies the plugin from the file the app found (next to it, or in its folder)"
[ "$(grep -c 'g.paths.plugin_file' ConfigApp/source/pages_enable.cpp)" -ge "3" ] || fail "pages_enable.cpp looks for the plugin file in a place that is not the app's"
[ ! -d ConfigApp/source/files ] && ! grep -rn 'FilesPage\|NewFilesPage' ConfigApp/source || fail "the Files page is back (the Configuration page has no Files row)"
# The delivery module is the original plus the documented patches (ConfigApp/titleshare/PATCHES.md): only a path, a text the user sees and the log
# layer are changed, Tests/titleshare_vendor_check.sh proves it (the copy is MANIFEST.md5, the copy minus vendor.patch is ORIGINALS.md5, the
# originals in ../TitleShare plus the patch are the copy), and the app builds all of it, listed by name, and runs the module's own main()
if ! VENDOR_OUT=$(bash Tests/titleshare_vendor_check.sh 2>&1); then
  echo "$VENDOR_OUT"
  fail "the copy of the delivery module in ConfigApp/titleshare is not the original plus its documented patches (Tests/titleshare_vendor_check.sh)"
fi
need Tests/titleshare_vendor_check.sh 'ALLOWED="common/proto.c common/proto.h common/sdcrypt.c source/catalog.c source/ctx.c source/log.c source/log.h source/main.c source/net.h source/power.c source/receiver.c source/receiver.h source/sender.c source/sender.h"' "only these fourteen files of the module may be patched (the last six on the user's requests, 2026-10-16 and 2026-10-17)"
need Tests/titleshare_vendor_check.sh "patch --binary -R -p1" "the proof applies the patch backwards to the copy and compares the md5 of the originals"
need Tests/titleshare_vendor_check.sh 'cmp -s "$TMP/$f" "$DIR/$f"' "where the originals are, original plus patch is compared with the copy byte for byte"
need ConfigApp/titleshare/PATCHES.md "the thirteen patches" "the patches are documented"
need_line ConfigApp/Makefile $'TS_COMMON\t:=\taes catcache cia_front ciastream contentpath flow ncchname pace proto room runcache sched sdcrypt seed sha256 store win' "the module files the app builds, by name (all of common/)"
need_line ConfigApp/Makefile $'TS_SOURCE\t:=\tamsink catalog ctx log power receiver sender' "the module files the app builds, by name (all of source/ but main.c: the module's own text screens are not built)"
need_line ConfigApp/Makefile $'TS_HOST\t\t:=\tdelivery hostcfg logrot builtin_keys' "the bridge to the module (the one file that includes its headers for the app), with what the app tells the module, the log's rotation and the two built-in values (builtin_keys)"
need_line ConfigApp/Makefile $'SOURCES\t\t:=\tsource ../Sources/azl ../Sources/enet' "TitleShare's folders are not scanned for sources: its files are built from the list only"

# 3DS Link Delivery: this console as a Download Play host (pages_dlp.cpp, over the library in ConfigApp/dlph, which is DlpLink/host unchanged:
# Tests/dlph_vendor_check.sh proves it against ConfigApp/dlph/MANIFEST.md5). What the entry promises is pinned here, because a slip in any of it
# shows only on two consoles: the room of two made again after every console, the pause between fragments the hardware gave, ONE thread that makes
# every call into the library, the thread waited for in a way that cannot read a timeout as success, sleep and HOME off while the room is up, the
# question before cutting a console off, and a menu row that does not depend on the Plugin Loader.
if ! DLPH_OUT=$(bash Tests/dlph_vendor_check.sh 2>&1); then
  echo "$DLPH_OUT"
  fail "the copy of the Download Play host library in ConfigApp/dlph is not DlpLink/host unchanged (Tests/dlph_vendor_check.sh)"
fi
need Tests/dlph_vendor_check.sh 'SOURCES="dlph.c dlph.h dlph_wire.c dlph_wire.h"' "the library is these four files and the app builds exactly them"
need_line ConfigApp/Makefile $'DLPH\t\t:=\tdlph dlph_wire' "the library files the app builds, by name"
need ConfigApp/Makefile '$(DLPH_OFILES) : %.o : $(TOPDIR)/dlph/%.c' "the library is compiled from its own folder, with the module's flags"
need ConfigApp/Makefile 'export OFILES := $(OFILES_BIN) $(OFILES_SOURCES) $(TS_OFILES) $(DLPH_OFILES)' "the library is linked"
need ConfigApp/source/pages_dlp.cpp "cfg.max_nodes = 2; " "3DS Link Delivery is a room of two places: this console and ONE seat"
need ConfigApp/source/pages_dlp.cpp "cfg.persistent = false;" "the room is made again after every console (a used seat is dirty: the next console stays half-connected for 44-49 s)"
need ConfigApp/source/pages_dlp.cpp "constexpr int kGapUs = 3000;" "the pause between two fragments is the one measured on hardware (no pause is slower: 17 s against 10 s, 2.4 times the bytes)"
need ConfigApp/source/pages_dlp.cpp "cfg.gap_us = kGapUs;" "the library is given that pause"
need ConfigApp/source/pages_dlp.cpp "constexpr u32 kRadioStack = 128 * 1024;" "the thread of the library has a stack of its own"
need ConfigApp/source/pages_dlp.cpp "prio > 0x19 ? prio - 1 : prio" "the radio thread is a little more important than the screen (a frame must not make it miss its rhythm)"
need ConfigApp/source/pages_dlp.cpp "threadJoin(radio_->thread, U64_MAX);" "the thread is waited for with no timeout (a timeout reads as success on the 3DS)"
need ConfigApp/source/pages_dlp.cpp "svcSleepThread(10ULL * 1000000ULL);" "dlph_poll does not wait when idle: the thread sleeps between two calls (it needs at least 10 a second)"
need ConfigApp/source/pages_dlp.cpp "dlph_request_kick();" "the user can send the console in the room away"
need ConfigApp/source/pages_dlp.cpp "dl_power_begin();" "sleep is refused and HOME is off while the room is up (a console asleep drops the room)"
need ConfigApp/source/pages_dlp.cpp "dl_power_end();" "...and given back when it is closed"
need ConfigApp/source/pages_dlp.cpp "g.HoldHome();" "HOME is off while the room is up, and the bar says so"
need ConfigApp/source/pages_dlp.cpp "g.ReleaseHome();" "...and given back when it is closed"
need ConfigApp/source/pages_dlp.cpp "wireless_.Close(); " "the local wireless goes back to the app when the room is closed"
need ConfigApp/source/pages_dlp.cpp "dlp::CloseNeedsQuestion(stats_.seat)" "closing the room asks first when a console is in it (and only then)"
need ConfigApp/source/pages_dlp.cpp "if (dl_screens_off()) {" "with the backlights out only SELECT counts"
need ConfigApp/source/pages_dlp.cpp "std::memcmp(soc, shared.data(), 6) != 0" "the two sources of this console's address are compared and the log says when they differ"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kCloseQuestion = "A console that has not finished will stop receiving.";' "the question before closing the room"
need ConfigApp/source/dlp_text.hpp 'return "Had it already"; // it said it has this version: nothing was sent' "a console that already had the app is never reported as delivered"
need ConfigApp/source/paths.cpp "p.dlp_child = JoinPath(p.dlp_dir, kDlpChildFileName);" "the package that is handed out is <data folder>/dlp/child.cia"
need ConfigApp/source/delivery.hpp "ui::Page* NewDlpHostPage();" "the page of 3DS Link Delivery is made through delivery.hpp"
need ConfigApp/source/pages_main.cpp "dlp.label = \"3DS Link Delivery\";" "the main page has the entry 3DS Link Delivery"
need ConfigApp/source/pages_main.cpp "PushPage(pages::NewDlpHostPage());" "...and A opens its page"
[ "$(tr -d '\r' < ConfigApp/source/pages_main.cpp | grep -c '\.gap')" = "2" ] || fail "ConfigApp/source/pages_main.cpp puts a gap above 3DS Link Delivery (the user wants none: with Titles Delivery it is one service in two parts)"   # (exactly the two there are: above Titles Delivery and above Settings)
need ConfigApp/source/pages_main.cpp "enum Id { kPower = 1, kRoom, kEnableGame, kGameDelivery, kDlpDelivery, kSettings };" "the entry is between Titles Delivery and Settings"
[ "$(grep -c 'dlp\.dim.*luma_too_old' ConfigApp/source/pages_main.cpp)" = "0" ] || fail "the 3DS Link Delivery row is shut when Luma is too old (like Titles Delivery and Settings it does not need the Plugin Loader; it is shut by its own switch and by a missing boot9.bin, never by this)"
need ConfigApp/source/main.cpp "if (!dl_skip_drawing() && gfx::BeginFrame()) {" "a frame is not drawn with the backlights out or the shell closed while a room is up"
# only the thread of the page calls into the library; the page reads the snapshot and asks for a kick from the screen's thread
[ "$(grep -rn -E 'dlph_(open|start|poll|stop|close)\(' ConfigApp/source | wc -l)" = "5" ] || fail "the library's open / start / poll / stop / close are called from more or fewer places than the thread of the page (RadioMain)"
[ "$(grep -rn -E 'dlph_(open|start|poll|stop|close)\(' ConfigApp/source | grep -v '^ConfigApp/source/pages_dlp\.cpp:' | wc -l)" = "0" ] || fail "the Download Play host library is called from outside the page of 3DS Link Delivery"
need_after ConfigApp/source/pages_dlp.cpp "        dlph_stop(); // (before the wireless is closed" "}" "the room is destroyed by the thread, before the page closes the wireless"
if tr -d '\r' < ConfigApp/Makefile | grep -n -E 'wildcard.*titleshare'; then
  fail "ConfigApp/Makefile builds TitleShare with a wildcard (its files are built by name)"
fi
if tr -d '\r' < ConfigApp/Makefile | grep -n -E -e '-Dmain|-Dmain=|-D *main='; then
  fail "ConfigApp/Makefile renames a main with a flag (the host file renames TitleShare's, for that file only)"
fi
# Titles Delivery: the module's own text screens (its main.c) are NOT built; the app draws the screens and reaches the module through the one bridge,
# ConfigApp/titleshare_host/delivery.c (the one file that includes the module's headers for the app). What the module does is called as it is.
need ConfigApp/titleshare_host/delivery.c "return sender_run(gCtx, &gMan, send_adapter, NULL);" "sending is the module's own sender_run"
need ConfigApp/titleshare_host/delivery.c "return receiver_run(gCtx, &gPicked, recv_adapter, NULL);" "receiving is the module's own receiver_run, in the room that was picked"
need ConfigApp/titleshare_host/delivery.c "sender_check_title(gCtx, gCat->items[item].tid, &t, why, sizeof(why))" "a title is checked by the module before it is offered"
need ConfigApp/titleshare_host/delivery.c "udsInit(TS_SHARED_MEM, NULL)" "the wireless is opened as the module opened it"
need ConfigApp/titleshare_host/delivery.c "return answer == DL_CANCEL && st->phase <= 1 ? TS_UI_CANCEL : TS_UI_NONE;" "the room can be closed in its first two phases only, as in the module"
if grep -rn 'main_host\|titleshare_main\|titleshare_reset_state' ConfigApp/source ConfigApp/titleshare_host ConfigApp/Makefile; then
  fail "the module's own main() is run again (the app draws the Titles Delivery screens itself)"
fi
[ "$(grep -rn -E '#include +["<][^">]*\.c[">]' ConfigApp/source ConfigApp/titleshare_host | wc -l)" = "0" ] || fail "a .c file is included where it was not (no file includes another one)"
need ConfigApp/source/delivery.cpp "session::SuspendNetwork();" "the sockets and the network status are put away while the module has the local wireless (it found a console with none of them open)"
need ConfigApp/source/delivery.cpp "dl_power_exit();" "what the module held (the backlights, the apt hook) is given back when the wireless is closed"
need ConfigApp/source/delivery.cpp "threadCreate(Trampoline," "the module's loops call the screens from a thread with a big stack"
need ConfigApp/source/delivery_run.cpp "RunOnBigStack([&]() { rc = dl_send_run(&SendTick, &run); });" "the room runs on the big stack"
need ConfigApp/source/delivery_run.cpp "RunOnBigStack([&]() { rc = dl_recv_run(&ReceiveTick, &run); });" "the receiving runs on the big stack"
need ConfigApp/source/delivery_run.cpp "DrawPage(*page, wait_for_screen);" "a transfer's frame never waits for the screen; only the choice does"
need ConfigApp/source/delivery_run.cpp "DrawPage(run->page, false);" "the sender's frames never wait for the screen (the radio loop must not stand still)"
need ConfigApp/source/pages_delivery.cpp "dl_scan_stop();" "the scan for rooms is stopped before a room is joined"
need_after ConfigApp/source/pages_delivery.cpp "StopScan();" "delivery::RunReceive" "the scan for rooms is stopped just before a room is joined"
need ConfigApp/source/pages_delivery.cpp "g.HoldHome();" "HOME is off while the list of rooms is up (a press in the middle of a scan once froze a console)"
# The fourth round of the app (the user's third hardware test): the same bar on every top screen, HOME and START while the plugin is copied, the
# loading popup, the receivers' names and their details, the join of a console to a room that is already sending, and the plain words of the screens
[ "$(grep -rn 'DrawTitleBar(gfx::kTopW' ConfigApp/source --include=*.cpp | wc -l)" = "0" ] || fail "a top screen has its own bar again (the version only, no battery and card): they all use DrawTitleBarStatus"
need ConfigApp/source/ui.cpp "limit = bx - 8.0f;" "the title of the top bar keeps to the left of the battery and the card (a long title is cut, the version goes when it does not fit)"
need ConfigApp/source/ui.cpp "return setup::CopyWorker::Get().PendingCount() == 0 || g.plugin_pass_open;" "START does nothing while the user's own copies are being made, and works (after asking) during the pass over the games that follows an update"
need ConfigApp/source/ui.cpp "AskLeaveDuringPluginPass(); // (the pass over the games is not over: say so, and let the user choose)" "START during the pass over the games asks before it leaves"
need ConfigApp/source/ui.cpp "setup::CopyWorker::Get().CancelRefreshes();" "leaving in the middle of the pass drops the looks that wait (the one that runs is finished)"
need ConfigApp/source/copyworker.cpp "int CopyWorker::CancelRefreshes() {" "the worker can drop the quiet looks that wait"
need ConfigApp/source/ui.cpp "plugin_pass_total = submitted;" "the pass knows how many folders it looks at (the main page says how many are done)"
need ConfigApp/source/ui.cpp "aptSetHomeAllowed(home_holds == 0);" "HOME is given back only when the last page that switched it off lets go"
need ConfigApp/source/main.cpp "g.HoldHome();" "HOME is off while the plugin is being copied into the folders of the titles"
need ConfigApp/source/pages_enable.cpp 'Fmt("%d / %d ON", on, static_cast<int>(entries_.size()))' "the bar of the page of the titles says how many are ON"
need ConfigApp/source/pages_delivery.cpp "PushPage(new LoadingPage(make));" "a list that takes a moment to build is opened behind a Loading popup"
need ConfigApp/source/pages_delivery.cpp 'return "Select titles to share";' "the bar of the list of titles to send says what the page is for"
need ConfigApp/source/pages_delivery.cpp "g.HoldHome(); // (the choice of what to send" "HOME is off on the page where the titles to send are chosen (the room that follows would break), and the bar says so"
need ConfigApp/source/delivery_run.cpp 'return "Choose what to download";' "the bar of the list of what to receive says what the page is for"
need ConfigApp/source/pages_delivery.cpp 'return "Options"; // (not "Titles Delivery" once more' "the menu of Titles Delivery does not repeat its name on the bottom bar"
[ "$(grep -c 'FooterKey{"SELECT"' ConfigApp/source/delivery_run.cpp)" = "2" ] || fail "the sending screen and the receiving screen both have the Screens off button"
! grep -q -i "speed mode" ConfigApp/source/delivery_run.cpp || fail "a transfer screen talks about the speed mode again (it is a setting of the ini file)"
need ConfigApp/source/delivery_run.cpp "const bool stale = s.data_age_ms > 2500 && !no_more;" "when no data comes in the receiving screen says 0 MiB/s and no time, not what it saw last (but a console that needs no more data is not stale)"
need ConfigApp/source/delivery_run.cpp 'Fmt("reconnected %d time%s", s.reconnects, s.reconnects == 1 ? "" : "s")' "the number of times the connection was lost is said once, in the line of the state"
need ConfigApp/source/delivery_run.cpp '"repeated packets"' "the packets that were repeated are called that"
need ConfigApp/source/delivery_run.cpp "row.label_color = KindColor(s.title[i].kind);" "the list of what to receive has the same colours as the list of what to send"
need ConfigApp/source/ui.cpp 'gfx::TextWidth("1023.9 MB", 0.42f)' "the size of a row of a list with boxes is a column of its own (the chips before it stay in line)"
need ConfigApp/titleshare/source/sender.c "static void note_node(uint16_t token, uint16_t node) {" "the sender knows which node each receiver is, to say its name"
need ConfigApp/titleshare/source/sender.c "joinHoldUntil = now + 500;" "a console that joins a room that is sending stops the blocks for half a second..."
need ConfigApp/titleshare/source/sender.c "holdCooldownUntil = now + 2000;" "...at most once every two seconds (consoles that join one after the other do not stall the room)"
need ConfigApp/titleshare/source/sender.c "joinBoostUntil = now + 1000;" "...and gets the manifest five times in a second (the second starts again for every new node)"
need ConfigApp/titleshare/source/sender.c "now - lastManifest >= (now < joinBoostUntil ? 200u : 1000u)" "...every 200 ms while the boost lasts"
need ConfigApp/titleshare/source/sender.c "if(r != (Result) 0xC86113F0) {" "a control frame (the manifest, the state, the roster) is sent again while the radio queue is full"
# The second hardware test (2026-10-17): the name of a receiver's console stays (by token), two new states, a failed title leaves nothing, the messages, the channel
need ConfigApp/titleshare/source/sender.c 'snprintf(st->cname[k], sizeof(st->cname[k]), "%s", name_of(cl->token));' "the name of a receiver is the last good one the sender knew (a failed lookup does not make it Receiver N)"
need ConfigApp/titleshare/source/sender.c "st->cstate[k] = err_of(cl->token) ? 6 : 2;" "a receiver that finished with a failed title is told apart from one that finished"
need ConfigApp/titleshare/source/sender.c "st->cstate[k] = 5; // it still needs blocks, but not of the title that is being sent: it waits for its turn" "a receiver that waits for its turn is not shown as downloading"
need ConfigApp/titleshare/source/sender.c "note_err(token, (flags & TS_F_ERR) != 0);" "the sender learns from the acknowledgement that a receiver finished with an error"
need ConfigApp/titleshare/source/sender.c "udsGenerateDefaultNetworkStruct(&ns, TS_WLANCOMM_ID, 0, TS_NET_NODES);" "the room takes the sender and seven receivers"
need ConfigApp/titleshare/source/net.h "#define TS_NET_NODES 8" "eight nodes: the sender and seven receivers"
! grep -q "TS_SILENCE_MS\|TS_CLIENT_TIMEOUT_MS\|TS_PAUSE_MAX_MS" ConfigApp/titleshare/source/net.h || fail "ConfigApp/titleshare/source/net.h has a dead constant again (TS_SILENCE_MS, TS_CLIENT_TIMEOUT_MS or TS_PAUSE_MAX_MS: nothing uses them and their comments were false)"
need ConfigApp/titleshare/source/net.h "char firstName[48];" "the beacon carries the whole name of the first title (after the 29 bytes an older receiver reads)"
need ConfigApp/titleshare/source/receiver.c "ackFlags |= TS_F_ERR; // (the sender shows \"finished with errors\")" "an acknowledgement tells the sender that a title failed"
[ "$(grep -c 'scrub_failed(wt, m->t\[cur\]\.tid);' ConfigApp/titleshare/source/receiver.c)" = "5" ] || fail "ConfigApp/titleshare/source/receiver.c: a title that fails in AM (a refused write, an unreadable park file twice, a refused end) or is stopped half way must be taken away with scrub_failed, five places"
need ConfigApp/titleshare/source/receiver.c "Result rt = AM_DeleteTitle(dest, tid);" "what a failed title leaves is deleted: that title and its ticket only"
need ConfigApp/titleshare/source/receiver.c 'snprintf(st.msg, sizeof(st.msg), "tutto scaricato: installo");' "a console that has everything and only installs does not say that the sender serves others"
need ConfigApp/titleshare/source/receiver.c "n->reconnects++; // (only the ones that worked)" "the number of reconnections counts the ones that worked, not the attempts"
need ConfigApp/titleshare/source/receiver.c "n->reconnFull = maxRoom > 0 && inRoom >= maxRoom;" "the room is said to be full only when its beacon says so"
need ConfigApp/titleshare/source/receiver.c 'ts_log("fine ricezione: risultato %d (%s)' "the log says why a receiving ended"
need ConfigApp/titleshare_host/delivery.c '"The room is full (%d/%d), still trying (%u s)"' "the screen says the room is full, with how many are in it, and that it tries again"
need ConfigApp/titleshare/source/power.c "static void power_thread(void* arg) {" "the shell state and the periodic system line are asked in a thread of their own, not in the interface's loop"
need ConfigApp/titleshare/source/power.c "if(!ts_log_active()) {" "the periodic system line costs nothing when the Debug log is off"
need ConfigApp/titleshare/source/power.c 'ts_log("chiamata lenta: %s %llu ms"' "a call of the system that takes long is written in the log"
need ConfigApp/source/delivery_run.cpp "static constexpr int kShownRows = 5;" "the bottom screen of a room shows five receivers (with seven it scrolls)"
need ConfigApp/source/delivery_run.cpp 'return seconds > 7200.0 ? std::string("over 2 h") : ShortDurationText(seconds);' "the time of one receiver says over 2 h beyond two hours"
need ConfigApp/source/delivery_run.cpp '"download starts in " + (mine.wait > 7200.0' "a receiver whose first title has not started is told when the download starts, not how long is left"
need ConfigApp/source/delivery_run.cpp 'return "waiting";' "a receiver that waits for its turn says so"
need ConfigApp/source/delivery_run.cpp "word += Fmt(\" %d%%\", static_cast<int>(pct > 99 ? 99 : pct));" "the title that installs says how far it is, on its row"
need ConfigApp/source/delivery_run.cpp "RefreshStatus(run->last_status_read, false);" "the receiving screen does not ask the card itself (the card may be busy with the install)"
need ConfigApp/source/ui.cpp 'const std::string chan = Fmt("Wi-Fi CH %d", wifi_channel);' "the bar of the transfer screens says the Wi-Fi channel of the room"
need ConfigApp/source/ui.cpp "latch.down_n += (down & KEY_DOWN) ? 1 : 0;" "the latch counts the presses of the keys that move a list"
need ConfigApp/source/status.cpp "return (major == 13 && minor < 3) ? LumaSupport::Maybe : LumaSupport::Tested;" "Luma3DS 13.3 and newer is tested, 13.0 to 13.2 maybe"
need ConfigApp/source/status.cpp "return loader != LoaderState::Unknown ? LumaSupport::Maybe : LumaSupport::NotWork;" "a Luma3DS older than 13.0 has no Plugin Loader: not work (unless it answers as one)"
need ConfigApp/source/pages_delivery.cpp 'row.chip = Fmt("CH %d", room.channel);' "a room of the list says its Wi-Fi channel"
need ConfigApp/source/pages_delivery.cpp '"\nEstimated time to download all: about "' "a room of the list says how long it takes to get it all, as an estimate"
need_line ConfigApp/Makefile $'APP_AUTHOR\t\t:= Powered by Gionko' "the author of the app in the home menu"
[ -f ConfigApp/tools/make_icon.py ] || fail "ConfigApp/tools/make_icon.py is missing (the icon is drawn by it: icon.png can be made again)"
# The icon has NO alpha channel: an SMDH icon is RGB565 and smdhtool flattens the PNG onto BLACK, so transparent corners come out as black wedges on HOME, in the
# Homebrew Launcher and in the Download Play list. icon.png is a 48x48 PNG of colour type 2 (RGB), and its generator never puts an alpha channel back.
[ "$(od -An -tu1 -j16 -N10 ConfigApp/icon.png | tr -s ' ')" = " 0 0 0 48 0 0 0 48 8 2" ] || fail "ConfigApp/icon.png must be a 48x48 PNG without alpha (an SMDH icon has none: smdhtool turns transparency into black corners)"
[ "$(grep -c -E 'putalpha[(]|convert[(].RGBA.[)]' ConfigApp/tools/make_icon.py)" = "0" ] || fail "ConfigApp/tools/make_icon.py makes an alpha channel again (the corners of the icon would turn black on HOME)"
need ConfigApp/titleshare/source/sender.c "send_ctl(frame, rn);" "...the roster too"
need ConfigApp/titleshare/source/sender.c "if(serving > 0 && now >= joinHoldUntil && sched_next(sc, now, &g)) {" "the blocks wait while a console is joining"

need ConfigApp/source/pages_delivery.cpp "if (row.id == kSend && ready_ && !dl_have_keys()) {" "Send Titles cannot be opened without boot9.bin"
need ConfigApp/source/pages_delivery.cpp "if (send && !dl_have_keys()) {" "...also when it was asked for while the titles were still being read"
# The third round of the app (after the first hardware test): the two new checks, the files of earlier builds, the room test, the screens of the
# delivery (what they say and how they answer to the keys)
need ConfigApp/source/system.cpp '"/data/" + id0 + "/sysdata/0001000f/00000000"' "the seed database is the system save 0001000F of this console's ID0, read from the NAND (boot9.bin is not needed for it)"
need ConfigApp/source/status.cpp 'MakeItem("Seed database", "not readable", CheckLevel::Warning,' "a seed database that cannot be read is a warning (receiving works)"
need ConfigApp/source/status.cpp 'MakeItem("Plugin Loader", "disabled", CheckLevel::Problem,' "a Plugin Loader that is off is a problem (no title can load the plugin)"
need ConfigApp/source/status.cpp 'CheckAction::EnableLoader, "Plugin Loader off"' "...that A fixes"
need ConfigApp/source/ui.cpp "setup::InstallFacts facts = setup::ReadInstallFacts(paths, boot9, loader);" "the check reads the Plugin Loader, Luma3DS and the seed database every time"
[ "$(grep -rn 'MigrateOldFiles' ConfigApp/source --include=*.cpp --include=*.hpp | wc -l)" = "0" ] || fail "the files of earlier builds are brought over again at the first start"
need ConfigApp/source/pages_startup.cpp "static constexpr int kSteps = 6;" "the first start lists six steps (the titles, boot9.bin, the seed database, the plugin file, the plugin folder, the Plugin Loader)"
need ConfigApp/source/ui.cpp "box.y = kBarH + 1.0f + (static_cast<float>(gfx::kScreenH) - kBarH - 1.0f - box.h) * 0.5f;" "a popup is in the middle of the touch screen and as high as its text"
need ConfigApp/source/pages_servers.cpp 'return "Server list";' "the bottom bar of the list of rooms says what the list is"
need ConfigApp/source/pages_servers.cpp 'test.label = "Test room";' "the form of a room has a row that tests it"
need ConfigApp/source/pages_servers.cpp "PushPage(pages::NewProbePage(room_, nullptr));" "...with the values of the form, saving nothing"
need ConfigApp/source/probe.cpp "(outcome_ == ProbeOutcome::Cancelled || (close_key && was_ended_))" "the test of a room stays on the screen until the user closes it (the result and its reason can be read)"
need ConfigApp/source/pages_delivery.cpp "Up to 16 titles and 7 receivers." "a room has 8 places: the sender and 7 receivers"
need ConfigApp/source/pages_delivery.cpp 'Fmt("Selected titles %d of %d", PickedCount(), delivery::kMaxTitles)' "the card of Send Titles says how many are chosen, not the name of the page again"
need ConfigApp/source/pages_delivery.cpp "row.detail = size > 0 ? delivery::ShortDurationText" "the time of a part is a column of its own (the size does not move with its words)"
need ConfigApp/source/ui.cpp 'edge -= gfx::TextWidth("1 h 05 min", 0.42f) + 10.0f;' "...a column as wide as its longest word"
need ConfigApp/source/pages_enable.cpp "Switched on, this title connects to the Azahar emulator network room set in Azahar Emulator Room." "the page of the titles says what the switch is for"
need ConfigApp/source/pages_main.cpp "Lets your titles connect to an Azahar emulator network room" "...and so does its line on the main page"
need ConfigApp/source/delivery_run.cpp 'DrawPopupBox(box, "Close the room?", kCloseText);' "B on the sending screen asks before the room is closed"
[ "$(grep -c 'confirming_ = true;' ConfigApp/source/delivery_run.cpp)" = "2" ] || fail "B on the sending screen or on the receiving screen does not start its question"
# The time of the room (the user's fifth round: the minutes of a console that joins late): delivery_eta.cpp works it out from what every receiver
# still needs, title by title, in the order the sender serves them; the sending screen feeds it with what it measures
need ConfigApp/source/delivery_run.cpp "eta_ = EstimateRoom(in);" "the sending screen works out the time that is left from what every receiver still needs, title by title"
need ConfigApp/source/delivery_run.cpp "dl_send_estimate(pct_, eta_.kind == EtaKind::Busy ? eta_.seconds : -1.0);" "...and the receivers are given the same share and the same time"
need ConfigApp/source/delivery_run.cpp "send_rate_.Add(dt, sent - radio_bytes_);" "the speed the radio sends at is measured from everything it sends (the repeated blocks too)"
need ConfigApp/source/delivery_run.cpp "if (static_cast<double>(c.down_bytes) > static_cast<double>(c.inst_bytes) + kInstallBacklog) {" "an installer is measured only while it has a backlog (else it just keeps up with the radio)"
! grep -q "SecondsLeft" ConfigApp/source/delivery_run.cpp || fail "the sending screen has the old time estimate again (it was the module's floor, a one-second speed and no order of the titles)"
need ConfigApp/source/delivery_run.cpp 'left = "All receivers finished";' "a room with nothing left says so, not 'waiting for receivers'"
need ConfigApp/source/delivery_run.cpp 'right = "for all titles: " + ShortDurationText(eta_.seconds);' "with nobody in yet the time is what one receiver would take, said as that"
need ConfigApp/source/delivery_run.cpp "if (mine.wait >= 60.0 && (c.state == DL_C_WAITING || c.state == DL_C_JOINED)) {" "a receiver whose title comes later is told when it starts"
need ConfigApp/source/delivery_eta.cpp "slowest = std::max(slowest, n / (rate * eff[a]));" "a title lasts as long as the receiver that needs most of it (and has the worst air) needs"
need ConfigApp/source/delivery_eta.cpp "dur[t] = slowest + kLagShare * std::max(0.0, (mx - mn) - kAbsorb) / rate;" "a console far behind the others costs radio time of its own"
need ConfigApp/source/delivery_eta.cpp "better = tiny[t] < tiny[pick];" "the sender serves a small remainder first, the smallest first"
need ConfigApp/source/delivery_eta.cpp "better = k[t] > k[pick];" "...else the title that most receivers can take"
need ConfigApp/source/delivery_eta.cpp "if (n_present == 0 || (n_live == 0 && n_silent == 0 && n_choosing > 0)) {" "a console that has not chosen yet is nobody"
need ConfigApp/source/delivery_eta.cpp "f = std::max(begin + inst_left / q + kTitleFixed, takes[a][t] ? e + kTail : 0.0);" "a receiver is done when its titles have come AND been installed, one after the other"
need ConfigApp/source/delivery_eta.cpp "pct = std::min(std::max(static_cast<int>(100.0 * done / total), 0), 99);" "the percentage is the work done over the work asked for, and never 100 before everybody is done"
need ConfigApp/source/delivery_eta.cpp "if (total <= total_ + 1.0 && pct < pct_) {" "...it goes up and falls only when somebody comes in who adds work (the minutes move, the percentage does not)"
need ConfigApp/source/delivery_eta.cpp "credit_ += finished_ - eta.work_finished;" "...and a finished receiver that leaves keeps its work in the count"
need ConfigApp/source/delivery_eta.cpp "out.work_done += r.finished ? asked : installed;" "the work done is what is installed (a finished receiver counts in full)"
need ConfigApp/source/delivery_eta.cpp "if (idle_ >= 30.0) {" "the share starts again after half a minute without work"
need ConfigApp/source/delivery_run.cpp "pct_ = share_.Update(dt, eta_);" "the percentage of the room comes from the work done, not from the minutes"
[ "$(grep -rn 'RoomClock' ConfigApp/source --include=*.cpp --include=*.hpp | wc -l)" = "0" ] || fail "the percentage is a function of the minutes again (RoomClock fell with every revision of the estimate)"
# ... and the other fixes of that review
need ConfigApp/source/delivery_run.cpp "in.down = select;" "with the screens off only SELECT reaches the screens (a press of B in a pocket must not answer the invisible question)"
need ConfigApp/source/delivery_run.cpp "} else if (total && free_bytes < sp.need) {" "Y does not start a receiving that cannot fit on the card"
need ConfigApp/source/pages_delivery.cpp 'if (dl_scan_pick(row.id, &room) != 0 || gfx::Clean(room.host[0] ? room.host : "A console") != row.label) {' "the room that is joined is the one the user picked, not whatever is at that place after a new scan"
need ConfigApp/source/delivery_run.cpp '"The room was closed by an error" + (run.failure.empty()' "an error that closes the room says why"
need ConfigApp/source/delivery_run.cpp "Not enough memory to join the room: close other applications and try again." "a receiving that could not get its stack says so (it does not run on the small one)"
need ConfigApp/source/delivery_run.cpp "Not enough memory to open the room: close other applications and try again." "...nor does a sending"
need ConfigApp/source/ui.cpp "} else if (latch.nav_since != 0 && now - latch.nav_since >= 400 && now - latch.nav_last >= 100) {" "up and down repeat while held, also in the screens that read the keys through the key thread"
need ConfigApp/source/delivery_run.cpp "if (sp.who != who) {" "the speed of a receiver is measured by receiver, not by place in the list"
need ConfigApp/source/delivery_run.cpp 'Fmt("and %d more: %d installed", more, more_done)' "the list of the receiving screen says how many more there are and how many are installed"
! grep -q "dl_pace_next" ConfigApp/source/delivery_run.cpp || fail "the sending screen has the Speed button again"
[ "$(grep -c 'g.input.StartLatch();' ConfigApp/source/delivery_run.cpp)" = "2" ] || fail "a transfer screen does not keep the keys while the module runs (a short press would fall between two looks)"
[ "$(grep -c 'g.input.StopLatch();' ConfigApp/source/delivery_run.cpp)" = "2" ] || fail "a transfer screen leaves the key thread running"
[ "$(grep -A1 'bool StartButton() const override {' ConfigApp/source/pages_delivery.cpp | grep -c 'return false;')" = "2" ] || fail "a page of Titles Delivery (the menu, the rooms) has a START button: the app is left from the main page (only the summary of a receiving has one, to leave at once)"
need ConfigApp/source/pages_delivery.cpp "PopPages(2); // (this page and the menu of Titles Delivery under it)" "B on the summary of a receiving goes back to the main page, not to the menu it came from"
need ConfigApp/source/pages_delivery.cpp 'return "Main page";' "...and says so"
need ConfigApp/source/pages_delivery.cpp 'return "Continue";' "A on the summary goes on to the menu of Titles Delivery"
need ConfigApp/source/pages_delivery.cpp 'return "Exit";' "START on the summary leaves the app"
need ConfigApp/source/delivery_run.cpp "run->result.done_ms[i] = now - run->result.start_ms + 1;" "the summary knows how long after the start each title was installed"
need ConfigApp/source/pages_delivery.cpp 'x = delivery::DrawPair(x, 88, 0.44f, "in", delivery::ClockText(elapsed_));' "the summary says in how long (really) it was done"
# The clean-up before Download and Play: what was written five times is written once, and what nothing used is gone
[ "$(grep -rn 'FillRect(0, 213' ConfigApp/source --include=*.cpp | wc -l)" = "1" ] || fail "the strip behind the footer is drawn in more than one place (DrawFooterBackground in ui.cpp)"
[ "$(grep -rn 'FillCircle(142.0f' ConfigApp/source --include=*.cpp | wc -l)" = "1" ] || fail "the moving dots are drawn in more than one place (DrawSpinner in ui.cpp)"
[ "$(grep -rn 'float DrawPair(' ConfigApp/source --include=*.cpp | wc -l)" = "1" ] || fail "the pair of a label and a value is written in more than one place (delivery.cpp)"
[ "$(grep -rln '6a09e667' ConfigApp/source ConfigApp/titleshare_host | wc -l)" = "0" ] || fail "the app has its own SHA-256 again (it uses the module's common/sha256.c)"
need ConfigApp/source/paths.cpp "sha256_update(&sha, buffer, got);" "a boot9.bin is recognised by the SHA-256 of its content"
[ "$(grep -rn 'dl_pace_\|MigrateOldFiles\|FindCheckAction\|SdUsageText\|BatteryText\|is_azaharlink' ConfigApp/source ConfigApp/titleshare_host | wc -l)" = "0" ] || fail "code that nothing used is back (the pace chooser, the migration, the texts only the tests read, the plugin name guess)"
need ConfigApp/titleshare_host/delivery.c "_Static_assert(SAME_NUMBER(DL_RES_PENDING, TS_RES_PENDING)" "the numbers the screens read are the module's"
need ConfigApp/source/ui.cpp "while (g_pop_extra > 0 && g_pages.size() > 1) {" "PopPages takes several pages off at once"
need ConfigApp/source/pages_config.cpp "g.cfg.nickname = input;" "the nickname typed on the Settings page is saved"
need ConfigApp/source/titles.cpp "UseAppNames(impl);" "the catalog takes over the names the app reads, so every list (and what is sent) says the same name"
if grep -rn 'AZL_TEST_DELIVERY_DEMO' ConfigApp --include=*.cpp --include=*.hpp --include=*.h | grep -v 'ConfigApp/titleshare_host/'; then
  fail "the test build of the delivery screens (made-up rooms and numbers) is used outside the bridge"
fi
if grep -rn 'AZL_TEST_TS_' ConfigApp; then
  fail "a test switch of the old game delivery is back"
fi
need ConfigApp/source/paths.cpp 'p.delivery_log = JoinPath(p.datadir, "3DSLink-TitleDelivery.log");' "the delivery log is in the data folder, with the name logrot.c gives it"
need ConfigApp/source/main.cpp "if (!session::Start()) {" "the services and the graphics of the app are opened in one place"
need_line ConfigApp/source/session.cpp '    gfxSetDoubleBuffering(GFX_TOP, true);' "the app draws every frame into the buffer that is not on the screen: the double buffering is turned on whenever the graphics are opened"

# The log has two levels and nothing between: 0 writes no file at all, 1 writes everything
if grep -rn -E "g_debug_level *(>=|==|>) *2|g_debug_level *< *2|debug_level\(\) *\+ *1\) *% *3|kDebugNames" Sources Includes ConfigApp/source --include=*.cpp --include=*.hpp --include=*.h | grep -v "^Sources/enet"; then
  fail "a third log level is back (debug is 0 or 1)"
fi
need Includes/azl/ini_values.hpp "std::strtol(value.c_str(), nullptr, 10) >= 1 ? 1 : 0" "debug in the ini: any value from 1 up is 1, the rest 0"
need Sources/ctr/plugin.cpp "config_.debug = ini::DebugLevel(value);" "the plugin reads debug through the shared rule"
need ConfigApp/source/settings.cpp "cfg.debug = azl::ini::DebugLevel(value);" "the Setup app reads debug through the shared rule"
need_after Sources/azl/log.cpp "void Log(const char* fmt, ...) {" "if (g_debug_level <= 0) {" "Log() returns at once when the log is off"
need_after Sources/azl/log.cpp "LogLine::LogLine() :" "if (g_debug_level <= 0) {" "a LogLine is inactive when the log is off"
need_after Sources/azl/log.cpp "DebugIpc(u32 handle, u32 header, u32 arg, u32 result) {" "if (g_debug_level <= 0 || !g_dbg_ring) {" "the capture does nothing when the log is off"
# No log file exists unless the log is on: the files of the log are opened in two places, both behind the level check, and
# the console's file layer is the only user of File::Open besides the ini (read in LoadConfig, written in SaveConfig) and the memory
# record of the last launch (read and written in CompareMemoryWithLastLaunch, with the log on only)
need Sources/azl/log_files.cpp "if (log_file_ || g_debug_level <= 0) {" "the main log is never opened with the log off"
need Sources/azl/log_files.cpp "if (debug_file_ || g_debug_level <= 0 || !DebugRingReady()) {" "the capture file is never opened with the log off"
need Sources/azl/log_files.cpp "if (crumb_file_ || g_debug_level <= 0) {" "the trail of a sleep (AzaharLink.sleep.log) is never opened with the log off"
# (the capture, the main log, and the trail's file: opened and, for the trail, reopened after the old one is removed)
[ "$(grep -c 'OpenAppend(' Sources/azl/log_files.cpp)" = "4" ] || fail "Sources/azl/log_files.cpp opens a file in a place that is not behind the level check"
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -o 'File::Open(' | wc -l)" = "12" ] || fail "Sources/ctr/plugin.cpp opens a file besides the log layer, the exit guard's own file (AppendLumaLine, on the plugin's thread), the ini read, the ini write, the memory record (read and write), the startup-occupancy warning (CheckStartupOccupancy, the one debug-0 write), the line of the link to Luma3DS's plugin loader (CheckPluginLoaderLink, debug-independent too), the three read-only openings of the comparison of this game's plugin with 3DS Link's (ReadPluginHead, PluginFilesAreEqual), and the read-only one of the in-game menu's lists (ReadDataFile)"
need_after Sources/ctr/plugin.cpp "void Plugin::CompareMemoryWithLastLaunch() {" "if (g_debug_level <= 0) {" "the memory record is read and written with the log on only (debug=0 leaves the card alone)"
need Sources/ctr/plugin.cpp "mem against the last launch of this title:" "two launches of a title are compared (2026-10-20: the proof that an exit leaves nothing)"
need Sources/ctr/plugin.cpp "    CompareMemoryWithLastLaunch();" "... at the start, after the memory line"
need Sources/ctr/plugin.cpp "    constexpr int kRecords = 16;" "... with a record for each of the last 16 titles (a launch of the same title right after the last one is rare)"
need Sources/ctr/plugin.cpp "void Plugin::CheckLinkSilence(const Stats& s) {" "with the log on, a notice when the room's datagrams stop for 150 ms or more (the WiFi link: 2026-10-20, one console 17-23 a minute, the other 0-2)"
need Sources/ctr/plugin.cpp "    CheckLinkSilence(s);" "... checked every statistics window"
need Sources/ctr/plugin.cpp "    if (s.rx_datagrams >= 100 && silent >= 2) {" "... in a window with traffic and at least two silences"
need_after Sources/ctr/plugin.cpp "void Plugin::CheckLinkSilence(const Stats& s) {" "if (g_debug_level <= 0) {" "... and only with the log on"
need Sources/ctr/plugin.cpp "if (config_.debug > 0) {" "the capture ring is allocated only when debug is on"
need Sources/ctr/plugin.cpp "config_.debug = g_debug_level;" "the menu label and the toggle follow what the log really does"
need Sources/ctr/plugin.cpp ': DebugRingReady() ? "AzaharLink.log and AzaharLink.debug.log"' "the line that says the capture is written is only true when the ring exists"
# ... and nothing else in the product touches a file: not one call that creates, writes, removes or renames outside the
# log layer (Sources/azl/log_files.cpp, which only calls its FileLayer) and plugin.cpp (counted above; its FSUSER_*
# calls only read the game's icon)
if grep -rn -E 'File::(Open|Remove|Rename)|fopen|freopen|std::(o|i)?fstream|FSUSER_(OpenFile|DeleteFile|RenameFile|CreateFile)|FS_OPEN_(WRITE|CREATE)|mkdir|\bopen\(' Sources Includes --include=*.cpp --include=*.hpp --include=*.c --include=*.h | grep -v "^Sources/ctr/plugin.cpp:\|^Sources/azl/log_files.cpp:\|^Sources/ctr/boot_diag.cpp:\|^Sources/game_menu_tour.cpp:"; then
  fail "a source file besides the log layer and plugin.cpp touches a file (debug=0 must leave the card alone)"
fi
grep -q 'FS_OPEN_WRITE\|FS_OPEN_CREATE' Sources/ctr/plugin.cpp && fail "plugin.cpp opens a file of the card for writing through FSUSER (only the icon is read that way)"
# ... except the diagnostic build's own file (Sources/ctr/boot_diag.cpp, AzaharLink/diag/README.md): it writes /3ds/3DSLink/AzaharLink.boot.log even at debug=0, so the whole file is
# compiled only with -DAZL_BOOT_DIAG, which only diag/build_diag.sh gives (in a copy of the folders), never the delivered plugin's Makefile or build_all.sh.
[ "$(tr -d '\r' < Sources/ctr/boot_diag.cpp | grep -c '^#ifdef AZL_BOOT_DIAG$')" = "1" ] || fail "Sources/ctr/boot_diag.cpp is not compiled only under AZL_BOOT_DIAG (it writes to the card at debug=0)"
tr -d '\r' < Sources/ctr/boot_diag.cpp | tail -1 | grep -q -F -- '#endif // AZL_BOOT_DIAG' || fail "Sources/ctr/boot_diag.cpp does not end with the #endif of AZL_BOOT_DIAG"
! grep -q 'AZL_BOOT_DIAG' Makefile ../DlpLink/tools/build_all.sh || fail "the delivered plugin's Makefile or build_all.sh names AZL_BOOT_DIAG (the diagnostic build is made only by diag/build_diag.sh)"
need diag/build_diag.sh "sed -i 's/-DAZL_USE_CTRPF/-DAZL_USE_CTRPF -DAZL_BOOT_DIAG/' \"\$WORK/Makefile\"" "the diagnostic flag goes into a COPY of the Makefile"
# ... and the screenshots of the in-game menu (Sources/game_menu_tour.cpp): written to the card by a test build only, the whole file under
# -DAZL_MENU_TOUR, which only `make EXTRA_CFLAGS=-DAZL_MENU_TOUR` gives (by hand, in a copy of the folders); build_all.sh never passes EXTRA_CFLAGS.
[ "$(tr -d '\r' < Sources/game_menu_tour.cpp | grep -c '^#ifdef AZL_MENU_TOUR$')" = "1" ] || fail "Sources/game_menu_tour.cpp is not compiled only under AZL_MENU_TOUR (it writes to the card)"
tr -d '\r' < Sources/game_menu_tour.cpp | head -1 | grep -q -x '#ifdef AZL_MENU_TOUR' || fail "Sources/game_menu_tour.cpp does not begin with the #ifdef of AZL_MENU_TOUR"
tr -d '\r' < Sources/game_menu_tour.cpp | tail -1 | grep -q -F -- '#endif // AZL_MENU_TOUR' || fail "Sources/game_menu_tour.cpp does not end with the #endif of AZL_MENU_TOUR"
! grep -q 'AZL_MENU_TOUR\|EXTRA_CFLAGS' ../DlpLink/tools/build_all.sh || fail "build_all.sh names AZL_MENU_TOUR or EXTRA_CFLAGS (the menu's test build is made by hand only)"

# The in-game menu switch: menu_key=off (or none) vetoes every opening, and nothing waits for the menu to join the room
need Sources/main.cpp "g_menu_off = plugin.menu_off();" "main reads the switch"
tr -d '\r' < Sources/main.cpp | grep -A1 "static bool OnMenuOpening" | grep -q "return false;" || fail "OnMenuOpening lets the framework's menu open (the plugin's own menu is the only one)"
need Sources/main.cpp "    if (!g_menu_off) {" "the plugin's menu is looked for only when the switch is on"
need Sources/main.cpp "g_want_connected = (cfg.autoconnect || g_menu_off) && !cfg.host.empty();" "with the menu off the room is joined at start whatever autoconnect says"
# The plugin's own in-game menu (Sources/game_menu.cpp): no cheat tools, B closes it, the room changes with the game running.
need Sources/main.cpp "settings.AllowActionReplay = false;" "the framework's cheat codes are off"
need Sources/main.cpp "settings.AllowSearchEngine = false;" "... and its memory search"
need Sources/game_menu.cpp "    } else if (down & Key::B) {" "B goes back, and closes the menu on its first page"
need Sources/game_menu.cpp "        if (!(Controller::GetKeysDown() & Key::B)) {" "a B still held when the menu closes does not reach the game"
need Sources/game_menu.cpp "    m.file.Clear();" "the list shown is given back when the menu closes (the plugin keeps nothing of it in the game's memory)"
# The game's APT notifications are passed on as they come: an SDK title's thread of the notifications panics on an empty one (svcBreak:
# Super Street Fighter IV 3D on two consoles, 2026-11-04, when the in-game menu took the HOME notification away; analysis/ssf4-crash-menu-2026-11-04).
! tr -d '\r' < Sources/ctr/hook.cpp | grep -q -E 'cmd\[2\] *= *0;' || fail "the svc hook changes the game's APT notification (an empty one makes an SDK title panic)"
need Includes/azl/room_choices.hpp "        std::string().swap(text_);" "... really given back (clear() keeps the capacity)"
need Includes/azl/ini_values.hpp 'constexpr const char* kDefaultMenuKey = "L+Right+Select";' "the menu's chord is L + Right + Select (beside Rosalina's L + Down + Select)"
need Sources/ctr/plugin.cpp "config_.menu_key = ini::MenuKeyFromFile(value);" "a file that still holds the old default L+R+Select takes the new one (the plugin)"
need ConfigApp/source/settings.cpp "cfg.menu_key = azl::ini::MenuKeyFromFile(value);" "... and the app"
need Includes/azl/ini_values.hpp 'return word == "off" || word == "none";' "off and none switch the menu off"

# The scan filter only counts (spec C7): nothing in the plugin turns it on
if grep -rn "SetScanFilterEnforced" Sources/ctr Sources/main.cpp >/dev/null 2>&1; then
  fail "the plugin calls SetScanFilterEnforced: the filter's mode is the engine's default (enforced since 2026-10-20), the switch is the tests'"
fi
need Includes/azl/uds_engine.hpp "bool scan_filter_enforced_ = true;" "the scan filter leaves the networks of another game out (2026-10-20)"

# The sleep action exists in one place, behind sleep_stop
[ "$(grep -c 'StopService(' Sources/ctr/plugin.cpp)" = "1" ] || fail "StopService is called from more than one place in Sources/ctr/plugin.cpp"
need Sources/ctr/plugin.cpp "if (!acts) {" "OnSleepEnter returns before it stops anything unless the sleep is acted on"
need Includes/azl/apt_sleep.hpp "return sleep_stop != 0 && (sleep_stop >= 2 || !hosting);" "sleep_stop=0 never acts, 1 acts for a console that is not hosting, 2 forces it for a host too (the one host that was stopped by a sleep froze the console)"
need Sources/ctr/plugin.cpp "return SleepActsFor(mode, mode == 1 && engine_->status() == NetworkStatus::ConnectedAsHost);" "the sleep action asks the engine whether the console hosts"
need Sources/ctr/plugin.cpp "config_.sleep_stop = ini::SleepStop(value);" "the plugin reads sleep_stop through the shared rule"
need Sources/ctr/plugin.cpp "sleep_.NeedsActs(event_count_[kEventSleepExit]) && SleepActs()" "the frame callback works out whether the sleep acts only when a wake-up is to be looked at (with sleep_stop=1 that takes the engine's lock)"
need Includes/azl/ini_values.hpp 'return Word(value) == "2" ? 2 : kSleepStopDefault;' "the ini value of sleep_stop: 0, 1 (or on/true/yes), 2; a value that is none of them is the default, off (the action ends a game session)"

# The link (0.1.16): what the room link does when the WiFi goes and comes back, and what it lets into the game
# - the schedule of the attempts is fixed (2 s after the last began; no growing interval), and held back while the console's
#   WiFi is down (shared page byte 0x1FF81067, read every 500 ms, an attempt at once when it is back)
need Includes/azl/link_policy.hpp "static constexpr u32 kRetryMs = 2000;" "a failed attempt is followed by the next one 2 s after it began"
need Includes/azl/link_policy.hpp "static constexpr u32 kWifiPollMs = 500;" "the WiFi state is read every 500 ms"
need Includes/azl/link_policy.hpp "static constexpr u8 kWifiConnected = 2;" "the shared page's network state 2 is a console on its access point"
if grep -n "kStepMs\|kMaxWaitMs\|interval_ms_" Includes/azl/link_policy.hpp; then
  fail "the growing interval of the attempts (2, 4, 6, 8, 10 s) is back: the schedule is fixed since 0.1.16"
fi
need Sources/ctr/plugin.cpp "in.wifi_state = *reinterpret_cast<volatile const u8*>(kSharedPageWifiState);" "the policy gets the WiFi byte of the shared page"
# - the room client: a bounded join (a stuck network thread is abandoned, not waited for), an escalating pause after socket
#   errors, and no wait for the SD card in the state callback
need Sources/azl/room_client.cpp "thread_.JoinTimeout(kStopJoinMs)" "RoomClient::Stop waits for the network thread a bounded time"
if grep -n "thread_.Join()" Sources/azl/room_client.cpp; then
  fail "RoomClient waits for its thread without a limit (JoinTimeout)"
fi
need Sources/azl/room_client.cpp "platform::SleepMs(ServiceErrorPauseMs(errors_in_a_row));" "a failing socket is answered by a pause that grows with the errors in a row"
# ... a thread that Stop() gave up on leaves the loop right after the callbacks (where it gets stuck) and, before that, changes
# the client's state and error only through the lambdas that check its session (the one Fail(Error:: left is Start()'s)
need Sources/azl/room_client.cpp "if (!keep_running || !Live(generation)) {" "an abandoned network thread must not go on to the queue of the session that replaced it"
[ "$(grep -c 'Fail(Error::' Sources/azl/room_client.cpp)" = "1" ] || fail "the network thread of Sources/azl/room_client.cpp sets an error without checking that its session is still the client's (fail())"
if grep -n 'SetState(State::Joined)' Sources/azl/room_client.cpp; then
  fail "the network thread of Sources/azl/room_client.cpp sets Joined without checking that its session is still the client's (set_state())"
fi
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c 'log_files_->Flush()')" = "2" ] || fail "Sources/ctr/plugin.cpp calls log_files_->Flush() from more than the log writer's two places (the room state callback must not wait for the SD card)"
# - the foreign-lobby filter: a host always refuses, a client only counts until the switch is turned (nothing of the plugin's own code
#   turns it; 1.1.3, decision Q8 of PROGETTO-DLP-VERO-1.1.3.md taken in the night session of 2026-11-09 and written in REGISTRO-MODIFICHE-ERRORI.md:
#   the adapter of the real Download Play turns it on for a probe and a session of a RealDlp game only, and its Release turns it off on every
#   path - pinned in the phase F3/F4/F5 blocks below)
need Includes/azl/uds_engine.hpp "bool client_foreign_filter_enforced_ = false;" "a client only counts the frames of another lobby (until a session on hardware shows it can drop them)"
if grep -rn "SetClientForeignFilterEnforced" Sources/ctr Sources/main.cpp >/dev/null 2>&1; then
  fail "the plugin turns the client's foreign-lobby filter on: that waits for a hardware session with zero would-be drops"
fi
need Sources/azl/uds_engine.cpp "const bool refuse = status == NetworkStatus::ConnectedAsHost || client_foreign_filter_enforced_ || lobby_inbox_;" "a host refuses the frames of another lobby always, a client when its filter is on or when it is in a lobby (a second lobby of the game in the room has the same node ids and would rewrite its roster)"
# - the beacon timer is an absolute schedule on the exact period, and the late second Deauthentication is swallowed
need Sources/azl/uds_engine.cpp "next_beacon_tenths_ += kBeaconPeriodTenths;" "the host's beacon is due a period after the last one was DUE (105.5 ms per beacon on hardware before)"
need Sources/azl/uds_engine.cpp "constexpr u64 kBeaconPeriodTenths = 1024;" "100 TU = 102.4 ms"
need Sources/azl/uds_engine.cpp "it->second.deauth_guard = false;" "the guard of the late second Deauthentication is used up by the first copy it swallows"
# - a game thread that polls a command that keeps failing is slowed down (never answered differently)
need Sources/ctr/uds_hle.cpp "svcSleepThread(kFailPauseNs);" "the throttle of a failing-reply spin"
need Sources/ctr/uds_hle.cpp "ThrottleFailing(ended);" "every failing reply goes through the throttle"

# The capture (0.1.16): a ring with a lane for control records, markers for the holes, a producer that never waits for the
# ring's lock, the diet of the IPC lines, header-only records for the frames that are not for us, bounded passes of the writer
need Sources/azl/log.cpp "constexpr u32 kControlReserveBytes = 32 * 1024;" "the last 32 KB of the ring are for the control records (a flood of frames cannot push them out)"
need Sources/azl/log.cpp "constexpr u32 kHeavyLimit = kDebugRingBytes - kControlReserveBytes;" "the heavy lane stops where the control lane's room begins"
need Sources/azl/log.cpp "const u32 limit = lane == kLaneHeavy ? kHeavyLimit : kDebugRingBytes;" "a heavy record is held to the heavy limit, a control record to the whole ring"
need Sources/azl/log.cpp "EmitLostLocked(lane, now);" "the first record after a hole is preceded by the marker of the hole"
need Sources/azl/log.cpp "if (mutex.try_lock()) {" "a producer of the capture tries the ring's lock a few times and gives up (it never waits: the writer has the lowest priority)"
[ "$(grep -c 'platform::LockGuard lock(g_dbg_mutex);' Sources/azl/log.cpp)" = "7" ] || fail "Sources/azl/log.cpp: a function that waits for the ring's lock besides those of the writer side (DebugPop twice, DebugOldestMs, DebugFlushAggregates, DebugCaptureTotals, DebugReset, DebugTakeDropped)"
need Sources/azl/log.cpp "constexpr u32 kLockSpins = 256;" "a producer tries the ring's lock 256 times (about 30 us) and gives the record up: a game thread never waits for the writer"
if grep -n 'g_dbg_mutex.lock()' Sources/azl/log.cpp; then
  fail "Sources/azl/log.cpp waits for the ring's lock with lock(): a producer must use TryLockBriefly"
fi
need Sources/azl/log.cpp "constexpr u32 kDietFullCalls = 3;" "the diet writes the first three calls of a kind in a second whole"
need Sources/azl/log.cpp "constexpr u32 kDietWindowMs = 1000;" "... and counts the rest of that second in one line"
need Sources/azl/log.cpp "if ((result != 0 || !DietLocked(handle, header, arg, now)) &&" "a call with a result other than 0 is never counted"
need Sources/azl/uds_engine.cpp "DebugSecureHeader(kDirEcho, packet, secure_data);" "the echo of our own frame is a header-only record"
need Sources/azl/uds_engine.cpp "DebugSecureHeader(kDirOther, packet, secure_data);" "a frame for another node is a header-only record"
need Sources/azl/uds_engine.cpp "DebugFrame(kDirEcho, src_node, dest_node, data_channel, nullptr, frame_size);" "the prefilter writes the same header-only echo record"
need Sources/azl/uds_engine.cpp "DebugFrame(kDirOther, src_node, dest_node, data_channel, nullptr, frame_size);" "... and the same header-only record of a frame for another node"
if grep -n 'DebugSecure(kDirOther\|DebugSecure(kDirEcho' Sources/azl/uds_engine.cpp; then
  fail "the bytes of a frame that is not for this node are in the capture again (half of its volume)"
fi
need Sources/azl/log_files.cpp "constexpr std::size_t kPassBytes = 256 * 1024;" "a pass of the capture's writer puts at most 256 KB of text on the card"
need Sources/azl/log_files.cpp "constexpr u64 kPassMs = 200;" "... and takes at most 200 ms"
need Sources/azl/log_files.cpp "if (more && !final && (written + out.size() >= kPassBytes || platform::NowMs() - begin_ms > kPassMs)) {" "both bounds end a pass that is not the last"
need Sources/azl/log_files.cpp "DebugFlushAggregates(final);" "the writer closes the counts of the diet before it drains"
# - the writer is light on the card (2026-10-20, the user: "a volte si inchioda il gioco"): the capture goes to the card in writes of 64 KB
#   (16 KB until then) and is flushed ONCE a pass (every write was flushed until then: a flush costs the card as much as a small write), the
#   main log at every write (its last lines are what a post-mortem needs); the console's file layer flushes only in Flush(); a writer whose
#   pass was cut short rests as long as the card was busy (20..250 ms) before the next pass; the hex of a frame is formatted in one go
need Sources/azl/log_files.cpp "constexpr std::size_t kWriteBytes = 64 * 1024;" "the capture goes to the card in writes of 64 KB"
[ "$(grep -c 'files_->Flush(debug_file_);' Sources/azl/log_files.cpp)" = "1" ] || fail "Sources/azl/log_files.cpp: the capture is flushed in more or fewer than one place (once a pass, at the end of DrainDebug)"
grep -A4 "    if (written != 0) {" Sources/azl/log_files.cpp | grep -q "files_->Flush(debug_file_);" || fail "Sources/azl/log_files.cpp: the capture is not flushed at the end of the pass (when something was written)"
need Sources/azl/log_files.cpp "files_->Flush(log_file_); // (every time: the main log is small, and its last lines are what a post-mortem needs)" "the main log is flushed at every write"
need Sources/azl/log_files.cpp "files_->Flush(crumb_file_); // (at once: the trail is what is read after a freeze)" "the trail of a sleep is flushed at once"
[ "$(grep -c 'static_cast<File\*>(file)->Flush();' Sources/ctr/plugin.cpp)" = "1" ] || fail "Sources/ctr/plugin.cpp: the console's file layer flushes in more or fewer than one place (Flush())"
if grep -n 'f->Flush()' Sources/ctr/plugin.cpp; then
  fail "Sources/ctr/plugin.cpp: the console's file layer flushes at every write again (every 16 KB of the capture flushed the card until 2026-10-20)"
fi
need Sources/ctr/plugin.cpp "const u32 wait_ms = backlog ? rest_ms : g_debug_level > 0 ? 250 : 1000;" "a writer whose pass was cut short rests before the next pass ..."
need Sources/ctr/plugin.cpp "rest_ms = card_ms < 20 ? 20 : card_ms > 250 ? 250 : card_ms;" "... for as long as the card was busy, 20 ms at least, 250 at most"
need Sources/azl/log.cpp "out->resize(at + 2 * static_cast<std::size_t>(n) + 1);" "the hex of a frame is formatted in one go, not a char at a time"
# - the session of 2026-10-20 with Friend-A (Tekken, DOA, SSF4, NSMB2 over the internet, the log on): the writer runs ABOVE the game's
#   threads (0x2E: a thread that holds a lock the game's threads take must not be preemptable by them), a scan leaves out the networks of
#   another game (NSMB2 listed the emulator's Tekken lobby of the same room and joined it, in a loop), and with the log on a notice on
#   the screen says when it is the log that slows the game (the SD card busy, the capture dropping records)
need Sources/ctr/plugin.cpp "constexpr int kLogWriterPriority = 0x2E;" "the writer runs above the game's threads and the network thread"
need Sources/ctr/plugin.cpp "            0, kLogWriterPriority)) {" "... and that is the priority its thread is started with"
need Includes/azl/uds_engine.hpp "bool scan_filter_enforced_ = true;" "a scan leaves out the networks of another game (2026-10-20: NSMB2 listed and joined the emulator's Tekken lobby)"
need Sources/ctr/plugin.cpp "AzaharLink debug: SD card busy %lu%%, capture lost %lu%% (the log slows the game)" "with the log on the screen says when the log slows the game"
need Sources/ctr/plugin.cpp "    CheckDebugLoad(s);" "... checked every statistics window"
need Sources/ctr/plugin.cpp "    if (card_pct >= 50 || lost_pct >= 30) {" "... when the card was ours half the time, or the ring dropped almost a third"
# - no thread above the log writer ever waits for a lock of the log (2026-10-20, Tekken stuck at the publisher's screen with the log on: the
#   writer has the lowest priority, a LightLock has no priority inheritance, and a game that spins never lets the writer back on the core):
#   the writer marks itself and is the one thread that may wait; Log(), LogLine and Append() try briefly and give the line up, counted, and
#   the writer says the count in the log; the queue's room is made outside its lock
need Includes/azl/platform.hpp "void MarkLogWriterThread();" "the writer marks itself ..."
need Sources/ctr/plugin.cpp "platform::MarkLogWriterThread(); // (the one thread that may wait for the log's locks: azl/platform.hpp)" "... at the start of its thread"
need Sources/azl/log.cpp "inline bool LockLogBriefly() {" "the log's lock is tried briefly"
need_after Sources/azl/log.cpp "    if (platform::IsLogWriterThread()) {" "g_log_mutex.lock();" "... and only the writer waits for it"
[ "$(grep -c 'g_log_mutex.lock();' Sources/azl/log.cpp)" = "1" ] || fail "Sources/azl/log.cpp waits for the log's lock with lock() somewhere besides the writer's branch of LockLogBriefly"
[ "$(grep -c '    if (!LockLogBriefly()) {' Sources/azl/log.cpp)" = "2" ] || fail "Sources/azl/log.cpp: Log() and LogLine do not both go through LockLogBriefly"
[ "$(grep -c 'platform::LockGuard lock(g_log_mutex);' Sources/azl/log.cpp)" = "1" ] || fail "Sources/azl/log.cpp: a line of the log waits for the log's lock with a LockGuard (only SetLogSink, at start-up, may)"
need Sources/azl/log_files.cpp "    if (!platform::TryLockBriefly(log_mutex_)) {" "Append() tries the queue's lock briefly"
need_after Sources/azl/log_files.cpp "        if (!platform::IsLogWriterThread()) {" "__atomic_fetch_add(&append_busy_, 1u, __ATOMIC_RELAXED);" "... and a thread that is not the writer gives the line up, counted"
need Sources/azl/log_files.cpp "log: %lu lines of other threads were given up: a lock of the log was busy" "a hole in the log is never silent"
need Sources/azl/log_files.cpp "    ReportBusyDrops();" "... the writer says it at every flush"
[ "$(grep -c '    fresh.reserve(kQueueReserve);' Sources/azl/log_files.cpp)" = "2" ] || fail "Sources/azl/log_files.cpp: the room for the next lines is made under the queue's lock again (Flush and FlushWithin make it outside)"
[ "$(grep -c 'pending_.reserve(kQueueReserve);' Sources/azl/log_files.cpp)" = "1" ] || fail "Sources/azl/log_files.cpp: a reserve of the queue under its lock besides Open()'s (a malloc under the lock a game thread tries)"
# - the end of a launch: the capture and the main log are closed once the writer thread has stopped (a breadcrumb thread that is
#   stuck in a write does not keep them from it: Close() does not wait for the trail's lock), and the first line of a capture file
#   stands where the oldest record waiting for it does
need Sources/ctr/plugin.cpp "if (writer_stopped) {" "the logs are closed when the writer has stopped, whatever the breadcrumb thread is doing"
need Sources/azl/log_files.cpp "if (crumb_mutex_.try_lock()) {" "closing the logs does not wait for the trail's lock"
need Sources/azl/log_files.cpp "if (DebugOldestMs(&oldest)) {" "the first line of a capture file is not later than the first record that follows it"
need Sources/ctr/plugin.cpp "the trail of this launch begins" "the trail's file is created when its thread starts, not by the first event of a sleep"
# - the trail of the launch before a crash is the one file that reached the card (2026-11-07): a launch that finds it over 128 KB moves it aside
#   as AzaharLink.sleep.1.log (the one before it goes) instead of erasing it; its first line says which launch it is (the title, the process
#   id, the console's clock at the plugin's start); the lines of a turn of the breadcrumb thread go in one write and one flush
need Sources/azl/log_files.cpp 'constexpr const char* kCrumbOldName = "AzaharLink.sleep.1.log";' "the trail of the launches before is kept as AzaharLink.sleep.1.log"
need_after Sources/azl/log_files.cpp "        files_->Remove(dir_ + kCrumbOldName);" "        files_->Rename(dir_ + kCrumbName, dir_ + kCrumbOldName);" "a full trail is moved aside, not erased"
if grep -n -F 'files_->Remove(dir_ + kCrumbName)' Sources/azl/log_files.cpp; then
  fail "Sources/azl/log_files.cpp erases the trail of the launches before (it is moved aside as AzaharLink.sleep.1.log)"
fi
need Sources/ctr/plugin.cpp "    start_os_ms_ = osGetTime();" "the console's clock at the plugin's start is kept for the trail"
need Sources/ctr/plugin.cpp 'id_line.Str("title ").Hex(static_cast<u32>(game_id_ >> 32), 8).Hex(static_cast<u32>(game_id_), 8).Str(" pid 0x").Hex(process_id_);' "the trail's first line names the title and the process"
need Sources/ctr/plugin.cpp " | plugin started at console clock " "the trail's first line says when the plugin started"
[ "$(grep -c 'log_files_->WriteCrumb(' Sources/ctr/plugin.cpp)" = "0" ] || fail "Sources/ctr/plugin.cpp writes the trail a line at a time (a write and a flush each: WriteCrumbs takes a turn's lines in one)"
[ "$(grep -c 'log_files_->WriteCrumbs(lines.data(), lines.size());' Sources/ctr/plugin.cpp)" = "2" ] || fail "Sources/ctr/plugin.cpp: the breadcrumb thread's two turns (the first and the loop) do not write their lines in one"
[ "$(grep -c 'files_->Flush(crumb_file_);' Sources/azl/log_files.cpp)" = "1" ] || fail "Sources/azl/log_files.cpp flushes the trail in more or fewer than one place (once a turn, WriteCrumbs)"
# - the main log's queue full (the card behind): the lines dropped are counted, said by the writer's next flush (the line goes in past the full
#   queue) and kept in a total that the stats line shows
need_after Sources/azl/log_files.cpp "    } else {" "        __atomic_fetch_add(&queue_full_, 1u, __ATOMIC_RELAXED);" "a line dropped because the queue is full is counted"
need Sources/azl/log_files.cpp "log: %lu lines were dropped: the queue of the main log was full" "the writer says how many lines a full queue dropped"
need Sources/azl/log_files.cpp "    if (report || pending_.size() < limits_.max_pending) {" "the report of the drops goes in past a full queue"
need Sources/ctr/plugin.cpp "lines dropped since the start: the queue was full)" "the stats line shows the lines a full queue dropped"
need_line Tests/log_files_test.cpp "    TestQueueFullIsCounted();" "the log's test fills the queue and reads the count"
# - the capture does not keep the game from leaving (0.1.17): once Shutdown begins the capture has half a second, then every pass stops after the
#   write in hand; the main log is flushed first and the lines are never dropped for the capture; a writer stuck in a write to the card gets a
#   short wait, not Close()'s drain. (On the full 128 GB card of the Zelda session, 2026-10-14, the exit took very long and the log's last lines
#   - EXIT, shutdown - were never written.)
need Includes/azl/log_files.hpp "u32 exit_capture_ms = 500;" "the capture gets half a second at the end of a launch"
need Sources/ctr/plugin.cpp "log_files_->BeginShutdown();" "Shutdown arms the capture's deadline"
need Sources/ctr/plugin.cpp "log_files_->FlushWithin(kLogStopTimeoutMs);" "a writer that did not stop in time does not keep the main log's last lines from the file"
need Sources/azl/log_files.cpp "if (deadline != 0 && platform::NowMs() >= deadline) {" "a pass of the capture stops at the deadline of the end of the launch"
need Sources/azl/log_files.cpp "shutdown: the capture was cut at the end: " "the main log says how much of the capture was not written"
# - a count line of the diet that finds the control lane full takes its calls into the lost count
need Sources/azl/log.cpp "g_dbg_totals.lost_full[kLaneControl] += count - 1;" "a count line that is lost loses the calls it stood for, and the LOST marker says so"
need Sources/ctr/plugin.cpp "backlog = log_files_->DrainDebug(false);" "a pass cut at its bound is followed soon by the next, not after the usual pause"
# - Log() and DebugEvent() write the plugin's conversions themselves (the HLE proper calls them on the game's UDS thread, where newlib's
#   vsnprintf chain needed 1.4 KB), a raw record carries a command's words to the writer, and the request copy of the capture is as long as
#   the words the header declares
need Sources/azl/log.cpp "if (printf_lite::Supported(fmt)) {" "Log() and DebugEvent() use the plugin's formatter for the formats it writes"
need Sources/ctr/uds_hle.cpp "u32 request[request_words != 0 ? request_words : 1];" "the request copy of the capture is as long as the header says, not 256 bytes"
need Tests/chain_sim/chain_sim.py "HLE_CEILING = {0: 320, 1: 520}" "the HLE proper at debug 1 is held to 520 bytes (1896 with newlib's printf under Log())"
need Tests/chain_sim/chain_sim.py "the printf family ran in the HLE proper" "the chain test fails a printf in the HLE proper"
need Sources/ctr/uds_hle.cpp "const u32 started = static_cast<u32>(platform::NowTicks());" "the hot frame passes no 64-bit argument on the stack (the canary of Tri Force Heroes' thread saw 12 bytes more with one)"
need Tests/chain_sim/chain_sim.py "PURE_WRITTEN = {0: 104, 1: 132}" "the pure path is held to what it writes below the gate's entry SP (the emulator's canary: the hotfix's 88 / 156, the gate's context +16 at debug 0)"

# The screen (0.1.16): the numbers that decide what the plugin shows the player
# - the CPU figure is CPU. Every soc call is timed (sampling one in four put the ENet share at 0 in 98% of the windows of PROVA 2),
#   each section of the network loop is its wall time less the waits that fell in it (the soc calls, the engine's lock, the pause
#   after a socket error), the writer's pass less its SD card time, and every sum is 64 bits (a 32-bit tick sum wraps after 16 s)
if grep -n 'AZL_SOC_TIMED\|Counter = 0' Sources/enet/enet_3ds.c; then
  fail "enet_3ds.c samples the soc calls again (every call is timed)"
fi
need Sources/enet/enet_3ds.c 'azl_c_soc_call (3, svcGetSystemTick () - startTick);' "the poll (the network thread idle) is timed on every call"
need Sources/enet/enet_3ds.c 'azl_c_soc_call (2, svcGetSystemTick () - startTick);' "every send is timed"
need Sources/enet/enet_3ds.c 'azl_c_soc_call (recvLength > 0 ? 0 : 1, now - startTick);' "every receive is timed"
need Sources/azl/log.cpp "platform::NoteNetWait(ticks);" "a soc call is a wait of the network loop: it comes off the section it fell in"
need Sources/azl/log.cpp "if (!platform::IsNetThread()) {" "the soc statistics are those of the network thread"
need Includes/azl/platform.hpp "NoteNetWait(waited);" "the network thread's wait for the engine's lock comes off its sections too"
need Sources/azl/room_client.cpp "cut(g_stats.t_net_service, service_end - round_start);" "ENet's own code: the service call less the soc calls in it"
need Sources/azl/room_client.cpp "cut(g_stats.t_net_events, events_end - service_end);" "the frames and the engine: less the engine-lock waits"
need Sources/azl/room_client.cpp "cut(g_stats.t_net_send, done_ticks - events_end);" "the queue and the tick: less the flush's soc calls, the lock waits and the pause"
need Sources/azl/room_client.cpp "platform::NoteNetWait(pause_to_note);" "the pause after a failing socket call is a wait, not CPU"
need Sources/azl/room_client.cpp "StatsAdd64(g_stats.t_net_wall, done_ticks - round_start);" "the wall time of the loop, to show that its pieces add up"
need Sources/ctr/plugin.cpp "StatsAdd64(g_stats.t_writer, pass_ticks - waited_ticks);" "the log writer's CPU is its pass less the SD card"
need Sources/ctr/plugin.cpp "StatsAdd64(g_stats.t_writer_wait, waited_ticks);" "... and the SD card time is counted apart"
# ... and the line keeps the two labels the analysis scripts of PROVA 1 and 2 read ("plugin total N", "poll N (waiting)"): the
# figures are CPU and waiting now, the words are the same
need Sources/ctr/plugin.cpp "| plugin total %lu.%lu (CPU) = " "the CPU line starts with 'plugin total N' (parse_main.py and the other scripts of the PROVA analyses)"
need Sources/ctr/plugin.cpp "poll %lu.%lu (waiting)," "... and gives the idle poll as 'poll N (waiting)'"
[ "$(grep -c 'CardTimer card(&card_wait_ticks_);' Sources/azl/log_files.cpp)" = "3" ] || fail "Sources/azl/log_files.cpp: the main log's flush, the capture's write and the capture's flush must both time their time in the file layer"
need Sources/ctr/uds_hle.cpp "StatsAdd64(g_stats.ipc_sum_ticks, ticks);" "the game threads' time in the commands is a 64-bit sum"
need Sources/main.cpp "azl::StatsAdd64(azl::g_stats.t_cb, svcGetSystemTick() - start);" "the frame callback's time is a 64-bit sum"
need Sources/main.cpp "            start += svcGetSystemTick() - menu_from;" "the time the in-game menu was open is not the frame callback's"
need Sources/ctr/plugin.cpp "const Stats s = TakeStats();" "the window is taken whole, the 64-bit sums read and cleared atomically"
# The chat's OSD callback draws at the game's present, outside the frame callback: its time goes into the stats window and the total (review
# of the chat, defect 34: 'plugin total 1.0 = 0.1% of a core' with about 40 ms a second of chat lines drawn on the game's thread)
need_after Sources/ctr/chat_overlay.cpp "        const u32 ticks = static_cast<u32>(svcGetSystemTick() - start);" "        StatsNoteChatOsd(ticks);" "the chat's OSD callback's time goes into the stats window"
# The keyboard's Redraw takes its time as the callback does, only with the log on (review of the chat, defect 39: svcGetSystemTick at every
# redraw at debug=0, for an Add that was skipped): at debug=0 the measure costs no svc
body=$(tr -d '\r' < Sources/ctr/chat_overlay.cpp | awk '/^void Redraw\(bool force\) \{/{f=1} f{print} f&&/^}/{exit}' | sed 's://.*$::')
printf '%s\n' "$body" | grep -F -x -q -- "    const u64 start = timed ? svcGetSystemTick() : 0;" || fail "Sources/ctr/chat_overlay.cpp: Redraw no longer reads the clock only with the log on"
printf '%s\n' "$body" | grep -F -A1 -- "    g_drawn_valid = true;" | grep -F -x -q -- "    if (timed) {" || fail "Sources/ctr/chat_overlay.cpp: Redraw's cost is no longer noted only with the log on"
# The chat's counters go into the debug log's line of the costs, also when nothing was drawn (review of the chat, defect 42: counted and never
# read, a message dropped by the rate limit, malformed or refused looked the same in the log as one never sent)
need Sources/ctr/chat_overlay.cpp "| messages: taken %lu, over the rate %lu, malformed %lu, empty %lu | ours: sent %lu, refused %lu (not in the room %lu, by the \"" "the chat's line of the costs gives its counters, the refusals by reason ..."
need Sources/ctr/chat_overlay.cpp "\"room's connection %lu) | OSD notifies: chat %lu, plugin %lu\"," "... and the OSD-notify count"
need_line Sources/ctr/chat_overlay.cpp "    const unsigned long taken = get(k.taken), drops = get(k.rate_drops), malformed = get(k.malformed), empty = get(k.empty);" "the chat's line reads the counters of the room's messages"
need_line Sources/ctr/chat_overlay.cpp "    const unsigned long sent = get(k.sent), refused = get(k.refused), refused_left = get(k.refused_left);" "the chat's line reads the counters of our messages"
need_line Sources/ctr/chat_overlay.cpp "        malformed, empty, sent, refused, refused_left, refused - refused_left, StatsChatOsdNotifies(), StatsPluginOsdNotifies());" "the chat's line is given its counters and the OSD-notify counts (the chat's and the plugin's, apart)"
need_line Sources/ctr/chat_overlay.cpp "    if (g_cost_top.n.load() == 0 && g_cost_bottom.n.load() == 0 && g_cost_redraw.n.load() == 0 && counted == g_counted_logged) {" "the chat's line is written when a counter moved, nothing drawn"
need_line Sources/ctr/chat_overlay.cpp "        LogCosts(now_ms, *sink); // (on: the sink is there)" "the chat's line is given the sink's counters"
need_line Sources/azl/log.cpp "    c.plugin10 = c.net10 + c.uds10 + c.frame10 + c.chat10;" "the plugin's total holds the chat's OSD callback"
need Sources/azl/log.cpp "    out.t_chat_osd = take(g_stats.t_chat_osd);" "TakeStats takes the chat's OSD sum"
need_after Sources/azl/log.cpp "    if (ticks > g_stats.chat_osd_max_ticks) {" "        g_stats.chat_osd_max_ticks = ticks;" "the stats window keeps the longest draw of the chat's OSD callback"
need Sources/ctr/plugin.cpp "frame callback %lu.%lu, chat osd %lu.%lu (%lu, max %lu us)" "the stats line shows the chat's OSD callback beside the frame callback"
if grep -n 'const Stats s = g_stats' Sources/ctr/plugin.cpp; then
  fail "the statistics window is copied from g_stats again (a 64-bit sum can be read in two halves): TakeStats()"
fi
for sum in t_net_service t_net_events t_net_send t_cb t_chat_osd t_writer ipc_sum_ticks t_net_wall t_net_pause t_writer_wait lock_wait_ticks lock_wait_net_ticks frame_gap_sum; do
  grep -q "Ticks64 $sum;" Includes/azl/stats.hpp || fail "Includes/azl/stats.hpp: $sum is not a 64-bit sum (Ticks64) any more"
done
need Includes/azl/stats.hpp "Ticks64 soc_ticks[kSocKinds];" "the soc time is a 64-bit sum"
need Includes/azl/stats.hpp "static_assert(sizeof(Stats) - offsetof(Stats, t_net_service) ==" "TakeStats clears every sum after t_net_service, and the struct ends with them"
# - the memory the game has left: read about 20 s after the start (the game is done allocating) and every 30 s, one toast, a warning
#   only on a fall below the limit (ONE limit for both layouts: 5 MB, the size of the plugin's own block; no other number is known
#   for an Old 3DS), and no "Memory margin low" for a game that is promised more than is free (Tri Force Heroes: 56,320 KB free
#   all session)
need Includes/azl/screen_text.hpp "constexpr long kLowKb = 5 * 1024;" "the limit of the memory warning, the same on a New and an Old 3DS: the size of the plugin's own block"
if grep -n 'kLowNewKb\|kLowOldKb\|LowLimitKb' Includes/azl/screen_text.hpp Sources/ctr/plugin.cpp; then
  fail "a second, per-layout limit of the memory warning is back (one limit, 5 MB, for both layouts)"
fi
need Includes/azl/screen_text.hpp "constexpr u32 kFirstReadingMs = 20000;" "the first reading is 20 s after the start"
need Includes/azl/screen_text.hpp "constexpr u32 kRefreshMs = 30000;" "... then every 30 s"
need Includes/azl/screen_text.hpp 'const std::string mode = std::string(r.new_layout ? "New 3DS" : "Old 3DS") + " memory mode: ";' "the texts of the toast of the start"
need Includes/azl/screen_text.hpp 'return AllInUse(r) ? mode + "all memory in use" : mode + FreeText(r.free_kb) + " free";' "a game that takes the whole region reads 'all memory in use', not '0 KB free'"
need Includes/azl/screen_text.hpp "constexpr long kAllInUseKb = 256;" "under 256 KB free the texts say that all the memory is in use"
need Includes/azl/apt_sleep.hpp "return reply_word & 0xFFu;" "the APT signal is the low byte of the reply word (0x1BC00001 is the HOME button in Mario Party)"
need Sources/ctr/hook.cpp "const u32 id = AptSignalOf(raw);" "the hook classifies the notification by its low byte"
need Includes/azl/screen_text.hpp 'return "Game memory almost full: " + FreeText(r.free_kb) + " free";' "the text of the warning"
if grep -rn 'Memory margin low\|AzaharLink uses\|memory margin low' Sources Includes ConfigApp/source; then
  fail "the alarm about a memory margin (a game promised more than is free), or a toast about what AzaharLink takes, is back"
fi
need Sources/ctr/plugin.cpp "if (!mem_watch_.Due(uptime)) {" "the reading waits for its moment"
need Sources/ctr/plugin.cpp "const std::string text = gamemem::InfoText(reading);" "the toast of the start says the memory mode and the figure"
need Sources/ctr/plugin.cpp "const std::string text = gamemem::LowText(reading);" "... and a figure below the limit says so instead"
need_after Sources/ctr/plugin.cpp "void Plugin::CheckGameMemory(u64 now_ms) {" "if (!active_) {" "a plugin that is off for this launch has nothing to say about the game's memory"
need Sources/ctr/plugin.cpp "socplan::StartupIssue(SocketBufferSize(), g_debug_level > 0)" "the start-up text drops the pointer to the log when the log is on"
# - The list of the games that cannot spare the socket buffer while they size their heap (Monster Hunter XX): not there before their first instruction; one outside the budget goes on the kernel's books as it is taken, or a game closed from HOME on an Old 3DS leaves the next one without a budget
need Sources/ctr/plugin.cpp '(late || ReserveSocketMemory())' "a title of the list does not take the socket buffer before the game's first instruction"
need Sources/ctr/soc.cpp 'if (g_reserve_late && FindBooks()) {' "a block outside the budget only for a title of the list, and only with the kernel's books found on the live object"
#   ... on the books BEFORE it is taken (a process that dies in between keeps the block's bytes too many on current, never a sum of the private blocks above
#   current, which gives the whole budget back to nobody), and off them again when it is not had (refused, or at another address: freed with svc 0xA3's FREE,
#   which does not touch the books, where svcControlMemory's FREE would release a commit the kernel never charged)
need_order Sources/ctr/soc.cpp '    if (outside_budget && !ChargeBooks(size)) {' '    const Result rc = outside_budget' "the block outside the budget goes on the kernel's books before svc 0xA3 takes it"
need_after Sources/ctr/soc.cpp '    if (outside_budget && !ChargeBooks(size)) {' '        return Try::Stop;' "a charge that could not be made does not go on to take the block outside the books"
need_after Sources/ctr/soc.cpp '        return Try::Allocated;' '        UnchargeBack(size);' "a block outside the budget that was refused comes off the books"
need_after Sources/ctr/soc.cpp '            if (outside_budget) {' '                svcControlMemoryUnsafe(&ignored, out, size, MEMOP_FREE, static_cast<MemPerm>(0));' "a block outside the budget at another address is freed with svc 0xA3, which does not touch the books"
need_after Sources/ctr/soc.cpp '                svcControlMemoryUnsafe(&ignored, out, size, MEMOP_FREE, static_cast<MemPerm>(0));' '                UnchargeBack(size);' "... and comes off the books"
[ "$(tr -d '\r' < Sources/ctr/soc.cpp | sed 's://.*$::' | grep -c 'UnchargeBack(size);')" = "2" ] || fail "Sources/ctr/soc.cpp: the charge of a block outside the budget is taken back in another number of places than the two where the block is not had"
[ "$(grep -rn --include='*.cpp' 'UnchargeBooks(' Sources | sed 's://.*$::' | grep -c 'UnchargeBooks(')" = "2" ] || fail "UnchargeBooks is called elsewhere than soc.cpp's UnchargeBack (its definition and that one call)"
need_after Sources/ctr/soc.cpp '    if (g_reserve_late && FindBooks()) {' '        if (AllocateBlock(socplan::kSocBlockSize, true, &p)) {' "the late block outside the budget is the one AllocateBlock takes through TryAt's charge"
# - frames lost: the text depends on the game's UDS calls (no call in the window, or none in its last second: not reading)
need Includes/azl/screen_text.hpp "constexpr u32 kNotReadingMs = 1000;" "a game that has made no UDS call for a second is not reading"
need Includes/azl/screen_text.hpp "constexpr unsigned long kWarnAbove = 50;" "more than 50 frames in a window is worth a toast"
need Sources/ctr/plugin.cpp "frameslost::NotReading(s.ipc_requests, idle_ms)" "the text is chosen from the game's UDS calls"
need Sources/ctr/plugin.cpp "frameslost::Text(s.ring_overwritten, not_reading)" "... and built in one place"
if grep -n 'reads too slowly' Sources/ctr/plugin.cpp; then
  fail "plugin.cpp says \"reads too slowly\" by itself: the text is frameslost::Text's, chosen from the game's UDS calls"
fi
need Sources/ctr/soc.cpp 'g_debug_level > 0 ? "Could not start the network layer (0x%08lx)"' "no pointer to the log when the log is on"
need Sources/game_menu.cpp "plugin.startup_issue().empty()" "the menu of a plugin that stayed out of the game says why"

# No management frame is ever sent into the room (Azahar asserts on one): the only builder of a data frame
# writes a zero management byte (the rule checker of the unit tests watches every frame the engines send)
need Sources/azl/uds_frames.cpp "h[6] = 0; " "data frames carry no management bit"

# The socket buffer: 64 KB is a hard limit, SO_SNDBUF / SO_BROADCAST are not touched
need Includes/azl/rcvbuf.h "#define AZL_RCVBUF_MAX 65536" "more than 64 KB makes the soc service fail or crash"
if grep "setsockopt" Sources/enet/enet_3ds.c | grep -q "SO_SNDBUF\|SO_BROADCAST"; then
  fail "enet_3ds.c sets SO_SNDBUF or SO_BROADCAST (the soc service answers errno 109 for UDP)"
fi

# ---- Nothing on a GAME thread formats text with the printf family ------------------------------------------------------
# The svc hook runs the plugin's C++ on the stack of whatever thread of the game made an IPC call, and some of those stacks
# are tiny (Tri Force Heroes' srv-notification thread: 464 bytes below its call). 0.1.15 at debug=1 ran DebugEvent ->
# vsnprintf (~1.3 KB of stack) there and the game crashed on exit (hardware, 2026-10-10). The code that can run on a game
# thread is marked in the sources with  // >>> game-thread code  ...  // <<< game-thread code  and this scan forbids in it:
#   - the printf family (printf, snprintf, vsnprintf, sprintf, fprintf, ...), Log( and DebugEvent( (they take a format and
#     format on the caller's stack; the printf-free LogLine / DebugLine / DebugIpc / DebugSvcReal take their place),
#   - any char buffer over 64 bytes, std::string / std::vector / new / malloc (nothing allocated on a game thread),
# String literals are blanked first (a word inside a quoted text is not code), then the // comments. The buffer rule reads
# `char` arrays only: a big array of another type (RunTraced's u32 request[64], 256 bytes, is in a region on purpose) is held by
# the frame budgets of Tests/stack_usage.sh.
# The marked code is what the hook reaches before the HLE proper, written out here (this scan is lexical; the EXACT answer,
# from the machine code, is Tests/callgraph_check.py, which Tests/stack_usage.sh runs on the built ELF - it follows direct
# calls, tail calls and function addresses from the entries of the hook and from the callbacks the hook reaches through a
# pointer, and fails if the printf family, Log() or DebugEvent() is among them):
#   Sources/ctr/hook.cpp         the whole hook: PassThroughPre/Post and their branches, PassThroughAptPre/Post, FindTraced,
#                                WorthLogging, the name tests, azl_pre32/azl_post32/azl_pre23/azl_post23 (what the gate calls)
#   Sources/ctr/uds_hle.cpp      the front of the IPC front-end: FakeKind, MarkInitialized, ForgetFakeHandle,
#                                NoteHandleClosed, InterceptGetServiceHandle, HandleSendSyncRequest; and RunTraced
#   Sources/ctr/plugin.cpp       the observers of the game's APT session: OnSleepEnter (and SleepActs), OnWake, OnHomeButton,
#                                NoteSleepEvent (the trail of a sleep)
#   Includes/azl/apt_sleep.hpp   AptSleepEventOf (PassThroughApt)      Includes/azl/link_policy.hpp  SleepWatch::Enter, Woke
#   Includes/azl/sleep_crumbs.hpp  SleepCrumbs::Push (the ring NoteSleepEvent stores into)
#   Sources/ctr/uds_hle.cpp      also ThrottleFailing (the pause of a failing reply of a polling game thread)
#   Sources/azl/uds_engine.cpp   what the front reaches in the engine: the role line (TraceRoleLocked) and StopService
#                                (UdsHle::NoteHandleClosed -> Shutdown, OnSleepEnter -> StopService)
#   Sources/azl/room_client.cpp  DebugWifi (the Deauthentications that ending a session sends)
#   Sources/azl/ndm_hle.cpp      NdmHle::OpenSession      Sources/azl/platform.cpp  the clocks
#   Sources/azl/log.cpp          what they call in the log: the producers of the capture - DebugIpc, DebugSvcReal, DebugLine,
#                                DebugCommand, DebugServiceCommand, DebugPullPacket, DebugWifiLine, DebugFrame, DebugBeacon, their ring
#                                (the lanes, the lost markers, the diet, the try-lock) - and LogLine
#   Sources/azl/log_files.cpp    LogFiles::Append (the sink of the log)    Includes/azl/line.hpp  LineBuf
#   Includes/azl/printf_lite.hpp the formatter Log() and DebugEvent() use for the formats it writes (no printf inside it)
# NOT in the set, and allowed to log with formats: UdsHle::Run -> Dispatch / RunNdm / NoteCommand and the engine below
# them - the HLE proper, which a fake session's command reaches. At debug 0 Log() and DebugEvent() cost 32 bytes; at debug 1
# the HLE proper needs 464 bytes since 0.1.16 (1896 before): the formats it logs are written by printf_lite.hpp, not newlib's
# vsnprintf (a format printf_lite does not write still goes to newlib: Tests/line_test.cpp holds every format of the
# sources to the ones it writes), and the lines of the capture are raw words for the writer thread.
GT_BEGIN='^[[:space:]]*// >>> game-thread code'
GT_END='^[[:space:]]*// <<< game-thread code'
game_thread_file() { # $1 = file, $2 = how many regions it must have
  local begins ends
  begins=$(tr -d '\r' < "$1" | grep -c "$GT_BEGIN")
  ends=$(tr -d '\r' < "$1" | grep -c "$GT_END")
  [ "$begins" = "$2" ] && [ "$ends" = "$2" ] || { fail "$1 has $begins region starts and $ends ends of 'game-thread code', expected $2 (the scan for printf on game threads reads only what is between them)"; return; }
  # the lines of the regions (no CR, string literals blanked, no // comments), numbered; a region that starts inside another
  # or never ends is an error
  local text
  text=$(tr -d '\r' < "$1" | awk -v b="$GT_BEGIN" -v e="$GT_END" '
      $0 ~ b { if (on) print "UNBALANCED"; on = 1; next }
      $0 ~ e { if (!on) print "UNBALANCED"; on = 0; next }
      on { line = $0; gsub(/"([^"\\]|\\.)*"/, "\"\"", line); sub(/\/\/.*$/, "", line); print FNR ": " line }
      END { if (on) print "UNBALANCED" }')
  case "$text" in *UNBALANCED*) fail "$1: the 'game-thread code' markers are not balanced"; return ;; esac
  local hit
  hit=$(printf '%s\n' "$text" | grep -E '\b(printf|snprintf|sprintf|vsnprintf|vsprintf|fprintf|vfprintf|asprintf|puts|sscanf)[[:space:]]*\(|\bLog[[:space:]]*\(|\bDebugEvent[[:space:]]*\(|azl_c_log|std::(to_string|stringstream|ostringstream|string|vector)|\bnew\b|\b(malloc|calloc|realloc)[[:space:]]*\(' | head -3)
  [ -z "$hit" ] || fail "$1 formats or allocates on a game thread (printf family, Log/DebugEvent with a format, heap): $hit"
  # (the static buffers the lines are built in are named g_*, as every static of the plugin: they are not on the stack)
  hit=$(printf '%s\n' "$text" | awk '{ s = $0; while (match(s, /char[[:space:]]+[A-Za-z_][A-Za-z0-9_]*(\[[[:space:]]*[0-9]+[[:space:]]*\])+/)) { m = substr(s, RSTART, RLENGTH); name = m; sub(/^char[[:space:]]+/, "", name); n = m; sub(/^[^\[]*\[[[:space:]]*/, "", n); if (n + 0 > 64 && name !~ /^g_/) print $0; s = substr(s, RSTART + RLENGTH) } }' | head -2)
  [ -z "$hit" ] || fail "$1 has a char buffer over 64 bytes on a game thread: $hit"
}
game_thread_file Sources/ctr/hook.cpp 1
game_thread_file Sources/ctr/uds_hle.cpp 2
game_thread_file Sources/ctr/plugin.cpp 1
game_thread_file Sources/azl/log.cpp 4
game_thread_file Sources/azl/log_files.cpp 1
game_thread_file Includes/azl/line.hpp 1
game_thread_file Sources/azl/uds_engine.cpp 2
game_thread_file Sources/azl/room_client.cpp 1
game_thread_file Includes/azl/printf_lite.hpp 1
game_thread_file Includes/azl/apt_sleep.hpp 2
game_thread_file Includes/azl/traced_sessions.hpp 1
game_thread_file Includes/azl/link_policy.hpp 1
game_thread_file Includes/azl/sleep_crumbs.hpp 1
game_thread_file Sources/azl/ndm_hle.cpp 1
game_thread_file Sources/azl/latency.cpp 2
game_thread_file Includes/azl/chat_core.hpp 2
# ... and the chat's drawing (the OSD callback runs on the game's thread that presents a frame): the stores and the text, and the lines
game_thread_file Includes/azl/gfx_surface.hpp 1
game_thread_file Includes/azl/chat_draw.hpp 1
# ... and the chat over the running game: the hook's count of the notifications, the handshake of the keyboard's picture and the callback's
# drawing (Includes/azl/chat_host.hpp), and the callback itself (Sources/ctr/chat_overlay.cpp)
game_thread_file Includes/azl/chat_host.hpp 4
game_thread_file Sources/ctr/chat_overlay.cpp 1
# ... and the input mirror's way back, which the hook runs on the game's thread that got a notification (Sources/ctr/input_mirror.cpp,
# MirrorRestoreFromGame and the kernel's calls under it; Includes/azl/hid_mirror.hpp, the Switcher)
game_thread_file Includes/azl/hid_mirror.hpp 1
game_thread_file Sources/ctr/input_mirror.cpp 1
# ... and the zombie probe's one game-thread function (ctr/zombie_probe.cpp, BackdoorOk, reached from the hook's ipc-xfer line): no Log, no heap
game_thread_file Sources/ctr/zombie_probe.cpp 1
game_thread_file Sources/azl/platform.cpp 1
# ... and the three headers of the fake Download Play lobby (pure, header only: one region each, which is all of the code in them)
game_thread_file Includes/azl/dlp_types.hpp 1
game_thread_file Includes/azl/dlp_lobby.hpp 1
game_thread_file Includes/azl/dlp_ipc.hpp 1
# ... and the code on the game's threads does call the printf-free lines (the scan above would also pass if the lines were
# simply dropped): the two pass-through lines are stored raw, the others are built piece by piece
need Sources/ctr/hook.cpp "DebugIpc(handle, header, arg," "the pass-through line of every IPC call is stored as raw words"
need Sources/ctr/hook.cpp "DebugSvcReal(traced->name, command, arg0, result, ctx[0]);" "the line of a traced service is stored as raw words, its session's handle given to the diet"
need Sources/azl/log.cpp "    if (result != 0 || !DietLocked(handle, command | kDietSvcReal, arg, rec.ms, payload.service)) {" "the lines of a traced service are under the diet of the IPC lines (Fantasy Life: 90 a second of frd:u)"
[ "$(grep -c 'g_home_observer();' Sources/ctr/hook.cpp)" = "2" ] || fail "Sources/ctr/hook.cpp: the HOME button must wake the log writer in both places of PassThroughApt (after the notification's lines, and when the lines are over)"
need Sources/ctr/uds_hle.cpp "DebugServiceCommand(uds, id, request," "the capture of a command is built without printf"
need Sources/ctr/uds_hle.cpp "DebugPullPacket(request[0]," "the capture of a PullPacket is built without printf"
need Sources/azl/uds_engine.cpp 'line.Str("uds: role ")' "the role line of the engine is built without printf (a game thread that closes its session gets there)"
need Sources/azl/uds_engine.cpp 'line.Str("uds: the service stops (")' "the sleep's StopService line is built without printf"
need Sources/azl/room_client.cpp "DebugWifiLine(dir," "the capture line of a frame that ends a session is built without printf"
# Log() and DebugEvent() keep the 400 / 200-byte line out of their own frame (a call at debug 0 costs a few words of stack)
need_after Sources/azl/log.cpp "void DebugEvent(const char* fmt, ...) {" "if (g_debug_level <= 0 || !g_dbg_ring) {" "DebugEvent() returns at once when the capture is off"
need Sources/azl/log.cpp "__attribute__((noinline)) static void LogV(const char* fmt, va_list args) {" "Log()'s 400-byte line is in a frame of its own"
need Sources/azl/log.cpp "__attribute__((noinline)) static void DebugEventV(const char* fmt, va_list args) {" "DebugEvent()'s 200-byte line is in a frame of its own"

# The gates that see this class: Tests/stack_usage.sh runs the whole-chain test, and the chain test holds the pass-through
# path to 300 bytes at both log levels (a frame budget alone missed the 944 bytes of libc below DebugEvent)
need Tests/stack_usage.sh 'chain_sim.py "$WORK/AzaharLink.elf"' "the stack check runs the whole-chain test on the built plugin"
need Tests/stack_usage.sh 'callgraph_check.py "$WORK/cg"' "the stack check asks the machine code whether a formatter can be reached from a game thread"
need Tests/stack_usage.sh 'callgraph_check.py --selftest' "the call graph checker is tried against a made-up listing first"
need Tests/chain_sim/chain_sim.py "BUDGET_PURE = 300 " "the pure pass-through path is held to 300 bytes below the entry SP, at debug 0 and 1"
need Tests/chain_sim/chain_sim.py "for level in (0, 1):" "the chain test runs at both log levels"
need_line Tests/stack_usage.sh 'budget hook "azl::ctr::PassThroughPre" 32' "the hot frame of every IPC call stays small (the half before the svc: PassThrough was split in pre and post for the gate)"

# Test hygiene. A test that no script runs is a gate that is not there: every test source is built AND run by the script
# that owns it (the host tests by Tests/run.sh, the 32-bit ones, which drive UdsHle, by Tests/ipc_test/build.sh and then
# Tests/hle_tests.sh and Tests/integration.sh), and a new *_test.cpp that is in none of them fails here
for t in test_main ring_test dedupe_test link_policy_test soc_plan_test screen_text_test log_files_test line_test sleep_crumbs_test capture_test dlp_lobby_test dlp_ipc_test dlp_lobby_sim_test dlp_adv_lobby_test dlp_adv_ipc_test dlp_net_test dlp_net_adv_test dlp_net_churn_test dlp_net_extra_test dlp_real_engine_test dlp_real_net_test dlp_real_client_test dlp_real_hub_test dlp_hub_test dlp_hub_test_tsan plugin_dlp_test chat_core_test chat_core_test_tsan chat_draw_test osk_test chat_host_test chat_host_test_tsan hid_mirror_test settings_test games_test paths_test plugin_install_test eta_test dlp_text_test dlp_prepare_test guide_test about_test update_test logrot_test log_module_test ncch_icon_test hook_swap_test hook_swap_test_tsan gate_plan_test kernel_books_test gsp_wrapper_test traced_sessions_test traced_sessions_test_tsan heap_plan_test; do
  tr -d '\r' < Tests/run.sh | grep -q -E "^  (timeout 1200 )?\"\\\$OUT/$t\"( |\$)" || fail "Tests/run.sh does not run the unit test $t (a line of its own that calls \"\$OUT/$t\")"
  need Tests/run.sh "-o \"\$OUT/$t\"" "the unit test $t is built by Tests/run.sh"
done
for t in ffe_replay_test trace_replay_test result_closure_test command_matrix_test ndm_wiring_test dlp_fkcl_test dlp_lobby_hle_test; do
  need Tests/ipc_test/build.sh "$t" "the 32-bit test $t is built by Tests/ipc_test/build.sh"
  need Tests/hle_tests.sh "\"\$OUT/$t\"" "the 32-bit test $t is run by Tests/hle_tests.sh"
  need Tests/integration.sh "/tmp/azl_tests32/$t" "the 32-bit test $t is run by Tests/integration.sh"
done
for src in Tests/*_test.cpp Tests/settings_test/*_test.cpp; do
  name=$(basename "$src" .cpp)
  grep -w -q -- "$name" Tests/run.sh Tests/ipc_test/build.sh Tests/hle_tests.sh || fail "$src is built by no script (Tests/run.sh, Tests/ipc_test/build.sh)"
done
# The golden data of the replays are made from the real captures by Tests/make_goldens.py and carry its name: a file edited by
# hand, or one that lost its origin, fails here (run the script again with the captures instead)
for f in Tests/ffe_host_golden.inc Tests/tfh_host_golden.inc Tests/lobby_trace.inc; do
  head -1 "$f" | grep -q "Generated by Tests/make_goldens.py" || fail "$f is not marked as generated by Tests/make_goldens.py"
done
# The golden data of the real Download Play (RealDlp, 1.1.3) are cut out of the capture 1c by Tests/make_dlp_real_goldens.py, with the game's
# titles and icon replaced by the sum of their words (what is Nintendo's stays out of the repository; the checksums still hold)
head -1 Tests/dlp_real_luigi_golden.inc | grep -q "^// Generated by Tests/make_dlp_real_goldens.py from the capture 1c" || fail "Tests/dlp_real_luigi_golden.inc is not marked as generated by Tests/make_dlp_real_goldens.py"
need Tests/dlp_real_luigi_golden.inc '{"F0", "770.168", 1, 1, 65535, 768, kDlpRealLuigi_00_F0, 0xa85b0d4c, 0xbdfff7f2, {0x40, 0x2f8}},' "frame 0 of the announcement carries no title and no icon of the game (+0x40..+0x2F7 is its sum only)"
# 1.1.3, phase F1: the codec of the real Download Play (azl/dlp_real_wire.hpp / .cpp) and the ONE file of the Download Play host library it builds on.
# - the plugin's Makefile compiles dlph_wire.c of ConfigApp/dlph by its name alone: never the folder (dlph.c, the app's host, stays out of the plugin)
need_line Makefile $'DLPH_WIRE\t:=\tdlph_wire.c' "the plugin builds one file of the Download Play host library, by name"
need_line Makefile 'vpath $(DLPH_WIRE) $(TOPDIR)/ConfigApp/dlph' "dlph_wire.c is found by its name in ConfigApp/dlph, the folder is not a source folder"
need Makefile '$(notdir $(wildcard $(dir)/*.c))) $(DLPH_WIRE)' "dlph_wire.c is among the C files of the plugin"
[ "$(tr -d '\r' < Makefile | sed 's/#.*$//' | grep -c 'ConfigApp')" = "1" ] || fail "the plugin's Makefile names ConfigApp somewhere else than the vpath of dlph_wire.c (a source folder of the app, or another of its files, would go into the plugin)"
! tr -d '\r' < Makefile | sed 's/#.*$//' | grep -E '^(SOURCES|INCLUDES)[[:space:]]' | grep -q -E 'ConfigApp|dlph' || fail "the plugin's SOURCES or INCLUDES name the app's folders (dlph.c would be built into the plugin)"
need_line Sources/azl/dlp_real_wire.cpp '#include "../../ConfigApp/dlph/dlph_wire.h"  // (one file of the library, by name: the Makefile builds dlph_wire.c and nothing else of ConfigApp/dlph)' "the codec reaches the library through its wire header alone"
hit=$(grep -r -n -E '#include[[:space:]]*"[^"]*(ConfigApp|dlph)' Sources Includes --include=*.cpp --include=*.c --include=*.hpp --include=*.h | grep -v '^Sources/azl/dlp_real_wire.cpp:' | head -3)
[ -z "$hit" ] || fail "something else of the plugin includes a file of the app or of the Download Play host library (only azl/dlp_real_wire.cpp includes dlph_wire.h): $hit"
# - what we send is sealed with the constant seed 0 (design 2.5, decision written in REGISTRO); the step of the checksum is the library's, explained where the codec says it
need_line Includes/azl/dlp_real_wire.hpp 'inline constexpr u8 kTxSeed[4] = {0, 0, 0, 0};' "the seed of the checksum of what the plugin sends is the constant 0 (design 2.5)"
need Includes/azl/dlp_real_wire.hpp 'h = ((h >> s) | (h << s)) ^ BE32(seed) with s = (seed[2] & 0xF) + 4. That step is NOT a rotation' "the codec says that the checksum's step is not a rotation, so that nobody corrects it"
# - the notice A8 is Azahar's (index 0, 0x18 bytes, 1 and 9): the library's begin_ping (index 1, 0x20, +0x1D = 8) is an empty final to an Azahar client and is never used by the plugin
[ "$(grep -r -c 'dlph_pk_begin_ping' Sources Includes | awk -F: '{s+=$2} END {print s+0}')" = "0" ] || fail "the plugin uses dlph_pk_begin_ping (an Azahar client takes it for the final with an empty passphrase: design 2.4)"
need_after Sources/azl/dlp_real_wire.cpp 'std::size_t Notice(u8* buf, const u8 seed[4], u8 seq, const u8 rid[2]) {' 'Header(buf, kTypeBegin, kNoticeBytes, 0, seq, rid[0], rid[1]);' "the notice is index 0, 0x18 bytes"
# - the codec is pure: no log, no printf family, no heap, no static storage of its own
hit=$(tr -d '\r' < Sources/azl/dlp_real_wire.cpp | sed 's://.*$::' | grep -n -E '\bLog\(|DebugEvent\(|printf|\bnew\b|malloc|std::vector|std::string|^[[:space:]]*static[[:space:]]+[A-Za-z_:<>0-9]+[[:space:]]+[A-Za-z_0-9]+[[:space:]]*(\[|=|;)' | head -3)
[ -z "$hit" ] || fail "Sources/azl/dlp_real_wire.cpp logs, allocates or keeps static storage (it is a pure codec): $hit"
# - the suite builds and runs its test (W1..W10), with the sanitizers, and the test stops at the first undefined behaviour
need Tests/run.sh 'g++ $CXXFLAGS Tests/dlp_real_wire_test.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" -o "$OUT/dlp_real_wire_test"' "the suite builds the codec's test against the library's dlph_wire.c"
need Tests/run.sh 'gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -c ConfigApp/dlph/dlph_wire.c -o "$OUT/dlph_wire.o"' "the library's file is built with the sanitizers for the codec's test"
need_stmt Tests/run.sh 'timeout 1200 "$OUT/dlp_real_wire_test"' "the suite runs the codec's test"
need Tests/dlp_real_wire_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "the codec's test stops at the first undefined behaviour"
need Tests/dlp_real_wire_test.cpp '#include "dlp_real_luigi_golden.inc"' "the codec is tested against the golden bytes of the capture 1c"
# 1.1.3, phase F2: the engine's half of the real Download Play (design E1/D13, D11, D12, D16; Tests/dlp_real_engine_test.cpp).
# - the beacons of the real Download Play (its wlancommid, NO application data, any id8 / network id / attributes) are lobbies only under
#   SetLobbyBeaconKind(RealDlp): an engine starts with the dialect's kind, whose rule stays the text it was
need Includes/azl/uds_engine.hpp 'LobbyBeaconKind lobby_beacon_kind_ = LobbyBeaconKind::Ours;' "an engine files the dialect's beacons until the plugin says the game is of the real Download Play"
need Sources/azl/uds_engine.cpp '        lobby = (!GetBeaconApplicationData(tags, &data, &size) || size == 0) && tags.network_size > kMaxAt &&' "a real Download Play's beacon has no application data (a tag too short for the size byte counts as none) and the node counts"
need_after Sources/azl/uds_engine.cpp '        lobby = (!GetBeaconApplicationData(tags, &data, &size) || size == 0) && tags.network_size > kMaxAt &&' '                LoadBE32(tags.network + 4) == kLobbyWlanCommId;' "...and the Download Play's wlancommid, and nothing else of the beacon (Azahar leaves id8, network id and attributes uninitialised)"
need Sources/azl/uds_engine.cpp '    } else if (!GetBeaconApplicationData(tags, &data, &size) || size == 0 || tags.network_size <= kMaxAt ||' "the dialect's kind keeps its own rule (the text it had), and the real kind is a branch of its own before it"
need_after Sources/azl/uds_engine.cpp '    if (copied != 0) {' 'std::memcpy(slot->lobby.data, data, copied);' "no memcpy from the null pointer of a beacon with no data"
# - the network the engine composes for it: the Download Play's ids, no application data, never the lobby's inbox; its flag is switched on by
#   those two calls only, off by Shutdown only (the bodies are read without their comments; region() and code_of() come later in this file)
[ "$(grep -c 'dlp_net_ = true;' Sources/azl/uds_engine.cpp)" = "2" ] || fail "Sources/azl/uds_engine.cpp: the real Download Play's network begins in two places (BeginHostingDlp, ConnectBeginDlp)"
[ "$(grep -c 'dlp_net_ = false;' Sources/azl/uds_engine.cpp)" = "1" ] || fail "Sources/azl/uds_engine.cpp: the rules of the real Download Play's network end in one place (Shutdown)"
need_after Sources/azl/uds_engine.cpp "    lobby_inbox_ = false; // (the lobby's end" '    dlp_net_ = false;' "Shutdown ends the rules of the real Download Play's network where it switches the lobby's inbox off"
r=$(awk -v a='u32 UdsEngine::BeginHostingDlp(u8 max_nodes, u8 channel) {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' Sources/azl/uds_engine.cpp | sed 's://.*$::')
echo "$r" | grep -q 'StoreBE32(network_info_.wlan_comm_id_be, kLobbyWlanCommId);' && echo "$r" | grep -q 'network_info_.id = kLobbyId8;' || fail "BeginHostingDlp no longer hosts under the Download Play's ids"
echo "$r" | grep -q -E 'lobby_inbox_|application_data|EnsureLobbyLocked|SetLobbyInbox' && fail "BeginHostingDlp touches the lobby's inbox or puts application data in the beacon (Azahar's client parses a tag of 52 bytes at most)"
r=$(awk -v a='u32 UdsEngine::ConnectBeginDlp(const Mac& host, u8 channel, ConnectionType type) {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' Sources/azl/uds_engine.cpp | sed 's://.*$::')
echo "$r" | grep -q 'if (const u32 r = Gate(kIdle)) {' || fail "ConnectBeginDlp no longer checks the gate before it overwrites the NetworkInfo"
echo "$r" | grep -q -E 'lobby_inbox_|SetLobbyInbox|heard|received_beacons_' && fail "ConnectBeginDlp touches the lobby's inbox or reads the host's beacon (its id8, network id and attributes are undefined in Azahar)"
# - D16: a client that is Connecting takes no Logoff before its host's answer and none that names node 0 (a spectator's admission); a host notes
#   no departure (every peer of that network says goodbye once: capture 1c). The dialect's two rules (below, by the churn fuzz) are untouched.
need Sources/azl/uds_engine.cpp '        if (dlp_net_ && (!associated_ || (conn_type_ == ConnectionType::Client && logoff.assigned_node_id == 0))) {' "D16 a and b: the Logoffs a client of the real Download Play's network does not take"
need_after Sources/azl/uds_engine.cpp '    if (!dlp_net_) {' '        NoteLeftLocked(packet.transmitter_address, now);' "D16 c: a host of the real Download Play's network notes no departure (no guard against a second goodbye that never comes)"
[ "$(grep -c 'NoteLeftLocked(packet.transmitter_address, now);' Sources/azl/uds_engine.cpp)" = "1" ] || fail "Sources/azl/uds_engine.cpp: a departure is noted in one place (the Deauthentication a host handles)"
# - D12: the spectators are counted on the node map (connected and spectator), never kept in step elsewhere
need Sources/azl/uds_engine.cpp '        n += entry.second.connected && entry.second.spec ? 1u : 0u;' "SpectatorCount counts the connected spectators of the node map"
# - nothing of the plugin calls these pieces but the engine and the adapter of the real Download Play (F3-F5)
hit=$(grep -rn -E 'BeginHostingDlp|ConnectBeginDlp|SetLobbyBeaconKind|SpectatorCount' Sources Includes --include=*.cpp --include=*.hpp --include=*.h --include=*.c | grep -v -E '^(Sources/azl/uds_engine\.cpp|Includes/azl/uds_engine\.hpp|Sources/azl/dlp_real_net\.cpp|Includes/azl/dlp_real_net\.hpp):' | head -3)
[ -z "$hit" ] || fail "something besides the engine and the adapter of the real Download Play calls its engine pieces: $hit"
# - the suite builds and runs the engine's test of the real Download Play, which stops at the first undefined behaviour and runs every scenario
need Tests/run.sh 'g++ $CXXFLAGS Tests/dlp_real_engine_test.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_engine_test"' "the suite builds the engine's test of the real Download Play"
need_stmt Tests/run.sh 'timeout 1200 "$OUT/dlp_real_engine_test"' "the suite runs the engine's test of the real Download Play"
need Tests/dlp_real_engine_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "the engine's test of the real Download Play stops at the first undefined behaviour"
for s in 'E1_Kinds()' 'D11_Calls()' 'D12_Spectators()' 'CH_Channels()' 'R7_Goodbyes(100)' 'R7_Goodbyes(2000)' 'R7_GameUnchanged()' 'R7b_Logoffs()' 'R7b_Together()' 'R11b_Ghost(500)' 'R11b_Ghost(1500)' 'R11b_Race()' 'R11b_Goodbye()' 'R11b_Foreign()' 'R11b_StaleNodeMap()' 'R13_Dialects()' 'Fuzz(seed, 22, 2);' 'Fuzz(seed, 22, 4);' 'CHECK_EQ(g_diverged, 0u);'; do
  need_stmt Tests/dlp_real_engine_test.cpp "$s" "the scenario $s of the engine's test of the real Download Play runs in main()"
done
need_line Tests/dlp_real_engine_test.cpp '    for (u32 seed : {1u, 2u, 3u}) {' "the fuzz of two nodes (Luigi's Mansion) runs three seeds"
need Tests/dlp_net_stack_check.sh '|EnsureLobbyLocked|BeginHostingDlp|ConnectBeginDlp|SetLobbyBeaconKind|SpectatorCount' "the engine's pieces of the real Download Play are held to the frame budget of the lobby's (64 bytes on ARM32)"
# 1.1.3, phase F3: the HOST of the real Download Play (Includes/azl/dlp_real_net.hpp, Sources/azl/dlp_real_net.cpp: RealDlpNet under the lobby; design 4, 6.3, 6.4;
# Tests/dlp_real_net_test.cpp in front of the model of Azahar's client, Tests/azahar_dlp_peer.hpp). What must not change by accident: that the game threads get no frame
# and no allocation from it, that the announcement goes out only to a spectator and one frame at a time, that the dialogue's timers and bounds are the design's, that the
# lobby hears a START_ACK / GO_ACK only for the answers that earn them, that the first passphrase is the one sent, and that it gives the engine back whole.
game_thread_file Includes/azl/dlp_real_net.hpp 1
game_thread_file Sources/azl/dlp_real_net.cpp 1
need Includes/azl/dlp_real_net.hpp 'static_assert(sizeof(RealDlpNet) <= 2048,' "the adapter of the real Download Play stays under 2 KB (one frame buffer and some words)"
for c in 'kAnnounceMs = 500;' 'kAuthResendMs = 300;' 'kDistResendMs = 400;' 'kFinishResendMs = 200;' 'kNoticeMs = 100;' 'kFinalResendMs = 500;' 'kFinalTries = 4;' 'kPumpMs = 10;' 'kPullsPerPump = 4;'; do
  need Includes/azl/dlp_real_net.hpp "static constexpr u32 $c" "the host's timers and bounds are the design's (4.5): $c"
done
# - a game thread gets no frame from it: Send records the lobby's START / GO, Receive hands the acks earned, neither touches the wire
r=$(tr -d '\r' < Sources/azl/dlp_real_net.cpp | awk -v a='bool RealDlpNet::Send(u8 to_node, const u8* data, u32 size) {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' | sed 's://.*$::')
echo "$r" | grep -q -E 'SendTo|SendDialogue|PullPacket|Step|Announce\(' && fail "RealDlpNet::Send puts something on the wire (a game thread runs StartDistribution and BeginGame: the Pump sends)"
echo "$r" | grep -q 'if (!pass_set_) {' || fail "RealDlpNet::Send no longer keeps the FIRST passphrase (the game calls BeginGame again and again with new ones)"
r=$(tr -d '\r' < Sources/azl/dlp_real_net.cpp | awk -v a='bool RealDlpNet::Receive(u8* from_node, u8* data, u32 capacity, u32* size) {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' | sed 's://.*$::')
echo "$r" | grep -q -E 'SendTo|SendDialogue|PullPacket' && fail "RealDlpNet::Receive touches the wire (it only hands the acks the dialogue earned)"
# - the lobby hears an ack only when the client earned it: START_ACK at the Finish's answer (A6, no content), GO_ACK at the notice's answer (A9) in GoWait
[ "$(grep -c 'peer.flags |= kOweStartAck;' Sources/azl/dlp_real_net.cpp)" = "1" ] || fail "Sources/azl/dlp_real_net.cpp: the lobby's START_ACK is earned in one place (A6)"
[ "$(grep -c 'peer.flags |= kOweGoAck;' Sources/azl/dlp_real_net.cpp)" = "1" ] || fail "Sources/azl/dlp_real_net.cpp: the lobby's GO_ACK is earned in one place (A9)"
need Sources/azl/dlp_real_net.cpp '} else if (p.kind == dlp_real::Kind::FinishAck && peer.st == kFinWait && !p.needs_content) {' "START_ACK only for the Finish's answer of a client that needs no content"
need Sources/azl/dlp_real_net.cpp '} else if (p.kind == dlp_real::Kind::NoticeAck && peer.st == kGoWait) {' "GO_ACK only for a notice's answer after the final"
need_after Sources/azl/dlp_real_net.cpp '        if (p.needs_content) {' '            engine_->EjectClient(node);' "a client that asks for the content (a download) is ejected: this mode carries none"
# - the announcement: only while the host accepts and a spectator is in, ONE frame per Pump (the network thread holds the hub's mutex the game threads wait for)
need Sources/azl/dlp_real_net.cpp 'const u32 specs = accepting_ ? engine_->SpectatorCount() : 0;' "the announcement only while the host accepts and a spectator is connected (design 4.3)"
r=$(tr -d '\r' < Sources/azl/dlp_real_net.cpp | awk -v a='void RealDlpNet::Announce(u32 now) {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' | sed 's://.*$::')
[ "$(echo "$r" | grep -c 'engine_->SendTo(')" = "1" ] || fail "RealDlpNet::Announce sends more or less than one frame"
echo "$r" | grep -q -E '\b(for|while)[[:space:]]*\(|[^:]Announce\(' && fail "RealDlpNet::Announce loops or calls itself: one frame of the announcement per Pump"
need Sources/azl/dlp_real_net.cpp 'engine_->SendTo(kBroadcastNetworkNodeId, kChannelAnnounce, static_cast<u32>(len), 0, buf_, len);' "the announcement goes to every node on channel 1 (never with the management bit: Azahar ASSERTs)"
# - the packets of channel 2 taken per Pump are bounded, and none can stay stuck in the bind (PullPacket may take out more than the buffer holds: the parser refuses it)
need Sources/azl/dlp_real_net.cpp '    for (u32 i = 0; i < kPullsPerPump; ++i) {' "the Pump takes at most kPullsPerPump packets of the dialogue"
need Sources/azl/dlp_real_net.cpp 'engine_->PullPacket(kHostBind, kPullMax, buf_, sizeof(buf_), &n, &src)' "a packet longer than the buffer is taken out (cut) and refused, never left at the head of the bind"
need Includes/azl/dlp_real_net.hpp 'static constexpr u32 kPullMax = 0xFFFF;' "PullPacket may take out a frame of any length the ring can say (a longer one would stay at the head of the bind for ever)"
# - the host's bind hands no handle out (nobody would close it), and the engine still gives the games theirs
need Sources/azl/dlp_real_net.cpp 'engine_->Bind(kHostBind, kDialogueRing, kChannelDialogue, kBroadcastNetworkNodeId, nullptr)' "the adapter's bind takes no duplicate of its event (a handle nobody closes)"
need_after Sources/azl/uds_engine.cpp '    if (game_event != nullptr) {' '        *game_event = kernel_->DuplicateForGame(event);' "a game's Bind still gets its duplicate of the event"
# - the engine given back whole, the client's foreign-lobby filter off on every path (held or not), the adapter forgets a client no later than the lobby
need_after Sources/azl/dlp_real_net.cpp 'void RealDlpNet::Release() {' '        engine_->Shutdown();' "Release gives the service back whole"
r=$(tr -d '\r' < Sources/azl/dlp_real_net.cpp | awk -v a='void RealDlpNet::Release() {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' | sed 's://.*$::')
echo "$r" | grep -q -E -x '    engine_->SetClientForeignFilterEnforced\(false\);[[:space:]]*' || fail "RealDlpNet::Release no longer switches the client's foreign-lobby filter off outside any condition (decision Q8: the engine keeps it across Shutdown)"
need_after Sources/azl/dlp_real_net.cpp '    if (hosting_) {' '        ForgetGone(mask);' "the lobby's own Members makes the adapter forget a client that left (a node that comes back starts from the Auth)"
# - it never uses the dialect's pieces of the engine (who builds it is phase F5's block below)
hit=$(cat Sources/azl/dlp_real_net.cpp Includes/azl/dlp_real_net.hpp | tr -d '\r' | sed 's://.*$::' | grep -n -E 'BeginHostingLobby|ConnectBeginLobby|PopLobbyMessage|kLobbyChannel|SetLobbyInbox' | head -2)
[ -z "$hit" ] || fail "the adapter of the real Download Play uses the dialect's pieces of the engine: $hit"
# - the suite builds and runs its test, which stops at the first undefined behaviour and runs every scenario
need Tests/run.sh 'g++ $CXXFLAGS Tests/dlp_real_net_test.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_net_test"' "the suite builds the host's test of the real Download Play"
need_stmt Tests/run.sh 'timeout 1200 "$OUT/dlp_real_net_test"' "the suite runs the host's test of the real Download Play"
need Tests/dlp_real_net_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "the host's test of the real Download Play stops at the first undefined behaviour"
need Tests/dlp_real_net_test.cpp '#include "azahar_dlp_peer.hpp"' "the host is tested against the model of Azahar's client"
for s in 'R1_Sequence(0, true);' 'R1b_Announcement();' 'R1_Sequence(rtt, true);' 'R1_Sequence(700, false);' 'R5_Losses();' 'R6b_ThirdConsole();' 'R7_Rejoin(100);' 'R7_Rejoin(2000);' 'R9_Malformed();' 'R12_Full();' 'R14_Idempotent();' 'R14b_FinalsBounded();' 'Release_Paths();'; do
  need_stmt Tests/dlp_real_net_test.cpp "$s" "the scenario $s of the host's test of the real Download Play runs in main()"
done
need_line Tests/dlp_real_net_test.cpp '    for (u32 rtt : {100u, 300u, 500u}) {' "R4: the round trips that must pass"
# 1.1.3, phase F4: the SEARCHER of the real Download Play (the same adapter, RealDlpNet; design 5, NETCODE.md; Tests/dlp_real_client_test.cpp in front of
# the model of Azahar's host, Tests/azahar_dlp_host.hpp). What must not change by accident: that the engine is taken for a probe only and given back
# after it, that a probe reads only its own host (the filter, decision Q8), that its work per Pump is bounded, that a join needs nothing of the scan,
# and that the searcher's binds take no handle.
need_after Sources/azl/dlp_real_net.cpp 'RealDlpNet::RealDlpNet(UdsEngine* engine) : engine_(engine) {' '    engine_->SetLobbyBeaconKind(UdsEngine::LobbyBeaconKind::RealDlp);' "the adapter files the beacons of the real Download Play from its construction on (design 5.1: the game is of that mode for the whole session)"
for c in 'kProbeEveryMs = 20;' 'kProbeFreshMs = 400;' 'kProbeConnectMs = 3000;' 'kProbeCollectMs = 1500;' 'kProbeRetryMs = 5000;' 'kPullsPerProbe = 2;' 'kProbeRing = 8192;'; do
  need Includes/azl/dlp_real_net.hpp "static constexpr u32 $c" "the searcher's timers and bounds are the design's (5.2): $c"
done
# - ScanOpen takes no engine (a held engine would close nwm::UDS to the game for the whole scan: the gate); only a probe takes it, for itself
r=$(tr -d '\r' < Sources/azl/dlp_real_net.cpp | awk -v a='bool RealDlpNet::ScanOpen() {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' | sed 's://.*$::')
echo "$r" | grep -q -E 'Acquire\(|InitializeForLobby|ConnectBegin' && fail "RealDlpNet::ScanOpen takes the engine (design 5.2: only a probe does, for as long as it lasts)"
need Sources/azl/dlp_real_net.cpp 'engine_->ConnectBeginDlp(beacon.host, beacon.channel, ConnectionType::Spectator)' "a probe reads the announcement as a spectator, on the channel of the host's beacon"
need Sources/azl/dlp_real_net.cpp 'engine_->ConnectBeginDlp(host, beacon.channel, ConnectionType::Client)' "the join is a client's, on the channel of the host's beacon (NETCODE point 4: Luigi hosts on 6)"
# - the probe's work per Pump is bounded (1.5 KB frames under the hub's mutex the game threads wait for), and it never loops
need Sources/azl/dlp_real_net.cpp '    for (u32 k = 0; k < kPullsPerProbe; ++k) {' "a probe takes at most kPullsPerProbe frames of the announcement per Pump"
r=$(tr -d '\r' < Sources/azl/dlp_real_net.cpp | awk -v a='void RealDlpNet::Collect(u32 now) {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' | sed 's://.*$::')
[ "$(echo "$r" | grep -c -E '\b(for|while)[[:space:]]*\(')" = "1" ] || fail "RealDlpNet::Collect has more than its one bounded loop"
echo "$r" | grep -q 'engine_->PullPacket(kProbeBind, kPullMax, buf_, sizeof(buf_), &n, &src)' || fail "RealDlpNet::Collect no longer takes a frame longer than the buffer out of the bind (cut, then refused)"
# - the foreign-lobby filter (decision Q8 of the session of 2026-11-09, REGISTRO): on only in this adapter, only after the engine was taken for a probe
#   or a session, off in Release on every path (the pin of F3 above); nothing else of the plugin turns it on
hit=$(grep -rn 'SetClientForeignFilterEnforced(true)' Sources Includes | grep -v '^Sources/azl/dlp_real_net\.cpp:' | head -2)
[ -z "$hit" ] || fail "something besides the adapter of the real Download Play turns the client's foreign-lobby filter on: $hit"
[ "$(grep -c 'engine_->SetClientForeignFilterEnforced(true);' Sources/azl/dlp_real_net.cpp)" = "2" ] || fail "Sources/azl/dlp_real_net.cpp turns the filter on in other places than the probe and the session"
need Sources/azl/dlp_real_net.cpp '    engine_->SetClientForeignFilterEnforced(true);  // (two hosts in the room' "a probe reads its own host only: the filter is on for it"
need Sources/azl/dlp_real_net.cpp '    engine_->SetClientForeignFilterEnforced(true);  // (a second host of the Download Play' "a session hears its own host only: the filter is on for it"
# - a probe ends on the network thread: its 8 KB ring freed there, then the engine given back
r=$(tr -d '\r' < Sources/azl/dlp_real_net.cpp | awk -v a='void RealDlpNet::EndProbe(u8 verdict, u32 now) {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' | sed 's://.*$::')
echo "$r" | awk '/engine_->Unbind\(kProbeBind\);/ { u = NR } /^    Release\(\);/ { r = NR } END { exit !(u && r && u < r) }' || fail "RealDlpNet::EndProbe no longer frees the probe's bind before it gives the engine back"
# - a join needs nothing of the scan: the channel comes from the engine's table of beacons, never from the probes' cache (NETCODE point 7: StopScan empties it)
r=$(tr -d '\r' < Sources/azl/dlp_real_net.cpp | awk -v a='bool RealDlpNet::ConnectBegin(const Mac& host, const Identity& me) {' '!on && index($0, a) { on = 1 } on { print } on && $0 == "}" { exit }' | sed 's://.*$::')
echo "$r" | grep -q 'if (!BeaconOf(host, kProbeConnectMs, &beacon)) {' || fail "RealDlpNet::ConnectBegin no longer takes the host's channel from the engine's table of beacons"
echo "$r" | grep -q -E 'FindHeard|heard_' && fail "RealDlpNet::ConnectBegin depends on the probes' cache (the lobby joins after StopScan, which empties it)"
# - the searcher's binds: channel 1 / channel 2 from the host (node 1) only, no handle handed out
need Sources/azl/dlp_real_net.cpp 'engine_->Bind(kProbeBind, kProbeRing, kChannelAnnounce, kHostNode, nullptr)' "the probe binds channel 1 from the host only, with no handle"
need Sources/azl/dlp_real_net.cpp 'engine_->Bind(kSessionBind, kDialogueRing, kChannelDialogue, kHostNode, nullptr)' "the session binds channel 2 from the host only, with no handle"
# - the lobby hears StartDistribution and the final once each; the answers leave from the Pump, never from a game thread
need Sources/azl/dlp_real_net.cpp '            session_flags_ = static_cast<u8>(session_flags_ | kSawStart | kOweStart);' "the host's StartDistribution is the lobby's START, once"
need Sources/azl/dlp_real_net.cpp '                session_flags_ = static_cast<u8>(session_flags_ | kSawFinal | kOweGo);' "the host's FIRST final is the lobby's GO with its passphrase"
[ "$(grep -c '| kOweGo);' Sources/azl/dlp_real_net.cpp)" = "1" ] || fail "Sources/azl/dlp_real_net.cpp: the lobby's GO is owed in one place (the first final)"
# - the suite builds and runs its test, which stops at the first undefined behaviour and runs every scenario
need Tests/run.sh 'g++ $CXXFLAGS Tests/dlp_real_client_test.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_client_test"' "the suite builds the searcher's test of the real Download Play"
need_stmt Tests/run.sh 'timeout 1200 "$OUT/dlp_real_client_test"' "the suite runs the searcher's test of the real Download Play"
need Tests/dlp_real_client_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "the searcher's test of the real Download Play stops at the first undefined behaviour"
need Tests/dlp_real_client_test.cpp '#include "azahar_dlp_host.hpp"' "the searcher is tested against the model of Azahar's host"
for s in 'R2_Sequence(1, false);' 'R2_Sequence(2, false);' 'R2_Sequence(3, true);' 'R5b_Losses(false);' 'R5b_Losses(true);' 'R5c_ProbeLosses(false);' 'R5c_ProbeLosses(true);' 'R6f_Full();' 'R6_Three();' 'R9c_R12c();' 'R3a_Ours();' 'Rel_Paths();'; do
  need_stmt Tests/dlp_real_client_test.cpp "$s" "the scenario $s of the searcher's test of the real Download Play runs in main()"
done
need Tests/dlp_real_client_test.cpp 'return after == std::vector<u32>({5, 6, 9});' "R2: the statuses of our lobby after the join are 5, 6, 9 (NETCODE point 1: never 7)"
# 1.1.3, phase F5: the real Download Play IN THE PLUGIN (design 6.1, D1-D10; Tests/plugin_dlp_test.cpp runs the decision of EarlyInit for every row of the table,
# Tests/dlp_real_hub_test.cpp the hub with the adapter in both directions and in both modes). What must not change by accident: that only a RealDlp row gets
# the adapter (and the Lobby rows exactly what they had), that the hub keeps no member for it and pumps / asks it only when it is not its own adapter,
# that MakeDlpHub is untouched, that nothing but the plugin's two files and the adapter's own name it.
# - D1, D2: the mode, and the four Luigi's Mansion rows in it (the other rows are pinned with the table of phase 3)
need Includes/azl/dlp_hub.hpp 'enum class DlpGameMode : u8 { None = 0, ProbeOnly = 1, Lobby = 2, RealDlp = 3 };' "the modes of the table: RealDlp is a new value, the old ones keep theirs"
for t in 0x00040000001D1A00ull 0x00040000001D1900ull 0x00040000001D1800ull 0x00040000001D4F00ull; do
  need Sources/ctr/plugin.cpp "{$t, DlpGameMode::RealDlp}," "Luigi's Mansion ($t) speaks the real Download Play (1.1.3; back to Lobby with this row if O5 fails)"
done
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c 'DlpGameMode::RealDlp}')" = "4" ] || fail "Sources/ctr/plugin.cpp: the RealDlp rows are the four Luigi's Mansion ones and no other (a new one comes with its own Azahar - Azahar oracle, design F7)"
# - D3, D5: the branch of EarlyInit after the Lobby one (whose line is pinned above), the adapter made there and only there, kept by the plugin before the hub
need_line Sources/ctr/plugin.cpp '    } else if (DlpModeOf(game_id_) == DlpGameMode::RealDlp && active_) {' "the real Download Play's hub exists only for a RealDlp game, only when the plugin is active, right after the Lobby branch"
[ "$(grep -r -c 'new (std::nothrow) RealDlpNet(engine_.get())' Sources Includes | awk -F: '{ n += $2 } END { print n + 0 }')" = "1" ] || fail "the adapter of the real Download Play is made in one place (the RealDlp branch of EarlyInit)"
need_after Sources/ctr/plugin.cpp '        dlp_real_.reset(new (std::nothrow) RealDlpNet(engine_.get()));' '            dlp_hub_ = MakeRealDlpHub(engine_.get(), &kernel_, game_id_, console_mac_, dlp_real_.get());' "the hub of a RealDlp game runs on the adapter just made, and only if it was made"
need_after Sources/ctr/plugin.cpp '        dlp_real_.reset(new (std::nothrow) RealDlpNet(engine_.get()));' '        if (dlp_real_) {' "no hub without the adapter (MakeRealDlpHub gives it its identity)"
need_order Includes/ctr/plugin.hpp 'std::unique_ptr<RealDlpNet> dlp_real_;' 'std::unique_ptr<DlpHub> dlp_hub_;' "the adapter is declared before the hub that points to it"
# - D4: MakeDlpHub untouched (its return line is pinned above and is the start of the extractor's search), MakeRealDlpHub right after it with the same configuration
need Sources/ctr/plugin.cpp 'return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver, real));' "MakeRealDlpHub gives the hub the adapter as the lobby's network"
need_order Sources/ctr/plugin.cpp 'return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver));' 'return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver, real));' "MakeRealDlpHub comes right after MakeDlpHub (the extractor cuts both)"
need Sources/ctr/plugin.cpp '    real->SetIdentity(probe);' "the probes take the lobby's own identity (F4's open point: it was zero)"
need Tests/plugin_dlp_extract.py "marker = 'return std::unique_ptr<DlpHub>(new (std::nothrow) DlpHub(engine, kernel, config, resolver, real));" "the extractor cuts the plugin's text to the end of MakeRealDlpHub"
need Tests/plugin_dlp_extract.py "wiring_start = text.index('    if (DlpModeOf(game_id_) == DlpGameMode::Lobby && active_) {" "the extractor also cuts the decision of EarlyInit, from the Lobby line"
need Tests/plugin_dlp_test.cpp '#include "plugin_dlp_wiring.inc"' "the plugin test runs the decision of EarlyInit itself"
need_stmt Tests/plugin_dlp_test.cpp 'azl::ctr::WiringTest();' "the plugin test runs the decision for every row, other games and an inactive plugin"
need_stmt Tests/plugin_dlp_test.cpp 'azl::ctr::RealHubTest();' "the plugin test checks MakeRealDlpHub's configuration"
need Tests/plugin_dlp_test.cpp 'CHECK_EQ(lobby_hubs, 7u);' "the seven Lobby rows get the hub of the dialect, on the hub's own adapter"
need Tests/plugin_dlp_test.cpp 'CHECK_EQ(real_hubs, 4u);' "the four RealDlp rows get the adapter of the real Download Play"
need Tests/run.sh 'g++ $CXXFLAGS -I"$OUT" Tests/plugin_dlp_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/plugin_dlp_test"' "the plugin test links the adapter of the real Download Play (the wiring makes one)"
need_order Tests/run.sh 'gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -c ConfigApp/dlph/dlph_wire.c -o "$OUT/dlph_wire.o"' 'Tests/plugin_dlp_test.cpp Sources/azl/dlp_hub.cpp' "the library's dlph_wire.o is built before the plugin test that links it"
# - D6-D10: the hub's backend: no member, the lobby keeps the pointer; pumped first and asked whether it holds the engine only when it is not the hub's own adapter
need Sources/azl/dlp_hub.cpp ': net_(engine), lobby_(backend != nullptr ? backend : &net_, cfg), resolver_(resolver), kernel_(kernel) {' "the hub's lobby runs on the backend when it has one, on its own adapter otherwise (every Lobby game)"
need Includes/azl/dlp_hub.hpp 'DlpHub(UdsEngine* engine, KernelOps* kernel, const LobbyConfig& cfg, const IpcResolver& resolver, LobbyNet* backend = nullptr);' "the backend is the hub's last argument, null by default"
need_after Sources/azl/dlp_hub.cpp 'void DlpHub::Refresh() {' 'const bool held = b != &net_ ? b->Holds() : net_.engine_held();' "the hub's EngineHeld is the backend's Holds for a RealDlp game, its own adapter's otherwise"
need_after Sources/azl/dlp_hub.cpp 'void DlpHub::FreeEngine() {' 'if (b != &net_) {' "the give-back of a RealDlp game asks its adapter"
need_order Sources/azl/dlp_hub.cpp '        b->Pump();  // (1.1.3' '    lobby_.Pump();' "the backend does its wire work before the lobby's pump, in the same Tick under the same lock"
[ "$(grep -c -- '->Pump();' Sources/azl/dlp_hub.cpp)" = "1" ] || fail "Sources/azl/dlp_hub.cpp pumps the backend in one place (Tick)"
hit=$(grep -n -E 'RealDlpNet|dlp_real' Sources/azl/dlp_hub.cpp Includes/azl/dlp_hub.hpp | grep -v -E '//.*(dlp_real_net|real Download Play)' | head -2)
[ -z "$hit" ] || fail "the hub names the adapter of the real Download Play (it knows only LobbyNet: a backend of any kind): $hit"
need Includes/azl/dlp_lobby.hpp '    virtual void Pump() {}' "LobbyNet::Pump is not pure: the dialect's adapter and the tests' networks keep the default"
need Includes/azl/dlp_lobby.hpp '    virtual bool Holds() const { return false; }' "LobbyNet::Holds is not pure either"
need Includes/azl/dlp_lobby.hpp '    LobbyNet* net() const {' "the lobby tells the hub which network it runs on (no member of the hub for it)"
need Includes/azl/dlp_real_net.hpp '    void Pump() override;' "the adapter's Pump is the hub's (LobbyNet::Pump)"
need Includes/azl/dlp_real_net.hpp '    bool Holds() const override {' "the adapter's Holds is the hub's (LobbyNet::Holds)"
# - who names the adapter: its own two files, the plugin's two (the member and the branch: an exception to the rule of the hub's header, decided in
#   the design 7.4 point 13) and the tests; the app knows nothing of it
hit=$(grep -r -n -E 'azl/dlp_real_net\.hpp"' Sources Includes ConfigApp/source --include=*.cpp --include=*.c --include=*.hpp --include=*.h | grep -v -E '^(Sources/azl/dlp_real_net\.cpp|Includes/ctr/plugin\.hpp):' | head -3)
[ -z "$hit" ] || fail "something other than plugin.hpp and the adapter itself includes azl/dlp_real_net.hpp: $hit"
hit=$(grep -r -n -E 'RealDlpNet' Sources Includes ConfigApp/source --include=*.cpp --include=*.c --include=*.hpp --include=*.h | grep -v -E '^(Sources/azl/dlp_real_net\.cpp|Includes/azl/dlp_real_net\.hpp|Includes/ctr/plugin\.hpp|Sources/ctr/plugin\.cpp):' | head -3)
[ -z "$hit" ] || fail "something other than the plugin and the adapter itself names RealDlpNet: $hit"
# - the stack and the size (design 7.4 points 3, 4, 12): the adapter's size on ARM32, the frames of its two units and of what a game thread runs of it,
#   the give-back of a RealDlp game on the game's nwm::UDS thread in the chain test
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(RealDlpNet) <= 2048,' "the adapter of the real Download Play is at most 2 KB on ARM32: a growth is a decision"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(DlpHub) <= 408,' "the hub keeps its 408 bytes with a backend (no member for it)"
need Tests/dlp_net_stack_check.sh 'FRAME_REAL=288 ' "no function of the real Download Play's adapter and codec has a frame over 288 bytes on ARM32"
need Tests/dlp_net_stack_check.sh 'FRAME_REAL_GAME=80 ' "what a game thread runs of that adapter has a frame of 80 bytes at most"
need Tests/dlp_net_stack_check.sh "REAL_GAME_FUNCTIONS='NowMs|NowTicks|Holds|HostClose|Disconnect|ScanClose|Release|ClearHost|ClearClient'" "the functions of the adapter a game thread runs (the hub's RunHere, the clock)"
need Tests/dlp_net_stack_check.sh '-c Sources/azl/dlp_real_net.cpp -o "$W/real_net.o"' "the check compiles the adapter of the real Download Play strictly"
need Tests/dlp_net_stack_check.sh '-c Sources/azl/dlp_real_wire.cpp -o "$W/real_wire.o"' "... and its codec"
need Tests/dlp_net_stack_check.sh 'for o in real_net real_wire; do' "the two units of the real Download Play keep no static storage"
need Tests/stack_usage.sh 'budget dlp_real_net "azl::RealDlpNet::Release" 16' "the adapter's Release, which a game thread may run, keeps its frame"
need Tests/chain_sim/chain_sim.py 'def scenario_hub_real_engine(elf, level):' "the chain test gives the engine back on the game's nwm::UDS thread for a RealDlp game, with and without a network thread"
need Tests/chain_sim/chain_sim.py "sim.call(elf.sym('_ZN3azl10RealDlpNetC1EPNS_9UdsEngineE'), [self.real, self.engine])" "the chain test builds the adapter with its own compiled constructor"
[ "$(tr -d '\r' < Tests/chain_sim/chain_sim.py | grep -c -x '            scenario_hub_real_engine(elf, level)')" = "2" ] || fail "Tests/chain_sim/chain_sim.py does not run the RealDlp give-back in both of its lists (--hub-only and the whole run)"
# - the hub test of the real Download Play: built, run, every scenario in main()
need Tests/run.sh 'g++ $CXXFLAGS Tests/dlp_real_hub_test.cpp Sources/azl/dlp_hub.cpp Sources/azl/dlp_lobby_net.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -o "$OUT/dlp_real_hub_test"' "the suite builds the hub test of the real Download Play"
need_stmt Tests/run.sh 'timeout 1200 "$OUT/dlp_real_hub_test"' "the suite runs the hub test of the real Download Play"
need Tests/dlp_real_hub_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "the hub test of the real Download Play stops at the first undefined behaviour"
need Tests/dlp_real_hub_test.cpp 'static const Scenario all[] = {{"R3", R3}, {"R8", R8}, {"R10", R10}, {"R11b", R11b}, {"R13", R13}};' "the scenarios of the hub test of the real Download Play"
need Tests/dlp_real_hub_test.cpp 'const std::vector<std::string> lobby = RunR3(false, clients);' "R3 runs the same commands in the dialect's mode ..."
need Tests/dlp_real_hub_test.cpp 'const std::vector<std::string> real = RunR3(true, clients);' "... and in the real Download Play's, and compares what the games saw"
need Tests/dlp_real_hub_test.cpp 'TCHECK(lobby[i] == real[i]);' "R3: what the games see is the same line for line"
need Tests/dlp_real_hub_test.cpp 'TCHECK(has("searcher Bob: statuses 5 6 9"));' "R3: the searcher's statuses after the join are 5, 6, 9 (NETCODE point 1)"
need_line Tests/dlp_real_hub_test.cpp '    for (u32 clients : {1u, 3u}) {' "R3 runs with 1 and 3 clients"
need_line Tests/dlp_real_hub_test.cpp '    for (const bool alive : {true, false}) {' "R8 runs with the network thread alive and gone"
# 1.1.3, review of F0-F5 (analysis/notte-2026-11-09/CORREZIONI.md): what the three reviews found and the fixes keep.
# - protocol 1: in the real Download Play's network the Logoff of an admission reaches its members only (Azahar's nwm adopts ANY Logoff while it is
#   Connecting): a spectator's to the spectator alone, a client's to every member; every other network broadcasts as before
need_after Sources/azl/uds_engine.cpp '        if (!dlp_net_) {' '            SendEapolLogoffLocked(kBroadcastMac, node.network_node_id);' "outside the real Download Play the Logoff of an admission is broadcast as since 1.0"
need_line Sources/azl/uds_engine.cpp '            SendEapolLogoffLocked(packet.transmitter_address, node.network_node_id);' "a spectator's admission goes to the spectator alone"
need_line Sources/azl/uds_engine.cpp '                    SendEapolLogoffLocked(member.first, node.network_node_id);' "a client's admission goes to every member of the network, never to the room"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c 'SendEapolLogoffLocked(kBroadcastMac,')" = "1" ] || fail "Sources/azl/uds_engine.cpp: a Logoff is broadcast in more or fewer than one place (a game's admission)"
# - protocol 2: the real Download Play's host closed to clients still takes spectators (Azahar's host never closes; its searchers refused wait 3 s inside the
#   emulation); the door to spectators or to everyone keeps them out; never a member's association dropped while it is closed
need_line Sources/azl/uds_engine.cpp '    return dlp_net_ && (admission_attributes_ & (kAttrBlockSpectators | kAttrBlockAll)) == 0;' "spectators welcome at a closed host only in the real Download Play, and not past the bits that close to them"
need_line Sources/azl/uds_engine.cpp '        if (AdmissionBlocked() && !(spectator && SpectatorsWelcomeLocked())) {' "a closed host takes no client's EAPoL-Start"
need_line Sources/azl/uds_engine.cpp '        (!SpectatorsWelcomeLocked() || (known != node_map_.end() && known->second.connected))) {' "a closed host answers no Authentication of a member (its association stays)"
# - integration 2-3: a full host is never probed (it answers no spectator; Azahar's asserts on a late EAPoL-Start)
need_after Sources/azl/dlp_real_net.cpp '        if (beacon.total_nodes >= beacon.max_nodes) {' '            continue;' "a full host is not probed, nor filed in the cache"
# - the tests of the fixes run: RV1-RV2 (engine), RV3-RV4 (searcher), R1b with the closed host and a signed first-group time (host)
need_line Tests/dlp_real_engine_test.cpp '    RV1_LogoffsStayInTheNetwork();' "RV1 runs: the Logoffs reach the members only"
need_line Tests/dlp_real_engine_test.cpp '    RV2_ClosedHostTakesSpectators();' "RV2 runs: a closed host takes spectators, never a client"
need_line Tests/dlp_real_client_test.cpp '    RV3_FullNotProbed();' "RV3 runs: a full host is not probed"
need_line Tests/dlp_real_client_test.cpp '    RV4_StartBeforeGo();' "RV4 runs: START before GO when both are in hand"
need Tests/dlp_real_net_test.cpp 'CHECK(!groups.empty() && groups[0] >= t_conn && groups[0] <= t_in + 100);' "R1b: the first group between the connect and 100 ms after it (no unsigned difference that wraps)"
need Tests/dlp_real_net_test.cpp 'CHECK_EQ(h.net.counters().announce, ann_closed);' "R1b: a spectator let in at a closed host reads no announcement"
need Tests/azahar_rules.hpp 'const auto it = std::find(host.pending.begin(), host.pending.end(), p.destination_address);' "the rule checker admits the MAC a unicast admission goes to"
need Tests/trace_replay_test.cpp '#include "ffe_host_golden.inc"' "the FFE host replay runs on the golden data of PROVA 2"
need Tests/trace_replay_test.cpp '#include "tfh_host_golden.inc"' "the TFH host replay runs on the golden data of the first TFH trace"
need Tests/trace_replay_test.cpp '#include "lobby_trace.inc"' "the lid episode and the client stretch run on the frames of the real traces"
# the client-role replay has Azahar's own models around the 3DS, not our engine (what it is tested against must not change with it)
need Tests/ffe_replay_test.cpp "AzaharClient peer(room," "the other players of the client-role replay are the frozen Azahar client model"
need Tests/ffe_replay_test.cpp "AzaharHost host(room," "the host of the client-role replay is the frozen Azahar host model"

# A client's GetApplicationData is the application data of its host's beacon (3dbrew: "from the current beacon"). TEKKEN 3D PRIME EDITION
# reads it 100 ms after it joins and leaves the network when it gets 0 bytes (log of 2026-10-18, both consoles, both ways): the NetworkInfo a
# game joins with is the 0x3C bytes of the old Connect and holds none of it.
need_line Sources/azl/uds_engine.cpp "    if (reads_host_beacon) {" "a client or a spectator answers from the beacon of its host"
need_line Sources/azl/uds_engine.cpp "                                   connection_status_.status == static_cast<u32>(NetworkStatus::ConnectedAsSpectator);" "a spectator reads its host's beacon like a client"
need Sources/azl/uds_engine.cpp "if (heard.packet.transmitter_address != network_info_.host_mac_address) {" "only the beacon of the host it joined is read"
need Sources/azl/uds_engine.cpp "if (GetBeaconApplicationData(FindBeaconTags(heard.packet.data), &beacon_data, &beacon_size)) {" "the data is the one the beacon's tag carries"
need Sources/azl/uds_engine.cpp "data = beacon_data;" "what the beacon carries is what the game is given"
need Sources/azl/uds_frames.cpp "constexpr std::size_t kBeforeData = kNetworkInfoTagSize - 2;" "the application data of a beacon starts where the generator of the tag puts it"
need Sources/azl/uds_frames.cpp "const std::size_t count = tags.network[kBeforeData - 1];" "its size is the byte the generator writes right before it"
need Sources/azl/uds_frames.cpp "if (count > kApplicationDataSize || tags.network_size < kBeforeData + count) {" "a size the tag cannot hold is not read"
need Tests/test_main.cpp "TestBeaconApplicationData();" "the test of the beacon's application data runs"

# The replies the audit of 2026-10-18 (every UDS command against the firmware) put right, each pinned where it is made:
need Sources/azl/uds_engine.cpp "if (network_node_id == 0) {" "GetNodeInformation(0) is the empty slot of nwm's table: found, as zeros, in every state"
need Sources/azl/uds_engine.cpp "connection_status_.changed_nodes |= static_cast<u16>(old_bitmask ^ connection_status_.node_bitmask);" "a client's changed nodes accumulate until the game reads them (the status event is one-shot)"
need Sources/azl/uds_engine.cpp "network_channel_ = kDefaultNetworkChannel; // (not the channel of the network hosted or joined before)" "hosting on channel 0 takes the default channel for GetChannel and for the frames"
need Sources/azl/uds_engine.cpp "return; // (a Logoff with id 0 would be adopted by every client that is still Connecting)" "an EAPoL-Start of an unknown type admits nobody and is not answered"
need Sources/azl/uds_engine.cpp "if (conn_type_ == ConnectionType::Client && logoff.assigned_node_id != 0) {" "a client that is still Connecting takes the Logoff that names its own node"
need Sources/azl/uds_engine.cpp "return pa != pb ? pb : a.rx_ms < b.rx_ms;" "the beacon of the host a client joined is never the one that makes room for another"
need Sources/ctr/uds_hle.cpp "size == 0 ? engine_->SetApplicationData(&kNone, 0) : res::kTooLarge" "SetApplicationData with size 0 and no buffer clears the data (the firmware never sees the pointer)"
for t in TestRepliesByRole TestJoinsInOneRoundTrip TestChangedNodesAccumulate TestHostBeaconKept; do
  need Tests/test_main.cpp "    $t();" "the test of the replies by role ($t) runs"
done

# The lag analysis of 2026-10-19 (NOTE_LAG_2026-10-19.md): ENet's packet throttle never destroys a game frame, expendable frames go out
# UNSEQUENCED, a pause of the network thread is neither a timeout nor a round-trip sample, and the network thread may be asked for a
# New 3DS core.
need Sources/azl/room_client.cpp "    // Only once the connection is up: the room refuses this command from a peer that is not connected yet, and a refused" "the throttle is configured only after the connect event (2026-10-20: queued before it, the real room never answered the connect)"
need_after Sources/azl/room_client.cpp "    // never answered the connect)." "enet_peer_throttle_configure(server, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);" "... right there"
need Sources/azl/room_client.cpp "enet_peer_throttle_configure(server, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_SCALE, 0);" "the packet throttle never decelerates: no frame is destroyed at the sender"
need Sources/azl/room_client.cpp "                    : static_cast<u32>(ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);" "beacons and broadcast game data go out unsequenced (never held behind a reliable packet)"
[ "$(grep -c 'EnetPacketFlags(item.reliable)' Sources/azl/room_client.cpp)" = "2" ] || fail "both places that hand a frame to ENet use EnetPacketFlags"
need Sources/enet/protocol.c "switch (enet_protocol_send_outgoing_commands (host, event, 0))" "the socket is read before the timeouts are looked at (an acknowledgement that waited is not a loss)"
need Sources/enet/protocol.c "if (peer -> lastReceiveTime > 0 && host -> azlServiceGap >= ENET_AZL_PAUSE_MS &&" "a round-trip sample taken after a pause of the thread is not taken"
need Sources/enet/protocol.c "++ enet_azl_throttled_packets;" "the frames the throttle destroys are counted"
# The data path of the ENet client (the series of 2026-10-27): the defaults of its three switches and the two guards that make them safe
need Sources/enet/host.c "    host -> azlSkipEmptyRecv = 1;" "ENet leaves out the receive that only says nothing when a poll follows (on by default)"
need Sources/enet/protocol.c "&& ! enet_protocol_timers_due (host))" "... but never when a timer of ours is due: the acknowledgement that stops it may be in the socket"
need Sources/enet/host.c "    host -> azlAckHoldEvents = ENET_AZL_ACK_HOLD_EVENTS;" "a packet is handed over before the acknowledgement of its datagram (on by default)"
need Includes/enet/enet.h "#define ENET_AZL_ACK_HOLD_EVENTS 8" "... and the acknowledgement waits for 8 packets at the most"
need Includes/enet/enet.h "#define ENET_AZL_ACK_HOLD_MS 8" "... or 8 ms (less on a link that answers faster: a third of the room's timeout)"
need Includes/enet/enet.h "#define ENET_AZL_READ_MAX_DATAGRAMS 32" "a read pass stops after 32 datagrams and hands over what it has"
need Sources/enet/protocol.c "enet_protocol_send_outgoing_commands (host, event, ! host -> azlReadCapped)" "... and the timers are looked at only once the socket has been read to its end"
need Sources/enet/protocol.c "? 0 : ENET_TIME_GREATER_EQUAL (host -> serviceTime + 1, timeout);" "the socket counts as empty only after a poll that waited its whole time: a poll that came back early with nothing readable is a socket in trouble, and the receive of the next call is where the error shows"
need Sources/ctr/plugin.cpp "if (core >= -1 && core <= 3) {" "net_thread_core accepts the extra cores of a New 3DS"
need Sources/ctr/plugin.cpp ', rtt samples skipped %lu |' "the link line says how many samples were left out"
need Sources/ctr/plugin.cpp 'dropped: stale %lu cap %lu throttled %lu |' "the link line says how many frames the throttle destroyed"
need Tests/test_main.cpp "    TestEnetThrottleAndPause();" "the ENet test runs"

# The app review of 2026-10-19 (README, tappa N): nothing of a receiving stays half installed, what is half installed is found and can be deleted,
# the scan says what it is doing, the caches and the copies cost what they must and no more.
need ConfigApp/titleshare/source/receiver.c "wt->err[cur] = 0xC8604C03; // (fermata prima della fine: la pagina del risultato lo dice con queste parole)" "a title stopped half way is an error the result page explains"
need ConfigApp/source/pages_delivery.cpp 'if (s.result[i] == DL_RES_ERROR && s.err[i] == 0xC8604C03u) {' "the result page says in words that a title was stopped before the end"
need ConfigApp/titleshare/source/catalog.c "if(R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &c->titleIds[i], &infos[i]))) {" "the module asks the versions title by title when the list is refused (one broken title must not cost every icon at every start)"
need ConfigApp/titleshare/source/catalog.c "int verOk = haveVer && !verBad[i];" "a title AM cannot describe stays out of the names cache"
need ConfigApp/source/titles.cpp "if (R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &infos[i]))) {" "the app asks the versions title by title when the list is refused, and names the broken ones"
need ConfigApp/source/titles.cpp "if (R_SUCCEEDED(AM_GetPendingTitleList(&got, ids.data(), pending, MEDIATYPE_SD))) {" "the installations that were begun and never finished are found"
need ConfigApp/source/titles.cpp "if (R_FAILED(AM_ListExistingContentInfos(&got, infos.data(), 0, n, tid, MEDIATYPE_SD))) {" "a DLC with contents its ticket grants but that are not on the card is found"
need ConfigApp/source/titles.cpp "missing += owned && !there ? 1 : 0;" "a DLC content counts as missing only when the ticket grants it and it is not there"
need ConfigApp/source/titles.cpp "rc = AM_DeletePendingTitle(MEDIATYPE_SD, t.tid); // (the ticket stays: it may be the one of a title that works, which this never finished replacing)" "deleting an unfinished installation never deletes a ticket"
need ConfigApp/source/status.cpp 'report.items.push_back(MakeItem("Installed titles", Counted(static_cast<int>(t.unusable.size()), "unusable title"), CheckLevel::Warning, "Titles of no use",' "the titles of no use are a warning of the installation check"
need ConfigApp/source/status.cpp "CheckAction::CleanTitles, \"unusable titles\"));" "A on the row deletes them, and the main page names them"
need ConfigApp/source/pages_check.cpp 'PushPage(NewConfirmPage("Titles of no use", text, "Yes, delete", "No", [to_delete](bool yes) {' "the deletion is asked first, with the list"
need ConfigApp/source/pages_check.cpp "const int gone = titles::DeleteUnusable(to_delete, &failed);" "A deletes what the scan found of no use"
need ConfigApp/source/pages_check.cpp "g.rescan_wanted = true; // (the list of titles and the check change: the scan runs again and the row is made from it)" "the titles are read again after the deletion"
need ConfigApp/source/pages_delivery.cpp 'g.ShowToast(gfx::Clean(dl_item_name(item)) + ": incomplete on this console, it cannot be sent (see Installation check).", col::kWarn, 5000);' "a title that is not whole on the card is not offered for sending"
need ConfigApp/source/ui.cpp "facts.titles = titles::Scan().report().health; // (the row of the titles: what the scan found of no use)" "the check's row of the titles comes from the scan"
need ConfigApp/source/main.cpp "if (!g.titles_health_applied && titles::Scan().done()) {" "the check is made again when a scan ends"
need ConfigApp/source/status.cpp 'std::snprintf(buf, sizeof(buf), "names: %d of %d titles", done, total);' "the scan says which step it is in (the names of every title, then the games) instead of one count over both"
need ConfigApp/source/titles.cpp "impl->phase.store(1);" "the scan reports its second step"
! grep -q "TitlesProgressText" ConfigApp/source/*.cpp ConfigApp/source/*.hpp || fail "the old progress text (one count over the two steps of the scan) is back"
need ConfigApp/source/settings.cpp "entry.password = stored.password; // (... and its password: a caller that knows only the address must not wipe it)" "a room used again by its address keeps its password"
need ConfigApp/source/pages_test.cpp "tested.password = g.cfg.password; // (the test used it: the room in the list keeps it)" "the connection test hands the room its password"
need ConfigApp/source/main.cpp "if (!g.save_ok && g.settings_loaded != setup::LoadResult::Unreadable) {" "the retry of a failed save on the way out never writes over a file that could not be read"
need ConfigApp/source/games.cpp "ok = ok && fstat(fileno(f), &st) == 0 && static_cast<u64>(st.st_size) == sizeof(header) + static_cast<u64>(count) * kCacheRecordBytes;" "the games cache is measured before memory is asked for it"
need ConfigApp/source/games.cpp "const bool same_size = FileSize(GamePluginPath(tid), &have) && have == want; // (another size is stale without reading a byte of either)" "a copy of another size is brought up to date without reading it"
need ConfigApp/source/delivery_run.cpp "if (run.result.start_ms != 0) {" "the titles are read again only after a receiving that began"
need ConfigApp/source/ui.cpp "setup::RemoveParkFiles(paths.park_dir);" "the block files a cut-off receiving left are removed when the app starts"
need ConfigApp/source/paths.cpp 'if (name.size() > 4 && name.compare(name.size() - 4, 4, ".blk") == 0) {' "only the block files go from park/"
need ConfigApp/source/ui.cpp "if (plugin_copies_checked || !plugin_ready) {" "the quiet look at the copies of the plugin starts at once: it does not wait for the scan of the titles (whoever installed an update waits for it)"
need ConfigApp/source/ui.cpp "const std::string stamp = setup::PluginContentStamp(setup::EmbeddedPluginData(), setup::EmbeddedPluginSize());" "the stamp of the copies is made from the BYTES of the plugin (the card gives every file the date 0; two builds can have the same size)"
need ConfigApp/source/ui.cpp "plugin_pass_open = submitted > 0;" "a pass of quiet looks is open while there are folders to look at (the main page tells how it goes)"
need ConfigApp/source/plugin_install.cpp '"%llu:h%016llx"' "the stamp of a plugin is its size and the hash of its bytes"
need ConfigApp/source/pages_main.cpp 'Updating the plugin in your games: %d of %d folders done. Please wait.' "the main page says how many folders are done while the copies of the plugin are looked at"
need ConfigApp/source/pages_main.cpp 'The plugin is up to date in all your games.' "... and that it is over when nothing had to be made again"
need Tests/settings_test/plugin_install_test.cpp "    TestContentStamp();" "the test of the stamp of the plugin runs"
need ConfigApp/source/ui.cpp "if (!stamp.empty() && stamp == app_cfg.plugin_stamp) {" "the copies are not read again while the plugin file is the one they were brought up to"
need ConfigApp/source/main.cpp "g.app_cfg.plugin_stamp = g.plugin_stamp_pending;" "the stamp of the plugin file is remembered when the looks are done"
need ConfigApp/source/appcfg.cpp '} else if (key == "plugin_stamp") {' "3DSLink.cfg holds the stamp"
for t in TestTitlesRow; do
  need Tests/settings_test/games_test.cpp "    $t();" "the test of the titles row ($t) runs"
done
need Tests/settings_test/paths_test.cpp "    TestFileStampAndPark();" "the test of the file stamp and the park cleanup runs"

# The performance round of 2026-10-19 (NOTE_LAG_2026-10-19.md, points 5-8; README, tappa O). The engine's lock: events are
# signalled after it is released (Guard), SendTo builds and hands over its frame without it, Tick takes it only when due; the
# network thread looks at the queue through an atomic and waits less after the game sends; the diagnostics behind the log;
# radio_like=2: frames numbered and the copy dropped, the room's reliable stream handed over as it arrives.
need Includes/azl/uds_engine.hpp "        explicit Guard(const UdsEngine* e);" "every entry point of the engine takes its lock through the guard"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c 'platform::LockGuard lock(mu_);')" = "0" ] || fail "Sources/azl/uds_engine.cpp takes mu_ without the guard (a signal under the lock again)"
need Sources/azl/uds_engine.cpp "    engine->mu_.unlock();" "the guard releases the lock ..."
need_after Sources/azl/uds_engine.cpp "    engine->mu_.unlock();" "engine->kernel_->SignalEvent(handles[i]);" "... and signals the queued events only then"
need Sources/azl/uds_engine.cpp "    QueueSignalLocked(bind->event); // (given once the lock is free: the game's PullPacket must not find it taken)" "a data frame's signal is queued, not given under the lock"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c 'QueueSignalLocked(connection_status_event_);')" = "11" ] || fail "the status event is signalled under the lock somewhere (QueueSignalLocked expected in 11 places)"
need Sources/azl/uds_engine.cpp "    packet.data = GenerateDataPayload(data, data_size, data_channel, dest_node_id, own_node_id, sequence);" "SendTo builds its frame outside the lock (the guard's block ends before it)"
need Sources/azl/uds_engine.cpp "    if (!beacon_due && !presence) {" "Tick takes the lock only for a beacon that is due or a member being timed"
need Sources/azl/uds_engine.cpp "        host_beacon_ms_.compare_exchange_strong(heard, now_ms, std::memory_order_relaxed); // just joined: the clock starts now" "the client's beacon bookkeeping runs without the lock"
need Sources/azl/uds_engine.cpp "    role_.store(connection_status_.status, std::memory_order_relaxed);" "the role mirror follows every status change"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c 'connection_status_.status = static_cast<u32>(NetworkStatus::')" = "0" ] || fail "connection_status_.status is assigned outside SetStatusLocked (the mirror would lag)"
need Sources/azl/uds_engine.cpp "    connection_status_.status = static_cast<u32>(status);" "SetStatusLocked is the one place the status changes"
need Includes/azl/uds_ring.hpp "    Put Enqueue(u16 dest, u16 src, u8 channel, const u8* payload, u32 n, u32 stamp_ms = 0) {" "the ring keeps a stamp per frame"
need Sources/azl/uds_engine.cpp "        NotePullLatency(static_cast<u32>(platform::NowMs()) - queued_ms); // (from the ring's stamp: how long the game let it wait)" "PullPacket measures the wait of the frame it takes (with the log on)"
need Includes/azl/room_client.hpp "inline u32 ServiceTimeoutFor(bool queued, u32 now_ms, u32 last_enqueue_ms) {" "the wait in the socket follows the game's frames"
need Includes/azl/room_client.hpp "constexpr u32 kQuickPollMs = 1;" "1 ms after the game sent"
need Includes/azl/room_client.hpp "constexpr u32 kQuickPollWindowMs = 10;" "... for 10 ms"
need Sources/azl/room_client.cpp "        if (tx_pending_.load(std::memory_order_acquire) != 0) { // (the game's thread sets it under the lock: nothing pushed, no lock)" "the queue is looked at through an atomic, not the lock, every round"
need Sources/ctr/uds_hle.cpp "            svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);" "the first call of a command says the thread and its priority"
need Sources/azl/log.cpp "    c.plugin10 = c.net10 + c.uds10 + c.frame10 + c.chat10;" "the writer's wall time is not in the CPU total"
need Includes/enet/enet.h "   enet_uint32          azlDeliverOutOfOrder;" "ENet can hand a reliable packet over ahead of a hole"
need Sources/enet/peer.c "          placeholder -> azlDispatched = 1;" "... leaving a placeholder for the sequence accounting"
need_after Sources/enet/peer.c "          if (incomingCommand -> azlDispatched)" "enet_free (incomingCommand);" "... that is freed, never delivered, when its turn comes"
need Sources/enet/protocol.c "return 0; /* AzaharLink: the packet of this fragment was handed over already (a late copy of one of its fragments) */" "a late fragment of a packet handed over already is ignored"
need Sources/azl/room_client.cpp "        client->azlDeliverOutOfOrder = fast_level_.load() >= 2 ? 1u : 0u; // (the room's reliable stream handed over as it arrives)" "the room's stream is handed over as it arrives only at radio_like=2"
need Includes/azl/room_client.hpp "    } else if (level >= 2 && peer_numbers) {" "unicast game data goes unreliable and twice only at radio_like=2, and only to a console that numbers its frames"
need_after Includes/azl/room_client.hpp "    } else if (level >= 2 && peer_numbers) {" "d.duplicate = true;" "... twice"
need Sources/azl/uds_engine.cpp "        peer_numbers = dest_node_id != kBroadcastNetworkNodeId && copy_dedupe_.Numbers(dest_node_id);" "the engine says whether the destination numbers its frames (an Azahar gets the frame once, reliable)"
need Sources/azl/uds_engine.cpp "            out_packet.peer_numbers = copy_dedupe_.Numbers(secure_data.dest_node_id);" "... for a relayed frame too"
need Sources/azl/room_client.cpp "                ++g_stats.tx_duplicates;" "the copies are counted"
need Sources/azl/uds_engine.cpp "        sequence = ++tx_seq_;" "SendTo numbers its frames"
need Sources/azl/uds_engine.cpp "    tx_seq_ = static_cast<u16>(platform::NowMs() & 0x7FFFu);" "the numbers start from the clock"
need Includes/azl/uds_dedupe.hpp "        if (sequence == 0 || node == 0 || node >= kNodes) {" "an unnumbered frame (Azahar, an older plugin) is never dropped as a copy"
need_after Sources/azl/uds_engine.cpp "    // The copy a radio_like=2 sender adds (the same frame, the same number, a round later): the first one was delivered." "if (copy_dedupe_.Seen(secure_data.src_node_id, secure_data.sequence_number)) {" "a copy is dropped before the ring"
need Sources/azl/uds_engine.cpp "            if (copy_dedupe_.Seen(secure_data.src_node_id, secure_data.sequence_number)) {" "... and before the host forwards"
need Sources/azl/uds_engine.cpp "    copy_dedupe_.ForgetNode(node.node_id); // (the next console with this id numbers its frames from its own start)" "a node that leaves is forgotten (host)"
need Sources/azl/uds_engine.cpp "            copy_dedupe_.ForgetNode(logoff.assigned_node_id);" "a console that rejoined its old slot is forgotten (client)"
need Sources/azl/uds_engine.cpp "    kernel_->SignalEvent(connect_event_);" "the connect event is signalled under the lock (a deferred one could follow the next attempt's ClearEvent)"
need Sources/azl/uds_engine.cpp "        DrainPendingSignalLocked(bind.event); // (a teardown queued one: given before the close, as before the round)" "a signal queued for an event is given before the event is closed"
need Sources/azl/uds_engine.cpp "            out_packet.relayed = true; // (for the room client: a unicast frame of the radio, retried, not a broadcast)" "a relayed frame is unicast for the delivery rule"
need Sources/azl/room_client.cpp "                                               IsBroadcastDelivery(packet), packet.peer_numbers);" "... and the room client treats it so, and gives the copy only to a peer that numbers"
need Includes/azl/room_client.hpp "    return (p.destination_address == kBroadcastMac && !p.relayed) || p.broadcast_flag;" "a relayed copy is not a broadcast, and a frame the game sent with the broadcast flag is, whatever MAC it is addressed to"
need Sources/azl/uds_engine.cpp "    packet.broadcast_flag = (flags & kBroadcastFlag) != 0;" "the engine marks a frame the game sent with the broadcast flag, so that it stays unreliable when it is addressed to a node's own MAC"
need Sources/azl/uds_engine.cpp "                return false; // two MACs claim the node (a stale table of a host that was in another lobby): which one is right is not known" "a frame is addressed to a node's MAC only when the node table gives the node ONE MAC (a stale second entry makes it the broadcast, as before)"
need Sources/azl/room_client.cpp "constexpr u32 kConnectRttMs = 100;" "the CONNECT starts from a round trip of 100 ms, not ENet's 500: a lost CONNECT is resent after 100, 300, 700 ms"
need Sources/azl/room_client.cpp "    server->roundTripTime = kConnectRttMs;" "... and the connection is given that estimate before the first CONNECT goes out"
need Sources/azl/room_client.cpp "                ShouldProbeRoom(client->serviceTime, server->earliestTimeout, now_ms, last_probe_ms)) {" "the room is pinged when it speaks after our packets have gone unanswered for half a second (the link came back before ENet's check gave the room up)"
need Includes/azl/room_client.hpp "inline constexpr u32 kSilentPeerMs = 500;" "... after half a second of silence and no oftener than every 250 ms"
need Sources/azl/room_client.cpp "    client->azlEarlyFilter = EarlyFilter;" "only game frames and beacons go ahead of a hole"
need Sources/azl/room_client.cpp "    return ether == static_cast<u16>(EtherType::SecureData) ? 1 : 0;" "... the handshake and the room's messages keep their order"
need Sources/enet/peer.c "          if (peer -> host -> azlEarlyFilter != NULL &&" "ENet asks the filter before the early hand-over"
need Includes/azl/uds_dedupe.hpp "    static constexpr u32 kSeen = 128;" "the receiver remembers a whole batch of copies"
need Sources/azl/room_client.cpp 'static_assert(kRepeatMax * 2 <= CopyDedupe::kSeen, "the receiver remembers a whole batch of copies and what came between");' "... pinned against the sender's batch"
need Sources/azl/log.cpp "    out.lock_hold_ticks = take(g_stats.lock_hold_ticks);" "TakeStats takes the lock's hold sum"
need ConfigApp/source/settings.cpp "if (c >= -1 && c <= 3) {" "the app accepts net_thread_core 2 and 3 (the A/B key must survive a save)"
need Sources/ctr/uds_hle.cpp "        static bool said[kStatsCommands]; // (never cleared: the line below is said once per command in the life of the plugin)" "the first-caller line is said once, not once a window"
need Sources/ctr/plugin.cpp "    room_->SetFastData(config_.radio_like);" "the radio level is the settings' (the in-game menu has no switch for it)"
need Sources/azl/uds_engine.cpp "    if (twin_unmatched) {" "a forwarded copy with no twin is counted once the copy rule let it through"
[ "$(tr -d '\r' < Sources/azl/uds_engine.cpp | grep -c '        ForgetChangedNodesLocked(before);')" = "2" ] || fail "a client does not forget the numbers of a slot that changed console (both Logoff refreshes)"
need Tests/azahar_rules.hpp "            Add(\"secure-shape\", \"SecureData with aggregate count \" + std::to_string(LoadBE16(raw + 2)) + \" (Azahar emits 0)\");" "the Azahar rule leaves the sequence number free"
for t in TestSignalsAfterUnlock TestTickWithoutLock TestServiceTimeoutRule TestEnetOutOfOrder TestNumberedFrames; do
  need Tests/test_main.cpp "    $t();" "the test $t of the performance round runs"
done
need Tests/dedupe_test.cpp "    TestCopyDedupeUnit();" "the test of the copy dedupe runs"
need Tests/chain_sim/chain_sim.py "sim.log_lines[0].startswith('uds: command 0x23 is first called by thread ')" "the chain test expects the first-caller line"
need ConfigApp/titleshare_host/delivery.c "gSizes[i] = R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &ids[i], &infos[i])) ? infos[i].size : 0;" "the sizes of the send list are asked title by title when the list is refused"

# 3DS Link is ONE product with ONE version (the user's rule, 2026-10-21): the app, the plugin, both CIAs, the Download Play child and the documents all say the same
# X.Y.Z, and the file VERSION holds it. A patch runs Tests/set_version.py, which rewrites every point below; this check refuses a tree where one of them says
# another number (a forgotten point is a failed check, not a surprise on a console). The TMD of the CIAs and the SMDH descriptions are made from VERSION when
# DlpLink/tools/build_all.sh builds them (DlpLink/tools/verify_cia.py checks the result).
[ -f VERSION ] || fail "VERSION is missing (the one number of 3DS Link: Tests/set_version.py writes it)"
VERSION=$(tr -d '\r\n ' < VERSION)
case "$VERSION" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) fail "VERSION is not X.Y.Z ('$VERSION')" ;;
esac
MAJOR=${VERSION%%.*}; REST=${VERSION#*.}; MINOR=${REST%%.*}; REV=${REST#*.}
{ [ "$MAJOR" -lt 64 ] && [ "$MINOR" -lt 64 ] && [ "$REV" -lt 16 ]; } 2>/dev/null || fail "VERSION $VERSION does not fit the TMD of the CIAs (major 0-63, minor 0-63, micro 0-15: major << 10 | minor << 4 | micro)"
need Includes/ctr/plugin.hpp "kVersion = \"$VERSION\";" "the plugin says $VERSION (its log header and menu text)"
need Sources/main.cpp "PluginMenu(\"AzaharLink\", $MAJOR, $MINOR, $REV," "the menu's version integers say $VERSION"
for kv in "Major: $MAJOR" "Minor: $MINOR" "Revision: $REV"; do
  tr -d '\r' < CTRPluginFramework.plgInfo | grep -q -x "    $kv" || fail "CTRPluginFramework.plgInfo does not say '$kv' (the header of the 3gx says $VERSION)"
done
need ConfigApp/source/settings.hpp "kVersion = \"$VERSION\";" "the app says $VERSION"
tr -d '\r' < README.md | grep -q "^> \*\*Stato attuale: $VERSION" || fail "README.md does not say '$VERSION' as the current version (Stato attuale)"
tr -d '\r' < README.md | grep -q "^## Novità $VERSION" || fail "README.md has no section 'Novità $VERSION'"
need PROVE_HARDWARE.md "# Prove su hardware - AzaharLink $VERSION" "the plan of the hardware tests names $VERSION"
need dist/LEGGIMI.txt "AzaharLink $VERSION - LEGGIMI" "the short guide names $VERSION"
#  - every OTHER point that carries the number (the log header the tests expect, the CIA row of the short guide, the README's lines about the one version, the
#    test plan of the installer) and the README's own section and history row: Tests/set_version.py --check rewrites them all with VERSION and compares
PY=python; command -v python >/dev/null 2>&1 || PY=python3
"$PY" Tests/set_version.py --check >/dev/null || fail "a point that carries the version does not say $VERSION, or the README has the stub of a new version or no history row for it (Tests/set_version.py --check says which)"
#  - the .3gx files carry the version in their header: byte 9 = revision, 10 = minor, 11 = major
for f in AzaharLink.3gx ConfigApp/data/AzaharLink.3gx; do
  [ -f "$f" ] || continue
  [ "$(od -An -tu1 -j9 -N3 "$f" | tr -s ' ')" = " $REV $MINOR $MAJOR" ] || fail "$f is not a plugin of version $VERSION (its header says $(od -An -tu1 -j11 -N1 "$f" | tr -d ' ').$(od -An -tu1 -j10 -N1 "$f" | tr -d ' ').$(od -An -tu1 -j9 -N1 "$f" | tr -d ' ')): build the plugin again"
done
#  - ONE package (the user's rule, 2026-10-22): the plugin is inside the app, so it is not a file of dist/ (a plugin that sits there is a second way to install, and goes stale: build_all.sh puts an old one in dist/old)
[ ! -e dist/AzaharLink.3gx ] || fail "dist/AzaharLink.3gx is there: 3DS Link is ONE package, the plugin is inside 3DSLink.cia and 3DSLink.3dsx and the app writes it to the card (DlpLink/tools/build_all.sh --dist moves an old one to dist/old)"
#  - the descriptions of the SMDH (home menu) say the version, from the same file
need ConfigApp/Makefile 'APP_VERSION	:= $(shell cat $(TOPDIR)/../VERSION)' "the app's Makefile reads the version from VERSION"
need_line ConfigApp/Makefile $'APP_DESCRIPTION\t:= Connect, share, play together · v$(APP_VERSION)' "the description of the app in the home menu says the version"
need_line ConfigApp/Makefile $'$(OUTPUT).smdh\t:\t$(TOPDIR)/../VERSION' "the SMDH of the app is made again when VERSION changes (otherwise its description keeps the old number)"

# The documents the user reads (stage C). They call the app and its pages what the app calls them, they say where the files go, and
# the numbers of the two files of dist/ in them are the numbers of the files.
#  - the old name of the delivery module: one sentence of the README says where the module comes from, and no other document (nor any
#    line of the README) names it; the old names of the two pages are gone from all three
[ "$(tr -d '\r' < README.md | grep -c 'TitleShare')" = "1" ] || fail "README.md names TitleShare on a number of lines other than one (a single history sentence says where the module comes from)"
for f in PROVE_HARDWARE.md dist/LEGGIMI.txt dist/old/LEGGI.txt; do
  ! grep -q 'TitleShare' "$f" || fail "$f names TitleShare: the user knows no such app"
done
for f in README.md PROVE_HARDWARE.md dist/LEGGIMI.txt dist/old/LEGGI.txt; do
  ! grep -q -E 'Enable game|Game delivery|\*Files\*|"Files"' "$f" || fail "$f uses a name the app no longer has (Enable game, Game delivery, the Files page)"
done
#  - dist/LEGGIMI.txt is plain ASCII (the person who reads it on a console-side PC may not have the accents)
! LC_ALL=C grep -q -E '[^[:print:][:space:]]' dist/LEGGIMI.txt || fail "dist/LEGGIMI.txt has a character that is not ASCII"
#  - the install rule and the first-start question are said as the app says them
need README.md '**un pacchetto solo: il plugin `AzaharLink.3gx` è dentro l'"'"'app**, che lo scrive da sola in `sdmc:/3ds/3DSLink/AzaharLink.3gx`' "the table of what to copy says that the plugin is inside the app and where the app puts it (one package: nothing else to copy)"
need PROVE_HARDWARE.md '**Un pacchetto solo: il plugin `AzaharLink.3gx` è dentro l'"'"'app**, che lo scrive da sola in `sdmc:/3ds/3DSLink/AzaharLink.3gx`' "the plan says that the plugin is inside the app and where the app puts it (one package)"
need dist/LEGGIMI.txt "UN PACCHETTO SOLO: il plugin AzaharLink.3gx e' DENTRO l'app, e l'app lo scrive da sola in SD:/3ds/3DSLink/AzaharLink.3gx" "the short guide says that the plugin is inside the app and where the app puts it (one package)"
! grep -q "NELLA STESSA CARTELLA di 3DSLink.3dsx" dist/LEGGIMI.txt || fail "dist/LEGGIMI.txt tells to put the plugin in the folder of the app again (the app carries it and writes it)"
! grep -q 'dist/AzaharLink.3gx` | \*\*nella stessa cartella' README.md || fail "README.md tells to copy dist/AzaharLink.3gx next to the app again (the app carries the plugin and writes it)"
FIRST_START_QUESTION="First start: 3DS Link will scan the SD card and check your installation. This takes a while. Continue?"
need ConfigApp/source/pages_startup.cpp "\"${FIRST_START_QUESTION}\";" "the question of the first start, as the page holds it (not as the comment at the top of the file says it)"
need ConfigApp/source/pages_startup.cpp "DrawPopupBox(box, \"Continue?\", kQuestion);" "the welcome popup draws that question, in a box that fits it"
for f in README.md PROVE_HARDWARE.md dist/LEGGIMI.txt; do
  need "$f" "$FIRST_START_QUESTION" "the document quotes the question of the first start as the app asks it"
done
need dist/LEGGIMI.txt "3DSLink-TitleDelivery.log   il registro" "the delivery log is described next to the plugin's logs"
#  - the numbers of the files that are delivered: size and md5, in the two documents that give them. The plugin is not a file of dist/ (it is inside the app): its numbers are those of the
#    resource the app is built with, ConfigApp/data/AzaharLink.3gx, which is the file that has to appear on the card
for f in dist/3DSLink.3dsx ConfigApp/data/AzaharLink.3gx; do
  [ -f "$f" ] || { [ "$BUILT" != 1 ] || fail "$f is missing"; continue; }
  SUM=$(md5sum "$f" | cut -d' ' -f1)
  SIZE=$(stat -c %s "$f" | sed -E ':a;s/([0-9])([0-9]{3})($|\.)/\1.\2\3/;ta')
  need PROVE_HARDWARE.md "$SUM" "the md5 of $f"
  need dist/LEGGIMI.txt "$SUM" "the md5 of $f"
  need PROVE_HARDWARE.md "$SIZE byte" "the size of $f"
  need dist/LEGGIMI.txt "$SIZE byte" "the size of $f"
done
for f in 3DSLink.cia; do   # (3DS Link is ONE CIA: no dist/dlp/child.cia, the app makes the Download Play child)
  [ -f "dist/$f" ] || continue
  SUM=$(md5sum "dist/$f" | cut -d' ' -f1)
  SIZE=$(stat -c %s "dist/$f" | sed -E ':a;s/([0-9])([0-9]{3})($|\.)/\1.\2\3/;ta')
  need PROVE_HARDWARE.md "$SUM" "the md5 of dist/$f"
  need dist/LEGGIMI.txt "$SUM" "the md5 of dist/$f"
  need PROVE_HARDWARE.md "$SIZE byte" "the size of dist/$f"
  need dist/LEGGIMI.txt "$SIZE byte" "the size of dist/$f"
done
#  - dist/3DSLink.3dsx is the app of the tree (where the tree has been built)
[ ! -f ConfigApp/3DSLink.3dsx ] || cmp -s dist/3DSLink.3dsx ConfigApp/3DSLink.3dsx || fail "dist/3DSLink.3dsx is not ConfigApp/3DSLink.3dsx (copy the product of the clean build, and the numbers in the documents)"

# 3DS Link is ONE CIA (the user's rule, 2026-10-21): the Download Play child is not a file that is handed out, the app builds it on the console (ConfigApp/dlpc, DlpLink/docs/COMPOSITORE.md)
# from its own CIA, the plugin and the MODEL of the child that it carries as a resource (the child without its RomFS: DlpLink/tools/make_model.py). The app's Makefile takes in the composer,
# the model and the pictures of the guide; the model in the tree is a real one of this version; and what is delivered carries it. The build chain's order has no cycle.
need_line ConfigApp/Makefile $'SOURCES\t\t+=\tdlpc guide' "the app builds the composer (dlpc/) and the guide (guide/) whole"
need_line ConfigApp/Makefile $'DATA\t\t:=\tdata' "data/child_model.bin is linked in as the model resource"
need_line ConfigApp/Makefile $'GRAPHICS\t:=\tguide/gfx' "the pictures of the guide are made from the .t3s files of guide/gfx"
need_line ConfigApp/Makefile $'INCLUDES\t:=\t../Includes source' "the guide includes the program's own gfx.hpp, so source/ is on the include path"
need_line ConfigApp/Makefile $'GFXBUILD\t:=\t$(BUILD)' "the pictures are linked in as data, made next to the objects"
need ConfigApp/Makefile '@tex3ds -i $< -H $*.h -d $*.d -o $*.t3x' "a picture is made from its .t3s into a .t3x and a header, next to the objects"
need ConfigApp/Makefile $'%.t3x.o\t%_t3x.h :\t%.t3x' "a .t3x is linked in as data (its symbol is stepN_t3x)"
need ConfigApp/Makefile $'%.bin.o\t%_bin.h :\t%.bin' "the model (data/child_model.bin) is linked in by bin2o as child_model_bin"
[ "$(ls ConfigApp/dlpc/*.c | wc -l)" = "1" ] || fail "ConfigApp/dlpc holds another .c file than dlpc.c (the Makefile builds the whole folder: nothing else may be there)"
[ -f ConfigApp/dlpc/dlpc.c ] && [ -f ConfigApp/dlpc/dlpc.h ] || fail "the composer (ConfigApp/dlpc/dlpc.c and dlpc.h) is not there"
need ConfigApp/source/dlp_prepare.cpp 'g_model_version = dlpc_model_version(child_model_bin, child_model_bin_size);' "the model's version is asked of the composer, which hashes the whole resource"
need ConfigApp/source/dlp_prepare.cpp 'if (!g_model_version_known) {' "...once: the model is a constant of the build, and the page asks three times each time it opens (the hash of 250 KB is not repeated on the screen's thread)"
need ConfigApp/source/pages_dlp.cpp 'if (dlp::ModelVersion() == 0) {' "a build whose resource is no model (the placeholder) says so and does not go on"
need ConfigApp/source/dlp_prepare.cpp 'if (ModelVersion() == 0) {' "the work does not start without a model (a result of its own: nothing is written)"
need ConfigApp/dlpc/dlpc.c 'if (c->mi.version != j->version16) {' "the composer refuses a model of another version than the app (the app and its model are from one build)"
need ConfigApp/dlpc/dlpc.c 'if (c->mi.emu != c->emu) {' "the composer refuses the emulator model for the real child and the other way round"
need ConfigApp/dlpc/dlpc.c '#define MAX_CHILD 0x02000000u' "the child cannot pass 32 MiB: what dlph_open takes"
need ConfigApp/dlpc/dlpc.c 'remove(j->out_path); // a failure leaves no child' "a job starts by removing the old child, and a failure leaves none"
need ConfigApp/dlpc/dlpc.c 'if (rename(c->part, j->out_path) != 0) {' "the child is put in place by a rename of the finished and verified .part file"
need ConfigApp/dlpc/dlpc.c 'snprintf(c->part, sizeof(c->part), "%s.part", j->out_path);' "the child is written as <path>.part first"
need ConfigApp/source/dlp_prepare.cpp 'job.emu_variant = 0;' "the package that is delivered is the real child: the emulator variant is for the tests"
if [ -d ../DlpLink ]; then
  M16=$(( (MAJOR << 10) | (MINOR << 4) | REV ))
  [ "$BUILT" != 1 ] || "$PY" ../DlpLink/tools/check_chain.py model ConfigApp/data/child_model.bin --version16 "$M16" >/dev/null 2>&1 || fail "ConfigApp/data/child_model.bin is not a model of the child of version $VERSION (make_model.py --check, TMD version $M16): DlpLink/tools/build_all.sh makes it"
  # the delivered files carry exactly this model and the pictures of the guide (the pictures are the build's own t3x files; a tree that was never built has none)
  WHAT="ConfigApp/data/child_model.bin"; [ ! -f ConfigApp/build/step1.t3x ] || WHAT="$WHAT ConfigApp/build/step1.t3x ConfigApp/build/step2.t3x ConfigApp/build/step3.t3x ConfigApp/build/step4.t3x ConfigApp/build/step5.t3x ConfigApp/build/step6.t3x ConfigApp/build/step7.t3x"
  [ "$BUILT" != 1 ] || "$PY" ../DlpLink/tools/check_chain.py carries $WHAT --in dist/3DSLink.3dsx dist/3DSLink.cia >/dev/null 2>&1 || fail "dist/3DSLink.3dsx or dist/3DSLink.cia does not carry the model that is in ConfigApp/data (and the pictures of the guide): the app that is delivered must be built with it (DlpLink/tools/build_all.sh --dist)"
  # the installer inside the model carries the guide as well (the same pictures as the app's, the same bytes)
  [ ! -f ConfigApp/build/step1.t3x ] || "$PY" ../DlpLink/tools/check_chain.py carries ConfigApp/build/step1.t3x ConfigApp/build/step2.t3x ConfigApp/build/step3.t3x ConfigApp/build/step4.t3x ConfigApp/build/step5.t3x ConfigApp/build/step6.t3x ConfigApp/build/step7.t3x --in ConfigApp/data/child_model.bin >/dev/null 2>&1 || fail "the installer in ConfigApp/data/child_model.bin does not carry the pictures of the guide"
fi
# The package page: before the room opens it looks at child.cia; a good one (the app's version) goes straight to the room, otherwise it is built (never with the room open), from the
# app's own CIA (a file left by the installer, else the installed title through the module), the plugin and the model, on a thread of its own, with a screen that says what it does.
need ConfigApp/source/pages_dlp.cpp 'const dlp::Verdict verdict = force ? dlp::Verdict::NotCia : dlp::CheckChildFile(g.paths.dlp_child, version16);' "a package that is there and is this version of the app is looked at first"
need ConfigApp/source/pages_dlp.cpp 'if (verdict == dlp::Verdict::Good) {' "a good package opens the room at once; nothing is built"
[ "$(grep -c -E '^ +Open\(\);' ConfigApp/source/pages_dlp.cpp)" = "2" ] || fail "ConfigApp/source/pages_dlp.cpp opens the room from more or fewer places than two (a good package, and the work that ended in Done): the room is never open while the package is built"
need_after ConfigApp/source/pages_dlp.cpp 'if (state == dlp::PrepareState::Done) {' 'Open();' "the room opens only when the work ended in Done"
need ConfigApp/source/pages_dlp.cpp 'died_ = dlp::DiedRuns(g.paths.dlp_marker);' "a marker left by a run that died is looked for before the work starts"
need ConfigApp/source/pages_dlp.cpp 'if (died_ > 0 && !asked_) {' "a run that did not end is asked about, not looped on"
need ConfigApp/source/pages_dlp.cpp 'const bool keys = scan_done && delivery::AttachModule() && dl_have_keys() != 0;' "the installed 3DS Link is read only when the scan is done and its context has the keys"
need ConfigApp/source/pages_dlp.cpp 'if (source == dlp::Source::WaitScan) {' "without a good file the page waits for the scan of the titles (the key comes with it)"
need ConfigApp/source/pages_dlp.cpp 'if (source == dlp::Source::None) {' "with neither a file nor a key the page says exactly that"
need ConfigApp/source/pages_dlp.cpp 'if (g.status.sd_known && g.status.sd_free < dlp::SpaceNeeded(' "the card is asked for room before the work"
need ConfigApp/source/pages_dlp.cpp 'job.use_file = source == dlp::Source::File;' "a good file is composed from as it is"
need ConfigApp/source/pages_dlp.cpp 'job.allow_title = keys;' "the installed title may be read only with the keys"
need ConfigApp/source/pages_dlp.cpp $'        holds_.Take(); // (HOME and sleep are off, and the keys of the scan\'s context stay, until the work ends)' "HOME and sleep are off while the package is built, and the scan's context is not freed under the work"
need ConfigApp/source/pages_dlp.cpp 'if (back && !cancelling_) {' "B asks first, and only once: it does not stop the work by itself"
need ConfigApp/source/pages_dlp.cpp 'prep_->Cancel(); // (the composer stops within one slice and leaves nothing behind; the page waits for it)' "the question's A stops the work through the composer, which leaves nothing behind"
need ConfigApp/source/dlp_prepare.cpp 'if (!setup::WriteTextFile(job_.marker_path, MarkerText(job_.attempt, job_.version_text))) {' "the marker goes down BEFORE the work begins"
need_after ConfigApp/source/dlp_prepare.cpp 'const int rc = Execute();' 'std::remove(job_.marker_path.c_str());' "the marker comes up AFTER the work, however it ends"
need ConfigApp/source/dlp_prepare.cpp 'std::remove((job_.out_path + ".part").c_str());' "what a run that died left of the package is removed before the work"
need ConfigApp/source/dlp_prepare.cpp 'thread_ = threadCreate(Hooks::ThreadMain, this, kWorkerStack, prio < 0x3F ? prio + 1 : prio, -2, false);' "the work is a thread a little less important than the screen (the page keeps drawing and B keeps answering)"
need ConfigApp/source/dlp_prepare.cpp 'return p->cancel_.load() ? 1 : 0; // (nonzero stops the composer: it leaves nothing behind)' "the composer is asked after every slice whether to stop"
need ConfigApp/source/dlp_prepare.cpp 'if (dlpc_memory_needed(&sizes) + kWorkerSlackBytes + kWorkerStack > dl_heap_free()) {' "a heap that cannot hold the package is told in words before anything is written"
need ConfigApp/source/dlp_prepare.cpp 'if (!facts.parsed || facts.tid != kAppTid || facts.version != job_.version16) {' "the installed 3DS Link that is copied is the app's title, of this version"
need ConfigApp/source/dlp_prepare.cpp 'if (std::rename(part.c_str(), job_.app_cia_path.c_str()) != 0) {' "the copy of the app's CIA is put in place by a rename of a finished .part file"
need ConfigApp/source/dlp_prepare.cpp 'ui::g.HoldHome();' "the work holds HOME"
need ConfigApp/source/dlp_prepare.cpp 'aptSetSleepAllowed(false);' "the work refuses sleep"
need ConfigApp/source/dlp_prepare.cpp '++ui::g.module_holds;' "the work keeps the module's context (no rescan under it)"
need ConfigApp/source/dlp_prepare.cpp 'threadJoin(thread_, U64_MAX); // (U64_MAX never times out: a timeout would read as success)' "the work is waited for with no timeout (a timeout reads as success on the 3DS)"
need ConfigApp/source/dlp_prepare_logic.hpp 'if (app_file == Verdict::Good) {' "the first source of the app's CIA is the good file (the installer leaves it: no keys needed)"
need ConfigApp/source/dlp_prepare_logic.hpp 'if (!scan_done) {' "the second is the installed title, which needs the scan's keys: the page waits for the scan"
need ConfigApp/source/dlp_prepare_logic.hpp 'return keys ? Source::Title : Source::None;' "with the scan done: the installed title when the keys are there, nothing otherwise"
need ConfigApp/source/dlp_prepare_logic.hpp 'constexpr u64 kAppTid = 0x000400000F83D100ull;' "the title that the child installs is 3DS Link's (DlpLink/tools/common.sh: unique id 0x0F83D1)"
need ConfigApp/source/dlp_prepare_logic.hpp 'constexpr u64 kMaxChildBytes = 32ull << 20;' "a child past 32 MiB is no package (dlph_open)"
need ConfigApp/source/dlp_prepare_logic.hpp 'if ((facts.tid >> 32) != 0x00040001u) {' "child.cia must be a title of the Download Play kind"
need ConfigApp/source/dlp_prepare_logic.hpp 'return f < 0.0 ? 0.0 : (f > 0.99 ? 0.99 : f);' "the bar is never full before the end"
[ "$(grep -c -F 'return facts.version == version16 ? Verdict::Good : Verdict::WrongVersion;' ConfigApp/source/dlp_prepare_logic.hpp)" = "2" ] || fail "ConfigApp/source/dlp_prepare_logic.hpp: a package and an app copy are good only when their TMD says the version of this app (two judgements, one line each)"
need ConfigApp/source/dlp_prepare_logic.hpp 'if (facts.tid != kAppTid) {' "the app's own copy must be the app's title"
need ConfigApp/source/dlp_prepare_logic.hpp 'double aes = 0.7;' "the speed of the software AES is taken low: a time longer than the work is better than a shorter one"
need ConfigApp/source/paths.hpp 'constexpr const char* kDlpMarkerFileName = "child.preparing";' "the marker of a run in progress is dlp/child.preparing"
need ConfigApp/source/paths.hpp 'constexpr const char* kDlpAppCiaFileName = "3dslink.cia";' "the app's own CIA is dlp/3dslink.cia: where the installer of the child leaves it"
need ConfigApp/source/paths.hpp 'constexpr const char* kDlpChildFileName = "child.cia";' "the package that is handed out is dlp/child.cia"
# The emergency switch (Settings > Download Play delivery, 3DSLink.cfg: the app's own file, never the plugin's ini): on by default, an unreadable value is on, it is written every time (a hand edit
# flips a digit), and with it off the entry is faint, the page does not open and nothing of the feature runs. The same entry is shut, as Titles Delivery's Send Titles is, without boot9.bin.
need ConfigApp/source/appcfg.hpp 'bool dlp_delivery = true;' "the feature is on by default"
need ConfigApp/source/appcfg.cpp 'cfg.dlp_delivery = azl::ini::ParseBool(value, true);' "the switch is read with the plugin's words, and a slip means on"
need ConfigApp/source/appcfg.cpp $'out += std::string("dlp_delivery=") + (cfg.dlp_delivery ? "1" : "0") + "\\n";' "the switch is written every time, so that a hand edit only flips a digit"
need ConfigApp/source/pages_config.cpp 'dlp_switch.on = g.app_cfg.dlp_delivery;' "the row of Settings shows the switch"
need ConfigApp/source/pages_config.cpp 'g.app_cfg.dlp_delivery = on;' "the row of Settings flips the switch"
need_after ConfigApp/source/pages_config.cpp 'g.app_cfg.dlp_delivery = on;' 'g.SaveAppConfig();' "the delivery switch is saved when it is flipped"
need_after ConfigApp/source/pages_config.cpp 'g.app_cfg.update_check = on;' 'g.SaveAppConfig();' "the update-check switch is saved when it is flipped"
need ConfigApp/source/pages_config.cpp 'dlp_switch.label = dlp::kSwitchLabel;' "the row is called as the text says"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kSwitchLabel = "Download Play delivery";' "the row of the switch is called Download Play delivery (as the notes and the documents call it)"
need ConfigApp/source/dlp_text.hpp '"OFF: the whole feature is shut. Its entry is faint and nothing of it runs or is read or written.' "the note of the switch says what off means"
need ConfigApp/source/dlp_text.hpp 'return boot9_found ? Gate::Open : Gate::NoBoot9;' "the entry needs boot9.bin as Titles Delivery's Send Titles does"
need_after ConfigApp/source/dlp_text.hpp 'inline Gate GateOf(bool switch_on, bool boot9_found) {' 'if (!switch_on) {' "the switch comes first: switched off, nothing else about the entry is looked at"
need ConfigApp/source/dlp_text.hpp 'return g == Gate::Off ? "switched off" : (g == Gate::NoBoot9 ? "needs boot9.bin" : "");' "the shut entry says why at the right of its row, as Send Titles says \"needs boot9.bin\""
need ConfigApp/source/dlp_text.hpp 'return g == Gate::Off ? "Switched off: Settings > Download Play delivery." : "boot9.bin is missing: press Y to see how to get it.";' "A on the shut entry answers with a line that says why and where"
need ConfigApp/source/dlp_text.hpp $'return "Sharing 3DS Link needs boot9.bin, the key file of this console. 3DS Link itself works without it.\\n\\nPress Y to see, step by step, how to get it.";' "the note says that sharing needs boot9.bin, that 3DS Link works without it, and the key of the guide (two lines, a blank one and the Y line: all it has room for on the main page, where the emulator's wider font shows four lines of the note)"
need ConfigApp/source/dlp_text.hpp 'The first time it builds the package, and says how long.";' "the note of the open entry is honest about the first time and still short enough to be shown whole (the main page's note has four lines: two, a blank one and one)"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kGuideHint = "Y guide";' "the hint of the guide's key"
need ConfigApp/source/dlp_text.hpp 'text = "Sharing 3DS Link needs boot9.bin, the key file of this console. 3DS Link works without it: it is only needed to share. "' "the page without boot9.bin says what the file is for and that 3DS Link works without it"
need ConfigApp/source/dlp_text.hpp 'text = "It is switched off in Settings (Download Play delivery). Switch it on there to use it.";' "the page with the switch off says where to switch it on"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kPrepareKeepAwake = "B stops it. Nothing is left half done. HOME and sleep are off until it ends.";' "the work's screen says how to stop it and that nothing is left half done"
need ConfigApp/source/dlp_text.hpp 'std::string s = "One time only: 3DS Link builds the package that other consoles download.";' "the work's screen says that it is one time only and what it builds (and, next, how long)"
need ConfigApp/source/dlp_text.hpp 'constexpr const char* kCancelPrepareQuestion = "Nothing is left half done, and the work starts again the next time you open this page.";' "the question before a stop says that nothing is left half done"
need ConfigApp/source/pages_dlp.cpp 'DrawPopupBox(box, "Stop now?", dlp::kCancelPrepareQuestion);' "the popup before a stop asks \"Stop now?\" in its title, and the text under it does not ask it again"
! grep -q -F -- 'kCancelPrepareQuestion = "Stop now' ConfigApp/source/dlp_text.hpp || fail "ConfigApp/source/dlp_text.hpp: the text of the stop popup starts with 'Stop now' again (the popup's title is that question: the card would ask it twice)"
need ConfigApp/source/dlp_text.hpp 'inline std::string PrepareIntro(int seconds, bool measured) {' "the time on the work's screen knows whether it is a guess or the measured pace"
need ConfigApp/source/dlp_text.hpp '" It should take about " + time + ", and it can take longer."' "until the pace is measured the time is a first guess, and says that it can take longer (the speeds were never measured on a console)"
need ConfigApp/source/pages_dlp.cpp 'dlp::PrepareIntro(view.plan_seconds, view.measured)' "the screen tells the guess or the measured time, as the work says"
need ConfigApp/source/dlp_prepare.cpp 'RestartPlan(true, job_.app_bytes > 0 ? job_.app_bytes : kUsualAppBytes);' "the copy from the installed title, after the composer refused the file, begins the estimate again (the bar and the clock): the time left must not count the first attempt"
need ConfigApp/source/dlp_prepare.cpp 'pace_ms_ = ui::NowMs();' "the preparer keeps the clock of the estimate itself (it begins with a plan and begins again with the next one)"
need ConfigApp/source/dlp_prepare_logic.hpp 'return PaceKnown(fraction, elapsed_s) ? elapsed_s / fraction : plan.total;' "the whole time is the plan's until a seventh of the work and two seconds have shown the pace"
need ConfigApp/source/dlp_prepare_logic.hpp 'return fraction >= 0.15 && elapsed_s >= 2.0;' "the pace is trusted after a seventh of the work and two seconds (the time left and the time of the whole work say the same)"
need ConfigApp/source/pages_dlp.cpp 'Fail(dlp::Failure::Prepare, dlp::PrepareFailedText(dlp::FailureSentence(phrase, extra), dlp::PrepAdvice(rc)));' "the card never reads the composer's phrase twice (the composer's detail starts with it)"
need ConfigApp/source/pages_dlp.cpp 'if (dlp::FileIsToBlame(app_file_) || refused) {' "when there is no way to the installed copy, a file in the dlp folder that cannot be used is named first (not \"the key is missing\" alone)"
need ConfigApp/source/pages_dlp.cpp 'Fail(dlp::Failure::BadAppFile, dlp::BadAppFileText(g.paths.dlp_app_cia, found, setup::kVersion));' "...with its path, what is wrong with it, its version when known, and the two ways out"
need ConfigApp/source/dlp_prepare_logic.hpp 'return app_file != Verdict::Missing && app_file != Verdict::Good;' "a file that is there and is no good is to blame; one that is not there is the plain case"
need ConfigApp/source/pages_main.cpp 'return dlp::GateOf(g.app_cfg.dlp_delivery, g.boot9.found);' "the main page decides from the switch and from boot9.bin as the app found it"
need ConfigApp/source/pages_main.cpp 'dlp.dim = gate != dlp::Gate::Open;' "a shut entry is faint"
need ConfigApp/source/pages_main.cpp 'if (gate != dlp::Gate::Open) {' "A on a shut entry only says why: the page is not pushed"
need ConfigApp/source/pages_main.cpp 'g.RefreshBoot9(); // (the card is looked at now: a boot9.bin put there since the last look counts)' "the card is looked at when A is pressed"
need ConfigApp/source/pages_main.cpp 'return row != nullptr && row->id == kDlpDelivery && EntryGate() == dlp::Gate::NoBoot9;' "the guide's key is offered on the entry that is shut for the lack of boot9.bin, and only there"
need ConfigApp/source/pages_main.cpp 'if ((in.down & KEY_Y) && GuideKeyOffered()) {' "Y opens the guide on that entry"
need ConfigApp/source/pages_dlp.cpp 'if (!g.app_cfg.dlp_delivery) {' "the page looks at the switch again before it does anything (a second lock)"
need ConfigApp/source/pages_dlp.cpp 'if (!g.boot9.found) {' "the page looks at boot9.bin again before it does anything (a second lock)"
[ "$(grep -rn "NewDlpHostPage()" ConfigApp/source | grep -v "ConfigApp/source/pages_main.cpp\|ConfigApp/source/pages_dlp.cpp\|ConfigApp/source/delivery.hpp" | wc -l)" = "0" ] || fail "the page of 3DS Link Delivery is opened from a place other than the main page's entry (which is shut by the switch and by boot9.bin)"
[ "$(grep -rn -E "dlpc_(compose|stage_work)\(" ConfigApp/source | grep -v "^ConfigApp/source/dlp_prepare" | wc -l)" = "0" ] || fail "the composer is called from a file other than the work of the package (dlp_prepare.cpp, dlp_prepare_logic.hpp)"
# The guide "How to get boot9.bin" (ConfigApp/guide, shared by the app and the installer child): its keys (L/R and the D-pad's up and down move, B closes: the steps are a vertical list), the seven pictures, the folders it
# names, which are the ones the app looks in (the guide must not lie), and the three places it shows: the installer when it ends without boot9.bin on the card, the app where it explains
# boot9.bin, and the 3DS Link Delivery page while the room is up (a free key, the room goes on).
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyB = 1u << 1;' "the guide's B is libctru's KEY_B"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyDUp = 1u << 6;' "the D-pad's up is libctru's"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyDDown = 1u << 7;' "the D-pad's down is libctru's"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyR = 1u << 8;' "R is libctru's"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyL = 1u << 9;' "L is libctru's"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyCPadUp = 1u << 30;' "the Circle Pad's up is libctru's (KEY_UP includes it: every list of the app answers to it)"
need ConfigApp/guide/guide_logic.hpp 'constexpr u32 kKeyCPadDown = 1u << 31;' "the Circle Pad's down is libctru's"
need ConfigApp/guide/guide_logic.hpp 'const bool back = (keys_down & (kKeyL | kKeyDUp | kKeyCPadUp)) != 0;' "L, the D-pad's up and the Circle Pad's up go back (the steps are a vertical list)"
need ConfigApp/guide/guide_logic.hpp 'const bool forward = (keys_down & (kKeyR | kKeyDDown | kKeyCPadDown)) != 0;' "R, the D-pad's down and the Circle Pad's down go forward"
! grep -q -E 'kKeyDLeft|kKeyDRight' ConfigApp/guide/guide_logic.hpp ConfigApp/guide/guide.cpp || fail "the guide names the D-pad's left or right again (the steps are a vertical list: up and down move them, left and right do nothing)"
need ConfigApp/guide/guide_logic.hpp 'constexpr const char* kBackKeyText = "Up/L";' "the footer's Back button names the keys that go back"
need ConfigApp/guide/guide_logic.hpp 'constexpr const char* kNextKeyText = "Down/R";' "the footer's Next button names the keys that go forward"
need ConfigApp/guide/guide.cpp 'DrawKeyButton(kBackButton, kBackKeyText, kBackLabel, step_ > 0);' "the footer draws the words of guide_logic.hpp (the test counts them against the button)"
need ConfigApp/guide/guide.cpp 'DrawKeyButton(kNextButton, kNextKeyText, kNextLabel, step_ < total - 1);' "the footer draws the words of guide_logic.hpp (the test counts them against the button)"
need ConfigApp/guide/guide_logic.hpp 'if (back == forward) {' "both directions at once do nothing"
need ConfigApp/guide/guide_logic.hpp 'constexpr int kImageCount = 7;' "the guide has seven pictures (step8 is text only)"
need ConfigApp/guide/guide_logic.hpp '"The dump is in gm9/out on the SD card."' "the last step names the folder GodMode9 dumps to"
need ConfigApp/guide/guide_logic.hpp '"3DS Link finds it there by itself."' "the last step says that 3DS Link finds it (true: paths.cpp looks in gm9/out)"
need ConfigApp/guide/guide_logic.hpp 'constexpr const char* kBoot9AppDir = "sdmc:/3ds/3DSLink";' "the guide's folder of an app installed as a CIA is the app's default folder (paths.hpp kDefaultAppDir)"
need ConfigApp/guide/guide_logic.hpp 'constexpr const char* kBoot9GodModeDir = "sdmc:/gm9/out";' "the guide's other folder is GodMode9's (paths.hpp kGodModeOutDir, hostcfg.c HC_GODMODE_OUT)"
need ConfigApp/guide/guide_logic.hpp 'return size == 0x10000 || size == 0x8000;' "a boot9 dump is 64 KB or 32 KB, as paths.cpp says"
need ConfigApp/guide/guide_logic.hpp 'return lower.find("boot9") != std::string::npos && lower.find("boot11") == std::string::npos;' "a boot9 name has boot9 in it and not boot11, as paths.cpp says"
need ConfigApp/source/paths.cpp 'return size == 0x10000 || size == 0x8000;' "the app's own size rule (the guide has the same line)"
need ConfigApp/source/paths.cpp 'return lower.find("boot9") != std::string::npos && lower.find("boot11") == std::string::npos;' "the app's own name rule (the guide has the same line)"
need ConfigApp/source/paths.cpp 'for (const std::string& place : {paths.boot9_beside, paths.boot9_in_data, paths.boot9_godmode, saved_path}) {' "the app looks for boot9.bin next to itself, in its own folder, in GodMode9's folder and where the user pointed: what the guide's last step says"
need ConfigApp/source/paths.cpp 'for (const std::string& dir : {paths.appdir, paths.datadir, std::string(kGodModeOutDir)}) {' "a dump with GodMode9 2.x's name (<serial>_boot9_00.bin) is found in the same folders"
need ConfigApp/source/module_config.cpp 'hc_set_boot9_path(boot9.found ? boot9.path.c_str() : "");' "the delivery module is given the boot9.bin the app found (hc_boot9_path), by the same search the guide describes"
need ConfigApp/titleshare_host/hostcfg.c '#define HC_GODMODE_OUT "sdmc:/gm9/out"' "the module looks for movable.sed where GodMode9 dumps it, in the same folder as boot9.bin"
need ConfigApp/guide/guide.cpp $'static_assert(kKeyB == KEY_B && kKeyDUp == KEY_DUP && kKeyDDown == KEY_DDOWN && kKeyR == KEY_R && kKeyL == KEY_L && kKeyCPadUp == KEY_CPAD_UP && kKeyCPadDown == KEY_CPAD_DOWN, "guide_logic.hpp\'s key bits are libctru\'s");' "the bits of the guide's keys are checked against libctru's at build time"
for n in 1 2 3 4 5 6 7; do
  [ -f ConfigApp/guide/gfx/step$n.png ] && [ -f ConfigApp/guide/gfx/step$n.t3s ] || fail "the guide has no picture $n (ConfigApp/guide/gfx/step$n.png and .t3s: DlpLink/tools/make_guide.py makes them)"
  grep -q -x "step$n.png" ConfigApp/guide/gfx/step$n.t3s || fail "ConfigApp/guide/gfx/step$n.t3s does not name step$n.png"
done
[ "$(ls ConfigApp/guide/gfx/*.t3s | wc -l)" = "7" ] || fail "the guide has a number of pictures other than seven (guide_logic.hpp kImageCount)"
need ConfigApp/source/pages_startup.cpp '} else if ((in.down & KEY_Y) || Tapped(in, GuideRect())) {' "Y on the first start's question opens the guide"
need ConfigApp/source/pages_startup.cpp 'DrawButton(GuideRect(), "Y", "How to get boot9.bin", true, PressedNow(g.input.in(), GuideRect()), true);' "the first start's question shows the key of the guide"
need ConfigApp/source/pages_check.cpp 'if ((in.down & KEY_Y) && Boot9RowToFix() && !pages::OpenBoot9Guide()) {' "Y on the boot9.bin row of the installation check opens the guide"
need ConfigApp/source/pages_check.cpp 'return Boot9RowToFix() ? dlp::kGuideHint : "";' "the installation check's footer says Y on the boot9.bin row that is not fine"
need ConfigApp/source/pages_check.cpp '} else if (!pages::OpenBoot9Guide()) {' "the installation check's choice \"how to get it\" opens the guide"
need ConfigApp/source/pages_delivery.cpp 'if (GuideKeyOffered() && !pages::OpenBoot9Guide()) {' "Y on Titles Delivery's Send Titles without boot9.bin opens the guide (the footer's Y button: OnExtra, which a press of Y and a tap on the button both reach)"
need ConfigApp/source/pages_delivery.cpp 'out[0] = FooterKey{"Y", dlp::kGuideLabel, KEY_Y};' "the footer of Titles Delivery's page shows Y as a button (the page has no START button, so a hint line would not be drawn: the layout gives it room only beside START)"
! tr -d '\r' < ConfigApp/source/pages_delivery.cpp | sed -n '/^class DeliveryPage/,/^};/p' | grep -q 'Hint() const override' || fail "ConfigApp/source/pages_delivery.cpp: Titles Delivery's page has a footer hint again (it has no START button: the hint is never drawn there)"
need ConfigApp/source/pages_delivery.cpp 'return row != nullptr && row->id == kSend && ready_ && !g.boot9.found;' "the guide's key is offered on Send Titles while boot9.bin is missing"
need ConfigApp/source/status.cpp 'A: search the SD card, choose a folder, or see how to get it. Y: the guide, step by step.' "the note of the boot9.bin row says A and Y"
need ConfigApp/source/pages.hpp 'bool OpenBoot9Guide();' "the guide opens from any page through one function"
need ConfigApp/source/pages_dlp.cpp 'if ((in.down & KEY_Y) || Tapped(in, r.extra[1])) {' "Y (or a tap on its button) opens the guide while the room is up"
need ConfigApp/source/pages_dlp.cpp 'f.extra[1] = FooterKey{"Y", dlp::kGuideLabel, KEY_Y}; // (the guide "How to get boot9.bin": free while the room is up)' "the footer of the room's page shows the guide's key"
need ConfigApp/source/pages_dlp.cpp 'guide::Viewer viewer_;' "the guide is a viewer INSIDE the page of the room (the room and its thread go on): not a page pushed over it"
need_after ConfigApp/source/pages_dlp.cpp '        Refresh();' 'if (guide_open_) {' "the room's snapshot is read before the guide takes the keys: the room goes on while the guide is up"
need ConfigApp/source/pages_dlp.cpp 'if (dl_screens_off() != 0) {' "the guide needs the screens: they are lit when it opens"
# The top screen of 3DS Link Delivery shows the radio channel of the room ("Wi-Fi CH 6") like Titles Delivery does (1.0 did not: the user's console test, 2026-10-22). The channel is the firmware's
# choice for each room, so the thread that makes the room asks the wireless service for it (when the room is new, and once a second while it is not known) and the page only reads the number: a
# call made by the screen would wait while a room is being made.
need ConfigApp/titleshare_host/delivery.h 'int dl_wifi_channel(void);' "the delivery module can say the channel of the room"
need ConfigApp/titleshare_host/delivery.c 'if(!gUdsOk || R_FAILED(udsGetChannel(&channel))) {' "no network, no channel (0: the top screen draws nothing)"
need ConfigApp/source/pages_dlp.cpp 'std::atomic<int> channel{0};' "the channel is a number the thread writes and the page reads"
need ConfigApp/source/pages_dlp.cpp 'if (!s.remaking && (s.remade != seen_remade || (radio->channel.load() == 0 && ++retry >= 100))) {' "the channel is read when the room is new and once a second while unknown, never in the middle of a re-make"
need ConfigApp/source/pages_dlp.cpp 'DrawTitleBarStatus(gfx::kTopW, "3DS Link Delivery", failed_ || radio_ == nullptr ? 0 : radio_->channel.load());' "the top screen of the room shows its channel"
[ "$(grep -c 'dl_wifi_channel()' ConfigApp/source/pages_dlp.cpp)" = "1" ] || fail "ConfigApp/source/pages_dlp.cpp asks the wireless service for the channel from more or fewer places than the radio thread"
! grep -q -F -- 'OpenBoot9Guide' ConfigApp/source/pages_dlp.cpp || fail "ConfigApp/source/pages_dlp.cpp has 'OpenBoot9Guide' (the page of the room does not push the guide as a page (it would stop the page that keeps the room alive))"
[ "$(grep -c "NewDlpHostPage" ConfigApp/source/pages_main.cpp)" = "1" ] || fail "ConfigApp/source/pages_main.cpp opens 3DS Link Delivery from more or fewer places than the one entry"
# About 3DS Link and the credits (ConfigApp/source/about_text.hpp, pages_config.cpp, pages_credits.cpp, and AzaharLink/CREDITS.md, the file the GitHub repository carries). The NAME of the app is
# "3DS Link" and "Connect, share, play together" is its claim (the first line of the note of the first row); the row "This app: unknown" is gone (the page says how the app was installed); the page says
# "Created by Gionko"; the credits are ONE list that the page and CREDITS.md both say (Tests/settings_test/about_test.cpp compares them, word for word, and looks each claim up in the tree).
need ConfigApp/source/about_text.hpp 'constexpr const char* kAppName = "3DS Link";' "the name of the app is 3DS Link"
need ConfigApp/source/about_text.hpp 'constexpr const char* kClaim = "Connect, share, play together";' "Connect, share, play together is the claim, not the name"
need ConfigApp/source/about_text.hpp 'constexpr const char* kCreatedBy = "Created by Gionko";' "About says who made the app"
need ConfigApp/source/pages_config.cpp 'version.label = about::kAppName;' "the first row of About is the name of the app"
need ConfigApp/source/pages_config.cpp 'version.note = about::AppNote(setup::kVersion);' "the claim and the one version are the note of the first row"
need ConfigApp/source/pages_config.cpp 'author.label = about::kCreatedBy;' "About says Created by Gionko"
need ConfigApp/source/pages_config.cpp 'app.label = "Installed as";' "About says how the app was installed, not where it was started from"
need ConfigApp/source/pages_config.cpp 'origin_ = about::OriginOf(g.app_path, envIsHomebrew(), program_known, program_id, dlp::kAppTid);' "the installed app is known by its title id, not only by the lack of a path"
need ConfigApp/source/pages_config.cpp 'about::DescribeOrigin(origin_, g.app_path, g.paths.appdir, g.paths.datadir, dlp::kAppTid);' "the row Installed as says what is true for the way the app was started"
need ConfigApp/source/pages_config.cpp 'about::DescribePluginRow(g.plugin_card, setup::EmbeddedPluginUsable(),' "the plugin row of About is the card against the plugin inside the app"
need ConfigApp/source/pages_config.cpp 'credits.label = about::kCreditsLabel;' "About leads to the credits"
need ConfigApp/source/pages_config.cpp 'PushPage(pages::NewCreditsPage());' "A on the credits row opens the credits"
! grep -q -F '"unknown"' ConfigApp/source/pages_config.cpp || fail "ConfigApp/source/pages_config.cpp says \"unknown\" again (About says what is true: the installed app, the file, or no path given)"
! grep -q -F '"This app"' ConfigApp/source/pages_config.cpp || fail "ConfigApp/source/pages_config.cpp has the row \"This app\" again (it is \"Installed as\")"
need ConfigApp/source/pages_credits.cpp 'row.label = credit.name;' "a row of the credits is a credit of the list"
need ConfigApp/source/pages_credits.cpp 'row.note = about::CreditNote(credit);' "the note of a credit says what was taken, from whom, under which license"
need ConfigApp/source/pages.hpp 'ui::Page* NewCreditsPage();' "the credits open from About through one function"
[ -f CREDITS.md ] || fail "AzaharLink/CREDITS.md is not there (the credits: the file the GitHub repository carries; it says what about_text.hpp says)"
for n in "Azahar Emulator" FBI 3dbrew devkitPro libctru "citro2d, citro3d" Luma3DS CTRPluginFramework ENet GodMode9 "makerom, ctrtool" bannertool Pillow; do
  tr -d '\r' < CREDITS.md | grep -F -x -q -- "### $n" || fail "CREDITS.md has no entry for $n (the list of about_text.hpp and CREDITS.md are one list)"
done
# The build chain (DlpLink/tools): plugin -> installer -> MODEL (a seed child with stand-in files: no app needed) -> app -> 3dslink.cia -> reference child (makerom: the oracle of the tests, not
# a deliverable) -> the checks of the chain. The model does not depend on the app, so there is no cycle; the guard of the registry looks at what is delivered (the app's CIA, the model, the plugin).
if [ -d ../DlpLink ]; then
  BA=../DlpLink/tools/build_all.sh
  L1=$(grep -n "^make_in DlpLink/child " "$BA" | head -1 | cut -d: -f1); L2=$(grep -n "bash DlpLink/tools/make_child_model.sh" "$BA" | head -1 | cut -d: -f1)
  L3=$(grep -n "^  make_in AzaharLink/ConfigApp " "$BA" | head -1 | cut -d: -f1); L4=$(grep -n "bash DlpLink/tools/build_cias.sh" "$BA" | head -1 | cut -d: -f1)
  { [ -n "$L1" ] && [ -n "$L2" ] && [ -n "$L3" ] && [ -n "$L4" ] && [ "$L1" -lt "$L2" ] && [ "$L2" -lt "$L3" ] && [ "$L3" -lt "$L4" ]; } || fail "DlpLink/tools/build_all.sh does not build in the order installer, model, app, CIAs (the app embeds the model, which is made from the installer without the app)"
  BC=../DlpLink/tools/build_cias.sh
  C1=$(grep -n "make_3dslink_cia.sh" "$BC" | tail -1 | cut -d: -f1); C2=$(grep -n "make_dlp_child.sh" "$BC" | tail -1 | cut -d: -f1); C3=$(grep -n "check_chain.sh" "$BC" | tail -1 | cut -d: -f1); C4=$(grep -n "run_checks.sh" "$BC" | tail -1 | cut -d: -f1)
  { [ -n "$C1" ] && [ -n "$C2" ] && [ -n "$C3" ] && [ -n "$C4" ] && [ "$C1" -lt "$C2" ] && [ "$C2" -lt "$C3" ] && [ "$C3" -lt "$C4" ]; } || fail "DlpLink/tools/build_cias.sh does not run in the order app CIA, reference child, checks of the chain, tests"
  ! grep -q "OUT/3dslink" ../DlpLink/tools/make_child_model.sh || fail "DlpLink/tools/make_child_model.sh uses the app's CIA: the model must not depend on the app (the app embeds the model, the child embeds the app: a cycle)"
  ! grep -q "dist/dlp" ../DlpLink/tools/build_all.sh || ! grep -q "cp .*dist/dlp" ../DlpLink/tools/build_all.sh || fail "DlpLink/tools/build_all.sh copies a child into dist/dlp (the child is not delivered: the app makes it)"
fi
[ ! -e dist/dlp ] || fail "dist/dlp is there: the Download Play child is not delivered (3DS Link is ONE CIA: the app builds the child)"
# The plugin is ONE package with the app (the user's rule, 2026-10-22: "pacchetto unico"): AzaharLink.3gx is a RESOURCE of the app (data/AzaharLink.3gx, linked in as AzaharLink_3gx), the app puts
# it on the SD card by itself, at every start and from the Installation check, comparing the file BYTE for byte with the plugin inside it, the 3DS Link switch cannot be ON without it, and the
# Download Play child is composed from the resource, never from the card. The build copies the plugin it has just built into data/ before the app, and what is delivered carries exactly it.
need_line ConfigApp/Makefile $'%.3gx.o\t%_3gx.h :\t%.3gx' "the plugin (data/AzaharLink.3gx) is linked in by bin2o as AzaharLink_3gx"
need ConfigApp/source/plugin_resource.cpp '#include "AzaharLink_3gx.h"' "the plugin resource is the symbol of data/AzaharLink.3gx"
need ConfigApp/source/plugin_resource.cpp 'return word != 0 && PluginVersionText(word) == kVersion;' "a plugin resource can be used only when it is a .3gx of the version of the app"
[ "$BUILT" != 1 ] || [ -f ConfigApp/data/AzaharLink.3gx ] || fail "ConfigApp/data/AzaharLink.3gx is missing (the plugin the app carries as a resource: DlpLink/tools/build_all.sh copies the plugin it built there)"
[ ! -f AzaharLink.3gx ] || cmp -s AzaharLink.3gx ConfigApp/data/AzaharLink.3gx || fail "ConfigApp/data/AzaharLink.3gx is not the plugin of the last build (AzaharLink.3gx): the app would carry another plugin (DlpLink/tools/build_all.sh copies it before the app)"
if [ -d ../DlpLink ]; then
  [ "$BUILT" != 1 ] || "$PY" ../DlpLink/tools/check_chain.py plugin ConfigApp/data/AzaharLink.3gx --version "$VERSION" --copy ConfigApp/data/AzaharLink.3gx --in dist/3DSLink.3dsx dist/3DSLink.cia >/dev/null 2>&1 || fail "dist/3DSLink.3dsx or dist/3DSLink.cia does not carry the plugin that is in ConfigApp/data, or that plugin does not say $VERSION (the app that is delivered must carry exactly the plugin of its own build: DlpLink/tools/build_all.sh --dist; check_chain.py plugin says which and why)"
  BA=../DlpLink/tools/build_all.sh
  LP=$(grep -n 'libctrpf' "$BA" | head -1 | cut -d: -f1); LC=$(grep -n 'cp AzaharLink/AzaharLink.3gx AzaharLink/ConfigApp/data/AzaharLink.3gx' "$BA" | head -1 | cut -d: -f1); LA=$(grep -n "^  make_in AzaharLink/ConfigApp " "$BA" | head -1 | cut -d: -f1)
  { [ -n "$LP" ] && [ -n "$LC" ] && [ -n "$LA" ] && [ "$LP" -lt "$LC" ] && [ "$LC" -lt "$LA" ]; } || fail "DlpLink/tools/build_all.sh does not build the plugin, copy it into ConfigApp/data and only then build the app (the app carries the plugin of THIS build)"
  need ../DlpLink/tools/check_chain.sh '$CHK plugin "$PLUGIN_3GX" --version "$VERSION" --copy "$APP_PLUGIN" --in "$ELF_LINK" "$APP_DIR/3DSLink.3dsx" "$OUT/3dslink.cia" || rc=1' "the app's ELF, .3dsx and CIA carry exactly the plugin of this build (size and SHA-256), the resource is that file, and its header says VERSION"
  need ../DlpLink/tools/check_chain.py 'say(found == a.version,' "the check of the plugin compares the version in its header with VERSION"
  need ../DlpLink/tools/check_chain.py 'say(same, f"{a.copy} is the plugin of this build' "... and the resource the app is linked with with the plugin that was built"
  need ../DlpLink/tools/check_chain.py 'say(at >= 0, f"{os.path.basename(where)} carries the plugin' "... and looks for every byte of it in each file"
  need ../DlpLink/tests/selftest_chain.py 'def plugin_checks(tmp):' "the check of the plugin is tried against another version, a copy that differs by a byte, an app with another plugin or a cut one (a check that cannot say NO checks nothing)"
  need ../DlpLink/tests/selftest_chain.py '        plugin_checks(tmp)' "... and that try runs"
fi
#  - the file on the card is compared with the plugin inside the app, BYTE for byte (a header that looks good says nothing), and a file that is not the same is replaced (one version), never kept
need ConfigApp/source/plugin_install.hpp 'if (p.same_bytes) {' "the file on the card is the plugin of the app when its bytes say so (and only then)"
need ConfigApp/source/plugin_install.hpp 'return p.version_word != wanted_version_word ? PluginCard::OtherVersion : PluginCard::Altered;' "a good header over bytes that are not the app's is Altered, never the same"
need ConfigApp/source/plugin_install.hpp 'if (!embedded_usable || card == PluginCard::Same) {' "a plugin that is the app's is left alone, and a build with no plugin inside puts none"
need ConfigApp/source/plugin_install.hpp 'return card == PluginCard::Missing ? PluginStep::Put : PluginStep::Replace;' "a file that is not the plugin of the app is replaced, never kept: the app and its plugin are one version"
need ConfigApp/source/plugin_install.cpp 'probe.same_bytes = probe.size == size && FileEqualsBytes(path, data, size);' "the compare is the size and every byte"
need ConfigApp/source/plugin_install.cpp 'const std::string part = dest + ".part";' "the plugin is written to a .part file first"
need ConfigApp/source/plugin_install.cpp 'if (!FileEqualsBytes(part, data, size)) {' "... read back against the plugin before it takes the place of the old file"
need ConfigApp/source/plugin_install.cpp 'if (std::rename(part.c_str(), dest.c_str()) != 0) {' "... and put in place by a rename"
need ConfigApp/source/plugin_install.cpp 'std::rename(old.c_str(), dest.c_str()); // (the old file goes back)' "a swap that fails puts the old file back"
need ConfigApp/source/plugin_install.cpp 'result.write = WriteResult::NotVerified; // (written, and the card does not hold it: say so, never "put")' "a file that was written and is not the plugin on the card is a failure, never 'put'"
need ConfigApp/source/plugin_ensure.cpp 'setup::EmbeddedPluginSize(), setup::EmbeddedPluginUsable(),' "the app puts only a plugin that is a .3gx of its own version"
need ConfigApp/source/plugin_ensure.cpp 'plugin_copies_checked = false;' "a plugin file that was just written is another file: the copies in the titles' folders are looked at again with it"
need ConfigApp/source/plugin_ensure.cpp 'app_cfg.plugin_stamp.clear();' "... and the stamp of the old file is forgotten, or the quiet look would be skipped"
need ConfigApp/source/paths.cpp 'return paths.plugin_in_data;' "when no plugin file is anywhere, the one in use is in the app's own folder (sdmc:/3ds/3DSLink for a CIA install), where the app puts it"
#  - when it is done: at every start (the first start after its question was answered Yes, never before: until then the app only reads), and from the Installation check
need ConfigApp/source/ui.cpp '    EnsurePlugin();' "at every start the app puts its own plugin on the card when it is missing or is not the same (OpenDataFolder ends with it)"
need_after ConfigApp/source/pages_startup.cpp 'void Begin() {' 'g.OpenDataFolder();' "the first start opens the data folder (and so puts the plugin on the card) only after the question was answered Yes"
! grep -q 'EnsurePlugin' ConfigApp/source/pages_startup.cpp || fail "pages_startup.cpp puts the plugin on the card by itself: the first start's question comes first, and until it is answered Yes the app only reads (App::OpenDataFolder, which Begin() calls after Yes, does it)"
[ "$(grep -c 'g.EnsurePlugin()' ConfigApp/source/pages_check.cpp)" = "2" ] || fail "ConfigApp/source/pages_check.cpp: the plugin is put back when the Installation check opens and on Fix (two places call EnsurePlugin)"
need_after ConfigApp/source/pages_check.cpp 'case setup::CheckAction::RestorePlugin:' 'pages::RestorePlugin();' "A on the plugin row puts the plugin back from the app (nothing is searched for)"
! grep -rq 'StartPluginSearch' ConfigApp/source || fail "a search of the card for the plugin is back: the plugin is a resource of the app, and Fix puts it back from there"
#  - the 3DS Link switch cannot be turned ON without the plugin (after the install step had its chance): dim, A and the D-pad say why, it goes OFF by itself when it was ON and stays OFF when the file is back
need ConfigApp/source/plugin_install.hpp 'return luma_too_old ? SwitchLock::Luma : (plugin_ready ? SwitchLock::None : SwitchLock::Plugin);' "a Luma3DS that is too old shuts the switch first, then a plugin that is not on the card"
need ConfigApp/source/plugin_install.hpp 'return enabled && tunnel && lock == SwitchLock::None;' "a shut switch shows OFF"
need ConfigApp/source/ui.cpp 'plugin_ready = plugin_card == setup::PluginCard::Same;' "the plugin is ready only when it is, byte for byte, the plugin of the app"
need ConfigApp/source/ui.cpp 'plugin_locked = setup::LockOfSwitch(luma_too_old, plugin_ready) == setup::SwitchLock::Plugin;' "the switch is shut by a plugin that is not on the card"
need ConfigApp/source/ui.cpp '} else if (plugin_locked && cfg.enabled) {' "a switch that was ON goes OFF by itself, once, when the plugin is not on the card (like with a Luma3DS that is too old)"
need ConfigApp/source/pages_main.cpp 'power.on = setup::SwitchIsOn(g.cfg.enabled, g.cfg.tunnel, lock);' "the main page's switch shows what the rule says"
need_after ConfigApp/source/pages_main.cpp 'if (g.plugin_locked) {' 'g.ShowToast(g.PluginLockText(), col::kWarn, 12000);' "A and the D-pad on the shut switch say why in a toast"
need ConfigApp/source/pages_main.cpp 'g.ShowToast(g.plugin_notice, g.plugin_notice_color, 7000);' "what the app did about its plugin at the start is said once, on the main page"
# The review of 1.0.1 (F): the reason a plugin could not be put is a sentence of 100 to 150 letters that runs by on the main page, so a long toast stays up for the run (two seconds and 150 ms a
# letter); the main card says the FILE is not right (it may be there, of another version); the "puts it back by itself" of the note is only for a build that has a plugin to put; the
# stray ".old" file of a power cut at the end of the swap is taken away; the child is about 3 MB (12 s, not 10).
need ConfigApp/source/plugin_install.hpp 'inline std::uint64_t ToastMsToRead(std::size_t letters, std::uint64_t given_ms) {' "a long toast stays up for the run of the marquee"
need ConfigApp/source/plugin_install.hpp 'const std::uint64_t run = 2000 + 150 * static_cast<std::uint64_t>(letters);' "...two seconds and 150 ms a letter (the step of gfx::MarqueeText)"
need ConfigApp/source/plugin_install.hpp 'return letters > 40 && run > given_ms ? run : given_ms;' "...for a text of more than 40 letters; a short one keeps its time"
need ConfigApp/source/ui.cpp 'toast_until = toast_from + setup::ToastMsToRead(text.size(), ms);' "every toast goes through the time it needs"
need ConfigApp/source/pages_main.cpp 'message = "The plugin file is not right";' "the main card says the file is not right (it may be there, of another version)"
need ConfigApp/source/pages_main.cpp '(setup::EmbeddedPluginUsable() ? std::string(" ") + setup::kPluginLockMore : std::string())' "the note promises that the plugin is put back only when the app has one to put"
need ConfigApp/source/plugin_install.cpp 'if (result.after == PluginCard::Same && IsFile(dest + ".old")) {' "a stray .old file of a power cut is taken away once the card holds the plugin"
need ConfigApp/source/dlp_text.hpp '. About 12 s to download, a few more to install."' "the Download Play child is about 3 MB: 12 s"
need ConfigApp/source/about_text.hpp '{"ENet", "MIT", "Copyright (c) 2002-2024 Lee Salzman",' "ENet's MIT notice is carried in its credit"
#  - the Download Play child is composed from the plugin INSIDE the app (a memory read), never from the file on the card
need ConfigApp/source/dlp_prepare.cpp 'job.read_plugin = &Hooks::ReadEmbeddedPlugin;' "the composer reads the plugin from the resource of the app"
need ConfigApp/source/dlp_prepare.cpp 'plugin_bytes_ = setup::EmbeddedPluginSize();' "the size of the plugin in the plan is the resource's"
need ConfigApp/source/pages_dlp.cpp 'if (!setup::EmbeddedPluginUsable()) {' "a build whose plugin resource is no plugin of the app's version has none to give: the page says so and does not go on"
! grep -q 'plugin_path' ConfigApp/source/dlp_prepare.cpp ConfigApp/source/dlp_prepare.hpp ConfigApp/source/pages_dlp.cpp || fail "the package page or the work has a plugin_path again: the composer's plugin is the resource of the app, never a file of the card"
! grep -q -E 'DescribePlugin|ResolvePluginFile' ConfigApp/source/dlp_prepare.cpp ConfigApp/source/pages_dlp.cpp || fail "the package page or the work looks at the plugin file of the card: the package carries the plugin inside the app, whatever is on the card"
need ConfigApp/source/dlp_text.hpp 'built without the plugin inside' "a build with no plugin says that, and the way out is the CIA"
# Tests: the unit tests of the new parts (dlp_prepare_test, guide_test: in the list of the test hygiene above) are built AND run by Tests/run.sh, and the installer's own are run by the
# checks of the package
# The neighbour DlpLink (the installer child, the build chain, its tests): the pins that read it, only where the tree has it (a copy of AzaharLink alone has none)
if [ -d ../DlpLink ]; then
  need_line ../DlpLink/child/Makefile $'GUIDE\t\t:=\t../../AzaharLink/ConfigApp/guide' "the installer builds the same guide module as the app"
  need_line ../DlpLink/child/Makefile $'SOURCES\t\t:=\tsource $(GUIDE)' "the installer compiles guide.cpp"
  need_line ../DlpLink/child/Makefile $'INCLUDES\t:=\tsource $(GUIDE)' "the installer's include path has its own gfx.hpp and the guide"
  need_line ../DlpLink/child/Makefile $'GRAPHICS\t:=\t$(GUIDE)/gfx' "the installer carries the pictures"
  need_line ../DlpLink/child/Makefile $'GFXBUILD\t:=\t$(BUILD)' "the pictures are linked in as data"
  need ../DlpLink/child/source/installer_logic.hpp 'constexpr u32 kGuideKey = kPadY;' "the guide's key on the installer's finished screen is Y"
  need ../DlpLink/child/source/installer_logic.hpp 'constexpr int kScreenGuide = 100;' "the guide is a screen of its own for the key lockout"
  need ../DlpLink/child/source/installer_logic.hpp 'if (done && !boot9_found && LeavesWorkingApp(outcome)) {' "the notice is for a finished job that left a working app, without boot9.bin on the card (it never blocks and never shows when boot9.bin is there)"
  need ../DlpLink/child/source/installer_logic.hpp 'return outcome == OUT_INSTALLED || outcome == OUT_UPDATED || outcome == OUT_DOWNGRADED || outcome == OUT_REPAIRED || outcome == OUT_CURRENT;' "the notice is for installed, updated, replaced, restored and kept-the-same, not for \"left as it is\" and \"a newer one stays\""
  need ../DlpLink/child/source/installer_logic.hpp 'constexpr u32 kLeavePads = kPadA | kPadB;' "only A and B take the user off a finished screen (1.0.1: every button did, and a user who pressed X for Y was thrown to the HOME menu)"
  need ../DlpLink/child/source/installer_logic.hpp 'if (done && guide_offered && (down & kGuideKey) != 0) {' "the guide's key opens the guide, whatever else is down in the same frame"
  need ../DlpLink/child/source/installer_logic.hpp 'const u32 leave = done ? kLeavePads : kPadB;' "a failure screen that cannot be tried again is left with B only (A is not on its footer)"
  need ../DlpLink/child/source/main.cpp 'L("leave: the user pressed %s (buttons down 0x%08lX, phase %d)"' "the log says which button took the user off the screen (and the line below says when the system closed the app: the two looked the same)"
  need ../DlpLink/child/source/main.cpp 'L("leave: aptMainLoop() returned false (the system asks the app to close)");' "...and when it was the system"
  [ "$(grep -c '"Done. Press A: the console goes back to the HOME menu, where 3DS Link is waiting."' ../DlpLink/child/source/main.cpp)" = "2" ] || fail "../DlpLink/child/source/main.cpp: the finished screen (with and without the notice) says which button leaves (A), in both places"
  ! grep -q -F -- 'Press any button' ../DlpLink/child/source/main.cpp || fail "../DlpLink/child/source/main.cpp has 'Press any button' (only A and B leave a finished screen)"
  need ../DlpLink/child/source/main.cpp 'g_boot9_found = guide::Boot9Found();' "the installer looks for boot9.bin once for each finished screen"
  need ../DlpLink/child/source/main.cpp 'return std::string("boot9.bin was not found in ") + guide::kBoot9AppDir + " or " + guide::kBoot9GodModeDir;' "the installer's heading names the two folders the search looked in (the guide's own constants: Boot9Found looks nowhere else)"
  need ../DlpLink/child/source/main.cpp 'const float head_h = gfx::DrawWrapped(34, card_y + 8.0f, 270, 0.44f, col::kText, Boot9Heading(), 2);' "the heading is longer than a line: it wraps (two lines) and the explanation starts under its real height"
  # The installer installs 3DS Link and nothing else: the job deletes the title the manifest names, and its ticket, before it installs, so a manifest that names another title is refused whole.
  need ../DlpLink/child/source/installer_logic.hpp 'constexpr u64 kAppTid = 0x000400000F83D100ull;' "the installer installs 3DS Link's title and nothing else (DlpLink/tools/common.sh: unique id 0x0F83D1)"
  need ../DlpLink/child/source/installer_logic.hpp 'return m.tid == kAppTid;' "a manifest names 3DS Link's title or the package is refused"
  need ../DlpLink/child/source/main.cpp 'if (!inst::NamesApp(man)) {' "the job refuses a manifest that names another title before it reads the CIA or touches the console"
  need ../DlpLink/child/source/main.cpp $'Fail(inst::kErrNotThis, "the manifest names a title that is not 3DS Link\'s");' "...with an error of its own in the log (the screen says the package is damaged)"
  [ "$(grep -o 'kAppTid = 0x[0-9A-Fa-f]*ull' ../DlpLink/child/source/installer_logic.hpp)" = "$(grep -o 'kAppTid = 0x[0-9A-Fa-f]*ull' ConfigApp/source/dlp_prepare_logic.hpp)" ] || fail "the installer and the app do not say the same title for 3DS Link (installer_logic.hpp and dlp_prepare_logic.hpp kAppTid)"
  need ../DlpLink/child/source/main.cpp 'const bool lock_open = lock.Open(g_guide_open ? inst::kScreenGuide : phase, osGetTime());' "the guide is its own screen for the lockout (the B that closes it does not also leave)"
  need ../DlpLink/child/source/main.cpp 'const inst::ResultKey key = inst::ResultKeyFor(down, phase == PH_DONE, GuideOffered(notice, gok));' "the finished screen asks the logic what a press does"
  need ../DlpLink/child/source/main.cpp 'constexpr const char* kCopyPath = "sdmc:/3ds/3DSLink/dlp/3dslink.cia";' "the installer leaves the app's CIA where the app looks for it: <data folder>/dlp/3dslink.cia of an app with no folder of its own"
  need ../DlpLink/child/source/main.cpp 'return inst::SaveCopy("romfs:/3dslink.cia", kDlpDir, size, kCopyTarget, g_buf, sizeof(g_buf), g_hooks);' "the copy is made by SaveCopy: a .part file, a rename, a read back from its final place"
  need ../DlpLink/child/source/main.cpp 'const bool copy_same = SameFileAs(cia, kCopyPath, cia_size);' "the copy is compared once and written only when it is not already the right file"
  need ../DlpLink/child/source/safe_write.hpp 'inline bool SaveCopy(' "the copy goes through safe_write.hpp (a .part file and a rename, the old file put back on a failure)"
  need ../DlpLink/tools/build_all.sh 'cp DlpLink/out/child_model.bin AzaharLink/ConfigApp/data/child_model.bin' "the model that was just made is the resource the app is built with"
  need ../DlpLink/tools/build_all.sh 'rm -f AzaharLink/dist/dlp/child.cia && rmdir AzaharLink/dist/dlp 2>/dev/null || true' "a child left in dist/dlp by an older build is removed: it is not a deliverable"
  need ../DlpLink/tools/build_cias.sh 'step "catena (modello, app, guida, ciclo, registro)"' "the checks of the chain run after the reference child"
  need ../DlpLink/tools/check_chain.sh '$CHK agree "$OUT_MODEL" "$OUT/model_seed/model_of_reference.bin" || rc=1' "the cycle is proven at every build: the model of the reference child (made with the real app in its RomFS) is the model that was made without the app"
  need ../DlpLink/tools/check_chain.sh '$CHK carries "$APP_MODEL" "$APP_DIR"/build/step[1-7].t3x --in "$ELF_LINK" "$APP_DIR/3DSLink.3dsx" "$OUT/3dslink.cia" || rc=1' "the app's ELF, 3dsx and CIA carry the model and the seven pictures of the guide"
  need ../DlpLink/tools/check_chain.sh '$CHK carries "$CHILD_DIR"/build/step[1-7].t3x --in "$ELF_CHILD" "$CHILD_DIR/child.3dsx" "$OUT_MODEL" "$OUT/3dslink-child.cia" || rc=1' "the installer (ELF, 3dsx, the model, the reference child) carries the pictures of the guide"
  need ../DlpLink/tools/check_chain.sh '$CHK model "$OUT_MODEL" --version16 "$VER16" || rc=1' "the model carries the version of 3DS Link (VERSION) in its TMD"
  need ../DlpLink/tools/check_chain.sh 'if cmp -s "$OUT_MODEL" "$APP_MODEL"; then' "the model in the app is the model of this build (the same bytes, or with --no-app the same outside the random fields)"
  need ../DlpLink/tools/version.py '  guard APP.cia MODEL.bin PLUGIN.3gx TSV [--check-only]' "the guard of the registry looks at what is delivered: the app's CIA, the model and the plugin (a Download Play client may have kept the child of that version)"
  need ../DlpLink/tools/version.py 'app, model, plugin, tsv = args' "the guard takes the three files"
  ! grep -q -F -- 'version.py" guard' ../DlpLink/tools/make_dlp_child.sh || fail "../DlpLink/tools/make_dlp_child.sh has 'version.py\" guard' (the reference child is not a deliverable: it has no guard (check_chain.sh has it))"
  need ../DlpLink/tests/run_checks.sh 'bash "$PKG/tests/ship_check.sh" || rc=1' "the model that ships is proven at every build: it composes a child that verify_cia.py accepts, and is the child without its RomFS whatever app is in it"
  need ../DlpLink/tests/run_checks.sh 'bash "$PKG/tests/dlpc_check.sh" || rc=1' "the composer is proven against the reference child at every build"
  need ../DlpLink/tests/ship_check.sh 'same_bytes "$W/m2.bin" "$MODEL" "the model of the child of the OTHER app is the shipped model too: it does not depend on the app"' "the model is the same bytes whatever app is in the child"
  need ../DlpLink/tests/ship_check.sh 'same_bytes "$W/m1.bin" "$MODEL" "the model of the composed child is the shipped model (real variant)"' "the child composed with the shipped model gives the shipped model back"
  need ../DlpLink/tests/run_checks.sh '/tmp/dlplink_child_logic/child_logic_test "${CL_ARGS[@]}" | tail -3 || rc=1' "the installer's decisions (the notice, the keys, the copy) are tested against real CIAs at every build"
  # What the HOME menu shows of the app CIA (the user's console test, 2026-10-22): an unopened package shows the NAME ("3DS Link") and the publisher, not the claim; the top screen has a banner (the
  # small 3D model with the tune: ConfigApp/banner, made by DlpLink/tools/bannertool and put in the ExeFS by makerom) and verify_cia.py refuses a CIA without it.
  need ../DlpLink/tools/common.sh 'APP_CIA_NAME="${APP_CIA_NAME:-3DS Link}"; APP_CIA_DESC="${APP_CIA_DESC:-3DS Link}"; APP_CIA_AUTHOR="${APP_CIA_AUTHOR:-Powered by Gionko}"' "the HOME menu shows the name of an unopened package, never the claim"
  need ../DlpLink/tools/make_3dslink_cia.sh 'smdhtool --create "$APP_CIA_NAME" "$APP_CIA_DESC" "$APP_CIA_AUTHOR" "$ICON_LINK" "$OUT/3dslink-cia.smdh"' "the CIA has an SMDH of its own, not the .3dsx's (that one says the claim)"
  need ../DlpLink/tools/make_3dslink_cia.sh '-icon "$OUT/3dslink-cia.smdh" -banner "$OUT/banner.bnr"' "makerom takes the SMDH of the CIA and the banner"
  need ../DlpLink/tools/make_3dslink_cia.sh '"$BANNERTOOL" makebanner -i "$BANNER_DIR/banner.png" -a "$BANNER_DIR/jingle.wav" -o "$OUT/banner.bnr"' "the banner is made from the picture and the tune of ConfigApp/banner"
  need ../DlpLink/tools/make_3dslink_cia.sh '--expect-banner --expect-strings "$APP_CIA_NAME" "$APP_CIA_DESC" "$APP_CIA_AUTHOR"' "the CIA is checked for the banner and for the name, the description and the publisher"
  need ../DlpLink/tools/verify_cia.py 'if a.expect_banner:' "the verifier knows how to look for the banner"
  need ../DlpLink/tools/verify_cia.py 'bn[:4] == b"CBMD"' "the banner of the ExeFS is a CBMD"
  need ../DlpLink/tools/bannertool/build.sh 'OUT="${OUT:-$HERE/../../out}"' "bannertool is fetched at a fixed commit and built by build.sh"
  need ConfigApp/banner/make_banner.py 'CLAIM = "Connect, share, play together"' "the banner says the claim the user chose"
  [ "$(od -An -tu1 -j16 -N8 ConfigApp/banner/banner.png | tr -s ' ')" = " 0 0 1 0 0 0 0 128" ] || fail "ConfigApp/banner/banner.png must be 256x128 (the front face of the banner)"
  [ -s ConfigApp/banner/jingle.wav ] || fail "ConfigApp/banner/jingle.wav is missing (the tune of the banner)"
fi

# The documents say what the user decided (2026-10-21): 3DS Link is ONE CIA and the app makes the Download Play child; the switch; no gap between Titles Delivery and 3DS Link Delivery; the numbers of
# dist/ are the numbers of the files (the loops above). A document may name dist/dlp only to say it is gone.
need README.md $'**3DS Link è UN CIA solo, e il figlio per il Download Play lo fa l\'app' "the README says that 3DS Link is one CIA and the app makes the child"
need README.md 'Nessuno spazio fra *Titles Delivery* e *3DS Link Delivery*' "the README says there is no gap between the two entries"
need README.md 'Un interruttore di emergenza, *Settings > Download Play delivery*, spegne tutta la funzione' "the README names the emergency switch"
need README.md $'il modello del figlio di riferimento (fatto con l\'app vera nel RomFS) è lo stesso modello di quello fatto senza l\'app' "the README says how the cycle of the build is broken"
need dist/LEGGIMI.txt $'3DS Link e\' UN CIA SOLO, e il figlio per il Download Play lo fa l\'app' "the short guide says that 3DS Link is one CIA and the app makes the child"
need dist/LEGGIMI.txt $'dist/dlp/child.cia NON c\'e\' piu\' e non va copiato da nessuna parte' "the short guide says there is no child to copy"
need dist/LEGGIMI.txt $'Fra "Titles Delivery" e "3DS Link Delivery" nella pagina principale non c\'e\' spazio' "the short guide says there is no gap between the two entries"
need PROVE_HARDWARE.md $'**Il CIA unico di 3DS Link, il compositore del figlio, la guida per `boot9.bin` e l\'icona' "the plan of the tests says what the one CIA, the composer and the guide need"
need PROVE_HARDWARE.md '**3DS Link è UN CIA solo: il figlio Download Play non è un file di `dist/`**' "the plan of the tests says the child is not a file of dist/"
for r in O15 O16 O17 O18 O19 O20; do
  grep -q "^| $r |" CHECKLIST_DA_VERIFICARE.md || fail "CHECKLIST_DA_VERIFICARE.md has no row $r (the console rows of the one CIA: the package, the sources, the time, the switch, the guide, the size over the air)"
done
# The 1.0.1 round (2026-10-22, the first clean install of 3DSLink.cia through FBI): the plugin is inside the app and the app puts it on the card, the switch cannot be ON without it, About says
# what the app is and who made it, the credits, and the guide moves with the D-pad's up and down. The console rows of it, and the documents that say what the guide's keys are.
for r in P1 P2 P3 P4 P5 P6 P7 P8 P9 P10 P11 P12; do
  grep -q "^| $r |" CHECKLIST_DA_VERIFICARE.md || fail "CHECKLIST_DA_VERIFICARE.md has no row $r (the console rows of 1.0.1: the clean install through FBI, the plugin that comes back, the shut switch, Enable Titles, About and the credits, the guide's keys, the package made from the plugin inside the app, the install from Download Play)"
done
need CHECKLIST_DA_VERIFICARE.md 'croce su / giù' "the checklist says that the guide's keys are the D-pad's up and down"
need README.md 'croce su / giù' "the README says that the guide's keys are the D-pad's up and down"
need PROVE_HARDWARE.md 'croce su / giù' "the plan says that the guide's keys are the D-pad's up and down"
need dist/LEGGIMI.txt "croce su/giu'" "the short guide says that the guide's keys are the D-pad's up and down"
need README.md 'è in [`CREDITS.md`](CREDITS.md)' "the README points to the credits (the thanks to Azahar, FBI, 3dbrew and the others, with author, use and license)"
need dist/LEGGIMI.txt 'Created by Gionko' "the short guide says what About says: who made 3DS Link"
# a line that names dist/dlp must say that it is gone (the child is not a deliverable: the app makes it on the console)
! grep -n "dist/dlp" README.md PROVE_HARDWARE.md dist/LEGGIMI.txt CHECKLIST_DA_VERIFICARE.md | grep -v -E "non c'è più|NON c'e' piu'|non c'e' piu'|non ha più|non è un file|non esiste|non va copiato" | grep -q . || fail "a document speaks of dist/dlp without saying that it is gone: the child is not a deliverable any more (the app makes it on the console)"
# The neighbour DlpLink (the installer child, the build chain, its tests): the pins that read it, only where the tree has it (a copy of AzaharLink alone has none)
if [ -d ../DlpLink ]; then
  need ../DlpLink/docs/COMPOSITORE.md '* **Un CIA solo.** Si distribuisce `3DSLink.cia`.' "the design of the composer starts from the decision: one CIA"
  need ../DlpLink/docs/COMPOSITORE.md '### Come si rompe il ciclo nella costruzione' "the design says how the cycle of the build is broken"
  need ../DlpLink/docs/GUIDA-BOOT9.md $'## Dove compare (le tre posizioni decise dall\'utente)' "the design of the guide names the three places"
  need ../DlpLink/docs/GUIDA-BOOT9.md '## La guida non deve mentire (e Tests/policy_check.sh lo fissa)' "the design of the guide says that what it tells is pinned to the code"
  need ../DlpLink/docs/3DSLINK-DELIVERY.md $'**Fra Titles Delivery e 3DS Link Delivery NON c\'è nessuno spazio**' "the hand-over document says there is no gap between the entries (the user changed his mind)"
  need ../DlpLink/test/PROVA-INSTALLATORE.md $'**Non c\'è più un `child.cia` da copiare**' "the test of the installer does not tell to copy a child"
  need ../DlpLink/tests/run_checks.sh 'python3 "$PKG/tests/selftest_chain.py" "$OUT/child_model.bin" || rc=1' "the checks of the chain are tried against another installer, another version and a missing picture at every build (a check that cannot say NO checks nothing)"
  # ONE package: what is delivered is the CIA and the .3dsx, and the plugin inside them (the 1.0.1 round)
  need ../DlpLink/tools/build_all.sh '  cp AzaharLink/ConfigApp/3DSLink.3dsx AzaharLink/dist/3DSLink.3dsx' "the app is delivered as the .3dsx of the build"
  need ../DlpLink/tools/build_all.sh '  cp DlpLink/out/3dslink.cia AzaharLink/dist/3DSLink.cia' "the app is delivered as the CIA of the build"
  ! grep -q 'cp AzaharLink/AzaharLink.3gx AzaharLink/dist/AzaharLink.3gx' ../DlpLink/tools/build_all.sh || fail "DlpLink/tools/build_all.sh copies the plugin into dist/ again (ONE package: the plugin is inside the app, and not a file of dist/)"
  need ../DlpLink/tools/build_all.sh 'old_dest="AzaharLink/dist/old/AzaharLink-$old_major.$old_minor.$old_rev.3gx"' "a plugin left in dist/ by an older build is put in dist/old under its version, not thrown away (a console may have that file)"
  need ../DlpLink/docs/COMPOSITORE.md 'è la **risorsa** che l'"'"'app porta' "the design of the composer says that the plugin is the resource of the app (no file of the card)"
  need ../DlpLink/test/PROVA-INSTALLATORE.md 'croce su / giù' "the test of the installer says that the guide's keys are the D-pad's up and down"
fi

# The log header says when the CODE last changed (P10-0001): kBuildStamp comes from build_id.cpp, which the Makefile compiles again after every other
# object; plugin.cpp's own __DATE__ / __TIME__ stayed as old as plugin.cpp (the first C-stick build said "1.0.3 (built Oct 23)"). Tests/build_id_test.sh builds
# twice (Docker) and requires the stamp to move with a change to main.cpp alone.
need Includes/ctr/plugin.hpp "extern const char kBuildStamp[];" "the build stamp of the log header is declared in one place"
need Sources/ctr/build_id.cpp 'extern const char kBuildStamp[] = __DATE__ " " __TIME__;' "the stamp is made by a file the Makefile compiles after every other object"
need_line Makefile 'build_id.o : $(filter-out build_id.o,$(OFILES))' "build_id.cpp is compiled again whenever any other object was (the whole line: a commented-out copy does not count)"
need Sources/ctr/plugin.cpp 'kVersion, kBuildStamp, stamp);' "the log header prints that stamp, not plugin.cpp's own date"
! grep -q '__DATE__' Sources/ctr/plugin.cpp || fail "Sources/ctr/plugin.cpp uses __DATE__ again: the header's build date would be as old as plugin.cpp"
need Sources/ctr/plugin.cpp 'Log("hid: the plugin %s ir:rst' "the log says whether the plugin holds ir:rst (the C-stick fix of 1.0.4) so that a log tells the two builds apart"
need Sources/main.cpp 'extern "C" bool hidShouldUseIrrst(void) {' "the plugin overrides libctru's hidShouldUseIrrst (no ir:rst: the game keeps its C-stick)"
need Tests/build_id_test.sh "build id test: OK" "the build stamp test exists"

# The plugin that runs in a game against the plugin of 3DS Link's folder (2026-10-28): once, 25 s after the start, on the writer thread; it says "open 3DS Link" when the folder has a later
# version or another build of the same one, and the log says what it found. The first join of a launch also says the version on the screen.
need Includes/azl/plugin_compare.hpp "inline MasterPlugin RelateMaster(u32 own, u32 master, bool bytes_equal) {" "the decision is a pure function (tested on a PC)"
need Sources/ctr/plugin.cpp 'constexpr const char* kMasterPluginPath = "/3ds/3DSLink/AzaharLink.3gx";' "the plugin of 3DS Link's folder is the one the app puts there"
need Sources/ctr/plugin.cpp "constexpr u64 kMasterCheckAfterMs = 25000;" "the look is made a while after the start (the game loads from the same card before)"
need Sources/ctr/plugin.cpp "const MasterPlugin relation = LookAtMasterPlugin(game_id_, &detail);" "the writer thread makes the look, once"
need Sources/ctr/plugin.cpp "if (MasterPluginWarns(relation)) {" "only a later version or another build of the same one warns"
need Sources/ctr/plugin.cpp 'PostNotification(std::string("AzaharLink ") + kVersion + " (built " + kBuildStamp + ")");' "the first join of a launch says the version of the plugin on the screen"
need Tests/run.sh "Tests/plugin_compare_test.cpp" "the PC test of the comparison is built by the suite"
need Sources/ctr/plugin.cpp "constexpr u64 kAppTitleId = 0x000400000F83D100ull;" "the probe asks about 3DS Link's title id"
need ../DlpLink/tools/common.sh 'UNIQUE_ID="${UNIQUE_ID:-0x0F83D1}"' "... which is the app's unique id in the build chain (000400000F83D100): the two change together"
need Sources/ctr/plugin.cpp "ProbeInstalledApp(kVersion); // (log only)" "the probe of the installed 3DS Link only logs, and only with the log on"
need_line Tests/run.sh '  "$OUT/plugin_compare_test"' "... and run (the whole line: the build line has the same words)"
# A player that stays in the room but tells it that it plays nothing (Azahar closed, no goodbye on the air) is counted out of the game's session like one that left
# (2026-10-28, Kirby Battle Royale: the 3DS host waited 30 s for a client that was gone, and the game froze for that long).
need Sources/ctr/plugin.cpp "if (RoomMemberPlays(member.game_name, member.game_id)) {" "the engine is given only the members that play: one that stays in the room but plays nothing is counted out of the session"
need Includes/azl/room_members.hpp "return !game_name.empty() || game_id != 0;" "a member plays unless it has neither a title nor an id (an id alone, or a name alone, is a game: a lobby of two regions stays whole)"
need Sources/ctr/plugin.cpp "RoomMemberStoppedLines(room_members_, now)" "the log says which player stopped playing"
need Tests/test_main.cpp "    TestRoomMemberStopsPlaying();" "the chain from the list of members to the engine is tested"
# The capture goes to the card in few, large passes (2026-10-28: the writer spent 63-77% of its time in the SD card with a pass every 250 ms, and the music of Kirby Battle
# Royale stuttered at debug=1): a pass is due every 2 s, or when the ring holds 160 KB, or at once on request; the writer still wakes every 250 ms for the main log and the statistics.
need Includes/azl/log_files.hpp "u32 capture_every_ms = 2000;" "the capture is written every 2 s (not four times a second)"
need Includes/azl/log_files.hpp "u32 capture_ring_high = 160 * 1024;" "... or at once when the ring holds 160 KB"
need Sources/azl/log_files.cpp "return early || last == 0 || ring_used >= limits_.capture_ring_high || now_ms - last >= limits_.capture_every_ms;" "a pass is due on request, the first time, when the ring is high, or when the interval is over"
need Sources/azl/log_files.cpp "    NoteCapturePass(platform::NowMs());" "a pass of the capture notes when it began"
need Sources/ctr/plugin.cpp "if (backlog || log_files_->CaptureDue(platform::NowMs(), DebugRingUsedBytes(), capture_early)) {" "the writer only passes when the capture is due (or the last pass was cut short)"
need Tests/log_files_test.cpp "    TestCaptureCadence();" "the test of the capture's cadence runs"
# Kirby Triple Deluxe and Kirby Planet Robobot died at start (a data abort inside CTRPluginFramework's startup search, before the plugin ran a line): Process::GetTextSize() goes through a
# guard that cuts the size of THAT search to what is left of the code (text_search_guard.cpp; the PC test and the check of the call in the built .elf), and the log says when it did.
need Makefile '-Wl,--wrap=_ZN18CTRPluginFramework7Process11GetTextSizeEv' "the framework's Process::GetTextSize() goes through the guard of the startup search"
need Sources/ctr/text_search_guard.cpp 'constexpr u32 kMovR7R0 = 0xe1a07000;' "the guard knows the call in KeepThreadMain (mov r7, r0 ...)"
need Sources/ctr/text_search_guard.cpp 'constexpr u32 kAddR6R6Imm4 = 0xe2866004;' "... (... add r6, r6, #4)"
need Sources/ctr/text_search_guard.cpp 'if (return_address[0] != kMovR7R0 || return_address[1] != kAddR6R6Imm4) {' "every other caller of GetTextSize() gets the real value"
need_line Sources/ctr/text_search_guard_entry.s '    b    azl_text_size_guard' "the stub of the guard hands over to the C++ function by a plain branch (lr is left alone)"
need Sources/ctr/hook.cpp 'if (TextSearchTrims() != 0) {' "the log says when the startup search was cut to the game's code"
# A game whose GSP wrapper has its svc behind a stub (LEGO Batman 2) makes the framework abort at start unless the plugin writes the svc in place of the bl BEFORE the framework searches (GSP::Initialize runs after PatchProcess); the framework's own sites are found after that
need Sources/ctr/hook.cpp 'FindStubbedGspWrappers(ReadGameWord, nullptr, first, first + text_size, stubbed, 4);' "the GSP wrapper behind a stub is looked for in the game's .text"
need Sources/ctr/hook.cpp 'CTRPluginFramework::Process::Patch(stubbed[i], kSvcSendSyncRequest)' "the bl of the GSP wrapper becomes the svc itself"
[ "$(grep -c 'FindStubbedGspWrappers(' Sources/ctr/hook.cpp)" = "1" ] || fail "Sources/ctr/hook.cpp: the GSP wrapper behind a stub is rewritten in ONE place (RewriteStubbedGspWrappers), not again in InstallSvcHooks"
need Sources/ctr/hook.cpp 'u32 RewriteStubbedGspWrappers() {' "the rewrite is a function of its own, which the plugin calls whether it is on or off"
need Sources/ctr/hook.cpp 'stats.gsp_stub_wrappers = g_gsp_stub_wrappers;' "InstallSvcHooks reports what the rewrite did"
# ... called from EarlyInit, outside `if (active_)` (LEGO Batman 2 aborted at start with the main switch off, the plugin still loaded by Luma3DS), BEFORE the hooks look for the framework's sites
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c '^    RewriteStubbedGspWrappers();$')" = "1" ] || fail "Sources/ctr/plugin.cpp: RewriteStubbedGspWrappers() is called once, at the level of EarlyInit itself and not inside 'if (active_)': the framework aborts at start with the switch off otherwise"
need_order Sources/ctr/plugin.cpp '    RewriteStubbedGspWrappers();' 'hook_stats_ = InstallSvcHooks(hle_.get(), !swap_console_ && luma_alias_);' "the GSP wrapper is rewritten BEFORE the hooks look for the framework's sites"
need_order Sources/ctr/plugin.cpp '    RewriteStubbedGspWrappers();' "// The game's APT session tells when the console goes to sleep; we only look" "... and before the block that only runs when the plugin is on"
need_order Sources/ctr/hook.cpp 'u32 RewriteStubbedGspWrappers() {' 'HookStats InstallSvcHooks(UdsHle* hle, bool reserve_exit_tramp) {' "the rewrite is defined before the install, which reports it"
need Includes/azl/gsp_wrapper.hpp 'stub > end - 8' "the end of the stub is tested without adding to the target (a bl whose target wraps round the top of the address space passed 'stub + 8 > end')"
! grep -q 'stub + 8 > end' Includes/azl/gsp_wrapper.hpp || fail "Includes/azl/gsp_wrapper.hpp tests 'stub + 8 > end' again: it wraps for a target of 0xFFFFFFFC"
need Tests/gsp_wrapper_test.cpp 'for (const u32 target : {0xFFFFFFFCu, 0xFFFFFFF8u, 0xFFFFFFF4u, 0xFFFFFFFFu, 0x00000000u, 0x00000004u}) {' "the test tries a bl whose target wraps round the top of the address space"
[ "$(grep -c 'CHECK(g_wild_reads == before);' Tests/gsp_wrapper_test.cpp)" = "2" ] || fail "Tests/gsp_wrapper_test.cpp: both the wrap-round case and the last-stub case hold that the search never reads outside the image"
need Tests/run.sh 'Tests/text_search_guard_test.cpp' "the PC test of the guard is built by the suite"
need_line Tests/run.sh '  "$OUT/text_search_guard_test"' "... and run (the whole line: the build line has the same words)"
need Tests/text_search_guard_elf_check.sh 'text search guard: OK' "the check of the call in the built .elf exists"
# Kirby Battle Royale asks dlp:FKCL (Initialize / InitializeWithName, Finalize 1 ms later) only to know whether the wireless is on, and then plays over nwm::UDS: the real module says
# "wireless off" because the console stays on its access point for the tunnel. For the titles in the list the plugin answers that probe itself (azl/dlp_fkcl_hle.hpp); for every other
# title dlp:FKCL is the real service.
need Sources/ctr/plugin.cpp 'hle_->SetDlpProbeFake(AsksDlpOnlyForTheWireless(game_id_));' "the fake dlp:FKCL is switched on per title, from the list"
need Sources/ctr/plugin.cpp '0x00040000001C2100ull' "Kirby Battle Royale is in the list of the titles whose dlp:FKCL is answered by the plugin"
# Phase 3: the table of games is {title id, mode}. Kirby Triple Deluxe and Planet Robobot are NOT in the probe's list (their wireless mode needs the Download Play client -
# GetChannels, scan, join - and the probe answers 'invalid state' to it, so the game shows an error screen, 0xE0A0AC01, seen 2026-10-28): they are Lobby games, whose
# Download Play the room carries (azl/dlp_hub.hpp). Every title id of these games is on a line with its mode, and Battle Royale keeps the probe.
need Sources/ctr/plugin.cpp '{0x000400000010C000ull, DlpGameMode::Lobby}' "Kirby Triple Deluxe is a Lobby game: the room carries its Download Play lobby"
need Sources/ctr/plugin.cpp '{0x000400000010BF00ull, DlpGameMode::Lobby}' "Kirby Triple Deluxe, the other region's id, is a Lobby game"
need Sources/ctr/plugin.cpp '{0x0004000000183600ull, DlpGameMode::Lobby}' "Kirby Planet Robobot is a Lobby game"
# Mario Tennis Open: its local multiplayer scene starts the Download Play CLIENT at once (Initialize with no name, GetMyStatus every 5 ms, GetChannels, StartScan, StartFakeSession) and the host side runs
# the SERVER (StartAccepting, GetConnectingClients): it is NOT a game of the probe (Battle Royale asks Initialize/Finalize only), it is a Lobby game, in all the regions whose id is known.
need Sources/ctr/plugin.cpp '{0x000400000007C800ull, DlpGameMode::Lobby, true}' "Mario Tennis Open, Europe is a Lobby game: the room carries its Download Play lobby"
need Sources/ctr/plugin.cpp '{0x000400000007C700ull, DlpGameMode::Lobby, true}' "Mario Tennis Open, USA is a Lobby game: the room carries its Download Play lobby"
need Sources/ctr/plugin.cpp '{0x0004000000064D00ull, DlpGameMode::Lobby, true}' "Mario Tennis Open, Japan is a Lobby game: the room carries its Download Play lobby"
need Sources/ctr/plugin.cpp '{0x00040000000B9100ull, DlpGameMode::Lobby, true}' "Mario Tennis Open, Taiwan is a Lobby game: the room carries its Download Play lobby"
need Sources/ctr/plugin.cpp '{0x00040000001D1A00ull, DlpGameMode::RealDlp}' "Luigi's Mansion 2018 (Europe, seen: dlp:SRVR / dlp:FKCL InitializeWithName) is a RealDlp game (1.1.3: the real Download Play on the wire)"
need Sources/ctr/plugin.cpp '{0x00040000001C2100ull, DlpGameMode::ProbeOnly}' "Kirby Battle Royale answers the wireless check only (the probe)"
need Sources/ctr/plugin.cpp '{0x00040000001C2000ull, DlpGameMode::ProbeOnly}' "Kirby Battle Royale, the other region's id, answers the wireless check only"
hit=$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -n -i -E '0x000400000010C000|0x000400000010BF00|0x0004000000183600|0x00040000001C2100|0x00040000001C2000|0x000400000007C800|0x000400000007C700|0x0004000000064D00|0x00040000000B9100' | grep -v 'DlpGameMode::' | head -3)
[ -z "$hit" ] || fail "a title id of Triple Deluxe, Planet Robobot, Mario Tennis Open or Battle Royale is in Sources/ctr/plugin.cpp outside the table of games (a mode must stay next to it): $hit"
need Sources/ctr/plugin.cpp 'constexpr DlpGame kDlpGames[] = {' "the table of games of the Download Play wiring"
need Sources/ctr/plugin.cpp 'return DlpModeOf(title_id) == DlpGameMode::ProbeOnly;' "the probe is for the ProbeOnly games and nobody else"
need Sources/ctr/plugin.cpp 'if (DlpModeOf(game_id_) == DlpGameMode::Lobby && active_) {' "the hub exists only for a Lobby game, and only when the plugin is active"
need Sources/ctr/plugin.cpp 'new (std::nothrow) DlpHub(engine, kernel, config, resolver)' "the hub is the one allocation of the lobby and a refusal leaves the real services"
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c -F 'hle_->SetDlpHub(dlp_hub_.get());')" = "2" ] || fail "Sources/ctr/plugin.cpp: the HLE is given the hub in both branches of EarlyInit, Lobby and RealDlp (the line twice)"
need_order Sources/ctr/plugin.cpp 'hle_->SetDlpHub(dlp_hub_.get());' 'hook_stats_ = InstallSvcHooks(hle_.get(), !swap_console_ && luma_alias_);' "the hub is set before the hooks are installed (a game's first call finds it)"
need_after Sources/ctr/plugin.cpp 'engine_->Tick(now_ms);' 'dlp_hub_->Tick();' "the hub serves after the engine's own Tick, on the network thread"
need_order Sources/ctr/plugin.cpp 'dlp_hub_->Stop();' 'room_->StopWithin(kNetStopTimeoutMs)' "the hub is stopped before the room: no request waits for a network thread that is going away"
need Sources/ctr/plugin.cpp 'dlp_hub_->SetPlayerName(name);' "the console's profile name is the name the lobby shows (the games send Initialize with no name)"
# Mario Tennis Open matches the players of the Download Play lobby with the nodes of its own network by the friend code seed (2026-11-01, found with the debugger on a stalled
# game): the lobby announces the console's OWN seed (config block 0x00090000, 8 bytes: what the game gives nwm::UDS), for these four titles only.
need Sources/ctr/plugin.cpp 'bool LobbySeedIsConsoles(u64 title_id) {' "the table says which games get the console's own friend code seed in the lobby"
need Sources/ctr/plugin.cpp 'return game.console_seed;' "... and it is the table's flag"
need Sources/ctr/plugin.cpp 'CFGU_GetConfigInfoBlk2(sizeof(console_seed), 0x00090000, &console_seed)' "the console's friend code seed is config block 0x00090000, 8 bytes (what the game reads itself)"
need Sources/ctr/plugin.cpp 'dlp_hub_->SetFriendSeed(console_seed);' "the lobby announces that seed (not the hash of the MAC) for the games with the flag"
need Includes/azl/dlp_lobby.hpp 'id_.seed = seed;' "the lobby's seed can be set (SetFriendSeed)"
need Sources/azl/dlp_hub.cpp 'lobby_.SetFriendSeed(seed);' "the hub gives the seed to the lobby under its lock"
need Tests/dlp_hub_test.cpp 'a.hub.SetFriendSeed(kHostSeed);' "F6 gives the hub a friend code seed"
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | grep -c -F 'static const char kDefaultName[] = "3DS Link";')" = "2" ] || fail "Sources/ctr/plugin.cpp: the name before the profile name is known, in MakeDlpHub and in MakeRealDlpHub (the line twice)"
need Sources/ctr/plugin.cpp 'u64 hash = 0xCBF29CE484222325ull;' "the friend code seed is FNV-1a-64 of the console's MAC (unique per console)"
need Sources/ctr/plugin.cpp 'hash *= 0x100000001B3ull;' "... FNV-1a-64's prime"
need Sources/ctr/plugin.cpp 'constexpr u32 kLow = 0x00100000;' "the resolver of the game's buffers refuses an address below the code"
need Sources/ctr/plugin.cpp 'constexpr u32 kHigh = 0x40000000;' "... and one at or past the end of the application's range"
need Sources/ctr/plugin.cpp 'if (address < kLow || address >= kHigh || size > kHigh - address) {' "... or a range that wraps"
need Sources/ctr/uds_hle.cpp '} else if ((dlp_hub_ != nullptr || dlp_probe_fake_) && cmd[3] == 8 && std::memcmp(&cmd[1], "dlp:FKCL", 8) == 0) {' "dlp:FKCL is intercepted only for a Lobby game (the hub) or when the title asked for the probe (otherwise the real service answers)"
need Sources/ctr/uds_hle.cpp '} else if (dlp_hub_ != nullptr && cmd[3] == 8 && std::memcmp(&cmd[1], "dlp:SRVR", 8) == 0) {' "dlp:SRVR is intercepted only for a Lobby game (the probe leaves it to the real service, as Battle Royale always had)"
# The front-end of the hub's services (DESIGN-FASE3 sez. 4.1).
need Includes/ctr/uds_hle.hpp 'enum Kind : u8 { kKindUds = 1, kKindNdm = 2, kKindDlp = 3, kKindDlpSrvr = 4 };' "the session kinds: nwm::UDS, ndm:u, dlp:FKCL and dlp:SRVR"
need Sources/ctr/uds_hle.cpp 'constexpr u32 kDlpJoinCapMs = 8000;' "the join wait of StartFakeSession gives up after 8 s (the lobby's own timeout is 3 s: the network thread is gone if this one runs out)"
need Sources/ctr/uds_hle.cpp 'constexpr s64 kDlpJoinSleepNs = 10 * 1000 * 1000;' "the join wait polls every 10 ms"
need Sources/ctr/uds_hle.cpp 'while (!hub.PollPending(cmd)) {' "the join wait asks the hub, outside every lock"
need Sources/ctr/uds_hle.cpp 'cmd[0] = (DlpService::kFkStartFakeSession << 16) | 0x40;' "the join that outlives its cap is answered as StartFakeSession ..."
need Sources/ctr/uds_hle.cpp 'cmd[1] = kNotFound;' "... with 'server not found'"
need Sources/ctr/uds_hle.cpp '} else if (kind >= kKindDlp && dlp_hub_ != nullptr && (id == 0x01 || id == (kind == kKindDlp ? 0x11u : 0x0Fu))) {' "a Download Play session that called Initialize (FKCL 0x01 / 0x11, SRVR 0x01 / 0x0F, with a hub only: the probe's session ends nothing) is marked, so that closing it without Finalize ends its role"
need Sources/ctr/uds_hle.cpp 'if (g_debug_level > 0 && (kind < kKindDlp || dlp_hub_ == nullptr)) {' "the hub's sessions skip the debug capture's copy of the request (a variable array under the hub's chain: 536 bytes at debug 1 against 520)"
need_after Sources/ctr/uds_hle.cpp 'if (kind < kKindDlp || dlp_hub_ == nullptr) {' 'AccountCall(id, cmd, started);' "the hub's sessions are not counted in the latency statistics and not throttled (they wait for the network thread; the end of a list is a normal result)"
need Sources/ctr/uds_hle.cpp 'if (dlp_hub_ != nullptr && id != 0x01 && id != 0x1B && id >= 0x02 && id <= kUdsLastCommand && dlp_hub_->EngineHeld()) {' "while the lobby holds the engine every nwm::UDS command but Initialize answers 'wrong state' (the lobby's own state does not leak to the game)"
need Sources/ctr/uds_hle.cpp 'Reply(cmd, id, id == 0x03 ? res::kSuccess : res::kWrongState);' "... and Shutdown answers success WITHOUT touching the engine (it is the lobby's network)"
need Sources/ctr/uds_hle.cpp 'constexpr u32 kUdsLastCommand = 0x23;' "the last command of nwm::UDS the gate knows (an id past it is the dispatcher's own answer)"
need_order Sources/ctr/uds_hle.cpp 'dlp_hub_->OnGameUdsStarting();' 'engine_->Initialize(node, cmd[1], version, &event);' "the lobby gives the engine back BEFORE the game's nwm::UDS Initialize reaches it (otherwise the Initialize changes nothing and the game inherits the lobby's node, name and seed)"
need Sources/ctr/uds_hle.cpp 'if (dlp_hub_ == nullptr || !dlp_hub_->EngineHeld()) {' "closing an initialised nwm::UDS session does not shut the engine down while the lobby holds it"
need Sources/ctr/uds_hle.cpp 'dlp_hub_->OnSessionClosed(kind == kKindDlpSrvr ? DlpKind::Srvr : DlpKind::Fkcl);' "the end of a Download Play session that had called Initialize is the hub's OnSessionClosed of its own service"
need Sources/ctr/uds_hle.cpp 'RunDlpHub(*dlp_hub_, kind == kKindDlpSrvr, id, cmd);' "a request of a fake dlp session goes to the hub with its service"
need Sources/ctr/uds_hle.cpp 'dlp_.Dispatch(cmd);' "the probe (Battle Royale) is DlpFkclHle as it always was"
need Sources/ctr/uds_hle.cpp 'if (hub.failed_fast() + hub.cancelled() != refusals && cmd[1] == DlpHub::kResultWirelessOff) {' "the 'answered wireless off' line is for the commands that were really answered so (a Finalize that could not be served is a success)"
need_after Sources/ctr/uds_hle.cpp 'NoteDlpTrouble(hub, srvr, id);' 'WaitForJoin(hub, cmd);' "the trouble of a command is decided before the join wait (the wait asks the hub to end the role, which a dead network refuses: not the answer of this command)"
need Sources/ctr/uds_hle.cpp 'constexpr int kPlainSeen = 48;' "the first-call table of nwm::UDS, ndm:u and the probe keeps its 48 pairs (a game without a hub logs what it always did) ..."
need Sources/ctr/uds_hle.cpp 'constexpr int kMaxSeen = kPlainSeen + 32;' "... and the hub's two services have 32 pairs of their own"
# The hub itself (DESIGN-FASE3 sez. 1 and 2).
game_thread_file Sources/azl/dlp_hub.cpp 1
need Includes/azl/dlp_hub.hpp 'static constexpr u32 kQueuedMs = 2000;' "a request nobody takes is cancelled after 2 s"
need Includes/azl/dlp_hub.hpp 'static constexpr u32 kAliveMs = 1000;' "the network thread is alive for 1 s after its last Tick"
need Includes/azl/dlp_hub.hpp 'static constexpr u32 kResultWirelessOff = 0xC940AC04;' "what a request that cannot be served is answered: the DLP module's 'wireless off'"
need Includes/azl/dlp_hub.hpp 'static constexpr u32 kSliceMs = 100;' "the wait of a game thread is cut in slices of 100 ms"
need Sources/azl/dlp_hub.cpp 'lobby_.SetExternalPoll(true);' "the lobby's network work is the network thread's (Poll does nothing on a game thread)"
need Sources/azl/dlp_hub.cpp 'if (!platform::TryLockBriefly(dlp_mutex_)) {' "the network thread never waits for dlp_mutex_ (a LightLock has no priority inheritance)"
need Sources/azl/dlp_hub.cpp 'lobby_.SetOnNet(true);' "the network thread says it is the network thread before it pumps or serves"
need_order Sources/azl/dlp_hub.cpp 'lobby_.SetOnNet(true);' 'lobby_.SetOnNet(false);' "... and takes it back before it lets go of the lock"
need Sources/azl/dlp_hub.cpp 'if (state_.compare_exchange_strong(expected, kFree, std::memory_order_acq_rel)) {' "a request not taken in time is taken back with the CAS that the network thread's Running also needs: it either ran or never will"
need_after Sources/azl/dlp_hub.cpp 'Execute();' 'state_.store(kDone, std::memory_order_release);' "the answer is published after the work"
need_after Sources/azl/dlp_hub.cpp 'Refresh();  // (BEFORE the answer is published' 'state_.store(kDone, std::memory_order_release);' "the hints (EngineHeld, active) are refreshed BEFORE the answer is published: the game may ask the moment it has it"
need Sources/azl/dlp_hub.cpp 'state_.store(kQueued, std::memory_order_seq_cst);' "a request is queued with a sequentially consistent store, paired with Stop()'s (a request posted just after Stop() fails at once)"
need Sources/azl/dlp_hub.cpp 'stopped_.store(true, std::memory_order_seq_cst);' "Stop() sets its flag with the matching sequentially consistent store"
need Sources/azl/dlp_hub.cpp 'if (post_mutex_.try_lock()) {' "OnGameUdsStarting does not return early while a request is in flight (post_mutex_ is its requester's)"
# The waits a game thread makes inside the engine and the hub end when the swap of the plugin's block closes the svc hook's gate (KernelOps::GateClosing,
# ctr/gate_flag.hpp): the swap waits 1.5 s for the threads counted in the gate, and after it the code they sleep in is gone (the class of dumps 03/05/06).
# ConnectToNetwork waits 5 s, a scan up to 10 s, a posted dlp request 2 s in the queue and the slot without a limit.
need Includes/azl/uds_engine.hpp 'static constexpr u32 kGateSliceMs = 10;' "a game thread in the engine looks at the gate at least every 10 ms (under the 20 ms a wait without the guard may take)"
need Includes/azl/uds_engine.hpp 'virtual bool GateClosing() {' "the engine and the hub ask the gate through the kernel they are given (the PC builds them without the plugin)"
need_after Includes/azl/uds_engine.hpp 'asked += slice;' 'if (GateClosing()) {' "the guarded wait ends when the gate closes"
need Includes/ctr/kernel_ops.hpp 'return ctr::GateClosing();' "on the console the gate is the flag of the svc hook's gate"
need Sources/azl/uds_engine.cpp 'const bool answered = kernel_->WaitEventGuarded(connect_event_, kConnectTimeoutNs);' "ConnectToNetwork's 5 s wait is the guarded one"
need Sources/azl/uds_engine.cpp 'constexpr u64 kScanSliceMs = 20;' "the scan looks at the gate every 20 ms at the most"
need_after Sources/azl/uds_engine.cpp 'if (scan.cancel != 0 && kernel_->WaitEvent(scan.cancel, 0)) {' 'return;' "the game's cancel event ends the scan ..."
need Sources/azl/uds_engine.cpp '        if (kernel_->GateClosing()) {' "... and so does the closing of the gate"
need Sources/azl/dlp_hub.cpp 'if (event_ == 0 || !NetAlive() || kernel_->GateClosing()) {' "no dlp request is posted while the gate closes (a close of a session is then left to the next Tick)"
need_after Sources/azl/dlp_hub.cpp 'while (!post_mutex_.try_lock()) {' 'if (kernel_->GateClosing()) {' "a requester behind another one does not wait for the slot while the gate closes"
need Sources/azl/dlp_hub.cpp 'if (state == kQueued && (platform::NowMs() >= deadline || kernel_->GateClosing())) {' "a queued request is taken back when the gate closes, as at kQueuedMs"
need_after Sources/azl/dlp_hub.cpp 'void DlpHub::WakeForSwap() {' 'if (event_ != 0 && state_.load(std::memory_order_acquire) == kQueued) {' "the swap can wake a queued requester at once (WakeForSwap) ..."
need_after Sources/azl/dlp_hub.cpp 'if (event_ != 0 && state_.load(std::memory_order_acquire) == kQueued) {' 'kernel_->SignalEvent(event_);' "... with the event it waits on"
need Sources/azl/dlp_hub.cpp 'return id == DlpService::kFkFinalize || id == DlpService::kFkStopScan || id == DlpService::kFkStopSession;' "the commands that succeed in every state (FKCL): Triple Deluxe and Planet Robobot throw a fatal error at a negative Finalize"
need Sources/azl/dlp_hub.cpp 'return id == DlpService::kSvFinalize || id == DlpService::kSvEndAccepting;' "... (SRVR)"
need Sources/azl/dlp_hub.cpp 'return id == DlpService::kFkFinalize || id == DlpService::kFkStopSession;' "the ones that end the FKCL role"
need Sources/azl/dlp_hub.cpp 'return id == DlpService::kSvFinalize;' "the one that ends the SRVR role"
need Sources/azl/dlp_hub.cpp 'closing_.fetch_or(kind == DlpKind::Fkcl ? kClosingFkcl : kClosingSrvr, std::memory_order_release);' "a session closed while no network thread serves is ended by the next Tick (nothing of the lobby's network runs on the closing thread)"
need Sources/azl/dlp_hub.cpp 'if (EndsRole(kind, id)) {' "an unserved Finalize / StopSession leaves the role it ends to the next Tick"
need Sources/azl/dlp_hub.cpp 'if (!Post(kOpClosed, kind, nullptr, nullptr, &unused)) {' "a session closed while no network thread takes the request is remembered, not lost"
need_order Sources/azl/dlp_hub.cpp 'RunDeferred();  // (the roles whose sessions' 'Execute();' "the deferred ends run before the request that follows them (a new session is not ended by an old close)"
need Sources/azl/dlp_hub.cpp 'Stamp();  // (a Tick that took long' "the liveness stamp is also taken at the end of Tick"
need Sources/azl/dlp_hub.cpp 'last_tick_ms_.store(now != 0 ? now : 1u, std::memory_order_relaxed);' "the stamp is never 0 (no Tick yet) and never ahead of the clock (NetAlive's age is unsigned)"
need Includes/azl/dlp_lobby.hpp 'if (use_ == kUseNone || (flags2_ & (kF2External | kF2OnNet)) == kF2External || !Due(force)) {' "Poll does nothing on a thread that is not the network thread's when the lobby is in the external mode"
need Includes/azl/dlp_lobby.hpp 'flags2_ = static_cast<u8>(flags2_ & ~kF2FinalRoster);' "ClientReset clears the searcher's bit only: the mode bits are the hub's (they were cleared once, and the lobby polled on game threads again)"
need Includes/azl/dlp_lobby.hpp 'static constexpr u8 kF2External = 2;' "the external-poll bit"
need Includes/azl/dlp_lobby.hpp 'static constexpr u8 kF2OnNet = 4;' "the on-the-network-thread bit"
need Tests/run.sh 'python3 Tests/dlp_chain_report.py --selftest' "the suite runs the self-test of the chain tool (it needs no Docker)"
need Tests/run.sh 'python3 Tests/plugin_dlp_extract.py Sources/ctr/plugin.cpp "$OUT/plugin_dlp_slice.inc"' "the test of the plugin's Download Play wiring compiles the text of plugin.cpp (it cannot be built on the PC)"
need Tests/run.sh '$HUB_WRAP -o "$OUT/dlp_hub_test"' "the hub test is linked with the thread spy's wraps (without them H2 fails on 'every wrapper fired')"
need Tests/run.sh '-fsanitize=thread Tests/dlp_hub_test.cpp' "the hub test is also built with ThreadSanitizer (some synchronisation mutants are visible only there)"
# The scenarios of the hub test (its table) and of the 32-bit front-end test (main), and the harness hooks that make some of them real.
for s in H1 H2a H2b H3 H3b H4 H5 H6 H7 H8 H9 X1 X2 X3 X4 X5 X6 X7 X8 X9 X10 X11 X12 F1a F1b F1c F1d F2 F3 F4 F4b F5; do
  need Tests/dlp_hub_test.cpp "{\"$s\", $s, " "the scenario $s of the hub test is in its table"
done
need Tests/dlp_hub_test.cpp 'a.kernel.SetOnClear([hub, net] {' "F2 makes the Stop()/Post race real: Stop() runs inside Post's ClearEvent"
need Tests/dlp_hub_test.cpp 'g_exec_slow_ms.store(600);' "F3 stalls the request so that it is RUNNING when OnGameUdsStarting arrives"
need Tests/dlp_hub_test.cpp 'a.hub.SetPlayerName(zoe);' "F5 gives the hub a player name"
need Tests/dlp_adv_lobby_test.cpp '    T_default_name();' "DlpLobby::SetDefaultName is the name of an Initialize that gives none"
need Tests/dlp_adv_lobby_test.cpp '    T_external_poll();' "the lobby's external-poll mode is tested"
need Tests/dlp_adv_lobby_test.cpp '    T_external_go();' "... also after the GO of a session"
for s in L1 L2 L3 L4 L5 L6 L7 L8 L9 L10 L11 L12 L13 L14; do
  need Tests/ipc_test/dlp_lobby_hle_test.cpp "    $s();" "the scenario $s of the 32-bit front-end test of the hub runs in main()"
done
need Tests/ipc_test/dlp_lobby_hle_test.cpp '    L12One(true, true);' "L12: a UDS session found dead while the lobby holds the engine shuts nothing down"
need Tests/ipc_test/dlp_lobby_hle_test.cpp '    L12One(false, false);' "L12's control: with no lobby the same close still shuts the service down"
need Tests/ipc_test/dlp_lobby_hle_test.cpp 'TCHECK(RunChild("--l13", nullptr));' "L13 runs in a fresh process (the trouble lines are the first eight events of one)"
need Tests/ipc_test/dlp_lobby_hle_test.cpp 'TCHECK(RunChild("--l14", "hub-first"));' "L14 runs in both orders, each in a fresh process"
need Tests/ipc_test/dlp_lobby_hle_test.cpp 'TCHECK_EQ(plain_logged, 48);' "L14: the plain first-call table is 48 pairs, written out in the test"
need Tests/ipc_test/dlp_lobby_hle_test.cpp 'TCHECK_EQ(hub_logged, 32);' "L14: the hub's two services have 32"
need Sources/ctr/uds_hle.cpp 'a DELIBERATE choice, not a copy of the real' "the gate's wrong state for every id is a deliberate choice (the real service before Initialize answers 0x02, 0x0B, 0x13, 0x23 differently)"
need Sources/azl/dlp_hub.cpp 'return AnswerUnserved(kind, cmd);' "the answer to an unserved request has a frame of its own (Serve keeps the frame the tightest chain of the 512-byte thread pays)"
need Sources/azl/dlp_hub.cpp '__attribute__((noinline)) bool DlpHub::RequestInFlight() {' "the in-flight test has a frame of its own too"
need_order Sources/azl/dlp_hub.cpp 'Stamp();    // (a request that stalled the engine' 'state_.store(kDone, std::memory_order_release);' "the stamp is taken before the answer is published: the requester it releases finds the thread alive"
need Tests/chain_sim/chain_sim.py 'def scenario_hub_always_succeeds(elf, level):' "the chain test runs the commands that succeed in every state with no network thread to serve them, at both log levels"
need Tests/stack_usage.sh 'budget dlp_hub "azl::DlpHub::Serve" 40' "Serve keeps its 40 bytes"
need Tests/stack_usage.sh 'budget dlp_hub "azl::DlpHub::OnGameUdsStarting" 24' "OnGameUdsStarting keeps its 24 bytes"
need Tests/stack_usage.sh 'budget dlp_hub "azl::DlpHub::AnswerUnserved" 32' "the unserved branch has its own budget"
need Tests/stack_usage.sh 'budget dlp_hub "azl::DlpHub::RequestInFlight" 24' "... and the in-flight test"
need Tests/stack_usage.sh 'budget dlp_hub "azl::DlpHub::AlwaysSucceeds" 8' "the table of the commands that cannot fail"
need Tests/stack_usage.sh 'budget dlp_hub "azl::DlpHub::EndsRole" 8' "the table of the ones that end a role"
need Tests/dlp_net_stack_check.sh 'FRAME_HUB=40 ' "no function of the hub has a frame over 40 bytes on ARM32 (the code a game thread runs to serve a dlp command)"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(DlpHub) <= 408,' "the hub is 408 bytes on ARM32: a growth is a decision"
# The debug trace of the hub's commands (Phase 4, DESIGN-FASE3 sez. 6): the log of the console test is the only record of what the game asked.
need Sources/ctr/uds_hle.cpp 'constexpr u32 kDlpTraceWords = 6;' "the trace shows 6 words of a request and of a reply"
need Sources/ctr/uds_hle.cpp 'constexpr u32 kDlpTraceEveryMs = 2000;' "a reply the command already gave is counted, not written again, for 2 s"
need Sources/ctr/uds_hle.cpp 'constexpr u32 kDlpTraceSigs = 6;' "... and the trace remembers 6 replies per command in that window (4 groups, 'no data', one more: the games poll a command that alternates between a few replies)"
need Sources/ctr/uds_hle.cpp 'line.Str("dlpc: ").Str(DlpServiceName(srvr))' "the trace's lines start with dlpc: (the reader of the console test reads them)"
need Sources/ctr/uds_hle.cpp ' more like it since the last line' "the trace says how many calls it left out (the reader counts them)"
need Sources/ctr/uds_hle.cpp 'DlpTraceRequest(srvr, cmd); // (debug only; before the reply replaces the request words)' "the request is taken before the reply replaces its words"
need_after Sources/ctr/uds_hle.cpp 'DlpTraceReply(srvr, id, cmd); // (debug only)' 'NoteCommand(DlpServiceName(srvr), id, cmd[1]);' "the trace line comes before NoteCommand, which stays the LAST call of RunDlpHub (a tail call: RunDlp's frame is gone when its Log() runs)"
need Sources/ctr/uds_hle.cpp 'if (g_debug_level <= 0 || id >= kDlpTraceIds) {' "the trace does nothing without the debug level (the test is inside the function: outside it the compiler kept g_debug_level in a register across RunDlp, 8 bytes more at every level)"
need Sources/ctr/uds_hle.cpp '.Str(": the Download Play lobby is carried by the room (thread ").Dec(thread_id).Str(", priority 0x")' "the first command of each dlp service says which thread made it and its priority (the hub's threads wait for each other's locks; the network thread is at 0x2F)"
need Tests/ipc_test/dlp_lobby_hle_test.cpp '    L15();' "the trace is tested in the 32-bit front-end test (L15, in fresh processes)"
need Tests/ipc_test/dlp_lobby_hle_test.cpp 'std::strcmp(argv[1], "--l15") == 0' "... in its five-six parts"
need Tests/chain_sim/chain_sim.py 'def scenario_hub_trace_dedupe(elf, level):' "the chain test runs the trace's window of replies on the compiled ARM code at both log levels"
need Tests/stack_usage.sh 'budget uds_hle "azl::ctr::UdsHle::RunDlp" 32' "RunDlp keeps its 32 bytes (the trace is called from RunDlpHub without growing its frame)"
need Tests/stack_usage.sh 'budget uds_hle "azl::ctr::{anonymous}::NoteDlpFirst" 48' "the first-command line has its frame (48)"
need Tests/stack_usage.sh 'budget uds_hle "azl::ctr::{anonymous}::DlpTraceRequest" 24' "the trace's request capture"
need Tests/stack_usage.sh 'budget uds_hle "azl::ctr::{anonymous}::DlpTraceReply" 72' "the trace's line"
need Includes/azl/dlp_fkcl_hle.hpp 'static constexpr u32 kResInvalidState = 0xE0A0AC01;' "a command beyond the wireless check gets the service's own 'invalid state'"
need Tests/dlp_fkcl_test.cpp 'TestOffByDefault();' "the test that dlp:FKCL is the real service by default runs"

# ---- the fake Download Play lobby, Phase 1 (Includes/azl/dlp_types.hpp, dlp_lobby.hpp, dlp_ipc.hpp; analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE1.md) ----
# Three pure headers, tested on the PC only: nothing in the plugin includes them yet (Phase 3 does, and lifts that pin). What must not change by accident: the ceiling of
# the user (THERE IS NEVER A FIFTH PLAYER, and every table is sized on the constants), the memory (no heap, objects of a fixed size, a few hundred bytes), the stack (the
# code runs on the game thread that made the IPC call, some of which have 464 bytes below it), the numbers the games read (the results of the module, the channel mask, the
# records, the times), the wire format between two plugins, and the answer of the probe mode that Battle Royale is served by (DlpFkclHle, which stays as it is).
# - the ceiling of the user (2026-10-28): THERE IS NEVER A FIFTH PLAYER, a host and 3 clients and no more
need Includes/azl/dlp_types.hpp 'constexpr u32 kMaxPlayers = 4;' "the fake lobby holds a host and 3 clients and never a fifth player"
need Includes/azl/dlp_types.hpp 'constexpr u32 kMaxClients = 3;' "a host has 3 clients at most"
need Includes/azl/dlp_types.hpp 'constexpr u32 kMaxGroups = 4;' "a searcher keeps 4 lobbies at most: a room of 4 players holds no more hosts"
need Includes/azl/dlp_types.hpp 'static_assert(kMaxPlayers == 4,' "the ceiling is an assertion too: raising it takes two edits, and the second says why not"
need Includes/azl/dlp_types.hpp 'static_assert(kMaxClients + 1 == kMaxPlayers,' "host + clients = players"
need Includes/azl/dlp_types.hpp 'static_assert(kMaxGroups <= kMaxPlayers,' "no more lobbies than players"
# - every table of the modules is sized on those constants and says so in an assertion (a table of 8 places is the first step to a fifth player)
need Includes/azl/dlp_lobby.hpp 'Group g[kMaxGroups];' "the scan table holds kMaxGroups lobbies"
need Includes/azl/dlp_lobby.hpp 'NodeEntry roster[kMaxPlayers];' "the roster of a session holds kMaxPlayers players"
need Includes/azl/dlp_lobby.hpp 'Track t[kMaxClients];' "the creator tracks kMaxClients clients"
need Includes/azl/dlp_lobby.hpp 'static_assert(sizeof(ScanTable::g) / sizeof(Group) == kMaxGroups,' "the scan table is checked against kMaxGroups"
need Includes/azl/dlp_lobby.hpp 'static_assert(sizeof(SessionTable::roster) / sizeof(NodeEntry) == kMaxPlayers,' "the roster is checked against kMaxPlayers"
need Includes/azl/dlp_lobby.hpp 'static_assert(sizeof(HostTable::t) / sizeof(Track) == kMaxClients,' "the creator table is checked against kMaxClients"
need Includes/azl/dlp_ipc.hpp 'u16 ids[kMaxPlayers];' "GetConnectingNodes writes 4 node ids at most, whatever the game asks"
need Includes/azl/dlp_ipc.hpp 'static_assert(sizeof(ids) / sizeof(ids[0]) == kMaxPlayers,' "the ids of GetConnectingNodes are checked against kMaxPlayers"
need Includes/azl/dlp_ipc.hpp 'u16 ids[kMaxClients];' "GetConnectingClients writes 3 client ids at most, whatever the game asks"
need Includes/azl/dlp_ipc.hpp 'static_assert(sizeof(ids) / sizeof(ids[0]) == kMaxClients,' "the ids of GetConnectingClients are checked against kMaxClients"
need Includes/azl/dlp_types.hpp 'static_assert(sizeof(HeardGroup) <= 40,' "a lobby heard is a local of a game thread: 36 bytes, and not more than 40"
# - the memory: a few hundred bytes next to a game that needs all of its own (272 measured), and no record of the game (5032 and 664 bytes) kept
need Includes/azl/dlp_lobby.hpp 'static_assert(sizeof(DlpLobby) <= 320,' "the whole lobby is at most 320 bytes: no heap, and one table of ONE of three shapes"
need Includes/azl/dlp_lobby.hpp 'static_assert(sizeof(Tables) <= 168,' "the tables are one union whose biggest shape is 168 bytes"
need Includes/azl/dlp_lobby.hpp 'static_assert(sizeof(Group) == 40,' "a lobby heard is 40 bytes with no u64: the same on ARM32 and on the PC"
need Includes/azl/dlp_ipc.hpp 'static_assert(sizeof(DlpService) <= 64,' "the IPC front-end is a handful of words: no buffer, no record of the game"
# - the results of the module that the games read (module 43, Download Play); success is 0 and there is no placeholder -1
need Includes/azl/dlp_types.hpp 'constexpr u32 kInvalidState = 0xE0A0AC01;' "a command out of its state, the module own result (DlpFkclHle answers the same)"
need Includes/azl/dlp_types.hpp 'constexpr u32 kBusy = 0xE0A0AC03;' "the module or the radio is taken by the other role or the engine"
need Includes/azl/dlp_types.hpp 'constexpr u32 kNoData = 0xC880AFEF;' "the end of the list: Triple Deluxe calls GetTitleInfoInOrder until it gets exactly this result, any other error is a title to it"
need Includes/azl/dlp_types.hpp 'constexpr u32 kNotFound = 0xC880AC05;' "server not found, the result Battle Royale handles for a join that cannot start"
need Includes/azl/dlp_types.hpp 'constexpr u32 kTimeout = 0xC920AFFE;' "the join did not finish in time"
need Includes/azl/dlp_types.hpp 'constexpr u32 kOutOfRange = 0xD8E0AFFD;' "StartAccepting with a channel that is not 0, 1, 6 or 11"
need Includes/azl/dlp_types.hpp 'constexpr u32 kInvalidPointer = 0xD8E0AFF6;' "a buffer of the game that cannot be used"
need Includes/azl/dlp_types.hpp 'constexpr u32 kBadHeader = 0xD9001830;' "an id the service does not have, answered as DlpFkclHle does (not 0xD900182F of the specification)"
need Includes/azl/dlp_ipc.hpp 'cmd[2] = 0x0421;' "GetChannels answers the mask of the channels 1, 6 and 11: Triple Deluxe calls it inside StartScan and a negative result is fatal there"
# - the sizes and the places of the records the games read in their own buffers (the layout is the game: Triple Deluxe confirmed the name at +0x0C and the node at +0x24)
need Includes/azl/dlp_types.hpp 'constexpr u32 kTitleInfoSize = 0x13A8;' "DLPTitleInfo is 5032 bytes"
need Includes/azl/dlp_types.hpp 'constexpr u32 kServerInfoSize = 0x298;' "DLPServerInfo is 664 bytes"
need Includes/azl/dlp_types.hpp 'constexpr u32 kDlpNodeInfoSize = 0x28;' "DLPNodeInfo is 40 bytes (and its name is not kNodeInfoSize, which uds_types.hpp has in the same namespace)"
need Includes/azl/dlp_types.hpp 'constexpr u32 kNodeInfoName = 0x0C;' "the name of a player is at +0x0C of the node record, as Triple Deluxe reads it (3dbrew says +0x08)"
need Includes/azl/dlp_types.hpp 'constexpr u32 kNodeInfoNodeId = 0x24;' "the node id is a u32 at +0x24 of the node record, as Triple Deluxe reads it"
need Includes/azl/dlp_types.hpp 'constexpr u32 kServerInfoTick = 0x290;' "ServerInfo+0x290 is the system tick the lobby was last heard at: the games drop an entry older than 5000 ms, and a 0 there deletes every lobby at every pass"
# - the wire format between two plugins (version 1): a change of any of these makes a console of the next version deaf to one of this
need Includes/azl/dlp_types.hpp 'constexpr u8 kAnnounceVersion = 1;' "the announcement of a lobby is version 1"
need Includes/azl/dlp_types.hpp 'constexpr u32 kAnnounceSize = 29;' "the announcement is 29 bytes (9 fixed and the 20-byte name)"
need Includes/azl/dlp_types.hpp 'constexpr u32 kMaxMessage = 16;' "a message is 16 bytes at most (the biggest, GO, is 11)"
need Includes/azl/dlp_types.hpp 'enum class MsgType : u8 { Start = 1, StartAck = 2, Go = 3, GoAck = 4 };' "the four messages and their numbers on the wire"
# - the times (DESIGN-FASE1 sez. 4.4): the numbers a game waits on, and the ones the games are known to bound
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kPollMs = 20;' "the network work runs at most every 20 ms"
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kMaxRx = 8;' "a pass reads 8 messages at most: the cost of a call into the lobby is bounded"
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kGroupTtlMs = 1500;' "a lobby that is no longer heard disappears after 1500 ms, well under the 5000 ms after which the game drops an entry"
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kJoinMs = 3000;' "a join gives up after 3000 ms, like the blocking StartFakeSession of Azahar"
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kHostGoneMs = 2000;' "a host missing for 2000 ms is a lost host"
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kResendMs = 200;' "START and GO are sent again every 200 ms"
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kStartGraceMs = 1500;' "a client that does not answer START in 1500 ms is thrown out"
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kGoGraceMs = 2000;' "the creator goes to 9 at most 2000 ms after it sent GO"
need Includes/azl/dlp_lobby.hpp 'static constexpr u32 kRosterMs = 250;' "the roster of a session is read again every 250 ms"
need Includes/azl/dlp_lobby.hpp 'static_assert(kGroupTtlMs < 5000,' "the game drops the entries older than 5000 ms: the lobby must drop them first"
need Includes/azl/dlp_lobby.hpp 'static_assert(kResendMs * 2 < kStartGraceMs && kResendMs * 2 < kGoGraceMs,' "at least two resends before the grace of START and of GO"
need Includes/azl/dlp_lobby.hpp 'static_assert(kPollMs <= kResendMs,' "Poll runs at least once per resend period"
# - Battle Royale is served today by DlpFkclHle and goes on being served by it: it stays exactly as it is (its text is pinned by its sha256 below), and DlpService
#   without a lobby (ProbeOnly) is tested against it word for word and counter for counter by dlp_ipc_test
need Includes/azl/dlp_fkcl_hle.hpp 'static constexpr u32 kResBadHeader = 0xD9001830;' "an id the service does not have gets the dispatcher own answer"
need Includes/azl/dlp_fkcl_hle.hpp 'static constexpr u32 kLastCommand = 0x11;' "DlpFkclHle knows the commands 1 to 0x11"
need Includes/azl/dlp_ipc.hpp 'DlpLobby* lobby_ = nullptr;' "a DlpService starts without a lobby, that is in ProbeOnly mode"
need Includes/azl/dlp_ipc.hpp 'return lobby_ != nullptr ? DlpMode::Lobby : DlpMode::ProbeOnly;' "the mode is decided by the lobby alone: none is ProbeOnly"
need Tests/dlp_ipc_test.cpp '    TestProbeOnlyIsDlpFkclHle();' "the test that ProbeOnly is DlpFkclHle runs"
need Tests/dlp_ipc_test.cpp '#include "azl/dlp_fkcl_hle.hpp"' "the test compares ProbeOnly with the real DlpFkclHle"
need Tests/dlp_ipc_test.cpp '#include "azl/uds_engine.hpp"' "the test includes the engine header beside the DLP ones: the plugin uds_hle.cpp includes both, and a clash of names (kNodeInfoSize) must stop the test, not the plugin"
need Tests/dlp_adv_ipc_test.cpp '#include "azl/uds_engine.hpp"' "the attack test includes the engine header beside the DLP ones too"
# - Phase 1 is PC only: the plugin still answers dlp:FKCL with DlpFkclHle (Phase 3 changes this, and lifts the pins below)
need_line Includes/ctr/uds_hle.hpp '#include "azl/dlp_fkcl_hle.hpp"' "the plugin front-end still carries the probe fake of Battle Royale"
need Sources/ctr/uds_hle.cpp 'dlp_.Dispatch(cmd);' "the plugin dlp:FKCL request still goes to the probe fake"
# - the PC tests of the lobby are built with the sanitizers and stop at the first undefined behaviour (an index beyond a table is a failure, not a line nobody reads)
need Tests/run.sh '-pthread -fsanitize=address,undefined"' "the PC tests are built with AddressSanitizer and UBSan"
need Tests/dlp_lobby_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "this test stops at the first undefined behaviour (UBSan options: halt_on_error=1)"
need Tests/dlp_ipc_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "this test stops at the first undefined behaviour (UBSan options: halt_on_error=1)"
need Tests/dlp_lobby_sim_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "this test stops at the first undefined behaviour (UBSan options: halt_on_error=1)"
need Tests/dlp_adv_lobby_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "this test stops at the first undefined behaviour (UBSan options: halt_on_error=1)"
need Tests/dlp_adv_ipc_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "this test stops at the first undefined behaviour (UBSan options: halt_on_error=1)"

# - no heap, no container, no function object, no lock, no exception and no system header in the three headers (only std::memcpy and std::memset are used); the one region of
#   'game-thread code' of each (counted by game_thread_file above) is also held to the rule of the code that runs on a game thread: no printf family, no Log() / DebugEvent()
#   with a format, no new / malloc, no std::string / std::vector, no char buffer over 64 bytes
for h in Includes/azl/dlp_types.hpp Includes/azl/dlp_lobby.hpp Includes/azl/dlp_ipc.hpp; do
  hit=$(tr -d '\r' < "$h" | sed 's://.*$::' | grep -o -E 'std::[A-Za-z_0-9]+|#include[[:space:]]*<[a-z_.]*>|\bthrow\b|\btry\b|\bcatch\b|\bthread_local\b|\bdelete[[:space:]]*(\[\])?[[:space:]]*[A-Za-z_(*]' | grep -v -E '^std::mem(cpy|set)$' | head -3)
  [ -z "$hit" ] || fail "$h uses a heap, a container, a function object, a lock, an exception or a system header (only std::memcpy and std::memset are allowed): $hit"
done
# - DlpFkclHle (the answer of Battle Royale's probe, and the oracle that DlpService without a lobby is tested against) is exactly the file that was tested; Phase 3 may replace it, and then changes this line
DLPFKCL_SHA=3f5019d169ad65249ab61d922f6991cbc393bba2b81feaffd1a614b07281b784
SUM=$(tr -d '\r' < Includes/azl/dlp_fkcl_hle.hpp | sha256sum | cut -d' ' -f1)
[ "$SUM" = "$DLPFKCL_SHA" ] || fail "Includes/azl/dlp_fkcl_hle.hpp is not the file of Phase 1 (sha256 $SUM): it is the answer of Battle Royale's probe and the oracle that DlpService in ProbeOnly mode is tested against; it stays as it is"
# - the plugin and the app include none of the three headers (Phase 1 is PC only; Phase 3 changes the plugin and lifts this pin)
hit=$(grep -r -n -E 'dlp_(types|lobby|ipc)\.hpp' Sources Includes ConfigApp/source --include=*.cpp --include=*.c --include=*.hpp --include=*.h | grep -v '^Includes/azl/dlp_' | head -3)
[ -z "$hit" ] || fail "the plugin or the app includes a header of the fake Download Play lobby other than the hub's (Phase 3: the plugin reaches the lobby only through azl/dlp_hub.hpp): $hit"
# - Phase 3: the plugin reaches the lobby through the hub alone. Only Includes/ctr/plugin.hpp (the member) and Sources/ctr/uds_hle.cpp (the HLE) include azl/dlp_hub.hpp, nothing includes the
#   adapter's header (the hub owns the adapter), and the app (ConfigApp) knows nothing of it.
hit=$(grep -r -n -E 'azl/dlp_hub\.hpp"' Sources Includes ConfigApp/source --include=*.cpp --include=*.c --include=*.hpp --include=*.h | grep -v '^Sources/azl/dlp_hub.cpp' | grep -v '^Includes/ctr/plugin.hpp' | grep -v '^Sources/ctr/uds_hle.cpp' | head -3)
[ -z "$hit" ] || fail "something other than plugin.hpp, uds_hle.cpp and the hub itself includes azl/dlp_hub.hpp: $hit"
hit=$(grep -r -n -E 'dlp_lobby_net\.hpp"' Sources Includes ConfigApp/source --include=*.cpp --include=*.c --include=*.hpp --include=*.h | grep -v '^Sources/azl/dlp_lobby_net.cpp' | grep -v '^Includes/azl/dlp_hub.hpp' | head -3)
[ -z "$hit" ] || fail "something other than the hub includes the adapter's header azl/dlp_lobby_net.hpp (the hub owns the adapter): $hit"
hit=$(grep -r -n -E 'dlp_hub|DlpHub|EngineLobbyNet' ConfigApp --include=*.cpp --include=*.c --include=*.hpp --include=*.h | head -3)
[ -z "$hit" ] || fail "the app (ConfigApp) uses the plugin's Download Play hub or adapter: $hit"

# ---- the stack and the size of the fake Download Play lobby on the plugin's own target (rules 3 and 4 of the user; DESIGN-FASE1 sez. 1.3) ----
# The PC tests are x86-64 with the sanitizers: they see neither the frame of an ARM function (some game threads have 464 bytes under them) nor what the plugin's compiler
# and flags say about the code. Tests/dlp_stack_check.sh compiles Tests/dlp_stack_probe.cpp with arm-none-eabi-g++ and the flags of the Makefile, reads the frame of every
# function and the sum of the frames down to the network, and Tests/run.sh runs it. What must not change by accident: that it runs, the flags it compiles with, its budgets,
# the sizes the probe asserts on ARM32, a wrapper for every public function, and the noinline that keeps the big frames out of their callers.
need_line Tests/run.sh '  bash Tests/dlp_stack_check.sh' "the suite runs the stack and size check of the fake lobby on the plugin's target (a frame of an ARM function is not seen by the x86-64 tests)"
need Tests/dlp_stack_check.sh '-c Tests/dlp_stack_probe.cpp -o "$W/probe_$pass.o"' "the check compiles the probe (passes A and B)"
need Tests/dlp_stack_check.sh '-c Tests/dlp_stack_probe.cpp -o "$W/probe_H.o"' "the check compiles the probe with the hub (pass H)"
need_line Tests/dlp_stack_check.sh 'for pass in A B; do' "two passes: as the plugin would build it (the sums of the frames are held) and with every function kept (every frame and every circle is seen)"
need_line Tests/dlp_stack_check.sh '  [ "$pass" = B ] && extra="-fkeep-inline-functions"' "pass B keeps every inline function: a public function that has no wrapper yet, or a helper that pass A inlines everywhere, is still measured"
need_line Tests/dlp_stack_check.sh '"$PY" "$W/check.py" --selftest' "the check tries itself on made-up listings first: a gate that cannot say no is not a gate"
need Tests/dlp_stack_check.sh 'FRAME_MAX=48 ' "no function of the lobby has a frame over 48 bytes on ARM32"
need Tests/dlp_stack_check.sh 'FRAME_DISPATCH=64 ' "Dispatch, which every command passes through, has a frame of 64 bytes at most"
need Tests/dlp_stack_check.sh 'FRAME_POLLSCAN=64 ' "PollScan, which holds the lobby heard, is the one function allowed 64 bytes"
need Tests/dlp_stack_check.sh 'CHAIN_MAX=160 ' "from a function of the lobby down to a call out of the module the frames add up to 160 bytes at most (Dispatch: 152 measured)"
need_line Tests/dlp_stack_check.sh 'ARM_FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -Os -mword-relocations -fomit-frame-pointer -ffunction-sections -fno-strict-aliasing -Wall -Wno-psabi -fno-rtti -fno-exceptions -std=gnu++17"' "the probe is compiled with the flags of the Makefile (ARM11 MPCore, -Os, no RTTI, no exceptions, gnu++17): the frames of another compilation are not the plugin's"
need_line Tests/dlp_stack_check.sh 'STRICT_FLAGS="-Wextra -Wpedantic -Wshadow -Werror -fstack-usage -fcallgraph-info=su"' "the headers must be clean for the plugin's compiler, and the compiler is asked for the frames and the call graph"
need Tests/dlp_stack_check.sh '"${DEVKITARM:+$DEVKITARM/bin/arm-none-eabi-g++}" /opt/devkitpro/devkitARM/bin/arm-none-eabi-g++; do' "the compiler is looked for where the devkitARM container keeps it (not in its PATH)"
need_after Tests/dlp_stack_check.sh 'if [ -z "$ARM_GXX" ]; then' 'if [ -n "$DLP_STACK_REQUIRED" ]; then' "without the compiler the check is skipped aloud, and DLP_STACK_REQUIRED makes that a failure"
need_after Tests/dlp_stack_check.sh 'if [ -z "$CTRU" ]; then' 'if [ -n "$DLP_STACK_REQUIRED" ]; then' "without the libctru headers the pass H is skipped aloud, and DLP_STACK_REQUIRED makes that a failure"
# Nothing may be kept in memory: a `static u8 scratch[4096]` is not in sizeof(DlpLobby) and not in a frame. The object file of the probe shows it (data and bss stay at 0),
# and the array scan of the game-thread code looks at every line that is not a constant.
need Tests/dlp_stack_check.sh 'read -r _text data bss _rest < <("$ARM_SIZE" "$W/probe_$pass.o" | awk '"'"'NR == 2'"'"')' "the check reads the size of the object file of the probe (text data bss)"
need_after Tests/dlp_stack_check.sh '"$W/probe_$pass.o" | awk' 'if [ "$data" != 0 ] || [ "$bss" != 0 ]; then' "the fake lobby keeps no static storage: data and bss of the probe are 0 (a static buffer would hide from sizeof and from the frames)"
need_after Tests/dlp_stack_check.sh '"$W/probe_H.o" | awk' 'if [ "$data" != 0 ] || [ "$bss" != 0 ]; then' "the hub's header keeps no static storage: data and bss of the probe with the hub are 0"
# A test of the lobby that hangs (a loop that waits for something a regression no longer does) must fail the suite after 20 minutes, not stop it for good.
for t in dlp_lobby_test dlp_ipc_test dlp_lobby_sim_test dlp_adv_lobby_test dlp_adv_ipc_test dlp_net_test dlp_net_adv_test dlp_net_churn_test dlp_net_extra_test dlp_real_engine_test dlp_real_hub_test dlp_hub_test dlp_hub_test_tsan plugin_dlp_test; do
  need_line Tests/run.sh "  timeout 1200 \"\$OUT/$t\"" "the suite runs $t under a timeout: a hang is a failure, not a stop"
done
ARM_ARCH=$(tr -d '\r' < Makefile | sed -n 's/^ARCH[[:space:]]*:=[[:space:]]*//p')
[ -n "$ARM_ARCH" ] && tr -d '\r' < Tests/dlp_stack_check.sh | grep -F -q -- "$ARM_ARCH" || fail "Tests/dlp_stack_check.sh does not compile for the Makefile's ARCH ('$ARM_ARCH'): the frames it measures are not the plugin's"
[ -n "$ARM_ARCH" ] && tr -d '\r' < Tests/dlp_net_stack_check.sh | grep -F -q -- "$ARM_ARCH" || fail "Tests/dlp_net_stack_check.sh does not compile for the Makefile's ARCH ('$ARM_ARCH'): the frames it measures are not the plugin's"
need Makefile '$(ARCH) -Os -mword-relocations' "the plugin is built with -Os (the check compiles with it too)"
need Makefile '-fno-rtti -fno-exceptions -std=gnu++17' "the plugin is built without RTTI and exceptions as gnu++17 (the check compiles with the same)"
# - the probe: the sizes on ARM32 as assertions, and a wrapper for every public function of the two classes
need_line Tests/dlp_stack_probe.cpp '#if !defined(__arm__)' "the probe measures the plugin's target and refuses another compiler"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(void*) == 4,' "the probe knows it is ARM32"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(DlpLobby) <= 272,' "the whole lobby is 272 bytes on ARM32: a growth is a decision"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(DlpService) <= 40,' "the IPC front-end is 40 bytes on ARM32: a growth is a decision"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(DlpLobby::Tables) == 168,' "the tables are one union of 168 bytes on ARM32"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(Group) == 40,' "a lobby heard is 40 bytes on ARM32"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(NodeEntry) == 32,' "a player as the network reports it is 32 bytes on ARM32"
need Tests/dlp_stack_probe.cpp 'static_assert(sizeof(HeardGroup) <= 40,' "a lobby heard on the stack of a game thread is at most 40 bytes on ARM32"
need Tests/dlp_stack_probe.cpp 'PROBE_NOINLINE void p_OnSessionClosed(DlpService* s, DlpKind kind) { s->OnSessionClosed(kind); }' "the close of a game's session (a game thread) is measured too: without its wrapper the stack check would not see it, and only the ARM compiler would notice"
dlp_wrappers() { # $1 = header, $2 = class: every public function of the class is called by the probe
  local names n
  names=$(tr -d '\r' < "$1" | awk -v cls="$2" '
      $0 ~ "^class " cls " [{]" { inclass = 1; pub = 0; next }
      inclass && /^public:/ { pub = 1; next }
      inclass && /^(private|protected):/ { pub = 0; next }
      inclass && /^[}];/ { inclass = 0 }
      inclass && pub && /^    [A-Za-z_][A-Za-z0-9_:<>*& ]* [*&]?[A-Za-z_][A-Za-z0-9_]*[(].*[)]( const)? [{]$/ { s = $0; sub(/[(].*$/, "", s); sub(/^.* [*&]?/, "", s); print s }')
  [ -n "$names" ] || { fail "$1: the scan for the public functions of $2 found none (the probe's wrappers cannot be checked)"; return; }
  for n in $names; do
    grep -q -E "(->|[.])$n[(]" Tests/dlp_stack_probe.cpp || fail "Tests/dlp_stack_probe.cpp has no wrapper for $2::$n (every public function of the lobby is measured on ARM32 as the plugin calls it)"
  done
}
dlp_wrappers Includes/azl/dlp_lobby.hpp DlpLobby
dlp_wrappers Includes/azl/dlp_ipc.hpp DlpService
# - the noinline that keeps the big frames out of their callers (the check measures the result, this holds the cause: with 13 and 28 of them the frames are 48 or less)
need Includes/azl/dlp_lobby.hpp 'DLP_NOINLINE bool Announce(u8 accepting, u32 channel) {' "the announcement is built in a frame of its own: the creator's commands stay small"
need Includes/azl/dlp_lobby.hpp 'DLP_NOINLINE void PollScan() {' "the lobby heard (36 bytes) lives in a frame of its own: the only one over 48"
[ "$(grep -c '^    DLP_NOINLINE ' Includes/azl/dlp_lobby.hpp)" = "13" ] || fail "Includes/azl/dlp_lobby.hpp has not 13 functions marked DLP_NOINLINE (each keeps a frame out of its callers; a change is a decision: tell Tests/dlp_stack_check.sh and this line)"
[ "$(grep -c '^    DLPIPC_NOINLINE ' Includes/azl/dlp_ipc.hpp)" = "28" ] || fail "Includes/azl/dlp_ipc.hpp has not 28 functions marked DLPIPC_NOINLINE (each handler keeps its frame out of Dispatch; a change is a decision: tell Tests/dlp_stack_check.sh and this line)"
# - no array over 64 bytes with a number in its size in the code of the three headers (the stack check sees every size, the scan sees a number as it is written): char[N] is
#   already refused above; here also u8, u16, u32, u64, Mac and the records of the lobby. A table sized by a constant (Group g[kMaxGroups]) is a member and is held by its assertion.
dlp_big_arrays() { # $1 = a header of the lobby
  local hit
  hit=$(tr -d '\r' < "$1" | awk -v b="$GT_BEGIN" -v e="$GT_END" '
      BEGIN { n = split("char:1 bool:1 u8:1 s8:1 u16:2 s16:2 u32:4 s32:4 int:4 unsigned:4 u64:8 s64:8 Mac:6 Track:8 HostAnnounce:28 Identity:32 NodeEntry:32 HeardGroup:36 Group:40", t, " "); for (i = 1; i <= n; i++) { split(t[i], kv, ":"); sz[kv[1]] = kv[2] } }
      $0 ~ b { on = 1; next }
      $0 ~ e { on = 0; next }
      on { line = $0; sub(/\/\/.*$/, "", line); s = line
           if (line ~ /constexpr|static[[:space:]]+const/) next
           while (match(s, /[A-Za-z_][A-Za-z0-9_]*[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(\[[[:space:]]*[0-9]+[[:space:]]*\])+/)) {
             m = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
             type = m; sub(/[[:space:]].*$/, "", type)
             dims = m; sub(/^[^[]*/, "", dims)
             bytes = sz[type] + 0
             if (bytes > 0) { nd = split(dims, d, "]"); for (i = 1; i < nd; i++) { x = d[i]; sub(/^[[:space:]]*\[[[:space:]]*/, "", x); bytes *= x + 0 } }
             if (bytes > 64) print FNR ": " m
           } }' | head -2)
  [ -z "$hit" ] || fail "$1 has an array over 64 bytes on a game thread (some of them have 464 bytes under the call): $hit"
}
dlp_big_arrays Includes/azl/dlp_types.hpp
dlp_big_arrays Includes/azl/dlp_lobby.hpp
dlp_big_arrays Includes/azl/dlp_ipc.hpp

# ---- the fake Download Play lobby, Phase 2: the adapter on the UDS engine (Includes/azl/dlp_lobby_net.hpp, Sources/azl/dlp_lobby_net.cpp and the lobby's pieces of the engine; analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE2.md) ----
# The lobby is a UDS network of its own that two plugins host and join; the engine is the game's nwm::UDS as well and has ONE state. What must not change by accident: whose network it
# is (the Download Play's ids, never the game's), who may speak on it (a message counts only from the MAC of the host, or of the node it names), how much the engine keeps for it (a ring
# of 8 messages of 16 bytes, 4 lobbies of 32 bytes: the user's ceiling of 4 players), that it hands the engine back clean, that nothing is allocated or kept by the adapter, and that
# the plugin does not use any of it yet (Phase 3).
game_thread_file Includes/azl/dlp_lobby_net.hpp 1
game_thread_file Sources/azl/dlp_lobby_net.cpp 1
dlp_big_arrays Includes/azl/dlp_lobby_net.hpp
dlp_big_arrays Sources/azl/dlp_lobby_net.cpp
# - the Download Play's own ids (a game's scan must not list the lobby, and ours must not list a game): wlancommid 0x2810 and id8 0x55 are the ones the real Download Play uses
need Includes/azl/uds_engine.hpp 'static constexpr u32 kLobbyWlanCommId = 0x00002810;' "the lobby's network is the Download Play's, never a game's: a game's scan (filtered by its ids) must not list it"
need Includes/azl/uds_engine.hpp 'static constexpr u8 kLobbyId8 = 0x55;' "...and its id8"
need Sources/azl/dlp_lobby_net.cpp 'engine_->BeginHostingLobby(UdsEngine::kLobbyWlanCommId, UdsEngine::kLobbyId8, channel,' "the creator hosts the lobby under the Download Play's ids"
need Sources/azl/uds_engine.cpp 'LoadBE32(tags.network + 4) != kLobbyWlanCommId || tags.network[8] != kLobbyId8) {' "the engine files only the beacons of the Download Play's ids as lobbies"
need Sources/azl/uds_engine.cpp 'if (!GetBeaconApplicationData(tags, &data, &size) || size == 0 || tags.network_size <= kMaxAt ||' "a beacon with no application data, or a tag too short for the node counts, is no lobby"
# - the sizes the engine keeps for it: 8 messages of 16 bytes, 4 lobbies of 32 bytes of announcement (a room of 4 players holds 4 hosts at most)
need Includes/azl/uds_engine.hpp 'static constexpr u8 kLobbyChannel = 0xD1;' "the lobby's messages travel on a data channel of their own"
need Includes/azl/uds_engine.hpp 'static constexpr u32 kLobbyMessageMax = 16;' "a lobby message is 16 bytes at most (the largest, GO, is 11)"
need Includes/azl/uds_engine.hpp 'static constexpr u32 kLobbyInbox = 8;' "the ring of the lobby's messages holds 8: 144 bytes"
need Includes/azl/uds_engine.hpp 'static constexpr u32 kHeardLobbies = 4;' "the engine remembers 4 lobbies: a room of 4 players holds at most 4 hosts"
need Includes/azl/uds_engine.hpp 'static constexpr u32 kHeardDataMax = 32;' "an announcement is 29 bytes: the engine copies 32 of a beacon's application data"
need Includes/azl/dlp_lobby_net.hpp 'static_assert(kMaxGroups <= UdsEngine::kHeardLobbies,' "the engine remembers as many lobbies as the searcher keeps"
need Includes/azl/dlp_lobby_net.hpp 'static_assert(kAnnounceSize <= UdsEngine::kHeardDataMax,' "an announcement fits in what the engine copies out of a beacon"
need Includes/azl/dlp_lobby_net.hpp 'static_assert(kMaxMessage <= UdsEngine::kLobbyMessageMax,' "a message fits in what the engine's ring keeps"
need Includes/azl/dlp_lobby_net.hpp 'static constexpr u32 kHeardFreshMs = 400;' "a lobby is listed while its last beacon is younger than 400 ms (they come every 102 ms; the lobby's own table keeps an entry 1500 ms)"
need Includes/azl/dlp_lobby_net.hpp 'static constexpr u16 kBlockClients = 0x2;' "UDSNETATTR_DISABLE_CONNECT: the creator that stops accepting closes the admission in the engine"
# - who may speak: the transmitter of the frame decides, never the node id in its header; a stranger's frame, a broadcast, a size outside 1..16 are refused
need Sources/azl/uds_engine.cpp 'if (lobby_inbox_ && secure_data.data_channel == kLobbyChannel) {' "the lobby's messages are taken before the binds: they never reach a game's bind"
need Sources/azl/uds_engine.cpp 'bool ok = secure_data.dest_node_id == connection_status_.network_node_id;' "the lobby speaks to one node at a time: a broadcast is not its message"
need Sources/azl/uds_engine.cpp 'it->second.node_id == secure_data.src_node_id;' "the creator takes a message only from the node that has the transmitter's MAC"
need Sources/azl/uds_engine.cpp 'ok = status == NetworkStatus::ConnectedAsClient && secure_data.src_node_id == kHostDestNodeId &&' "the searcher takes a message only from node 1 ..."
need_line Sources/azl/uds_engine.cpp '             packet.transmitter_address == network_info_.host_mac_address;' "... and only from the MAC of the host it joined (a second lobby has the same node ids)"
need Sources/azl/uds_engine.cpp 'if (!ok || n == 0 || n > kLobbyMessageMax) {' "a message outside 1..16 bytes, or from a stranger, is refused"
[ "$(grep -c 'copy_dedupe_.Seen(secure_data.src_node_id, secure_data.sequence_number)' Sources/azl/uds_engine.cpp)" = "3" ] || fail "Sources/azl/uds_engine.cpp: the copy a radio_like=2 sender adds is dropped in three places (the host's router, the lobby's ring, the binds)"
[ "$(grep -c 'lobby_->rx_first = static_cast<u8>((lobby_->rx_first + 1) % kLobbyInbox);' Sources/azl/uds_engine.cpp)" = "2" ] || fail "Sources/azl/uds_engine.cpp: the lobby's ring moves its first message on in two places (a full ring loses the oldest, a pop takes it)"
need Sources/azl/uds_engine.cpp 'at = lobby_->rx_first; // full: the oldest message makes room' "a full ring loses its oldest message and keeps the newest"
# - the engine hands out what the adapter needs, without a game's block: InitializeForLobby refuses an engine that is up, and a host says goodbye with Deauthentications (version 0x200 on)
need Sources/azl/uds_engine.cpp 'if (initialized_ || service_state_ != kServiceRunning) {' "InitializeForLobby touches nothing of a session that is up (a game's, or the lobby's own)"
need Sources/azl/uds_engine.cpp 'version_ = kMaxVersion; // (a host sends the Deauthentications of its goodbye from version 0x200 on)' "the lobby's host says goodbye to its clients: a goodbye lost leaves them connected to a host that is gone"
need Sources/azl/uds_engine.cpp 'return initialized_ && (status == static_cast<u32>(NetworkStatus::ConnectedAsClient) ||' "ConnectPoll says 1 only for a node that is a client of its network"
need Sources/azl/uds_engine.cpp 'network_info_.application_data_size = static_cast<u8>(app_size);' "the lobby's announcement is the application data of its beacon"
[ "$(grep -c -F 'StoreBE32(network_info_.network_id_be, network_id | 1u);' Sources/azl/uds_engine.cpp)" = "2" ] || fail "Sources/azl/uds_engine.cpp: the lobby's network (BeginHostingLobby) and the real Download Play's (BeginHostingDlp, 1.1.3) each have a network id of their own, never 0 (the line twice)"
# - the adapter: it takes the engine only when it is free, gives it back whole, and keeps nothing
need Sources/azl/dlp_lobby_net.cpp 'if (engine_->InitializeForLobby(me.seed, me.name) != res::kSuccess) {' "the lobby takes the service only when no game has it"
need Sources/azl/dlp_lobby_net.cpp 'return !engine_->initialized() && engine_->PrepareLobby();' "a scan is possible unless a game has the service, and it asks the engine to keep the lobbies it hears"
need Sources/azl/dlp_lobby_net.cpp '    engine_->Shutdown();' "the lobby gives the service back whole (a game's Initialize after ours must not inherit our node, name or seed)"
need Sources/azl/dlp_lobby_net.cpp 'engine_->UpdateNetworkAttribute(kBlockClients, a.accepting != 0 ? 0 : 1);' "a creator that stops accepting closes the admission in the engine, not only in its announcement"
need Sources/azl/dlp_lobby_net.cpp 'if (to_node < 1 || to_node > kMaxPlayers || size == 0 || size > kMaxMessage) {' "a message goes to one node of 1..4, 1..16 bytes"
need Sources/azl/dlp_lobby_net.cpp 'for (u32 id = 1; id <= 16 && count < kMaxPlayers; ++id) {' "the lobby is told about 4 nodes at most, whatever the engine says"
need Sources/azl/dlp_lobby_net.cpp 'out->signal = 3;' "a lobby is always heard at full strength (as Azahar says)"
[ "$(grep -c 'SetLobbyInbox' Sources/azl/dlp_lobby_net.cpp)" = "0" ] || fail "Sources/azl/dlp_lobby_net.cpp switches the inbox itself: the engine does it with the network (BeginHostingLobby, ConnectBeginLobby, Shutdown), in the same breath, so that no frame of the handshake meets it off"
[ "$(grep -c 'lobby_inbox_ = true;' Sources/azl/uds_engine.cpp)" = "2" ] || fail "Sources/azl/uds_engine.cpp: the lobby's inbox is switched on in two places by the network that begins (the creator's BeginHostingLobby, the searcher's ConnectBeginLobby)"
need Sources/azl/uds_engine.cpp 'lobby_inbox_ = false; // (the lobby' "Shutdown switches the lobby's inbox off and empties the ring"
# - the plugin reaches the adapter and the engine's lobby functions only through the hub (Phase 3: Sources/azl/dlp_hub.cpp is the one caller; the front-end and the plugin never name them)
hit=$(grep -r -n -E 'EngineLobbyNet|dlp_lobby_net|InitializeForLobby|BeginHostingLobby|HeardLobbyAt|ConnectBeginLobby|SetLobbyInbox|PopLobbyMessage|NodeSnapshot' Sources/ctr Includes/ctr Sources/main.cpp ConfigApp/source 2>/dev/null | head -3)
[ -z "$hit" ] || fail "the plugin or the app names the adapter of the fake Download Play lobby or the engine's lobby functions: they are the hub's alone (Sources/azl/dlp_hub.cpp), the plugin goes through DlpHub: $hit"
# - the checks that hold all this: the suite builds and runs them (the unit test under a timeout, the stack and static storage check of the adapter on the plugin's target, the probe against a real room)
need_line Tests/run.sh '  bash Tests/dlp_net_stack_check.sh' "the suite runs the stack and static storage check of the lobby's adapter on the plugin's target"
need Tests/run.sh 'g++ $CXXFLAGS Tests/dlp_net_test.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -o "$OUT/dlp_net_test"' "the unit test of the adapter over the engine is built with the adapter and the core"
need Tests/run.sh 'g++ $CXXFLAGS Tests/dlp_room_probe.cpp Tests/mono_clock.cpp Sources/azl/dlp_lobby_net.cpp $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_time_get -o "$OUT/dlp_room_probe"' "the probe of the lobby over a real room is built (with the monotonic clock of the other room tests)"
need Tests/integration.sh '/tmp/azl_tests/dlp_room_probe $R1 24872' "the integration run plays the lobby over the real room server"
# 1.1.3, design 7.5: the real Download Play over the real room (I1-I3), built by the suite and played by the integration run
need Tests/run.sh 'g++ $CXXFLAGS Tests/dlp_real_room_probe.cpp Tests/mono_clock.cpp Sources/azl/dlp_real_net.cpp Sources/azl/dlp_real_wire.cpp "$OUT/dlph_wire.o" $CORE "$OUT"/enet_*.o -Wl,--wrap=enet_time_get -o "$OUT/dlp_real_room_probe"' "the probe of the real Download Play over a real room is built (with the monotonic clock of the other room tests)"
need Tests/integration.sh '/tmp/azl_tests/dlp_real_room_probe $R1 24872' "the integration run plays the real Download Play over the real room server (I1-I3)"
need Tests/dlp_real_room_probe.cpp 'return "halt_on_error=1:print_stacktrace=1";' "the probe of the real Download Play stops at the first undefined behaviour"
for s in TestI1 TestI2 TestI3; do
  need_line Tests/dlp_real_room_probe.cpp "    $s(host, port);" "the scenario $s of the real Download Play over the real room runs in main()"
done
[ "$(grep -c 'CHECK(false);' Tests/dlp_real_room_probe.cpp)" = "2" ] || fail "Tests/dlp_real_room_probe.cpp: a room that cannot be joined is a failure (TwoIn for I1 and I3, and I2's own; a probe that returns green without checking anything proves nothing)"
need_line Tests/dlp_real_room_probe.cpp '    CHECK(sent == got);' "I1: the frames of the announcement reach the searcher whole (each one the host sent, with its length)"
need_line Tests/dlp_real_room_probe.cpp '    CHECK_EQ(ghosts, 0u);' "I3: the searcher is node 2 in every round (no ghost node left by the round before)"
need_line Tests/dlp_real_room_probe.cpp '    CHECK(mem_most <= mem_after2 + 64 * 1024);  // (stable: a leak of a round would add up over 18 rounds)' "I3: the memory of the process is stable over the rounds"
need_line Tests/dlp_net_stack_check.sh 'ARM_FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -Os -mword-relocations -fomit-frame-pointer -ffunction-sections -fno-strict-aliasing -Wall -Wno-psabi -fno-rtti -fno-exceptions -std=gnu++17 -D__3DS__ -DAZL_USE_CTRPF"' "the adapter and the engine are compiled with the flags of the Makefile and the plugin's 3DS defines"
# - what the review of 2026-10-29 asked for: a client of a lobby drops the frames of another lobby, its roster follows the departures, the engine keeps lobby state only when a lobby is used
need Sources/azl/uds_engine.cpp 'PurgeLobbyMessagesLocked(node.node_id);' "a node that leaves takes its unread lobby messages with it"
[ "$(grep -c 'RefreshClientsLocked();' Sources/azl/uds_engine.cpp)" = "1" ] || fail "Sources/azl/uds_engine.cpp: a departure is told to the clients that remain in one place (RemoveNodeLocked, while a lobby is up)"
need Sources/azl/uds_engine.cpp 'if (slot.used && static_cast<u32>(stamp - slot.rx_ms) > kBeaconKeepMs) {' "a lobby silent for longer than the engine keeps a beacon is forgotten (unsigned age: no ghost after 24.8 days)"
need Sources/azl/uds_engine.cpp 'return; // (no lobby is in use: a game' "a game that uses no lobby does not parse a beacon for one"
need Sources/azl/uds_engine.cpp 'lobby_.reset(new (std::nothrow) LobbyState());' "what a lobby keeps is allocated the first time one is used, and a failure to allocate is an answer, not an abort"
need Includes/azl/uds_engine.hpp 'static_assert(sizeof(HeardLobbySlot) == 48,' "a lobby heard is 48 bytes"
need Sources/azl/uds_engine.cpp '    DebugSecure(kDirRx, packet, secure_data); // (the capture of a lobby' "a lobby frame that is taken is in the debug capture, as the ones that go out are"
[ "$(grep -c 'const bool took = !held_;' Sources/azl/dlp_lobby_net.cpp)" = "2" ] || fail "Sources/azl/dlp_lobby_net.cpp: HostOpen and ConnectBegin end the lobby on a failure only when it was this call that took the engine"
need Sources/azl/dlp_lobby_net.cpp 'if (a.max_nodes > kMaxPlayers) {' "a lobby of more than 4 players is never announced"
[ "$(grep -c 'slot.lobby.host == packet.transmitter_address' Sources/azl/uds_engine.cpp)" = "1" ] || fail "Sources/azl/uds_engine.cpp: a lobby whose host beacons other ids is forgotten in one place (NoteLobbyBeaconLocked)"
need Tests/dlp_net_adv_test.cpp "    XGameOwnsEngine();" "the scenario XGameOwnsEngine runs in main()"
need Tests/dlp_net_adv_test.cpp "    XFailures();" "the scenario XFailures runs in main()"
need Tests/dlp_net_adv_test.cpp "    XInbox();" "the scenario XInbox runs in main()"
need Tests/dlp_net_adv_test.cpp "    XRejoin();" "the scenario XRejoin runs in main()"
need Tests/dlp_net_adv_test.cpp "    XHostile();" "the scenario XHostile runs in main()"
need Tests/dlp_net_adv_test.cpp "    XNames();" "the scenario XNames runs in main()"
need Tests/dlp_net_adv_test.cpp "    XSpectator();" "the scenario XSpectator runs in main()"
need Tests/dlp_net_adv_test.cpp "    XBeacons();" "the scenario XBeacons runs in main()"
need Tests/dlp_net_adv_test.cpp "    XLimits();" "the scenario XLimits runs in main()"
need Tests/dlp_net_adv_test.cpp "    XStrangers();" "the scenario XStrangers runs in main()"
need Tests/dlp_net_adv_test.cpp "    XRosters();" "the scenario XRosters runs in main()"
need Tests/dlp_net_adv_test.cpp "    XDeparture();" "the scenario XDeparture runs in main()"
need Tests/dlp_net_adv_test.cpp "    XPurge();" "the scenario XPurge runs in main()"
need Tests/dlp_net_adv_test.cpp "    XStaleSlot();" "the scenario XStaleSlot runs in main()"
need Tests/dlp_net_adv_test.cpp "    XPresence();" "the scenario XPresence runs in main()"
need Tests/dlp_net_adv_test.cpp "    XSleep();" "the scenario XSleep runs in main()"
need Tests/dlp_net_adv_test.cpp "    XAdmission();" "the scenario XAdmission runs in main()"
need Tests/dlp_net_adv_test.cpp "    XNeverAccepted();" "the scenario XNeverAccepted runs in main()"
need Tests/dlp_net_adv_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "this test stops at the first undefined behaviour (UBSan options: halt_on_error=1)"
need Tests/dlp_net_adv_test.cpp '#include "dlp_net_rig.hpp"' "the adversarial half uses the rig of the first"
# The third and the fourth half (2026-10-29, second review round and second mutation round).
for t in dlp_net_churn_test dlp_net_extra_test; do
  need Tests/$t.cpp 'return "halt_on_error=1:print_stacktrace=1";' "the test $t stops at the first undefined behaviour (UBSan options: halt_on_error=1)"
  need Tests/$t.cpp '#include "dlp_net_rig.hpp"' "the test $t uses the rig of the first"
done
for s in "R_Race()" "TA_Ghost(500)" "TA_Ghost(1500)" "G1_HalfOpen()" "G3_ClientSilent()" "G4_RejoinRing()" "C_Closed()"; do
  need Tests/dlp_net_churn_test.cpp "    $s;" "the scenario $s of the churn test runs in main()"
done
need Tests/dlp_net_churn_test.cpp "        Fuzz(seed, 22);" "the churn fuzz runs 22 steps for each seed"
need Tests/dlp_net_churn_test.cpp "    for (u32 seed : {1u, 2u, 3u}) {" "the churn fuzz runs three seeds (seed 2 and 3 are the ones that found the ghost node)"
need Tests/dlp_net_churn_test.cpp "    CHECK_EQ(g_diverged, 0u);" "a roster or a message that disagrees in the churn fuzz is a failure"
for s in YHeardTable YFreshEngine YRing YStaleRing YGameChannel YRefresh YGameHostQuiet YClientQuiet YForeign YManagement YGoodbye YGameConnect YCleanNode YCapture YClockWrap YNoMemory YStaleNodeMap; do
  need Tests/dlp_net_extra_test.cpp "    $s();" "the scenario $s of the extra test runs in main()"
done
need Tests/run.sh '-Wl,--wrap=_ZN3azl8platform5NowMsEv -Wl,--wrap=malloc -o "$OUT/dlp_net_extra_test"' "the extra test is linked with the two wraps it needs (the clock and malloc): without them Y15 and Y16 fail on their own check"
# The two engine rules the churn fuzz found (the departure's Logoff is no admission for a console that has not been answered; a lobby has no guard for a second goodbye).
need Sources/azl/uds_engine.cpp "        if (lobby_inbox_ && !associated_) {" "a lobby client that is Connecting takes no Logoff before its host has answered the Authentication (the departure refresh is not its admission)"
need Sources/azl/uds_engine.cpp "(!lobby_inbox_ && LeftRecentlyLocked(packet.transmitter_address, now))" "a lobby host arms no guard against a late second goodbye (its peers say it once, and the guard would keep the node of a console that left twice)"
need Sources/azl/dlp_lobby_net.cpp "    if (a.accepting == 0) {" "a lobby opened closed is closed (BeginHostingLobby leaves the admission open)"
[ "$(grep -c 'CHECK(false)' Tests/dlp_room_probe.cpp)" = "4" ] || fail "Tests/dlp_room_probe.cpp: a room that cannot be joined is a failure in each of the four scenarios (a probe that returns green without checking anything proves nothing)"
[ "$(grep -c 'if (held_) {' Sources/azl/dlp_lobby_net.cpp)" = "3" ] || fail "Sources/azl/dlp_lobby_net.cpp: Acquire, ScanOpen and Kick each ask whether the lobby holds the engine (a scan is allowed while it does; nothing of a game's session is touched)"
need_after Sources/azl/uds_engine.cpp 'void UdsEngine::NoteLobbyBeaconLocked(' 'if (!lobby_) {' "a game that uses no lobby does not parse a beacon for one"
need Tests/dlp_net_stack_check.sh 'FRAME_ADAPTER=80 ' "no method of the adapter has a frame over 80 bytes on ARM32 (ScanRead, which holds the lobby heard, is the largest)"
need Tests/dlp_net_stack_check.sh 'FRAME_ENGINE=64 ' "no function of the engine's lobby pieces that a game thread reaches has a frame over 64 bytes"
need Tests/dlp_net_stack_check.sh '"$ARM_GXX" $ARM_FLAGS -Wextra -Wpedantic -Wshadow -Werror -fstack-usage -c Sources/azl/dlp_lobby_net.cpp -o "$W/adapter.o"' "the adapter is compiled for the plugin's target with the flags of the Makefile, strictly"
need Tests/dlp_net_stack_check.sh '"$ARM_GXX" $ARM_FLAGS -fstack-usage -c Sources/azl/uds_engine.cpp -o "$W/engine.o"' "the engine is compiled for the plugin's target with the flags of the Makefile"
need_after Tests/dlp_net_stack_check.sh '"$W/adapter.o" | awk' 'if [ "$data" != 0 ] || [ "$bss" != 0 ]; then' "the adapter keeps no static storage: data and bss of its object are 0"
need_after Tests/dlp_net_stack_check.sh '"$W/hub.o" | awk' 'if [ "$data" != 0 ] || [ "$bss" != 0 ]; then' "the hub keeps no static storage: data and bss of its object are 0"
need Tests/dlp_net_stack_check.sh 'if check_su "$W/bad_frame.su" ' "the gate tries itself first: a frame over its budget must be refused"
need Tests/dlp_net_stack_check.sh 'if check_su "$W/bad_dynamic.su" ' "... and a dynamic frame"
need Tests/dlp_net_test.cpp 'return "halt_on_error=1:print_stacktrace=1";' "this test stops at the first undefined behaviour (UBSan options: halt_on_error=1)"
need Tests/dlp_room_probe.cpp 'return "halt_on_error=1:print_stacktrace=1";' "this probe stops at the first undefined behaviour (UBSan options: halt_on_error=1)"
for s in TestClock TestEngineGuards TestHeardLobbies TestAnnounceAndScan TestJoin TestFullSequenceThenGameUds TestAdmissionClosedAndFull TestFourPlayers TestLeaveAndGone TestGameStartsUds TestTwoLobbies TestInboxAndStrangers TestNoAllocation; do
  need Tests/dlp_net_test.cpp "    $s();" "the scenario $s of the lobby over the engine runs in main()"
done
for s in TestLobbyThenGameUds TestFourPlayers TestLeaveAndGone TestTwoLobbies; do
  need Tests/dlp_room_probe.cpp "    $s(host, port);" "the scenario $s of the lobby over the real room runs in main()"
done
[ "$(grep -c 'CHECK_EQ(counted.count(), 0u);' Tests/dlp_net_test.cpp)" = "2" ] || fail "Tests/dlp_net_test.cpp: the reading calls of the adapter are proven to allocate nothing in two places (the console that holds the engine, the one that only looks)"
[ "$(grep -c 'CHECK_EQ(res, R_TIMEOUT);' Tests/dlp_net_test.cpp)" = "2" ] || fail "Tests/dlp_net_test.cpp: a join to a host that does not answer (full, or closed) is proven to give up with the games' own timeout, twice"

# ---- The swap of the plugin's memory on an Old 3DS: the HOME button must not take the game down (1.0.12; the gate, 1.1.2+) ----------------------------------
# Luma3DS saves the plugin's block to a file and frees it when an Old 3DS game goes to the HOME menu, and the game's threads are never stopped for it. Until 1.1.2 the
# sites were two words rewritten at every swap, and a thread asleep in the svc of a site woke up on the literal (LEGO Batman 2's dumps). Since the gate
# (analysis/fix-gancio-swap-old-2026-11-05): every site is ONE word `b stub`, written once before the game runs, the stubs and the gate (Sources/ctr/gate.s) are in a page
# the swap does not take away (ctr/gate_page.cpp), every real svc of the game is made by the gate, and the swap only flips the gate's flag: CloseGate at SWAP_ENTER (it
# waits for the threads counted in the plugin's code), OpenGate once the block is mapped again. The logic is held by Tests/gate_plan_test.cpp and Tests/hook_swap_test.cpp
# (and Tests/hook_swap_mutants.py), the assembly by Tests/hook_sim/gate_sim.py (Tests/hook_sim/gate_mutants.py), the whole chain by Tests/chain_sim/chain_sim.py; these pins
# hold the text of each piece, and the rules that make the gate the fix for good (analysis/fix-gancio-swap-old-2026-11-05/PROGETTO-CORREZIONE.md, 7.3).
need Sources/ctr/hook.cpp 'written = WriteGate(memory, g_gate_page.address, g_gate_page.bytes, blob, pointers, g_sites, g_site_count);' "the sites are hooked by the tested WriteGate: the gate, the stubs, then ONE word per site, once"
need Sources/ctr/hook.cpp 'g_gate_flag = &g_gate->flag;' "once the gate is there, the waits on a game thread look at the flag the swap closes"
need Sources/ctr/hook.cpp 'g_gate_page = TakeGatePage(g_sites[0].address, g_sites[g_site_count - 1].address, bytes, &stats.no_gate);' "the gate's page is taken for the span of the sites found"
# (the lines of a file from the first that has $2 to the first after it that is exactly $3, no CR; and code without its // comments)
region() {
  tr -d '\r' < "$1" | awk -v a="$2" -v b="$3" '!on && index($0, a) { on = 1 } on { print } on && $0 == b { exit }'
}
code_of() {
  sed 's://.*$::'
}
# 1. No word of the game's text is written but by the installation, once, before the game runs (InstallSvcHooks > WriteGate through GateTextMemory: the sites)
#    and by RewriteStubbedGspWrappers (the bl of a GSP wrapper, before the framework's search). A write anywhere else is a word that can change under a thread
#    asleep in a svc: the defect of 1.1.2 (LEGO Batman 2's dumps).
PATCHERS=$(grep -rn --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.c' 'Process::Patch(' Sources Includes | awk '{ f = $0; sub(/:.*$/, "", f); l = $0; sub(/^[^:]*:[0-9]*:/, "", l); sub(/\/\/.*$/, "", l); if (index(l, "Process::Patch(")) n[f]++ } END { for (f in n) print n[f], f }' | sort | tr '\n' ' ')
[ "$PATCHERS" = "3 Sources/ctr/hook.cpp " ] || fail "Process::Patch is called in '$PATCHERS', expected three times in Sources/ctr/hook.cpp (the gate's installation, the GSP wrapper's bl and the exit guard's svc 0x03 words): the game's text is written only before the game runs"
region Sources/ctr/hook.cpp 'ExitGuardStats InstallExitGuard(void (*guard)(), u32 stack_top) {' '}' | code_of | grep -q 'Process::Patch(addr, jump)' || fail "the exit guard's words are written somewhere else than in InstallExitGuard"
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | code_of | grep -c 'InstallExitGuard(')" = "1" ] || fail "InstallExitGuard is called more than once (it is the installation, before the game runs)"
need_order Sources/ctr/plugin.cpp '            hook_stats_ = InstallSvcHooks(hle_.get(), !swap_console_ && luma_alias_);' '                g_exit_stats = InstallExitGuard([]() { RunExitGuard(); },' "the exit guard is installed right after the hooks, in EarlyInit, before the game runs"
[ "$(region Sources/ctr/hook.cpp 'struct GateTextMemory {' '};' | code_of | grep -c 'Process::Patch(')" = "1" ] || fail "Sources/ctr/hook.cpp: GateTextMemory (what WriteGate writes the sites through) no longer has the one Process::Patch"
[ "$(region Sources/ctr/hook.cpp 'u32 RewriteStubbedGspWrappers() {' '}' | code_of | grep -c 'Process::Patch(')" = "1" ] || fail "Sources/ctr/hook.cpp: RewriteStubbedGspWrappers no longer has the other Process::Patch"
[ "$(cat Sources/ctr/*.cpp Includes/azl/*.hpp Includes/ctr/*.hpp | tr -d '\r' | code_of | grep -c 'GateTextMemory memory{')" = "1" ] && \
  region Sources/ctr/hook.cpp 'HookStats InstallSvcHooks(UdsHle* hle, bool reserve_exit_tramp) {' '}' | code_of | grep -q 'GateTextMemory memory{' || fail "the memory that writes the game's text (GateTextMemory) is made somewhere else than in InstallSvcHooks"
[ "$(cat Sources/ctr/*.cpp Sources/azl/*.cpp Includes/azl/*.hpp Includes/ctr/*.hpp | tr -d '\r' | code_of | grep -c 'PatchText(')" = "2" ] || fail "PatchText (one word of the game's text) is called somewhere else than in WriteGate (azl/gate_plan.hpp)"
[ "$(cat Sources/ctr/*.cpp Sources/azl/*.cpp Sources/*.cpp | tr -d '\r' | code_of | grep -c 'WriteGate(')" = "1" ] || fail "WriteGate is called more than once in the plugin (the sites are written once, by InstallSvcHooks)"
hit=$(grep -rnE --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.c' 'Process::(Write(8|16|32|64|Float|Double|String)?|CopyMemory|Poke|ProtectMemory|ProtectRegion)\b|\bPoke(8|16|32)?[[:space:]]*\(' Sources Includes | grep -v '^[^:]*:[0-9]*:[[:space:]]*//' | head -2)
[ -z "$hit" ] || fail "a write of the process's memory through the framework besides Process::Patch: $hit"
# ... the swap's path (SWAP_ENTER, the way back, SWAP_EXIT) writes no code: it only flips the gate's flag (GateClose / GateOpen write `flag` and nothing else of the header).
for fn in 'void Plugin::NoteProcessEvent(int event) {' 'void Plugin::OnSwapBack() {' 'extern "C" void __wrap_PLGLDR__Reply(s32 event) {'; do
  hit=$(region Sources/ctr/plugin.cpp "$fn" '}' | code_of | grep -E 'Process::|WriteGate|PatchText|StorePage|InstallSvcHooks|RewriteStubbed|svcFlushProcessDataCache|InstructionCache' | head -1)
  [ -z "$hit" ] || fail "Sources/ctr/plugin.cpp: the swap's path ($fn) writes code: $hit"
done
for fn in 'u32 CloseGate(u32 timeout_ms, void (*wake)(void*), void* arg) {' 'void OpenGate() {'; do
  hit=$(region Sources/ctr/hook.cpp "$fn" '}' | code_of | grep -E 'Process::|WriteGate|PatchText|StorePage|g_sites\[|g_gate->[a-z_]+ *=[^=]|svcFlushProcessDataCache|InstructionCache' | head -1)
  [ -z "$hit" ] || fail "Sources/ctr/hook.cpp: $fn writes something else than the gate's flag (through GateClose / GateOpen): $hit"
done
# ... nothing but GateTextMemory (the installation) syncs the caches of code, and nothing stores through the address of a site: a function of
#    its own that gave the sites their svc back at the swap (the two-word hook of 1.1.2) would need one or the other.
CACHES='svcFlushProcessDataCache|svcInvalidateEntireInstructionCache|svcInvalidateProcessDataCache|svcStoreProcessDataCache|InvalidateInstructionCache|FlushEntireDataCache'
[ "$(cat Sources/ctr/*.cpp Sources/azl/*.cpp Sources/*.cpp Includes/azl/*.hpp Includes/ctr/*.hpp | tr -d '\r' | code_of | grep -cE "\b($CACHES)\b")" = "8" ] && \
  [ "$(region Sources/ctr/hook.cpp 'ExitGuardStats InstallExitGuard(void (*guard)(), u32 stack_top) {' '}' | code_of | grep -cE "\b($CACHES)\b")" = "4" ] && \
  [ "$(region Sources/ctr/plugin.cpp 'void Plugin::RaiseRosalinaReplyWait() {' '}' | code_of | grep -cE "\b($CACHES)\b")" = "1" ] && \
  [ "$(region Sources/ctr/hook.cpp 'struct GateTextMemory {' '};' | code_of | grep -cE "\b($CACHES)\b")" = "3" ] || fail "a sync of the caches of code outside GateTextMemory (the installation of the gate): code written again somewhere else"
hit=$(cat Sources/ctr/*.cpp Includes/ctr/*.hpp | tr -d '\r' | code_of | grep -E 'reinterpret_cast<[[:space:]]*(volatile[[:space:]]+)?u(8|16|32)[[:space:]]*\*[[:space:]]*>[[:space:]]*\([^)]*g_sites\[|\([[:space:]]*(volatile[[:space:]]+)?u(8|16|32)[[:space:]]*\*[[:space:]]*\)[[:space:]]*\(?[[:space:]]*g_sites\[' | head -1)
[ -z "$hit" ] || fail "a store through the address of a site (the game's text written outside WriteGate): $hit"
# ... CloseGate hands the gate to GateClose whole, with its time in microseconds, and touches g_gate in no other way (Tests/chain_sim/chain_sim.py,
#    scenario_close_open_gate, runs the compiled CloseGate and OpenGate: the whole wait with a thread counted, the flag the only word written).
need Sources/ctr/hook.cpp '        *g_gate, static_cast<u64>(budget_ms) * 1000, []() { return platform::NowUs(); },' "CloseGate waits its time (ms to us, SWAP_ENTER's one deadline at most) on the gate itself"
for fn in 'u32 CloseGate(u32 timeout_ms, void (*wake)(void*), void* arg) {' 'void OpenGate() {'; do
  [ "$(region Sources/ctr/hook.cpp "$fn" '}' | code_of | grep -oE '\bg_gate\b' | wc -l | tr -d ' ')" = "2" ] || fail "Sources/ctr/hook.cpp: $fn uses g_gate otherwise than to test it and hand it to GateClose / GateOpen"
done
need Tests/chain_sim/chain_sim.py '        scenario_close_open_gate(elf, level)' "the whole-chain test runs the compiled CloseGate and OpenGate"
# ... the books come down only once the game's heap is SEEN (GameHeapSeen says yes only from svcQueryMemory; WaitForGameHeap only from GameHeapSeen, and only there)
[ "$(region Sources/ctr/kernel_books.cpp 'bool GameHeapSeen() {' '}' | code_of | grep -c 'return')" = "1" ] && \
  region Sources/ctr/kernel_books.cpp 'bool GameHeapSeen() {' '}' | code_of | grep -q -x -F '    return R_SUCCEEDED(svcQueryMemory(&info, &page, socplan::kRegionBase)) && info.state != MEMSTATE_FREE;' || \
  fail "Sources/ctr/kernel_books.cpp: GameHeapSeen says yes otherwise than from the game's region mapped"
region Sources/ctr/kernel_books.cpp 'bool WaitForGameHeap() {' '}' | code_of | grep -A1 -F 'if (GameHeapSeen()) {' | grep -q -F 'return true;' && \
  [ "$(region Sources/ctr/kernel_books.cpp 'bool WaitForGameHeap() {' '}' | code_of | grep -c 'return true')" = "1" ] || fail "Sources/ctr/kernel_books.cpp: WaitForGameHeap says yes without the game's heap seen"
for fn in 'inline u32 GateClose(' 'inline void GateOpen('; do
  body=$(region Includes/azl/hook_swap.hpp "$fn" '}' | code_of)
  hit=$(printf '%s\n' "$body" | grep -E '\bg\.[a-z_]+[[:space:]]*([-+*|&^]?=[^=]|\+\+|--)|Store[A-Za-z]*\(&g\.' | grep -v 'Ops::StoreFlag(&g.flag, ' | head -1)
  [ -z "$hit" ] && [ "$(printf '%s\n' "$body" | grep -c 'Ops::StoreFlag(&g.flag, ')" = "1" ] || fail "Includes/azl/hook_swap.hpp: $fn writes the gate's header besides its flag, or not the flag once: $hit"
done
# ... and the svc 0x32 words CTRPluginFramework hooks by itself (GSP::Initialize, the wrappers of RegisterInterruptRelayQueue) are never sites: the framework's
#    search wants them as its pattern has them and aborts at the game's start without them (the class of LEGO Batman 2 and Balloon Fight). The tested functions of
#    azl/gsp_wrapper.hpp (Tests/gsp_wrapper_test.cpp, TestFrameworkSites) find them before the scan and keep them out of it.
need Sources/ctr/hook.cpp '    g_framework_site_count = FindFrameworkSites(ReadGameWord, nullptr, first, end, g_framework_sites, kMaxFrameworkSites);' "the framework's own sites are found in the whole text"
need_order Sources/ctr/hook.cpp '    g_framework_site_count = FindFrameworkSites(ReadGameWord, nullptr, first, end, g_framework_sites, kMaxFrameworkSites);' '        if (!IsHookSite(word, addr, g_framework_sites, g_framework_site_count, &framework_site)) {' "... before the scan of the sites, which keeps them out"
region Sources/ctr/hook.cpp '        if (!IsHookSite(word, addr, g_framework_sites, g_framework_site_count, &framework_site)) {' '        }' | code_of | tail -2 | head -1 | grep -q -x '            continue;' || fail "Sources/ctr/hook.cpp: a word that is not a site of the hook (the framework's svc among them) is not skipped by the scan"
[ "$(region Includes/azl/gsp_wrapper.hpp 'inline bool IsHookSite(' '}' | code_of | grep -c -x -F '    if (word == kSvcSendSyncRequestWord) {')" = "1" ] && \
  need_after Includes/azl/gsp_wrapper.hpp '            if (framework[i] == address) {' '                return false;' "a framework's svc 0x32 is not a site of the hook" || fail "Includes/azl/gsp_wrapper.hpp: IsHookSite no longer looks for the framework's svc 0x32 among the sites"
need_stmt Tests/gsp_wrapper_test.cpp 'TestFrameworkSites();' "the GSP test runs the framework's sites and the filter of the hook's on an image with both wrappers"
# 2. Every real svc 0x32 / 0x23 the game makes through the hook is made by the gate (Sources/ctr/gate.s), in its page, uncounted: a thread asleep in it holds
#    nothing of the plugin and wakes up on the gate's own instructions. None from the plugin's code on a game thread (Tests/callgraph_check.py follows the
#    calls of the built ELF; this is the text).
hit=$(for f in $(grep -rlE 'svc[[:space:]]+#?0x(32|23)\b' Sources Includes | grep -v '^Sources/ctr/gate\.s$'); do
        tr -d '\r' < "$f" | sed 's://.*$::; s:@.*$::' | grep -nE '^[[:space:]]*svc[[:space:]]+#?0x(32|23)\b|("|\\[nt])[[:space:]]*svc[[:space:]]+#?0x(32|23)\b' | sed "s:^:$f\::"
      done | head -2)
[ -z "$hit" ] || fail "a svc 0x32 / 0x23 in the plugin's code besides the gate's (Sources/ctr/gate.s): $hit"
hit=$(tr -d '\r' < Sources/ctr/hook.cpp | awk -v b="$GT_BEGIN" -v e="$GT_END" '$0 ~ b { on = 1; next } $0 ~ e { on = 0; next } on { l = $0; sub(/\/\/.*$/, "", l); if (l ~ /svc(SendSyncRequest|CloseHandle)[[:space:]]*\(/) print FNR ": " l }' | head -2)
[ -z "$hit" ] || fail "Sources/ctr/hook.cpp: pre or post makes a real svc 0x32 / 0x23 itself (the gate makes the game's): $hit"
need Sources/ctr/gate.s '    GATE azl_gate_32, 0x32, azl_gate_pre32, azl_gate_post32' "the gate's two entries make the two svc of the game"
need Sources/ctr/gate.s '    GATE azl_gate_23, 0x23, azl_gate_pre23, azl_gate_post23' "the gate's two entries make the two svc of the game"
# 3. SWAP_ENTER closes the gate outside every lock (a game's thread counted in the gate may wait for one: the close would wait for its whole budget), and what runs
#    before it on the framework's thread (the trail) takes none either; CloseGate is called once, from there.
# SWAP_ENTER's one deadline (risk O-B, analysis/piano-2026-11-08): begun at the event from the APT anchor, applied to every wait, ended after the answer
need Sources/main.cpp '        azl::ctr::BeginSwapDeadline(azl::ctr::LastAptReceiveTick());' "SWAP_ENTER starts its one deadline from Luma3DS's event"
need_order Sources/main.cpp '        azl::ctr::BeginSwapDeadline(azl::ctr::LastAptReceiveTick());' '    azl::ctr::ChatProcessEvent(static_cast<int>(event));' "the deadline begins before the chat's mirror waits"
need Sources/ctr/hook.cpp '            __atomic_store_n(&g_apt_receive_tick, svcGetSystemTick(), __ATOMIC_RELAXED);' "the APT ReceiveParameter pre stores the anchor at every debug level"
need Sources/ctr/swap_gate.cpp '    const u32 budget_ms = SwapClampMs(timeout_ms); // (the one deadline of SWAP_ENTER: SwapClampMs)' "the plugin's threads' wait takes the deadline"
need Sources/ctr/hook.cpp '    const u32 budget_ms = SwapClampMs(timeout_ms); // (the one deadline of SWAP_ENTER, ctr/swap_gate.hpp)' "the hooks' drain takes the deadline"
need Sources/ctr/plugin.cpp '    const u32 budget_ms = SwapClampMs(timeout_ms); // (SWAP_ENTER'"'"'s one deadline, ctr/swap_gate.hpp; no deadline: the budget as given)' "the trail's wait takes the deadline"
need Sources/ctr/input_mirror.cpp '    const u64 left = SwapTicksLeft(); // (SWAP_ENTER'"'"'s one deadline, ctr/swap_gate.hpp; none at EXIT: the full second)' "the mirror's wait takes the deadline"
need_order Sources/ctr/plugin.cpp '            CloseSwapGate(kSwapGateTimeoutMs);' '                WaitGateDrained();' "a game's thread still in the hook is waited for with what is left of the deadline"
need_order Sources/ctr/plugin.cpp '                WaitGateDrained();' '            EndSwapDeadline();' "the deadline ends after the answer's last wait"
[ "$(grep -c 'kSwapDeadlineMs = 8000;' Includes/ctr/swap_gate.hpp)" = "1" ] || fail "Includes/ctr/swap_gate.hpp: SWAP_ENTER's deadline is no longer 8 s from Luma3DS's event (Rosalina waits 10 s, or 15 s raised)"
# The fake sessions with the gate shut (risk O-A, analysis/piano-2026-11-08): the gate answers a svc 0x32 on one itself and only marks a
# svc 0x23; the plugin fills the table under UdsHle'"'"'s lock and settles the closes on Plugin::Tick, by asking the kernel
need Sources/ctr/gate.s '    GATE azl_gate_32, 0x32, azl_gate_pre32, azl_gate_post32, 1' "svc 0x32's gate answers the fake sessions with the flag 0"
need Sources/ctr/gate.s '    GATE azl_gate_23, 0x23, azl_gate_pre23, azl_gate_post23, 2' "svc 0x23's gate marks the fake sessions closed with the flag 0"
need Sources/ctr/gate.s '    cmp   r0, #0                      @ (handle 0: never a fake one)' "handle 0 never matches a free slot"
need Sources/ctr/gate.s '    bne   8b                          @ (the slot itself is the plugin'"'"'s alone: it stays as it is)' "the gate never writes a slot of the fake sessions"
need Sources/ctr/gate.s '    DMB   r1                          @ the handle read before its result (the plugin writes the result, then the handle)' "the gate reads a slot's handle before its result (no emulator sees a missing barrier)"
need Sources/ctr/gate.s '    bne   5f                          @ closed while shut: the value may name something else now, the kernel answers' "a slot closed while shut is not answered by the gate"
need Sources/ctr/uds_hle.cpp '    GateFakeRemove(handle); // (the gate'"'"'s table of the fake sessions: written under this lock only)' "a forgotten fake session leaves the gate's table"
need Sources/ctr/uds_hle.cpp '        if (GateFakeAdd(fake, shut_result) < 0) {' "a new fake session goes into the gate's table"
need_order Sources/ctr/uds_hle.cpp '        fake_[slot] = fake;' '        if (GateFakeAdd(fake, shut_result) < 0) {' "the session is in the HLE's table, under its lock, before the gate's"
need Sources/ctr/plugin.cpp '    SettleGateFakeClosed(); // (the fake sessions the game closed while the gate was shut: one load when there is none)' "Plugin::Tick settles the fake sessions closed with the gate shut"
need Sources/ctr/hook.cpp '        if (R_FAILED(svcClearEvent(handle))) {' "a fake session closed with the gate shut is forgotten only once the kernel says it is gone"
need Sources/ctr/hook.cpp '            __atomic_store_n(&slot.handle, handle, __ATOMIC_RELEASE); // (the result is there before the gate can match the handle)' "the slot's result is written before its handle"
[ "$(grep -c 'kSwapDeadlineRaisedMs = 12500;' Includes/ctr/swap_gate.hpp)" = "1" ] || fail "Includes/ctr/swap_gate.hpp: SWAP_ENTER's deadline with Rosalina's wait raised to 15 s is no longer 12.5 s"
need Sources/ctr/plugin.cpp '    SetSwapDeadlineRaised(rosalina_wait_raised_); // (SWAP_ENTER'"'"'s deadline follows Luma3DS'"'"'s wait)' "the deadline follows Rosalina's wait once it is raised"
# The exit guard (REGISTRO phase Q): a New 3DS only, the word read and written through Luma's uncached alias, the guard on its own stack,
# its line in a file of its own; the tests that prove it are run
need Sources/ctr/plugin.cpp '    if (!swap_console_ && loader_linked) {' "the alias of Luma's word is looked for on a New 3DS linked to the loader only"
need Sources/ctr/plugin.cpp '            if (!swap_console_ && luma_alias_ && hook_stats_.gate_ready) {' "the exit guard is installed on a New 3DS with the gate and the alias only (never on an Old 3DS: the guard is in the block the swap takes away)"
need Sources/ctr/plugin.cpp 'constexpr const char* kLumaLogPath = "/3ds/3DSLink/AzaharLink.luma.log";' "the guard's line goes to a file of its own (the writer appends to the main log)"
need Sources/ctr/plugin.cpp '                                                reinterpret_cast<u32>(g_exit_guard_stack) + sizeof(g_exit_guard_stack));' "the guard runs on the plugin's own stack, not the exiting thread's"
need Sources/ctr/hook.cpp '    if (svcGetSystemInfo(&emulator, 0x20000, 0) == 0) {' "no alias, and so no guard, on an emulator"
need Sources/ctr/hook.cpp '        const u32 bytes = PageCount(g_site_count + (reserve_exit_tramp ? kExitTrampStubs : 0), blob.bytes) * kGatePageBytes; // (+ the exit guard'"'"'s trampoline, InstallExitGuard: a New 3DS only)' "the gate's page keeps room for the exit guard's trampoline, on a New 3DS only"
need Sources/ctr/plugin.cpp '            hook_stats_ = InstallSvcHooks(hle_.get(), !swap_console_ && luma_alias_);' "the reserve for the trampoline only where the guard goes"
need Includes/ctr/hook.hpp '    return *g_luma_word_ptr;' "the probes read Luma's word through the alias Luma writes (phase O read the cached copy)"
need Includes/azl/exit_guard.hpp '        return Action::Leave; // (not a state Luma3DS writes: never written over)' "a value that is not Luma's state is never written over"
need Includes/azl/exit_guard.hpp 'constexpr u32 kWaitBudgetMs = 1000;' "the guard waits 1 s at most (the system gives a closing game about 3 s)"
need Tests/run.sh '  "$OUT/exit_guard_test"' "the exit guard runs against the model of Luma3DS"
need Tests/stack_usage.sh '    "$PY" Tests/hook_sim/exit_tramp_sim.py "$WORK/gate" || FAILED=1' "the exit guard's trampoline runs in the emulator"
# The chat says who came into the room and who left it (2026-11-08, asked by the testers): from the list of members, on the network thread
need Sources/ctr/plugin.cpp '                sink->OnPresence(c.nickname.data(), c.nickname.size(), c.joined, now_ms);' "the chat tells who comes and goes"
need_order Sources/ctr/plugin.cpp 'void Plugin::OnRoomInfo(const room::RoomInfo& info) {' '    NoteRoomMembers(info);' "the presence lines are pushed on the network thread (the room's list of members), the chat ring's one writer"
# The guest in the game's sockets (Fantasy Life, REGISTRO phase S): only on 0xC8A0700B; never ShutdownSockets nor socInit as a guest
need Sources/ctr/soc.cpp '    if (rc == kSocAlreadyInitialized && g_guest_allowed && EnterGuest()) {' "the plugin becomes a guest only when the game initialised its sockets first, and only for a title of the list"
need Includes/azl/soc_plan.hpp '    0x0004000000113100ull, // Fantasy Life, Europe (seen: InitializeSockets at 1.9 s, Friend-A 2026-11-08)' "Fantasy Life shares the game's sockets"
need_order Sources/ctr/soc.cpp '    if (g_guest) {' '    const Result rc = socExit();' "a guest leaves before socExit (its ShutdownSockets would end the game's sockets)"
need Sources/ctr/soc.cpp '    d.name = "soc";' "the guest's device is named soc (libctru's sendto and the others check the name)"
need Sources/ctr/soc.cpp '    azl_enet_rcvbuf_bytes = azl_rcvbuf_limit(azl_enet_rcvbuf_bytes, kGuestPoolBytes);' "a guest's receive ring fits the game's smallest pool"
# Old 3DS: Rosalina's wait for the answer raised to 15 s in the plugin's own header, only from the exact 10 s Luma3DS writes
need Sources/ctr/plugin.cpp '    if (*wait_ns != kRosalinaReplyWaitNs) {' "Rosalina's wait is raised only from the 10 s Luma3DS writes"
need Sources/ctr/plugin.cpp 'constexpr u64 kRosalinaReplyWaitRaisedNs = 15000000000ull;  // Old 3DS: 15 s' "Rosalina's wait raised to 15 s"
need Sources/ctr/plugin.cpp '    if (swap_console_ && loader_linked) {' "Rosalina's wait is raised only on an Old 3DS linked to the plugin loader"
hit=$(region Sources/ctr/plugin.cpp 'void Plugin::NoteProcessEvent(int event) {' '            const u32 gate_left = CloseGate(kSwapDrainTimeoutMs, dlp_hub_ ? &WakeHubForSwap : nullptr, dlp_hub_.get());' | code_of | grep -E 'LockGuard|\.lock\(\)|LightLock_Lock|RecursiveLock_Lock|TakePatchLock|SwapGateScope|GateEnter|TryLockBriefly|CloseSwapGate' | head -1)
[ -z "$hit" ] || fail "Sources/ctr/plugin.cpp: SWAP_ENTER takes a lock (or stops the plugin's threads) before it closes the svc hook's gate: $hit"
for fn in 'void Plugin::WaitForTrail(u32 timeout_ms) {' 'void Plugin::NoteSwapToTrail(u32 kind, u32 number) {'; do
  hit=$(region Sources/ctr/plugin.cpp "$fn" '}' | code_of | grep -E 'LockGuard|\.lock\(\)|LightLock_Lock|RecursiveLock_Lock|TakePatchLock|SwapGateScope|GateEnter' | head -1)
  [ -z "$hit" ] || fail "Sources/ctr/plugin.cpp: $fn, which SWAP_ENTER runs before it closes the gate, takes a lock: $hit"
done
hit=$(region Sources/ctr/hook.cpp 'u32 CloseGate(u32 timeout_ms, void (*wake)(void*), void* arg) {' '}' | code_of | grep -E 'LockGuard|\.lock\(\)|LightLock_Lock|RecursiveLock_Lock|TakePatchLock' | head -1)
[ -z "$hit" ] || fail "Sources/ctr/hook.cpp: CloseGate takes a lock: $hit"
[ "$(cat Sources/ctr/*.cpp Sources/*.cpp | tr -d '\r' | code_of | grep -c 'CloseGate(kSwapDrainTimeoutMs,')" = "1" ] || fail "CloseGate is called from somewhere else than SWAP_ENTER"
need_after Sources/ctr/hook.cpp '            if (!woken && wake != nullptr) { // (a thread counted in the gate that sleeps in slices: its slice ends now)' '                wake(arg);' "the close wakes the hub's queue at its first look, with the flag already 0"
# 4. The gate opens only once the block is mapped again: the framework's reply to Luma3DS returns then (the first statement of the wrap), OnSwapBack opens it after
#    it, and SWAP_EXIT again (idempotent). Nowhere else.
[ "$(tr -d '\r' < Sources/ctr/plugin.cpp | awk 'p { print; exit } index($0, "extern \"C\" void __wrap_PLGLDR__Reply(s32 event) {") { p = 1 }')" = '    __real_PLGLDR__Reply(event);' ] || fail "Sources/ctr/plugin.cpp: the wrap of PLGLDR__Reply does something before the framework's reply (the block is mapped only when it returns)"
OPENERS=$(grep -rn --include='*.cpp' 'OpenGate()' Sources | awk '{ f = $0; sub(/:.*$/, "", f); l = $0; sub(/^[^:]*:[0-9]*:/, "", l); sub(/\/\/.*$/, "", l); if (index(l, "OpenGate()") && !index(l, "void OpenGate()")) n[f]++ } END { for (f in n) print n[f], f }' | sort | tr '\n' ' ')
[ "$OPENERS" = "2 Sources/ctr/plugin.cpp " ] || fail "OpenGate is called in '$OPENERS', expected twice in Sources/ctr/plugin.cpp (OnSwapBack, after the framework's reply, and SWAP_EXIT)"
# 5. Every wait a game thread can make in the plugin's code (in pre, in the HLE, in post) ends when the gate closes, or is shorter than 20 ms: the swap waits
#    1.5 s for the threads counted in the gate and the block goes after it. The waits of the engine and the hub are pinned above (kGateSliceMs, the scan, Post);
#    Tests/callgraph_check.py lists every wait reachable in the built ELF, with its reason (WAITS_ALLOWED), and fails on a new one.
need_after Sources/ctr/uds_hle.cpp '    while (!hub.PollPending(cmd)) {' '        const bool closing = GateClosing();' "the wait for a join (8 s) looks at the gate at every slice"
need Sources/ctr/uds_hle.cpp '        if (closing || platform::NowMs() >= deadline) {' "... and ends at once when it closes, as at its cap"
need_line Sources/ctr/uds_hle.cpp 'constexpr s64 kDlpJoinSleepNs = 10 * 1000 * 1000;' "the join's slice is 10 ms (under the 20 ms a wait may take past the close)"
need_after Sources/ctr/uds_hle.cpp '    if (!GateClosing()) { // (not while the swap of the plugin' '        svcSleepThread(kFailPauseNs);' "the pause of a thread that keeps failing is not made while the gate closes"
need Sources/ctr/uds_hle.cpp '    constexpr s64 kFailPauseNs = 500000;' "... and it is half a millisecond"
need_after Sources/ctr/hook.cpp '    if (kind == kAptNoteFatal && !GateClosing()) {' '        svcSleepThread(50000000LL);' "the 50 ms a fatal error gives the trail are not taken while the gate closes"
need Includes/azl/dlp_hub.hpp '    static constexpr u32 kSliceMs = 100;' "the hub's queue sleeps in slices of 100 ms, which the close of the gate ends (WakeForSwap, at CloseGate's first look)"
need Tests/callgraph_check.py "    bad += check_waits(funcs, dem, quiet)" "the call graph check holds every wait a game thread can reach to the list of the guarded or short ones"
need Tests/callgraph_check.py "WAIT_PRIMITIVES = ('svcSleepThread', 'svcWaitSynchronization', 'svcWaitSynchronizationN', 'LightLock_Lock', 'RecursiveLock_Lock', 'LightEvent_Wait'," "the waits the call graph check looks for"
need Tests/callgraph_check.py "GAME_SVCS = ('svcSendSyncRequest', 'svcCloseHandle')" "... and the game's two svc, which only the gate makes"
need Tests/callgraph_mutants.py "('W1 a sleep in a helper of PassThroughPre" "the call graph's mutants hold a wait in pre"
need Tests/callgraph_mutants.py "('W2 a 30 ms sleep in the engine" "... a wait in the HLE proper, past the cut of the formatters"
need Tests/callgraph_mutants.py "('W3 the plugin makes the game" "... and the game's CloseHandle made by the plugin"
# 6. Between pre and post the context is values only (a swap during the svc skips the post: a pointer into the plugin's block would outlive it), ctx[3] is the
#    gate's, and nothing pre takes waits for post to give it back: the table of the traced sessions takes its entries in post only.
hit=$(tr -d '\r' < Sources/ctr/hook.cpp | awk -v b="$GT_BEGIN" -v e="$GT_END" '$0 ~ b { on = 1; next } $0 ~ e { on = 0; next } on { l = $0; sub(/\/\/.*$/, "", l); if (l ~ /ctx\[3\][[:space:]]*=[^=]/ || (l ~ /ctx\[[0-2]\][[:space:]]*=[^=]/ && l ~ /reinterpret_cast|&|->|\.c_str|traced[^-]/)) print FNR ": " l }' | head -2)
[ -z "$hit" ] || fail "Sources/ctr/hook.cpp: pre carries something else than a value to post in the context (or writes the gate's ctx[3]): $hit"
[ "$(grep -c 'Result PassThrough[A-Za-z]*Post(Result rc, const u32\* ctx) {' Sources/ctr/hook.cpp)" = "5" ] || fail "Sources/ctr/hook.cpp: a post that may write the context, or a sixth post (the five read ctx[0..2] as pre left them)"
for take in 'g_traced.Add(session, name, apt);' 'g_traced.Opened(session);'; do
  [ "$(tr -d '\r' < Sources/ctr/hook.cpp | code_of | grep -c -F "${take%%(*}(")" = "1" ] && region Sources/ctr/hook.cpp '__attribute__((noinline)) Result PassThroughGetServicePost(Result rc, const u32* ctx) {' '}' | grep -q -F "$take" || \
    fail "Sources/ctr/hook.cpp: '$take' is made somewhere else than in the post of GetServiceHandle (an entry taken in pre would stay taken when a swap skips the post)"
done
need_after Sources/ctr/hook.cpp '__attribute__((noinline)) Result PassThroughTracedPost(Result rc, const u32* ctx) {' '    const TracedSession* traced = FindTraced(ctx[0]);' "the traced session is looked up again after the call, by its handle"
# 7. The gate's page is never in CTRPF's and Luma's page (0x01E80000, 0x2000 bytes): ChooseAddress starts with the page's end at 0x01E80000 and only goes down
#    (the three ways it moves; Tests/gate_plan_test.cpp runs it on the maps of 36 launches and on the case of that page not mapped yet).
need Includes/azl/gate_plan.hpp 'constexpr u32 kGateCeiling = 0x01E80000;' "the gate's page ends at or below CTRPF's page"
need Includes/azl/gate_plan.hpp '    u32 top = kGateCeiling; // the highest end the page may have' "ChooseAddress starts under CTRPF's page"
need Includes/azl/gate_plan.hpp '        const u32 base = top - size;' "the page ends at top"
TOPS=$(region Includes/azl/gate_plan.hpp 'inline u32 ChooseAddress(Query query,' '}' | code_of | grep -E '^[[:space:]]*top[[:space:]]*[-+]?=' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | tr '\n' '|')
[ "$TOPS" = 'top = span.base & ~(kGatePageBytes - 1);|top = (span.base + span.size) & ~(kGatePageBytes - 1);|top = reach_top < top ? reach_top : top - kGatePageBytes;|' ] || fail "Includes/azl/gate_plan.hpp: ChooseAddress moves the page's end otherwise than down ($TOPS)"
need Tests/gate_plan_test.cpp 'never in 0x01E80000-0x01E82000' "the test of the place with CTRPF's page not mapped yet"
need Sources/ctr/gate_page.cpp 'const u32 address = ChooseAddress(QuerySpan, min_site, max_site, bytes, &place);' "the console takes the place the tested ChooseAddress gives"
# 8. An Old 3DS has no alias of the plugin's memory (the swap takes it away under a thread asleep in the gate: the defect itself). Four ways out of
#    ChooseGateWay (azl/gate_plan.hpp, Tests/gate_plan_test.cpp): Azahar the alias; from Luma3DS 13.0 svc 0xA3 on an Old AND a New 3DS (an alias is
#    out of the kernel's books while the game's process outlives the plugin's block: an application started then that sizes its memory to the byte
#    fails at its start, 3DS Link on a New 3DS XL, 1.1.2); before 13.0 the alias on a New 3DS (no swap there; svc 0xA3 may not be there) and nothing on
#    an Old 3DS. A SYSTEM page that failed has NO fallback, on an Old and a New 3DS alike: no hooks for that launch (on a New 3DS the alias is never unmapped
#    and outlives the game: "Cannot map plugin memory" at every start until a reboot, console 63, 2026-11-06; the alias only where ChooseGateWay gives it).
#    The SYSTEM page goes on the kernel's books at once, after svc 0xA3 and before anything else (no game thread runs yet), and the limit comes back
#    down only once the game has its heap (LateInit), except on a New 3DS in memtype 6, where pm puts it back at the end of every application.
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'return Alias(')" = "1" ] || fail "Sources/ctr/gate_page.cpp: the alias of the plugin's memory is taken in another case than the way ChooseGateWay gives (a fallback on it outlives the game)"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'MapProcessMemoryEx5(')" = "2" ] || fail "Sources/ctr/gate_page.cpp: the plugin's memory is mapped outside Alias"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'Alias(')" = "2" ] || fail "Sources/ctr/gate_page.cpp: Alias is called elsewhere than in the way ChooseGateWay gives (a fallback on it outlives the game)"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'IsNew3DS')" = "2" ] || fail "Sources/ctr/gate_page.cpp: the New 3DS is asked of the framework elsewhere than for is_new and SettleGateBooks"
region Sources/ctr/gate_page.cpp '    const char* fail = nullptr;' '}' | code_of | grep -q -E 'Alias\(|MapProcessMemory|IsNew3DS' && fail "Sources/ctr/gate_page.cpp: a branch of the SYSTEM page's attempt takes the alias or tells a New 3DS apart"
need_line Sources/ctr/gate_page.cpp '    const bool is_new = CTRPluginFramework::System::IsNew3DS();' "the New 3DS from the framework"
need Sources/ctr/gate_page.cpp '    const GateWay way = ChooseGateWay(CTRPluginFramework::System::IsCitra(), is_new, luma_version, &reason);' "the way from the framework's Azahar and New 3DS and from Luma3DS's version"
#    no fallback: is_new only for ChooseGateWay, SettleNeeded and the drift (six lines: its definition, ChooseGateWay, SettleNeeded, SettleDrift's call,
#    SettleDrift's parameter and its DriftReference), never a test of its own; the failure's branch is the same for an Old and a New 3DS, with no alias
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'is_new')" = "6" ] || fail "Sources/ctr/gate_page.cpp: is_new is used otherwise than for ChooseGateWay, SettleNeeded and the drift's reference (a fallback of a New 3DS?)"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c -E 'if \(!?is_new')" = "0" ] || fail "Sources/ctr/gate_page.cpp: a test of is_new in TakeGatePage (the failure of the SYSTEM page is the same on an Old and a New 3DS: no hooks)"
FAIL_BRANCH=$(region Sources/ctr/gate_page.cpp '    // No SYSTEM page, and no RaiseBooks' '}' | code_of)
[ -n "$FAIL_BRANCH" ] || fail "Sources/ctr/gate_page.cpp: the branch of a failed SYSTEM page is no longer found"
printf '%s\n' "$FAIL_BRANCH" | grep -q -E 'is_new|Alias|MapProcessMemory|IsNew3DS' && fail "Sources/ctr/gate_page.cpp: the branch of a failed SYSTEM page takes the alias or tells a New 3DS apart (an alias outlives the game)"
printf '%s\n' "$FAIL_BRANCH" | grep -q -x -F '    *why = fail;' || fail "Sources/ctr/gate_page.cpp: a failed SYSTEM page does not say why on the screen"
printf '%s\n' "$FAIL_BRANCH" | grep -q -x -F '    return GatePage{};' || fail "Sources/ctr/gate_page.cpp: a failed SYSTEM page does not end with no page"
grep -rn -F 'kSystemFallbackReason' Sources Includes Tests/gate_plan_test.cpp >/dev/null && fail "the reason of the alias of a New 3DS whose SYSTEM page failed is back (there is no such fallback)"
need Tests/gate_plan_test.cpp '    const char* reasons[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};' "the plan's test tells the reasons apart: Azahar, a New and an Old 3DS before and from Luma3DS 13.0"
need Tests/gate_plan_test.cpp '            CHECK(reasons[i] != nullptr && reasons[j] != nullptr && std::strcmp(reasons[i], reasons[j]) != 0);' "... two by two"
need Tests/gate_plan_test.cpp '        for (int j = i + 1; j < 5; ++j) {' "... every pair of the five"
need_after Sources/ctr/gate_page.cpp '    *why = fail;' '    Log("gate: no hooks: SYSTEM page failed: %s (0x%08lx, %lu bytes at 0x%08lx): no other page is taken (never the alias)", fail,' "... and says so in the log, with the words the sheet of the test looks for"
need_order Sources/ctr/gate_page.cpp '    if (!FindBooks()) {' '    *why = fail;' "... only once the SYSTEM page was tried"
need_order Sources/ctr/gate_page.cpp '        } else if (!RaiseBooks(bytes, &rc)) {' '    *why = fail;' "... after the one branch that raises the books"
region Sources/ctr/gate_page.cpp '            page.books_raised = true;' '        }' | code_of | grep -q -x '            return page;' || fail "Sources/ctr/gate_page.cpp: the branch that raised the books goes on to the failure's branch (no hooks with the books raised by a page that is not the gate's)"
#    A Raise the kernel refuses (rc: its result, in the line of the failure) frees the page again with svc 0xA3, which leaves the books alone, and goes on to
#    no hooks like any other failure there: never a SYSTEM page off the books (the next game's budget lost at this one's end, Tests/kernel_books_test.cpp TestRaiseRefused).
RAISE_FAIL=$(region Sources/ctr/gate_page.cpp '        } else if (!RaiseBooks(bytes, &rc)) {' '        } else {' | code_of)
printf '%s\n' "$RAISE_FAIL" | grep -q -x -F '            const Result freed = svcControlMemoryUnsafe(&ignored, address, bytes, MEMOP_FREE, static_cast<MemPerm>(0));' || \
  fail "Sources/ctr/gate_page.cpp: a page whose Raise failed is not freed with svc 0xA3 (a SYSTEM page left off the books)"
printf '%s\n' "$RAISE_FAIL" | grep -q -E 'return|books_raised|g_settle_owed|GatePageKind::System|RaiseBooks\(bytes[^)]*\)\)? *;' && fail "Sources/ctr/gate_page.cpp: the branch of a failed Raise keeps the page, owes a Settle or raises again"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'books_raised = ')" = "1" ] || fail "Sources/ctr/gate_page.cpp: books_raised is set elsewhere than in the branch whose RaiseBooks was made"
need_after Sources/ctr/gate_page.cpp '        } else {' '            page.books_raised = true;' "the branch after a RaiseBooks that was made is the one that keeps the page"
need Sources/ctr/gate_page.cpp '    if (R_FAILED(svcGetSystemInfo(&luma, 0x10000, 0))) {' "... Luma3DS's version from Luma3DS itself (0 without it)"
need Sources/ctr/gate_page.cpp '    const u32 luma_version = static_cast<u32>(luma);' "... and that is the version the way is chosen with"
need_after Sources/ctr/gate_page.cpp '    if (way == GateWay::Alias) {' '        return Alias(address, bytes, reason, why);' "the alias only where ChooseGateWay gives it"
region Sources/ctr/gate_page.cpp '    if (way == GateWay::None) {' '    }' | code_of | grep -q '        return page;' || fail "Sources/ctr/gate_page.cpp: an Old 3DS with a Luma3DS before 13.0 goes on to svc 0xA3"
need_order Sources/ctr/gate_page.cpp '    if (way == GateWay::None) {' '    if (!FindBooks()) {' "... and the books and svc 0xA3 come after that test"
need_order Sources/ctr/gate_page.cpp '    if (!FindBooks()) {' '    return GatePage{};' "an Old or New 3DS where the SYSTEM page failed has no page and no hooks"
need_line Includes/azl/gate_plan.hpp 'constexpr u32 kLumaWithControlMemoryUnsafe = 13u << 24;' "svc 0xA3 from Luma3DS 13.0 (its plugin loader takes the block of an Old 3DS with it)"
need_after Includes/azl/gate_plan.hpp '    if (citra) {' '        return GateWay::Alias;' "Azahar takes the alias"
need_after Includes/azl/gate_plan.hpp '    if (luma_version < kLumaWithControlMemoryUnsafe) {' '        if (is_new) {' "before Luma3DS 13.0 the New 3DS is asked first"
need_after Includes/azl/gate_plan.hpp '        if (is_new) {' '            return GateWay::Alias;' "... and a New 3DS before Luma3DS 13.0 takes the alias"
WAYS=$(region Includes/azl/gate_plan.hpp 'inline GateWay ChooseGateWay(' '}' | code_of | grep -o -E 'if \([^{]*\) \{|return GateWay::[A-Za-z]+;' | tr '\n' '|')
[ "$WAYS" = 'if (citra) {|return GateWay::Alias;|if (luma_version < kLumaWithControlMemoryUnsafe) {|if (is_new) {|return GateWay::Alias;|return GateWay::None;|return GateWay::System;|' ] || \
  fail "Includes/azl/gate_plan.hpp: ChooseGateWay has other ways out than its four (Azahar the alias; before Luma3DS 13.0 a New 3DS the alias and an Old 3DS nothing; then the SYSTEM page, Old and New): $WAYS"
need_stmt Tests/gate_plan_test.cpp 'TestChooseGateWay();' "the plan's test runs the way of every console and Luma3DS"
need_order Sources/ctr/gate_page.cpp '    if (!FindBooks()) {' '        rc = svcControlMemoryUnsafe(&out, address, bytes, static_cast<MemOp>(MEMOP_ALLOC | MEMOP_REGION_SYSTEM),' "no SYSTEM page without the kernel's books found"
need_after Sources/ctr/gate_page.cpp '            fail = "the page is not where it was asked";' '        } else if (!RaiseBooks(bytes, &rc)) {' "the books are raised right after the page is taken and found where it was asked"
need_order Sources/ctr/gate_page.cpp '        rc = svcControlMemoryUnsafe(&out, address, bytes,' '        } else if (!RaiseBooks(bytes, &rc)) {' "... after svc 0xA3"
need_order Sources/ctr/gate_page.cpp '        } else if (!RaiseBooks(bytes, &rc)) {' '            page.kind = GatePageKind::System;' "... before the page is the gate's"
need_after Sources/ctr/gate_page.cpp 'void SettleGateBooks(const GatePage& page) {' '    if (page.kind != GatePageKind::System || !page.books_raised) {' "only a SYSTEM page whose books were raised is settled"
#    ... and not on a New 3DS in memtype 6 (pm resets the limit at the end of every application): out before WaitForGameHeap and SettleBooks
need_line Includes/azl/books_math.hpp 'inline bool SettleNeeded(bool is_new, u32 app_memtype) {' "who owes the Settle, in the tested books_math.hpp"
need_line Includes/azl/books_math.hpp '    return !(is_new && app_memtype == 6);' "... everyone but a New 3DS in memtype 6"
need_line Sources/ctr/gate_page.cpp '    if (!books::SettleNeeded(CTRPluginFramework::System::IsNew3DS(), OS_KernelConfig->app_memtype)) {' "SettleGateBooks asks SettleNeeded with the framework's New 3DS and the kernel's app_memtype"
region Sources/ctr/gate_page.cpp '    if (!books::SettleNeeded(' '    }' | code_of | grep -q -x '        return;' || fail "Sources/ctr/gate_page.cpp: a New 3DS in memtype 6 settles the books all the same"
need_order Sources/ctr/gate_page.cpp '    if (page.kind != GatePageKind::System || !page.books_raised) {' '    if (!books::SettleNeeded(' "... asked in SettleGateBooks"
need_order Sources/ctr/gate_page.cpp '    if (!books::SettleNeeded(' '    if (!WaitForGameHeap()) {' "... before the wait for the game's heap"
need_order Sources/ctr/gate_page.cpp '    if (!books::SettleNeeded(' '    const bool settled = SettleBooks(owed);' "... and before the Settle"
need_stmt Tests/kernel_books_test.cpp 'TestSettleNeeded();' "the books' test asks SettleNeeded of every console and memtype"
need_stmt Tests/kernel_books_test.cpp 'TestNewMemtype6RaiseOnly();' "... and plays a New 3DS in memtype 6 with Raise only"
need_order Sources/ctr/gate_page.cpp '    if (!WaitForGameHeap()) {' '    const u32 owed = books::TakeOwed(&g_settle_owed);' "the limit comes down only once the game has its heap"
need_after Sources/ctr/gate_page.cpp '    const u32 owed = books::TakeOwed(&g_settle_owed);' '    if (owed == 0) {' "... and only with the bytes still owed (the EXIT event may have settled them)"
region Sources/ctr/gate_page.cpp '    if (owed == 0) {' '    }' | code_of | grep -q -x '        return;' || fail "Sources/ctr/gate_page.cpp: SettleGateBooks settles what the EXIT event has settled already"
need_order Sources/ctr/gate_page.cpp '    if (owed == 0) {' '    const bool settled = SettleBooks(owed);' "... and settles what it took"
region Sources/ctr/gate_page.cpp '    if (!WaitForGameHeap()) {' '    }' | code_of | grep -q '        return;' || fail "Sources/ctr/gate_page.cpp: with no heap of the game seen the books are settled all the same"
#    On an Old 3DS (and a New 3DS in memtype 7) the limit of the applications outlives the process (pm writes it only at boot): a launch that ended between RaiseBooks and its
#    Settle with no EXIT event (killed while the plugin is swapped out, a crash) left it raised until a reboot. The next launch takes that drift back before it
#    raises the books (books::Drift against books::DriftReference, the limit of boot: the region's size on a New 3DS, where Rosalina has taken the plugin's block
#    off APPMEMALLOC, APPMEMALLOC on an Old 3DS; at most 1 MiB; Tests/kernel_books_test.cpp). Not on a New 3DS in memtype 6, where pm puts the limit back itself.
need_order Sources/ctr/gate_page.cpp '    if (!FindBooks()) {' '        const bool settle_needed = books::SettleNeeded(is_new, OS_KernelConfig->app_memtype);' "who owes the Settle is asked once the books are found"
need_after Sources/ctr/gate_page.cpp '        const bool settle_needed = books::SettleNeeded(is_new, OS_KernelConfig->app_memtype);' '        if (settle_needed) {' "... and the drift is taken back only where Settle is owed"
need_after Sources/ctr/gate_page.cpp '        if (settle_needed) {' '            SettleDrift(is_new); // (before this launch raises the books: what an earlier one left there)' "an Old 3DS (a New 3DS in memtype 7) takes back the drift of the limit once the books are found"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'SettleDrift(is_new);')" = "1" ] || fail "Sources/ctr/gate_page.cpp: SettleDrift is called elsewhere than under settle_needed"
need_order Sources/ctr/gate_page.cpp '            SettleDrift(is_new); // (before this launch raises the books: what an earlier one left there)' '        rc = svcControlMemoryUnsafe(&out, address, bytes,' "... before its own page and its own raise"
need Sources/ctr/gate_page.cpp '    const u32 appmemalloc = static_cast<u32>(osGetMemRegionSize(MEMREGION_APPLICATION)); // (APPMEMALLOC, 0x1FF80040)' "APPMEMALLOC, the limit pm set at boot on an Old 3DS"
need Sources/ctr/gate_page.cpp '    if (R_FAILED(svcGetSystemInfo(&region, 0x10000, 0x80)) || region <= 0 || region >= 0x40000000) {' "the region's size from Luma3DS (0 when it does not answer)"
need Sources/ctr/gate_page.cpp '    const u32 reference = books::DriftReference(is_new, static_cast<u32>(region), appmemalloc);' "the reference of the drift by the tested books::DriftReference"
need Sources/ctr/gate_page.cpp '    const s32 drift = books::Drift(limit, reference);' "... and the drift by the tested books::Drift"
need_line Includes/azl/books_math.hpp '    return is_new && region_size != 0 ? region_size : appmemalloc;' "the region's size on a New 3DS (APPMEMALLOC there is short of the plugin's block), APPMEMALLOC on an Old 3DS or without an answer"
need_stmt Tests/kernel_books_test.cpp 'TestDriftReference();' "the books' test measures the drift of a New 3DS in memtype 7 against the region, and an Old 3DS against APPMEMALLOC"
#    The Settle is owed once (g_settle_owed, set in TakeGatePage once the books are raised and Settle is owed): LateInit, SWAP_ENTER or the EXIT event, whichever comes
#    first, takes it with one exchange (books::TakeOwed). The EXIT event (the framework's keep-alive thread, the game in svcExitProcess and alive) settles a game closed
#    before LateInit, and on an Old 3DS one closed from the HOME menu too (Luma3DS maps the plugin's block back, then sends EXIT: k11_extension SendSyncRequest.c,
#    plgloader.c); SWAP_ENTER (an Old 3DS, HOME) is the net for an end with no EXIT event (a process killed, a block that does not come back), but only
#    once the game's heap is seen (books::TakeOwedOnceHeapSeen: before it, a lower limit takes a page off the room the game sizes its heap with). The keep-alive thread
#    answers Luma3DS: it never waits for the lock of the books (TrySettleOwedBooks: LightLock_TryLock, the bytes taken only under it; busy, they stay owed).
need_line Includes/azl/books_math.hpp '    return __atomic_exchange_n(owed, 0u, __ATOMIC_SEQ_CST);' "what Settle owes is taken with one exchange: never twice"
need_line Sources/ctr/gate_page.cpp '            __atomic_store_n(&g_settle_owed, settle_needed ? bytes : 0u, __ATOMIC_SEQ_CST);' "Settle is owed only where pm does not put the limit back"
need_order Sources/ctr/gate_page.cpp '            page.books_raised = true;' '            __atomic_store_n(&g_settle_owed,' "... after the raise that was made"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'g_settle_owed')" = "6" ] || fail "Sources/ctr/gate_page.cpp: g_settle_owed is touched otherwise than by its definition, TakeGatePage's store, LateInit's TakeOwed, the EXIT's and SWAP_ENTER's TrySettleOwedBooks and SWAP_ENTER's first look"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'books::TakeOwed(&g_settle_owed);')" = "1" ] || fail "Sources/ctr/gate_page.cpp: the bytes owed are taken otherwise than by SettleGateBooks (LateInit) and TrySettleOwedBooks (the keep-alive thread)"
[ "$(tr -d '\r' < Sources/ctr/gate_page.cpp | code_of | grep -c 'TrySettleOwedBooks(&g_settle_owed, ')" = "2" ] || fail "Sources/ctr/gate_page.cpp: the keep-alive thread settles otherwise than at the EXIT and at SWAP_ENTER"
need_after Sources/ctr/gate_page.cpp 'void SettleGateBooksAtExit() {' '    const OwedSettle result = TrySettleOwedBooks(&g_settle_owed, true, &bytes);' "the EXIT event settles what is still owed, whatever the heap (nothing sizes one any more)"
need_after Sources/ctr/gate_page.cpp 'void SettleGateBooksAtSwap() {' '    if (__atomic_load_n(&g_settle_owed, __ATOMIC_SEQ_CST) == 0) {' "SWAP_ENTER: one load at every HOME once the Settle is made"
need_after Sources/ctr/gate_page.cpp '    if (__atomic_load_n(&g_settle_owed, __ATOMIC_SEQ_CST) == 0) {' '        return;' "... and nothing more then"
need_line Sources/ctr/gate_page.cpp '    const OwedSettle result = TrySettleOwedBooks(&g_settle_owed, GameHeapSeen(), &bytes);' "SWAP_ENTER settles only once the game's heap is seen"
for fn in 'void SettleGateBooksAtExit() {' 'void SettleGateBooksAtSwap() {'; do
  region Sources/ctr/gate_page.cpp "$fn" '}' | code_of | grep -q -E 'Log\(|WaitForGameHeap|ReadBooks|SettleBooks\(|svcSleepThread|LightLock_Lock' && fail "Sources/ctr/gate_page.cpp: $fn writes a line or waits on the keep-alive thread"
done
need_line Includes/azl/books_math.hpp '    return heap_seen ? TakeOwed(owed) : 0u;' "the bytes owed are taken at SWAP_ENTER only once the game's heap is there"
body=$(region Sources/ctr/kernel_books.cpp 'OwedSettle TrySettleOwedBooks(u32* owed, bool heap_seen, u32* bytes) {' '}' | code_of | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$' | sed -n '2,19p' | tr '\n' '|')
[ "$body" = '*bytes = 0;|if (__atomic_load_n(owed, __ATOMIC_SEQ_CST) == 0) {|return OwedSettle::Nothing;|}|if (LightLock_TryLock(&g_books_lock) != 0) {|return OwedSettle::Busy;|}|const u32 take = books::TakeOwedOnceHeapSeen(owed, heap_seen);|if (take != 0) {|g_books.add = static_cast<s32>(take);|svcCustomBackdoor(reinterpret_cast<void*>(KernelSettleBooks));|}|LightLock_Unlock(&g_books_lock);|*bytes = take;|return take != 0 ? OwedSettle::Settled : heap_seen ? OwedSettle::Nothing : OwedSettle::NoHeap;|}|' ] || \
  fail "Sources/ctr/kernel_books.cpp: TrySettleOwedBooks no longer only TRIES the lock of the books, takes the bytes owed under it (once the heap is seen) and settles them ($body)"
need_after Sources/ctr/plugin.cpp '        if (event == kEventExit) {' '            SettleGateBooksAtExit();' "the EXIT event settles the books of the gate's page the game did not live to settle"
need_order Sources/ctr/plugin.cpp 'void Plugin::NoteProcessEvent(int event) {' '            SettleGateBooksAtExit();' "... in the framework's event, before its answer to Luma3DS"
[ "$(grep -rn --include='*.cpp' 'SettleGateBooksAtExit()' Sources | sed 's://.*$::' | grep -c 'SettleGateBooksAtExit()')" = "2" ] || fail "SettleGateBooksAtExit is called elsewhere than at the EXIT event (its definition and that one call)"
need_order Sources/ctr/plugin.cpp '        if (event == kEventSwapEnter) {' '            SettleGateBooksAtSwap();' "SWAP_ENTER settles the books of a game that may be closed from the HOME menu"
need_order Sources/ctr/plugin.cpp '            SettleGateBooksAtSwap();' '            const u32 gate_left = CloseGate(kSwapDrainTimeoutMs,' "... in the framework's event, before its answer to Luma3DS"
need_order Sources/ctr/plugin.cpp '            SettleGateBooksAtSwap();' '        } else if (event == kEventSwapExit) {' "... at SWAP_ENTER only"
[ "$(grep -rn --include='*.cpp' 'SettleGateBooksAtSwap()' Sources | sed 's://.*$::' | grep -c 'SettleGateBooksAtSwap()')" = "2" ] || fail "SettleGateBooksAtSwap is called elsewhere than at SWAP_ENTER (its definition and that one call)"
need_order Sources/ctr/plugin.cpp 'void Plugin::Shutdown() {' '    LogGateBooksAtEvents(); // (what the EXIT event, or the last SWAP_ENTER, did with the books, if anything)' "the shutdown says what the EXIT event did with the books"
need_after Sources/ctr/plugin.cpp '            if (i == kEventSwapEnter) {' '                LogGateBooksAtEvents(); // (what SWAP_ENTER did with the books, if anything)' "... and the writer what SWAP_ENTER did"
need_stmt Tests/kernel_books_test.cpp 'TestDeathBeforeSettleWithExit();' "the books' test plays a game closed before LateInit, settled at its EXIT, and the claim taken once"
need_stmt Tests/kernel_books_test.cpp 'TestOldClosedFromHome();' "... an Old 3DS closed from the HOME menu, settled at SWAP_ENTER after the heap, left owed before it"
need_stmt Tests/kernel_books_test.cpp 'TestRaiseWhileTheGameBeforeEnds();' "... the Release of the application before between any two calls of the Raise"
need_stmt Tests/kernel_books_test.cpp 'TestRaiseRefused();' "... and a page whose Raise failed, freed"
region Sources/ctr/gate_page.cpp '    if (drift != 0) {' '    }' | code_of | grep -q 'SettleBooks(static_cast<u32>(drift));' || fail "Sources/ctr/gate_page.cpp: the drift is not taken back with SettleBooks"
need_line Includes/azl/books_math.hpp 'constexpr s32 kMaxDrift = 1024 * 1024;' "the drift taken back is at most 1 MiB, under the 2 MiB of the smallest block MODE3 takes off APPMEMALLOC"
need Includes/azl/books_math.hpp '    if (diff <= 0 || diff > kMaxDrift || diff % kDriftUnit != 0) {' "only a whole number of pages above APPMEMALLOC is a drift"
need_stmt Tests/kernel_books_test.cpp 'TestDriftOfLaunchesThatDied();' "the books' test plays launches that die before the settle and the next one that takes the drift back"
need_stmt Tests/kernel_books_test.cpp 'TestDriftBounds();' "... and what is not a drift"
#    The KResourceLimit's mutex is never taken by the plugin (AV1, 2026-11-06: the kernel changes current[] under the object's KLightMutex, and a mutex taken
#    from outside and let go with a plain store leaves a waiter of the other core asleep, KLightMutex::UnlockImpl's interrupt missing). current[] moves through
#    the kernel's own calls first (books_math.hpp, Tests/kernel_books_test.cpp): current += X is svcControlMemory's ALLOC (the kernel reserves X, Kernel11
#    ControlMemory) freed with svc 0xA3 (Luma3DS's FREE releases nothing); current -= X is svc 0xA3's ALLOC freed with svcControlMemory's FREE (the kernel
#    releases X). Only limits[kCommit] is written in kernel mode, with the interrupts off (Luma3DS's svcHandler turns them on before the backdoor's function):
#    the kernel writes limits[] only in SetLimitValue, which pm calls at boot and on a New 3DS in memtype 6, never while a game runs; Reserve reads it once.
#    Raise lifts the limit, then reserves (refused: the limit back); Charge lifts it around the reservation only (written before it, back down after it).
#    Only when the kernel answers that it has no pages (0xD86007F3, books::kNoPages: Monster Hunter XX on an Old 3DS holds the whole region, and the kernel's
#    way alone left it with no socket block, no network) do Charge and Uncharge write current[kCommit] themselves, after the kernel's try: KernelChargeBooks,
#    load-add-store with the interrupts off, the way of 1.0.13-1.1.1 (2026-11-06, the regression of the first AV1 fix). Never for another answer, never first.
need Sources/ctr/kernel_books.cpp '    asm volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr)::"memory");' "the interrupts go off in kernel mode"
need Sources/ctr/kernel_books.cpp '    asm volatile("msr cpsr_c, %0" ::"r"(cpsr) : "memory");' "... and come back as they were"
body=$(region Sources/ctr/kernel_books.cpp "__attribute__((noinline)) s32 KernelSettleBooks() {" '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | sed -n '2,6p' | tr '\n' '|')
[ "$body" = 'const u32 cpsr = InterruptsOff();|books::Settle(g_books.limits, g_books.add);|InterruptsBack(cpsr);|return 0;|}|' ] || \
  fail "Sources/ctr/kernel_books.cpp: KernelSettleBooks writes more than limits[] or with the interrupts on ($body)"
body=$(region Sources/ctr/kernel_books.cpp "__attribute__((noinline)) s32 KernelLiftBooks() {" '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | sed -n '2,6p' | tr '\n' '|')
[ "$body" = 'const u32 cpsr = InterruptsOff();|books::Lift(g_books.limits, g_books.add);|InterruptsBack(cpsr);|return 0;|}|' ] || \
  fail "Sources/ctr/kernel_books.cpp: KernelLiftBooks writes more than limits[] or with the interrupts on ($body)"
for fn in 'inline void Lift(volatile s32* limits, s32 bytes) {:limits[kCommit] += bytes;' 'inline void Settle(volatile s32* limits, s32 bytes) {:limits[kCommit] -= bytes;'; do
  body=$(region Includes/azl/books_math.hpp "${fn%%:*}" '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | sed -n '2,$p' | tr '\n' '|')
  [ "$body" = "${fn#*:}|}|" ] || fail "Includes/azl/books_math.hpp: ${fn%%(*} writes more than the commit's limit, or writes it otherwise ($body)"
done
if cat Includes/azl/books_math.hpp Sources/ctr/kernel_books.cpp | tr -d '\r' | code_of | grep -n -E 'ldrex|strex|clrex|kMutexOffset|kMutexWaitersOffset|0xFFFF9000|WriteUnderMutex|TryTakeMutex|g_books\.mutex'; then
  fail "the plugin takes the KResourceLimit's mutex itself: never (AV1)"
fi
[ "$(cat Includes/azl/books_math.hpp Sources/ctr/kernel_books.cpp | tr -d '\r' | code_of | grep -c -E 'current\[[^]]*\] *(\+|-)?= ')" = "1" ] || \
  fail "current[] of the KResourceLimit is written elsewhere than in books::Charge (the fallback when the kernel has no pages)"
body=$(region Includes/azl/books_math.hpp 'inline void Charge(volatile s32* current, s32 bytes) {' '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | sed -n '2,$p' | tr '\n' '|')
[ "$body" = 'current[kCommit] += bytes;|}|' ] || fail "Includes/azl/books_math.hpp: Charge writes more than the commit's current, or writes it otherwise ($body)"
body=$(region Sources/ctr/kernel_books.cpp "__attribute__((noinline)) s32 KernelChargeBooks() {" '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | sed -n '2,6p' | tr '\n' '|')
[ "$body" = 'const u32 cpsr = InterruptsOff();|books::Charge(g_books.current, g_books.add);|InterruptsBack(cpsr);|return 0;|}|' ] || \
  fail "Sources/ctr/kernel_books.cpp: KernelChargeBooks writes more than current[kCommit] or with the interrupts on ($body)"
[ "$(tr -d '\r' < Sources/ctr/kernel_books.cpp | code_of | grep -c 'KernelChargeBooks')" = "2" ] || fail "Sources/ctr/kernel_books.cpp: current[] is written otherwise than through ConsoleKernel::WriteCurrent"
[ "$(grep -rn --include='*.cpp' --include='*.hpp' 'books::Charge(' Sources Includes | sed 's://.*$::' | grep -c 'books::Charge(')" = "1" ] || fail "books::Charge is called elsewhere than in KernelChargeBooks"
[ "$(tr -d '\r' < Includes/azl/books_math.hpp | code_of | grep -c 'WriteCurrent(')" = "2" ] || fail "Includes/azl/books_math.hpp: current[] is written by other steps than ChargeOrWrite and UnchargeOrWrite"
[ "$(grep -rn --include='*.cpp' 'WriteCurrent(' Sources | sed 's://.*$::' | grep -c 'WriteCurrent(')" = "1" ] || fail "WriteCurrent is called from the console's code itself (only books::ChargeOrWrite and UnchargeOrWrite may)"
[ "$(tr -d '\r' < Sources/ctr/kernel_books.cpp | code_of | grep -c 'KernelLiftBooks')" = "2" ] || fail "Sources/ctr/kernel_books.cpp: the limit is lifted otherwise than through ConsoleKernel::Lift"
#    The kernel's calls of the steps, on the console (ConsoleKernel): which svc moves the books and which does not
body=$(region Sources/ctr/kernel_books.cpp 'struct ConsoleKernel {' '};' | code_of | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$' | tr '\n' '|')
[ "$body" = 'struct ConsoleKernel {|void Read(s32* limit, s32* current) {|svcCustomBackdoor(reinterpret_cast<void*>(KernelReadBooks));|*limit = g_books.read_limits[0];|*current = g_books.read_commit;|}|static void MoveLimit(s32 (*write)(), u32 bytes) {|g_books.add = static_cast<s32>(bytes);|svcCustomBackdoor(reinterpret_cast<void*>(write));|}|void Lift(u32 bytes) {|MoveLimit(KernelLiftBooks, bytes);|}|void Lower(u32 bytes) {|MoveLimit(KernelSettleBooks, bytes);|}|s32 AllocCharged(u32 bytes, u32* address) {|return svcControlMemory(address, 0, 0, bytes, MEMOP_ALLOC_LINEAR, static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));|}|s32 FreeUncharged(u32 address, u32 bytes) {|u32 ignored = 0;|return svcControlMemoryUnsafe(&ignored, address, bytes, MEMOP_FREE, static_cast<MemPerm>(0));|}|s32 AllocUncharged(u32 address, u32 bytes, books::Region region) {|const u32 from = region == books::Region::System        ? MEMOP_REGION_SYSTEM|: region == books::Region::Application ? MEMOP_REGION_APP|: MEMOP_REGION_BASE;|u32 out = 0;|return svcControlMemoryUnsafe(&out, address, bytes, static_cast<MemOp>(MEMOP_ALLOC | from),|static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));|}|s32 FreeCharged(u32 address, u32 bytes) {|u32 ignored = 0;|return svcControlMemory(&ignored, address, 0, bytes, MEMOP_FREE, static_cast<MemPerm>(0));|}|u32 FreeSpan(u32 bytes) {|return FreeHeapSpan(bytes);|}|void WriteCurrent(s32 bytes) {|g_books.add = bytes;|svcCustomBackdoor(reinterpret_cast<void*>(KernelChargeBooks));|}|};|' ] || \
  fail "Sources/ctr/kernel_books.cpp: ConsoleKernel no longer reads the books with KernelReadBooks, reserves with svcControlMemory's linear ALLOC at an address the kernel chooses, frees that with svc 0xA3 (no Release), allocates with svc 0xA3 in the region it is given (SYSTEM, APPLICATION, BASE) and releases with svcControlMemory's FREE, or writes current[] otherwise than with KernelChargeBooks ($body)"
#    The steps themselves (Includes/azl/books_math.hpp, run by Tests/kernel_books_test.cpp)
body=$(region Includes/azl/books_math.hpp 's32 ReserveThroughKernel(Kernel& kernel, u32 bytes) {' '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | tr '\n' '|')
[ "$body" = 's32 ReserveThroughKernel(Kernel& kernel, u32 bytes) {|u32 address = 0;|const s32 rc = kernel.AllocCharged(bytes, &address);|if (rc < 0) {|return rc;|}|const s32 freed = kernel.FreeUncharged(address, bytes);|if (freed < 0) {|kernel.FreeCharged(address, bytes);|return freed;|}|return 0;|}|' ] || \
  fail "Includes/azl/books_math.hpp: ReserveThroughKernel no longer reserves through the kernel's ALLOC and frees the block with svc 0xA3, svcControlMemory only when svc 0xA3 could not ($body)"
body=$(region Includes/azl/books_math.hpp 's32 ReleaseThroughKernel(Kernel& kernel, u32 bytes, u32* released) {' '}' | code_of | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | grep -v '^$' | tr '\n' '|')
[ "$body" = 's32 ReleaseThroughKernel(Kernel& kernel, u32 bytes, u32* released) {|*released = 0;|u32 piece = bytes;|int region = 0;|while (*released < bytes) {|if (piece > bytes - *released) {|piece = bytes - *released;|}|const u32 address = kernel.FreeSpan(piece);|const s32 rc = address == 0 ? kNoFreeSpan : kernel.AllocUncharged(address, piece, kReleaseRegions[region]);|if (rc < 0) {|if (piece > kPageBytes) {|piece = kPageBytes;|continue;|}|if (rc == kNoPages && region + 1 < kReleaseRegionCount) {|++region;|continue;|}|return rc;|}|const s32 freed = kernel.FreeCharged(address, piece);|if (freed < 0) {|kernel.FreeUncharged(address, piece);|return freed;|}|*released += piece;|}|return 0;|}|' ] || \
  fail "Includes/azl/books_math.hpp: ReleaseThroughKernel no longer takes svc 0xA3's block and frees it with svcControlMemory (whole in SYSTEM, then a page at a time, on to the next region only when one has no page) ($body)"
need_line Includes/azl/books_math.hpp 'constexpr Region kReleaseRegions[kReleaseRegionCount] = {Region::System, Region::Application, Region::Base};' "the Release's block from SYSTEM first, then APPLICATION, then BASE (AV1-R2: a SYSTEM region with no page left)"
need_line Includes/azl/books_math.hpp 'constexpr s32 kNoPages = static_cast<s32>(0xD86007F3);' "the one answer of the kernel that lets the books be written: no physical page left in the region"
body=$(region Includes/azl/books_math.hpp 's32 RaiseThroughKernel(Kernel& kernel, u32 bytes) {' '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | tr '\n' '|')
[ "$body" = 's32 RaiseThroughKernel(Kernel& kernel, u32 bytes) {|kernel.Lift(bytes);|const s32 rc = ReserveThroughKernel(kernel, bytes);|if (rc != 0) {|kernel.Lower(bytes);|}|return rc;|}|' ] || \
  fail "Includes/azl/books_math.hpp: RaiseThroughKernel no longer lifts the limit before the reservation and puts it back when the kernel refuses ($body)"
body=$(region Includes/azl/books_math.hpp 's32 ChargeThroughKernel(Kernel& kernel, u32 bytes) {' '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | tr '\n' '|')
[ "$body" = 's32 ChargeThroughKernel(Kernel& kernel, u32 bytes) {|s32 limit = 0;|s32 current = 0;|kernel.Read(&limit, &current);|const u32 lift = ChargeLift(limit, current, bytes);|kernel.Lift(lift);|const s32 rc = ReserveThroughKernel(kernel, bytes);|kernel.Lower(lift);|return rc;|}|' ] || \
  fail "Includes/azl/books_math.hpp: ChargeThroughKernel no longer reads the books and lifts the limit by ChargeLift around the reservation only, back down by the same ($body)"
#    The lift of a Charge covers what current already stands above the limit (2026-11-06, AVV-1): a game sized on limit - current with the gate's page settled after it
#    stands above the limit by the page, and a lift of the block alone would be refused above the limit (0xC860180A) before the kernel looks for pages: no fallback.
body=$(region Includes/azl/books_math.hpp 'inline u32 ChargeLift(s32 limit, s32 current, u32 bytes) {' '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | sed -n '2,$p' | tr '\n' '|')
[ "$body" = 'const s64 over = static_cast<s64>(current) - limit;|return over > 0 ? bytes + static_cast<u32>(over) : bytes;|}|' ] || \
  fail "Includes/azl/books_math.hpp: ChargeLift no longer lifts by the block and by what current stands above the limit ($body)"
need_line Includes/azl/books_math.hpp 'constexpr s32 kAboveLimit = static_cast<s32>(0xC860180A);' "the kernel's refusal above the limit, which the lift keeps away from the Charge's own reservation"
need_after Sources/ctr/kernel_books.cpp '    } else if (rc == books::kAboveLimit) {' '        s32 limit = 0;' "a Charge refused above the lifted limit has a line of its own, with the books (no fallback)"
need_stmt Tests/kernel_books_test.cpp 'TestChargeAfterTheGatePage();' "... the whole launch of a late title, the gate's page, the heap (Monster Hunter XX's way and libctru's), the Settle before and after the Charge"
body=$(region Includes/azl/books_math.hpp 's32 ChargeOrWrite(Kernel& kernel, u32 bytes, Way* way) {' '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | tr '\n' '|')
[ "$body" = 's32 ChargeOrWrite(Kernel& kernel, u32 bytes, Way* way) {|*way = Way::Kernel;|const s32 rc = ChargeThroughKernel(kernel, bytes);|if (rc != kNoPages) {|return rc;|}|kernel.WriteCurrent(static_cast<s32>(bytes));|*way = Way::Written;|return 0;|}|' ] || \
  fail "Includes/azl/books_math.hpp: ChargeOrWrite no longer charges through the kernel first and writes current[] only when the kernel had no pages ($body)"
body=$(region Includes/azl/books_math.hpp 's32 UnchargeOrWrite(Kernel& kernel, u32 bytes, u32* released, Way* way) {' '}' | code_of | sed 's/^[[:space:]]*//' | grep -v '^$' | tr '\n' '|')
[ "$body" = 's32 UnchargeOrWrite(Kernel& kernel, u32 bytes, u32* released, Way* way) {|*way = Way::Kernel;|const s32 rc = ReleaseThroughKernel(kernel, bytes, released);|if (rc != kNoPages) {|return rc;|}|kernel.WriteCurrent(-static_cast<s32>(bytes - *released));|*way = Way::Written;|return 0;|}|' ] || \
  fail "Includes/azl/books_math.hpp: UnchargeOrWrite no longer releases through the kernel first and writes off current[] only what it could not release for want of pages ($body)"
need_line Sources/ctr/kernel_books.cpp '    const s32 rc = books::ChargeOrWrite(kernel, size, &way);' "Charge through the kernel's reservation, written only when the kernel had no pages"
need_line Sources/ctr/kernel_books.cpp '    const s32 rc = books::UnchargeOrWrite(kernel, size, &released, &way);' "Uncharge through the kernel's release, written only when no region had a page"
need_after Sources/ctr/kernel_books.cpp '    const s32 rc = books::ChargeOrWrite(kernel, size, &way);' '    if (way == books::Way::Written) {' "the line of the Charge says which way it went"
need_after Sources/ctr/kernel_books.cpp '    if (way == books::Way::Written) {' '        Log("books: charge of 0x%lx bytes WRITTEN in kernel mode: the kernel had no pages for its reservation (0x%08lx), the way of 1.0.13-1.1.1",' "... the written one with its own words"
need_after Sources/ctr/kernel_books.cpp '    const s32 rc = books::UnchargeOrWrite(kernel, size, &released, &way);' '    if (way == books::Way::Written) {' "the line of the Uncharge says which way it went"
need_line Sources/ctr/kernel_books.cpp '    *rc = books::RaiseThroughKernel(kernel, size);' "Raise through the lifted limit and the kernel's reservation, its result for the line of the fallback"
need_line Sources/ctr/kernel_books.cpp '    return *rc == 0;' "... made only when the kernel reserved"
need Sources/ctr/gate_page.cpp '"gate: no hooks: SYSTEM page failed: %s (0x%08lx, %lu bytes at 0x%08lx)' "the failure's line says the kernel's result of the Raise"
need_stmt Tests/kernel_books_test.cpp 'TestCharge();' "the books' test charges through the made-up kernel, the game's budget spent"
need_stmt Tests/kernel_books_test.cpp 'TestChargeOnAFullRegion();' "... a region with no page for the reservation (Monster Hunter XX on an Old 3DS): written, and the end in balance"
need_stmt Tests/kernel_books_test.cpp 'TestChargeOrWriteKernelFirst();' "... the kernel's way first, and the write only for want of pages"
need Tests/kernel_books_test.cpp 'CHECK(books::ChargeOrWrite(k, kBlock, &way) == 0);' "... the console's own Charge with its fallback"
need Tests/kernel_books_test.cpp 'CHECK(books::UnchargeOrWrite(k, kBlock, &released, &way) == 0);' "... and its own Uncharge with its fallback"
need_stmt Tests/kernel_books_test.cpp 'TestChargeBeforeTheBlock();' "... the charge before the block, a block refused, a process that dies in between, a block freed the wrong way"
need_stmt Tests/kernel_books_test.cpp 'TestChargeRacedByTheGame();' "... a game thread that takes the lifted room first"
need_stmt Tests/kernel_books_test.cpp 'TestUncharge();' "... and the Uncharge: whole, a page at a time, and what stays when it cannot be made"
need Tests/kernel_books_test.cpp 'CHECK(books::RaiseThroughKernel(k, kPage) == 0);' "the test runs the console's own Raise steps"
need Tests/kernel_books_test.cpp 'CHECK(books::ChargeThroughKernel(k, kBlock) == 0);' "... its own Charge steps"
need Tests/kernel_books_test.cpp 'CHECK(books::ReleaseThroughKernel(k, kBlock, &released) == 0);' "... and its own Uncharge steps"
CALLERS=$(grep -rnE --include='*.cpp' '\b(RaiseBooks|ChargeBooks)\(' Sources | awk '{ f = $0; sub(/:.*$/, "", f); l = $0; sub(/^[^:]*:[0-9]*:/, "", l); sub(/\/\/.*$/, "", l); if (l ~ /(RaiseBooks|ChargeBooks)\(/ && l !~ /^(bool|void) (RaiseBooks|ChargeBooks)\(/) { n[f ":" (l ~ /RaiseBooks\(/ ? "Raise" : "Charge")]++ } } END { for (k in n) print n[k], k }' | sort | tr '\n' ' ')
[ "$CALLERS" = "1 Sources/ctr/gate_page.cpp:Raise 1 Sources/ctr/soc.cpp:Charge " ] || fail "RaiseBooks / ChargeBooks are called in '$CALLERS': Raise only for the gate's page (before any game thread), Charge only for the late socket block"
for k in KernelReadBooks:4 KernelLiftBooks:0 KernelSettleBooks:0 KernelChargeBooks:0; do
  need_line Tests/stack_usage.sh "budget kernel_books \"azl::ctr::{anonymous}::${k%%:*}\" ${k#*:}" "the books are read and their limit written in kernel mode, on the kernel's stack: leaves (one saved register at the most)"
done
need Tests/chain_sim/chain_sim.py '        scenario_swap_during_svc(elf, level)' "the whole-chain test plays the swap inside the gate's svc, before the call and 10,000 times"
need_stmt Sources/ctr/plugin.cpp 'SettleGatePage();' "LateInit settles the books of the gate's page"
need_order Sources/ctr/plugin.cpp 'void Plugin::LateInit() {' '    SettleGatePage();' "... in LateInit, long after the game sized its heap"
# The plugin's own threads around the same swap (2026-11-02): stopping a thread does not take back its service call, and a service answering with the block gone stops the console (kernel panic, process `socket`: issues 3 and 5).
# The gate's logic is held by Tests/swap_gate_test.cpp (and Tests/swap_gate_mutants.py); these pins hold its callers, which no test on the PC runs.
[ "$(grep -c 'azl_c_soc_enter ();' Sources/enet/enet_3ds.c)" = "12" ] || fail "Sources/enet/enet_3ds.c: the twelve socket calls that move bytes through the plugin's memory go through the gate"
[ "$(grep -c 'azl_c_soc_leave ();' Sources/enet/enet_3ds.c)" = "13" ] || fail "Sources/enet/enet_3ds.c: every enter of the gate has its leave (the receive has two, one per branch)"
need Sources/ctr/plugin.cpp 'CloseSwapGate(kSwapGateTimeoutMs);' "SWAP_ENTER closes the gate of the plugin's threads"
need Sources/ctr/plugin.cpp 'OpenSwapGate();' "SWAP_EXIT opens it"
need Sources/ctr/plugin.cpp 'OpenSwapGate(); // (a thread stopped at the gate could never be joined)' "the shutdown opens it"
need Sources/ctr/plugin.cpp 'InitSwapGate(); // (before the threads that use it)' "the gate is set up on an Old 3DS before the threads start"
need Sources/ctr/plugin.cpp 'const SwapGateScope gate(kGateWriter);' "a pass of the log's writer is inside the gate"
need Sources/ctr/plugin.cpp 'const SwapGateScope gate(kGateTrail);' "the lines of the sleep's trail are inside the gate"
need Sources/ctr/swap_gate.cpp 'if (CTRPluginFramework::System::IsNew3DS()) {' "the gate stays off on a New 3DS"
need Sources/ctr/swap_gate.cpp 'svcWaitSynchronization(g_gate_open, -1);' "a stopped thread waits with no end, until SWAP_EXIT"
need Sources/ctr/plugin.cpp 'log_files_->SetStopCapture(&g_swap_gate.closed);' "the capture leaves the card when the gate closes: two writes on a slow card outlast the wait"
need Makefile '-Wl,--wrap=PLGLDR__Reply' "on an Old 3DS the gate opens as soon as the block is back and mapped (the framework's reply has returned)"
need Sources/ctr/plugin.cpp 'extern "C" void __wrap_PLGLDR__Reply(s32 event) {' "the wrap of the framework's reply to Luma3DS is there"
need_order Sources/ctr/plugin.cpp '    __real_PLGLDR__Reply(event);' 'azl::ctr::Plugin::Get().OnSwapBack();' "the gate opens only after the framework's reply returned: before it the block is not mapped"
need_after Sources/ctr/plugin.cpp 'void Plugin::OnSwapBack() {' '    OpenGate();' "the block is back: the gate opens"
need Makefile '-Wl,--wrap=svcCreateThread -Wl,--wrap=_ZN18CTRPluginFramework9Scheduler10InitializeEv' "the framework's syscore worker is held off core 1 on an Old 3DS: a thread that has not begun escapes the stop at the swap"
need Sources/ctr/thread_core.cpp 'g_moving = svcGetSystemInfo(&is_new, 0x10001, 0) != 0;' "only on an Old 3DS, by the very test of Luma's swap"
need Sources/ctr/plugin.cpp 'constexpr u32 kSwapGateTimeoutMs = 6100;' "the wait of the gate stays within what Luma waits for the answer (10 s: the mirror's 1 s, the trail's 40 ms and the hooks' 1.5 s before it, 8.64 s in all)"
# The table of the game's real sessions the hook follows (1.1.2): around the swap some of the game's closes are not seen, and after 12 HOMEs the APT:A sessions were no longer
# followed (issue 5, Pokemon Alpha Sapphire, Old 3DS): no HOME seen, the game woke up in freed memory. The logic is held by Tests/traced_sessions_test.cpp (and
# Tests/traced_sessions_mutants.py); these pins hold its callers.
need_stmt Sources/ctr/hook.cpp 'g_traced.Opened(session);' "every session the game opens frees the entries with its index, followed or not"
need Sources/ctr/hook.cpp 'if (cmd[0] == kSrvGetServiceHandleReply && cmd[2] == kIpcMoveOneHandle) {' "only the answer of srv (one handle moved to the game) says a handle is new"
need_stmt Sources/ctr/hook.cpp 'g_traced.Touch(*traced);' "an APT session the game keeps and uses is never the one a full table gives up"
need_stmt Sources/ctr/hook.cpp 'g_traced.Add(session, name, apt);' "the followed services go in the table, the APT ones with their flag (a full table gives them the oldest APT slot)"
need_stmt Sources/ctr/hook.cpp 'g_traced.Closed(handle);' "a close seen by the hook frees the entry"
# Every file with 'game-thread code' regions has its game_thread_file line here: without it the printf scan skips the file without a word.
for f in $(grep -rlE "$GT_BEGIN" Sources Includes | sort); do
  grep -q "^game_thread_file $f " Tests/policy_check.sh || fail "$f has 'game-thread code' regions but no game_thread_file line in Tests/policy_check.sh (the scan for printf on game threads would skip it)"
done
need Sources/ctr/plugin.cpp 'followed sessions closed unseen %lu, APT slots reused %lu, sessions not followed %lu' "the log of the swap says what the table had to do without the closes"
need Includes/azl/apt_sleep.hpp 'return signal == 1 || signal == 2 || signal == 8 || signal == 9;' "the power button comes before the swap too (a dump from an Old 3DS: the thread woke up in unmapped memory)"
need Sources/ctr/plugin.cpp 'constexpr int kEventSwapEnter = 5;' "the framework's SWAP_ENTER"
need Sources/ctr/plugin.cpp 'constexpr int kEventSwapExit = 6;' "the framework's SWAP_EXIT"
need Sources/ctr/plugin.cpp 'static_cast<int>(Process::Event::SWAP_ENTER) == kEventSwapEnter &&' "the indices are checked against the framework's enum"
need Sources/ctr/plugin.cpp 'CloseGate(kSwapDrainTimeoutMs, dlp_hub_ ? &WakeHubForSwap : nullptr, dlp_hub_.get());' "SWAP_ENTER closes the svc hook's gate before the framework answers Luma3DS (and wakes the hub's queue)"
need Sources/ctr/plugin.cpp 'OpenGate(); // (idempotent' "SWAP_EXIT opens it again (OnSwapBack opened it already)"
need_order Sources/ctr/plugin.cpp 'CloseGate(kSwapDrainTimeoutMs,' 'OpenGate(); // (idempotent' "enter before exit"
need_order Sources/ctr/plugin.cpp 'CloseGate(kSwapDrainTimeoutMs,' 'CloseSwapGate(kSwapGateTimeoutMs);' "the hook's gate closes before the plugin's threads stop: a game's thread in the plugin may wait for what a stopped thread holds"
DRAIN=$(grep -o 'kSwapDrainTimeoutMs = [0-9]*' Sources/ctr/plugin.cpp | grep -o '[0-9]*$')
{ [ -n "$DRAIN" ] && [ "$DRAIN" -ge 500 ] && [ "$DRAIN" -le 5000 ]; } || fail "Sources/ctr/plugin.cpp: the wait for the game's threads counted in the gate is $DRAIN ms (between half a second and five seconds: Luma3DS gives up on the answer after 10 s)"
TRAIL=$(grep -o 'kSwapTrailWaitMs = [0-9]*' Sources/ctr/plugin.cpp | grep -o '[0-9]*$')
GATEMS=$(grep -o 'kSwapGateTimeoutMs = [0-9]*' Sources/ctr/plugin.cpp | grep -o '[0-9]*$')
{ [ -n "$TRAIL" ] && [ -n "$DRAIN" ] && [ -n "$GATEMS" ] && [ $((1000 + TRAIL + DRAIN + GATEMS)) -le 9000 ]; } || fail "Sources/ctr/plugin.cpp: the waits of SWAP_ENTER (the mirror's 1 s + trail $TRAIL + hooks $DRAIN + gate $GATEMS ms) are over 9 s: Luma3DS gives up on the answer after 10 s"
# (two calls since the in-game menu: a plain need was met by the menu's alone, and the frame callback's could go unnoticed)
need Tests/stack_usage.sh '"$PY" Tests/hook_sim/gate_sim.py "$WORK/gate" || FAILED=1' "the stack check runs the gate (Sources/ctr/gate.s) in the emulator against the swap"
need Tests/chain_sim/chain_sim.py '        self.run_game(site)' "the whole-chain test enters through the real gate of the ELF, from a one-word site"
need Tests/stack_usage.sh 'budget hook "azl::ctr::NoteGameFailure" 56' "the line of a failed call runs on the game's thread: its frame is held"
need Sources/ctr/plugin.cpp 'WaitForTrail(kSwapTrailWaitMs);' "SWAP_ENTER gives the trail its moment on the card before the threads stop"

# The framework's link to Luma3DS's plugin loader (REGISTRO, phase N; azl/plg_guard.hpp): console 63 crashed in PLGLDR__FetchEvent on the NULL event pointer
# that plgLdrInit leaves when its one connection to plg:ldr fails. The three wraps, the budget of the tries (about 2 s), the arbiter, the fetch that never
# connects, the plugin out of the launch without the link, and the line that goes synchronously to the main log's file (never AzaharLink.mem).
need Makefile '-Wl,--wrap=PLGLDR__Reply -Wl,--wrap=plgLdrInit -Wl,--wrap=PLGLDR__FetchEvent' "the framework's connection to plg:ldr and its fetch are guarded (and its reply, for the Old 3DS's gate)"
need Sources/ctr/plugin.cpp 'extern "C" Result __wrap_plgLdrInit(void) {' "the wrap of the framework's connection to plg:ldr is there"
need Sources/ctr/plugin.cpp 'extern "C" s32 __wrap_PLGLDR__FetchEvent(void) {' "the wrap of the framework's fetch of the events is there"
need_line Includes/azl/plg_guard.hpp 'constexpr u32 kMaxTries = 100;' "the connection is tried 100 times"
need_line Includes/azl/plg_guard.hpp 'constexpr u32 kRetryMs = 20;' "... 20 ms apart: about 2 s, which covers the loader's own session closing (it holds the port's one session just before)"
need Includes/azl/plg_guard.hpp '    if (rc >= 0 && (arbiter != 0 || s->emulator)) {' "a try counts with the connection AND the arbiter (without it every reply leaves Luma3DS waiting 10 s); an emulator's loader excepted"
need Sources/ctr/plugin.cpp '            cmd[3] = 0;' "the word the arbiter comes in is cleared before each try (a try whose IPC did not answer shows 0)"
need Sources/ctr/plugin.cpp '            return azl::plgguard::Try{r, r >= 0 ? cmd[3] : 0u};' "the arbiter is read from word 3 of the answer, as plgLdrInit reads it (ldr [tls, #0x8c])"
need Includes/azl/plg_guard.hpp '    return s.connected ? fetch() : 0;' "no event is fetched without a connection: 0 is 'no event' for KeepThreadMain (the NULL pointer is never read)"
for fn in 'extern "C" Result __wrap_plgLdrInit(void) {' 'extern "C" s32 __wrap_PLGLDR__FetchEvent(void) {'; do
  hit=$(region Sources/ctr/plugin.cpp "$fn" '}' | code_of | grep -E '\bLog[[:space:]]*\(|DebugEvent|std::|printf|\bnew\b|malloc|Plugin::Get|PostNotification' | head -1)
  [ -z "$hit" ] || fail "Sources/ctr/plugin.cpp: $fn runs on the framework's keep-alive thread (4 KB) before anything of the plugin: no Log, no string, no heap ($hit)"
done
hit=$(region Sources/ctr/plugin.cpp 'extern "C" s32 __wrap_PLGLDR__FetchEvent(void) {' '}' | code_of | grep -E 'plgLdrInit|Connect\(|plgLdrExit' | head -1)
[ -z "$hit" ] || fail "Sources/ctr/plugin.cpp: the fetch tries a connection ($hit): plg:ldr and the events are one loop of Rosalina, a try while it waits for our reply blocks 10 s and reads an old event"
hit=$(region Includes/azl/plg_guard.hpp 's32 FetchOrNone(const State& s, Fetch fetch) {' '}' | code_of | grep -E 'Connect|try_once|After\(' | head -1)
[ -z "$hit" ] || fail "Includes/azl/plg_guard.hpp: the fetch tries a connection ($hit)"
need_after Sources/ctr/plugin.cpp '    const bool loader_linked = CheckPluginLoaderLink();' '    active_ = config_.enabled && config_.tunnel_enabled && loader_linked;' "without the link the plugin stays out of the launch: no hooks, no network, no chat, no mirror"
need_order Sources/ctr/plugin.cpp '    const bool loader_linked = CheckPluginLoaderLink();' 'capture_ready = !active_ || DebugInit();' "the link decides before the capture, the socket block and the hooks"
need_order Sources/ctr/plugin.cpp '    const bool loader_linked = CheckPluginLoaderLink();' 'hook_stats_ = InstallSvcHooks(hle_.get(), !swap_console_ && luma_alias_);' "the link decides before the hooks"
need Sources/ctr/plugin.cpp "startup_issue_ = \"3DS Link: the plugin could not talk to Luma's plugin loader: restart the game\";" "the notice on screen without the link"
need Sources/ctr/plugin.cpp '    if (active_ || startup_issue_.empty() || startup_issue_once_) {' "... once, not every minute"
region Sources/ctr/plugin.cpp 'bool Plugin::CheckPluginLoaderLink() {' '}' | grep -q -F 'File::Open(out, kLogSyncPath, File::RWC | File::APPEND | File::SYNC)' || fail "Sources/ctr/plugin.cpp: the line of the link is no longer written synchronously to the main log's file"
region Sources/ctr/plugin.cpp 'bool Plugin::CheckPluginLoaderLink() {' '}' | grep -q -E 'kMemPath|g_debug_level' && fail "Sources/ctr/plugin.cpp: the line of the link goes to AzaharLink.mem (read with sscanf, written over at every launch) or depends on the log level"
need Sources/ctr/plugin.cpp '    if (plgguard::WorthALine(s)) {' "the synchronous line only when the link took more than a try, came without its arbiter or did not come"
need Includes/azl/plg_guard.hpp '    return s.tries != 1 || (s.no_arbiter != 0 && !s.emulator) || !s.connected;' "... which is these three (an emulator's session without an arbiter at the first try is its normal case: no line)"
need Tests/run.sh '  "$OUT/plg_guard_test"' "the guard's logic is run (fake plgLdrInit: busy port, never, no arbiter, emulator; the fetch)"
need Tests/stack_usage.sh 'budget plugin "__wrap_plgLdrInit" 64' "the wrap of plgLdrInit runs on the framework's 4 KB keep-alive thread: its frame is held"
need Tests/stack_usage.sh 'budget plugin "__wrap_PLGLDR__FetchEvent" 8' "the wrap of the fetch: a load and a tail call"
need Tests/stack_usage.sh 'budget plugin "azl::ctr::Plugin::CheckPluginLoaderLink" 192' "the line of the link, on the keep-alive thread (its buffer is static; the framework's File and the clock on the stack)"
# The close of a game in the sleep's trail (debug=1 only; REGISTRO, phase N, F2): which close left Luma3DS's block behind. The kinds of the trail have
# numbers of their own (kFwExit, 5, is the end of a SLEEP: the EXIT of the process is kFwProcExit), Luma3DS's plugin word is read at HOME_ENTER /
# HOME_EXIT / EXIT and after every ReceiveParameter, the lines after a close command have a budget of their own, srv's termination request is noted,
# the trail's thread writes once more when it is told to stop, and the game's thread gives the trail 200 ms at most after the close command (12),
# with the log on, never on an Old 3DS, ended by the gate's close.
KINDS=$(cat Includes/azl/apt_sleep.hpp Includes/azl/sleep_crumbs.hpp | tr -d '\r' | grep -oE 'constexpr u32 k(AptNote|Fw|Mem)[A-Za-z]* = [0-9]+;' | grep -oE '[0-9]+;$' | tr -d ';' | sort -n)
KDUP=$(printf '%s\n' "$KINDS" | uniq -d | tr '\n' ' ')
KCOUNT=$(printf '%s\n' "$KINDS" | grep -c .)
{ [ -z "$KDUP" ] && [ "$KCOUNT" -ge 25 ]; } || fail "the kinds of the sleep's trail (kAptNote*, kFw*, kMem*) share a number ($KDUP) or are fewer than 25 ($KCOUNT): a line would be read as another kind"
need_line Includes/azl/sleep_crumbs.hpp '    static constexpr u32 kFwExit = 5;' "kFwExit is the end of a sleep (the EXIT of the process is kFwProcExit)"
need_line Includes/azl/apt_sleep.hpp 'constexpr u32 kLumaPluginWordAddress = 0x1FF800F0;' "Luma3DS's plugin word in the configuration page"
for push in 'NoteSleepEvent(SleepCrumbs::kFwProcExit, LumaPluginWord());' 'NoteSleepEvent(event == kEventHomeEnter ? SleepCrumbs::kFwProcHomeEnter : SleepCrumbs::kFwProcHomeExit, LumaPluginWord());'; do
  tr -d '\r' < Sources/ctr/plugin.cpp | grep -B1 -F -- "$push" | head -1 | grep -q -F 'if (g_debug_level > 0) {' || fail "Sources/ctr/plugin.cpp: '$push' is gone or not behind 'if (g_debug_level > 0)' (the probes of the close cost nothing with the log off)"
done
need Sources/ctr/hook.cpp '            g_apt_observer(kAptNoteReceiveWord, LumaPluginWord());' "Luma3DS's word after every ReceiveParameter (the state the game is left in)"
need Sources/ctr/hook.cpp '    const u32 take = g_receive_notes.Take(AptCommandCloses(command));' "the lines after a close command have their own budget (the 64 were used up by a game that polls)"
need_line Includes/azl/apt_sleep.hpp '    static constexpr u32 kAfterClose = 32;' "... of 32"
need Includes/azl/apt_sleep.hpp '    return command == kAptCommandCloseFromHome;' "the close command is 12 only (11 is the return from the HOME menu, 10 a library applet that exited)"
need Sources/ctr/hook.cpp '        if (g_debug_level > 0 && g_apt_observer && (header == kAptAppletUtility || (header == kAptReceiveParameter && cmd[1] == 0x300))) {' "the lines of ReceiveParameter (and the wait after the close) only with the log on"
need Sources/ctr/hook.cpp '    if (header == kSrvReceiveNotification) {' "srv's termination request to the game is noted (no 0x100 and no EXIT: killed by pm)"
need_line Sources/ctr/hook.cpp 'constexpr u32 kMaxSrvNotes = 4;' "... a few times a launch"
need Sources/ctr/hook.cpp 'cmd[2] == kSrvNotificationTerminate &&' "... only 0x100"
need Sources/ctr/plugin.cpp '        for (bool last = false; !last;) {' "the trail's thread takes one more turn when it is told to stop (the EXIT of the process is pushed just before)"
hit=$(region Sources/ctr/plugin.cpp '        for (bool last = false; !last;) {' '            crumb_written_.store(crumbs_.taken());' | code_of | grep -E '\bbreak\b|return' | head -1)
[ -z "$hit" ] || fail "Sources/ctr/plugin.cpp: the trail's thread leaves without writing what was noted up to the stop ($hit)"
region Sources/ctr/plugin.cpp 'void Plugin::RequestStop() {' '}' | grep -q -F '    crumb_stop_.store(true);' || fail "Sources/ctr/plugin.cpp: RequestStop no longer stops the trail's thread (it would live on through the framework's __ctru_exit if main() is late)"
CLOSEMS=$(grep -o 'kCloseTrailWaitMs = [0-9]*' Sources/ctr/plugin.cpp | grep -o '[0-9]*$')
{ [ -n "$CLOSEMS" ] && [ "$CLOSEMS" -ge 20 ] && [ "$CLOSEMS" -le 250 ]; } || fail "Sources/ctr/plugin.cpp: the game's thread waits $CLOSEMS ms for the trail after the close command (between 20 and 250 ms)"
need Sources/ctr/hook.cpp '    if (command == kAptCommandCloseFromHome && g_trail_waiter) {' "the wait for the trail comes after the close command (12) only"
WCT=$(region Sources/ctr/plugin.cpp 'void Plugin::WaitForCloseTrail() {' '}' | code_of)
printf '%s\n' "$WCT" | grep -q -F 'if (g_debug_level <= 0 || self.swap_console_ || self.crumb_wake_ == 0) {' || fail "Sources/ctr/plugin.cpp: the wait after the close command is made with the log off or on an Old 3DS (its HOME goes through the swap and its own budget)"
printf '%s\n' "$WCT" | grep -q -F 'waited < kCloseTrailWaitMs && !GateClosing() &&' || fail "Sources/ctr/plugin.cpp: the wait after the close command is not bounded by kCloseTrailWaitMs or not ended by the gate's close"
need Sources/ctr/plugin.cpp 'swap_console_ = svcGetSystemInfo(&is_new, 0x10001, 0) != 0;' "an Old 3DS by the very test of Luma3DS's swap"
need Includes/ctr/plugin.hpp '    bool swap_console_ = true;' "... and the safe side until EarlyInit knows"
for fn in 'void Plugin::NoteProcessEvent(int event) {' 'void Plugin::OnSwapBack() {' 'extern "C" void __wrap_PLGLDR__Reply(s32 event) {' 'void Plugin::NoteSwapToTrail(u32 kind, u32 number) {'; do
  hit=$(region Sources/ctr/plugin.cpp "$fn" '}' | code_of | grep -E 'WaitForCloseTrail|kCloseTrailWaitMs|g_trail_waiter' | head -1)
  [ -z "$hit" ] || fail "Sources/ctr/plugin.cpp: the wait after the close command is on the swap's path ($fn): $hit"
done
[ "$(cat Sources/ctr/*.cpp Sources/*.cpp | tr -d '\r' | code_of | grep -c 'WaitForCloseTrail')" = "2" ] || fail "WaitForCloseTrail is used somewhere else than as the hook's TrailWaiter (EarlyInit) and its definition"
need Tests/stack_usage.sh 'budget hook "azl::ctr::NoteSrvNotification" 16' "the note of srv's 0x100 runs on Tri Force Heroes' 464-byte thread"
need Tests/stack_usage.sh 'budget plugin "azl::ctr::Plugin::WaitForCloseTrail" 24' "the wait after the close command, on the game's thread"
need Tests/callgraph_check.py "    r'^azl::ctr::Plugin::WaitForCloseTrail\(\)\$'," "the call graph follows the hook's TrailWaiter (a wait on a game thread)"

# The page "Public lobbies" (the branch public-lobbies): the rooms are asked without joining, without the console's MAC, not more often than
# SELECT allows, and the job of the lists never writes the files the page uses (it leaves .new files the page applies on its own thread).
# The logic is held by Tests/settings_test/public_lobbies_test.cpp, the asking by Tests/lobby_probe_test.cpp (against real rooms, integration.sh).
need ConfigApp/source/lobby_probe.cpp "    w.MacAddress(azl::kNoPreferredMac);" "a public room is asked without the console's MAC"
need ConfigApp/source/lobby_probe.cpp "    w.U32(kNoVersion);" "... and with a protocol version no room speaks, so that nobody is taken in"
need ConfigApp/source/lobby_probe.cpp "        s.joined = true;" "... and a room that takes us in all the same is left at once"
need ConfigApp/source/pages_lobbies.cpp "constexpr u64 kRefreshPauseMs = 10000;" "SELECT asks every public room again at most every 10 s (every ask reaches the games played in it)"
need ConfigApp/source/update_worker.cpp '    return setup::WriteTextFile(path + ".new", text);' "the job of the lists writes the list it fetched beside the one the page uses, never over it"
need ConfigApp/source/session.cpp "    lobbies::Scanner::Get().Shutdown(15000);" "the asking of the public rooms is stopped before the sockets go away for Titles Delivery"
if grep -n -E 'timer|Timer|every [0-9]+ (s|min)' ConfigApp/source/pages_lobbies.cpp | grep -v -i 'refresh\|pause\|kRebuildMs\|kRetryMs' >/dev/null; then
  fail "the page of the public lobbies asks the rooms on a timer: it asks them when it opens and on SELECT only"
fi

# The app's own allocator before main() (ConfigApp/source/heap_alloc.cpp; its decisions are heap_plan.hpp, held by Tests/settings_test/heap_plan_test.cpp).
# heap_alloc.cpp does not build on a PC: what it says to the kernel is pinned here, line by line. The test's kernel takes these for granted: the
# linear heap is asked for first (a refused linear heap costs nothing), a refusal says WHICH part it was (else the cut of the linear heap never
# comes), the clock is in ms (in s the 3 s of waiting are 50 minutes before main()), the readings are the types the plan reads.
HA=ConfigApp/source/heap_alloc.cpp
need_line $HA 'extern "C" void __system_allocateHeaps(void) {' "the app's allocator replaces libctru's (weak) one"
need_line $HA '    if (!heap_plan::Run(env, __ctru_heap_size, __ctru_linear_heap_size, g_record)) {' "the plan of heap_plan.hpp decides, with the sizes the program fixed"
need_after $HA '    if (!heap_plan::Run(env, __ctru_heap_size, __ctru_linear_heap_size, g_record)) {' '        BreakWithNumbers(g_record);' "a stop leaves the numbers in the registers of the crash dump"
need_line $HA '        __asm__ __volatile__("svc 0x3C"' "the stop is svcBreak (0x3C), as libctru's"
need_line $HA '        r.used_rc = svcGetSystemInfo(&v, 0, 1);' "the region's used pages: svcGetSystemInfo(0, 1)"
need_line $HA '        r.luma_rc = svcGetSystemInfo(&v, 0x10000, 0x80);' "the region's size from Luma: svcGetSystemInfo(0x10000, 0x80)"
need_line $HA '        r.own_rc = svcGetProcessInfo(&v, CUR_PROCESS_HANDLE, 2);' "the app's own process: svcGetProcessInfo type 2 (another type and the books never see a game)"
need_line $HA '        r.memtype_rc = svcGetProcessInfo(&v, CUR_PROCESS_HANDLE, 19);' "the memory type: svcGetProcessInfo type 19 (an applet counts the books alone)"
need_line $HA '        Result rc = svcControlMemory(&linear_addr, 0x0, 0x0, linear, MEMOP_ALLOC_LINEAR, static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));' "the linear heap is one request"
need_line $HA '        rc = svcControlMemory(&heap_addr, OS_HEAP_AREA_BEGIN, 0x0, heap, MEMOP_ALLOC, static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));' "the heap is one request, at the heap area"
need_order $HA ', MEMOP_ALLOC_LINEAR, static_cast<MemPerm>' ', MEMOP_ALLOC, static_cast<MemPerm>' "the linear heap is asked for first: refused, it costs nothing (the kernel zeroes it only once found)"
need_after $HA ', MEMOP_ALLOC_LINEAR, static_cast<MemPerm>' '            return heap_plan::Part::Linear;' "a refused linear heap says so: the plan cuts the linear heap, not the total"
need_line $HA '            svcControlMemory(&unused, linear_addr, 0x0, linear, MEMOP_FREE, static_cast<MemPerm>(0)); // (the next request starts from nothing)' "a refused heap gives the linear heap back"
need_after $HA ', MEMOP_FREE, static_cast<MemPerm>(0)); // (the next request starts from nothing)' '            return heap_plan::Part::Heap;' "a refused heap says so"
need_order $HA ', MEMOP_ALLOC, static_cast<MemPerm>' '        return heap_plan::Part::None;' "done only after both requests"
[ "$(tr -d '\r' < $HA | grep -c 'return heap_plan::Part::')" = 3 ] || fail "$HA: Try returns a Part in more or fewer than its three places (Linear, Heap, None)"
need_line $HA '        return static_cast<u32>(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000));' "the plan's clock is in ms (the 3 s of waiting and the 1 s between equal requests)"
need_line $HA '        svcSleepThread(static_cast<s64>(ms) * 1000000LL);' "a wait of the plan is in ms"
# its log keeps the last starts (one line each, with the console's date and time and the time since boot): the sequences of a test session need them all
need_line $HA '    if (setup::ReadTextFile(path, heap_plan::kLogReadBytes, old) == setup::LoadResult::Unreadable) {' "the old lines are read first (a file that could not be read is not written over)"
need_line $HA '    setup::WriteTextFile(path, heap_plan::KeepLast(old, std::string(stamp) + line, heap_plan::kLogMaxLines, heap_plan::kLogMaxBytes));' "the heap log grows by one stamped line per start and keeps the last ones"
need ConfigApp/source/heap_plan.hpp 'constexpr std::size_t kLogMaxLines = 50;' "the heap log keeps the last 50 starts"
need ConfigApp/source/main.cpp '        heap_alloc::WriteStartLog(g.paths.datadir);' "every start but the first adds its line"
need ConfigApp/source/pages_startup.cpp '        heap_alloc::WriteStartLog(g.paths.datadir);' "the first start adds its line once it may write"

if [ "$FAILED" = 0 ]; then
  echo "policy check: defaults and switches as intended"
fi
exit $FAILED
