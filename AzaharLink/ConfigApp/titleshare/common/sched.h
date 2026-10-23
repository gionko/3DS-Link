#pragma once
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

// Schedulatore del mittente (carosello, PROGETTO-CAROSELLO.md §2.5): decide quale blocco inviare ora in base a cio' che ogni ricevente ha
// dichiarato di avere ancora da ricevere. Nessun controllo di flusso a finestra: non aspetta mai nessuno, se nessuno ha bisogno di niente tace.
// Codice senza dipendenze dalla console (lo usano il mittente e le prove su PC).

#define TS_S_MAX 8              // riceventi registrati (stanza da 8 nodi: 7 riceventi + il mittente; un posto di margine per un rientro)
#define TS_TINY_BLOCKS 7200     // rimasuglio: a un ricevente mancano al massimo questi blocchi (~10 MiB, ~5 s) per chiudere un titolo
#define TS_DWELL_MS 3000        // un altro titolo con piu' riceventi deve restare in testa questo tempo prima di cambiare
#define TS_S_LAG_MS 5000        // chi non si fa sentire da tanto non conta fra i riceventi serviti
#define TS_S_LOST_MS 30000      // chi non si fa sentire da tanto esce dalla stanza
#define TS_S_UNACKED_MS 8000    // chi ha mandato solo READY e nessun riscontro conta solo per questo tempo
#define TS_S_INFLIGHT_MS 500    // un blocco inviato da meno di cosi' non si rimanda (il riscontro non puo' ancora dire se e' arrivato)
#define TS_S_TURN 4             // un blocco ogni TS_S_TURN va al ricevente che ha avuto meno (nessuna inedia)
#define TS_S_RECENT_SLOTS 16384 // tabella dei blocchi inviati da poco (ad accesso diretto)
#define TS_S_DECIDE_MS 25       // ogni quanto si riconsidera il titolo (o subito a ogni READY/riscontro/addio)
#define TS_LEAD_LOW 3000        // all'installatore restano meno di questi blocchi pronti in ordine: e' a secco (~2 s di AM)
#define TS_URGENT_BEHIND 4000   // il blocco che gli serve e' piu' indietro di tanto rispetto al punto piu' avanzato inviato: non e' un blocco della marcia
#define TS_URGENT_RUN 3         // blocchi urgenti consecutivi al massimo: poi uno ai riceventi della marcia (3 = urgenza ai 3/4, come in C2: meno frame inviati; con 2 la simulazione ne invia l'8% in piu', con 1 il doppio)
#define TS_EFF_MIN 500          // efficienza minima (per mille) perche' un ricevente abbia il turno intero: sotto, ne ha uno ogni quattro
#define TS_FLAKY_GAP_MS 1200    // un silenzio dei riscontri cosi' lungo (con il ricevente vivo e che poi riprende) e' un'interruzione della linea
#define TS_FLAKY_MS 20000       // dopo un'interruzione la linea e' sospetta per tanto: urgenza e turno ridotti (i frame spesi per lui rischiano di perdersi di nuovo)
#define TS_FRESH_MS 1000        // urgenza e turno solo a chi ha mandato un riscontro da meno di cosi' (chi e' sparito non si insegue)
#define TS_S_RETRY_MS 15        // dopo "niente da inviare" a un ricevente non si ricerca per questo tempo

typedef struct {
    uint8_t used;
    uint16_t token;
    uint16_t ident;                   // identificativo stabile della console (0 = non noto)
    uint8_t want[TS_BITMAP_BYTES];    // titoli voluti
    uint32_t freeMiB;                 // spazio libero dichiarato (0xFFFFFFFF = ignoto)
    uint32_t win;                     // finestra del ricevente in blocchi (0 = ignota): del titolo che sta installando si possono inviare solo i blocchi in [inst, inst + win)
    uint32_t enteredAt, lastHeard;
    uint8_t acked;                    // 1 dopo il primo ACK2
    uint32_t remain[TS_MAX_TITLES];   // blocchi che gli mancano (non installati e non disponibili) per titolo
    uint32_t first[TS_MAX_TITLES];    // primo blocco locale che non ha
    uint8_t flags[TS_MAX_TITLES];     // TS_SF_*
    uint32_t inst[TS_MAX_TITLES];     // blocchi gia' consegnati all'installazione
    uint32_t urgServed;               // scelte urgenti fatte per lui (diagnostica)
    uint32_t flakyUntil;              // fino a quando la linea del ricevente si considera instabile (dopo un silenzio dei riscontri): efficienza ridotta
    uint32_t eff;                     // efficienza recente (per mille): cio' che gli manca in meno rispetto ai blocchi inviati per lui
    uint8_t effT;
    uint32_t effAt, effRemain, effNeed;
    uint8_t focus;                    // titolo a cui si riferiscono gli intervalli
    int nRanges;
    ts_range r[TS_ACK2_MAX_RANGES];   // blocchi locali mancanti del titolo focus, in ordine crescente, senza sovrapposizioni
    uint32_t installed;
    uint32_t instSampleMs, instSample; // ultimo campione di `installed` (per la velocita' di installazione)
    double instRate;                   // blocchi installati al secondo (media mobile ~20 s), 0 = non ancora misurata
    uint32_t served;                  // blocchi inviati per lui, cioe' che erano il suo prossimo blocco (per il turno)
    uint32_t needSent;                // tutti i blocchi inviati che gli mancavano (anche condivisi): serve a misurare l'efficienza
    uint8_t hintTitle;
    uint32_t hint;                    // prossimo blocco locale da cui cercare (0xFFFFFFFF = dall'inizio degli intervalli)
    uint32_t sweepAt;                 // quando il suggerimento si azzera (per ritrovare i blocchi persi dietro la scansione)
    uint32_t retryAt;                 // fino a quando non si cerca di nuovo dopo un "niente da inviare"
} ts_sclient;

typedef struct {
    uint32_t picks, turnPicks, urgentPicks, idle, switches, tinySwitches, dwellSwitches;
} ts_sched_stats;

typedef struct ts_sched {
    const ts_manifest* m;
    ts_sclient cl[TS_S_MAX];
    int cur;                 // titolo in corso (-1 = nessuno)
    uint16_t gen;            // cresce a ogni cambio di titolo
    int betterTitle;         // titolo che ha piu' riceventi di quello in corso, da quando
    uint32_t betterSince;
    uint32_t decideAt;
    int dirty;
    uint32_t turn;
    int urgentRun;
    uint32_t fr[TS_MAX_TITLES]; // punto piu' avanzato inviato per titolo (blocco locale + 1)
    uint16_t byeTok[4];
    uint32_t byeAt[4];
    uint16_t byeIdent[4];
    int byeNext;
    ts_sched_stats st;
    uint32_t* recentG;       // g + 1 (0 = vuoto)
    uint32_t* recentMs;
} ts_sched;

ts_sched* sched_create(const ts_manifest* m);
void sched_destroy(ts_sched* S);

// READY (ripetuto ogni secondo dal ricevente). Ritorna 1 se e' nuovo, 0 se noto, -1 se non valido o stanza piena, -2 se e' un READY in ritardo
// di chi ha appena salutato. Se una voce della stessa console (stesso ident) con un altro token tace da piu' di 2 s la sostituisce (token in *replaced).
// Prima del primo riscontro si assume che gli manchi tutto cio' che vuole.
int sched_ready(ts_sched* S, uint16_t token, uint16_t ident, const uint8_t* bits, uint32_t freeMiB, uint32_t now, uint16_t* replaced);
// Finestra del ricevente (blocchi), dichiarata nel READY.
void sched_set_window(ts_sched* S, uint16_t token, uint32_t blocks);
// Riscontro. 1 se accettato (il ricevente deve essere registrato).
int sched_ack(ts_sched* S, uint16_t token, uint32_t now, const ts_ack2* a);
// Uscita pulita. 1 se lo conosceva.
int sched_bye(ts_sched* S, uint16_t token, uint32_t now);
// Toglie chi non si fa sentire da TS_S_LOST_MS. Ritorna quanti (i token in `dropped`, al massimo max).
int sched_expire(ts_sched* S, uint32_t now, uint16_t* dropped, int max);

// Prossimo blocco globale da inviare, o 0 se non serve a nessuno in questo momento. Il blocco si considera inviato (non si ripropone per
// TS_S_INFLIGHT_MS).
int sched_next(ts_sched* S, uint32_t now, uint32_t* g);
// Il blocco scelto da sched_next non e' stato inviato davvero (lettura fallita): lo si ripropone senza aspettare TS_S_INFLIGHT_MS.
void sched_unsend(ts_sched* S, uint32_t g);
// SOLA LETTURA (strumentazione): quanti riceventi vivi hanno ancora bisogno del blocco globale g (stesso controllo "mancante" di sched_next, sugli slot vivi a `now`).
// Si chiama dopo un invio riuscito per sapere con quanti riceventi utili e' stato condiviso quel frame (molteplicita' reale: 0, 1, 2, 3 o piu'). Non modifica nulla.
int sched_need_count(const ts_sched* S, uint32_t g, uint32_t now);
// Titolo in corso (-1 = nessuno) per il frame STATE.
int sched_cur(const ts_sched* S);
// Riceventi che servono ora (vivi, con qualcosa da ricevere).
int sched_serving(const ts_sched* S, uint32_t now);
// Riceventi registrati e vivi.
int sched_alive(const ts_sched* S, uint32_t now);
// 1 se c'e' almeno un ricevente registrato e nessuno ha ancora bisogno di niente.
int sched_all_done(const ts_sched* S);
// Definizioni per lo schermo del mittente (e per il riepilogo ai riceventi): tutte sui titoli che QUEL ricevente vuole.
uint32_t sched_need_blocks(const ts_sched* S, const ts_sclient* c);    // somma dei blocchi dei titoli voluti
uint32_t sched_remain_blocks(const ts_sched* S, const ts_sclient* c);  // blocchi che gli mancano ancora (non scaricati): 0 = ha tutto, anche se sta ancora installando
int sched_finished(const ts_sched* S, const ts_sclient* c);            // ha risposto e ogni titolo voluto e' installato (o in errore)
// Percentuali 0-100: scaricato = blocchi che ha (in finestra, in coda o su file, o gia' installati); installato = blocchi consegnati ad AM.
// 100 solo quando e' davvero tutto fatto (scaricato: remain == 0; installato: sched_finished); prima il massimo e' 99.
int sched_pct_down(const ts_sched* S, const ts_sclient* c);
int sched_pct_inst(const ts_sched* S, const ts_sclient* c);
// Tempo stimato alla fine dell'installazione (secondi), -1 se non noto (velocita' non misurata o ricevente che non risponde).
double sched_eta_sec(const ts_sched* S, const ts_sclient* c, uint32_t now);
// Indice dello slot di un token (-1 se non c'e').
int sched_find(const ts_sched* S, uint16_t token);
void sched_stats(const ts_sched* S, ts_sched_stats* out);
