// The pages of the 3DS Link app. Each lives in its own pages_*.cpp and is only created through these functions, so a page
// can open another one without knowing its class.
//
//   StartupPage         the first page: on the first start the question, the scan, the checks and the summary; on every other start the
//                       scan and the Plugin Loader question; then the main page takes its place (pages_startup.cpp)
//   InstallCheckPage    what the installation has and lacks, and what A does about it (pages_check.cpp)
//   MainPage            the main page (pages_main.cpp)
//   ServersPage         the saved rooms: use, add, remove, rename (pages_servers.cpp)
//   LobbiesPage         the public rooms: their ping, who plays what, use one, keep one among the saved rooms (pages_lobbies.cpp)
//   EnableGamePage      which titles run with 3DS Link: their icons, names and switches (pages_enable.cpp)
//   DeliveryPage        Titles Delivery: send titles, receive titles (pages_delivery.cpp, delivery_run.cpp; declared in
//                       delivery.hpp with the pages under it)
//   ConfigPage          the options, with the test, the installation check and About (pages_config.cpp)
//   AboutPage           the name and version, who made it, how it was installed, the plugin file (pages_config.cpp)
//   CreditsPage         who 3DS Link thanks: what it uses and has copied, with author and license (pages_credits.cpp, about_text.hpp)
//   TestPage            the connection test (pages_test.cpp)
//   the popups          a question, a message, a choice, the search of the card, the folder browser and the test of a new room
//                       (modal.cpp, probe.cpp)
//   the guide           "How to get boot9.bin": a picture of GodMode9's screen for each step (pages_guide.cpp, over ConfigApp/guide)
//   the update          the check, the question, the download and the install (pages_update.cpp)
//   DlpPage             Download Play, the entry of the main page: Join and Games (pages_dlpguest.cpp; 1.1.4, spec 9)
//   DlpGamesPage        Download Play > Games: the games that have a Download Play child, a switch each (prepare, remove), their space, remove all;
//                       the run of the worker (pages_dlpguest.cpp)
//   DlpJoinPage         Download Play > Join: the hosts of the room, the join, the wait, the hand-over to the prepared child
//                       (pages_dlpjoin.cpp; 1.1.4, route B)
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "search.hpp"
#include "ui.hpp"

namespace pages {

ui::Page* NewStartupPage();
// `on_close` is called when the first start's summary is closed, after the page has asked to be popped: START goes on (true), B cancels (false).
ui::Page* NewInstallCheckPage(std::function<void(bool go_on)> on_close = nullptr, bool first_start_summary = false);
ui::Page* NewMainPage();
ui::Page* NewServersPage();
ui::Page* NewLobbiesPage();
// The page of a room's name, address, port and password (pages_servers.cpp). Add and Change are the saved rooms' own; Favourite is a saved room
// that is a public one (X on Public lobbies put it there): only its name can change, the rest comes with the list. `saved` gets the room when Y
// saved it (the page is gone by then).
enum class RoomForm { Add, Change, Favourite };
// Whether host:port is a room of the list of public rooms on the card (pages_lobbies.cpp).
bool InPublicList(const std::string& host, u16 port);
ui::Page* NewRoomFormPage(const setup::Server& room, RoomForm form, std::function<void(const setup::Server& room)> saved);
// Looks at the public list for the room in use: when it is a public room (and not one of the saved ones) its name goes to g.public_room_name,
// for the screens that name the room. True when it is one.
bool RefreshPublicRoomName();
ui::Page* NewEnableGamePage();
ui::Page* NewConfigPage();
ui::Page* NewAboutPage();
ui::Page* NewCreditsPage();
ui::Page* NewTestPage();

// ---------------------------------------------------------------------------------------------
// Popups (modal.cpp). Each one is pushed over the current page, draws it dimmed on the touch screen with its own card on top, and pops
// itself before it calls back, so the callback may push the next popup.
// ---------------------------------------------------------------------------------------------

using YesNoFn = std::function<void(bool yes)>;
// A question: [A yes_label] [B no_label].
ui::Page* NewConfirmPage(const std::string& title, const std::string& text, const std::string& yes_label, const std::string& no_label, YesNoFn done);
// A message with one button. `top_text` (when not empty) is a longer text drawn on the top screen; `text` is the one on the touch screen.
ui::Page* NewMessagePage(const std::string& title, const std::string& text, const std::string& top_text = std::string());
// A choice among a few options (D-pad and A, or a tap; B cancels without calling back). `done` gets the index. `top_text` as above.
using ChoiceFn = std::function<void(int index)>;
ui::Page* NewChoicePage(const std::string& title, const std::string& text, const std::vector<std::string>& options, ChoiceFn done,
                        const std::string& top_text = std::string());

// The search of the SD card for a file: shows what it is doing (B cancels) and calls back with how it went.
enum class SearchResult { Found, NotFound, Cancelled };
using SearchFn = std::function<void(SearchResult result, const std::string& path)>;
ui::Page* NewSearchPage(const std::string& file_name, setup::FileSearch::Accept accept, const setup::FileSearch::Options& options, SearchFn done);

// The folder browser: A opens the folder under the cursor or, on the first row, uses the current one; B goes up (and cancels at the top).
// `chosen` gets the folder and says whether it is good: true closes the browser, false leaves it open (it has told the user why).
using FolderFn = std::function<bool(const std::string& folder)>;
ui::Page* NewFolderPage(const std::string& title, const std::string& hint, FolderFn chosen);

// The test of a new room, in a short popup with its three steps (B cancels): `done` gets how it went.
enum class ProbeOutcome { Reached, Failed, Cancelled, WrongPassword };
using ProbeFn = std::function<void(ProbeOutcome outcome)>;
ui::Page* NewProbePage(const setup::Server& server, ProbeFn done);

// ---------------------------------------------------------------------------------------------
// What A does about a problem of the installation (pages_check.cpp): each starts a few popups and ends with a toast.
// ---------------------------------------------------------------------------------------------

// Put the plugin that is inside 3DS Link on the card again (where 3DS Link keeps it), over a file that is not the same, and say how it went.
void RestorePlugin();
// Make sdmc:/luma/plugins (and AzaharLink.ini in it).
void CreatePluginFolder();
// The three ways to a boot9.bin: search the card, choose a folder, how to get it.
void StartBoot9Choice();
// The row "Installed titles": the question that lists the titles of no use, then their deletion and a new scan.
void StartTitlesCleanup();

// ---------------------------------------------------------------------------------------------
// The guide "How to get boot9.bin" (pages_guide.cpp)
// ---------------------------------------------------------------------------------------------

// Opens the guide (GodMode9's steps, a picture each) as a page over the current one: L / R and the D-pad's up and down move between the steps, B closes it.
// False when this build has no pictures: nothing was opened, and the caller says so (dlp::kNoGuideToast) or falls back on words.
bool OpenBoot9Guide();

// ---------------------------------------------------------------------------------------------
// The update from GitHub (pages_update.cpp)
// ---------------------------------------------------------------------------------------------

// The row of Settings: asks GitHub now and says what came of it, in the open (the check of a start says nothing).
ui::Page* NewUpdateCheckPage();
// Once a frame on the main page, before anything else it does: the quiet check of this start. It may only start the check, or (true) it has just pushed a popup: the question, or the
// one word about the clock. Nothing else happens on the main page in that frame.
bool PollUpdateAtStart(const ui::Input& in);

// ---------------------------------------------------------------------------------------------
// The Download Play children 3DS Link prepares (pages_dlpguest.cpp, dlp_guest.hpp)
// ---------------------------------------------------------------------------------------------

// Download Play, from the main page (1.1.4, spec 9): the rows Join and Games. When it opens, once, the quiet work of the children (a child whose game
// is gone is removed): the only place where the children are worked on, but the reading of the background.
ui::Page* NewDlpPage();
// Download Play > Games: the games with a Download Play child, a switch each (A on Off prepares that game's child, A on On removes it), the space, X
// removes them all, Y the guide of boot9.bin.
ui::Page* NewDlpGamesPage();
// Download Play > Join (1.1.4, route B: specifiche/8-INGRESSO-DA-3DSLINK.md): the gate, then the room's hosts of the games whose child is Ready here;
// A joins, B leaves, and at the host's START the app hands the game over and ends into the prepared child.
ui::Page* NewDlpJoinPage();
// (the build 2h, U3) the work under way that a page of Download Play waits for - the scan of the titles, the copies of the plugin, a new scan, the
// worker of the guests, the update, the public lobbies - said as Enable Titles says its own work: the card's title, the line under way, the detail,
// and the bar (`fraction` -1: the dots). false: none (pages_dlpguest.cpp; Download Play > Enable games and Join lobby show the same card).
bool WorkUnderWay(std::string& title, std::string& text, std::string& detail, float& fraction);

} // namespace pages
