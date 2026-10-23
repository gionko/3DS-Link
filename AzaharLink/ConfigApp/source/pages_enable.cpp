// Page: Enable Titles for Azahar Emulator Net.
//
// It lists the titles installed on this console (the SD card and the card in the slot) with their icon and name and a switch each.
// Switching a title ON puts a copy of the plugin (AzaharLink.3gx, the file 3DS Link keeps in its own folder: it carries the plugin inside and puts it there by itself)
// in the title's own plugin folder, sd:/luma/plugins/<TID>/AzaharLink.3gx, which is the folder Luma3DS loads a title's plugin from; OFF removes it (and the folder,
// when nothing else is in it). The switch is read from the card every time the page opens, so a copy made or removed by hand shows as it is.
// FAT32 has no links: a copy of about 1 MB per title is the design.
//
// The copies are made by a thread of their own (copyworker.hpp), one after the other: the page stays alive while it works, and the user can
// switch more titles on. A title that is being made shows "Enabling..." in green in place of its name (its icon stays), then "Enabled" for
// two seconds, then its name again with the switch on.
#include "copyworker.hpp"
#include "pages.hpp"
#include "plugin_resource.hpp"
#include "titles.hpp"

using namespace ui;

namespace {

constexpr u64 kFlashMs = 2000; // how long "Enabled" stays in place of the name

class EnableGamePage : public ListPage {
public:
    EnableGamePage() : generation_(titles::ScanGeneration()) {
        // What an earlier visit left in the queue's results is no news: the switches are read from the card.
        setup::CopyDone ignored;
        while (setup::CopyWorker::Get().TakeDone(ignored)) {
        }
        RefreshSource();
    }

    ~EnableGamePage() override {
        // The icon textures are keyed by where the icon data is: the card title (this page's own) goes away with the page, and a
        // later one could be put at the same place.
        gfx::ReleaseIcons();
    }

    void Update(const Input& in) override {
        if (generation_ != titles::ScanGeneration()) {
            // The titles were read again (the key file appeared): what this page holds points into the old scan.
            generation_ = titles::ScanGeneration();
            entries_.clear();
            rows_.clear();
            built_ = false;
            dirty_ = true;
        }
        if (!source_ok_ && (++recheck_ % 15) == 0) {
            RefreshSource(); // (the plugin file may have been put back since: by A on this page, or by the Installation check)
        }
        if (!built_ && source_ok_ && titles::Scan().done()) {
            BuildEntries();
        }
        TakeFinishedCopies();
        if (Animating()) {
            dirty_ = true; // (the rows say "Enabling..." or "Enabled": they are made again while that lasts)
        }
        ListPage::Update(in);
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, Title());
        DrawCard(10, 32, 380, 168);
        gfx::FillRect(10, 32, 3, 168, col::kAccent);
        const Entry* entry = SelectedEntry();
        if (entry) {
            DrawGameCard(*entry);
        } else {
            std::string title, text, detail;
            Message(title, text, detail);
            gfx::DrawText(26, 48, 0.55f, col::kAccent, gfx::Ellipsize(title, 0.55f, 350));
            gfx::DrawWrapped(26, 62, 350, 0.45f, col::kText, detail.empty() ? text : text + "\n\n" + detail, 9);
        }
        DrawStatusStrip(10, 206, 380, 28);
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        if (entries_.empty()) {
            std::string title, text, detail;
            Message(title, text, detail);
            DrawCard(8, 34, 304, 168);
            gfx::DrawText(160, 54, 0.55f, col::kAccent, gfx::Ellipsize(title, 0.55f, 280), gfx::Align::Center);
            gfx::DrawWrapped(22, 68, 276, 0.45f, col::kText, text, 6);
            if (source_ok_ && !titles::Scan().done()) {
                DrawProgressBar(22, 100, 276, 12, setup::ProgressFraction(titles::Scan().progress_done(), titles::Scan().progress_total()));
            }
            DrawListChrome(BarTitle(), BarRight());
            Footer footer;
            footer.action_label = EmptyLabel();
            DrawFooter(footer);
        } else {
            ListPage::DrawBottom();
        }
    }

protected:
    const char* Title() const override {
        return "Enable Titles for Azahar Emulator Net";
    }
    const char* BarTitle() const override {
        return "Enable Titles";
    }
    std::string BarRight() const override {
        int on = 0;
        for (const Entry& e : entries_) {
            on += e.slot == setup::Slot::On ? 1 : 0;
        }
        return entries_.empty() ? std::string() : Fmt("%d / %d ON", on, static_cast<int>(entries_.size()));
    }
    const char* ActionLabel() const override {
        return "Switch";
    }
    void BuildRows(std::vector<Row>& rows) override {
        if (!dirty_) {
            return; // (the rows are the same as last frame: this page can list a few hundred titles)
        }
        dirty_ = false;
        rows.clear();
        const u64 now = NowMs();
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            Row row;
            row.id = static_cast<int>(i);
            row.label = e.name;
            row.icon = e.game->icon;
            if (e.slot == setup::Slot::Other) {
                row.kind = Kind::Info;
                row.value = "other plugin";
                row.value_color = col::kWarn;
                row.dim = true;
            } else {
                row.kind = Kind::Toggle;
                row.on = e.slot == setup::Slot::On;
            }
            setup::CopyKind kind;
            if (setup::CopyWorker::Get().Pending(e.game->tid, &kind)) {
                row.status_text = kind == setup::CopyKind::Disable ? "Disabling" : (kind == setup::CopyKind::Update ? "Updating" : "Enabling");
                row.status_color = kind == setup::CopyKind::Disable ? col::kWarn : col::kOn;
                row.busy = true;
            } else if (now < e.flash_until) {
                row.status_text = e.flash_text;
                row.status_color = e.flash_color;
            }
            rows.push_back(row);
        }
    }

    void OnKeys(const Input& in) override {
        const char* label = EmptyLabel();
        const bool pressed = *label && ((in.down & KEY_A) || Tapped(in, ActionButton(label, true)));
        if (!entries_.empty() || !pressed) {
            return;
        }
        // The page has no list: A does what the message says.
        switch (Action()) {
        case Fix::Plugin:
            pages::RestorePlugin();
            break;
        case Fix::Boot9:
            pages::StartBoot9Choice();
            break;
        case Fix::None:
            break;
        }
    }

    void Activate(const Row& row, int step) override {
        if (row.id < 0 || row.id >= static_cast<int>(entries_.size())) {
            return;
        }
        Entry& e = entries_[static_cast<std::size_t>(row.id)];
        if (e.slot == setup::Slot::Other) {
            if (step == 0) {
                g.ShowToast("Another plugin is in this title's folder: remove it first.", col::kWarn, 5000);
            }
            return;
        }
        setup::CopyWorker& worker = setup::CopyWorker::Get();
        if (worker.Pending(e.game->tid, nullptr)) {
            return; // (its copy is being made: it can be switched again when it is done)
        }
        const bool on = e.slot == setup::Slot::On;
        if (SwitchTarget(on, step) == on) {
            return;
        }
        if (on) {
            worker.Submit(e.game->tid, setup::CopyKind::Disable, std::string());
        } else {
            if (!source_ok_) {
                g.ShowToast("AzaharLink.3gx is gone from " + g.paths.appdir + ".", col::kOff, 5000);
                return;
            }
            worker.Submit(e.game->tid, setup::CopyKind::Enable, g.paths.plugin_file);
        }
        dirty_ = true;
    }

private:
    struct Entry {
        const titles::Game* game = nullptr;
        std::string name;
        setup::Slot slot = setup::Slot::Off;
        u64 flash_until = 0;  // "Enabled" / "Disabled" stays in place of the name until then
        std::string flash_text;
        u32 flash_color = 0;
    };

    // What A can do when the list is not there.
    enum class Fix { None, Plugin, Boot9 };

    void RefreshSource() {
        g.LookAtPlugin(false);
        bool exists = false;
        source_ = setup::DescribePlugin(g.paths.plugin_file, &exists);
        source_exists_ = exists;
        // The copies are made from the master file, and it is good only when it is, byte for byte, the plugin inside the app (3DS Link puts it there by itself): the app and
        // its plugin are one version, and a copy of a plugin that is not the app's is not made.
        source_ok_ = exists && g.plugin_card == setup::PluginCard::Same;
    }

    // The A button of the page when the list is not there ("" when there is nothing to do).
    const char* EmptyLabel() const {
        return Action() == Fix::None ? "" : (Action() == Fix::Plugin ? "Fix" : "Find");
    }

    Fix Action() const {
        if (!source_ok_) {
            return setup::EmbeddedPluginUsable() ? Fix::Plugin : Fix::None; // (a build with no plugin inside has nothing to put back)
        }
        if (titles::Scan().done()) {
            const titles::ScanReport report = titles::Scan().report();
            if (report.applications > 0 && report.recognised == 0 && !report.have_keys) {
                return Fix::Boot9;
            }
        }
        return Fix::None;
    }

    // Why the list is not there: the plugin file is missing, the scan is not over, or there is no title at all. `text` is the one
    // message (on both screens); `detail` is what the top screen adds.
    void Message(std::string& title, std::string& text, std::string& detail) const {
        detail.clear();
        if (!setup::EmbeddedPluginUsable()) {
            title = "No plugin in this app";
            text = "This copy of 3DS Link was built without its plugin. Install the 3DS Link CIA again.";
            detail = "3DS Link carries AzaharLink.3gx inside and copies it into the folder of each title you enable (sdmc:/luma/plugins/<title id>/). This build has none to give.";
        } else if (!source_exists_) {
            title = "AzaharLink.3gx is missing";
            text = "3DS Link puts its plugin on the card by itself, and could not. Press A to try again.";
            detail = "3DS Link copies it into the folder of each title you enable (sdmc:/luma/plugins/<title id>/), so it needs that file. "
                     "Expected here:\n" + g.paths.plugin_file + (g.plugin_tried && g.plugin_write != setup::WriteResult::Ok
                                                                    ? std::string("\nIt could not be put there: ") + setup::WriteFailurePhrase(g.plugin_write) + "."
                                                                    : std::string());
        } else if (!source_ok_) {
            title = "AzaharLink.3gx is not the plugin of this app";
            text = "Press A to put the plugin of this app back.";
            detail = "The file " + g.paths.plugin_file + " (" + setup::FormatSize(source_.size) +
                     ") is not, byte for byte, the plugin inside 3DS Link: it is damaged or of another version, and the app and its plugin are one version.";
        } else if (!titles::Scan().done()) {
            title = "Reading your titles...";
            text = setup::ScanProgressText(titles::Scan().progress());
            detail = "The list appears as soon as the titles are read. This happens at every start; the first time it takes "
                     "longer.";
        } else if (const titles::ScanReport report = titles::Scan().report(); report.applications > 0 && report.recognised == 0) {
            // Titles are there, but their names and icons could not be read: the system refuses a 3dsx, and no key file is there.
            title = "The titles cannot be read";
            text = report.have_keys ? "The names and icons of your titles could not be read from their files."
                                    : "This console does not let this app read the names and icons of your titles. Press A to find boot9.bin: "
                                      "with it they can be read.";
            detail = Fmt("%d titles are installed on the SD card.", report.applications);
        } else {
            title = "No title found";
            text = "Titles installed on the SD card and the title in the card slot are listed here.";
            detail = "Only a title with a readable name and icon is listed.";
        }
    }

    const Entry* SelectedEntry() const {
        if (entries_.empty()) {
            return nullptr;
        }
        const int i = Clamp(list_.sel, 0, static_cast<int>(entries_.size()) - 1);
        return &entries_[static_cast<std::size_t>(i)];
    }

    void DrawGameCard(const Entry& e) {
        // The big icon, twice its size.
        gfx::FillRect(22, 44, 100, 100, col::kPanelAlt);
        gfx::StrokeRect(22, 44, 100, 100, 1.0f, col::kLine);
        gfx::DrawIcon(e.game->icon, setup::kLargeIcon, 24.0f, 46.0f, 2.0f, e.slot == setup::Slot::Other ? 140 : 255);
        // Everything else is in the space at the right of the icon: the name, the ID, whether 3DS Link is on for the title, and what the switch is for.
        const float used = gfx::DrawWrapped(136, 44, 244, 0.62f, col::kText, e.name, 3);
        const float id_y = 44.0f + used + 8.0f;
        gfx::DrawFitted(136, id_y, 0.52f, 0.4f, 244, col::kDim, "ID: " + setup::TidText(e.game->tid) + (e.game->card ? "  (game card)" : ""));
        u32 dot = col::kTrack;
        const char* state = "3DS Link is OFF for this title";
        std::string note = "Switched on, this title connects to the Azahar emulator network room set in Azahar Emulator Room.";
        switch (e.slot) {
        case setup::Slot::On:
            dot = col::kOn;
            state = "3DS Link is ON for this title";
            break;
        case setup::Slot::Other:
            dot = col::kWarn;
            state = "Another plugin is in its folder";
            note = "Remove that plugin first (" + setup::GamePluginDir(e.game->tid) + "/); 3DS Link never touches it.";
            break;
        case setup::Slot::Off:
            break;
        }
        const float state_y = id_y + 22.0f;
        gfx::FillCircle(142, state_y, 5, dot);
        gfx::DrawFitted(154, state_y, 0.52f, 0.4f, 226, col::kText, state);
        gfx::DrawWrapped(136, state_y + 15.0f, 244, 0.46f, col::kDim, note, 4);
    }

    // The titles of the scan, the title in the card slot, and what the card says about each: one listing of sd:/luma/plugins, then a
    // look at the folder of every title that has one.
    void BuildEntries() {
        built_ = true;
        std::vector<const titles::Game*> games;
        for (const std::unique_ptr<titles::Game>& game : titles::Scan().games()) {
            games.push_back(game.get());
        }
        card_.reset(new titles::Game);
        if (titles::ReadCardGame(g.language, *card_)) {
            bool listed = false;
            for (const titles::Game* game : games) {
                listed = listed || game->tid == card_->tid;
            }
            if (!listed) {
                std::size_t at = 0;
                while (at < games.size() && !setup::NameLess(card_->name, games[at]->name)) {
                    ++at;
                }
                games.insert(games.begin() + static_cast<std::ptrdiff_t>(at), card_.get());
            }
        }
        const std::set<std::string> folders = setup::ListPluginFolders();
        setup::CopyWorker& worker = setup::CopyWorker::Get();
        for (const titles::Game* game : games) {
            Entry e;
            e.game = game;
            e.name = gfx::Clean(game->name);
            if (folders.count(setup::TidText(game->tid)) != 0) {
                const setup::SlotInfo info = setup::ReadSlot(game->tid);
                e.slot = info.slot;
                if (info.slot == setup::Slot::On && setup::CopyIsStale(info.copy_size, source_.size) && !worker.Pending(game->tid, nullptr)) {
                    // The plugin file was replaced by a newer one: the copy is made again, in the background like any other.
                    worker.Submit(game->tid, setup::CopyKind::Update, g.paths.plugin_file);
                }
            }
            entries_.push_back(e);
        }
        dirty_ = true;
    }

    // The copies the worker has finished since the last frame: the entry takes the new state and says so for a moment.
    void TakeFinishedCopies() {
        setup::CopyDone done;
        while (setup::CopyWorker::Get().TakeDone(done)) {
            Entry* entry = nullptr;
            for (Entry& e : entries_) {
                if (e.game->tid == done.tid) {
                    entry = &e;
                    break;
                }
            }
            if (!entry) {
                continue;
            }
            dirty_ = true;
            const setup::SlotInfo info = setup::ReadSlot(done.tid); // (what the card says now)
            entry->slot = info.slot;
            if (done.kind == setup::CopyKind::Disable) {
                if (done.disable_ok) {
                    entry->flash_text = "Disabled";
                    entry->flash_color = col::kWarn;
                    entry->flash_until = NowMs() + kFlashMs;
                } else {
                    g.ShowToast("Could not remove the plugin from the title's folder.", col::kOff, 5000);
                }
                continue;
            }
            switch (done.enable_result) {
            case setup::EnableResult::Ok:
                entry->flash_text = done.kind == setup::CopyKind::Update ? "Updated" : "Enabled";
                entry->flash_color = col::kOn;
                entry->flash_until = NowMs() + kFlashMs;
                break;
            case setup::EnableResult::OtherPlugin:
                g.ShowToast("Another plugin is in this title's folder: remove it first.", col::kWarn, 5000);
                break;
            case setup::EnableResult::SourceMissing:
                g.ShowToast("AzaharLink.3gx is gone from " + g.paths.appdir + ".", col::kOff, 5000);
                break;
            case setup::EnableResult::CopyFailed:
                g.ShowToast("Could not copy the plugin (is the card full or locked?).", col::kOff, 5000);
                break;
            }
        }
    }

    // Is a row saying something that changes (a copy under way, a word that stays two seconds)?
    bool Animating() const {
        if (setup::CopyWorker::Get().PendingCount() > 0) {
            return true;
        }
        const u64 now = NowMs();
        for (const Entry& e : entries_) {
            if (e.flash_until != 0 && now < e.flash_until + 50) {
                return true;
            }
        }
        return false;
    }

    setup::PluginFile source_;
    bool source_exists_ = false;
    bool source_ok_ = false;
    bool built_ = false;
    bool dirty_ = true;
    int generation_;
    unsigned recheck_ = 0;
    std::vector<Entry> entries_;
    std::unique_ptr<titles::Game> card_; // the title in the card slot, when there is one (the scan only knows the SD card)
};

} // namespace

namespace pages {

ui::Page* NewEnableGamePage() {
    return new EnableGamePage;
}

} // namespace pages
