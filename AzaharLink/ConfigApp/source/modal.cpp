// The popups: a question, a message, a choice, the search of the SD card and the folder browser. A popup is a page pushed over the current
// one. It draws that page first (both screens), dims the touch screen and puts its own card on top, so the user sees what the question is
// about. It pops itself before it calls back, so a callback can push the next popup (the same frame pops this one and pushes that one).
#include "pages.hpp"

using namespace ui;

namespace {

// The top screen of a popup: the page under it, a little dimmed, or (with a long text) a card of its own.
void DrawPopupTop(const std::string& title, const std::string& top_text) {
    if (top_text.empty()) {
        if (Page* below = PageBelow()) {
            below->DrawTop();
        }
        DrawDim(gfx::kTopW);
        return;
    }
    DrawTitleBarStatus(gfx::kTopW, title.c_str());
    DrawCard(10, 32, 380, 200);
    gfx::FillRect(10, 32, 3, 200, col::kAccent);
    gfx::DrawText(26, 48, 0.55f, col::kAccent, gfx::Ellipsize(title, 0.55f, 350));
    gfx::DrawWrapped(26, 62, 354, 0.45f, col::kText, top_text, 12);
    DrawFrameBorder(gfx::kTopW);
}

void DrawPopupUnder() {
    if (Page* below = PageBelow()) {
        below->DrawBottom();
    }
    DrawDim(gfx::kBottomW);
}

// ---------------------------------------------------------------------------------------------
// A question
// ---------------------------------------------------------------------------------------------

class ConfirmPage : public Page {
public:
    ConfirmPage(const std::string& title, const std::string& text, const std::string& yes, const std::string& no, pages::YesNoFn done)
        : title_(title), text_(text), yes_(yes), no_(no), done_(std::move(done)), box_(LayoutPopup(text, true)) {}

    void Update(const Input& in) override {
        const bool yes = (in.down & KEY_A) || Tapped(in, box_.yes);
        const bool no = (in.down & KEY_B) || Tapped(in, box_.no);
        if (yes || no) {
            PopPage();
            if (done_) {
                done_(yes);
            }
        }
    }

    void DrawTop() override {
        DrawPopupTop(title_, std::string());
    }

    void DrawBottom() override {
        DrawPopupUnder();
        DrawPopupBox(box_, title_, text_); // (the card is as high as its text, in the middle, and holds its two buttons)
        const Input& in = g.input.in();
        DrawButton(box_.yes, "A", yes_.c_str(), true, PressedNow(in, box_.yes));
        DrawButton(box_.no, "B", no_.c_str(), true, PressedNow(in, box_.no));
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    std::string title_, text_, yes_, no_;
    pages::YesNoFn done_;
    PopupBox box_;
};

// ---------------------------------------------------------------------------------------------
// A message
// ---------------------------------------------------------------------------------------------

class MessagePage : public Page {
public:
    MessagePage(const std::string& title, const std::string& text, const std::string& top_text)
        : title_(title), text_(text), top_text_(top_text), box_(LayoutPopup(text, false)) {}

    void Update(const Input& in) override {
        if ((in.down & (KEY_A | KEY_B)) || Tapped(in, box_.ok)) {
            PopPage();
        }
    }

    void DrawTop() override {
        DrawPopupTop(title_, top_text_);
    }

    void DrawBottom() override {
        DrawPopupUnder();
        DrawPopupBox(box_, title_, text_);
        DrawButton(box_.ok, "A", "OK", true, PressedNow(g.input.in(), box_.ok));
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    std::string title_, text_, top_text_;
    PopupBox box_;
};

// ---------------------------------------------------------------------------------------------
// A choice
// ---------------------------------------------------------------------------------------------

class ChoicePage : public Page {
public:
    ChoicePage(const std::string& title, const std::string& text, const std::vector<std::string>& options, pages::ChoiceFn done,
               const std::string& top_text)
        : title_(title), text_(text), options_(options), done_(std::move(done)), top_text_(top_text) {}

    void Update(const Input& in) override {
        const int n = static_cast<int>(options_.size());
        if (in.down & KEY_B) {
            PopPage(); // (no choice: nothing is called back)
            return;
        }
        if (in.nav & KEY_UP) {
            sel_ = (sel_ + n - 1) % n;
        }
        if (in.nav & KEY_DOWN) {
            sel_ = (sel_ + 1) % n;
        }
        int chosen = (in.down & KEY_A) ? sel_ : -1;
        for (int i = 0; i < n; ++i) {
            if (Tapped(in, OptionRect(i))) {
                sel_ = chosen = i;
            }
        }
        if (chosen >= 0) {
            PopPage();
            if (done_) {
                done_(chosen);
            }
        }
    }

    void DrawTop() override {
        DrawPopupTop(title_, top_text_);
    }

    void DrawBottom() override {
        DrawPopupUnder();
        const float card_h = 78.0f + 34.0f * static_cast<float>(options_.size());
        DrawCard(8, 22, 304, card_h);
        gfx::FillRect(8, 22, 3, card_h, col::kAccent);
        gfx::DrawText(22, 38, 0.55f, col::kAccent, gfx::Ellipsize(title_, 0.55f, 280));
        gfx::DrawWrapped(22, 52, 280, 0.42f, col::kText, text_, 2);
        const Input& in = g.input.in();
        for (int i = 0; i < static_cast<int>(options_.size()); ++i) {
            const Rect r = OptionRect(i);
            const bool selected = i == sel_;
            const bool pressed = PressedNow(in, r);
            gfx::FillRect(r.x, r.y, r.w, r.h, pressed ? col::kSelPressed : (selected ? col::kSel : col::kPanelAlt));
            gfx::StrokeRect(r.x, r.y, r.w, r.h, 1.0f, selected ? col::kAccent : col::kLine);
            gfx::DrawFitted(r.x + 12, r.y + r.h * 0.5f, 0.5f, 0.38f, r.w - 24, selected ? col::kText : col::kDim, options_[static_cast<std::size_t>(i)]);
        }
        gfx::DrawText(160, 22 + card_h - 10, 0.4f, col::kDim, "Up / Down, A choose, B cancel", gfx::Align::Center);
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    static Rect OptionRect(int i) {
        return {20.0f, 82.0f + 34.0f * static_cast<float>(i), 280.0f, 30.0f};
    }

    std::string title_, text_;
    std::vector<std::string> options_;
    pages::ChoiceFn done_;
    std::string top_text_;
    int sel_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The search of the SD card
// ---------------------------------------------------------------------------------------------

class SearchPage : public Page {
public:
    SearchPage(const std::string& file_name, setup::FileSearch::Accept accept, const setup::FileSearch::Options& options, pages::SearchFn done)
        : file_name_(file_name), search_("sdmc:", file_name, std::move(accept), options), done_(std::move(done)), start_ms_(NowMs()) {}

    void Update(const Input& in) override {
        ++frame_;
        if ((in.down & KEY_B) || Tapped(in, kPopupOk)) {
            search_.Cancel();
            PopPage();
            if (done_) {
                done_(pages::SearchResult::Cancelled, std::string());
            }
            return;
        }
        // A slice of work a frame: the screen keeps drawing and B keeps working however large the card is.
        const u64 begin = NowMs();
        do {
            search_.Step(24);
#ifdef AZL_TEST_SEARCH_DELAY_MS
            svcSleepThread(static_cast<s64>(AZL_TEST_SEARCH_DELAY_MS) * 1000000LL); // (a build made to look at the search on an emulator's tiny card)
#endif
        } while (!search_.finished() && NowMs() - begin < 8);
        if (search_.finished()) {
            PopPage();
            if (done_) {
                done_(search_.found() ? pages::SearchResult::Found : pages::SearchResult::NotFound, search_.result());
            }
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, "Searching the SD card");
        DrawCard(10, 32, 380, 200);
        gfx::FillRect(10, 32, 3, 200, col::kAccent);
        gfx::DrawText(26, 52, 0.6f, col::kText, "Looking for " + file_name_);
        gfx::DrawText(26, 78, 0.45f, col::kDim, "In the folder");
        gfx::DrawFitted(26, 96, 0.46f, 0.34f, 350, col::kText, gfx::Clean(search_.current_folder()));
        const u64 seconds = (NowMs() - start_ms_) / 1000;
        gfx::DrawText(26, 128, 0.45f, col::kText,
                      Fmt("%d folders looked at, %ld entries", search_.folders_done(), search_.entries_seen()));
        gfx::DrawText(26, 146, 0.45f, col::kText, Fmt("%llu s", static_cast<unsigned long long>(seconds)));
        gfx::DrawWrapped(26, 172, 350, 0.42f, col::kDim,
                         "This can take a while on a large card. B stops the search. The console's own Nintendo 3DS folder is not searched.", 3);
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        DrawPopupUnder();
        DrawPopupCard(30, 182, "Searching...", "Looking for " + file_name_ + " in every folder of the SD card. This can take a while.", 4);
        DrawSpinner(128.0f);
        DrawButton(kPopupOk, "B", "Cancel", true, PressedNow(g.input.in(), kPopupOk));
        DrawFrameBorder(gfx::kBottomW);
    }

private:
    std::string file_name_;
    setup::FileSearch search_;
    pages::SearchFn done_;
    u64 start_ms_;
    u32 frame_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The folder browser
// ---------------------------------------------------------------------------------------------

class FolderPage : public Page {
public:
    FolderPage(const std::string& title, const std::string& hint, pages::FolderFn chosen)
        : title_(title), hint_(hint), chosen_(std::move(chosen)) {
        Load("sdmc:", std::string());
    }

    void Update(const Input& in) override {
        if (in.down & KEY_START) {
            g.quit = true;
            return;
        }
        const ListEvent event = UpdateList(list_, rows_, in);
        if ((in.down & KEY_B) || Tapped(in, kBackButton)) {
            if (path_ == "sdmc:") {
                PopPage(); // (the top of the card: B leaves the browser)
            } else {
                Load(setup::ParentDir(path_), setup::BaseName(path_));
            }
            return;
        }
        int activated = event.activated;
        if (activated < 0 && !rows_.empty() && Tapped(in, ActionButton("Open", true))) {
            activated = Clamp(list_.sel, 0, static_cast<int>(rows_.size()) - 1); // (the A button of the footer)
        }
        if (activated < 0) {
            return;
        }
        if (activated == 0) {
            if (chosen_ && chosen_(path_)) {
                PopPage();
            }
        } else {
            Load(setup::JoinPath(path_, dirs_[static_cast<std::size_t>(activated - 1)]), std::string());
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, title_.c_str());
        DrawCard(10, 32, 380, 168);
        gfx::FillRect(10, 32, 3, 168, col::kAccent);
        gfx::DrawText(26, 48, 0.5f, col::kDim, "Folder");
        gfx::DrawWrapped(26, 60, 354, 0.55f, col::kText, gfx::Clean(path_), 2);
        gfx::DrawWrapped(26, 104, 354, 0.45f, col::kText, hint_, 4);
        gfx::DrawText(26, 180, 0.42f, col::kDim, !ok_ ? std::string("This folder cannot be read") : dirs_.size() == 1 ? std::string("1 folder here") : Fmt("%d folders here", static_cast<int>(dirs_.size())));
        DrawStatusStrip(10, 206, 380, 28);
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        DrawRows(list_, rows_);
        DrawListChrome("Choose a folder", std::string());
        Footer footer;
        footer.back_label = path_ == "sdmc:" ? "Cancel" : "Up";
        footer.action_label = "Open";
        DrawFooter(footer);
    }

private:
    // Lists a folder; `select` (a sub-folder's name) puts the cursor back on the folder the user came out of.
    void Load(const std::string& path, const std::string& select) {
        path_ = path;
        dirs_ = setup::ListSubfolders(path_, &ok_);
        rows_.clear();
        Row use;
        use.kind = Kind::Action;
        use.label = "Use this folder";
        use.note_title = "Use this folder";
        rows_.push_back(use);
        int sel = 0;
        for (std::size_t i = 0; i < dirs_.size(); ++i) {
            Row row;
            row.kind = Kind::Link;
            row.label = gfx::Clean(dirs_[i]);
            row.gap = i == 0 ? 6.0f : 0.0f;
            rows_.push_back(row);
            if (dirs_[i] == select) {
                sel = static_cast<int>(i) + 1;
            }
        }
        list_ = ListView();
        list_.sel = sel;
    }

    std::string title_, hint_;
    pages::FolderFn chosen_;
    std::string path_;
    std::vector<std::string> dirs_;
    bool ok_ = true;
    ListView list_;
    std::vector<Row> rows_;
};

} // namespace

namespace pages {

ui::Page* NewConfirmPage(const std::string& title, const std::string& text, const std::string& yes_label, const std::string& no_label, YesNoFn done) {
    return new ConfirmPage(title, text, yes_label, no_label, std::move(done));
}

ui::Page* NewMessagePage(const std::string& title, const std::string& text, const std::string& top_text) {
    return new MessagePage(title, text, top_text);
}

ui::Page* NewChoicePage(const std::string& title, const std::string& text, const std::vector<std::string>& options, ChoiceFn done,
                        const std::string& top_text) {
    return new ChoicePage(title, text, options, std::move(done), top_text);
}

ui::Page* NewSearchPage(const std::string& file_name, setup::FileSearch::Accept accept, const setup::FileSearch::Options& options, SearchFn done) {
    return new SearchPage(file_name, std::move(accept), options, std::move(done));
}

ui::Page* NewFolderPage(const std::string& title, const std::string& hint, FolderFn chosen) {
    return new FolderPage(title, hint, std::move(chosen));
}

} // namespace pages
