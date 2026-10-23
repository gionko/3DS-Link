// Pages: Titles Delivery. The menu (Send Titles, Receive Titles), the choice of what to send, the rooms around to receive from, the wait for the
// list of titles, and the page that closes a receiving. The screens that run while the module is sending or receiving are in delivery_run.cpp.
// None of these pages has a START button: Titles Delivery is left with B, to the main page, from where the app is left.
#include <cstring>

#include "delivery.hpp"
#include "dlp_text.hpp"
#include "pages.hpp"
#include "titles.hpp"

using namespace ui;

namespace {

// A title the scan found of no use (half installed, unreadable): it is not whole on the card, so it is not sent. The scan's report is copied
// once a list is built; before the scan is done nothing is known (and the send page is not open then).
bool Unusable(const setup::TitlesHealth& health, u64 tid) {
    for (const setup::UnusableTitle& t : health.unusable) {
        if (t.tid == tid) {
            return true;
        }
    }
    return false;
}

// A card at the top of the touch screen's counterpart: an accent heading and a paragraph.
void DrawIntro(float y, float h, const char* heading, const std::string& text, int lines) {
    DrawCard(10, y, 380, h);
    gfx::FillRect(10, y, 3, h, col::kAccent);
    gfx::DrawText(26, y + 14, 0.55f, col::kAccent, heading);
    gfx::DrawWrapped(26, y + 26, 354, 0.43f, col::kText, text, lines);
}

// A name that is only question marks where letters should be ("????"): the module could not read it, so there is nothing to show or to send.
bool UnreadableName(const std::string& name) {
    int run = 0;
    for (const char c : name) {
        run = c == '?' ? run + 1 : 0;
        if (run >= 4) {
            return true;
        }
    }
    return false;
}

// The icon of a game (the one the list of titles has), or nullptr.
const u16* IconOf(u64 tid) {
    for (const std::unique_ptr<titles::Game>& game : titles::Scan().games()) {
        if (game->tid == tid) {
            return game->icon;
        }
    }
    return nullptr;
}

// Draws `page` (both screens) with a card over its top screen: something is under way and takes a moment. The bottom screen is left as it
// is, the page does not jump to another layout.
void ShowBusyOver(Page& page, const std::string& title, const std::string& text, float fraction) {
    if (!gfx::BeginFrame(true)) {
        return;
    }
    gfx::BeginTop();
    page.DrawTop();
    DrawDim(gfx::kTopW);
    DrawCard(50, 84, 300, 76);
    gfx::FillRect(50, 84, 3, 76, col::kAccent);
    gfx::DrawText(66, 100, 0.55f, col::kAccent, gfx::Ellipsize(title, 0.55f, 270));
    gfx::DrawText(66, 120, 0.43f, col::kText, gfx::Ellipsize(text, 0.43f, 270));
    DrawProgressBar(66, 134, 268, 10, fraction);
    gfx::BeginBottom();
    page.DrawBottom();
    gfx::EndFrame();
}

// "Loading" with three dots that move: the lists of titles take a moment to build (and the wireless a moment to open), and a console that
// shows nothing in that moment looks stuck. The page that is being built is made while this one is on the screen; the popup stays for at least
// half a second, so that it is not a flash. A page that takes long calls LoadingTick() now and then, which draws a frame of the popup.
class LoadingPage;
LoadingPage* g_loading = nullptr;

class LoadingPage : public Page {
public:
    using MakeFn = std::function<Page*()>;
    explicit LoadingPage(MakeFn make) : make_(std::move(make)), start_(NowMs()) {}
    ~LoadingPage() override {
        delete next_; // (a page that was built and not handed over: the stack was cleared in the half second)
    }

    void Update(const Input&) override {
        ++frames_;
        if (!built_ && frames_ >= 3) { // (three frames were drawn: the popup is on the screen)
            g_loading = this;
            next_ = make_();
            g_loading = nullptr;
            built_ = true;
        }
        if (built_ && NowMs() - start_ >= kMinMs) {
            PopPage();
            if (next_) {
                PushPage(next_);
                next_ = nullptr; // (the stack owns it now)
            }
        }
    }

    void DrawTop() override {
        if (Page* below = PageBelow()) {
            below->DrawTop();
        }
        DrawDim(gfx::kTopW);
    }

    void DrawBottom() override {
        if (Page* below = PageBelow()) {
            below->DrawBottom();
        }
        DrawDim(gfx::kBottomW);
        constexpr float kW = 200.0f, kH = 66.0f;
        const float x = (gfx::kBottomW - kW) * 0.5f;
        const float y = kBarH + 1.0f + (static_cast<float>(gfx::kScreenH) - kBarH - 1.0f - kH) * 0.5f;
        DrawCard(x, y, kW, kH);
        gfx::FillRect(x, y, 3, kH, col::kAccent);
        gfx::DrawText(x + kW * 0.5f, y + 20.0f, 0.6f, col::kText, "Loading", gfx::Align::Center);
        const int lit = static_cast<int>((NowMs() / 200) % 3);
        for (int i = 0; i < 3; ++i) {
            gfx::FillCircle(x + kW * 0.5f - 14.0f + 14.0f * static_cast<float>(i), y + 45.0f, 4.0f, i == lit ? col::kAccent : col::kTrack);
        }
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    static constexpr u64 kMinMs = 500;
    MakeFn make_;
    u64 start_;
    int frames_ = 0;
    bool built_ = false;
    Page* next_ = nullptr;
};

// A frame of the loading popup from inside the building of a page (the main loop is not running then): the dots go on moving.
void LoadingTick() {
    static u64 last = 0;
    const u64 now = NowMs();
    if (!g_loading || now - last < 70) {
        return;
    }
    last = now;
    if (!gfx::BeginFrame(true)) {
        return;
    }
    gfx::BeginTop();
    g_loading->DrawTop();
    gfx::BeginBottom();
    g_loading->DrawBottom();
    gfx::EndFrame();
}

// The wait for the list of titles: Titles Delivery works with what the scan of the titles made, and the scan runs at every start.
class ScanWaitPage : public Page {
public:
    using MakeFn = std::function<Page*()>;
    explicit ScanWaitPage(MakeFn make) : make_(std::move(make)) {}

    void Update(const Input& in) override {
        if (titles::Scan().done() && delivery::AttachModule()) {
            PopPage();
            PushPage(new LoadingPage(make_));
            return;
        }
        const FooterRects r = LayoutFooter(MakeFooter());
        if ((in.down & KEY_B) || Tapped(in, r.back)) {
            PopPage();
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, "Titles Delivery");
        DrawCard(10, 32, 380, 198);
        gfx::FillRect(10, 32, 3, 198, col::kAccent);
        gfx::DrawText(200, 86, 0.62f, col::kText, "Reading your titles", gfx::Align::Center);
        const titles::Scanner& scan = titles::Scan();
        DrawProgressBar(40, 112, 320, 16, setup::ProgressFraction(scan.progress_done(), scan.progress_total()));
        gfx::DrawText(200, 142, 0.5f, col::kDim, setup::ScanProgressText(scan.progress()), gfx::Align::Center);
        gfx::DrawWrapped(40, 164, 320, 0.43f, col::kDim,
                         "It opens by itself as soon as the list is ready. The list is read at every start; only what is new takes time.", 3);
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        DrawWaitBottom(MakeFooter());
    }

private:
    static Footer MakeFooter() {
        Footer f;
        f.start = false;
        return f;
    }
    MakeFn make_;
};

// ---------------------------------------------------------------------------------------------
// Titles Delivery: the menu
// ---------------------------------------------------------------------------------------------

class DeliveryPage : public ListPage {
public:
    DeliveryPage() {
        g.RefreshBoot9();
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, Title());
        DrawIntro(32, 70, "Send and receive installed titles",
                  "Transfers games, updates and DLC from one 3DS to others over the local wireless, with no internet and no media. One "
                  "transmission reaches every console in range. You can close the shell while it works, to save battery.",
                  4);
        if (const Row* row = Selected()) {
            DrawNote(10, 108, 380, g.ToastActive() ? 92 : 124, *row);
        }
        if (g.ToastActive()) {
            DrawStatusStrip(10, 206, 380, 28); // (the message of the moment: the note gives way to it)
        }
        DrawFrameBorder(gfx::kTopW);
    }

protected:
    const char* Title() const override {
        return "Titles Delivery";
    }
    const char* BarTitle() const override {
        return "Options"; // (not "Titles Delivery" once more: it is the title of the top screen)
    }
    // Y opens the guide "How to get boot9.bin" on Send Titles while the file is missing. This page has no START button, so the footer's hint line is not drawn
    // (the layout gives it room only beside START): Y is a button of the footer instead, as it is on 3DS Link Delivery's own page, and a tap on it does what Y does.
    int ExtraKeys(FooterKey* out) const override {
        if (!GuideKeyOffered()) {
            return 0;
        }
        out[0] = FooterKey{"Y", dlp::kGuideLabel, KEY_Y};
        return 1;
    }
    void OnExtra(int, const Row*) override {
        if (GuideKeyOffered() && !pages::OpenBoot9Guide()) {
            g.ShowToast(dlp::kNoGuideToast, col::kWarn, 3500);
        }
    }
    const char* ActionLabel() const override {
        return "Open";
    }
    bool StartButton() const override {
        return false;
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        ready_ = titles::Scan().done() && delivery::AttachModule();
        const bool keys = ready_ && dl_have_keys();
        const titles::Scanner& scan = titles::Scan();
        const std::string reading = Fmt("reading %d%%", static_cast<int>(setup::ProgressFraction(scan.progress_done(), scan.progress_total()) * 100.0f));
        const std::string waiting = "3DS Link is still reading your titles (" + setup::ScanProgressText(scan.progress()) +
                                    "). A opens it as soon as the list is ready.";

        Row send;
        send.id = kSend;
        send.kind = Kind::Link;
        send.label = "Send Titles";
        send.dim = ready_ && !keys;
        if (!ready_) {
            send.value = reading;
            send.value_color = col::kWarn;
            send.note_title = "Send Titles";
            send.note = waiting;
        } else if (!keys) {
            send.value = "needs boot9.bin";
            send.value_color = col::kWarn;
            send.note_title = "boot9.bin is missing";
            send.note_color = col::kWarn;
            send.note = "Sending needs boot9.bin, the key file of this console, to read the titles. Receiving works without it.\n\n"
                        "To fix it: go back, open Settings > Installation check and press A on boot9.bin. It can search the SD card, "
                        "let you choose a folder, or tell you how to get the file. Y shows how, step by step.";
        } else {
            send.note_title = "Send Titles";
            send.note = "Choose games, updates and DLC installed on this console and open a room. Other consoles join it whenever they want "
                        "and take what they choose, all from the one transmission. Up to 16 titles and 7 receivers.";
        }
        rows.push_back(send);

        Row receive;
        receive.id = kReceive;
        receive.kind = Kind::Link;
        receive.label = "Receive Titles";
        receive.note_title = "Receive Titles";
        if (!ready_) {
            receive.value = reading;
            receive.value_color = col::kWarn;
            receive.note = waiting;
        } else {
            receive.note = "Look for the room of a console that is sending, choose what to take from it and install it here. The Wi-Fi must be on "
                           "(no internet and no access point are used). Installing needs Luma3DS with the signature patches.";
        }
        rows.push_back(receive);
    }

    void Activate(const Row& row, int step) override {
        if (step != 0) {
            return;
        }
        if (row.id == kSend && ready_ && !dl_have_keys()) {
            g.ShowToast("boot9.bin is missing: see the note above.", col::kWarn, 4500);
            return;
        }
#ifdef AZL_TEST_DIRECT_SEND
        if (row.id == kSend) { // (a build made to look at the screen of a room on an emulator without titles: the room opens at once, on made-up numbers)
            delivery::RunSend();
            return;
        }
#endif
        const bool send = row.id == kSend;
        const ScanWaitPage::MakeFn make = [send]() -> Page* {
            if (send && !dl_have_keys()) {
                g.ShowToast("boot9.bin is missing: see the note.", col::kWarn, 4500);
                return nullptr;
            }
            return send ? pages::NewSendTitlesPage() : pages::NewReceiveTitlesPage();
        };
        if (ready_) {
            PushPage(new LoadingPage(make)); // (the list takes a moment to build: a popup says so)
        } else {
            PushPage(new ScanWaitPage(make)); // (the list of titles is not ready: it opens by itself when it is)
        }
    }

private:
    enum Id { kSend = 1, kReceive };
    // The guide's key is offered on Send Titles when the page is ready and boot9.bin is not on the card (a Send Titles that is shut for another reason, such as a
    // movable.sed that cannot be read, is not a thing the guide helps with).
    bool GuideKeyOffered() const {
        const Row* row = Selected();
        return row != nullptr && row->id == kSend && ready_ && !g.boot9.found;
    }
    bool ready_ = false;
};

// ---------------------------------------------------------------------------------------------
// Send Titles: what to send
// ---------------------------------------------------------------------------------------------

class SendTitlesPage : public ListPage {
public:
    SendTitlesPage() {
        g.HoldHome(); // (the choice of what to send ends in a room that HOME would break: it is off here too, and the bar says so)
        for (int game = 0; game < dl_game_count(); ++game) {
            LoadingTick();
            Entry e;
            e.name = gfx::Clean(dl_game_name(game));
            if (UnreadableName(e.name)) {
                continue; // (a name of question marks: nothing to show, and the module could not read the title)
            }
            for (int k = 0; k < dl_game_member_count(game); ++k) {
                const int item = dl_game_member(game, k);
                if (item < 0) {
                    continue;
                }
                e.items.push_back(item);
                const int kind = dl_item_kind(item);
                e.tags |= delivery::KindTag(kind);
                if (kind == DL_KIND_APP && !e.icon) {
                    e.icon = IconOf(dl_item_tid(item));
                }
            }
            if (!e.items.empty()) {
                entries_.push_back(std::move(e));
            }
        }
        picked_.assign(static_cast<std::size_t>(dl_item_count()), 0);
        size_.assign(static_cast<std::size_t>(dl_item_count()), 0);
    }

    ~SendTitlesPage() override {
        gfx::ReleaseIcons(); // (the icon textures are keyed by where the icon data is: the scan may be made again)
        g.ReleaseHome();
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, Title());
        // The heading, with the three colours at its right.
        DrawCard(10, 30, 380, 44);
        gfx::FillRect(10, 30, 3, 44, col::kAccent);
        gfx::DrawText(26, 43, 0.55f, col::kAccent, Fmt("Selected titles %d of %d", PickedCount(), delivery::kMaxTitles));
        DrawKindLegend(382.0f, 43.0f);
        gfx::DrawText(26, 62, 0.42f, col::kDim, "Choose what to send, then press Y to open the room.");

        DrawCard(10, 78, 380, 124);
        const Entry* entry = SelectedEntry();
        if (entry) {
            DrawEntryCard(*entry);
        } else {
            gfx::DrawText(200, 130, 0.5f, col::kDim, "No title to send", gfx::Align::Center);
        }

        // The message of the moment, or what is chosen so far.
        if (g.ToastActive()) {
            DrawStatusStrip(10, 206, 380, 28);
        } else {
            const int count = PickedCount();
            u64 bytes = 0;
            for (std::size_t i = 0; i < picked_.size(); ++i) {
                bytes += picked_[i] ? Size(static_cast<int>(i)) : 0;
            }
            DrawCard(10, 206, 380, 28);
            gfx::FillRect(10, 206, 3, 28, count > 0 ? col::kOn : col::kTrack);
            if (count == 0) {
                gfx::DrawText(26, 220, 0.5f, col::kDim, "Nothing chosen yet");
            } else {
                gfx::DrawText(26, 220, 0.5f, col::kText, Fmt("%d %s, %s", count, count == 1 ? "title" : "titles", delivery::SizeText(bytes).c_str()));
                gfx::DrawText(384, 220, 0.44f, col::kDim, delivery::ShortDurationText(delivery::EstimateSeconds(bytes)) + " to send", gfx::Align::Right);
            }
        }
        DrawFrameBorder(gfx::kTopW);
    }

protected:
    const char* Title() const override {
        return "Send Titles";
    }
    const char* BarTitle() const override {
        return "Select titles to share";
    }
    std::string BarRight() const override {
        const int count = PickedCount();
        return count == 0 ? std::string() : Fmt("%d chosen", count);
    }
    const char* ActionLabel() const override {
        return "Choose";
    }
    int ExtraKeys(FooterKey* out) const override {
        out[0] = FooterKey{"X", "Parts", KEY_X};
        out[1] = FooterKey{"Y", "Send", KEY_Y};
        return 2;
    }

    void BuildRows(std::vector<Row>& rows) override {
        if (!dirty_) {
            return;
        }
        dirty_ = false;
        rows.clear();
        for (std::size_t e = 0; e < entries_.size(); ++e) {
            const Entry& entry = entries_[e];
            int chosen = 0;
            for (const int item : entry.items) {
                chosen += picked_[static_cast<std::size_t>(item)] ? 1 : 0;
            }
            Row header;
            header.id = static_cast<int>(e) * kStride;
            header.kind = Kind::Check;
            header.label = entry.name;
            header.icon = entry.icon;
            header.on = chosen == static_cast<int>(entry.items.size());
            header.partial = chosen > 0 && !header.on;
            header.tags = entry.tags; // (the sizes are on the parts and on the top screen: the name gets the room)
            header.note_title = entry.name;
            header.note = "A chooses all its parts, or none when some are chosen. Right (or X) shows the parts one by one, left hides them.";
            rows.push_back(header);
            if (!entry.expanded) {
                continue;
            }
            for (std::size_t k = 0; k < entry.items.size(); ++k) {
                const int item = entry.items[k];
                const int kind = dl_item_kind(item);
                Row row;
                row.id = static_cast<int>(e) * kStride + 1 + static_cast<int>(k);
                row.kind = Kind::Check;
                row.indent = 14.0f;
                row.label = delivery::KindText(kind, dl_item_tid(item));
                row.label_color = delivery::KindColor(kind);
                row.on = picked_[static_cast<std::size_t>(item)] != 0;
                if (Unusable(health_, dl_item_tid(item))) {
                    row.chip = "incomplete";
                    row.chip_color = col::kWarn;
                }
                const u64 size = Size(item);
                row.value = size > 0 ? delivery::SizeText(size) : std::string();
                row.detail = size > 0 ? delivery::ShortDurationText(delivery::EstimateSeconds(size)) : std::string();
                row.note_title = entry.name + ": " + row.label;
                row.note = gfx::Clean(dl_item_code(item)) + "  " + setup::TidText(dl_item_tid(item));
                rows.push_back(row);
            }
        }
        if (rows.empty()) {
            Row row;
            row.kind = Kind::Info;
            row.label = "No title found";
            row.note_title = "No title found";
            row.note = "Nothing that can be sent is installed on the SD card of this console.";
            rows.push_back(row);
        }
    }

    void Activate(const Row& row, int step) override {
        if (row.id < 0 || entries_.empty()) {
            return;
        }
        const int e = row.id / kStride;
        const int part = row.id % kStride; // 0 the game's name, k + 1 its k-th part
        if (e >= static_cast<int>(entries_.size())) {
            return;
        }
        if (part == 0) {
            if (step > 0) {
                SetExpanded(e, true);
            } else if (step < 0) {
                SetExpanded(e, false);
            } else {
                ToggleGame(e);
            }
        } else if (step == 0) {
            Toggle(entries_[static_cast<std::size_t>(e)].items[static_cast<std::size_t>(part - 1)]);
            dirty_ = true;
        } else if (step < 0) {
            SetExpanded(e, false);
            for (std::size_t i = 0; i < rows_.size(); ++i) { // (the cursor goes back to the game's name)
                if (rows_[i].id == e * kStride) {
                    list_.sel = static_cast<int>(i);
                }
            }
        }
    }

    void OnExtra(int index, const Row* selected) override {
        if (index == 0) {
            if (selected && selected->id >= 0 && !entries_.empty()) {
                const int e = selected->id / kStride;
                SetExpanded(e, !entries_[static_cast<std::size_t>(e)].expanded);
            }
            return;
        }
        const int count = PickedCount();
        if (count == 0) {
            g.ShowToast("Choose at least one title first.", col::kWarn, 3500);
            return;
        }
        u64 bytes = 0;
        for (std::size_t i = 0; i < picked_.size(); ++i) {
            bytes += picked_[i] ? Size(static_cast<int>(i)) : 0;
        }
        PushPage(pages::NewConfirmPage("Open the room",
                                       Fmt("Send %d %s (%s, %s)?\n\nThe room stays open until you close it with B. Receivers join when they want and "
                                           "choose what to take.",
                                           count, count == 1 ? "title" : "titles", delivery::SizeText(bytes).c_str(),
                                           delivery::DurationText(delivery::EstimateSeconds(bytes)).c_str()),
                                       "Yes, open it", "No", [this](bool yes) {
                                           if (yes) {
                                               StartSending();
                                           }
                                       }));
    }

private:
    static constexpr int kStride = 64; // the parts of a game are numbered under it (a game has at most 48)

    struct Entry {
        std::string name;
        std::vector<int> items; // the titles of the game: the game, its update, its DLC
        const u16* icon = nullptr;
        unsigned tags = 0;
        bool expanded = false;
    };

    // What a title weighs: the exact size once it has been checked, before that what the system says is installed.
    u64 Size(int item) const {
        const u64 exact = size_[static_cast<std::size_t>(item)];
        return exact ? exact : dl_item_size(item);
    }

    const Entry* SelectedEntry() const {
        const Row* row = Selected();
        if (!row || row->id < 0 || entries_.empty()) {
            return nullptr;
        }
        const std::size_t e = static_cast<std::size_t>(row->id / kStride);
        return e < entries_.size() ? &entries_[e] : nullptr;
    }

    // The game under the cursor: its icon at twice the size, its name, and its parts with what each weighs and how long it takes to send.
    void DrawEntryCard(const Entry& e) {
        gfx::FillRect(20, 88, 100, 100, col::kPanelAlt);
        gfx::StrokeRect(20, 88, 100, 100, 1.0f, col::kLine);
        if (e.icon) {
            gfx::DrawIcon(e.icon, setup::kLargeIcon, 22.0f, 90.0f, 2.0f);
        }
        // The name and the ID are drawn as on the page of Enable Titles for Azahar Emulator Net (the same sizes, the same "ID:").
        const float used = gfx::DrawWrapped(130, 86, 252, 0.62f, col::kText, e.name, 2);
        float y = 86.0f + used + 6.0f;
        gfx::DrawFitted(130, y, 0.52f, 0.4f, 252, col::kDim, "ID: " + setup::TidText(dl_item_tid(e.items.front())));
        y += 17.0f;
        int fit = static_cast<int>((198.0f - y) / 14.0f); // (the parts that fit under the ID; one line is kept for "and N more")
        fit = fit < 1 ? 1 : (fit > 4 ? 4 : fit);
        const std::size_t shown = e.items.size() <= static_cast<std::size_t>(fit) ? e.items.size() : static_cast<std::size_t>(fit > 1 ? fit - 1 : 1);
        for (std::size_t k = 0; k < shown; ++k) {
            const int item = e.items[k];
            const int kind = dl_item_kind(item);
            const u64 size = Size(item);
            gfx::DrawText(130, y, 0.44f, delivery::KindColor(kind), delivery::KindText(kind, dl_item_tid(item)));
            gfx::DrawText(300, y, 0.44f, col::kText, size ? delivery::SizeText(size) : std::string("?"), gfx::Align::Right);
            gfx::DrawText(384, y, 0.4f, col::kDim, size ? delivery::ShortDurationText(delivery::EstimateSeconds(size)) : std::string(), gfx::Align::Right);
            y += 14.0f;
        }
        if (e.items.size() > shown) {
            gfx::DrawText(130, y, 0.4f, col::kDim, Fmt("and %d more", static_cast<int>(e.items.size() - shown)));
        }
    }

    int PickedCount() const {
        int count = 0;
        for (const uint8_t p : picked_) {
            count += p ? 1 : 0;
        }
        return count;
    }

    void SetExpanded(int e, bool on) {
        if (e >= 0 && e < static_cast<int>(entries_.size()) && entries_[static_cast<std::size_t>(e)].expanded != on) {
            entries_[static_cast<std::size_t>(e)].expanded = on;
            dirty_ = true;
        }
    }

    // Chooses a title or leaves it. Nothing is read now: the check of the files comes when the room is opened.
    bool Toggle(int item) {
        uint8_t& p = picked_[static_cast<std::size_t>(item)];
        if (p) {
            p = 0;
            return true;
        }
        if (Unusable(health_, dl_item_tid(item))) {
            g.ShowToast(gfx::Clean(dl_item_name(item)) + ": incomplete on this console, it cannot be sent (see Installation check).", col::kWarn, 5000);
            return false;
        }
        if (PickedCount() >= delivery::kMaxTitles) {
            g.ShowToast(Fmt("A room carries %d titles at most.", delivery::kMaxTitles), col::kWarn, 3500);
            return false;
        }
        p = 1;
        return true;
    }

    void ToggleGame(int e) {
        const Entry& entry = entries_[static_cast<std::size_t>(e)];
        bool any = false;
        for (const int item : entry.items) {
            any = any || picked_[static_cast<std::size_t>(item)];
        }
        for (const int item : entry.items) {
            if (any) {
                picked_[static_cast<std::size_t>(item)] = 0;
            } else if (!Toggle(item)) {
                break;
            }
        }
        dirty_ = true;
    }

    // The room: every chosen title is checked first (its files are read: that is how its exact size and its seed are got, and a title that cannot
    // be sent is found), with a card over the top screen that says which; then the module opens the room.
    void StartSending() {
        std::vector<int> items;
        for (const Entry& e : entries_) {
            for (const int item : e.items) {
                if (picked_[static_cast<std::size_t>(item)]) {
                    items.push_back(item);
                }
            }
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            const int item = items[i];
            const std::string what = gfx::Clean(dl_item_name(item)) + " (" + delivery::KindText(dl_item_kind(item), dl_item_tid(item)) + ")";
            ShowBusyOver(*this, "Checking the titles", Fmt("%d of %d: ", static_cast<int>(i) + 1, static_cast<int>(items.size())) + what,
                         static_cast<float>(i) / static_cast<float>(items.size()));
            u64 size = 0;
            char why[96];
            if (dl_send_check(item, &size, why, sizeof(why)) != 0) {
                picked_[static_cast<std::size_t>(item)] = 0;
                dirty_ = true;
                g.ShowToast(what + " cannot be sent: " + why, col::kWarn, 6000);
                return;
            }
            size_[static_cast<std::size_t>(item)] = size;
        }
        const int rc = dl_send_prepare(items.data(), static_cast<int>(items.size()));
        if (rc != 0) {
            g.ShowToast(rc == -1 ? "Choose between 1 and 16 titles." : "A chosen title has not been checked.", col::kWarn, 4000);
            return;
        }
        delivery::RunSend();
        dirty_ = true;
    }

    bool dirty_ = true;
    std::vector<Entry> entries_;
    std::vector<uint8_t> picked_;
    setup::TitlesHealth health_ = titles::Scan().done() ? titles::Scan().report().health : setup::TitlesHealth(); // (the page opens when the scan is done)
    std::vector<u64> size_; // the exact size of a title once it has been checked (0 = not yet)
};

// ---------------------------------------------------------------------------------------------
// Receive Titles: the rooms around
// ---------------------------------------------------------------------------------------------

class ReceiveTitlesPage : public ListPage {
public:
    ReceiveTitlesPage() {
        if (!wireless_.Open()) {
            error_ = delivery::WirelessErrorText(wireless_.error());
            return;
        }
        // HOME is off while the list of rooms is up: a press of it in the middle of a scan once froze the console (the scan is one call to
        // the wireless service that cannot be broken off). B leaves. The bar of the touch screen says so.
        g.HoldHome();
        home_held_ = true;
        scanning_ = dl_scan_start() == 0;
        if (!scanning_) {
            error_ = "The search for rooms could not be started.";
        }
    }

    ~ReceiveTitlesPage() override {
        StopScan();
    }

    void Update(const Input& in) override {
        if (scanning_ && dl_scan_generation() != generation_) {
            generation_ = dl_scan_generation();
            dirty_ = true;
        }
        ListPage::Update(in);
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, Title());
        DrawIntro(32, 74, "Consoles that are sending",
                  "Looks for the rooms of consoles that are sending. Choose one to see what it offers and what to take from it. Keep the consoles "
                  "close to each other; the Wi-Fi must be on.",
                  4);
        const float note_h = g.ToastActive() ? 86.0f : 120.0f;
        if (!error_.empty()) {
            DrawCard(10, 112, 380, note_h);
            gfx::FillRect(10, 112, 3, note_h, col::kOff);
            gfx::DrawText(26, 128, 0.55f, col::kOff, "The wireless is not available");
            gfx::DrawWrapped(26, 142, 354, 0.43f, col::kText, error_, 4);
        } else if (const Row* row = Selected(); row && !rows_.empty()) {
            DrawNote(10, 112, 380, note_h, *row);
        } else {
            DrawCard(10, 112, 380, note_h);
            gfx::FillRect(10, 112, 3, note_h, col::kAccent);
            gfx::DrawText(26, 128, 0.55f, col::kAccent, "No room found yet");
            gfx::DrawWrapped(26, 142, 354, 0.43f, col::kText,
                             "On the other console open Titles Delivery > Send Titles, choose the titles and press Y to open the room. It shows "
                             "up here by itself.",
                             4);
        }
        if (g.ToastActive()) {
            DrawStatusStrip(10, 206, 380, 28);
        }
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        if (rows_.empty()) {
            DrawCard(8, 34, 304, 168);
            if (!error_.empty()) {
                gfx::DrawText(160, 60, 0.55f, col::kOff, "Wi-Fi?", gfx::Align::Center);
                gfx::DrawWrapped(22, 76, 276, 0.45f, col::kText, error_, 6);
            } else {
                gfx::DrawText(160, 80, 0.55f, col::kText, "Searching for rooms", gfx::Align::Center);
                DrawSpinner(104.0f);
                gfx::DrawWrapped(22, 124, 276, 0.42f, col::kDim, "A room appears as soon as a console opens one.", 3);
            }
            DrawListChrome(BarTitle(), BarRight());
            DrawFooter(BuildFooter());
        } else {
            ListPage::DrawBottom();
        }
    }

protected:
    const char* Title() const override {
        return "Receive Titles";
    }
    const char* BarTitle() const override {
        return "Rooms";
    }
    std::string BarRight() const override {
        return rows_.empty() ? std::string() : Fmt("%d found", static_cast<int>(rows_.size()));
    }
    const char* ActionLabel() const override {
        return "Join";
    }
    bool StartButton() const override {
        return false;
    }

    void BuildRows(std::vector<Row>& rows) override {
        if (!dirty_) {
            return;
        }
        dirty_ = false;
        rows.clear();
        for (int i = 0; i < dl_scan_count(); ++i) {
            dl_room room;
            if (dl_scan_get(i, &room) != 0) {
                continue;
            }
            Row row;
            row.id = i;
            row.kind = Kind::Link;
            row.label = gfx::Clean(room.host[0] ? room.host : "A console");
            row.value = Fmt("%u %s, %s", room.titles, room.titles == 1 ? "title" : "titles", delivery::SizeText(room.bytes).c_str());
            row.note_title = row.label;
            // the Wi-Fi channel of the room as a small tag (two rooms on neighbouring channels, 6 and 11, slow each other down)
            if (room.channel > 0) {
                row.chip = Fmt("CH %d", room.channel);
            }
            row.note = Fmt("Offers %u %s (%s), the first is %s.", room.titles, room.titles == 1 ? "title" : "titles", delivery::SizeText(room.bytes).c_str(),
                           gfx::Clean(room.first).c_str());
            // how long it takes to get it all: an estimate, from the usual speed of this console (the same average the other times of the app use)
            const double secs = delivery::EstimateSeconds(room.bytes);
            row.note += secs < 45.0 ? std::string("\nEstimated time to download all: under a minute.")
                                    : "\nEstimated time to download all: about " + delivery::ShortDurationText(secs) + ".";
            if (room.channel > 0) {
                row.note += Fmt("\nWi-Fi channel %d", room.channel);
                row.note += room.max_receivers > 0 ? Fmt(", %d of %d places taken.", room.receivers, room.max_receivers) : std::string(".");
            }
            row.note += "\nA: join this room, then choose what to take from it.";
            rows.push_back(row);
        }
        if (list_.sel >= static_cast<int>(rows.size())) {
            list_.sel = 0;
        }
    }

    void Activate(const Row& row, int step) override {
        if (step != 0) {
            return;
        }
        // The room is taken NOW, as the list shows it (a copy): the scan that is still running when it is stopped may make a new list, and the
        // place of the row in it is no longer the same room.
        dl_room room;
        if (dl_scan_pick(row.id, &room) != 0 || gfx::Clean(room.host[0] ? room.host : "A console") != row.label) {
            g.ShowToast("The list of rooms has changed: choose again.", col::kWarn, 3000);
            dirty_ = true;
            return;
        }
        // A scan takes more than a second and cannot be broken off: the join waits for it to end.
        ShowBusyOver(*this, "Joining " + gfx::Clean(room.host), "One moment...", 0.0f);
        StopScan();
        delivery::RunReceive(gfx::Clean(room.host));
        PopPage(); // (the list of rooms is out of date: the result page, if there is one, takes its place)
    }

private:
    void StopScan() {
        if (scanning_) {
            dl_scan_stop();
            scanning_ = false;
        }
        if (home_held_) {
            home_held_ = false;
            g.ReleaseHome();
        }
    }

    delivery::Wireless wireless_;
    bool scanning_ = false;
    bool home_held_ = false;
    std::string error_;
    unsigned generation_ = 0;
    bool dirty_ = true;
};

// ---------------------------------------------------------------------------------------------
// The end of a receiving
// ---------------------------------------------------------------------------------------------

class ReceiveResultPage : public ListPage {
public:
    explicit ReceiveResultPage(const delivery::ReceiveResult& result) : result_(result) {
        if (result_.stats.count < 0 || result_.stats.count > DL_MAX_TITLES) {
            result_.stats.count = 0;
        }
        const dl_recv_stats& s = result_.stats;
        for (int i = 0; i < s.count; ++i) {
            if (dl_bit(s.want, i)) {
                ++wanted_;
                wanted_bytes_ += s.title[i].size;
                if (s.result[i] == DL_RES_INSTALLED) {
                    ++installed_;
                    installed_bytes_ += s.title[i].size;
                } else if (s.result[i] == DL_RES_ERROR) {
                    ++failed_;
                }
            }
        }
        elapsed_ = result_.start_ms != 0 && result_.end_ms >= result_.start_ms ? static_cast<double>(result_.end_ms - result_.start_ms) / 1000.0 : 0.0;
    }

    // The summary of the operation: how it ended, how much came and how long it really took, then (on the touch screen) every title of the room
    // with its size, whether it was installed and when.
    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, Title());
        const dl_recv_stats& s = result_.stats;
        const bool ok = s.all_done && failed_ == 0;
        const u32 color = ok ? col::kOn : (result_.cancelled ? col::kWarn : col::kOff);
        DrawCard(10, 32, 380, 104);
        gfx::FillRect(10, 32, 3, 104, color);
        gfx::DrawText(26, 48, 0.65f, color, ok ? "All received" : (result_.cancelled ? "Cancelled" : "Not everything was received"));
        gfx::DrawText(26, 70, 0.46f, col::kText,
                      Fmt("%d of %d titles installed%s", installed_, wanted_, result_.room.empty() ? "" : (", from " + gfx::Clean(result_.room)).c_str()));
        // What really happened (measured, not estimated): the bytes of what was installed, the time from the moment the receiving began, and
        // the speed that makes.
        float x = delivery::DrawPair(26, 88, 0.44f, "received", delivery::SizeText(installed_bytes_), installed_ > 0 ? col::kOn : col::kText);
        if (elapsed_ > 0.0) {
            x = delivery::DrawPair(x, 88, 0.44f, "in", delivery::ClockText(elapsed_));
            if (installed_bytes_ > 0) {
                delivery::DrawPair(x, 88, 0.44f, "average", Fmt("%.2f MiB/s", static_cast<double>(installed_bytes_) / delivery::kMiB / elapsed_));
            }
        }
        if (wanted_bytes_ > installed_bytes_) {
            gfx::DrawText(26, 104, 0.4f, col::kWarn, "still missing " + delivery::SizeText(wanted_bytes_ - installed_bytes_));
        }
        if (s.msg[0] && !ok) {
            gfx::DrawText(26, 120, 0.4f, col::kDim, gfx::Ellipsize(gfx::Clean(s.msg), 0.4f, 350)); // (what the module said last: why it stopped)
        }
        if (const Row* row = Selected()) {
            DrawNote(10, 140, 380, g.ToastActive() ? 62 : 92, *row);
        }
        if (g.ToastActive()) {
            DrawStatusStrip(10, 206, 380, 28);
        }
        DrawFrameBorder(gfx::kTopW);
    }

protected:
    const char* Title() const override {
        return "Receive Titles";
    }
    const char* BarTitle() const override {
        return "Summary";
    }
    std::string BarRight() const override {
        return Fmt("%d installed", installed_);
    }
    // A goes on to the menu of Titles Delivery; B goes back to the main page; START leaves the app: three different places.
    const char* ActionLabel() const override {
        return "Continue";
    }
    const char* BackLabel() const override {
        return "Main page";
    }
    const char* StartLabel() const override {
        return "Exit";
    }
    void OnBack() override {
        PopPages(2); // (this page and the menu of Titles Delivery under it)
    }

    void BuildRows(std::vector<Row>& rows) override {
        if (!rows.empty()) {
            return;
        }
        const dl_recv_stats& s = result_.stats;
        int first = 0;
        while (first < s.count) {
            int last = first;
            while (last + 1 < s.count && std::strcmp(s.title[last + 1].name, s.title[first].name) == 0) {
                ++last;
            }
            int chosen = 0, done = 0;
            unsigned tags = 0;
            u64 bytes = 0;
            for (int i = first; i <= last; ++i) {
                tags |= delivery::KindTag(s.title[i].kind);
                bytes += s.title[i].size;
                if (dl_bit(s.want, i)) {
                    ++chosen;
                    done += s.result[i] == DL_RES_INSTALLED ? 1 : 0;
                }
            }
            Row header;
            header.id = 1000 + first;
            header.kind = Kind::Check;
            header.label = gfx::Clean(s.title[first].name);
            header.tags = tags;
            header.value = delivery::SizeText(bytes);
            header.on = chosen > 0 && done == chosen;
            header.partial = done > 0 && done < chosen;
            header.note_title = header.label;
            header.note = Fmt("%d of the room's %d %s here: %d chosen, %d installed.", last - first + 1, s.count, last == first ? "title is" : "titles are", chosen, done);
            rows.push_back(header);
            for (int i = first; i <= last; ++i) {
                const bool wanted = dl_bit(s.want, i) != 0;
                Row row;
                row.id = i;
                row.kind = Kind::Check;
                row.indent = 14.0f;
                row.label = delivery::KindText(s.title[i].kind, s.title[i].tid);
                row.label_color = delivery::KindColor(s.title[i].kind);
                row.value = delivery::SizeText(s.title[i].size);
                row.on = s.result[i] == DL_RES_INSTALLED;
                const char* state;
                if (!wanted) {
                    state = "not chosen";
                    row.chip_color = col::kDim;
                    row.dim = true;
                } else if (s.result[i] == DL_RES_INSTALLED) {
                    state = "installed";
                    row.chip_color = col::kOn;
                } else if (s.result[i] == DL_RES_ERROR) {
                    state = "error";
                    row.chip_color = col::kOff;
                } else {
                    state = "not finished";
                    row.chip_color = col::kWarn;
                }
                row.chip = state;
                if (result_.done_ms[i] != 0) {
                    row.detail = delivery::ClockText(static_cast<double>(result_.done_ms[i]) / 1000.0); // (when it was installed, from the start)
                }
                row.note_title = header.label + ": " + row.label;
                row.note = delivery::SizeText(s.title[i].size) + ", " + gfx::Clean(s.title[i].code) + "\n";
                if (s.result[i] == DL_RES_ERROR && s.err[i] == 0xC8604C03u) {
                    row.note += "The receiving was stopped before this title was complete: what had been written was removed."; // (receiver.c's stop path)
                } else if (s.result[i] == DL_RES_ERROR) {
                    row.note += Fmt("The installation failed (error 0x%08lX).", static_cast<unsigned long>(s.err[i]));
                } else if (s.result[i] == DL_RES_INSTALLED) {
                    row.note += "Installed on this console";
                    row.note += result_.done_ms[i] != 0 ? ", " + delivery::ClockText(static_cast<double>(result_.done_ms[i]) / 1000.0) + " after the start." : std::string(".");
                } else {
                    row.note += wanted ? "It was not installed." : "You did not choose it.";
                }
                rows.push_back(row);
            }
            first = last + 1;
        }
        if (rows.empty()) {
            Row row;
            row.kind = Kind::Info;
            row.label = "Nothing was chosen";
            row.note_title = "Nothing was chosen";
            row.note = "The room's list was not shown, or it was left before anything was chosen.";
            rows.push_back(row);
        }
    }

    void Activate(const Row&, int step) override {
        if (step == 0) {
            PopPage();
        }
    }

private:
    delivery::ReceiveResult result_;
    int wanted_ = 0, installed_ = 0, failed_ = 0;
    u64 wanted_bytes_ = 0, installed_bytes_ = 0;
    double elapsed_ = 0.0;
};

} // namespace

namespace pages {

ui::Page* NewDeliveryPage() {
    return new DeliveryPage;
}

ui::Page* NewSendTitlesPage() {
    return new SendTitlesPage;
}

ui::Page* NewReceiveTitlesPage() {
    return new ReceiveTitlesPage;
}

ui::Page* NewReceiveResultPage(const delivery::ReceiveResult& result) {
    return new ReceiveResultPage(result);
}

} // namespace pages
