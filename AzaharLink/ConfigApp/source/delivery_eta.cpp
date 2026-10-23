#include "delivery_eta.hpp"

#include <algorithm>
#include <cmath>

namespace delivery {

namespace {

constexpr double kBlock = 1456.0;                // a block of the module, in bytes
constexpr double kEps = kBlock;                   // less than a block is nothing
constexpr double kTiny = 7200.0 * kBlock;         // the "remainder" of the scheduler: what is left of a title is sent first when it is this little
constexpr double kMinRate = 0.3 * kMiB;           // a measured speed below this is not believed
constexpr double kMinInstRate = 0.05 * kMiB;
constexpr double kSlotGap = 2.0;                  // between two titles: the blocks in flight (0.5 s), the acknowledgements, the repairs of the end
constexpr double kTitleFixed = 3.0;               // what an installation costs apart from the bytes (StartCiaInstall, FinishCiaInstall)
constexpr double kTail = 5.0;                     // after the last block of a title, until it is installed
constexpr double kEffMin = 0.35;                  // the efficiency of a receiver is never taken below this (it is noisy after an interruption)
constexpr double kAbsorb = 256.0 * kMiB;          // what a receiver that joins late can take in at once (its window, its queue and its parking)
constexpr double kLagShare = 0.5;                 // of what a late joiner has to catch up beyond that, the part that costs radio time of its own

double Clamp01(double v) {
    return v != v ? 0.0 : (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
}

double Positive(double v) {
    return v != v || v < 0.0 ? 0.0 : v;
}

} // namespace

EtaOut EstimateRoom(const EtaInput& in) {
    EtaOut out;
    const int nt = std::min(std::max(in.n_titles, 0), kEtaTitles);
    const int nr = std::min(std::max(in.n_recv, 0), kEtaRecv);
    double size[kEtaTitles];
    double total = 0.0;
    for (int t = 0; t < nt; ++t) {
        size[t] = Positive(in.size[t]);
        total += size[t];
    }

    // the work of the room: what each receiver asked for and what of it is installed (before any early answer below)
    for (int a = 0; a < nr; ++a) {
        const EtaReceiver& r = in.recv[a];
        if (!r.present || r.want == 0) {
            continue;
        }
        double asked = 0.0, installed = 0.0;
        for (int t = 0; t < nt; ++t) {
            if ((r.want >> t) & 1u) {
                asked += size[t];
                installed += size[t] * Clamp01(r.inst[t]);
            }
        }
        out.work_total += asked;
        out.work_done += r.finished ? asked : installed;
        out.work_finished += r.finished ? asked : 0.0;
    }

    bool live[kEtaRecv] = {};
    int n_present = 0, n_silent = 0, n_live = 0, n_choosing = 0;
    for (int a = 0; a < nr; ++a) {
        const EtaReceiver& r = in.recv[a];
        if (!r.present) {
            continue;
        }
        if (r.finished) {
            ++n_present;
            continue;
        }
        if (r.silent) {
            ++n_present;
            ++n_silent;
            continue;
        }
        if (r.want == 0) {
            ++n_choosing; // (it has not said what it wants yet: there is nothing to count for it)
            continue;
        }
        ++n_present;
        live[a] = true;
        ++n_live;
    }
    if (n_present == 0 || (n_live == 0 && n_silent == 0 && n_choosing > 0)) {
        out.kind = EtaKind::Nobody;
        out.seconds = in.safe_rate > 0.0 ? total / in.safe_rate : 0.0;
        return out;
    }
    if (n_live == 0) {
        out.kind = n_silent > 0 ? EtaKind::Silent : EtaKind::Done;
        out.seconds = n_silent > 0 ? -1.0 : 0.0;
        return out;
    }
    out.kind = EtaKind::Busy;
    const double rate = in.send_rate >= kMinRate ? in.send_rate : std::max(in.safe_rate, kMinRate);

    // what every receiver still needs of every title, which title it installs now, how well the air reaches it
    double need[kEtaRecv][kEtaTitles] = {};
    bool takes[kEtaRecv][kEtaTitles] = {};
    int busy[kEtaRecv];
    double eff[kEtaRecv];
    for (int a = 0; a < nr; ++a) {
        busy[a] = -1;
        eff[a] = 1.0;
        if (!live[a]) {
            continue;
        }
        const EtaReceiver& r = in.recv[a];
        eff[a] = std::min(std::max(r.eff == r.eff ? r.eff : 1.0, kEffMin), 1.0);
        double best = 0.0;
        for (int t = 0; t < nt; ++t) {
            if (!((r.want >> t) & 1u)) {
                continue;
            }
            double n = size[t] * (1.0 - Clamp01(r.down[t]));
            if (n < kEps) {
                n = 0.0;
            }
            need[a][t] = n;
            takes[a][t] = n > 0.0;
            const double installing = Clamp01(r.inst[t]);
            if (n > 0.0 && installing > best) {
                best = installing;
                busy[a] = t; // (it is installing this title and still lacks blocks of it: it takes no other until it has them)
            }
        }
    }

    // the order of the titles: what the scheduler would choose, again after each one
    bool sent[kEtaTitles] = {};
    bool first = true;
    double clock = 0.0;
    for (;;) {
        bool cand[kEtaTitles] = {};
        int k[kEtaTitles] = {};
        double tiny[kEtaTitles] = {}, dur[kEtaTitles] = {};
        bool any = false;
        for (int t = 0; t < nt; ++t) {
            if (sent[t]) {
                continue;
            }
            double mx = 0.0, mn = 0.0, slowest = 0.0;
            for (int a = 0; a < nr; ++a) {
                const double n = live[a] ? need[a][t] : 0.0;
                if (n <= 0.0) {
                    continue;
                }
                cand[t] = true;
                mx = std::max(mx, n);
                mn = mn == 0.0 ? n : std::min(mn, n);
                slowest = std::max(slowest, n / (rate * eff[a]));
                if (busy[a] < 0 || busy[a] == t || n <= kTiny) {
                    ++k[t];
                }
                if (n <= kTiny && eff[a] >= 0.5) {
                    tiny[t] = tiny[t] == 0.0 ? n : std::min(tiny[t], n);
                }
            }
            if (!cand[t]) {
                continue;
            }
            any = true;
            dur[t] = slowest + kLagShare * std::max(0.0, (mx - mn) - kAbsorb) / rate;
        }
        if (!any) {
            break;
        }
        int pick = -1;
        for (int t = 0; t < nt; ++t) { // a remainder first (the smallest), else the title most receivers can take
            if (!cand[t]) {
                continue;
            }
            if (pick < 0) {
                pick = t;
                continue;
            }
            const bool t_tiny = tiny[t] > 0.0, p_tiny = tiny[pick] > 0.0;
            bool better = false;
            if (t_tiny != p_tiny) {
                better = t_tiny;
            } else if (t_tiny) {
                better = tiny[t] < tiny[pick];
            } else if (k[t] != k[pick]) {
                better = k[t] > k[pick];
            } else {
                const bool t_cur = first && t == in.cur_title, p_cur = first && pick == in.cur_title;
                if (t_cur != p_cur) {
                    better = t_cur;
                } else if (std::fabs(dur[t] - dur[pick]) > 1e-9) {
                    better = dur[t] < dur[pick];
                }
            }
            if (better) {
                pick = t;
            }
        }
        out.start[pick] = clock;
        out.end[pick] = clock + dur[pick];
        clock = out.end[pick] + kSlotGap;
        out.order[out.n_order++] = pick;
        sent[pick] = true;
        first = false;
        for (int a = 0; a < nr; ++a) {
            need[a][pick] = 0.0;
            if (busy[a] == pick) {
                busy[a] = -1;
            }
        }
    }
    out.send_seconds = out.n_order > 0 ? clock - kSlotGap : 0.0;

    // each receiver: its titles come in that order and are installed one after the other, the installation overlapping the next download
    for (int a = 0; a < nr; ++a) {
        if (!live[a]) {
            continue;
        }
        const EtaReceiver& r = in.recv[a];
        const double q = r.inst_rate >= kMinInstRate * 1.0 ? r.inst_rate : rate; // (not known: it is taken to keep up with the radio)
        int seq[2 * kEtaTitles];
        int n_seq = 0;
        // first what it has and has not installed yet (smallest first), then what is still to come
        int have[kEtaTitles];
        int n_have = 0;
        for (int t = 0; t < nt; ++t) {
            if (((r.want >> t) & 1u) && !takes[a][t] && size[t] * (1.0 - Clamp01(r.inst[t])) >= kEps) {
                have[n_have++] = t;
            }
        }
        std::stable_sort(have, have + n_have, [&](int x, int y) {
            return size[x] * (1.0 - Clamp01(r.inst[x])) < size[y] * (1.0 - Clamp01(r.inst[y]));
        });
        for (int i = 0; i < n_have; ++i) {
            seq[n_seq++] = have[i];
        }
        double wait = -1.0;
        for (int i = 0; i < out.n_order; ++i) {
            const int t = out.order[i];
            if (takes[a][t]) {
                seq[n_seq++] = t;
                if (wait < 0.0) {
                    wait = out.start[t];
                }
            }
        }
        double f = 0.0;
        for (int i = 0; i < n_seq; ++i) {
            const int t = seq[i];
            const double inst_left = size[t] * (1.0 - Clamp01(r.inst[t]));
            const double s = takes[a][t] ? out.start[t] : 0.0;
            const double e = takes[a][t] ? out.end[t] : 0.0;
            const double begin = std::max(f, s);
            f = std::max(begin + inst_left / q + kTitleFixed, takes[a][t] ? e + kTail : 0.0);
        }
        out.recv[a].seconds = f;
        out.recv[a].wait = wait > 0.0 ? wait : 0.0;
        out.seconds = std::max(out.seconds, f);
    }
    return out;
}

void RatioFilter::Add(double dt, double bytes) {
    if (!(dt > 0.0) || dt > 120.0) {
        return; // (a pause of the whole app is not a measurement)
    }
    const double decay = std::exp(-dt / tau_);
    num_ = num_ * decay + Positive(bytes);
    den_ = den_ * decay + dt;
}

double RatioFilter::Rate(double min_den) const {
    return den_ >= min_den && den_ > 0.0 ? num_ / den_ : 0.0;
}

int RoomShare::Update(double dt, const EtaOut& eta) {
    if (eta.kind == EtaKind::Busy || eta.kind == EtaKind::Done) {
        idle_ = 0.0;
    } else {
        idle_ += dt;
        if (idle_ >= 30.0) {
            credit_ = finished_ = total_ = 0.0; // (a new batch of work starts from nothing)
            pct_ = 0;
        }
        return 0;
    }
    // a finished receiver that has left: its work was done, so it stays in the count
    if (eta.work_finished < finished_) {
        credit_ += finished_ - eta.work_finished;
    }
    finished_ = eta.work_finished;
    const double total = eta.work_total + credit_;
    const double done = eta.work_done + credit_;
    int pct = pct_;
    if (eta.kind == EtaKind::Done) {
        pct = 100;
    } else if (total > 0.0) {
        pct = std::min(std::max(static_cast<int>(100.0 * done / total), 0), 99);
        if (total <= total_ + 1.0 && pct < pct_) {
            pct = pct_; // (only more work may make it fall: a receiver that leaves, or a rounding, must not)
        }
    }
    total_ = total;
    pct_ = pct;
    return pct;
}

} // namespace delivery
