// Page: Installation check, and what A does about each problem it lists. The same page is the summary at the end of the first start.
//
//   Plugin file    3DS Link carries its plugin inside (plugin_resource.hpp) and puts it on the card by itself, at every start and when this page opens; the row
//                  compares the card with the plugin inside the app, byte for byte. A puts the plugin back from the app (replacing a file that is not the same):
//                  that file is the one every title's copy is made from.
//   Plugin folder  A makes sdmc:/luma/plugins when it is not there.
//   boot9.bin      A opens a popup with three ways: search the whole card, choose a folder, or how to get the file (the guide, GodMode9's steps with a
//                  picture each; Y on the row opens it at once, and the footer says so). A boot9.bin that is found is used where it is (its path
//                  goes to 3DSLink.cfg).
//   Plugin Loader  A switches Rosalina's Plugin Loader on (the same switch as its menu).
//   Installed titles  A asks, listing them, then deletes the titles the scan found of no use (begun and never finished, unreadable, a DLC with
//                  contents missing) and reads the titles again.
//   Seed database  only shown: it is a file of the console's own memory; nothing here can make it readable.
// On a line that is fine A is not offered (the footer has B and START only).
#include "dlp_text.hpp"
#include "pages.hpp"
#include "titles.hpp"

using namespace ui;

namespace {

// The key file the user pointed to (or the search found): used where it is, remembered in 3DSLink.cfg.
void UseBoot9(const std::string& path) {
    g.app_cfg.boot9_path = path;
    g.SaveAppConfig();
    g.RefreshInstall();
    g.ShowToast("boot9.bin found: " + gfx::Clean(setup::ParentDir(path)), col::kOn, 5000);
}

void SearchBoot9() {
    setup::FileSearch::Options options;
    // Every .bin is looked at (a file of the right size is read and told by its content: a boot9 dump may be called anything); the accept below
    // takes it when its content says boot9 or its name does (boot9.bin, or GodMode9 2.x's "<serial>_boot9_00.bin").
    options.name_match = [](const std::string& name) { return name.size() > 4 && setup::SameNameNoCase(name.substr(name.size() - 4), ".bin"); };
    PushPage(pages::NewSearchPage(setup::kBoot9FileName,
                                  [](const std::string& path) {
                                      return setup::IsUsableBoot9(path) &&
                                             (setup::IsBoot9Name(setup::BaseName(path)) || setup::ClassifyBoot9File(path) == setup::Boot9Verdict::Good);
                                  },
                                  options,
                                  [](pages::SearchResult result, const std::string& path) {
                                      if (result == pages::SearchResult::Found) {
                                          UseBoot9(path);
                                      } else if (result == pages::SearchResult::NotFound) {
                                          PushPage(pages::NewMessagePage("boot9.bin not found",
                                                                         "boot9.bin is not on the SD card (a 64 KB or 32 KB file with boot9 in its name).",
                                                                         "boot9.bin was not found anywhere on the SD card.\n\nA file called boot9.bin "
                                                                         "(or with boot9 in its name, as GodMode9 2.x makes it: <serial>_boot9_00.bin) and "
                                                                         "the right size (64 KB, or 32 KB) is needed. See \"How to get it\" in the "
                                                                         "same popup, or put boot9.bin next to 3DS Link (" +
                                                                             g.paths.appdir + ")."));
                                      }
                                  }));
}

void ChooseBoot9Folder() {
    PushPage(pages::NewFolderPage("boot9.bin: choose its folder",
                                  "Go to the folder that holds boot9.bin and choose \"Use this folder\" (the first row). A opens a folder, B goes up.",
                                  [](const std::string& folder) {
                                      const std::string file = setup::FindBoot9InFolder(folder); // (boot9.bin, or GodMode9 2.x's <serial>_boot9_00.bin)
                                      if (!file.empty()) {
                                          UseBoot9(file);
                                          return true;
                                      }
                                      g.ShowToast("No boot9 file in " + gfx::Clean(folder), col::kWarn, 4000);
                                      return false;
                                  }));
}

void ExplainBoot9() {
    PushPage(pages::NewMessagePage(
        "How to get boot9.bin", "It is dumped from your own console: see the long text on the top screen.",
        "boot9.bin comes from your own console. With GodMode9 (a separate tool, on the SD card):\n\n"
        "1. Power the console off, then hold START and press POWER: GodMode9 opens.\n"
        "2. Press HOME for its menu, find the entry about the console's hardware files (the names differ between versions) and "
        "dump boot9.bin to the SD card.\n"
        "3. It lands in sdmc:/gm9/out/ and 3DS Link finds it there by itself, whatever its name: GodMode9 2.x calls it after your console's "
        "serial (for example ABC12345678_boot9_00.bin; the boot11 file next to it is not the one). Or put it next to 3DS Link: " +
            g.paths.appdir + ".\n\nThen open this Installation check again."));
}

// A on "Plugin Loader": the same switch as Rosalina's menu, set from here; the answer tells what the loader says afterwards.
void EnableLoaderNow() {
    const Result rc = setup::SetPluginLoader(true);
    g.RefreshInstall(); // (this reads the loader again: what it says now, not what was asked)
    bool good = false;
    const std::string text = setup::LoaderEnableMessage(static_cast<u32>(rc), g.loader, &good);
    g.ShowToast(text, good ? col::kOn : col::kOff, 6000);
}

// ---------------------------------------------------------------------------------------------
// Page: Installation check
// ---------------------------------------------------------------------------------------------

class InstallCheckPage : public ListPage {
public:
    InstallCheckPage(std::function<void(bool)> on_close, bool summary) : on_close_(std::move(on_close)), summary_(summary) {}

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, summary_ ? "First start" : Title());
        // The summary: one line (red while there is a problem, yellow when there are only warnings), then the note of the selected row. The
        // rows are in the order of urgency, so the first one is what the line talks about.
        const bool problems = g.install.problems > 0;
        const bool warnings = g.install.warnings > 0;
        const u32 color = problems ? col::kOff : (warnings ? col::kWarn : col::kOn);
        DrawCard(10, 32, 380, 34);
        gfx::FillCircle(28, 49, 7, color);
        gfx::DrawText(44, 49, 0.62f, col::kText, "Installation: " + setup::InstallSummary(g.install));
        gfx::DrawText(382, 49, 0.42f, col::kDim, problems || warnings ? "A on a line fixes it" : "everything 3DS Link needs is there",
                      gfx::Align::Right);
        if (const Row* row = Selected()) {
            DrawNote(10, 72, 380, 158, *row);
        }
        DrawFrameBorder(gfx::kTopW);
    }

protected:
    const char* Title() const override {
        return "Installation check";
    }
    std::string BarRight() const override {
        return setup::InstallSummary(g.install);
    }
    // A line that is fine has nothing to fix: its A button is not there (only B and START are).
    const char* ActionLabel() const override {
        const Row* row = Selected();
        return row && row->kind == Kind::Link ? "Fix" : "";
    }
    // The first start's summary is a step of the flow: B cancels it (nothing is saved, the first start comes again) and START goes on.
    const char* BackLabel() const override {
        return summary_ ? "Cancel" : "Back";
    }
    const char* StartLabel() const override {
        return summary_ ? "Continue" : "Save & exit";
    }
    // On the boot9.bin row that is not fine, Y opens the guide (GodMode9's steps, a picture each) at once: it is not a button, so the footer's hint says it.
    const char* Hint() const override {
        return Boot9RowToFix() ? dlp::kGuideHint : "";
    }
    void OnKeys(const Input& in) override {
        if ((in.down & KEY_Y) && Boot9RowToFix() && !pages::OpenBoot9Guide()) {
            ExplainBoot9();
        }
    }
    void OnBack() override {
        PopPage();
        if (summary_ && on_close_) {
            on_close_(false);
        }
    }
    void OnStart() override {
        if (!summary_) {
            g.quit = true;
            return;
        }
        PopPage();
        if (on_close_) {
            on_close_(true);
        }
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        for (const std::size_t index : setup::InstallRowOrder(g.install)) {
            const setup::CheckItem& item = g.install.items[index];
            const bool to_fix = item.level == setup::CheckLevel::Problem || item.level == setup::CheckLevel::Warning;
            Row row;
            row.id = static_cast<int>(index);
            row.kind = item.action != setup::CheckAction::None && to_fix ? Kind::Link : Kind::Info;
            row.label = item.label;
            row.value = gfx::Clean(item.value);
            row.value_color = item.level == setup::CheckLevel::Problem   ? col::kOff
                              : item.level == setup::CheckLevel::Warning ? col::kWarn
                              : item.level == setup::CheckLevel::Optional ? col::kDim
                                                                          : col::kOn;
            row.note_title = item.note_title;
            row.note_color = item.level == setup::CheckLevel::Problem ? col::kOff : (item.level == setup::CheckLevel::Warning ? col::kWarn : 0);
            row.note = item.note;
            rows.push_back(row);
        }
    }

    void Activate(const Row& row, int step) override {
        if (step != 0 || row.id < 0 || row.id >= static_cast<int>(g.install.items.size())) {
            return;
        }
        const setup::CheckItem& item = g.install.items[static_cast<std::size_t>(row.id)];
        if (item.level != setup::CheckLevel::Problem && item.level != setup::CheckLevel::Warning) {
            return; // (a line that is fine has nothing to do)
        }
        switch (item.action) {
        case setup::CheckAction::RestorePlugin:
            pages::RestorePlugin();
            break;
        case setup::CheckAction::CreatePluginsDir:
            pages::CreatePluginFolder();
            break;
        case setup::CheckAction::FindBoot9:
            pages::StartBoot9Choice();
            break;
        case setup::CheckAction::EnableLoader:
            EnableLoaderNow();
            break;
        case setup::CheckAction::CleanTitles:
            pages::StartTitlesCleanup();
            break;
        case setup::CheckAction::None:
            break;
        }
    }

private:
    // The row under the cursor is the key file and it is missing (a warning of the check): the row whose fix the guide is.
    bool Boot9RowToFix() const {
        const Row* row = Selected();
        if (row == nullptr || row->id < 0 || row->id >= static_cast<int>(g.install.items.size())) {
            return false;
        }
        const setup::CheckItem& item = g.install.items[static_cast<std::size_t>(row->id)];
        return item.action == setup::CheckAction::FindBoot9 && (item.level == setup::CheckLevel::Problem || item.level == setup::CheckLevel::Warning);
    }

    std::function<void(bool)> on_close_;
    bool summary_;
};

} // namespace

namespace pages {

// A on the row "Plugin file": the plugin that is inside 3DS Link goes on the card again, over a file that is not the same (one that is missing is put). Nothing is searched for: the
// plugin is a resource of the app. What came of it is said in a toast, in one honest sentence when the card could not be written.
void RestorePlugin() {
    const setup::PluginEnsure result = g.EnsurePlugin();
    g.RefreshInstall();
    const std::string folder = setup::ParentDir(g.paths.plugin_file);
    const std::string say = setup::PluginInstallToast(result, folder);
    if (!say.empty()) {
        g.plugin_notice.clear(); // (said here, now: not once more on the main page)
        g.ShowToast(say, result.failed() ? col::kOff : col::kOn, result.failed() ? 9000 : 5000);
    } else if (!g.plugin_ready) {
        g.ShowToast(g.PluginLockText(), col::kOff, 9000); // (nothing could be done: this build has no plugin inside)
    } else {
        g.ShowToast("The plugin in " + folder + " is the one of this app.", col::kOn, 4000);
    }
}

void CreatePluginFolder() {
    if (setup::MakeDirs(setup::kPluginsDir)) {
        g.EnsureIniFile(); // (the settings file is made as soon as there is a folder for it)
        g.RefreshInstall();
        g.ShowToast(std::string("Made ") + setup::kPluginsDir, col::kOn, 4000);
    } else {
        g.ShowToast(std::string("Could not make ") + setup::kPluginsDir, col::kOff, 5000);
    }
}

void StartBoot9Choice() {
    PushPage(NewChoicePage("boot9.bin is missing", "How do you want to find it?",
                           {"Search the whole SD card", "Choose a folder", "How to get it"},
                           [](int index) {
                               if (index == 0) {
                                   SearchBoot9();
                               } else if (index == 1) {
                                   ChooseBoot9Folder();
                               } else if (!pages::OpenBoot9Guide()) {
                                   ExplainBoot9(); // (a build without the pictures of the guide tells it in words)
                               }
                           },
                           "boot9.bin is the key file of your console. 3DS Link looks for it next to 3DS Link (" + g.paths.appdir +
                               "), in sdmc:/gm9/out (where GodMode9 dumps it) and where you point to.\n\n"
                               "Search the whole SD card: looks in every folder (it can take a while; B stops).\n"
                               "Choose a folder: browse to the folder that holds the boot9 file.\n"
                               "How to get it: where the file comes from."));
}

void StartTitlesCleanup() {
    const std::vector<setup::UnusableTitle> to_delete = g.titles_health.unusable;
    if (to_delete.empty()) {
        return;
    }
    const int n = static_cast<int>(to_delete.size());
    std::string text = Fmt("Delete %d title%s that cannot be used?", n, n == 1 ? "" : "s");
    const std::size_t shown = std::min<std::size_t>(to_delete.size(), 5);
    for (std::size_t i = 0; i < shown; ++i) {
        text += "\n" + gfx::Clean(setup::UnusableTitleLine(to_delete[i]));
    }
    if (to_delete.size() > shown) {
        text += Fmt("\n- and %d more", static_cast<int>(to_delete.size() - shown));
    }
    text += "\nWhat they have on the card goes (a DLC with contents missing goes whole). Nothing else is touched; the titles are read again afterwards.";
    PushPage(NewConfirmPage("Titles of no use", text, "Yes, delete", "No", [to_delete](bool yes) {
        if (!yes) {
            return;
        }
        if (!titles::Scan().done()) {
            g.ShowToast("The titles are being read: try again in a moment.", col::kWarn, 4000);
            return;
        }
        int failed = 0;
        const int gone = titles::DeleteUnusable(to_delete, &failed);
        g.rescan_wanted = true; // (the list of titles and the check change: the scan runs again and the row is made from it)
        if (failed == 0) {
            g.ShowToast(Fmt("Deleted %d title%s. Reading the titles again...", gone, gone == 1 ? "" : "s"), col::kOn, 5000);
        } else {
            g.ShowToast(Fmt("Deleted %d; the system refused %d. Reading the titles again...", gone, failed), col::kWarn, 7000);
        }
    }));
}

ui::Page* NewInstallCheckPage(std::function<void(bool)> on_close, bool first_start_summary) {
    // The plugin is looked at against the one inside the app and put back when it is not the same (the card is read in full here: the user may have changed the file by
    // hand since the start), and then everything else is looked at again.
    const setup::PluginEnsure result = g.EnsurePlugin();
    const std::string say = setup::PluginInstallToast(result, setup::ParentDir(g.paths.plugin_file));
    g.RefreshInstall(); // what is on the card now
    if (!say.empty()) {
        g.plugin_notice.clear(); // (said here, now: not once more on the main page. A notice from the start that this look had nothing to add to stays for the main page.)
        g.ShowToast(say, result.failed() ? col::kOff : col::kOn, result.failed() ? 9000 : 5000);
    }
    return new InstallCheckPage(std::move(on_close), first_start_summary);
}

} // namespace pages
