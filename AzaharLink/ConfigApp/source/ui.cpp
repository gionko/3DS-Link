#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "azl/platform.hpp"
#include "copyworker.hpp"
#include "pages.hpp"
#include "plugin_resource.hpp"
#include "titles.hpp"

namespace ui {

std::string Fmt(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

std::string TrimSpaces(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

u64 NowMs() {
    return azl::platform::NowMs();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

namespace {

// What the thread of the latch has seen since the last Poll().
struct Latch {
    LightLock lock;
    u32 down = 0, up = 0, held = 0;
    int up_n = 0, down_n = 0; // presses of KEY_UP / KEY_DOWN since the last Poll()
    int tx = 0, ty = 0, press_x = 0, press_y = 0;
    volatile bool stop = false;
    bool on = false;
    Thread thread = nullptr;
    u64 nav_since = 0, nav_last = 0; // the auto-repeat of up and down while held (the thread scans, so hidKeysDownRepeat is not there)
};
Latch latch;

void LatchThread(void*) {
    while (!latch.stop) {
        hidScanInput();
        const u32 down = hidKeysDown();
        const u32 held = hidKeysHeld();
        const u32 up = hidKeysUp();
        touchPosition tp = {};
        if (held & KEY_TOUCH) {
            hidTouchRead(&tp);
        }
        LightLock_Lock(&latch.lock);
        latch.down |= down;
        latch.up |= up;
        latch.up_n += (down & KEY_UP) ? 1 : 0;
        latch.down_n += (down & KEY_DOWN) ? 1 : 0;
        latch.held = held;
        if (held & KEY_TOUCH) {
            latch.tx = tp.px;
            latch.ty = tp.py;
            if (down & KEY_TOUCH) {
                latch.press_x = tp.px;
                latch.press_y = tp.py;
            }
        }
        LightLock_Unlock(&latch.lock);
        svcSleepThread(8LL * 1000 * 1000);
    }
}

} // namespace

void InputPoller::StartLatch() {
    if (latch.on) {
        return;
    }
    LightLock_Init(&latch.lock);
    latch.down = latch.up = latch.held = 0;
    latch.up_n = latch.down_n = 0;
    latch.stop = false;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    latch.thread = threadCreate(LatchThread, nullptr, 0x2000, priority, -2, false);
    latch.on = latch.thread != nullptr; // (without a thread the keys are read the plain way)
}

void InputPoller::StopLatch() {
    if (!latch.on) {
        return;
    }
    latch.stop = true;
    threadJoin(latch.thread, U64_MAX);
    threadFree(latch.thread);
    latch.thread = nullptr;
    latch.on = false;
}

void InputPoller::Poll() {
    u32 down, held, up, repeat;
    int up_n = 0, down_n = 0;
    if (latch.on) {
        LightLock_Lock(&latch.lock);
        down = latch.down;
        up = latch.up;
        held = latch.held;
        up_n = latch.up_n;
        down_n = latch.down_n;
        latch.up_n = latch.down_n = 0;
        latch.down = latch.up = 0;
        const int tx = latch.tx, ty = latch.ty, px = latch.press_x, py = latch.press_y;
        LightLock_Unlock(&latch.lock);
        // Up and down repeat while they are held (after 400 ms, ten times a second), as they do outside the transfers.
        const u32 nav_keys = KEY_UP | KEY_DOWN;
        const u64 now = NowMs();
        repeat = down;
        if (!(held & nav_keys)) {
            latch.nav_since = 0;
        } else if (down & nav_keys) {
            latch.nav_since = latch.nav_last = now;
        } else if (latch.nav_since != 0 && now - latch.nav_since >= 400 && now - latch.nav_last >= 100) {
            repeat |= held & nav_keys;
            latch.nav_last = now;
        }
        if ((held | up | down) & KEY_TOUCH) {
            in_.tx = tx;
            in_.ty = ty;
            if (down & KEY_TOUCH) {
                in_.press_x = px;
                in_.press_y = py;
            }
        }
    } else {
        hidScanInput();
        down = hidKeysDown();
        held = hidKeysHeld();
        up = hidKeysUp();
        repeat = hidKeysDownRepeat();
        up_n = (down & KEY_UP) ? 1 : 0;
        down_n = (down & KEY_DOWN) ? 1 : 0;
    }

    if (blocked_) {
        // After an applet: ignore everything until the buttons and the stylus were released, so
        // the tap that closed the keyboard does not also press a row of our list.
        ++blocked_frames_;
        const u32 watched = KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT | KEY_L | KEY_R |
                            KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_TOUCH;
        if (blocked_frames_ >= 4 && !(held & watched)) {
            blocked_ = false;
        }
        in_.down = in_.nav = 0;
        in_.up_presses = in_.down_presses = 0;
        in_.touch_pressed = in_.touch_held = in_.touch_released = false;
        return;
    }

    in_.down = down;
    in_.up_presses = up_n;
    in_.down_presses = down_n;
    in_.nav = repeat & (KEY_UP | KEY_DOWN);
    in_.touch_pressed = (down & KEY_TOUCH) != 0;
    in_.touch_held = (held & KEY_TOUCH) != 0;
    in_.touch_released = (up & KEY_TOUCH) != 0;
    if (in_.touch_held && !latch.on) {
        touchPosition tp;
        hidTouchRead(&tp);
        in_.tx = tp.px;
        in_.ty = tp.py;
        if (in_.touch_pressed) {
            in_.press_x = in_.tx;
            in_.press_y = in_.ty;
        }
    }
}

void InputPoller::BlockUntilReleased() {
    blocked_ = true;
    blocked_frames_ = 0;
}

bool Tapped(const Input& in, const Rect& r) {
    return in.touch_released && r.Has(static_cast<float>(in.press_x), static_cast<float>(in.press_y)) &&
           r.Has(static_cast<float>(in.tx), static_cast<float>(in.ty));
}

bool PressedNow(const Input& in, const Rect& r) {
    return in.touch_held && r.Has(static_cast<float>(in.press_x), static_cast<float>(in.press_y)) &&
           r.Has(static_cast<float>(in.tx), static_cast<float>(in.ty));
}

// ---------------------------------------------------------------------------------------------
// Application state shared by the pages
// ---------------------------------------------------------------------------------------------

App g;

void App::ShowToast(const std::string& text, u32 color, u64 ms) {
    toast = text;
    toast_color = color;
    toast_from = NowMs();
    toast_until = toast_from + setup::ToastMsToRead(text.size(), ms);
}

bool App::ToastActive() const {
    return !toast.empty() && NowMs() < toast_until;
}

void App::Save() {
    if (settings_loaded == setup::LoadResult::Unreadable) {
        // The file is there but could not be read at the start: what is in it is not known, so what is held here (the defaults and this change)
        // must not replace it.
        save_ok = false;
        ShowToast("The settings file could not be read: it is left as it is. Restart the app.", col::kWarn, 6000);
        return;
    }
    save_ok = setup::SaveSettings(cfg);
    if (save_ok) {
        ShowToast("Saved.", col::kOn, 1800);
    } else {
        ShowToast("Could not write the settings file!", col::kOff, 8000);
    }
}

void App::SaveServers() {
    if (!servers_writable) {
        ShowToast("The servers file could not be read: it is left as it is.", col::kWarn, 6000);
    } else if (!setup::SaveServers(paths.servers_file, servers)) {
        ShowToast("Could not write the list of rooms!", col::kOff, 8000);
    }
}

void App::SaveAppConfig() {
    if (!app_cfg.first_start_done) {
        return; // (the first start is not over: the file's absence is what keeps it going)
    }
    if (!setup::SaveAppConfig(paths.config_file, app_cfg)) {
        ShowToast("Could not write 3DSLink.cfg!", col::kOff, 6000);
    }
}

void App::EnsureIniFile() {
    // The settings file is made by itself where the plugin could be installed (no plugin folder, no file), and a file that lacks keys is
    // completed once, without a word: afterwards it lists every key, so a hand edit (FTP) only flips a value. A file that could not be
    // read is never written over.
    if ((settings_loaded == setup::LoadResult::Missing && setup::PluginsDirExists()) ||
        (settings_loaded == setup::LoadResult::Loaded && !setup::HasAllKeys(cfg))) {
        save_ok = setup::SaveSettings(cfg);
        if (save_ok) {
            settings_loaded = setup::LoadResult::Loaded;
            cfg.keys_seen = (1u << azl::ini::kKeyCount) - 1;
        }
    }
}

void App::OpenDataFolder() {
    setup::MakeDirs(paths.datadir);
    // (what a receiving that was cut off by a power cut left in park/: the delivery module cleans it at the next receiving, but it can be tens of MB)
    setup::RemoveParkFiles(paths.park_dir);
    setup::LoadAppConfig(paths.config_file, app_cfg);
    EnsureIniFile();
    // The saved rooms. Without a file the list starts with the room the settings name; a file that could not be read is never written over.
    const setup::LoadResult servers_loaded = setup::LoadServers(paths.servers_file, servers);
    servers_writable = servers_loaded != setup::LoadResult::Unreadable;
    if (servers_loaded == setup::LoadResult::Missing && !cfg.host.empty()) {
        setup::Server current;
        current.host = cfg.host;
        current.port = cfg.port;
        current.password = cfg.password;
        servers.Add(current);
        SaveServers();
    } else if (servers_loaded == setup::LoadResult::Loaded && !cfg.host.empty() && !cfg.password.empty()) {
        // Before the password belonged to the room there was ONE password, in AzaharLink.ini: it was the password of the room in use. The room in
        // the list takes it once (it has none yet), so that nothing is lost.
        setup::Server active;
        active.host = cfg.host;
        active.port = cfg.port;
        const int at = servers.Find(active);
        if (at >= 0 && servers.entries()[static_cast<std::size_t>(at)].password.empty()) {
            setup::Server with_password = servers.entries()[static_cast<std::size_t>(at)];
            with_password.password = cfg.password;
            servers.Replace(static_cast<std::size_t>(at), with_password);
            SaveServers();
        }
    }
    RefreshBoot9();
    // 3DS Link is ONE package: the plugin is inside the app, and the app puts it on the card (a start that is not the first, here; the first start, after its question was
    // answered Yes). What was done is said once on the main page (plugin_notice).
    EnsurePlugin();
}

void App::RefreshBoot9() {
    const bool was_found = boot9.found;
    boot9 = setup::FindBoot9(paths, app_cfg.boot9_path);
    ApplyModuleConfig();
    if (boot9.found && !was_found && titles::Scan().started()) {
        rescan_wanted = true; // (the scan that has run did not have the key: names and icons are read again)
    }
}

bool App::EditText(const char* hint, std::string& value, int max_chars, bool allow_empty, bool numeric) {
    SwkbdState swkbd;
    swkbdInit(&swkbd, numeric ? SWKBD_TYPE_NUMPAD : SWKBD_TYPE_NORMAL, numeric ? 1 : 2, max_chars);
    swkbdSetValidation(&swkbd, allow_empty ? SWKBD_ANYTHING : SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetInitialText(&swkbd, value.c_str());
    swkbdSetFeatures(&swkbd, SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET | SWKBD_ALLOW_POWER);
    char buffer[512] = {};
    const SwkbdButton button = swkbdInputText(&swkbd, buffer, sizeof(buffer));
    // The graphics state was handed over to the applet and back: put citro2d's state right and
    // swallow the stylus tap that closed the keyboard.
    gfx::ResumeAfterApplet();
    input.BlockUntilReleased();
    if (button != SWKBD_BUTTON_CONFIRM) {
        return false;
    }
    buffer[sizeof(buffer) - 1] = '\0';
    value = buffer;
    return true;
}

void App::RefreshInstall() {
    LookAtPlugin(false); // (the plugin may have been put next to the app, or in its own folder, since; the card is read in full only when the file changed)
    setup::SetCurrentPaths(paths);
    RefreshBoot9();
    RefreshLoader();
    setup::InstallFacts facts = setup::ReadInstallFacts(paths, boot9, loader);
    facts.plugin_card = plugin_card;
    facts.embedded_ok = setup::EmbeddedPluginUsable();
    facts.plugin_tried = plugin_tried;
    facts.plugin_write = plugin_write;
    if (titles::Scan().done()) {
        facts.titles = titles::Scan().report().health; // (the row of the titles: what the scan found of no use)
        titles_health = facts.titles;
    }
    install = setup::EvaluateInstall(facts, setup::kVersion);
    plugin_ready = plugin_card == setup::PluginCard::Same;
    luma_text = setup::LumaVersionText(facts.luma);
    luma_too_old = setup::ClassifyLuma(facts.luma, facts.loader) == setup::LumaSupport::NotWork;
    plugin_locked = setup::LockOfSwitch(luma_too_old, plugin_ready) == setup::SwitchLock::Plugin;
    if (luma_too_old && cfg.enabled) {
        // The plugin can never load on this Luma3DS: 3DS Link goes OFF by itself (once: after that the file already says enabled=0) and stays OFF when
        // Luma3DS is updated, until the user switches it on. A file that was not read is left alone, and a missing one is not made for this.
        cfg.enabled = false;
        if (settings_loaded == setup::LoadResult::Loaded) {
            Save();
        }
        ShowToast("This Luma is not supported", col::kWarn, 6000);
    } else if (plugin_locked && cfg.enabled) {
        // The same for a plugin that is not the plugin of the app (the app has already tried to put it on the card, at the start or in the Installation check): 3DS Link
        // cannot be ON without it. It goes OFF by itself, once, and stays OFF when the file is back, until the user switches it on.
        cfg.enabled = false;
        if (settings_loaded == setup::LoadResult::Loaded) {
            Save();
        }
        ShowToast(PluginLockText(), col::kWarn, 12000);
    }
    QueuePluginCopyRefresh();
}

void App::QueuePluginCopyRefresh() {
    if (plugin_copies_checked || !plugin_ready) {
        // (it does NOT wait for the scan of the titles any more: whoever has just installed an update waits for THIS, and the scan of a big library takes minutes; the thread of
        // the copies and the scan read the card side by side, which only makes the scan a little slower)
        return;
    }
    plugin_copies_checked = true;
    // The copies were brought up to this very plugin (its size and its bytes: PluginContentStamp) on an earlier start, and 3DSLink.cfg says so: nothing to read. A plugin that was
    // replaced has another stamp, and the look is made (and the new stamp written when it is done: main.cpp). The plugin file on the card is, byte for byte, the plugin inside the
    // app here (plugin_ready), so the stamp is made from the bytes in memory.
    const std::string stamp = setup::PluginContentStamp(setup::EmbeddedPluginData(), setup::EmbeddedPluginSize());
    if (!stamp.empty() && stamp == app_cfg.plugin_stamp) {
        return;
    }
    plugin_stamp_pending = stamp;
    setup::CopyWorker& worker = setup::CopyWorker::Get();
    int submitted = 0;
    for (const std::string& name : setup::ListPluginFolders()) {
        char* end = nullptr;
        const u64 tid = std::strtoull(name.c_str(), &end, 16);
        if (end == name.c_str() || *end != '\0') {
            continue;
        }
        worker.SubmitRefresh(tid, paths.plugin_file); // (the work, reading the folder and the two files, is the thread's)
        ++submitted;
    }
    plugin_pass_total = submitted;
    plugin_pass_open = submitted > 0; // (the main page tells how it goes and when it is over)
}

void App::RefreshLoader() {
    loader = setup::ReadPluginLoader();
}

void App::ApplyHome() {
    home_blocked = home_holds > 0;
    aptSetHomeAllowed(home_holds == 0);
}

void App::HoldHome() {
    ++home_holds;
    ApplyHome();
}

void App::ReleaseHome() {
    if (home_holds > 0) {
        --home_holds;
    }
    ApplyHome();
}

std::string RoomText() {
    return g.cfg.host.empty() ? std::string("not set") : g.cfg.host + ":" + std::to_string(g.cfg.port);
}

std::string RoomName() {
    setup::Server active;
    active.host = g.cfg.host;
    active.port = g.cfg.port;
    const int at = g.cfg.host.empty() ? -1 : g.servers.Find(active);
    return at < 0 ? std::string() : g.servers.entries()[static_cast<std::size_t>(at)].name;
}

std::string RoomLabel() {
    const std::string name = RoomName();
    return name.empty() ? RoomText() : name;
}

// ---------------------------------------------------------------------------------------------
// Page stack
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<std::unique_ptr<Page>> g_pages;
Page* g_push_request = nullptr;
Page* g_replace_request = nullptr;
bool g_pop_request = false;
int g_pop_extra = 0; // more pages to take off after the first (PopPages)

} // namespace

void ReplaceRootPage(Page* page) {
    delete g_replace_request;
    g_replace_request = page;
}

void PushPage(Page* page) {
    delete g_push_request; // a second request in the same frame replaces the first
    g_push_request = page;
}

void PopPage() {
    g_pop_request = true;
}

void PopPages(int count) {
    g_pop_request = true;
    g_pop_extra = count > 1 ? count - 1 : 0;
}

void SetRootPage(Page* page) {
    g_pages.emplace_back(page);
}

Page* PageBelow() {
    return g_pages.size() >= 2 ? g_pages[g_pages.size() - 2].get() : nullptr;
}

void ApplyPageRequests() {
    if (g_replace_request) {
        g_pages.clear();
        g_pages.emplace_back(g_replace_request);
        g_replace_request = nullptr;
        g_pop_request = false;
        delete g_push_request;
        g_push_request = nullptr;
        return;
    }
    if (g_pop_request && g_pages.size() > 1) {
        g_pages.pop_back();
        while (g_pop_extra > 0 && g_pages.size() > 1) {
            g_pages.pop_back();
            --g_pop_extra;
        }
        g_pages.back()->OnResume();
    }
    g_pop_request = false;
    g_pop_extra = 0;
    if (g_push_request) {
        g_pages.emplace_back(g_push_request);
        g_push_request = nullptr;
    }
}

Page& CurrentPage() {
    return *g_pages.back();
}

void ClearPages() {
    delete g_push_request;
    g_push_request = nullptr;
    delete g_replace_request;
    g_replace_request = nullptr;
    g_pages.clear();
}

// ---------------------------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------------------------

namespace {

void LayoutRows(const std::vector<Row>& rows, std::vector<float>& ys, float& total) {
    ys.resize(rows.size());
    float pos = 0.0f;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        pos += rows[i].gap;
        ys[i] = pos;
        pos += kRowH;
    }
    total = pos;
}

int RowAt(const std::vector<float>& ys, float scroll, float y) {
    const float content_y = y - kListTop + scroll;
    for (std::size_t i = 0; i < ys.size(); ++i) {
        if (content_y >= ys[i] && content_y < ys[i] + kRowH - 2.0f) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

ListEvent UpdateList(ListView& v, const std::vector<Row>& rows, const Input& in) {
    ListEvent event;
    const int n = static_cast<int>(rows.size());
    if (n == 0) {
        return event;
    }
    v.sel = Clamp(v.sel, 0, n - 1);

    std::vector<float> ys;
    float total = 0.0f;
    LayoutRows(rows, ys, total);
    const float view_h = kListBottom - kListTop;
    const float max_scroll = total > view_h ? total - view_h : 0.0f;

    if (in.nav & KEY_UP) {
        v.sel = (v.sel + n - 1) % n;
        v.follow = true;
    }
    if (in.nav & KEY_DOWN) {
        v.sel = (v.sel + 1) % n;
        v.follow = true;
    }
    if (in.down & KEY_A) {
        event.activated = v.sel;
    }
    // Only the D-pad sets a switch: a Circle Pad pushed a little sideways while scrolling must not flip anything.
    if (in.down & KEY_DRIGHT) {
        event.step = 1;
    } else if (in.down & KEY_DLEFT) {
        event.step = -1;
    }

    const float touch_y = static_cast<float>(in.ty);
    if (in.touch_pressed && touch_y >= kListTop && touch_y < kListBottom) {
        v.touching = true;
        v.dragging = false;
        v.drag_start_scroll = v.scroll;
        v.press_row = RowAt(ys, v.scroll, static_cast<float>(in.ty));
    }
    if (v.touching && in.touch_held) {
        const float dy = static_cast<float>(in.ty - in.press_y);
        if (!v.dragging && std::fabs(dy) > 8.0f) {
            v.dragging = true;
        }
        if (v.dragging) {
            v.scroll = Clamp(v.drag_start_scroll - dy, 0.0f, max_scroll);
            v.scroll_target = v.scroll;
            v.follow = false;
        }
    }
    if (v.touching && in.touch_released) {
        if (!v.dragging && v.press_row >= 0 && v.press_row < n) {
            v.sel = v.press_row;
            v.follow = true;
            event.activated = v.press_row;
        }
        v.touching = false;
        v.dragging = false;
        v.press_row = -1;
    }
    if (v.touching && !in.touch_held && !in.touch_released) {
        v.touching = false; // the touch ended while input was blocked
        v.dragging = false;
        v.press_row = -1;
    }

    if (v.sel != v.last_sel) {
        v.last_sel = v.sel;
        v.sel_since = NowMs(); // (a label too long for its row runs from here)
    }

    if (v.follow && !v.dragging) {
        const float top = ys[v.sel];
        const float bottom = top + kRowH;
        const float margin = kRowH * 0.6f;
        float st = v.scroll_target;
        if (top - margin < st) {
            st = top - margin;
        } else if (bottom + margin > st + view_h) {
            st = bottom + margin - view_h;
        }
        v.scroll_target = Clamp(st, 0.0f, max_scroll);
    }
    v.scroll_target = Clamp(v.scroll_target, 0.0f, max_scroll);
    if (!v.dragging) {
        const float diff = v.scroll_target - v.scroll;
        v.scroll = std::fabs(diff) < 0.5f ? v.scroll_target : v.scroll + diff * 0.3f;
    }

    const float want = ys[v.sel];
    if (v.highlight_y < 0.0f) {
        v.highlight_y = want;
    } else {
        const float diff = want - v.highlight_y;
        v.highlight_y = std::fabs(diff) < 0.5f ? want : v.highlight_y + diff * 0.4f;
    }
    return event;
}

namespace {

void DrawSwitch(float x, float yc, bool on, bool dim = false) {
    const float w = 34.0f, r = 8.0f;
    const u32 track = dim ? gfx::Mix(col::kPanel, col::kTrack, 0.6f) : (on ? col::kOn : col::kTrack);
    gfx::FillRect(x + r, yc - r, w - 2 * r, 2 * r, track);
    gfx::FillCircle(x + r, yc, r, track);
    gfx::FillCircle(x + w - r, yc, r, track);
    gfx::FillCircle(on ? x + w - r : x + r, yc, r - 2.5f, dim ? col::kDim : col::kKnob);
}

// A box of 16 pixels: empty, ticked, or with a bar (a part of what the row stands for is chosen).
void DrawCheckBox(float x, float yc, bool on, bool partial) {
    const float size = 16.0f;
    const bool lit = on || partial;
    gfx::FillRect(x, yc - size * 0.5f, size, size, lit ? col::kOn : col::kPanelAlt);
    gfx::StrokeRect(x, yc - size * 0.5f, size, size, 1.0f, lit ? col::kOn : col::kTrack);
    if (on) {
        gfx::Line(x + 3.5f, yc, x + 6.5f, yc + 3.5f, 2.0f, col::kBg);
        gfx::Line(x + 6.5f, yc + 3.5f, x + 12.5f, yc - 4.0f, 2.0f, col::kBg);
    } else if (partial) {
        gfx::FillRect(x + 4.0f, yc - 1.0f, size - 8.0f, 2.0f, col::kBg);
    }
}

// A small tag: a tinted plate with the word in the same colour.
void DrawChip(float x, float yc, float w, const std::string& text, u32 color) {
    gfx::FillRect(x, yc - 8.0f, w, 16.0f, gfx::Mix(col::kPanel, color, 0.2f));
    gfx::StrokeRect(x, yc - 8.0f, w, 16.0f, 1.0f, gfx::Mix(col::kPanel, color, 0.45f));
    gfx::DrawText(x + w * 0.5f, yc, 0.34f, color, text, gfx::Align::Center);
}

void DrawChevron(float x, float yc, u32 color) {
    gfx::Line(x, yc - 5.0f, x + 5.0f, yc, 2.0f, color);
    gfx::Line(x + 5.0f, yc, x, yc + 5.0f, 2.0f, color);
}

// The label of a row: always the one size. What does not fit is cut with "...", and on the selected row it runs by (MarqueeText), so the
// whole of it can be read. A row that has something to say (status_text) says it in place of the label.
void DrawRowLabel(const Row& r, float left, float yc, float scale, float max_w, u32 color, bool selected, u64 selected_since) {
    if (!r.status_text.empty()) {
        const u32 status_color = r.status_color ? r.status_color : color;
        gfx::DrawText(left, yc, scale, status_color, gfx::Ellipsize(r.status_text, scale, max_w));
        if (r.busy) {
            // Three dots that come one after the other, after the text.
            const float x = left + gfx::TextWidth(r.status_text, scale) + 2.0f;
            const int shown = 1 + static_cast<int>((NowMs() / 250) % 3);
            for (int i = 0; i < 3; ++i) {
                gfx::FillCircle(x + 4.0f + 7.0f * static_cast<float>(i), yc + 4.0f, 1.8f, i < shown ? status_color : col::kTrack);
            }
        }
        return;
    }
    if (selected && gfx::TextWidth(r.label, scale) > max_w) {
        gfx::DrawText(left, yc, scale, color, gfx::MarqueeText(r.label, scale, max_w, NowMs() - selected_since));
        return;
    }
    gfx::DrawText(left, yc, scale, color, gfx::Ellipsize(r.label, scale, max_w));
}

} // namespace

void DrawFooterBackground() {
    gfx::FillRect(0, 214, gfx::kBottomW, 26, col::kBg);
    gfx::FillRect(0, 213, gfx::kBottomW, 1, col::kLine);
}

void DrawSpinner(float yc) {
    const int step = static_cast<int>((NowMs() / 250) % 4);
    for (int i = 0; i < 4; ++i) {
        gfx::FillCircle(142.0f + 12.0f * static_cast<float>(i), yc, 3.5f, i == step ? col::kAccent : col::kTrack);
    }
}

void DrawWaitBottom(const Footer& footer) {
    DrawTitleBar(gfx::kBottomW, "Please wait", std::string());
    DrawSpinner(120.0f);
    DrawFooterBackground();
    DrawFooter(footer);
    DrawFrameBorder(gfx::kBottomW);
}

void DrawRows(const ListView& v, const std::vector<Row>& rows) {
    std::vector<float> ys;
    float total = 0.0f;
    LayoutRows(rows, ys, total);
    const float rh = kRowH - 2.0f;
    const int n = static_cast<int>(rows.size());
    const int pressed = (v.touching && !v.dragging) ? v.press_row : -1;

    // Row backgrounds, then the sliding selection on top of them.
    for (int i = 0; i < n; ++i) {
        const float ry = kListTop + ys[i] - v.scroll;
        if (ry + rh <= kListTop - 1 || ry >= kListBottom) {
            continue;
        }
        gfx::FillRect(kRowX, std::floor(ry), kRowW, rh, col::kPanel);
    }
    if (n > 0) {
        const float hy = std::floor(kListTop + v.highlight_y - v.scroll);
        const u32 hl = (pressed >= 0) ? col::kSelPressed : col::kSel;
        gfx::FillGradient(kRowX, hy, kRowW, rh, gfx::Mix(hl, col::kAccent, 0.22f), hl,
                          gfx::Mix(hl, col::kAccent, 0.22f), hl);
        gfx::FillRect(kRowX, hy, 3, rh, col::kAccent);
    }

    // Row contents.
    for (int i = 0; i < n; ++i) {
        const Row& r = rows[i];
        const float ry = std::floor(kListTop + ys[i] - v.scroll);
        if (ry + rh <= kListTop - 1 || ry >= kListBottom) {
            continue;
        }
        const bool selected = (i == v.sel);
        const float yc = ry + rh * 0.5f;
        const float label_scale = 0.55f, value_scale = 0.48f;
        float left = kRowX + 12.0f;
        const float right = kRowX + kRowW - 10.0f;
        if (r.mark != Mark::None) {
            if (r.mark == Mark::Dot) {
                gfx::FillCircle(left + 4.0f, yc, 4.0f, col::kOn);
            }
            left += 14.0f;
        }
        if (r.icon) {
            gfx::DrawIcon(r.icon, setup::kLargeIcon, left - 4.0f, ry + 1.0f, 0.5f, r.dim ? 110 : 255); // (24 pixels: half of the 48)
            left += 26.0f;
        }
        left += r.indent;
        const u32 label_color = r.dim ? col::kDim : (r.label_color ? r.label_color : col::kText);
        // The room for the label depends on what sits at the right of the row and on the font, so
        // the label shrinks (or is shortened) instead of running into it.
        if (r.kind == Kind::Toggle) {
            const float state_w = gfx::TextWidth("OFF", 0.42f);
            const float label_max = (right - 34.0f - 8.0f - state_w - 10.0f) - left;
            DrawRowLabel(r, left, yc, label_scale, label_max, label_color, selected, v.sel_since);
            DrawSwitch(right - 34.0f, yc, r.on, r.dim);
            gfx::DrawText(right - 34.0f - 8.0f, yc, 0.42f, r.dim ? col::kDim : (r.on ? col::kOn : col::kOff), r.on ? "ON" : "OFF",
                          gfx::Align::Right);
        } else if (r.kind == Kind::Check) {
            const float box_x = right - 16.0f;
            DrawCheckBox(box_x, yc, r.on, r.partial);
            float edge = box_x - 8.0f;
            if (!r.detail.empty()) {
                gfx::DrawText(edge, yc, 0.42f, col::kDim, r.detail, gfx::Align::Right);
                edge -= gfx::TextWidth("1 h 05 min", 0.42f) + 10.0f; // (the column is as wide as its longest word)
            }
            if (!r.value.empty()) {
                // The size is a column of its own, as wide as the widest size: what comes before it (the "installed" chip) stays in line.
                gfx::DrawText(edge, yc, 0.42f, r.value_color ? r.value_color : col::kDim, r.value, gfx::Align::Right);
                edge -= std::max(gfx::TextWidth(r.value, 0.42f), gfx::TextWidth("1023.9 MB", 0.42f)) + 8.0f;
            }
            if (r.tags) {
                edge = DrawKindMarks(edge, yc, r.tags) - 3.0f;
            }
            if (!r.chip.empty()) {
                const float chip_w = gfx::TextWidth(r.chip, 0.34f) + 10.0f;
                DrawChip(edge - chip_w, yc, chip_w, r.chip, r.chip_color ? r.chip_color : col::kDim);
                edge -= chip_w + 6.0f;
            }
            DrawRowLabel(r, left, yc, label_scale, edge - left, label_color, selected, v.sel_since);
        } else {
            float vr = right;
            if (r.kind == Kind::Link) {
                DrawChevron(right - 6.0f, yc, selected ? col::kText : col::kDim);
                vr = right - 16.0f;
            }
            // At the right, from the edge in: the value, then the chip (a small tag, e.g. the Wi-Fi channel of a room), and the label takes what is left.
            const float chip_w = r.chip.empty() ? 0.0f : gfx::TextWidth(r.chip, 0.34f) + 10.0f;
            const float chip_space = chip_w > 0.0f ? chip_w + 6.0f : 0.0f;
            const float room = vr - left;
            const float value_w = r.value.empty() ? 0.0f : std::min(gfx::TextWidth(r.value, value_scale), room * 0.45f);
            const float label_max = room - (r.value.empty() ? 0.0f : value_w + 14.0f) - chip_space;
            DrawRowLabel(r, left, yc, label_scale, label_max, label_color, selected, v.sel_since);
            float shown_w = 0.0f;
            if (!r.value.empty()) {
                const float label_w = std::min(gfx::TextWidth(r.label, label_scale), label_max);
                const float avail = vr - (left + label_w + 14.0f) - chip_space;
                const u32 color = r.value_color ? r.value_color : (selected ? col::kText : col::kDim);
                const std::string shown = gfx::Ellipsize(r.value, value_scale, avail);
                gfx::DrawText(vr, yc, value_scale, color, shown, gfx::Align::Right);
                shown_w = gfx::TextWidth(shown, value_scale) + 8.0f;
            }
            if (chip_w > 0.0f) {
                DrawChip(vr - shown_w - chip_w, yc, chip_w, r.chip, r.chip_color ? r.chip_color : col::kDim);
            }
        }
    }

    // Scroll bar.
    const float view_h = kListBottom - kListTop;
    if (total > view_h) {
        const float track_x = 317.0f, track_y = kListTop + 2.0f, track_h = view_h - 4.0f;
        gfx::FillRect(track_x, track_y, 2, track_h, col::kLine);
        const float thumb_h = Clamp(track_h * view_h / total, 14.0f, track_h);
        const float max_scroll = total - view_h;
        const float t = max_scroll > 0.0f ? Clamp(v.scroll / max_scroll, 0.0f, 1.0f) : 0.0f;
        gfx::FillRect(track_x - 1, track_y + (track_h - thumb_h) * t, 4, thumb_h, col::kAccent);
    }
}

// ---------------------------------------------------------------------------------------------
// Pieces of the screens
// ---------------------------------------------------------------------------------------------

namespace {

void DrawBarBackground(float width) {
    const u32 bar_bottom = gfx::Mix(col::kBar, gfx::Rgba(168, 174, 188), 0.55f);
    gfx::FillGradient(0, 0, width, kBarH, col::kBar, col::kBar, bar_bottom, bar_bottom);
    gfx::FillRect(0, kBarH, width, 1, gfx::WithAlpha(col::kAccent, 200));
    gfx::FillRect(0, 0, 5, kBarH, col::kAccent);
}

// The HOME button does nothing now: a small house with a bar through it and the words, ending at `right_edge`. Returns where it begins.
float DrawHomeOffBadge(float right_edge, float yc) {
    const float text_w = gfx::TextWidth("HOME off", 0.4f);
    const float w = 18.0f + text_w + 8.0f;
    const float x = right_edge - w;
    const u32 red = gfx::Rgba(190, 40, 40);
    gfx::FillRect(x, yc - 8.0f, w, 16.0f, gfx::Mix(col::kBar, red, 0.18f));
    gfx::StrokeRect(x, yc - 8.0f, w, 16.0f, 1.0f, red);
    // the house
    gfx::Line(x + 3.0f, yc, x + 8.0f, yc - 4.5f, 1.5f, red);
    gfx::Line(x + 8.0f, yc - 4.5f, x + 13.0f, yc, 1.5f, red);
    gfx::StrokeRect(x + 4.5f, yc, 7.0f, 5.0f, 1.0f, red);
    gfx::Line(x + 2.0f, yc + 6.0f, x + 14.0f, yc - 6.0f, 1.5f, red); // the bar
    gfx::DrawText(x + 17.0f, yc, 0.4f, red, "HOME off");
    return x;
}

} // namespace

void DrawTitleBar(float width, const char* title, const std::string& right) {
    DrawBarBackground(width);
    gfx::DrawText(14, kBarH * 0.5f, 0.56f, col::kBarText, title);
    float edge = width - 10.0f;
    if (!right.empty()) {
        gfx::DrawText(edge, kBarH * 0.5f, 0.45f, gfx::Mix(col::kBarText, col::kBar, 0.35f), right, gfx::Align::Right);
        edge -= gfx::TextWidth(right, 0.45f) + 10.0f;
    }
    if (g.home_blocked && width < 330.0f) {
        DrawHomeOffBadge(edge, kBarH * 0.5f);
    }
}

void DrawTitleBarStatus(float width, const char* title, int wifi_channel) {
    DrawBarBackground(width);
    const u32 dim = gfx::Mix(col::kBarText, col::kBar, 0.35f);

    // At the right: the battery, then the card. The card first (it ends at the edge), the battery before it. `limit` is where they begin: the
    // title and the version keep to the left of it.
    const setup::ConsoleStatus& status = g.status;
    float edge = width - 10.0f;
    float limit = edge;
    if (status.sd_known) {
        const double gb = static_cast<double>(status.sd_free) / (1024.0 * 1024.0 * 1024.0);
        const std::string sd = gb >= 1.0 ? Fmt("SD %.1f GB free", gb) : Fmt("SD %.0f MB free", gb * 1024.0);
        gfx::DrawText(edge, kBarH * 0.5f, 0.42f, col::kBarText, sd, gfx::Align::Right);
        edge -= gfx::TextWidth(sd, 0.42f) + 14.0f;
        limit = edge;
    }
    const bool known = status.battery_percent >= 0 || status.battery_steps >= 0;
    if (known) {
        const int percent = status.battery_percent >= 0 ? (status.battery_percent > 100 ? 100 : status.battery_percent)
                                                        : (status.battery_steps > 5 ? 5 : status.battery_steps) * 20;
        const bool low = !status.charging && percent < 15;
        std::string text = status.battery_percent >= 0 ? Fmt("%d%%", percent) : Fmt("%d/5", status.battery_steps > 5 ? 5 : status.battery_steps);
        if (status.charging) {
            text += " +";
        }
        gfx::DrawText(edge, kBarH * 0.5f, 0.42f, low ? gfx::Rgba(190, 40, 40) : col::kBarText, text, gfx::Align::Right);
        const float text_w = gfx::TextWidth(text, 0.42f);
        // the battery: a box with a nub, as full as it is
        const float bx = edge - text_w - 24.0f;
        const float by = kBarH * 0.5f - 5.0f;
        gfx::FillRect(bx, by, 18.0f, 10.0f, col::kBarText);
        gfx::FillRect(bx + 1.0f, by + 1.0f, 16.0f, 8.0f, col::kBar);
        gfx::FillRect(bx + 18.0f, by + 3.0f, 2.0f, 4.0f, col::kBarText);
        gfx::FillRect(bx + 2.0f, by + 2.0f, 14.0f * static_cast<float>(percent) / 100.0f, 6.0f, low ? gfx::Rgba(190, 40, 40) : gfx::Rgba(40, 150, 80));
        limit = bx - 8.0f;
    }
    if (wifi_channel > 0) {
        const std::string chan = Fmt("Wi-Fi CH %d", wifi_channel);
        gfx::DrawText(limit - 4.0f, kBarH * 0.5f, 0.42f, col::kBarText, chan, gfx::Align::Right);
        limit -= gfx::TextWidth(chan, 0.42f) + 14.0f;
    }

    const std::string shown = gfx::Ellipsize(title, 0.56f, limit - 14.0f - 6.0f);
    gfx::DrawText(14, kBarH * 0.5f, 0.56f, col::kBarText, shown);
    const float title_end = 14.0f + gfx::TextWidth(shown, 0.56f);
    const std::string version = std::string("v") + setup::kVersion;
    if (title_end + 8.0f + gfx::TextWidth(version, 0.36f) <= limit - 4.0f) {
        gfx::DrawText(title_end + 8.0f, kBarH * 0.5f + 1.0f, 0.36f, dim, version);
    }
}

// The kind of a title is shown by ONE small coloured bar, with no letter, wherever it is shown (a list, the offer, the legend): blue for a game, green for an
// update, red for a DLC. On whole pixels: a bar that starts between two of them is drawn a pixel off its neighbour.
float DrawKindMarks(float right_edge, float yc, unsigned tags) {
    const struct {
        unsigned bit;
        u32 color;
    } all[3] = {{kTagGame, col::kGame}, {kTagUpdate, col::kUpdate}, {kTagDlc, col::kDlc}};
    float x = right_edge;
    for (int i = 2; i >= 0; --i) {
        if (!(tags & all[i].bit)) {
            continue;
        }
        x = std::floor(x - 9.0f);
        const float py = std::floor(yc) - 3.0f;
        gfx::FillRect(x, py, 9.0f, 6.0f, gfx::Mix(col::kPanel, all[i].color, 0.55f));
        gfx::StrokeRect(x, py, 9.0f, 6.0f, 1.0f, all[i].color);
        x -= 3.0f;
    }
    return x;
}

void DrawKindLegend(float right_x, float yc) {
    struct Entry {
        const char* word;
        u32 color;
    };
    const Entry entries[3] = {{"Game", col::kGame}, {"Update", col::kUpdate}, {"DLC", col::kDlc}};
    float x = right_x;
    for (int i = 2; i >= 0; --i) {
        const float word_w = gfx::TextWidth(entries[i].word, 0.4f);
        x -= word_w;
        gfx::DrawText(x, yc, 0.4f, col::kText, entries[i].word);
        // the same bar as in the rows, on the same line as the text
        x = std::floor(x - 13.0f);
        const float py = std::floor(yc) - 3.0f;
        gfx::FillRect(x, py, 9.0f, 6.0f, gfx::Mix(col::kPanel, entries[i].color, 0.55f));
        gfx::StrokeRect(x, py, 9.0f, 6.0f, 1.0f, entries[i].color);
        x -= 8.0f;
    }
}

void DrawFrameBorder(float width) {
    gfx::StrokeRect(0, 0, width, static_cast<float>(gfx::kScreenH), 1.0f, gfx::WithAlpha(col::kBorder, 150));
}

void DrawCard(float x, float y, float w, float h) {
    gfx::FillRect(x, y, w, h, col::kPanel);
    gfx::StrokeRect(x, y, w, h, 1.0f, col::kLine);
}

void DrawDotText(float x, float yc, u32 dot, u32 text_color, const std::string& text, float scale, float max_w) {
    gfx::FillCircle(x + 4.0f, yc, 4.0f, dot);
    gfx::DrawText(x + 14.0f, yc, scale, text_color, gfx::Ellipsize(text, scale, max_w - 14.0f));
}

void DrawProgressBar(float x, float y, float w, float h, float fraction) {
    fraction = Clamp(fraction, 0.0f, 1.0f);
    gfx::FillRect(x, y, w, h, col::kTrack);
    const float filled = std::floor((w - 2.0f) * fraction);
    if (filled > 0.0f) {
        gfx::FillGradient(x + 1.0f, y + 1.0f, filled, h - 2.0f, gfx::Mix(col::kAccent, col::kText, 0.35f),
                          gfx::Mix(col::kAccent, col::kText, 0.35f), col::kAccent, col::kAccent);
    }
    gfx::StrokeRect(x, y, w, h, 1.0f, col::kBorder);
}

void DrawSignalBars(float x, float yc, int level) {
    for (int i = 0; i < 3; ++i) {
        const float bar_h = 4.0f + 3.0f * static_cast<float>(i);
        gfx::FillRect(x + 5.0f * static_cast<float>(i), yc + 5.0f - bar_h, 4.0f, bar_h, i < level ? col::kOn : col::kTrack);
    }
}

void DrawButton(const Rect& r, const char* key, const char* label, bool enabled, bool pressed, bool compact) {
    const u32 fill = !enabled ? col::kPanel : (pressed ? col::kSelPressed : col::kPanelAlt);
    gfx::FillRect(r.x, r.y, r.w, r.h, fill);
    gfx::StrokeRect(r.x, r.y, r.w, r.h, 1.0f, enabled ? col::kBorder : col::kLine);
    float key_scale = compact ? 0.34f : 0.38f, label_scale = compact ? 0.42f : 0.46f;
    float pad = 10.0f, gap = 6.0f; // the chip is the key's width plus `pad`; `gap` separates chip and label
    float chip_w = gfx::TextWidth(key, key_scale) + pad;
    float total = chip_w + gap + gfx::TextWidth(label, label_scale);
    if (total > r.w - 8.0f) {
        // The width of the text depends on the font: shrink everything to fit the button instead of overflowing.
        const float shrink = (r.w - 8.0f) / total;
        key_scale *= shrink;
        label_scale *= shrink;
        pad *= shrink;
        gap *= shrink;
        chip_w = gfx::TextWidth(key, key_scale) + pad;
        total = chip_w + gap + gfx::TextWidth(label, label_scale);
    }
    const float x = r.x + (r.w - total) * 0.5f;
    const float yc = r.y + r.h * 0.5f;
    gfx::FillRect(x, yc - 7.0f, chip_w, 14.0f, enabled ? col::kBar : col::kTrack);
    gfx::DrawText(x + chip_w * 0.5f, yc, key_scale, col::kBarText, key, gfx::Align::Center);
    gfx::DrawText(x + chip_w + gap, yc, label_scale, enabled ? col::kText : col::kDim, label);
}

void DrawListChrome(const char* title, const std::string& right) {
    DrawTitleBar(gfx::kBottomW, title, right);
    DrawFooterBackground();
    DrawFrameBorder(gfx::kBottomW);
}

void DrawDim(float width) {
    gfx::FillRect(0, 0, width, static_cast<float>(gfx::kScreenH), gfx::Rgba(0, 0, 0, 175));
}

PopupBox LayoutPopup(const std::string& text, bool two_buttons, float scale, int max_lines) {
    PopupBox box;
    box.scale = scale;
    const int total = static_cast<int>(gfx::WrapText(text, scale, box.w - 30.0f).size());
    box.lines = total < max_lines ? total : max_lines;
    constexpr float kButtonsH = 30.0f;
    box.h = 30.0f + gfx::LineHeight(scale) * static_cast<float>(box.lines) + 16.0f + kButtonsH + 12.0f;
    box.y = kBarH + 1.0f + (static_cast<float>(gfx::kScreenH) - kBarH - 1.0f - box.h) * 0.5f;
    const float by = box.y + box.h - 12.0f - kButtonsH;
    if (two_buttons) {
        const float bw = (box.w - 28.0f - 8.0f) * 0.5f;
        box.no = Rect{box.x + 14.0f, by, bw, kButtonsH};
        box.yes = Rect{box.x + 14.0f + bw + 8.0f, by, bw, kButtonsH};
    } else {
        box.ok = Rect{box.x + (box.w - 150.0f) * 0.5f, by, 150.0f, kButtonsH};
    }
    return box;
}

void DrawPopupBox(const PopupBox& box, const std::string& title, const std::string& text) {
    DrawCard(box.x, box.y, box.w, box.h);
    gfx::FillRect(box.x, box.y, 3, box.h, col::kAccent);
    gfx::DrawText(box.x + 14, box.y + 16, 0.58f, col::kAccent, gfx::Ellipsize(title, 0.58f, box.w - 30.0f));
    gfx::DrawWrapped(box.x + 14, box.y + 30, box.w - 30.0f, box.scale, col::kText, text, box.lines);
}

void DrawPopupCard(float y, float h, const std::string& title, const std::string& text, int max_lines) {
    DrawCard(8, y, 304, h);
    gfx::FillRect(8, y, 3, h, col::kAccent);
    gfx::DrawText(22, y + 16, 0.55f, col::kAccent, gfx::Ellipsize(title, 0.55f, 280));
    gfx::DrawWrapped(22, y + 30, 280, 0.42f, col::kText, text, max_lines);
}

namespace {

float ButtonWidth(const char* label) {
    return Clamp(gfx::TextWidth(label, 0.42f) + 42.0f, 52.0f, 120.0f);
}

} // namespace

Rect ActionButton(const char* label, bool back) {
    if (!label || !*label) {
        return Rect{0.0f, 0.0f, 0.0f, 0.0f};
    }
    const float x = back ? kBackButton.x + kBackButton.w + 6.0f : kBackButton.x;
    return Rect{x, kBackButton.y, std::max(ButtonWidth(label), 58.0f), kBackButton.h};
}

FooterRects LayoutFooter(const Footer& footer) {
    FooterRects r;
    if (!footer.start) {
        // No START button: the buttons share the whole width, equal and evenly spaced (the ones at the edges are as easy to hit as the others).
        const int count = (footer.back ? 1 : 0) + ((footer.action_label && *footer.action_label) ? 1 : 0) + (footer.n_extra < 3 ? footer.n_extra : 3);
        if (count > 0) {
            const float gap = 4.0f;
            const float w = (304.0f - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
            float x = 8.0f;
            const auto next = [&]() {
                const Rect rect{x, kBackButton.y, w, kBackButton.h};
                x += w + gap;
                return rect;
            };
            if (footer.back) {
                r.back = next();
            }
            if (footer.action_label && *footer.action_label) {
                r.action = next();
            }
            for (int i = 0; i < footer.n_extra && i < 3; ++i) {
                r.extra[i] = next();
            }
        }
        r.hint_x = 12.0f;
        r.hint_end = 12.0f;
        return r;
    }
    float x = kBackButton.x;
    if (footer.back) {
        r.back = kBackButton;
        x = kBackButton.x + kBackButton.w + 6.0f;
    }
    if (footer.action_label && *footer.action_label) {
        r.action = ActionButton(footer.action_label, footer.back);
        x = r.action.x + r.action.w + 6.0f;
    }
    for (int i = 0; i < footer.n_extra && i < 3; ++i) {
        r.extra[i] = Rect{x, kBackButton.y, ButtonWidth(footer.extra[i].label), kBackButton.h};
        x += r.extra[i].w + 6.0f;
    }
    r.start = kStartButton;
    r.hint_x = x + 2.0f;
    r.hint_end = kStartButton.x - 6.0f;
    return r;
}

void DrawFooter(const Footer& footer) {
    const Input& in = g.input.in();
    const FooterRects r = LayoutFooter(footer);
    if (footer.back) {
        DrawButton(r.back, "B", footer.back_label, true, PressedNow(in, r.back), true);
    }
    if (footer.action_label && *footer.action_label) {
        DrawButton(r.action, "A", footer.action_label, true, PressedNow(in, r.action), true);
    }
    for (int i = 0; i < footer.n_extra && i < 3; ++i) {
        DrawButton(r.extra[i], footer.extra[i].key, footer.extra[i].label, true, PressedNow(in, r.extra[i]), true);
    }
    if (footer.hint && *footer.hint && r.hint_end - r.hint_x > 20.0f) {
        gfx::DrawFitted(r.hint_x, 226, 0.38f, 0.3f, r.hint_end - r.hint_x, col::kDim, footer.hint);
    }
    if (footer.start) {
        DrawButton(r.start, "START", footer.start_label, footer.start_enabled, PressedNow(in, r.start) && footer.start_enabled, true);
    }
}

void DrawNote(float x, float y, float w, float h, const Row& row) {
    DrawCard(x, y, w, h);
    const u32 title_color = row.note_color ? row.note_color : col::kAccent;
    gfx::FillRect(x, y, 3, h, title_color);
    const std::string title = row.note_title.empty() ? row.label : row.note_title;
    gfx::DrawText(x + 14, y + 14, 0.55f, title_color, gfx::Ellipsize(title, 0.55f, w - 28));
    const float lh = gfx::LineHeight(0.45f);
    int max_lines = static_cast<int>((h - 34.0f) / lh);
    if (max_lines < 1) {
        max_lines = 1;
    }
    gfx::DrawWrapped(x + 14, y + 28, w - 28, 0.45f, col::kText, row.note, max_lines);
}

void DrawStatusStrip(float x, float y, float w, float h) {
    DrawCard(x, y, w, h);
    const float yc = y + h * 0.5f;
    if (g.ToastActive()) {
        gfx::DrawFitted(x + 12, yc, 0.46f, 0.36f, w - 24, g.toast_color, g.toast);
        return;
    }
    if (!g.save_ok) {
        gfx::DrawFitted(x + 12, yc, 0.46f, 0.36f, w - 24, col::kOff, "The settings file could not be written!");
        return;
    }
    const bool on = setup::SwitchIsOn(g.cfg.enabled, g.cfg.tunnel, setup::LockOfSwitch(g.luma_too_old, g.plugin_ready)); // (the rule of the main page's switch: no page says ON while it is shut)
    const std::string state = on ? "3DS Link is ON" : "3DS Link is OFF";
    gfx::FillCircle(x + 16, yc, 5, on ? col::kOn : col::kOff);
    gfx::DrawText(x + 28, yc, 0.5f, col::kText, state);
    const float state_w = gfx::TextWidth(state, 0.5f);
    const float room_left = x + 28 + state_w + 14;
    gfx::DrawFitted(x + w - 12, yc, 0.44f, 0.34f, x + w - 12 - room_left, g.cfg.host.empty() ? col::kWarn : col::kDim,
                    gfx::Clean(g.cfg.host.empty() ? std::string("Room not set") : "Room " + RoomLabel()),
                    gfx::Align::Right);
}

float CaptionWidth() {
    static const char* const kCaptions[] = {"Room", "WiFi", "Name", "MAC", "Join"};
    float width = 0.0f;
    for (const char* caption : kCaptions) {
        width = std::max(width, gfx::TextWidth(caption, 0.46f));
    }
    return width + 8.0f;
}

void DrawNetworkLine(float x, float yc, float max_w) {
    if (g.net.known && g.net.wifi) {
        const std::string text = g.net.ip.empty() ? std::string("connected") : g.net.ip;
        DrawDotText(x, yc, col::kOn, col::kText, text, 0.46f, max_w);
    } else if (g.net.known) {
        DrawDotText(x, yc, col::kOff, col::kWarn, "not connected", 0.46f, max_w);
    } else {
        DrawDotText(x, yc, col::kDim, col::kDim, "unknown", 0.46f, max_w);
    }
}

// ---------------------------------------------------------------------------------------------
// ListPage
// ---------------------------------------------------------------------------------------------

const Row* ListPage::Selected() const {
    if (rows_.empty()) {
        return nullptr;
    }
    return &rows_[Clamp(list_.sel, 0, static_cast<int>(rows_.size()) - 1)];
}

Footer ListPage::BuildFooter() const {
    Footer footer;
    footer.back = !IsRoot();
    footer.back_label = BackLabel();
    footer.action_label = rows_.empty() ? "" : ActionLabel();
    footer.hint = Hint();
    footer.n_extra = ExtraKeys(footer.extra);
    footer.start = footer.n_extra == 0 && StartButton();
    footer.start_label = StartLabel();
    footer.start_enabled = StartEnabled();
    return footer;
}

bool ListPage::StartEnabled() const {
    return setup::CopyWorker::Get().PendingCount() == 0 || g.plugin_pass_open;
}

void ListPage::AskLeaveDuringPluginPass() {
    const int left = setup::CopyWorker::Get().PendingCount();
    const int done = g.plugin_pass_total > left ? g.plugin_pass_total - left : 0;
    PushPage(pages::NewConfirmPage(
        "Plugin update in progress",
        Fmt("3DS Link is putting the new plugin in your games: %d of %d folders done. If you leave now, the games not reached yet keep the OLD plugin, and the update starts again the next "
            "time you open 3DS Link. Leave anyway?",
            done, g.plugin_pass_total),
        "Leave", "Keep waiting", [this](bool yes) {
            if (yes) {
                setup::CopyWorker::Get().CancelRefreshes();
                OnStart();
            }
        }));
}

void ListPage::Update(const Input& in) {
    BuildRows(rows_);
    const ListEvent event = UpdateList(list_, rows_, in);
    const Footer footer = BuildFooter();
    const FooterRects buttons = LayoutFooter(footer);
    if (footer.start && footer.start_enabled && ((in.down & KEY_START) || Tapped(in, buttons.start))) {
        if (g.plugin_pass_open && setup::CopyWorker::Get().PendingCount() > 0) {
            AskLeaveDuringPluginPass(); // (the pass over the games is not over: say so, and let the user choose)
            return;
        }
        OnStart();
        return;
    }
    if (!IsRoot() && ((in.down & KEY_B) || Tapped(in, buttons.back))) {
        OnBack();
        return;
    }
    for (int i = 0; i < footer.n_extra; ++i) {
        if ((in.down & footer.extra[i].mask) || Tapped(in, buttons.extra[i])) {
            OnExtra(i, Selected());
            return;
        }
    }
    int activated = event.activated;
    if (activated < 0 && !rows_.empty() && *footer.action_label && Tapped(in, buttons.action)) {
        activated = Clamp(list_.sel, 0, static_cast<int>(rows_.size()) - 1); // (the A button of the footer: what A does)
    }
    if (activated >= 0) {
        Activate(rows_[activated], 0);
    } else if (event.step != 0) {
        Activate(*Selected(), event.step);
    } else {
        OnKeys(in);
    }
}

void ListPage::DrawTop() {
    DrawTitleBarStatus(gfx::kTopW, Title());
    if (const Row* row = Selected()) {
        DrawNote(10, 32, 380, 168, *row);
    }
    DrawStatusStrip(10, 206, 380, 28);
    DrawFrameBorder(gfx::kTopW);
}

void ListPage::DrawBottom() {
    DrawRows(list_, rows_);
    DrawListChrome(BarTitle(), BarRight());
    DrawFooter(BuildFooter());
}

} // namespace ui
