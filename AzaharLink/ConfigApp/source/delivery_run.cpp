// Titles Delivery: the screens that are up while the module is sending or receiving. The module runs its own loops and calls the app back from
// inside them (4 times a second while sending, about 6.5 while receiving, and as often as it can while the receiver chooses): every call reads
// the keys, updates a page and draws one frame of it. A transfer never waits for the screen (the radio loop must not stand still): a frame
// that cannot be started because the last one is still being drawn is skipped.
#include <algorithm>
#include <cstring>

#include "delivery.hpp"
#include "delivery_eta.hpp"
#include "pages.hpp"

using namespace ui;

namespace delivery {

namespace {

// ---------------------------------------------------------------------------------------------
// Pieces
// ---------------------------------------------------------------------------------------------

// A bar with two parts: what is there (green) and what is added to it (red); the rest is free.
void DrawSpaceBar(float x, float y, float w, float h, double used, double added) {
    gfx::FillRect(x, y, w, h, col::kTrack);
    const float used_w = static_cast<float>(Clamp(used, 0.0, 1.0)) * w;
    const float add_w = static_cast<float>(Clamp(used + added, 0.0, 1.0)) * w - used_w;
    gfx::FillRect(x, y, used_w, h, col::kOn);
    gfx::FillRect(x + used_w, y, add_w, h, col::kOff);
    gfx::StrokeRect(x, y, w, h, 1.0f, col::kBorder);
}

std::string Percent(int pct) {
    return Fmt("%d%%", Clamp(pct, 0, 100));
}

// How much of a title a percentage of the module means (it rounds down: 99 can be 99.9, and a title that is done reads 100).
double Fraction(int pct) {
    return pct >= 100 ? 1.0 : (pct <= 0 ? 0.0 : (static_cast<double>(pct) + 0.5) / 100.0);
}

static_assert(DL_MAX_CLIENTS <= kEtaRecv && DL_MAX_TITLES <= kEtaTitles, "the estimate holds as many receivers and titles as the room does");
constexpr int kOtherRows = kRoomReceivers - 1; // the other receivers the receiving screen lists (this console is one of the room's receivers)
constexpr double kInstallBacklog = 16.0 * kMiB; // an installer is only measured while this much is waiting for it (else it just keeps up with the radio)

// ---------------------------------------------------------------------------------------------
// Small pieces of these screens
// ---------------------------------------------------------------------------------------------

// What a receiver is doing and the colour of that: downloading blue, installing and waiting for its turn yellow, finished green (orange when a title failed),
// silent red, just joined grey.
std::string ReceiverStateWord(int state, unsigned age_ds) {
    switch (state) {
    case DL_C_DOWNLOADING:
        return "downloading";
    case DL_C_INSTALLING:
        return "installing";
    case DL_C_FINISHED:
        return "finished";
    case DL_C_ERROR:
        return "had errors";
    case DL_C_WAITING:
        return "waiting";
    case DL_C_SILENT:
        return age_ds > 0 ? Fmt("silent %us", age_ds / 10) : std::string("silent");
    case DL_C_JOINED:
        return "just joined";
    default:
        return std::string();
    }
}

u32 ReceiverStateColor(int state) {
    switch (state) {
    case DL_C_DOWNLOADING:
        return col::kAccent;
    case DL_C_INSTALLING:
    case DL_C_WAITING:
        return col::kWarn;
    case DL_C_FINISHED:
        return col::kOn;
    case DL_C_ERROR:
        return col::kAlert;
    case DL_C_SILENT:
        return col::kOff;
    default:
        return col::kDim;
    }
}

// A time for a single receiver: a long one is only a guess (the sender serves one title at a time, others join), so beyond two hours it says so.
std::string CappedDuration(double seconds) {
    return seconds > 7200.0 ? std::string("over 2 h") : ShortDurationText(seconds);
}

// The name of a receiver's console (its user name on the wireless), or "Receiver N" when it is not known.
std::string ReceiverName(const dl_send_stats& s, int i) {
    const char* name = s.client[i].name;
    return name[0] ? gfx::Clean(name) : Fmt("Receiver %d", i + 1);
}

// ---------------------------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------------------------

class SendRunPage : public Page {
public:
    void Set(const dl_send_stats& stats) {
        s_ = stats;
        if (sel_ >= s_.n_clients) {
            sel_ = s_.n_clients > 0 ? s_.n_clients - 1 : 0;
        }
        ScrollToSel();
        if (n_titles_ == 0 && dl_send_title_count() > 0) {
            n_titles_ = dl_send_title_count() < DL_MAX_TITLES ? dl_send_title_count() : DL_MAX_TITLES;
            for (int i = 0; i < n_titles_; ++i) {
                dl_send_title(i, &titles_[i]);
            }
        }
        const u64 now = NowMs();
        const double dt = tick_ms_ != 0 ? static_cast<double>(now - tick_ms_) / 1000.0 : 0.0;
        tick_ms_ = now;
        SampleSpeeds();
        Estimate(dt);
    }
    bool close_requested() const {
        return close_;
    }

    void Update(const Input& in) override {
        const FooterRects r = LayoutFooter(MakeFooter());
        if ((in.down & KEY_SELECT) || Tapped(in, r.extra[0])) {
            dl_toggle_screens();
        }
        if (confirming_) {
            // B asks first: closing the room cuts off whoever is still receiving. A closes it, B leaves it open.
            const PopupBox box = ConfirmBox();
            if ((in.down & KEY_A) || Tapped(in, box.yes)) {
                close_ = true;
            } else if ((in.down & KEY_B) || Tapped(in, box.no)) {
                confirming_ = false;
            }
            return;
        }
        // Up and down (or a tap) choose the receiver whose details the top screen shows.
        // (every press counts: the screen is looked at four times a second, and two presses in one look are two steps)
        if (s_.n_clients > 0) {
            sel_ = Clamp(sel_ + in.down_presses - in.up_presses, 0, s_.n_clients - 1);
        }
        for (int i = first_; i < s_.n_clients && i < first_ + kShownRows; ++i) {
            if (Tapped(in, RowRect(i))) {
                sel_ = i;
            }
        }
        ScrollToSel();
        if ((in.down & KEY_B) || Tapped(in, r.back)) {
            confirming_ = true;
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, "Send Titles", s_.channel);
        const char* state = s_.phase == DL_S_OPEN ? "room open" : s_.phase == DL_S_SENDING ? "sending" : s_.phase == DL_S_CLOSED ? "room closed" : "error";

        // What is offered, a game with its update and DLC on one line. Only three lines fit: the three that show are always the ones around the title
        // that is being sent (the green dot), so that it is never out of sight.
        struct Group {
            std::string name;
            unsigned tags = 0;
            u64 bytes = 0;
            bool current = false;
        };
        std::vector<Group> groups;
        for (int i = 0; i < n_titles_; ++i) {
            const dl_title& t = titles_[i];
            if (groups.empty() || groups.back().name != gfx::Clean(t.name)) {
                Group group;
                group.name = gfx::Clean(t.name);
                groups.push_back(group);
            }
            Group& group = groups.back();
            group.tags |= KindTag(t.kind);
            group.bytes += t.size;
            group.current = group.current || i == s_.cur_title;
        }
        constexpr int kShownGroups = 3;
        const int total_groups = static_cast<int>(groups.size());
        const int shown = total_groups < kShownGroups ? total_groups : kShownGroups;
        int first = 0;
        if (total_groups > kShownGroups) {
            int at = 0;
            for (int i = 0; i < total_groups; ++i) {
                at = groups[static_cast<std::size_t>(i)].current ? i : at;
            }
            first = Clamp(at - 1, 0, total_groups - kShownGroups);
        }
        const float h1 = 24.0f + 13.0f * static_cast<float>(shown) + 16.0f;
        DrawCard(10, 30, 380, h1);
        gfx::FillRect(10, 30, 3, h1, col::kAccent);
        gfx::DrawText(22, 42, 0.5f, col::kAccent, std::string("Offered  ") + state);
        DrawKindLegend(382.0f, 42.0f);
        for (int i = 0; i < shown; ++i) {
            const Group& group = groups[static_cast<std::size_t>(first + i)];
            const float yc = 56.0f + 13.0f * static_cast<float>(i);
            if (group.current) {
                gfx::FillCircle(24, yc, 3.0f, col::kOn);
            }
            gfx::DrawText(32, yc, 0.4f, group.current ? col::kText : col::kDim, gfx::Ellipsize(group.name, 0.4f, 250));
            gfx::DrawText(382, yc, 0.38f, col::kDim, SizeText(group.bytes), gfx::Align::Right);
            DrawKindMarks(382.0f - gfx::TextWidth(SizeText(group.bytes), 0.38f) - 8.0f, yc, group.tags);
        }
        const u64 total = dl_send_total_bytes();
        const int count = dl_send_title_count();
        const float sy = 56.0f + 13.0f * static_cast<float>(shown) + 3.0f;
        if (total_groups > kShownGroups) {
            gfx::DrawText(32, sy, 0.38f, col::kDim, Fmt("and %d more", total_groups - kShownGroups));
        }
        gfx::DrawText(382, sy, 0.4f, col::kText,
                      Fmt("%d %s, %s", count, count == 1 ? "title" : "titles", SizeText(total).c_str()) + "  " + ShortDurationText(EstimateSeconds(total)),
                      gfx::Align::Right);

        // How far the room is and what is left of it (delivery_eta.hpp): the share done, the time that is left to serve everyone who is in, and
        // the speed the radio sends at. With nobody in yet, the time that one receiver would take for all the titles.
        const float y2 = 30.0f + h1 + 3.0f;
        DrawCard(10, y2, 380, 40);
        std::string left, right;
        float filled = 0.0f;
        switch (eta_.kind) {
        case EtaKind::Busy:
            left = "Room " + Percent(pct_);
            right = ShortDurationText(eta_.seconds) + " left";
            filled = static_cast<float>(pct_) / 100.0f;
            break;
        case EtaKind::Done:
            left = "All receivers finished";
            filled = 1.0f;
            break;
        case EtaKind::Silent:
            left = "Receivers not answering";
            break;
        case EtaKind::Nobody:
            left = "Waiting for receivers";
            right = "for all titles: " + ShortDurationText(eta_.seconds);
            break;
        }
        gfx::DrawText(22, y2 + 10, 0.5f, col::kText, left);
        gfx::DrawText(382, y2 + 10, 0.44f, col::kText, right, gfx::Align::Right);
        if (eta_.kind == EtaKind::Busy) {
            const float x = 22.0f + gfx::TextWidth(left, 0.5f) + 12.0f;
            const float avail = 382.0f - gfx::TextWidth(right, 0.44f) - 10.0f - x;
            if (avail > 60.0f) {
                gfx::DrawFitted(x, y2 + 10, 0.4f, 0.3f, avail, col::kDim, Fmt("sending speed: %.2f MiB/s", s_.mibps));
            }
        }
        DrawProgressBar(22, y2 + 19, 356, 7, filled);
        // The packets that had to be sent again, for the whole room (not for the receiver chosen below).
        const double sent_all = static_cast<double>(s_.next_blocks) + static_cast<double>(s_.repairs);
        gfx::DrawFitted(22, y2 + 33, 0.34f, 0.28f, 356, col::kDim,
                        Fmt("repeated packets, all receivers: %llu", static_cast<unsigned long long>(s_.repairs)) +
                            (sent_all > 0.0 ? Fmt(" (%.1f%% of all sent)", 100.0 * static_cast<double>(s_.repairs) / sent_all) : std::string()));

        // The receiver under the cursor of the bottom screen: who it is, where it is, what it downloads and installs now and how fast.
        const float y3 = y2 + 43.0f;
        const float h3 = 234.0f - y3;
        DrawCard(10, y3, 380, h3);
        if (s_.n_clients > 0 && sel_ < s_.n_clients) {
            const auto& c = s_.client[sel_];
            gfx::FillRect(10, y3, 3, h3, ReceiverStateColor(c.state));
            gfx::FillCircle(24, y3 + 11, 4.0f, ReceiverStateColor(c.state));
            gfx::DrawFitted(34, y3 + 11, 0.52f, 0.4f, 230, col::kText, ReceiverName(s_, sel_));
            gfx::DrawText(382, y3 + 11, 0.44f, ReceiverStateColor(c.state), ReceiverStateWord(c.state, c.age_ds), gfx::Align::Right);
            float x = DrawPair(22, y3 + 25, 0.42f, "Downloaded", Percent(c.down), col::kAccent);
            DrawPair(x, y3 + 25, 0.42f, "Installed", Percent(c.inst), col::kOn);
            // When this receiver will be done, and how long it waits before the first title it needs starts to come (the sender serves one title
            // at a time, and the one that most receivers need comes first).
            const EtaReceiverOut& mine = eta_.recv[sel_];
            if (mine.seconds >= 0.0 && c.state != DL_C_FINISHED && c.state != DL_C_ERROR) {
                if (mine.wait >= 60.0 && (c.state == DL_C_WAITING || c.state == DL_C_JOINED)) {
                    // Its first title has not started: the time that is left would mean nothing yet, so the screen says when the download begins
                    // (only for a receiver that is waiting: one that is downloading has started, whatever the estimate of the order says).
                    gfx::DrawText(382, y3 + 25, 0.42f, col::kText, "download starts in " + (mine.wait > 7200.0 ? std::string("more than 2 h") : ShortDurationText(mine.wait)),
                                  gfx::Align::Right);
                } else {
                    gfx::DrawText(382, y3 + 25, 0.42f, col::kText, CappedDuration(mine.seconds) + " left", gfx::Align::Right);
                }
            }
            // The numbers of this receiver, complete and without the names of the titles (there can be twenty): how fast it downloads and installs,
            // how many of the titles it asked for are installed, the space it has on its card and when it last answered.
            const Speed& sp = speed_[sel_];
            x = DrawPair(22, y3 + 39, 0.4f, "downloading at", Fmt("%.2f MiB/s", sp.down_mibps), col::kAccent);
            DrawPair(x, y3 + 39, 0.4f, "installing at", Fmt("%.2f MiB/s", sp.inst_mibps), col::kOn);
            int asked = 0, installed = 0, got_all = 0;
            for (int t = 0; t < n_titles_; ++t) {
                if (dl_bit(c.want, t)) {
                    ++asked;
                    installed += c.t_inst[t] >= 100 ? 1 : 0;
                    got_all += c.t_down[t] >= 100 ? 1 : 0;
                }
            }
            x = DrawPair(22, y3 + 53, 0.4f, "titles asked for", Fmt("%d", asked));
            x = DrawPair(x, y3 + 53, 0.4f, "downloaded", Fmt("%d", got_all), col::kAccent);
            x = DrawPair(x, y3 + 53, 0.4f, "installed", Fmt("%d", installed), col::kOn);
            DrawPair(x, y3 + 53, 0.4f, "to go", Fmt("%d", asked - installed), asked - installed > 0 ? col::kWarn : col::kText);
            x = DrawPair(22, y3 + 67, 0.4f, "SD free", c.free_mib != DL_FREE_UNKNOWN ? SizeText(static_cast<u64>(c.free_mib) << 20) : std::string("unknown"));
            DrawPair(x, y3 + 67, 0.4f, "last answer", Fmt("%.1f s ago", static_cast<double>(c.age_ds) / 10.0), c.state == DL_C_SILENT ? col::kOff : col::kText);
        } else {
            gfx::FillRect(10, y3, 3, h3, col::kTrack);
            gfx::DrawText(22, y3 + 13, 0.46f, col::kDim, "Nobody has joined yet");
            DrawPair(22, y3 + 28, 0.4f, "consoles in the room", Fmt("%d", s_.connected));
            if (s_.msg[0]) {
                gfx::DrawText(22, y3 + 42, 0.4f, s_.phase == DL_S_ERROR ? col::kOff : col::kWarn, gfx::Ellipsize(s_.msg, 0.4f, 350));
            }
        }
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        DrawTitleBar(gfx::kBottomW, "Receivers",
                     s_.n_clients > kRoomReceivers ? Fmt("%d", s_.n_clients) : (s_.n_clients > 0 ? Fmt("%d / %d", s_.n_clients, kRoomReceivers) : std::string()));
        if (s_.n_clients == 0) {
            DrawCard(8, 34, 304, 168);
            gfx::DrawText(160, 76, 0.55f, col::kText, "Nobody has joined yet", gfx::Align::Center);
            gfx::DrawWrapped(22, 96, 276, 0.45f, col::kDim,
                             "Receivers join from Titles Delivery > Receive Titles on their console, whenever they want, and take what they choose.", 5);
        } else {
            // One block of two lines for each receiver (five show; with more the list moves a row at a time, with no animation, and a thin bar on the right says where):
            // its name and what it does, then how far it has downloaded and installed, each with the speed and its unit.
            gfx::DrawText(30, 32, 0.4f, col::kDim, "Downloaded");
            gfx::DrawText(30 + kHalf, 32, 0.4f, col::kDim, "Installed");
            const int shown = std::min(s_.n_clients - first_, kShownRows);
            for (int k = 0; k < shown; ++k) {
                const int i = first_ + k;
                const Rect r = RowRect(i);
                const auto& c = s_.client[i];
                const u32 color = ReceiverStateColor(c.state);
                gfx::FillRect(r.x, r.y, r.w, r.h - 2.0f, i == sel_ ? col::kSel : col::kPanel);
                if (i == sel_) {
                    gfx::FillRect(r.x, r.y, 3, r.h - 2.0f, col::kAccent);
                }
                gfx::FillCircle(19, r.y + 10.0f, 4.0f, color);
                gfx::DrawFitted(30, r.y + 10.0f, 0.5f, 0.38f, 168, col::kText, ReceiverName(s_, i));
                gfx::DrawText(r.x + r.w - 6.0f, r.y + 10.0f, 0.44f, color, ReceiverStateWord(c.state, c.age_ds), gfx::Align::Right);
                DrawRate(30, r.y + 24.0f, c.down, speed_[i].down_mibps);
                DrawRate(30 + kHalf, r.y + 24.0f, c.inst, speed_[i].inst_mibps);
            }
            if (s_.n_clients > kShownRows) {
                const float track_y = kRowTop, track_h = kRowPitch * static_cast<float>(kShownRows) - 2.0f;
                const float thumb_h = track_h * static_cast<float>(kShownRows) / static_cast<float>(s_.n_clients);
                const float thumb_y = track_y + (track_h - thumb_h) * static_cast<float>(first_) / static_cast<float>(s_.n_clients - kShownRows);
                gfx::FillRect(313, track_y, 3, track_h, col::kTrack);
                gfx::FillRect(313, thumb_y, 3, thumb_h, col::kDim);
            }
        }
        DrawFooterBackground();
        DrawFooter(MakeFooter());
        DrawFrameBorder(gfx::kBottomW);
        if (confirming_) {
            const PopupBox box = ConfirmBox();
            DrawDim(gfx::kBottomW);
            DrawPopupBox(box, "Close the room?", kCloseText);
            DrawButton(box.yes, "A", "Yes, close it", true, PressedNow(g.input.in(), box.yes));
            DrawButton(box.no, "B", "No", true, PressedNow(g.input.in(), box.no));
            DrawFrameBorder(gfx::kBottomW);
        }
    }

private:
    struct Speed {
        double down_mibps = 0.0, inst_mibps = 0.0;
        u64 at_ms = 0, down_bytes = 0, inst_bytes = 0;
        bool have = false;
        RatioFilter install{90.0}; // how fast its installer takes what it has downloaded, while there is a backlog (for the time left)
        std::string who; // the receiver this slot was measuring (its name and what it asked for): another one in the same place starts again
    };

    static constexpr const char* kCloseText = "Receivers that have not finished will stop receiving.";
    static PopupBox ConfirmBox() {
        return LayoutPopup(kCloseText, true);
    }
    // The bottom screen shows five receivers at a time (the room holds seven): `first_` is the one at the top of the list.
    static constexpr int kShownRows = 5;
    static constexpr float kRowTop = 38.0f, kRowPitch = 34.0f, kHalf = 136.0f;
    Rect RowRect(int i) const {
        return Rect{8.0f, kRowTop + kRowPitch * static_cast<float>(i - first_), 302.0f, kRowPitch};
    }
    // The receiver under the cursor stays on the list: the list moves by whole rows when the cursor leaves it.
    void ScrollToSel() {
        if (sel_ < first_) {
            first_ = sel_;
        }
        if (sel_ >= first_ + kShownRows) {
            first_ = sel_ - kShownRows + 1;
        }
        first_ = Clamp(first_, 0, s_.n_clients > kShownRows ? s_.n_clients - kShownRows : 0);
    }
    // A bar with its percentage inside it and, to the right, how fast it moves.
    static void DrawRate(float x, float yc, int pct, double mibps) {
        DrawProgressBar(x, yc - 6.5f, 56, 13, static_cast<float>(pct) / 100.0f);
        gfx::DrawText(x + 28.0f, yc, 0.4f, col::kText, Percent(pct), gfx::Align::Center);
        gfx::DrawText(x + 62.0f, yc, 0.4f, col::kText, Fmt("%.2f MiB/s", mibps));
    }

    // How fast each receiver downloads and installs, from what it had a moment ago (every 1.5 s at least, smoothed).
    void SampleSpeeds() {
        const u64 now = NowMs();
        for (int i = 0; i < s_.n_clients && i < DL_MAX_CLIENTS; ++i) {
            Speed& sp = speed_[i];
            const auto& c = s_.client[i];
            const std::string who = ReceiverName(s_, i) + Fmt("/%02x%02x", c.want[0], c.want[1]);
            if (sp.who != who) {
                sp = Speed();
                sp.who = who;
            }
            if (sp.at_ms == 0) {
                sp.at_ms = now;
                sp.down_bytes = c.down_bytes;
                sp.inst_bytes = c.inst_bytes;
                continue;
            }
            if (now - sp.at_ms < 1500) {
                continue;
            }
            const double dt = static_cast<double>(now - sp.at_ms) / 1000.0;
            const double d = c.down_bytes >= sp.down_bytes ? static_cast<double>(c.down_bytes - sp.down_bytes) : 0.0;
            const double n = c.inst_bytes >= sp.inst_bytes ? static_cast<double>(c.inst_bytes - sp.inst_bytes) : 0.0;
            sp.down_mibps = sp.have ? 0.5 * sp.down_mibps + 0.5 * (d / dt / kMiB) : d / dt / kMiB;
            sp.inst_mibps = sp.have ? 0.5 * sp.inst_mibps + 0.5 * (n / dt / kMiB) : n / dt / kMiB;
            if (static_cast<double>(c.down_bytes) > static_cast<double>(c.inst_bytes) + kInstallBacklog) {
                sp.install.Add(dt, n);
            }
            sp.have = true;
            sp.at_ms = now;
            sp.down_bytes = c.down_bytes;
            sp.inst_bytes = c.inst_bytes;
        }
    }

    // The time of the room and of each receiver (delivery_eta.hpp), from what every receiver still needs of every title, the speed the radio really
    // sends at (an average with a memory: a second of nothing does not halve it) and how fast each installer takes what it has downloaded.
    void Estimate(double dt) {
        const double sent = static_cast<double>(s_.sent_bytes);
        if (sent < radio_bytes_) {
            send_rate_.Reset(); // (the counters started again: another room)
        } else if (s_.phase == DL_S_SENDING && sent > radio_bytes_) {
            send_rate_.Add(dt, sent - radio_bytes_); // (a tick in which nothing went out is the radio resting, not a slow radio)
        }
        radio_bytes_ = sent;

        EtaInput in;
        in.n_titles = n_titles_;
        for (int t = 0; t < n_titles_; ++t) {
            in.size[t] = static_cast<double>(titles_[t].size);
        }
        in.cur_title = s_.phase == DL_S_SENDING ? s_.cur_title : -1;
        in.n_recv = s_.n_clients < DL_MAX_CLIENTS ? s_.n_clients : DL_MAX_CLIENTS;
        for (int i = 0; i < in.n_recv; ++i) {
            const auto& c = s_.client[i];
            EtaReceiver& r = in.recv[i];
            r.present = true;
            r.silent = c.state == DL_C_SILENT;
            r.finished = c.state == DL_C_FINISHED || c.state == DL_C_ERROR;
            for (int t = 0; t < n_titles_; ++t) {
                if (dl_bit(c.want, t)) {
                    r.want |= 1u << t;
                    r.down[t] = Fraction(c.t_down[t]);
                    r.inst[t] = Fraction(c.t_inst[t]);
                }
            }
            r.inst_rate = speed_[i].install.Rate(10.0);
            r.eff = static_cast<double>(c.eff_pct < 1 ? 1 : c.eff_pct) / 100.0;
        }
        in.send_rate = send_rate_.Rate(3.0);
        in.safe_rate = SafeBytesPerSecond();
        eta_ = EstimateRoom(in);
        pct_ = share_.Update(dt, eta_);
        // The receivers show what this screen shows.
        dl_send_estimate(pct_, eta_.kind == EtaKind::Busy ? eta_.seconds : -1.0);
    }

    // B closes the room (after the question); SELECT switches the screens off to save the battery (the room goes on).
    Footer MakeFooter() const {
        Footer f;
        f.back_label = "Close";
        f.n_extra = 1;
        f.extra[0] = FooterKey{"SELECT", dl_screens_off() ? "Screens on" : "Screens off", KEY_SELECT};
        f.start = false;
        f.hint = "";
        return f;
    }

    dl_send_stats s_{};
    dl_title titles_[DL_MAX_TITLES]{};
    int n_titles_ = 0;
    Speed speed_[DL_MAX_CLIENTS];
    RatioFilter send_rate_{20.0}; // how fast the radio sends (everything it sends, repeated blocks too)
    double radio_bytes_ = 0.0;
    u64 tick_ms_ = 0;
    EtaOut eta_;
    RoomShare share_;
    int pct_ = 0;
    bool close_ = false;
    bool confirming_ = false;
    int sel_ = 0;
    int first_ = 0;
};

// The battery and the card in the bar are read again now and then (the main loop that does it is not running while the module is).
void RefreshStatus(u64& last, bool with_card) {
    const u64 now = NowMs();
    if (now - last > 10000) {
        last = now;
        setup::ReadConsoleStatus(g.status, with_card);
    }
}

// What the screens are given while the backlights are out (SELECT): only SELECT itself. A press of B in a pocket must not open the invisible
// question "Close the room?" and a second one answer it.
Input KeysForScreens() {
    Input in = g.input.in();
    if (dl_screens_off()) {
        const u32 select = in.down & KEY_SELECT;
        in = Input();
        in.down = select;
    }
    return in;
}

struct SendRun {
    SendRunPage page;
    u64 last_status = 0;
    std::string failure; // what the module said when it gave up (the last message of its phase "error")
};

int SendTick(void* user, const dl_send_stats* stats) {
    SendRun* run = static_cast<SendRun*>(user);
    PumpInput();
    RefreshStatus(run->last_status, false);
    if (stats->phase == DL_S_ERROR && stats->msg[0]) {
        run->failure = stats->msg;
    }
    const Input in = KeysForScreens();
    run->page.Set(*stats);
    run->page.Update(in);
    if (!dl_skip_drawing()) {
        DrawPage(run->page, false);
    }
    return run->page.close_requested() ? DL_CANCEL : DL_NONE;
}

// ---------------------------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------------------------

// Phases the user only waits through: joining, waiting for the list, an error.
class WaitPage : public Page {
public:
    void Set(const dl_recv_stats& stats, const std::string& room) {
        s_ = &stats;
        room_ = room;
    }
    bool cancel_requested() const {
        return cancel_;
    }

    void Update(const Input& in) override {
        Footer footer = MakeFooter();
        const FooterRects r = LayoutFooter(footer);
        if ((in.down & KEY_B) || Tapped(in, r.back)) {
            cancel_ = true;
        }
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, "Receive Titles", s_->channel);
        DrawCard(10, 32, 380, 198);
        const bool error = s_->phase == DL_R_ERROR;
        gfx::FillRect(10, 32, 3, 198, error ? col::kOff : col::kAccent);
        gfx::DrawText(200, 100, 0.62f, error ? col::kOff : col::kText,
                      error ? std::string("The room could not be joined") : "Joining " + gfx::Ellipsize(gfx::Clean(room_), 0.62f, 240), gfx::Align::Center);
        gfx::DrawText(200, 128, 0.46f, error ? col::kText : col::kDim, gfx::Ellipsize(s_->msg[0] ? s_->msg : "One moment...", 0.46f, 340),
                      gfx::Align::Center);
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        DrawWaitBottom(MakeFooter());
    }

private:
    static Footer MakeFooter() {
        Footer f;
        f.back_label = "Cancel";
        f.start = false;
        return f;
    }
    const dl_recv_stats* s_ = nullptr;
    std::string room_;
    bool cancel_ = false;
};

// What the chosen titles ask of the card: what they weigh, what writing over a title that is installed gives back, the room needed to install them
// one after the other (and a margin), and the room that lets the installing go at its own pace.
struct SpaceNeed {
    int n = 0;
    u64 bytes = 0, smallest = 0, reclaim = 0, largest_net = 0, net_total = 0, need = 0, comfy = 0;
};

SpaceNeed ComputeSpaceNeed(const dl_recv_stats& s) {
    SpaceNeed sp;
    for (int i = 0; i < s.count; ++i) {
        if (!dl_bit(s.want, i)) {
            continue;
        }
        ++sp.n;
        sp.bytes += s.title[i].size;
        sp.smallest = (sp.smallest == 0 || s.title[i].size < sp.smallest) ? s.title[i].size : sp.smallest;
        const u64 old = s.exists[i] ? s.old_size[i] : 0;
        const u64 freed = old < s.title[i].size ? old : s.title[i].size;
        sp.reclaim += freed;
        const u64 net = s.title[i].size - freed;
        sp.largest_net = net > sp.largest_net ? net : sp.largest_net;
    }
    const u64 margin = 64ull << 20;
    sp.net_total = sp.bytes > sp.reclaim ? sp.bytes - sp.reclaim : 0;
    sp.need = sp.n ? (sp.net_total > sp.largest_net ? sp.net_total : sp.largest_net) + margin : 0;
    sp.comfy = sp.n ? sp.bytes + sp.smallest + margin : 0;
    return sp;
}

// The choice: what to take from the room. Games, their updates and their DLC under the name of the game; A chooses, the top screen tells the
// space on the card and the time it takes.
class ChoosePage : public ListPage {
public:
    void Set(dl_recv_stats& stats, const std::string& room) {
        s_ = &stats;
        room_ = room;
    }
    bool cancel_requested() const {
        return cancel_;
    }
    bool confirmed() const {
        return confirm_;
    }
    void ClearConfirm() {
        confirm_ = false;
    }

    void DrawTop() override {
        DrawTitleBarStatus(gfx::kTopW, "Receive Titles", s_->channel);
        const dl_recv_stats& s = *s_;
        u64 offered = 0;
        for (int i = 0; i < s.count; ++i) {
            offered += s.title[i].size;
        }
        // The choice: how many titles, how big, how long.
        const SpaceNeed sp = ComputeSpaceNeed(s);
        const int n = sp.n;
        const u64 bytes = sp.bytes, reclaim = sp.reclaim;
        DrawCard(10, 30, 380, 44);
        gfx::FillRect(10, 30, 3, 44, col::kAccent);
        gfx::DrawText(22, 43, 0.46f, col::kDim,
                      gfx::Ellipsize(Fmt("%s offers %d %s, %s", gfx::Clean(room_).c_str(), s.count, s.count == 1 ? "title" : "titles",
                                         SizeText(offered).c_str()),
                                     0.46f, 196));
        DrawKindLegend(382.0f, 43.0f);
        if (n == 0) {
            gfx::DrawText(22, 61, 0.55f, col::kWarn, "Nothing chosen: choose what to receive");
        } else {
            gfx::DrawText(22, 61, 0.55f, col::kText, Fmt("%d %s, %s", n, n == 1 ? "title" : "titles", SizeText(bytes).c_str()));
            gfx::DrawText(380, 61, 0.46f, col::kDim, DurationText(EstimateSeconds(bytes)), gfx::Align::Right);
        }

        // The card.
        const u64 total = g.status.sd_known ? g.status.sd_total : 0;
        const u64 free_bytes = g.status.sd_known ? g.status.sd_free : 0;
        const u64 used = total > free_bytes ? total - free_bytes : 0;
        const u64 net_total = sp.net_total, need = sp.need, comfy = sp.comfy;
        DrawCard(10, 78, 380, 66);
        gfx::DrawText(22, 91, 0.44f, col::kText, "SD card");
        gfx::DrawText(380, 91, 0.42f, col::kDim,
                      total ? Fmt("%s free of %s", SizeText(free_bytes).c_str(), SizeText(total).c_str()) : std::string("space unknown"),
                      gfx::Align::Right);
        DrawSpaceBar(22, 100, 356, 12, total ? static_cast<double>(used) / static_cast<double>(total) : 0.0,
                     total ? static_cast<double>(net_total) / static_cast<double>(total) : 0.0);
        gfx::FillRect(22, 120, 8, 8, col::kOn);
        gfx::DrawText(34, 124, 0.38f, col::kDim, "in use");
        gfx::FillRect(86, 120, 8, 8, col::kOff);
        gfx::DrawText(98, 124, 0.38f, col::kDim, n ? "+" + SizeText(net_total) + " to add" : std::string("what you add"));
        if (n && total && free_bytes < need) {
            gfx::DrawText(380, 124, 0.4f, col::kOff, "not enough space for all", gfx::Align::Right);
        } else if (n && total && free_bytes < comfy) {
            gfx::DrawText(380, 124, 0.4f, col::kWarn, "tight: it can be slower", gfx::Align::Right);
        }
        gfx::DrawText(22, 137, 0.36f, col::kDim, reclaim > 0 ? "Writing over a title frees the space it takes now." : "");

        // The row under the cursor.
        if (const Row* row = Selected()) {
            DrawNote(10, 148, 380, g.ToastActive() ? 54 : 84, *row);
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
        return "Choose what to download";
    }
    std::string BarRight() const override {
        int n = 0;
        for (int i = 0; i < s_->count; ++i) {
            n += dl_bit(s_->want, i) ? 1 : 0;
        }
        return Fmt("%d / %d", n, s_->count);
    }
    const char* ActionLabel() const override {
        return "Choose";
    }
    const char* BackLabel() const override {
        return "Cancel";
    }
    int ExtraKeys(FooterKey* out) const override {
        out[0] = FooterKey{"X", "All", KEY_X};
        out[1] = FooterKey{"Y", "Receive", KEY_Y};
        return 2;
    }
    void OnBack() override {
        cancel_ = true;
    }
    void OnExtra(int index, const Row*) override {
        if (index == 0) {
            bool all = true;
            for (int i = 0; i < s_->count; ++i) {
                all = all && dl_bit(s_->want, i);
            }
            for (int i = 0; i < s_->count; ++i) {
                dl_set_bit(s_->want, i, all ? 0 : 1);
            }
            return;
        }
        const SpaceNeed sp = ComputeSpaceNeed(*s_);
        const u64 total = g.status.sd_known ? g.status.sd_total : 0;
        const u64 free_bytes = g.status.sd_known ? g.status.sd_free : 0;
        if (sp.n == 0) {
            g.ShowToast("Choose at least one title first.", col::kWarn, 3000);
        } else if (total && free_bytes < sp.need) {
            // (installing into a card that is too full fails half way, after hours of work: it is not started)
            g.ShowToast("Not enough space on the SD card for what is chosen: choose fewer titles.", col::kOff, 5000);
        } else {
            confirm_ = true;
        }
    }

    void BuildRows(std::vector<Row>& rows) override {
        rows.clear();
        const dl_recv_stats& s = *s_;
        int first = 0;
        while (first < s.count) {
            int last = first;
            while (last + 1 < s.count && std::strcmp(s.title[last + 1].name, s.title[first].name) == 0) {
                ++last;
            }
            int chosen = 0;
            u64 bytes = 0;
            for (int i = first; i <= last; ++i) {
                if (dl_bit(s.want, i)) {
                    ++chosen;
                    bytes += s.title[i].size;
                }
            }
            const int members = last - first + 1;
            Row header;
            header.id = kHeader + first;
            header.kind = Kind::Check;
            header.label = gfx::Clean(s.title[first].name);
            header.on = chosen == members;
            header.partial = chosen > 0 && chosen < members;
            for (int i = first; i <= last; ++i) {
                header.tags |= KindTag(s.title[i].kind); // (the coloured bars of the list the sender chose from)
            }
            header.value = bytes > 0 ? SizeText(bytes) : std::string();
            header.note_title = header.label;
            u64 group_bytes = 0;
            for (int i = first; i <= last; ++i) {
                group_bytes += s.title[i].size;
            }
            header.note = Fmt("%d %s offered, %s in all.\nTakes %s to download all of %s.", members, members == 1 ? "title" : "titles", SizeText(group_bytes).c_str(),
                              DurationText(EstimateSeconds(group_bytes)).c_str(), members == 1 ? "it" : "them");
            rows.push_back(header);
            for (int i = first; i <= last; ++i) {
                Row row;
                row.id = i;
                row.kind = Kind::Check;
                row.indent = 14.0f;
                row.label = KindText(s.title[i].kind, s.title[i].tid);
                row.label_color = KindColor(s.title[i].kind);
                row.on = dl_bit(s.want, i) != 0;
                row.value = SizeText(s.title[i].size);
                row.note_title = header.label + ": " + row.label;
                // What the screen at the top says of a title: how big it is and how long it takes, and what it is (the list below already says
                // whether it is installed and what A does).
                row.note = "Size " + SizeText(s.title[i].size) + ", " + DurationText(EstimateSeconds(s.title[i].size)) + " to download\nProduct code " +
                           gfx::Clean(s.title[i].code) + "\nID " + setup::TidText(s.title[i].tid) + (s.title[i].has_seed ? "\nAn eShop title (it comes with a seed)" : "");
                if (s.exists[i]) {
                    row.chip = "installed";
                    row.chip_color = col::kWarn;
                    row.note += "\nChoosing it replaces the installed copy: the title and its data are deleted first.";
                } else if (s.n3ds_only[i]) {
                    row.chip = "New 3DS";
                    row.chip_color = col::kOff;
                    row.note += "\nIt is for the New 3DS only and this console is an Old 3DS. You can still install it, as with FBI.";
                }
                rows.push_back(row);
            }
            first = last + 1;
        }
    }

    void Activate(const Row& row, int step) override {
        if (step != 0) {
            return;
        }
        dl_recv_stats& s = *s_;
        if (row.id >= kHeader) {
            const int first = row.id - kHeader;
            int last = first;
            while (last + 1 < s.count && std::strcmp(s.title[last + 1].name, s.title[first].name) == 0) {
                ++last;
            }
            bool any = false;
            for (int i = first; i <= last; ++i) {
                any = any || dl_bit(s.want, i);
            }
            for (int i = first; i <= last; ++i) {
                dl_set_bit(s.want, i, any ? 0 : 1);
            }
        } else if (row.id >= 0 && row.id < s.count) {
            dl_set_bit(s.want, row.id, !dl_bit(s.want, row.id));
        }
    }

private:
    static constexpr int kHeader = 100;
    dl_recv_stats* s_ = nullptr;
    std::string room_;
    bool cancel_ = false;
    bool confirm_ = false;
};

// While it receives: how far it is, how fast, and where each title stands.
class ProgressPage : public Page {
public:
    void Set(const dl_recv_stats& stats, const std::string& room) {
        s_ = &stats;
        room_ = room;
        // How fast the blocks come in (looked at once a second: the module counts blocks, not speed).
        const u64 now = NowMs();
        if (prev_ms_ == 0) {
            prev_ms_ = now;
            prev_blocks_ = stats.dl_blocks;
        } else if (now - prev_ms_ >= 1000) {
            const double dt = static_cast<double>(now - prev_ms_) / 1000.0;
            const double got = stats.dl_blocks >= prev_blocks_ ? static_cast<double>(stats.dl_blocks - prev_blocks_) : 0.0;
            rate_ = rate_ <= 0.0 ? got / dt : 0.6 * rate_ + 0.4 * (got / dt);
            prev_ms_ = now;
            prev_blocks_ = stats.dl_blocks;
        }
    }
    bool cancel_requested() const {
        return cancel_;
    }

    void Update(const Input& in) override {
        const FooterRects r = LayoutFooter(MakeFooter());
        if (Tapped(in, r.extra[0])) {
            dl_toggle_screens(); // (the key itself is read by ReceiveTick, in every phase)
        }
        if (confirming_) {
            // B asks first: leaving the room drops what is not installed yet. A leaves, B stays.
            const PopupBox box = ConfirmBox();
            if ((in.down & KEY_A) || Tapped(in, box.yes)) {
                cancel_ = true;
            } else if ((in.down & KEY_B) || Tapped(in, box.no)) {
                confirming_ = false;
            }
            return;
        }
        if (s_->phase != DL_R_DONE && ((in.down & KEY_B) || Tapped(in, r.back))) {
            confirming_ = true;
        }
    }

    void DrawTop() override {
        const dl_recv_stats& s = *s_;
        DrawTitleBarStatus(gfx::kTopW, "Receive Titles", s.channel);
        const int down = Pct(s.dl_blocks, s.need_blocks, s.need_blocks > 0 && s.dl_blocks >= s.need_blocks);
        const int inst = Pct(s.inst_blocks, s.need_blocks, s.all_done != 0);
        const bool done = s.phase == DL_R_DONE;
        // Nothing is missing any more (or it is over): this console needs no more data, so "no block for a while" is no fault and the sender's numbers stand.
        const bool no_more = done || (s.need_blocks > 0 && s.dl_blocks >= s.need_blocks);
        // No block for a while: the numbers that talk about what comes in say 0 instead of what they saw last.
        const bool stale = s.data_age_ms > 2500 && !no_more;

        // How far this console is: two bars of the same size, each with its percentage, then the speeds and the time left, then the state.
        DrawCard(10, 30, 380, 74);
        gfx::FillRect(10, 30, 3, 74, done ? col::kOn : col::kAccent);
        gfx::DrawText(22, 43, 0.5f, col::kText, "Installed");
        DrawProgressBar(100, 37, 212, 12, static_cast<float>(inst) / 100.0f);
        gfx::DrawText(322, 43, 0.62f, col::kText, Percent(inst));
        gfx::DrawText(22, 61, 0.5f, col::kText, "Downloaded");
        DrawProgressBar(100, 55, 212, 12, static_cast<float>(down) / 100.0f);
        gfx::DrawText(322, 61, 0.62f, col::kText, Percent(down));
        if (done) {
            gfx::DrawText(22, 79, 0.5f, s.all_done ? col::kOn : col::kWarn, s.all_done ? "Done" : "Finished");
        } else {
            u64 wanted_bytes = 0;
            for (int i = 0; i < s.count; ++i) {
                wanted_bytes += dl_bit(s.want, i) ? s.title[i].size : 0;
            }
            const double bytes_per_block = s.need_blocks > 0 ? static_cast<double>(wanted_bytes) / static_cast<double>(s.need_blocks) : 0.0;
            const double down_mibps = stale ? 0.0 : rate_ * bytes_per_block / kMiB;
            float x = DrawPair(22, 79, 0.4f, "installing at", Fmt("%.2f MiB/s", s.mibps));
            DrawPair(x, 79, 0.4f, "downloading at", Fmt("%.2f MiB/s", down_mibps));
            if (s.eta_sec >= 0.0) {
                gfx::DrawText(382, 79, 0.42f, col::kText, DurationText(s.eta_sec) + " left", gfx::Align::Right);
            }
        }
        // The state, once: green while it receives (or when it has all it needs and installs), yellow while it waits for the sender to serve others or for a
        // place in a full room, red when the connection is lost.
        if (s.msg[0]) {
            const std::string msg = gfx::Clean(s.msg);
            const bool lost = msg.rfind("connection lost", 0) == 0;
            const bool full = msg.rfind("The room is full", 0) == 0;
            const bool bad = lost || msg.rfind("finished with errors", 0) == 0 || msg.rfind("the sender", 0) == 0 || msg.rfind("could not", 0) == 0;
            const bool good = msg.rfind("receiving", 0) == 0 || msg.rfind("installation complete", 0) == 0 || msg.rfind("all downloaded", 0) == 0;
            const bool wait = msg.rfind("waiting", 0) == 0 || full;
            const u32 color = bad ? col::kOff : (good ? col::kOn : (wait ? col::kWarn : col::kText));
            const bool counter = s.reconnects > 0 && !lost && !full; // (while the link is being found again the line is the whole message)
            gfx::DrawFitted(22, 95, 0.42f, 0.34f, counter ? 220.0f : 356.0f, color, msg);
            if (counter) {
                gfx::DrawText(382, 95, 0.4f, col::kWarn, Fmt("reconnected %d time%s", s.reconnects, s.reconnects == 1 ? "" : "s"), gfx::Align::Right);
            }
        }

        // The room: who sends and how fast, how far the whole operation is, and the other receivers (the room holds 7: this console and six more),
        // each with the name of its console, what it is doing and how much it has installed. This console is not in the list: it is the card above.
        DrawCard(10, 108, 380, 100);
        const std::string sender = gfx::Clean(room_);
        const float x0 = 22.0f;
        gfx::DrawFitted(x0, 119, 0.44f, 0.34f, 110, col::kText, sender);
        float x = x0 + std::min(gfx::TextWidth(sender, 0.44f), 110.0f) + 6.0f;
        gfx::DrawText(x, 119, 0.36f, col::kDim, "sends");
        x += gfx::TextWidth("sends", 0.36f) + 4.0f;
        const std::string sends = Fmt("%.2f MiB/s", stale ? 0.0 : static_cast<double>(s.sender_kibs) / 1024.0);
        gfx::DrawText(x, 119, 0.4f, col::kText, sends);
        x += gfx::TextWidth(sends, 0.4f) + 8.0f;
        DrawPair(x, 119, 0.36f, "last one in", stale ? std::string("--:--") : ClockText(s.sender_eta_sec));
        const std::string completed = "completed: " + Percent(static_cast<int>(s.sender_next));
        gfx::DrawText(382, 119, 0.46f, col::kText, completed, gfx::Align::Right);
        if (s.sender_hears == 0) {
            gfx::DrawText(382, 131, 0.34f, col::kOff, "the sender cannot hear this console", gfx::Align::Right);
        }
        const int others = s.n_others < kOtherRows ? s.n_others : kOtherRows;
        if (others == 0) {
            gfx::DrawText(24, 150, 0.4f, col::kDim, "no other receiver in the room");
        }
        for (int slot = 0; slot < others; ++slot) {
            const float yc = 137.0f + 13.0f * static_cast<float>(slot);
            const int state = s.other_state[slot] == 255 ? -1 : static_cast<int>(s.other_state[slot]);
            const int pct = static_cast<int>(s.other_cursor[slot] > 100 ? 100 : s.other_cursor[slot]);
            gfx::FillCircle(24, yc, 3.0f, state >= 0 ? ReceiverStateColor(state) : col::kAccent);
            gfx::DrawFitted(33, yc, 0.4f, 0.3f, 106, col::kText, s.other_name[slot][0] ? gfx::Clean(s.other_name[slot]) : Fmt("Receiver %d", slot + 2));
            if (state >= 0) {
                gfx::DrawFitted(144, yc, 0.34f, 0.28f, 64, ReceiverStateColor(state), ReceiverStateWord(state, 0));
            }
            gfx::FillRect(214, yc - 3.0f, 112, 6, col::kTrack);
            gfx::FillRect(214, yc - 3.0f, 112.0f * static_cast<float>(pct) / 100.0f, 6, state == DL_C_FINISHED ? col::kOn : col::kAccent);
            gfx::DrawText(382, yc, 0.38f, col::kText, Percent(pct), gfx::Align::Right);
        }

        // What the radio did, on one line (the numbers bright, their names dim; shorter names when the line would not fit).
        DrawCard(10, 211, 380, 23);
        struct Item {
            const char* full;
            const char* shortname;
            std::string value;
            u32 color;
        };
        const Item items[] = {
            {"received", "recv", Fmt("%llu", static_cast<unsigned long long>(s.recv)), col::kText},
            {"repeated packets", "repeated", Fmt("%llu", static_cast<unsigned long long>(s.dups)), col::kText},
            {"outside the window", "outside", Fmt("%llu", static_cast<unsigned long long>(s.out_of_window)), col::kText},
            {"last data", "last", no_more ? std::string("-") : Fmt("%u ms ago", static_cast<unsigned>(s.data_age_ms)), no_more ? col::kDim : (stale ? col::kOff : col::kText)},
            {"AM", "AM", Fmt("%u/%u ms", static_cast<unsigned>(s.am_last_ms), static_cast<unsigned>(s.am_max_ms)), col::kText},
        };
        float scale = 0.34f;
        bool use_short = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            float total = 0.0f;
            for (const Item& item : items) {
                total += gfx::TextWidth(use_short ? item.shortname : item.full, scale) + gfx::TextWidth(item.value, scale) + 13.0f;
            }
            if (total <= 358.0f) {
                break;
            }
            use_short = true;
        }
        float lx = 22.0f;
        for (const Item& item : items) {
            lx = DrawPair(lx, 222, scale, use_short ? item.shortname : item.full, item.value, item.color);
        }
        DrawFrameBorder(gfx::kTopW);
    }

    void DrawBottom() override {
        const dl_recv_stats& s = *s_;
        DrawTitleBar(gfx::kBottomW, "Titles", std::string());
        float y = 38.0f;
        int more = 0, more_done = 0; // the titles that do not fit (a room carries up to 16)
        for (int i = 0; i < s.count; ++i) {
            if (!dl_bit(s.want, i)) {
                continue;
            }
            if (y >= 183.0f) {
                ++more;
                more_done += s.result[i] == DL_RES_INSTALLED ? 1 : 0;
                continue;
            }
            const bool current = s.phase == DL_R_RECEIVING && s.cur_title == i && s.result[i] == DL_RES_PENDING;
            const u32 color = s.result[i] == DL_RES_INSTALLED ? col::kOn : s.result[i] == DL_RES_ERROR ? col::kOff : current ? col::kWarn : col::kDim;
            gfx::FillRect(8, y - 10, 304, 22, col::kPanel);
            gfx::FillCircle(20, y + 2, 4.0f, color);
            DrawKindMarks(46.0f, y + 2, KindTag(s.title[i].kind));
            gfx::DrawText(50, y + 2, 0.46f, col::kText, gfx::Ellipsize(gfx::Clean(s.title[i].name), 0.46f, 176));
            std::string word = s.result[i] == DL_RES_INSTALLED ? "installed" : s.result[i] == DL_RES_ERROR ? "error" : current ? "installing" : "waiting";
            if (current && s.title[i].size > 0) {
                // how much of this title AM has been given (it stays at 0% for a while on a big title: AM makes the room first)
                const u64 pct = s.cur_inst_bytes * 100 / s.title[i].size;
                word += Fmt(" %d%%", static_cast<int>(pct > 99 ? 99 : pct));
            }
            gfx::DrawText(304, y + 2, 0.42f, color, word, gfx::Align::Right);
            y += 24.0f;
        }
        if (more > 0) {
            gfx::DrawText(160, 201, 0.4f, col::kDim, Fmt("and %d more: %d installed", more, more_done), gfx::Align::Center);
        }
        DrawFooterBackground();
        DrawFooter(MakeFooter());
        DrawFrameBorder(gfx::kBottomW);
        if (confirming_) {
            const PopupBox box = ConfirmBox();
            DrawDim(gfx::kBottomW);
            DrawPopupBox(box, "Leave the room?", kLeaveText);
            DrawButton(box.yes, "A", "Yes, leave", true, PressedNow(g.input.in(), box.yes));
            DrawButton(box.no, "B", "No, stay", true, PressedNow(g.input.in(), box.no));
            DrawFrameBorder(gfx::kBottomW);
        }
    }

private:
    static constexpr const char* kLeaveText =
        "The receiving stops here: what is not installed yet is dropped (the titles already installed stay on this console). You can join the room again later.";
    static PopupBox ConfirmBox() {
        return LayoutPopup(kLeaveText, true);
    }
    static int Pct(u32 a, u32 b, bool done) {
        if (done) {
            return 100;
        }
        if (b == 0) {
            return 0;
        }
        const u64 p = static_cast<u64>(a) * 100 / b;
        return p > 99 ? 99 : static_cast<int>(p);
    }
    Footer MakeFooter() const {
        Footer f;
        f.back = s_->phase != DL_R_DONE;
        f.back_label = "Cancel";
        f.n_extra = 1;
        f.extra[0] = FooterKey{"SELECT", dl_screens_off() ? "Screens on" : "Screens off", KEY_SELECT};
        f.start = false;
        return f;
    }

    const dl_recv_stats* s_ = nullptr;
    std::string room_;
    bool cancel_ = false;
    bool confirming_ = false;
    u64 prev_ms_ = 0;
    u32 prev_blocks_ = 0;
    double rate_ = 0.0; // blocks a second that came in (a mean of the last few seconds)
};

struct ReceiveRun {
    std::string room;
    WaitPage wait;
    ChoosePage choose;
    ProgressPage progress;
    ReceiveResult result;
    u64 last_status_read = 0;
};

int ReceiveTick(void* user, dl_recv_stats* stats) {
    ReceiveRun* run = static_cast<ReceiveRun*>(user);
    PumpInput();
    RefreshStatus(run->last_status_read, false);
    if (stats->free_mib != DL_FREE_UNKNOWN && g.status.sd_known) {
        g.status.sd_free = static_cast<u64>(stats->free_mib) << 20; // (the module's own reading: the screen does not ask the card, which may be busy with the installing)
    }
    if (g.input.in().down & KEY_SELECT) {
        dl_toggle_screens();
    }
    const Input in = KeysForScreens();
    run->result.stats = *stats;
    if (stats->phase == DL_R_RECEIVING || stats->phase == DL_R_DONE) {
        // The clock of the summary: from the moment the receiving begins to the last look, and when each title was installed.
        const u64 now = NowMs();
        if (run->result.start_ms == 0) {
            run->result.start_ms = now;
        }
        run->result.end_ms = now;
        for (int i = 0; i < stats->count && i < DL_MAX_TITLES; ++i) {
            if (stats->result[i] == DL_RES_INSTALLED && run->result.done_ms[i] == 0) {
                run->result.done_ms[i] = now - run->result.start_ms + 1;
            }
        }
    }
    Page* page = nullptr;
    bool cancel = false;
    bool confirm = false;
    bool wait_for_screen = false;
    if (stats->phase == DL_R_CHOOSE && stats->have_manifest) {
        run->choose.Set(*stats, run->room);
        run->choose.Update(in);
        cancel = run->choose.cancel_requested();
        confirm = run->choose.confirmed();
        run->choose.ClearConfirm();
        page = &run->choose;
        wait_for_screen = true; // (the choice runs at the screen's pace: nothing is being received yet)
    } else if (stats->phase == DL_R_RECEIVING || stats->phase == DL_R_DONE) {
        run->progress.Set(*stats, run->room);
        run->progress.Update(in);
        cancel = run->progress.cancel_requested();
        page = &run->progress;
    } else {
        run->wait.Set(*stats, run->room);
        run->wait.Update(in);
        cancel = run->wait.cancel_requested();
        page = &run->wait;
        wait_for_screen = true;
    }
    if (!dl_skip_drawing()) {
        DrawPage(*page, wait_for_screen);
    } else {
        svcSleepThread(2 * 1000 * 1000); // (nothing to draw and the module calls the tick without a pause while the user chooses: do not spin)
    }
    if (cancel) {
        run->result.cancelled = true;
        return DL_CANCEL;
    }
    return confirm ? DL_CONFIRM : DL_NONE;
}

} // namespace

void RunSend() {
    Wireless wireless;
    if (!wireless.Open()) {
        g.ShowToast(WirelessErrorText(wireless.error()), col::kOff, 7000);
        return;
    }
    SendRun run;
    int rc = 0;
    g.HoldHome(); // (the module turns HOME off for the whole room too: the bar of the touch screen says so)
    g.input.StartLatch(); // (the screen is drawn four times a second: a press that short must not fall between two looks)
    const bool started = RunOnBigStack([&]() { rc = dl_send_run(&SendTick, &run); });
    g.input.StopLatch();
    wireless.Close(); // (the module gives HOME back here)
    g.ReleaseHome();   // (...and this puts it off again when a page still wants it so)
    g.input.BlockUntilReleased(); // (the B that closed the room must not go on to the page under it)
    if (!dl_system_ok()) {
        g.quit = true; // (the system is closing the app: HOME menu, Close Software)
        return;
    }
    if (!started) {
        g.ShowToast("Not enough memory to open the room: close other applications and try again.", col::kOff, 7000);
        return;
    }
    // (the reason is told here: the log that could hold it exists only when the Debug log is on)
    g.ShowToast(rc == 0 ? std::string("The room was closed.")
                        : "The room was closed by an error" + (run.failure.empty() ? std::string(".") : ": " + gfx::Clean(run.failure)),
                rc == 0 ? col::kText : col::kWarn, 7000);
}

void RunReceive(const std::string& room_name) {
    ReceiveRun run;
    run.room = room_name;
    run.result.room = room_name;
    int rc = 0;
    g.HoldHome();
    g.input.StartLatch();
    const bool started = RunOnBigStack([&]() { rc = dl_recv_run(&ReceiveTick, &run); });
    g.input.StopLatch();
    g.ReleaseHome();
    g.input.BlockUntilReleased();
    if (run.result.start_ms != 0) {
        g.rescan_wanted = true; // (the receiving began: titles were installed, or a half one was taken away; the list of titles is read again)
    }
    if (!dl_system_ok()) {
        g.quit = true;
        return;
    }
    if (!started) {
        g.ShowToast("Not enough memory to join the room: close other applications and try again.", col::kOff, 7000);
        return;
    }
    // The summary is for a receiving that began. A room that could not be joined, or one that was left before anything was chosen, says so in a
    // message (with the reason the module gave) and goes back to the menu.
    if (run.result.start_ms == 0) {
        const char* msg = run.result.stats.msg;
        g.ShowToast(rc == -1 || run.result.stats.phase == DL_R_ERROR ? "The room could not be joined" + (msg[0] ? ": " + gfx::Clean(msg) : std::string("."))
                                                           : std::string("You left the room before receiving anything."),
                    col::kWarn, 7000);
        return;
    }
    PushPage(pages::NewReceiveResultPage(run.result));
}

} // namespace delivery
