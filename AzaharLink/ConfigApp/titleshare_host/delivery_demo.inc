// Included at the end of delivery.c by a build made to look at the Titles Delivery screens on an emulator (make
// EXTRA_CFLAGS=-DAZL_TEST_DELIVERY_DEMO): no wireless is used, the titles are checked at once with made-up sizes, three rooms are "found", and
// a sending and a receiving run through all their phases on made-up numbers. Nothing of this is in a release build.

static int demo_scan_count(void) {
    return ts_now_ms() % 4000 < 300 ? 0 : 3; // (the list empties for a moment now and then, as a real scan does)
}

static int demo_scan_get(int index, dl_room* out) {
    static const char* const hosts[3] = {"Friend-A", "Friend-D's New 3DS", "Player-G"};
    static const unsigned titles[3] = {5, 2, 12};
    static const unsigned mib[3] = {2315, 640, 4890};
    static const char* const first[3] = {"Fire Emblem Echoes: Shadows of Valentia", "Mario Kart 7", "A-Train 3D..."};
    static const int channel[3] = {1, 6, 11};
    memset(out, 0, sizeof(*out));
    if(index < 0 || index >= 3) {
        return -1;
    }

    snprintf(out->host, sizeof(out->host), "%s", hosts[index]);
    out->titles = titles[index];
    out->bytes = (uint64_t) mib[index] * 1048576ULL;
    snprintf(out->first, sizeof(out->first), "%s", first[index]);
    out->channel = channel[index];
    out->receivers = index + 1;
    out->max_receivers = 7;
    return 0;
}

static int demo_send_run(dl_send_tick_fn tick, void* user) {
    const uint64_t t0 = ts_now_ms();
    for(;;) {
        const uint64_t t = ts_now_ms() - t0;
        dl_send_stats s;
        memset(&s, 0, sizeof(s));
        s.phase = t < 6000 ? 0 : 1;
        s.channel = 6;
        s.connected = t < 3000 ? 0 : 2;
        s.cur_title = -1;
        if(s.phase == 1) {
            const double f = (double) (t - 6000) / 60000.0;
            const double p = f > 1.0 ? 1.0 : f;
            s.next_blocks = (uint32_t) (p * gMan.totalBlocks);
            s.repairs = (uint64_t) (p * 900);
            s.sent_bytes = ((uint64_t) s.next_blocks + s.repairs) * 1456ULL;
            s.mibps = 1.62;
            s.cur_title = (int) (p * gMan.count) < gMan.count ? (int) (p * gMan.count) : gMan.count - 1;
            s.n_clients = 7;
            static const char* const nick[7] = {"AZAHAR", "Friend-A", "Friend-D 3DS", "Player-G", "Friend-B", "Console-A 2", "Player-H"};
            for(int c = 0; c < 7; c++) {
                snprintf(s.client[c].name, sizeof(s.client[c].name), "%s", nick[c]);
                s.client[c].free_mib = 4000 + 700 * (unsigned) c;
                s.client[c].eff_pct = 96 - 6 * c;
                s.client[c].down_bytes = (uint64_t) (p * (100 - 10 * c) * 1.6e6);
                s.client[c].inst_bytes = (uint64_t) (p * (80 - 15 * c) * 1.3e6);
                for(int i = 0; i < gMan.count; i++) {
                    dl_set_bit(s.client[c].want, i, 1);
                    s.client[c].t_down[i] = (uint8_t) (p * (100 - 10 * c));
                    s.client[c].t_inst[i] = (uint8_t) (p * (80 - 15 * c));
                }
                if(c >= 2) {
                    s.client[c].down = (int) (p * (90 - 10 * c));
                    s.client[c].inst = (int) (p * (60 - 7 * c));
                    // 2 waits for its turn, 3 just came in, 4 had errors, 5 is silent, 6 finished
                    static const int st_of[7] = {0, 0, 5, 4, 6, 3, 2};
                    s.client[c].state = st_of[c];
                    s.client[c].age_ds = c == 5 ? 123 : 4;
                }
            }
            s.client[0].down = (int) (p * 100);
            s.client[0].inst = (int) (p * 80);
            s.client[0].state = p >= 1.0 ? 2 : 0;
            s.client[1].down = (int) (p * 90);
            s.client[1].inst = (int) (p * 60);
            s.client[1].state = p > 0.5 ? 1 : 0;
            s.client[1].age_ds = t > 40000 && t < 46000 ? 70 : 3;
            if(s.client[1].age_ds >= 50) {
                s.client[1].state = 3;
            }
        }

        snprintf(s.msg, sizeof(s.msg), "%s", s.phase == 0 ? "the room is open" : "");
        if(tick(user, &s) == DL_CANCEL) {
            return 0;
        }

        svcSleepThread(250ULL * 1000000ULL);
    }
}

static int demo_recv_run(int index, dl_recv_tick_fn tick, void* user) {
    static const char* const names[4] = {"Mario Kart 7", "Super Mario 3D Land", "Animal Crossing New Leaf", "The Legend of Zelda Majora's Mask 3D"};
    static const int game_of[7] = {0, 0, 1, 1, 2, 2, 3};
    static const int kind_of[7] = {0, 1, 0, 2, 0, 1, 0};
    const uint64_t t0 = ts_now_ms();
    uint64_t chosen_at = 0;
    int final_ticks = 0;
    dl_recv_stats s;
    memset(&s, 0, sizeof(s));
    s.cur_title = -1;
    s.sender_hears = -1;
    s.eta_sec = -1;
    s.sender_eta_sec = -1;
    (void) index;
    for(;;) {
        const uint64_t t = ts_now_ms() - t0;
        if(t < 1500) {
            s.phase = 1;
            snprintf(s.msg, sizeof(s.msg), "connecting...");
        } else if(t < 3000 && !s.have_manifest) {
            s.phase = 2;
            snprintf(s.msg, sizeof(s.msg), "waiting for the list of titles...");
        } else if(!s.have_manifest) {
            s.have_manifest = 1;
            s.count = 7;
            for(int i = 0; i < s.count; i++) {
                s.title[i].tid = 0x0004000000030000ULL + (uint64_t) i * 0x100 + (kind_of[i] == 1 ? 0x0E00000000ULL : 0);
                s.title[i].kind = (uint8_t) kind_of[i];
                s.title[i].size = (uint64_t) (kind_of[i] == 0 ? 1180 : kind_of[i] == 1 ? 96 : 18) * 1048576ULL + (uint64_t) i * 1000;
                snprintf(s.title[i].name, sizeof(s.title[i].name), "%s", names[game_of[i]]);
                snprintf(s.title[i].code, sizeof(s.title[i].code), "CTR-P-ABC%c", 'A' + i);
                s.exists[i] = i == 1;
                s.old_size[i] = i == 1 ? 80ULL * 1048576ULL : 0;
                s.n3ds_only[i] = i == 6;
                dl_set_bit(s.want, i, !s.exists[i] && !s.n3ds_only[i]);
            }

            s.phase = 3;
            snprintf(s.msg, sizeof(s.msg), "choose what to receive");
        }

        if(s.phase == 3) {
            const int answer = tick(user, &s);
            if(answer == DL_CANCEL) {
                return 1;
            }

            if(answer == DL_CONFIRM) {
                s.phase = 5;
                chosen_at = ts_now_ms();
                snprintf(s.msg, sizeof(s.msg), "receiving");
                for(int i = 0; i < s.count; i++) {
                    if(dl_bit(s.want, i)) {
                        s.need_blocks += (uint32_t) (s.title[i].size / 1456);
                    }
                }
            }

            svcSleepThread(2ULL * 1000000ULL);
            continue;
        }

        s.channel = 11;
        s.free_mib = 24000;
        if(s.phase == 5 || s.phase == 6) {
            const double p = s.phase == 6 ? 1.0 : (double) (ts_now_ms() - chosen_at) / 50000.0;
            const double q = p > 1.0 ? 1.0 : p;
            s.dl_blocks = (uint32_t) (s.need_blocks * (q > 0.1 ? q : q * 1.1));
            s.inst_blocks = (uint32_t) (s.need_blocks * q * 0.9);
            s.mibps = 1.58;
            s.eta_sec = (1.0 - q) * 100;
            s.sender_eta_sec = (1.0 - q) * 120;
            s.sender_next = (uint32_t) (q * 70);
            s.sender_kibs = 1650;
            s.sender_hears = 1;
            s.data_age_ms = 12;
            s.am_last_ms = 40;
            s.am_max_ms = 900;
            s.recv = (uint64_t) (q * 400000);
            s.dups = (uint64_t) (q * 3000);
            s.out_of_window = 12;
            s.n_others = 6;
            for(int o = 0; o < 6; o++) {
                static const char* const nick[6] = {"Player-I", "Friend-D", "Friend-A", "Player-G", "Console-B", "Player-J"};
                s.other_cursor[o] = (uint32_t) (q * (60 - 8 * o));
                snprintf(s.other_name[o], sizeof(s.other_name[o]), "%s", nick[o]);
                s.other_state[o] = (uint8_t) (o == 2 ? 1 : (o == 4 ? 4 : (o == 1 ? 5 : (o == 3 ? 6 : (q > 0.9 && o == 0 ? 2 : 0)))));
            }
            {
                // (what the receiver's screen says in each case: receiving, waiting for others, the connection lost and found again)
                const uint64_t since = ts_now_ms() - chosen_at;
                snprintf(s.msg, sizeof(s.msg), "%s", "receiving");
                if(since > 12000 && since < 20000) {
                    snprintf(s.msg, sizeof(s.msg), "%s", "waiting: the sender is serving others");
                } else if(since >= 20000 && since < 30000) {
                    snprintf(s.msg, sizeof(s.msg), "connection lost: looking for the sender... (%u s)", (unsigned) ((since - 20000) / 1000));
                    s.data_age_ms = 6000;
                    s.reconnects = 1;
                } else if(since >= 30000 && since < 40000) {
                    snprintf(s.msg, sizeof(s.msg), "The room is full (7/7), still trying (%u s)", (unsigned) ((since - 30000) / 1000));
                    s.room_full = 1;
                    s.reconnects = 1;
                } else if(since >= 40000) {
                    s.reconnects = 1;
                    snprintf(s.msg, sizeof(s.msg), "%s", "all downloaded: installing");
                    s.dl_blocks = s.need_blocks;
                    s.data_age_ms = 30000;
                }
            }
            s.cur_title = -1;
            int seen = 0;
            for(int i = 0; i < s.count; i++) {
                if(!dl_bit(s.want, i)) {
                    s.result[i] = 2;
                    continue;
                }

                seen++;
                s.result[i] = q >= 0.2 * seen ? 1 : 0;
                if(s.result[i] == 0 && s.cur_title < 0) {
                    s.cur_title = i;
                    s.cur_inst_bytes = s.title[i].size * 37 / 100;
                }
            }

            if(q >= 1.0) {
                s.phase = 6;
                s.all_done = 1;
                s.cur_title = -1;
                s.eta_sec = 0;
                snprintf(s.msg, sizeof(s.msg), "all done");
            }
        }

        if(tick(user, &s) == DL_CANCEL) {
            return s.phase == 6 ? 0 : 1;
        }

        if(s.phase == 6 && ++final_ticks >= 3) {
            return 0; // (the module's loop ends after its last call, as this one does)
        }

        svcSleepThread(100ULL * 1000000ULL);
    }
}
