#include "status.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "games.hpp"
#include "paths.hpp"
#include "settings.hpp"

namespace setup {

std::string FormatSize(u64 bytes) {
    char buf[32];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(bytes));
    } else if (bytes < 1024ull * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

namespace {

CheckItem MakeItem(const char* label, std::string value, CheckLevel level, const char* note_title, std::string note,
                   CheckAction action = CheckAction::None, const char* short_problem = "") {
    CheckItem item;
    item.label = label;
    item.value = std::move(value);
    item.level = level;
    item.note_title = note_title;
    item.note = std::move(note);
    item.action = action;
    item.short_problem = level == CheckLevel::Problem || level == CheckLevel::Warning ? short_problem : "";
    return item;
}

// "A on Installation check": where the main page sends the user (the Settings page's row of that name).
constexpr const char* kWhereToGo = " - A on Installation check";

std::string Counted(int n, const char* word) {
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

} // namespace

int TitlesHealth::Count(UnusableTitle::Kind kind) const {
    int n = 0;
    for (const UnusableTitle& t : unusable) {
        n += t.kind == kind ? 1 : 0;
    }
    return n;
}

std::string UnusableTitleLine(const UnusableTitle& t) {
    const std::string who = t.name.empty() ? TidText(t.tid) : t.name;
    switch (t.kind) {
    case UnusableTitle::Kind::Unfinished:
        return "- " + who + ": begun, never finished";
    case UnusableTitle::Kind::Broken:
        return "- " + who + ": the system cannot describe it";
    case UnusableTitle::Kind::PartialDlc:
        return "- " + who + " (DLC): " + std::to_string(t.missing) + " of " + std::to_string(t.installed + t.missing) + " contents missing";
    }
    return "- " + who;
}

InstallReport EvaluateInstall(const InstallFacts& facts, const std::string& app_version) {
    InstallReport report;

    // 1. The plugin file of 3DS Link: the master every title's copy (the Enable titles page) is made from. 3DS Link carries the plugin inside and puts it on the card by itself
    // at every start, so this row compares the card with the plugin inside the app, byte for byte (a header that looks good proves nothing), and A puts it back from the app.
    {
        const std::string path = facts.plugin_path;
        const std::string tried = facts.plugin_tried && facts.plugin_write != WriteResult::Ok
                                      ? std::string("\n3DS Link tried to put it there and could not: ") + WriteFailurePhrase(facts.plugin_write) + "."
                                      : std::string();
        const std::string version = PluginVersionText(facts.plugin_version);
        if (!facts.embedded_ok) {
            report.items.push_back(MakeItem("Plugin file", "none in this app", CheckLevel::Problem, "Plugin file",
                                            path + "\nThis copy of 3DS Link was built without the plugin inside, so it cannot put one on the card and 3DS Link cannot be "
                                                   "switched on. Install the 3DS Link CIA again.",
                                            CheckAction::None, "no plugin in the app"));
        } else {
            switch (facts.plugin_card) {
            case PluginCard::Missing:
                report.items.push_back(MakeItem("Plugin file", "missing", CheckLevel::Problem, "Plugin file",
                                                path + "\nNot there. 3DS Link carries its plugin inside and puts it here by itself at every start." + tried +
                                                    "\nA: put it there now.",
                                                CheckAction::RestorePlugin, "plugin file missing"));
                break;
            case PluginCard::Unreadable:
            case PluginCard::NotPlugin:
                report.items.push_back(MakeItem("Plugin file", facts.plugin_card == PluginCard::Unreadable ? "unreadable" : "not a plugin", CheckLevel::Problem,
                                                "Plugin file",
                                                path + "\nThe file (" + FormatSize(facts.plugin_size) +
                                                    (facts.plugin_card == PluginCard::Unreadable ? ") could not be read." : ") does not start like a .3gx plugin.") + tried +
                                                    "\nA: replace it with the plugin inside 3DS Link.",
                                                CheckAction::RestorePlugin, "plugin file damaged"));
                break;
            case PluginCard::OtherVersion:
                report.items.push_back(MakeItem("Plugin file", "v" + version + " (app v" + app_version + ")", CheckLevel::Problem, "Plugin file",
                                                path + "\nThis file is version " + version + " but this app is " + app_version +
                                                    ": the app and its plugin are one version." + tried + "\nA: replace it with the plugin inside 3DS Link.",
                                                CheckAction::RestorePlugin, "plugin file is another version"));
                break;
            case PluginCard::Altered:
                report.items.push_back(MakeItem("Plugin file", "damaged", CheckLevel::Problem, "Plugin file",
                                                path + "\nThe file (" + std::to_string(facts.plugin_size) + " bytes) says version " + version +
                                                    " but it is not, byte for byte, the plugin inside this app: it is cut short, damaged, or from another build." + tried +
                                                    "\nA: replace it with the plugin inside 3DS Link.",
                                                CheckAction::RestorePlugin, "plugin file damaged"));
                break;
            case PluginCard::Same:
                report.items.push_back(MakeItem("Plugin file", "v" + version + ", " + FormatSize(facts.plugin_size), CheckLevel::Ok, "Plugin file",
                                                path + "\nVersion " + version + ", " + std::to_string(facts.plugin_size) +
                                                    " bytes: byte for byte the plugin inside this app."));
                break;
            }
        }
    }

    // 2. The folder Luma3DS loads plugins from (the settings file the plugin reads lives there too: the app makes it by itself).
    if (facts.plugins_dir) {
        report.items.push_back(MakeItem("Plugin folder", "found", CheckLevel::Ok, "Plugin folder",
                                        std::string(kPluginsDir) + "\nLuma3DS loads plugins from here."));
    } else {
        report.items.push_back(MakeItem("Plugin folder", "not found", CheckLevel::Problem, "Plugin folder",
                                        std::string(kPluginsDir) +
                                            "\nNot found. Luma3DS loads plugins from this folder, and 3DS Link puts its files there. "
                                            "A: make it now.",
                                        CheckAction::CreatePluginsDir, "plugin folder missing"));
    }

    // 3. Can the card be written (every copy, the settings and the caches need it)?
    if (facts.sd_writable) {
        report.items.push_back(MakeItem("SD card", "writable", CheckLevel::Ok, "SD card",
                                        "A small test file could be written and removed."));
    } else {
        report.items.push_back(MakeItem("SD card", "read-only", CheckLevel::Problem, "SD card",
                                        "A test file could not be written. Check the lock switch of the SD card and its free space.",
                                        CheckAction::None, "SD card read-only"));
    }

    // 4. The key file: only sending titles needs it (and, on some consoles, reading the names and icons of the titles), so a missing one is a
    // warning: 3DS Link works, titles cannot be sent.
    if (facts.boot9) {
        report.items.push_back(MakeItem("boot9.bin", ParentDir(facts.boot9_path), CheckLevel::Ok, "boot9.bin",
                                        facts.boot9_path + "\nFound: 3DS Link uses it from there (it is not copied). It lets 3DS Link send titles "
                                                           "and read the names and icons of your titles where the system does not."));
    } else {
        const std::string beside = facts.app_dir.empty() ? std::string("next to 3DS Link") : "next to 3DS Link (" + facts.app_dir + ")";
        report.items.push_back(MakeItem("boot9.bin", "missing", CheckLevel::Warning, "boot9.bin is missing",
                                        "The key file of your console. Titles Delivery needs it to send titles (receiving works without it), "
                                        "3DS Link Delivery needs it to share 3DS Link, and 3DS Link uses it to read the names and icons of your titles. "
                                        "3DS Link itself works without it.\n\n"
                                        "Not found " + beside + ", in sdmc:/gm9/out/ (GodMode9's folder) or in a folder you chose: a file called boot9.bin, or with boot9 in its "
                                        "name (GodMode9 2.x makes <serial>_boot9_00.bin), of 64 KB or 32 KB.\n\n"
                                        "A: search the SD card, choose a folder, or see how to get it. Y: the guide, step by step.",
                                        CheckAction::FindBoot9, "boot9.bin missing"));
    }

    // 4b. The database of the seeds: it is read from the console's own memory (it is not extracted, and boot9.bin is not needed for it), but it has to be
    // readable: the titles that were downloaded with a seed cannot be sent without it. Left out when nobody asked.
    if (facts.seed == 1) {
        report.items.push_back(MakeItem("Seed database", FormatSize(facts.seed_size), CheckLevel::Ok, "Seed database",
                                        "The seeds of the titles downloaded from the eShop, read from the console's own memory: " + FormatSize(facts.seed_size) +
                                            ". Titles Delivery needs them to send those titles."));
    } else if (facts.seed == 2) {
        char code[24];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(facts.seed_error));
        report.items.push_back(MakeItem("Seed database", "not readable", CheckLevel::Warning, "Seed database is not readable",
                                        std::string("The seeds of the titles downloaded from the eShop are read from the console's own memory, and the system did "
                                                    "not let 3DS Link read them (") + code + "). Receiving works; titles that need a seed cannot be sent.",
                                        CheckAction::None, "seed database not readable"));
    }

    // 4c. Rosalina's Plugin Loader: with it off no title loads the plugin, whatever else is right.
    if (facts.loader == LoaderState::Enabled) {
        report.items.push_back(MakeItem("Plugin Loader", "enabled", CheckLevel::Ok, "Plugin Loader",
                                        "Rosalina's Plugin Loader is on: titles load the plugins of sdmc:/luma/plugins."));
    } else if (facts.loader == LoaderState::Disabled) {
        report.items.push_back(MakeItem("Plugin Loader", "disabled", CheckLevel::Problem, "Plugin Loader is disabled",
                                        "Rosalina's Plugin Loader is switched off, so no title can load the 3DS Link plugin. A: switch it on (the same switch "
                                        "as Rosalina's menu: L+Down+Select, Plugin Loader).",
                                        CheckAction::EnableLoader, "Plugin Loader off"));
    }

    // 4d. The version of Luma3DS, in the colours of the check: red "not work" (no Plugin Loader), yellow "maybe" (it has one, never tried), green "tested".
    {
        const std::string version = LumaVersionText(facts.luma);
        switch (ClassifyLuma(facts.luma, facts.loader)) {
        case LumaSupport::NotWork:
            report.items.push_back(MakeItem("Luma3DS", version + " not work", CheckLevel::Problem, "Luma3DS is too old",
                                            "This console runs Luma3DS " + version + ". The Plugin Loader, which games need to load the 3DS Link plugin, exists from Luma3DS 13.0, so on this "
                                            "version the room link of the plugin cannot work. Titles Delivery does not need it and works. To use the plugin, update Luma3DS to 13.3 or newer "
                                            "(3DS Link does not do it for you).",
                                            CheckAction::None, "Luma too old"));
            break;
        case LumaSupport::Maybe:
            report.items.push_back(MakeItem("Luma3DS", version + " maybe", CheckLevel::Warning, "Luma3DS: not tried",
                                            "This console runs Luma3DS " + version + ". It has a Plugin Loader, so the plugin should load, but 3DS Link was only tried on Luma3DS 13.3.3 "
                                            "and newer. If the room link does not work, update Luma3DS to 13.3 or newer.",
                                            CheckAction::None, "Luma not tried"));
            break;
        case LumaSupport::Tested:
            report.items.push_back(MakeItem("Luma3DS", version + " tested", CheckLevel::Ok, "Luma3DS",
                                            "This console runs Luma3DS " + version + ": 3DS Link was tried on Luma3DS 13.3.3, and 13.3 and newer should work the same."));
            break;
        case LumaSupport::Unknown:
            break;
        }
    }

    // 4e. The titles on the SD card that are of no use: an installation that was begun and never finished, a title the system cannot describe, a DLC
    // with contents missing. The scan of the titles finds them (no row until it has run). They are not a fault of the installation of 3DS Link, but
    // they are what a receiving that stopped half way leaves on an older app, every start reads them with the others, and one of them can make the
    // system refuse the versions of all the titles (then every icon is read again at every start): a warning, and A deletes them.
    if (facts.titles.checked) {
        const TitlesHealth& t = facts.titles;
        const std::string listed = "The system lists " + Counted(t.titles, "title") + " on the SD card (games, updates, DLC and system titles).";
        if (t.unusable.empty()) {
            report.items.push_back(MakeItem("Installed titles", Counted(t.titles, "title") + ", all usable", CheckLevel::Ok, "Installed titles",
                                            listed + " None is half installed or unreadable."));
        } else {
            std::string lines;
            const std::size_t shown = std::min<std::size_t>(t.unusable.size(), 8);
            for (std::size_t i = 0; i < shown; ++i) {
                lines += "\n" + UnusableTitleLine(t.unusable[i]);
            }
            if (t.unusable.size() > shown) {
                lines += "\n- and " + std::to_string(t.unusable.size() - shown) + " more";
            }
            report.items.push_back(MakeItem("Installed titles", Counted(static_cast<int>(t.unusable.size()), "unusable title"), CheckLevel::Warning, "Titles of no use",
                                            listed + " These cannot be used:" + lines +
                                                "\n\nAn installation that stopped half way (a receiving that was cancelled, a power cut) leaves this. They are read at every "
                                                "start with the others, and one of them can make the system refuse the versions of every title, which makes each start "
                                                "slow. A: delete them (what they have on the card and their tickets go; nothing else is touched). A DLC with contents "
                                                "missing still works for what is there: delete it only if you can receive it again.",
                                            CheckAction::CleanTitles, "unusable titles"));
        }
    }

    // 5. Luma3DS's default plugin: information only (3DS Link never uses it).
    if (facts.default_plugin) {
        report.items.push_back(MakeItem("default.3gx", "not used", CheckLevel::Optional, "default.3gx",
                                        std::string(kDefaultPluginPath) +
                                            "\ndefault.3gx would apply to every title; 3DS Link does not use it."));
    }

    for (const CheckItem& item : report.items) {
        if (item.level == CheckLevel::Problem) {
            ++report.problems;
        } else if (item.level == CheckLevel::Warning) {
            ++report.warnings;
        }
    }
    return report;
}

int SeverityRank(CheckLevel level) {
    switch (level) {
    case CheckLevel::Problem:
        return 0;
    case CheckLevel::Warning:
        return 1;
    case CheckLevel::Ok:
        return 2;
    case CheckLevel::Optional:
        return 3;
    }
    return 3;
}

std::vector<std::size_t> InstallRowOrder(const InstallReport& report) {
    std::vector<std::size_t> order(report.items.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&report](std::size_t a, std::size_t b) {
        return SeverityRank(report.items[a].level) < SeverityRank(report.items[b].level);
    });
    return order;
}

std::string InstallSummary(const InstallReport& report) {
    if (report.problems == 0 && report.warnings == 0) {
        return "All OK";
    }
    std::string text;
    if (report.problems > 0) {
        text = Counted(report.problems, "problem");
    }
    if (report.warnings > 0) {
        text += (text.empty() ? "" : ", ") + Counted(report.warnings, "warning");
    }
    return text;
}

InstallLine InstallLineText(const InstallReport& report) {
    InstallLine line;
    if (report.problems == 0 && report.warnings == 0) {
        line.full = line.compact = "All OK";
        return line;
    }
    // Problems first, then warnings, each group in the order of the check.
    for (const std::size_t i : InstallRowOrder(report)) {
        const CheckItem& item = report.items[i];
        if (item.level == CheckLevel::Problem || item.level == CheckLevel::Warning) {
            line.full += (line.full.empty() ? "" : ", ") + item.short_problem;
        }
    }
    line.full += kWhereToGo;
    line.compact = InstallSummary(report) + kWhereToGo;
    return line;
}

bool ProbeWritable(const char* dir) {
    mkdir(dir, 0777); // (it may be there already)
    const std::string path = std::string(dir) + "/.3dslink_write_test";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = std::fwrite("x", 1, 1, f) == 1;
    ok = std::fclose(f) == 0 && ok;
    std::remove(path.c_str());
    return ok;
}

std::string LumaVersionText(u32 version_word) {
    if (version_word == 0) {
        return std::string();
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", static_cast<unsigned>((version_word >> 24) & 0xFF), static_cast<unsigned>((version_word >> 16) & 0xFF),
                  static_cast<unsigned>((version_word >> 8) & 0xFF));
    return buf;
}

LumaSupport ClassifyLuma(u32 version_word, LoaderState loader) {
    if (version_word == 0) {
        return LumaSupport::Unknown;
    }
    const unsigned major = (version_word >> 24) & 0xFF;
    const unsigned minor = (version_word >> 16) & 0xFF;
    if (major < 13) {
        return loader != LoaderState::Unknown ? LumaSupport::Maybe : LumaSupport::NotWork;
    }
    return (major == 13 && minor < 3) ? LumaSupport::Maybe : LumaSupport::Tested;
}

const char* LoaderStateText(LoaderState state) {
    return state == LoaderState::Enabled ? "Enabled" : state == LoaderState::Disabled ? "Disabled" : "unknown";
}

bool ShouldOfferLoader(LoaderState state, bool link_on) {
    return state == LoaderState::Disabled && link_on;
}

std::string LoaderEnableMessage(u32 set_result, LoaderState after, bool* good) {
    char buf[96];
    bool ok = false;
    if (set_result != 0) {
        std::snprintf(buf, sizeof(buf), "Could not enable the Plugin Loader (0x%08lX).", static_cast<unsigned long>(set_result));
    } else if (after == LoaderState::Enabled) {
        std::snprintf(buf, sizeof(buf), "Plugin Loader enabled.");
        ok = true;
    } else {
        std::snprintf(buf, sizeof(buf), "The Plugin Loader is still not enabled.");
    }
    if (good) {
        *good = ok;
    }
    return buf;
}

std::string ScanProgressText(const ScanProgress& p) {
    const int total = p.total < 0 ? 0 : p.total;
    const int done = p.done < 0 ? 0 : (p.done > total ? total : p.done);
    char buf[64];
    switch (p.phase) {
    case 0:
        std::snprintf(buf, sizeof(buf), "names: %d of %d titles", done, total);
        break;
    case 1:
        std::snprintf(buf, sizeof(buf), "games: %d of %d", done, total);
        break;
    case 2:
        std::snprintf(buf, sizeof(buf), "checking the titles");
        break;
    default:
        std::snprintf(buf, sizeof(buf), "%d titles, %d games", p.titles < 0 ? 0 : p.titles, p.games < 0 ? 0 : p.games);
        break;
    }
    return buf;
}

float ProgressFraction(int done, int total) {
    if (total <= 0) {
        return 0.0f;
    }
    const float f = static_cast<float>(done) / static_cast<float>(total);
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

} // namespace setup
