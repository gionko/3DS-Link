// The interface core of the 3DS Link app: input, the state every page shares, the page stack and the widgets of the
// lists. The pages themselves live in pages_*.cpp and reach each other only through pages.hpp.
//
// Graphics: citro2d on top of citro3d (gfx.hpp). Both screens are redrawn from scratch every frame. The interactive
// list lives on the touch screen; the top screen shows the status and the note of the selected entry.
#pragma once

#include <3ds.h>

#include <string>
#include <vector>

#include "appcfg.hpp"
#include "games.hpp"
#include "gfx.hpp"
#include "paths.hpp"
#include "settings.hpp"
#include "system.hpp"

namespace ui {

namespace col = gfx::col;

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string TrimSpaces(const std::string& s);
// Monotonic milliseconds (unaffected by the console clock being changed).
u64 NowMs();

template <typename T>
T Clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

struct Rect {
    float x, y, w, h;
    bool Has(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

struct Input {
    u32 down = 0;                 // buttons pressed this frame
    u32 nav = 0;                  // KEY_UP / KEY_DOWN pressed, with auto-repeat while held
    int up_presses = 0, down_presses = 0; // how many times KEY_UP / KEY_DOWN were pressed since the last look (the latch counts them: two quick presses between two looks are two)
    bool touch_pressed = false;   // the stylus went down this frame
    bool touch_held = false;
    bool touch_released = false;  // the stylus went up this frame
    int tx = 0, ty = 0;           // touch position (the last one while released)
    int press_x = 0, press_y = 0; // where the current / last touch started
};

class InputPoller {
public:
    void Poll();
    // The screens of a transfer are drawn a few times a second, and a key pressed and let go between two looks would be lost. While the latch is
    // on, a thread of its own looks at the keys and the stylus every few milliseconds and keeps what happened until the next Poll() asks for it
    // (the thread is the only one that reads the pad then). Start and stop come from the thread that polls.
    void StartLatch();
    void StopLatch();
    // After an applet: ignore everything until the buttons and the stylus were released.
    void BlockUntilReleased();
    const Input& in() const {
        return in_;
    }

private:
    Input in_;
    bool blocked_ = false;
    int blocked_frames_ = 0;
};

bool Tapped(const Input& in, const Rect& r);
bool PressedNow(const Input& in, const Rect& r);

// ---------------------------------------------------------------------------------------------
// Application state shared by the pages
// ---------------------------------------------------------------------------------------------

struct App {
    setup::Settings cfg;
    setup::ServerList servers; // the saved rooms, the most recently used first (servers.txt in the data folder)
    bool servers_writable = true; // false when the file exists but could not be read: it is never written over
    std::string console_name;
    std::string app_path;      // where the launcher started this app from (argv[0]); empty when it does not say
    setup::Paths paths;        // every file of the app, worked out from the folder it was started from (paths.hpp)
    setup::AppConfig app_cfg;  // 3DSLink.cfg: the first start's end, the boot9.bin the user pointed to
    setup::Boot9Lookup boot9;  // where the key file is (or that it is not anywhere)
    bool first_start = false;  // there was no 3DSLink.cfg: the first start's question, scan and check come first
    bool rescan_wanted = false; // the key file appeared: read the titles again as soon as the scan is idle
    // Somebody holds the delivery module's context (the keys, the paths: the scan made it and a new scan destroys it): the page of 3DS Link Delivery while it reads
    // the app's own CIA from the installed title. The titles are not read again while it is held (main.cpp waits).
    int module_holds = 0;
    setup::LoadResult settings_loaded = setup::LoadResult::Missing; // what reading AzaharLink.ini said at the start
    bool ac_ready = false;
    Result soc_rc = -1;
    setup::NetInfo net;
    setup::ConsoleStatus status;                  // battery and SD space (main page)
    setup::InstallReport install;                 // the file check (see RefreshInstall)
    setup::TitlesHealth titles_health;            // what the scan of the titles found of no use (the check's row, the cleanup): copied when the scan is done
    bool titles_health_applied = false;           // the check was made again after the scan ended (false again when a scan is started)
    setup::LoaderState loader = setup::LoaderState::Unknown; // Rosalina's Plugin Loader
    // Luma3DS older than 13.0 without a Plugin Loader (the red "not work" of the installation check): no title can load the plugin, so the Azahar side of
    // the app (the 3DS Link switch, the room, the titles for the Azahar emulator) is shut. Read again at every start and every return to the main page.
    bool luma_too_old = false;
    std::string luma_text; // its version ("10.2.1"), "" when it cannot be read
    // The copies of the plugin in the games' folders are looked at once a start (after an update of the app a game would start the old plugin): see
    // QueuePluginCopyRefresh.
    bool plugin_copies_checked = false;
    std::string plugin_stamp_pending; // the stamp (size and bytes, setup::PluginContentStamp) of the plugin the quiet looks are being made for: written to 3DSLink.cfg when they are done
    int plugin_pass_total = 0;        // how many folders that pass looked at (the main page says how many are done)
    bool plugin_pass_open = false;    // a pass of quiet looks was queued and the main page has not told yet that it is over (how many folders are left, then the result)
    bool plugin_ready = false; // the plugin file of 3DS Link is there and is, byte for byte, the plugin inside the app (plugin_card == Same)
    // The plugin of 3DS Link: ONE package. The app carries AzaharLink.3gx (plugin_resource.hpp) and puts it on the card by itself (plugin_install.hpp, plugin_ensure.cpp).
    setup::PluginCard plugin_card = setup::PluginCard::Missing; // what the card holds at paths.plugin_file, against the plugin inside the app (as of the last look)
    bool plugin_card_known = false;
    std::string plugin_card_path;  // the file the last look was at ...
    std::string plugin_card_stamp; // ... and its size:mtime then: the card is read again (a whole megabyte) only when the file changed, or when a look is asked for
    std::string plugin_found_version; // the version its header says ("" when it has none)
    bool plugin_tried = false;     // the app tried to put its plugin on the card since it started ...
    setup::WriteResult plugin_write = setup::WriteResult::Ok; // ... and how the last try went
    std::string plugin_notice;     // what the app says once on the main page when it put the plugin on the card ("The plugin was put in sdmc:/3ds/3DSLink."), or could not
    u32 plugin_notice_color = 0;
    // The plugin on the card is not the one inside the app (missing, damaged, another version) AFTER the app tried to put it: the 3DS Link switch of the main page cannot be
    // turned ON (it goes OFF by itself when it was ON, like with a Luma3DS that is too old, and stays OFF until the file is back and the user switches it on).
    bool plugin_locked = false;
    int language = 1;                             // the console's language (CFG_Language): the names of the games are read in it
    InputPoller input;
    bool quit = false;
    bool save_ok = true;
    bool home_blocked = false; // the HOME button does nothing now (a transfer, the search for rooms): the bottom bar says so
    // HOME is switched off by whoever needs it (the list of rooms, a transfer, the copies of the plugin, the choice of what to send) and given
    // back only when the last of them lets go: two of them at once cannot give it back under each other. ApplyHome() sets the system's switch
    // and the badge from the count (it is also what puts HOME off again after the delivery module gave it back at the end of a transfer).
    int home_holds = 0;
    void HoldHome();
    void ReleaseHome();
    void ApplyHome();

    std::string toast;
    u32 toast_color = 0;
    u64 toast_until = 0;
    u64 toast_from = 0; // when it was shown: a toast too long for its place runs by (the main page)

    bool soc_ok() const {
        return R_SUCCEEDED(soc_rc);
    }
    std::string EffectiveNickname() const {
        return cfg.nickname.empty() ? console_name : cfg.nickname;
    }
    void ShowToast(const std::string& text, u32 color, u64 ms = 2500);
    bool ToastActive() const;
    // Writes AzaharLink.ini ("Saved." on success, a lasting error when it fails).
    void Save();
    // Writes the list of saved rooms (a toast only when it cannot).
    void SaveServers();
    // Writes 3DSLink.cfg. Until the first start is over (first_start_done) nothing is written: the file's presence is what says it is over.
    void SaveAppConfig();

    // What the app does once it may write: makes its data folder, reads its config, AzaharLink.ini and the saved rooms (completing a file that lacks keys), finds boot9.bin, and tells the delivery module.
    void OpenDataFolder();
    // Looks for boot9.bin again (next to the app, in GodMode9's folder, where the user pointed to) and tells the delivery module.
    void RefreshBoot9();
    // Hands the delivery module what it needs before it runs: the folders, the key file, the log switch (module_config.cpp).
    void ApplyModuleConfig();
    // AzaharLink.ini is made as soon as there is a plugin folder for it (the first start, and when the Installation check makes the folder).
    void EnsureIniFile();

    // Runs the system software keyboard. Blocks (the applet owns the screens meanwhile). Returns
    // true when the user confirmed; `value` is replaced then.
    // `numeric` shows the number pad (a port).
    bool EditText(const char* hint, std::string& value, int max_chars, bool allow_empty, bool numeric = false);

    // Looks at the files of the installation again (the plugin file, the folders, the key file, the card).
    void RefreshInstall();
    // Looks at Rosalina's Plugin Loader again.
    void RefreshLoader();
    // Puts the plugin that is inside the app on the card when the file of 3DS Link (paths.plugin_file) is missing or is not, byte for byte, that plugin: at every start (OpenDataFolder),
    // when the Installation check opens and on its Fix. The file is replaced, never kept (the app and its plugin are one version). Remembers how it went (plugin_tried, plugin_write),
    // sets plugin_notice when it put or replaced the file, and makes the copies in the titles' folders be looked at again with the new file. A card that cannot be written is
    // not an error of the app: the result says it and the app goes on.
    setup::PluginEnsure EnsurePlugin();
    // Looks at the plugin file against the plugin inside the app: the card is read in full only when `full` or when the file's size and date changed since the last look.
    void LookAtPlugin(bool full);
    // Why the switch is shut for the plugin, in one sentence ("" when it is not): setup::PluginLockText made from what the app knows.
    std::string PluginLockText() const;
    // Once per start, AT ONCE (it does not wait for the scan of the titles): every game folder of sd:/luma/plugins that holds our plugin (and nothing else) gets a quiet look in the
    // thread of the copies; a copy that is not the plugin file next to the app (an older version, or another build of the same size) is made again. Folders with another plugin
    // are never touched.
    void QueuePluginCopyRefresh();
};

extern App g;

// The room the ini names, as "host:port" ("not set" when there is none).
std::string RoomText();
// The name the user gave that room in the list of saved rooms ("" when it has none, or is not in the list).
std::string RoomName();
// RoomName() when there is one, else RoomText().
std::string RoomLabel();

// The state a switch row goes to: A or a tap (step 0) flips it, D-pad right (+1) sets it ON, left (-1) OFF.
inline bool SwitchTarget(bool now, int step) {
    return step == 0 ? !now : step > 0;
}

// ---------------------------------------------------------------------------------------------
// Page stack
// ---------------------------------------------------------------------------------------------

class Page {
public:
    virtual ~Page() {}
    virtual void Update(const Input& in) = 0;
    virtual void DrawTop() = 0;
    virtual void DrawBottom() = 0;
    // The page above this one was popped: this is the current page again (the main page looks at the installation again).
    virtual void OnResume() {}
};

// Takes effect before the next Update(), so a page is always updated once before it is drawn.
void PushPage(Page* page);
void PopPage();
// Takes `count` pages off at once (the page that remains gets OnResume): leaving a page and the menu it came from in one go.
void PopPages(int count);
// The page the app starts on (it can never be popped).
void SetRootPage(Page* page);
// The page under the current one (a popup draws it first and its own card over it), or nullptr when there is none.
Page* PageBelow();
// Takes the place of the root page (and of whatever is above it) before the next Update(): the start-up flow hands over to the
// main page this way.
void ReplaceRootPage(Page* page);
void ApplyPageRequests();
Page& CurrentPage();
// Leaves the room if a test is still running: call before the services are shut down.
void ClearPages();

// ---------------------------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------------------------

constexpr float kBarH = 24.0f;
constexpr float kListTop = 26.0f;
constexpr float kListBottom = 213.0f;
constexpr float kRowH = 28.0f;
constexpr float kRowX = 6.0f;
constexpr float kRowW = 308.0f;
// The footer of the test page: [B Close] [A Test again] (B on the left, A on the right, as the buttons are on the console).
const Rect kLeftButton = {8.0f, 216.0f, 148.0f, 20.0f};
const Rect kRightButton = {164.0f, 216.0f, 148.0f, 20.0f};
// The footer of every other page: [B Back] [A action] hint text [START Save & exit].
const Rect kBackButton = {8.0f, 216.0f, 68.0f, 20.0f};
const Rect kStartButton = {208.0f, 216.0f, 104.0f, 20.0f};
// The one button of a popup that only informs (or only lets the user cancel).
const Rect kPopupOk = {84.0f, 176.0f, 152.0f, 30.0f};

// Check: a row of a list to choose from (a box at the right, empty, half full or ticked, and the value before it).
enum class Kind { Toggle, Value, Link, Action, Info, Check };
constexpr unsigned kTagGame = 1, kTagUpdate = 2, kTagDlc = 4;
// A mark before the label (the room in use): Slot only keeps the room free so that all labels of a list line up.
enum class Mark { None, Slot, Dot };

struct Row {
    Kind kind = Kind::Info;
    int id = 0;
    std::string label;
    std::string value;
    u32 value_color = 0; // 0 = default
    bool on = false;     // Toggle
    Mark mark = Mark::None;
    float gap = 0.0f;    // extra space above the row
    const u16* icon = nullptr; // a title's icon (48x48, SMDH data: it must outlive the row), drawn at half size before the label
    bool dim = false;    // a row that cannot be used now: label and icon are drawn faint
    // What the row says instead of its label for a while ("Enabling...", "Enabled"): empty = the label. `busy` adds moving dots.
    std::string status_text;
    u32 status_color = 0;
    bool busy = false;
    // Check rows: `on` is a ticked box, `partial` a box with a bar (some of what the row stands for is chosen); `indent` moves the row's
    // content right (the parts of a game under its name); `chip` is a small tag after the label ("installed", "update").
    bool partial = false;
    float indent = 0.0f;
    // Check rows: a second column at the far right (before the box), of a fixed width and right-aligned: a time next to a size, so that the
    // size does not move with the words of the time.
    std::string detail;
    std::string chip;
    u32 chip_color = 0;
    // The kinds of title the row stands for, as small coloured bars before the value: kTagGame | kTagUpdate | kTagDlc.
    unsigned tags = 0;
    u32 label_color = 0; // the colour of the label (0 = the usual one)
    std::string note_title;
    u32 note_color = 0; // the colour of the note's title (0 = the accent colour)
    std::string note;
};

struct ListView {
    int sel = 0;
    int last_sel = -1;      // the row the selection was on at the last update: when it differs, `sel_since` is set again
    u64 sel_since = 0;      // when the selection came to the row (the long label of the selected row runs from then)
    float scroll = 0.0f;
    float scroll_target = 0.0f;
    float highlight_y = -1.0f; // animated, in content coordinates
    bool follow = true;        // keep the selection in view
    bool touching = false;
    bool dragging = false;
    float drag_start_scroll = 0.0f;
    int press_row = -1;
};

struct ListEvent {
    int activated = -1; // A or a tap: the row to act on, or -1
    int step = 0;       // D-pad right = +1, left = -1: applies to the selected row (ListView::sel)
};

// Handles D-Pad/Circle Pad (up, down), D-Pad left and right (the step), A and the stylus (tap = activate, drag = scroll).
ListEvent UpdateList(ListView& v, const std::vector<Row>& rows, const Input& in);
void DrawRows(const ListView& v, const std::vector<Row>& rows);

// ---------------------------------------------------------------------------------------------
// Pieces of the screens
// ---------------------------------------------------------------------------------------------

void DrawTitleBar(float width, const char* title, const std::string& right);
// The bar of a top screen that shows the console's state at its right: the battery and the free space of the SD card. The title is at the left
// with the app's version after it in a smaller type.
// `wifi_channel` (1, 6 or 11; 0 = not known: nothing is drawn): the channel of the room, "Wi-Fi CH 6", to the left of the battery.
void DrawTitleBarStatus(float width, const char* title, int wifi_channel = 0);
// The legend of the kinds of title, "Game  Update  DLC", each with its small coloured bar (the one DrawKindMarks draws), ending at `right_x`, centred on `yc`.
void DrawKindLegend(float right_x, float yc);
// The kinds a title (or a row) stands for (kTagGame | kTagUpdate | kTagDlc) as small coloured bars with no letter, blue / green / red, ending at `right_edge`.
// Returns where they begin. The one way the app shows them.
float DrawKindMarks(float right_edge, float yc, unsigned tags);
void DrawFrameBorder(float width);
void DrawCard(float x, float y, float w, float h);
void DrawDotText(float x, float yc, u32 dot, u32 text_color, const std::string& text, float scale, float max_w);
// A bar that is `fraction` (0..1) full.
void DrawProgressBar(float x, float y, float w, float h, float fraction);
// Three bars of rising height, the first `level` (0..3) lit: the WiFi signal. 14 pixels wide.
void DrawSignalBars(float x, float yc, int level);
// A touch button with a key chip, e.g. [B] Back. `compact` is the smaller type of the START button of the footer.
void DrawButton(const Rect& r, const char* key, const char* label, bool enabled, bool pressed, bool compact = false);
// The bar, the footer and the border are drawn after the rows so they cover the rows that scroll past the edges.
void DrawListChrome(const char* title, const std::string& right);
// The footer of a page: [B Back] (not on the first page), [A action] (when the page has one), the key hints, [START Save & exit].
// A button after A that does what a key of the pad does (X, Y, L, R): [X Parts].
struct FooterKey {
    const char* key = "";
    const char* label = "";
    u32 mask = 0; // the pad key: KEY_X ...
};
struct Footer {
    bool back = true;
    const char* back_label = "Back";
    const char* action_label = ""; // "" = no A button
    FooterKey extra[3];            // more buttons after A
    int n_extra = 0;
    const char* hint = "";         // the keys that are not buttons ("X del  Y add")
    bool start = true;             // false: no START button (a page with extra buttons has no room for it)
    const char* start_label = "Save & exit";
    bool start_enabled = true;     // false: the button is there but greyed, and START does nothing (the copies of the plugin are being made)
};
// Where the buttons of a footer are. Every button is a touch button too: a tap does what its key does.
struct FooterRects {
    Rect back{0, 0, 0, 0};
    Rect action{0, 0, 0, 0};
    Rect extra[3] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    Rect start{0, 0, 0, 0};
    float hint_x = 12.0f;  // where the hint text begins
    float hint_end = 200.0f;
};
FooterRects LayoutFooter(const Footer& footer);
void DrawFooter(const Footer& footer);
// The strip behind the buttons of a footer.
void DrawFooterBackground();
// Four dots, one of them lit, that move on while the app works: in the middle of the touch screen, `yc` high.
void DrawSpinner(float yc);
// The touch screen of a page that only waits: a title bar, the moving dots and the footer.
void DrawWaitBottom(const Footer& footer);
// Where the A button of a footer is for this label. Empty label: a rectangle nothing is in. `back` says whether the footer has the B button
// at its left (the first page has none).
Rect ActionButton(const char* label, bool back);

// A popup on the touch screen: what is under it dimmed, a card with a title and a text (at most `max_lines` lines; the card is `h` high).
// The caller draws its buttons.
void DrawDim(float width);
void DrawPopupCard(float y, float h, const std::string& title, const std::string& text, int max_lines);

// A popup that fits what it says: a card in the middle of the touch screen (below its bar), the title, the text, and the buttons under the text,
// all inside the card. The card is as high as its text needs; the buttons are where LayoutPopup puts them (the page tests taps with the same
// rectangles it draws).
struct PopupBox {
    float x = 16.0f, y = 0.0f, w = 288.0f, h = 0.0f;
    float scale = 0.5f; // the text's size
    int lines = 0;       // how many lines of the text are shown
    Rect yes{0, 0, 0, 0}, no{0, 0, 0, 0}, ok{0, 0, 0, 0};
};
PopupBox LayoutPopup(const std::string& text, bool two_buttons, float scale = 0.5f, int max_lines = 9);
void DrawPopupBox(const PopupBox& box, const std::string& title, const std::string& text);

// The note panel of a row (top screen).
void DrawNote(float x, float y, float w, float h, const Row& row);
// One line on the top screen: the last message when there is one, else "3DS Link ON/OFF" and the room.
void DrawStatusStrip(float x, float y, float w, float h);
// Width reserved for the dim captions ("WiFi", "Room"...) of the two-column status lines.
float CaptionWidth();
void DrawNetworkLine(float x, float yc, float max_w);

// The shape of every page but the connection test: a list on the touch screen, the note of the selected row on the top
// screen, [B Back] (all but the first page) and [START Save & exit] under the list. A page only builds its rows and says
// what an activation does.
class ListPage : public Page {
public:
    void Update(const Input& in) override;
    void DrawTop() override;
    void DrawBottom() override;

protected:
    virtual const char* Title() const = 0;
    // The title of the bar of the touch screen (the page's title unless it says otherwise).
    virtual const char* BarTitle() const {
        return Title();
    }
    virtual std::string BarRight() const {
        return std::string();
    }
    // The key hints between the footer buttons.
    virtual const char* Hint() const {
        return "";
    }
    // The label of the A button of the footer ("" = no button) and of the other two.
    virtual const char* ActionLabel() const {
        return "Select";
    }
    virtual const char* BackLabel() const {
        return "Back";
    }
    virtual const char* StartLabel() const {
        return "Save & exit";
    }
    // Whether the footer has the START button (Save & exit). The pages of Titles Delivery have none: leaving the app is done from the main page.
    virtual bool StartButton() const {
        return true;
    }
    // Whether START does anything now: not while the plugin is being copied into the folders of the titles by the user's own choice (Enable, Disable: leaving then would cut
    // the copies). During the pass over the games that follows an update (plugin_pass_open) START works but asks first (AskLeaveDuringPluginPass).
    virtual bool StartEnabled() const;
    // "The plugin is still being put in your games: leave anyway?": Leave drops the looks that wait (CopyWorker::CancelRefreshes) and does what START does (OnStart).
    void AskLeaveDuringPluginPass();
    // Buttons after A (X, Y...): fills `out` and says how many. A page that has some has no START button.
    virtual int ExtraKeys(FooterKey* out) const {
        (void)out;
        return 0;
    }
    virtual void OnExtra(int index, const Row* selected) {
        (void)index;
        (void)selected;
    }
    // B (or a tap on the Back button) and START (or a tap on its button). A page that is a step of a flow overrides them.
    virtual void OnBack() {
        PopPage();
    }
    virtual void OnStart() {
        g.quit = true;
    }
    // The first page has no Back, and B does nothing there.
    virtual bool IsRoot() const {
        return false;
    }
    virtual void BuildRows(std::vector<Row>& rows) = 0;
    // A or a tap (step 0), D-pad right (+1) or left (-1) on `row`, the selected one.
    virtual void Activate(const Row& row, int step) = 0;
    // The buttons the list does not use (X, Y...).
    virtual void OnKeys(const Input& in) {
        (void)in;
    }

    const Row* Selected() const;
    Footer BuildFooter() const;

    ListView list_;
    std::vector<Row> rows_;
};

} // namespace ui
