// TitleShare: trasferisce titoli installati (giochi, aggiornamenti, DLC) da un 3DS a uno o piu' altri 3DS
// via wireless locale (UDS), senza access point, senza PC e senza internet.

#include <3ds.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amsink.h"
#include "catalog.h"
#include "ctx.h"
#include "net.h"
#include "pace.h"
#include "power.h"
#include "receiver.h"
#include "sender.h"

static PrintConsole topC, botC;
static ts_ctx ctx;
static ts_catalog cat;

// ---------------------------------------------------------------- disegno
// La console di libctru scrive direttamente sullo schermo: cancellare e ridisegnare a ogni giro farebbe sfarfallare.
// Ogni schermata si compone in una pagina in memoria e si ridisegna solo se il testo e' cambiato.

typedef struct {
    char buf[4096];
    size_t len;
    char last[4096];
} Page;

static Page pgTop, pgBot;

static void pg_clear(Page* p) {
    p->len = 0;
    p->buf[0] = 0;
}

static void pg_printf(Page* p, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p->buf + p->len, sizeof(p->buf) - p->len, fmt, ap);
    va_end(ap);
    if(n > 0) {
        p->len += (size_t) n;
        if(p->len >= sizeof(p->buf)) {
            p->len = sizeof(p->buf) - 1;
        }
    }
}

#define TOP(...) pg_printf(&pgTop, __VA_ARGS__)
#define BOT(...) pg_printf(&pgBot, __VA_ARGS__)

// Disegna le pagine cambiate. Con fast = 0 aspetta anche il vblank (cicli dei menu); con fast = 1 non attende mai
// (durante il trasferimento non si deve fermare il ciclo radio).
static void flush_pages(int fast) {
    if(ts_power_skip_drawing()) {
        // schermi spenti o conchiglia chiusa: niente disegno. Fuori dal trasferimento si mantiene comunque il ritmo di un fotogramma.
        if(!fast) {
            svcSleepThread(16000000);
        }

        return;
    }

    if(strcmp(pgTop.buf, pgTop.last) != 0) {
        memcpy(pgTop.last, pgTop.buf, pgTop.len + 1);
        consoleSelect(&topC);
        consoleClear();
        fputs(pgTop.buf, stdout);
    }

    if(strcmp(pgBot.buf, pgBot.last) != 0) {
        memcpy(pgBot.last, pgBot.buf, pgBot.len + 1);
        consoleSelect(&botC);
        consoleClear();
        fputs(pgBot.buf, stdout);
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    if(!fast) {
        gspWaitForVBlank();
    }
}

// Barra da una percentuale intera (0-100): la barra e il numero stampato sono la stessa cosa, e la barra e' piena solo al 100%.
static void bar_pct(char* out, int pct, int width) {
    if(pct < 0) {
        pct = 0;
    }

    if(pct > 100) {
        pct = 100;
    }

    int fill = pct == 100 ? width : pct * width / 100;
    out[0] = '[';
    for(int i = 0; i < width; i++) {
        out[1 + i] = i < fill ? '#' : '.';
    }

    out[width + 1] = ']';
    out[width + 2] = 0;
}

// Percentuale di a su b: 100 solo se `done`, altrimenti al massimo 99 (a 100 si arriva quando e' davvero finito, non per arrotondamento).
static int pct_of(uint64_t a, uint64_t b, int done) {
    if(done) {
        return 100;
    }

    if(b == 0) {
        return 0;
    }

    uint64_t p = a * 100 / b;
    return p > 99 ? 99 : (int) p;
}

// Tempo restante in forma leggibile: "--:--" se non stimabile, "MM:SS" o "H:MM:SS".
static void fmt_eta(double sec, char* out, size_t size) {
    if(sec < 0 || sec > 99.0 * 3600.0) {
        snprintf(out, size, "--:--");
        return;
    }

    int s = (int) (sec + 0.5);
    if(s >= 3600) {
        snprintf(out, size, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(out, size, "%02d:%02d", s / 60, s % 60);
    }
}

static void wait_key(const char* text) {
    BOT("\n%s\n", text);
    flush_pages(0);
    while(aptMainLoop()) {
        hidScanInput();
        if(hidKeysDown() & (KEY_A | KEY_B | KEY_START)) {
            break;
        }

        flush_pages(0);
    }
}

// Tipo di titolo in poche lettere, es. "gioco", "aggiorn.", "DLC 02".
static void kind_text(uint8_t kind, u64 tid, char* out, size_t size) {
    if(kind == TS_KIND_APP) {
        snprintf(out, size, "gioco");
    } else if(kind == TS_KIND_UPDATE) {
        snprintf(out, size, "aggiorn.");
    } else if(kind == TS_KIND_DLC) {
        u32 low = (u32) (tid & 0xFF);
        if(low != 0) {
            snprintf(out, size, "DLC %02X", (unsigned int) low);
        } else {
            snprintf(out, size, "DLC");
        }
    } else {
        snprintf(out, size, "altro");
    }
}

// ---------------------------------------------------------------- invio

typedef struct {
    const ts_manifest* m;
} sender_ui_arg;

static int sender_ui(void* arg, const ts_sender_stats* st) {
    const ts_manifest* m = ((sender_ui_arg*) arg)->m;
    hidScanInput();
    u32 down = hidKeysDown();

    pg_clear(&pgTop);
    uint64_t totalBytes = 0;
    for(int i = 0; i < m->count; i++) {
        totalBytes += m->t[i].size;
    }

    static const char* titles[] = {"STANZA APERTA", "STANZA - trasferimento", "STANZA CHIUSA", "STANZA - INTERROTTA"};
    TOP("%s\n\n", titles[st->phase >= 0 && st->phase <= 3 ? st->phase : 3]);

    int shown = 0;
    const char* prev = "";
    for(int i = 0; i < m->count && shown < 10; i++) {
        if(strcmp(prev, m->t[i].name) != 0) {
            TOP(" %.46s\n", m->t[i].name);
            prev = m->t[i].name;
            shown++;
        }

        char k[16];
        kind_text(m->t[i].kind, m->t[i].tid, k, sizeof(k));
        TOP("    %-9s %8.1f MiB%s\n", k, (double) m->t[i].size / 1048576.0, m->t[i].hasSeed ? " (seed)" : "");
        shown++;
    }

    if(shown >= 10) {
        TOP(" ...\n");
    }

    TOP("\n Totale offerto: %d titoli, %.1f MiB\n", m->count, (double) totalBytes / 1048576.0);

    TOP("\n Collegati: %d   Registrati: %d (in linea %d, in attesa %d)\n Giro: %u   Titoli scelti dai riceventi: %d\n", st->connected, st->ready, st->nInline, st->nLate, (unsigned int) st->lap, st->wanted);
    if(st->phase == 0) {
        TOP("\n Passo di invio: %s   (Y = cambia)\n", pace_mode_name(sender_pace_mode()));
        TOP("\n I riceventi entrano quando vogliono.\n B = chiudi la stanza\n");
    }

    if(st->lap > 0) {
        // Completamento della stanza = installato totale / voluto totale dei riceventi registrati (non quanto il mittente ha inviato: dopo il primo
        // passaggio il mittente ha ancora ripetizioni e teste da mandare). 100% solo quando tutti hanno finito di installare.
        char b[48];
        bar_pct(b, st->pctRoom, 30);
        TOP("\n %s %3d%%  stanza completata\n", b, st->pctRoom);
        char eta[16];
        fmt_eta(st->etaSec, eta, sizeof(eta));
        TOP(" in invio: %.2f MiB/s   mancano (l'ultimo): %s\n", st->mibps, eta);
        TOP(" primo passaggio %d%%   ripetizioni: %" PRIu64 "\n", pct_of(st->next, st->total, st->next >= st->total && st->total > 0 && st->pctRoom >= 100), st->repairs);
        if(st->paceRate >= 50000.0) {
            TOP(" passo: libero (%s), perdita %.1f%%\n", pace_mode_name(st->paceMode), st->paceLossPct);
        } else {
            TOP(" passo: %.0f blocchi/s (%s), perdita %.1f%%\n", st->paceRate, pace_mode_name(st->paceMode), st->paceLossPct);
        }
        if(st->curTitle >= 0 && st->curTitle < m->count && st->phase == 1) {
            char k[16];
            kind_text(m->t[st->curTitle].kind, m->t[st->curTitle].tid, k, sizeof(k));
            TOP(" in invio: %.26s (%s)\n", m->t[st->curTitle].name, k);
        }

    }

    // Per ricevente: scaricato (ha i blocchi) e installato (consegnato ad AM), sui titoli che vuole lui. Stato: SCARICA (gli mancano blocchi), INSTALLA (ha tutto,
    // sta installando), FINITO (tutto installato), NON RISPONDE (nessun riscontro da piu' di 5 s), NUOVO (nessun riscontro ancora).
    for(int i = 0; i < st->nClients; i++) {
        char ce[16];
        fmt_eta(st->cEta[i], ce, sizeof(ce));
        static const char* names[] = {"SCARICA", "INSTALLA", "FINITO", "NON RISPONDE", "NUOVO"};
        int s = st->cstate[i] <= 4 ? st->cstate[i] : 0;
        if(s == 3) {
            TOP(" ricevente %d: giu' %3d%% inst %3d%% NON RISPONDE (%u s)\n", i + 1, st->pctDown[i], st->pctInst[i], (unsigned int) (st->ageDs[i] / 10));
        } else if(s == 2 || s == 4) {
            TOP(" ricevente %d: giu' %3d%% inst %3d%% %s\n", i + 1, st->pctDown[i], st->pctInst[i], names[s]);
        } else {
            TOP(" ricevente %d: giu' %3d%% inst %3d%% %s %s\n", i + 1, st->pctDown[i], st->pctInst[i], names[s], ce);
        }
    }

    if(st->msg[0]) {
        TOP("\n %s\n", st->msg);
    }

    TOP("\n SELECT = %s schermi   B = chiudi la stanza\n", ts_power_screens_off() ? "accendi" : "spegni");
    flush_pages(st->phase == 1);

    if(down & KEY_SELECT) {
        ts_power_toggle_screens();
    }

    if(st->phase == 0 && (down & KEY_Y)) {
        sender_pace_next();
    }

    if((down & KEY_B) && st->phase <= 1) {
        return TS_UI_CANCEL;
    }

    return TS_UI_NONE;
}

typedef struct {
    uint8_t* sel;  // titolo scelto
    uint8_t* chk;  // 0 = non controllato, 1 = inviabile, 2 = non inviabile
    ts_title* info;
    char note[100];
} SendSel;

static void select_item(SendSel* S, int idx, int on) {
    if(!on) {
        S->sel[idx] = 0;
        return;
    }

    if(S->chk[idx] == 2) {
        return;
    }

    if(S->chk[idx] == 0) {
        char label[64], e[80];
        catalog_label(&cat.items[idx], label, sizeof(label));
        pg_clear(&pgTop);
        TOP("Controllo %s ...\n", label);
        flush_pages(0);

        ts_title t;
        if(sender_check_title(&ctx, cat.items[idx].tid, &t, e, sizeof(e)) != 0) {
            S->chk[idx] = 2;
            snprintf(S->note, sizeof(S->note), "%.40s: non inviabile (%.40s)", label, e);
            return;
        }

        snprintf(t.name, sizeof(t.name), "%s", cat.items[idx].name);
        t.kind = cat.items[idx].kind;
        S->info[idx] = t;
        S->chk[idx] = 1;
    }

    S->sel[idx] = 1;
}

static void do_send(void) {
    if(!ctx.haveKeys || cat.nItems == 0) {
        pg_clear(&pgBot);
        BOT("Per inviare servono boot9.bin e\nmovable.sed leggibili.\n");
        wait_key("Premi un tasto");
        return;
    }

    SendSel S;
    memset(&S, 0, sizeof(S));
    S.sel = calloc((size_t) cat.nItems, 1);
    S.chk = calloc((size_t) cat.nItems, 1);
    S.info = calloc((size_t) cat.nItems, sizeof(ts_title));
    if(S.sel == NULL || S.chk == NULL || S.info == NULL) {
        free(S.sel);
        free(S.chk);
        free(S.info);
        return;
    }

    int cur = 0, detail = 0, dcur = 0, go = 0;
    while(aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        ts_game* gm = &cat.games[cur];

        if(down & KEY_B) {
            if(detail) {
                detail = 0;
            } else {
                break;
            }
        }

        if(detail) {
            if((down & KEY_DOWN) && dcur + 1 < gm->nMembers) {
                dcur++;
            }

            if((down & KEY_UP) && dcur > 0) {
                dcur--;
            }

            if(down & KEY_A) {
                int idx = gm->members[dcur];
                select_item(&S, idx, !S.sel[idx]);
            }
        } else {
            if((down & KEY_DOWN) && cur + 1 < cat.nGames) {
                cur++;
            }

            if((down & KEY_UP) && cur > 0) {
                cur--;
            }

            if(down & KEY_RIGHT) {
                cur = cur + 14 < cat.nGames ? cur + 14 : cat.nGames - 1;
            }

            if(down & KEY_LEFT) {
                cur = cur >= 14 ? cur - 14 : 0;
            }

            if(down & KEY_A) {
                int any = 0;
                for(int k = 0; k < gm->nMembers; k++) {
                    any |= S.sel[gm->members[k]];
                }

                for(int k = 0; k < gm->nMembers; k++) {
                    select_item(&S, gm->members[k], !any);
                }
            }

            if(down & KEY_X) {
                detail = 1;
                dcur = 0;
            }

            if(down & KEY_Y) {
                go = 1;
                break;
            }
        }

        int count = 0;
        uint64_t bytes = 0;
        for(int i = 0; i < cat.nItems; i++) {
            if(S.sel[i]) {
                count++;
                bytes += S.info[i].size;
            }
        }

        pg_clear(&pgTop);
        TOP("INVIA - scegli cosa mandare\n\n");
        TOP(" Scelti: %d titoli, %.1f MiB\n", count, (double) bytes / 1048576.0);
        if(count > TS_MAX_TITLES) {
            TOP(" (troppi: il massimo e' %d)\n", TS_MAX_TITLES);
        }

        TOP("\n SU/GIU muove, SX/DX salta di pagina\n");
        if(detail) {
            TOP(" A = scegli / togli questo titolo\n B = torna all'elenco\n");
        } else {
            TOP(" A = scegli / togli tutto il gioco\n X = dettaglio (gioco, aggiorn., DLC)\n Y = continua   B = indietro\n");
        }

        if(S.note[0]) {
            TOP("\n %s\n", S.note);
        }

        pg_clear(&pgBot);
        if(detail) {
            BOT("%.38s\n", gm->name);
            for(int k = 0; k < gm->nMembers; k++) {
                int idx = gm->members[k];
                char kt[16];
                kind_text(cat.items[idx].kind, cat.items[idx].tid, kt, sizeof(kt));
                BOT("%c[%c] %-9s %-18.18s %s\n", k == dcur ? '>' : ' ', S.sel[idx] ? 'x' : ' ', kt, cat.items[idx].code, S.chk[idx] == 2 ? "illeggibile" : "");
            }
        } else {
            int first = (cur / 28) * 28;
            for(int g = first; g < cat.nGames && g < first + 28; g++) {
                int n = 0, s = 0;
                for(int k = 0; k < cat.games[g].nMembers; k++) {
                    n++;
                    s += S.sel[cat.games[g].members[k]];
                }

                BOT("%c[%c] %-31.31s %2d\n", g == cur ? '>' : ' ', s == 0 ? ' ' : (s == n ? 'x' : '~'), cat.games[g].name, n);
            }
        }

        flush_pages(0);
    }

    if(go) {
        static ts_title list[TS_MAX_TITLES];
        int cnt = 0, over = 0;
        for(int g = 0; g < cat.nGames; g++) {
            for(int k = 0; k < cat.games[g].nMembers; k++) {
                int idx = cat.games[g].members[k];
                if(S.sel[idx]) {
                    if(cnt < TS_MAX_TITLES) {
                        list[cnt++] = S.info[idx];
                    } else {
                        over++;
                    }
                }
            }
        }

        static ts_manifest m;
        if(over > 0) {
            // niente invii a meta': i titoli oltre il massimo non entrerebbero nel manifesto
            pg_clear(&pgBot);
            BOT("Troppi titoli: il massimo e' %d (ne hai\nscelti %d). Togline qualcuno.\n", TS_MAX_TITLES, cnt + over);
            wait_key("Premi un tasto");
        } else if(cnt == 0 || sender_make_manifest(list, cnt, &m) != 0) {
            pg_clear(&pgBot);
            BOT("Nessun titolo scelto.\n");
            wait_key("Premi un tasto");
        } else {
            pg_clear(&pgBot);
            BOT("Stanza aperta: sugli altri DS scegli\n\"Ricevi\", quando vogliono.\n");
            sender_ui_arg arg = {&m};
            int rc = sender_run(&ctx, &m, sender_ui, &arg);
            wait_key(rc == 0 ? "Stanza chiusa. Premi un tasto" : "Interrotto. Premi un tasto");
        }
    }

    free(S.sel);
    free(S.chk);
    free(S.info);
}

// ---------------------------------------------------------------- ricezione

static int rcur, rtop;

static const char* res_text(int r) {
    switch(r) {
        case TS_RES_OK:
            return "installato";
        case TS_RES_SKIPPED:
            return "non richiesto";
        case TS_RES_ERROR:
            return "ERRORE";
        default:
            return "in attesa";
    }
}

static int recv_ui(void* arg, ts_recv_stats* st) {
    (void) arg;
    hidScanInput();
    u32 down = hidKeysDown();
    int action = TS_RUI_NONE;
    if(down & KEY_SELECT) {
        ts_power_toggle_screens();
    }

    if(st->phase == TS_R_CHOOSE && st->haveManifest) {
        const ts_manifest* m = &st->m;
        if((down & KEY_DOWN) && rcur + 1 < m->count) {
            rcur++;
        }

        if((down & KEY_UP) && rcur > 0) {
            rcur--;
        }

        if(down & KEY_RIGHT) {
            rcur = rcur + 14 < m->count ? rcur + 14 : m->count - 1;
        }

        if(down & KEY_LEFT) {
            rcur = rcur >= 14 ? rcur - 14 : 0;
        }

        if(down & KEY_A) {
            ts_set_bit(st->want, rcur, !ts_bit(st->want, rcur));
        }

        if(down & KEY_X) {
            // tutto il gioco del titolo sotto il cursore: se uno e' scelto li toglie tutti, altrimenti li sceglie tutti
            int a = rcur, b = rcur;
            while(a > 0 && strcmp(m->t[a - 1].name, m->t[rcur].name) == 0) {
                a--;
            }

            while(b + 1 < m->count && strcmp(m->t[b + 1].name, m->t[rcur].name) == 0) {
                b++;
            }

            int any = 0;
            for(int i = a; i <= b; i++) {
                any |= ts_bit(st->want, i);
            }

            for(int i = a; i <= b; i++) {
                ts_set_bit(st->want, i, !any);
            }
        }

        if(down & KEY_Y) {
            // senza nessun titolo scelto "comincia" non fa nulla di utile (14.10: la ricezione finiva in 4 s con "tutto fatto"): si resta nella scelta
            int any = 0;
            for(int i = 0; i < m->count; i++) {
                any |= ts_bit(st->want, i);
            }

            if(any) {
                action = TS_RUI_CONFIRM;
            }
        }
    }

    // ---- schermo superiore: stato
    pg_clear(&pgTop);
    TOP("RICEVI\n\n");
    const ts_manifest* m = &st->m;

    if(st->phase == TS_R_CHOOSE) {
        int n = 0;
        uint64_t bytes = 0;
        for(int i = 0; i < m->count; i++) {
            if(ts_bit(st->want, i)) {
                n++;
                bytes += m->t[i].size;
            }
        }

        TOP(" Offerti: %d titoli\n Scelti: %d, %.1f MiB\n", m->count, n, (double) bytes / 1048576.0);
        if(n == 0) {
            TOP(" ** NIENTE SCELTO: A sceglie/toglie, X tutto il gioco, poi Y **\n");
        }
        {
            // spazio sulla SD (riletto ogni 3 s): minimo = tutti i titoli installati + margine; comodo = anche il piu' piccolo in parcheggio
            static uint64_t freeB, freeAt;
            uint64_t nowMs = svcGetSystemTick() * 1000ULL / SYSCLOCK_ARM11;
            if(freeAt == 0 || nowMs - freeAt > 3000) {
                FS_ArchiveResource ar;
                memset(&ar, 0, sizeof(ar));
                freeB = R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&ar)) ? (uint64_t) ar.freeClusters * ar.clusterSize : 0;
                freeAt = nowMs;
            }

            uint64_t smallest = 0, reclaim = 0, largestNet = 0;
            for(int i = 0; i < m->count; i++) {
                if(!ts_bit(st->want, i)) {
                    continue;
                }

                if(smallest == 0 || m->t[i].size < smallest) {
                    smallest = m->t[i].size;
                }

                // sovrascrivere un titolo gia' installato lo cancella prima: lo spazio vecchio si libera quando comincia quell'installazione
                uint64_t old = st->exists[i] ? st->oldSize[i] : 0; // oldSize e' 0 per i titoli che vanno sulla NAND
                reclaim += old < m->t[i].size ? old : m->t[i].size;
                uint64_t net = m->t[i].size - (old < m->t[i].size ? old : m->t[i].size);
                if(net > largestNet) {
                    largestNet = net;
                }
            }

            const uint64_t margin = 64ULL << 20;
            // minimo: installazioni in fila dal flusso, a ogni titolo si libera il vecchio; comodo: tutto in parcheggio (i vecchi si liberano solo man mano)
            uint64_t netTot = bytes - reclaim;
            uint64_t need = n ? (netTot > largestNet ? netTot : largestNet) + margin : 0;
            uint64_t comfy = n ? bytes + smallest + margin : 0;
            TOP(" SD libera %.2f GiB: servono almeno %.2f GiB\n (%.2f GiB per parcheggiare tutto)\n", (double) freeB / 1073741824.0, (double) need / 1073741824.0, (double) comfy / 1073741824.0);
            if(reclaim > 0) {
                TOP(" sovrascrivendo se ne liberano %.2f GiB\n", (double) reclaim / 1073741824.0);
            }

            if(n && freeB < need) {
                TOP(" ATTENZIONE: spazio insufficiente per tutti\n");
            } else if(n && freeB < comfy) {
                TOP(" poco spazio: si installa dal flusso, senza\n parcheggio completo (puo' essere piu' lento)\n");
            }
        }
        TOP("\n SU/GIU muove, SX/DX salta di pagina\n A = scegli / togli il titolo\n X = tutto il gioco\n Y = conferma   B = annulla\n");
        TOP("\n [*] = gia' installato qui. Sceglierlo lo\n SOVRASCRIVE: cancella prima il titolo e\n i suoi dati.\n [N3DS] = solo per New 3DS (su un Old 3DS\n lo scegli tu, come in FBI).\n");
    } else if(st->phase >= TS_R_WAIT_START) {
        int shown = 0;
        for(int i = 0; i < m->count && shown < 13; i++) {
            if(!ts_bit(st->want, i)) {
                continue;
            }

            char k[16];
            kind_text(m->t[i].kind, m->t[i].tid, k, sizeof(k));
            char mark = (st->phase == TS_R_RECEIVE && st->curTitle == i && st->result[i] == TS_RES_PENDING) ? '>' : ' ';
            TOP("%c%.24s %s %.1fM %s\n", mark, m->t[i].name, k, (double) m->t[i].size / 1048576.0, st->phase == TS_R_WAIT_START ? "" : res_text(st->result[i]));
            if(st->result[i] == TS_RES_ERROR) {
                TOP("    errore 0x%08" PRIx32 "\n", st->err[i]);
            }

            shown++;
        }

        if(st->phase == TS_R_RECEIVE || st->phase == TS_R_DONE || st->phase == TS_R_ERROR) {
            // scaricato = blocchi che ho gia' (finestra, coda, file, installati); installato = consegnati ad AM. 100% solo a tutto fatto (nessun arrotondamento).
            char b[48];
            int pDown = pct_of(st->dlBlocks, st->needBlocks, st->needBlocks > 0 && st->dlBlocks >= st->needBlocks);
            int pInst = pct_of(st->instBlocks, st->needBlocks, st->allDone);
            bar_pct(b, pInst, 30);
            TOP("\n %s %3d%%\n", b, pInst);
            char eta[16], seta[16];
            fmt_eta(st->etaSec, eta, sizeof(eta));
            fmt_eta(st->senderEtaSec, seta, sizeof(seta));
            {
                char b2[48];
                bar_pct(b2, pDown, 22);
                TOP(" scaricato  %s %3d%%\n", b2, pDown);
                bar_pct(b2, pInst, 22);
                TOP(" installato %s %3d%%\n", b2, pInst);
            }

            TOP(" installazione %.2f MiB/s, mancano %s\n", st->mibps, eta);
            if(st->phase == TS_R_RECEIVE) {
                TOP(" stanza: %3u%% completata, mittente %.2f MiB/s, mancano (l'ultimo) %s\n", (unsigned int) st->senderNext, (double) st->senderKiBs / 1024.0, seta);
                if(st->senderHears == 0) {
                    TOP(" ATTENZIONE: il mittente non mi sente\n");
                }

                for(int i = 0; i < st->nOthers; i++) {
                    TOP(" altro ricevente %d: installato %3u%%\n", i + 1, (unsigned int) st->otherCursor[i]);
                }

                TOP(" ultimo dato %u ms fa   AM: %u ms (max %u)\n", (unsigned int) st->dataAgeMs, (unsigned int) st->amLastMs, (unsigned int) st->amMaxMs);
            }
            if(st->reconnects > 0) {
                TOP(" collegamento perso %d volte\n", st->reconnects);
            }
            TOP(" ricevuti %" PRIu64 "  doppi %" PRIu64 "  fuori finestra %" PRIu64 "\n", st->recv, st->dups, st->outOfWindow);
            TOP(" SELECT = %s schermi   B = annulla\n", ts_power_screens_off() ? "accendi" : "spegni");
        }
    }

    if(st->msg[0]) {
        TOP("\n %s\n", st->msg);
    }

    // ---- schermo inferiore: elenco da scegliere
    pg_clear(&pgBot);
    if(st->phase == TS_R_CHOOSE && st->haveManifest) {
        // righe: intestazione di gioco quando cambia il nome, poi un titolo per riga
        int rows = 0, curRow = 0;
        static int rowTitle[TS_MAX_TITLES * 2];
        const char* prev = "";
        for(int i = 0; i < m->count; i++) {
            if(strcmp(prev, m->t[i].name) != 0) {
                rowTitle[rows++] = -1 - i; // intestazione, legata al titolo i
                prev = m->t[i].name;
            }

            if(i == rcur) {
                curRow = rows;
            }

            rowTitle[rows++] = i;
        }

        if(curRow < rtop + 1) {
            rtop = curRow - 1 > 0 ? curRow - 1 : 0;
        }

        if(curRow >= rtop + 28) {
            rtop = curRow - 27;
        }

        for(int r = rtop; r < rows && r < rtop + 28; r++) {
            if(rowTitle[r] < 0) {
                BOT("%.39s\n", m->t[-1 - rowTitle[r]].name);
            } else {
                int i = rowTitle[r];
                char k[16];
                kind_text(m->t[i].kind, m->t[i].tid, k, sizeof(k));
                BOT("%c[%c] %-9s %7.1fM %s\n", i == rcur ? '>' : ' ', ts_bit(st->want, i) ? 'x' : ' ', k, (double) m->t[i].size / 1048576.0, st->exists[i] ? "[*]" : (st->n3dsOnly[i] ? "[N3DS]" : ""));
            }
        }
    }

    flush_pages(st->phase == TS_R_RECEIVE);

    if((down & KEY_B) && st->phase != TS_R_DONE && st->phase != TS_R_ERROR) {
        return TS_RUI_CANCEL;
    }

    return action;
}

static void do_receive(void) {
    udsNetworkScanInfo* nets = NULL;
    size_t total = 0;
    u32 cur = 0;
    int chosen = -1;
    uint64_t lastScan = 0;

    // HOME disattivato nella scelta dei mittenti: la scansione UDS e' una chiamata sincrona a NWM e il 14.10 premere HOME qui ha bloccato la console (stesso sintomo del
    // 12.10 dopo un annullamento); durante la ricezione e il trasferimento HOME e' gia' disattivato. Si esce con B.
    aptSetHomeAllowed(false);
    while(aptMainLoop() && chosen < 0) {
        hidScanInput();
        u32 down = hidKeysDown();
        if(down & KEY_B) {
            break;
        }

        if(ts_now_ms() - lastScan > 1500) {
            pg_clear(&pgTop);
            TOP("RICEVI - cerco i mittenti...\n\n B = indietro\n");
            flush_pages(0);
            free(nets);
            nets = NULL;
            total = 0;
            receiver_scan(&nets, &total);
            lastScan = ts_now_ms();
            if(cur >= total) {
                cur = 0;
            }
        }

        if((down & KEY_DOWN) && cur + 1 < total) {
            cur++;
        }

        if((down & KEY_UP) && cur > 0) {
            cur--;
        }

        if((down & KEY_A) && total > 0) {
            chosen = (int) cur;
            break;
        }

        pg_clear(&pgTop);
        TOP("RICEVI - mittenti trovati: %u\n\n", (unsigned int) total);
        for(size_t i = 0; i < total; i++) {
            char host[32], first[16];
            uint8_t titles;
            uint64_t bytes;
            receiver_describe(&nets[i], host, sizeof(host), &titles, &bytes, first);
            TOP("%c %s: %u titoli, %.0f MiB (%s...)\n", i == cur ? '>' : ' ', host[0] ? host : "?", titles, (double) bytes / 1048576.0, first);
        }

        TOP("\n SU/GIU sceglie, A = collegati, B = indietro (HOME qui e' disattivato)\n");
        flush_pages(0);
    }

    aptSetHomeAllowed(true);
    if(chosen >= 0) {
        rcur = 0;
        rtop = 0;
        int rc = receiver_run(&ctx, &nets[chosen], recv_ui, NULL);
        pg_clear(&pgBot);
        wait_key(rc == 0 ? "Fatto. Premi un tasto" : "Terminato. Premi un tasto");
    }

    free(nets);
}

// ---------------------------------------------------------------- elenco dei titoli installati

static void do_browse(void) {
    int cur = 0;
    while(aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if(down & KEY_B) {
            break;
        }

        if((down & KEY_DOWN) && cur + 1 < cat.nGames) {
            cur++;
        }

        if((down & KEY_UP) && cur > 0) {
            cur--;
        }

        if(down & KEY_RIGHT) {
            cur = cur + 14 < cat.nGames ? cur + 14 : cat.nGames - 1;
        }

        if(down & KEY_LEFT) {
            cur = cur >= 14 ? cur - 14 : 0;
        }

        pg_clear(&pgTop);
        TOP("TITOLI INSTALLATI\n\n %d giochi/titoli, %d voci in totale\n\n", cat.nGames, cat.nItems);
        TOP(" G = gioco  U = aggiornamento  D = DLC\n\n SU/GIU muove, SX/DX salta di pagina\n B = indietro\n");
        if(cat.nGames > 0) {
            const ts_game* g = &cat.games[cur];
            TOP("\n %.46s\n", g->name);
            for(int k = 0; k < g->nMembers; k++) {
                const ts_item* it = &cat.items[g->members[k]];
                char kt[16];
                kind_text(it->kind, it->tid, kt, sizeof(kt));
                TOP("   %-9s %s\n", kt, it->code);
            }
        }

        pg_clear(&pgBot);
        int first = (cur / 28) * 28;
        for(int g = first; g < cat.nGames && g < first + 28; g++) {
            int a = 0, u = 0, d = 0;
            for(int k = 0; k < cat.games[g].nMembers; k++) {
                uint8_t kind = cat.items[cat.games[g].members[k]].kind;
                a += kind == TS_KIND_APP;
                u += kind == TS_KIND_UPDATE;
                d += kind == TS_KIND_DLC;
            }

            BOT("%c%-30.30s %s%s%s\n", g == cur ? '>' : ' ', cat.games[g].name, a ? "G" : "-", u ? "U" : "-", d ? "D" : "-");
        }

        flush_pages(0);
    }
}

// ---------------------------------------------------------------- menu

static void names_progress(int done, int total) {
    pg_clear(&pgTop);
    TOP("3DS Link - Title delivery\n\nLeggo i nomi dei titoli... %d / %d\n", done, total);
    flush_pages(1);
}

// Memoria. Di base (TS_LINEAR_HEAP_MIB = 0) si lascia la divisione di libctru: lineare 32 MiB, il resto heap (~90 MiB su New 3DS).
// ATTENZIONE: il kernel limita lo heap del processo a 0x08000000..0x0E000000 = 96 MiB. Con una lineare piccola libctru darebbe allo
// heap tutto il resto (~114 MiB su New 3DS), la svcControlMemory fallisce e l'app muore all'avvio con svcBreak in __system_allocateHeaps
// (e' successo con 8 MiB, il 2026-10-11, su entrambe le console). Quindi un valore diverso da 0 va scelto con heap = totale - lineare <= 96 MiB
// (su New 3DS lineare >= 27 MiB, e il guadagno sarebbe di soli ~6 MiB). La dimensione si decide in compilazione (make LINEAR_MIB=28),
// prima di main, e il registro dice quanta memoria e' stata data (`lineare totale`).
#ifndef TS_LINEAR_HEAP_MIB
#define TS_LINEAR_HEAP_MIB 0
#endif
#if TS_LINEAR_HEAP_MIB > 0 && TS_LINEAR_HEAP_MIB < 27
#error "LINEAR_MIB troppo piccolo: lo heap supererebbe i 96 MiB consentiti dal kernel (crash all'avvio); usa 0 oppure almeno 27"
#endif
#if TS_LINEAR_HEAP_MIB > 0
u32 __ctru_linear_heap_size = (u32) TS_LINEAR_HEAP_MIB * 1024 * 1024;
#endif

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, &topC);
    consoleInit(GFX_BOTTOM, &botC);
    amInit();

    char msg[128];
    pg_clear(&pgTop);
    TOP("3DS Link - Title delivery\n\nPreparazione...\n");
    flush_pages(0);
    ctx_init(&ctx, msg, sizeof(msg));
    if(ctx.isNew) {
        osSetSpeedupEnable(true);
    }

    catalog_build(&ctx, &cat, names_progress);

    Result ur = udsInit(TS_SHARED_MEM, NULL);
    int udsOk = R_SUCCEEDED(ur);

    static const char* entries[] = {"Invia giochi", "Ricevi giochi", "Titoli installati"};
    int menu = 0;
    while(aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if(down & KEY_START) {
            break;
        }

        if((down & KEY_DOWN) && menu < 2) {
            menu++;
        }

        if((down & KEY_UP) && menu > 0) {
            menu--;
        }

        if(down & KEY_A) {
            if(menu == 2) {
                do_browse();
            } else if(!udsOk) {
                snprintf(msg, sizeof(msg), "Wi-Fi spento? (udsInit 0x%08" PRIx32 ")", (uint32_t) ur);
            } else if(menu == 0) {
                do_send();
            } else {
                do_receive();
            }
        }

        pg_clear(&pgTop);
        TOP("3DS Link - Title delivery\n\n");
        TOP(" Trasferisce giochi, aggiornamenti e DLC\n da un 3DS ad altri via wireless locale,\n senza internet.\n\n");
        TOP(" Console: %s\n", ctx.isNew ? "New 3DS" : "Old 3DS");
        TOP(" Titoli sulla SD: %u (%d giochi)\n", (unsigned int) ctx.titleCount, cat.nGames);
        TOP(" Chiavi: %s\n", ctx.haveKeys ? "ok" : "mancanti (solo ricezione)");
        TOP(" Nomi letti: %d su %d", cat.nNamed, cat.nTried);
        if(cat.usedNcch > 0) {
            TOP(" (%d dalle icone con le chiavi)", cat.usedNcch);
        }

        TOP("\n");
        if(cat.cacheHits + cat.cacheMisses > 0) {
            TOP(" Dalla cache %d, riletti %d (catalog.cache)\n", cat.cacheHits, cat.cacheMisses);
        }

        if(cat.nNamed < cat.nTried) {
            if(!ctx.haveKeys) {
                TOP(" Per leggere i nomi serve boot9.bin\n (3DS Link: Installation check)\n");
            } else if(cat.fsError != 0) {
                TOP(" (alcuni titoli senza icona; sistema: 0x%08" PRIx32 ")\n", (uint32_t) cat.fsError);
            }
        }
        if(msg[0]) {
            TOP("\n %s\n", msg);
        }

        pg_clear(&pgBot);
        BOT("\n");
        for(int i = 0; i < 3; i++) {
            BOT("  %c %s\n", menu == i ? '>' : ' ', entries[i]);
        }

        BOT("\n  START = esci\n");
        flush_pages(0);
    }

    if(udsOk) {
        udsExit();
    }

    catalog_free(&cat);
    ctx_exit(&ctx);
    ts_power_exit();
    amExit();
    gfxExit();
    return 0;
}
