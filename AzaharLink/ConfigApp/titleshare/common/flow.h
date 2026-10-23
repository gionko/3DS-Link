#pragma once
#include <stddef.h>
#include <stdint.h>
#include "proto.h"

// Controllo del flusso del mittente: decide quale blocco trasmettere (riparazione, recupero o nuovo) in base a cio' che
// i riceventi dichiarano di avere e di non avere. Nessuna dipendenza dalla console: il tempo e' passato dal chiamante.
//
// Tre tipi di invio (flow_pick / flow_sent):
//   TS_PICK_FRESH   blocco nuovo (mai inviato);
//   TS_PICK_REPAIR  ripetizione di un blocco gia' inviato dentro la finestra, richiesto da almeno un ricevente;
//   TS_PICK_CATCH   recupero per un ricevente rimasto indietro, fuori dalla finestra (vedi sotto).
//
// Ripetizioni: ogni ricevente dichiara i suoi buchi a ogni riscontro; per ogni blocco si conta quanti riceventi lo
// chiedono. Si ripete per primo il blocco chiesto da piu' riceventi, ma sempre fra le prime richieste (indice basso):
// l'installazione e' sequenziale e il buco piu' vecchio ferma tutto.
//
// Chi resta indietro: un ricevente che non si fa sentire per TS_LAG_MS mentre altri si fanno sentire esce dal calcolo
// della finestra, cosi' gli altri non aspettano. Quando rientra con il cursore sotto la base e' "behind": non viene piu'
// servito con le ripetizioni normali ma con il recupero: il mittente spinge in sequenza i blocchi dentro la sua finestra
// (saltando quelli che ha gia', dai suoi buchi) con una quota (TS_CATCH_SHARE_PCT) della banda, tutta la banda se gli altri
// non hanno nulla da ricevere, finche' il suo cursore non raggiunge la base. Con un solo ricevente nessuno esce mai dalla finestra: il comportamento e' quello di sempre.
//
// Stanza sempre aperta e giri: i riceventi entrano quando vogliono. Chi entra a giro in corso e non puo' agganciarsi (il suo
// cursore e' sotto la base, o vuole titoli che il giro non invia, o la sua finestra e' piu' piccola) e' "late": non tiene
// la finestra e non riceve recuperi, aspetta. Quando nessun ricevente in linea ha ancora da ricevere (tutti finiti o usciti)
// parte un nuovo giro (flow_update_lap): il flusso torna al cursore piu' basso dei "late", che diventano in linea. Chi era
// in linea e resta indietro viene invece recuperato con TS_PICK_CATCH come prima.
#define TS_MAX_CLIENTS 8
#define TS_LAG_MS 5000
#define TS_CATCH_SHARE_PCT 25
// Regola della molteplicita': la radio si usa sempre al massimo, e fra piu' candidati si invia quello che serve a piu' riceventi con una
// sola trasmissione (a parita', quello con l'indice piu' basso). Una ripetizione chiesta da k riceventi vale k; un blocco nuovo vale
// quanti riceventi in linea lo aspettano. Si invia per prima la ripetizione se k >= riceventi del blocco nuovo; se vale di meno si
// rimanda, tranne una scelta su TS_MULT_TURN (cosi' chi e' solo a chiederla non resta senza). Con un solo ricevente il blocco nuovo vale 1:
// si comporta come sempre. Nessuna eccezione per i buchi "urgenti": un ricevente che chiede da solo non ferma gli altri.
#define TS_MULT_TURN 4
// Coda mai vista: se il ricevente ha visto i blocchi solo fino a `tailHi` e il mittente e' piu' avanti di TS_TAIL_GAP blocchi, il tratto in mezzo
// si considera richiesto (senza contarlo come perdita).
#define TS_TAIL_GAP 1024
// Un ricevente che tiene ferma la finestra (il flusso non puo' avanzare perche' lui e' lontano) per piu' di TS_BLOCK_MS, mentre tutti gli altri
// in linea sono avanti di almeno TS_SLOW_AHEAD blocchi e hanno la finestra quasi vuota (aspettano dati), esce dalla finestra e si recupera a parte:
// il suo ritardo non ferma piu' gli altri.
#define TS_BLOCK_MS 3000
#define TS_SLOW_AHEAD 2048
#define TS_PICK_FRESH 0
#define TS_PICK_REPAIR 1
#define TS_PICK_CATCH 2

typedef struct {
    int used;
    int lost;    // scaduto senza aver finito: se rientra con lo stesso token riprende
    int lagging; // non si fa sentire da TS_LAG_MS: escluso dal calcolo della finestra
    int behind;  // rientrato con il cursore sotto la base: servito con il recupero
    uint16_t node;
    uint32_t cursor;
    uint32_t lastHeard;
    int done;
    uint32_t nReq;                    // blocchi che questo ricevente chiede dentro la finestra
    ts_range catchR[TS_MAX_RANGES];   // buchi dichiarati da un ricevente "behind" (sotto catchHi)
    int nCatch;
    uint32_t catchHi;                 // primo blocco che il ricevente non ha mai visto (da TS_F_TAIL)
    uint32_t catchPos;                // prossimo blocco da inviare per il recupero
    uint32_t catchLast, catchAt;      // ultimo blocco di recupero inviato e quando (+1, 0 = mai)
    int late;                         // entrato a giro in corso senza potersi agganciare: aspetta il giro successivo
    int everIn;                       // e' stato in linea (tiene o ha tenuto il passo): se resta indietro viene recuperato
    int noMerge;                      // non entra in una fusione (era stato messo in linea senza mai rispondere): aspetta che il giro finisca
    uint32_t startHint;               // primo blocco che gli serve (primo titolo voluto): vale finche' il suo cursore (0 prima di cominciare) non lo supera
    int mayRide;                      // il mittente dice che puo' agganciarsi al giro in corso (titoli nell'unione del giro)
    uint32_t cap;                     // finestra dichiarata (blocchi); 0 = non nota
    uint32_t scanPos;                 // nessuna richiesta di questo ricevente sotto questo blocco (scansione in avanti)
    uint32_t hi;                      // primo blocco che non ha mai visto (dal segnaposto di coda dell'ultimo riscontro)
    ts_range cskip[TS_MAX_TITLES + 1]; // blocchi dei titoli che questo ricevente NON vuole (li salta da solo)
    int ncskip;
} ts_client;

typedef struct {
    uint32_t total;  // blocchi totali
    uint32_t W;      // finestra in uso: i riceventi tengono almeno tanti blocchi (minimo fra quelli presenti all'avvio)
    uint32_t Wcap;   // finestra con cui sono dimensionate le tabelle: W non puo' superarla
    uint32_t safety; // margine: il blocco nuovo resta almeno `safety` sotto il limite della finestra
    uint32_t next;   // prossimo blocco nuovo
    ts_client cl[TS_MAX_CLIENTS];
    uint8_t* pend;       // W: quanti riceventi chiedono il blocco (indice % W); 0 = nessuno
    uint8_t* pc;         // TS_MAX_CLIENTS * W: 1 se il ricevente i chiede il blocco
    uint32_t* sentAt;    // W: istante dell'ultima riparazione (+1, 0 = mai)
    uint8_t* reqSeen;    // W: 1 se la richiesta di questo blocco e' gia' stata contata in statReq
    uint32_t pendCount;  // blocchi con almeno una richiesta
    uint32_t scan;       // nessuna richiesta sotto questo indice
    uint32_t base;       // minimo cursore noto: sotto non ci sono richieste valide
    uint32_t repairGapMs;
    uint32_t turn;       // contatore per dare al recupero la sua quota
    int pickClient;      // ricevente servito dall'ultimo TS_PICK_CATCH
    ts_range skip[TS_MAX_TITLES + 1]; // intervalli che nessuno ha chiesto: non si inviano
    int nSkip;
    uint32_t blockedSince;  // da quando la finestra del piu' lento tiene fermo il flusso (0 = non e' fermo)
    uint32_t multTurn;   // contatore: ogni TS_MULT_TURN scelte rimandate una ripetizione passa comunque (nessuna inedia)
    uint32_t lap;        // numero del giro in corso (0 = nessuno ancora partito)
    uint32_t lapStart;   // blocco da cui e' partito il giro
    uint64_t statRepairs, statFresh, statCatch;
    uint64_t statReq;    // buchi scoperti dai riceventi (ogni blocco una volta, finche' non viene ripetuto): misura la perdita
} ts_flow;

// `window` e' la capacita' massima delle tabelle; la finestra in uso si riduce con flow_set_window prima di inviare.
int flow_init(ts_flow* f, uint32_t total, uint32_t window, uint32_t safety);
// Imposta la finestra in uso (<= capacita'). Solo prima di inviare il primo blocco. Ritorna 0 se riuscito.
int flow_set_window(ts_flow* f, uint32_t w);
// Dichiara la finestra di un ricevente (blocchi) e se puo' agganciarsi al giro in corso. Si chiama alla ricezione del READY e a ogni nuovo giro.
void flow_set_client_info(ts_flow* f, int idx, uint32_t cap, int mayRide);
// Intervalli di blocchi dei titoli che il ricevente non vuole (servono a contare quanti riceventi aspettano un blocco nuovo).
void flow_set_client_skips(ts_flow* f, int idx, const ts_range* r, int n);
// Primo blocco che serve al ricevente (prima ancora che mandi un riscontro): decide se puo' agganciarsi e da dove riparte un giro.
void flow_set_client_hint(ts_flow* f, int idx, uint32_t firstNeeded);
// Se nessun ricevente in linea ha ancora da ricevere e ci sono riceventi "late", parte un nuovo giro: il flusso riparte dal cursore
// piu' basso dei "late" (*pos), la finestra diventa la minima fra le loro (*w), i "late" entrano in linea. Ritorna 1 se e' partito un giro.
// Dopo un nuovo giro il chiamante ricalcola gli intervalli da saltare (flow_set_skips) e riposiziona la lettura.
int flow_update_lap(ts_flow* f, uint32_t* pos, uint32_t* w);
// Fusione: quando chi arriva a giro in corso (i "late") perderebbe troppo ad aspettare il giro dopo, il flusso torna indietro al loro cursore piu'
// basso e prosegue per tutti (quelli gia' avanti ricevono doppioni, che servono ai nuovi: una trasmissione, piu' riceventi). Conviene finche' i
// blocchi gia' inviati dal punto di ripartenza (il costo) non superano il doppio di quelli ancora da inviare. Ritorna 1 se e' avvenuta;
// *pos = blocco di ripartenza, *w = finestra (la minima fra la attuale e quelle dei nuovi). Il chiamante ricalcola intervalli e lettore.
int flow_merge_lap(ts_flow* f, uint32_t* pos, uint32_t* w);
// Riceventi che hanno ancora da ricevere e sono in linea / che aspettano il giro successivo.
int flow_inline_count(const ts_flow* f);
// Indice del ricevente registrato con questo identificativo (-1 se non c'e', senza crearlo).
int flow_find(const ts_flow* f, uint16_t node);
// Toglie un ricevente senza segnarlo come perso (chi e' uscito prima di cominciare).
void flow_drop(ts_flow* f, int idx);
int flow_late_count(const ts_flow* f);
void flow_free(ts_flow* f);
// Registra (o aggiorna) un ricevente. Ritorna l'indice nella tabella, -1 se piena.
int flow_client(ts_flow* f, uint16_t node, uint32_t now);
// Elabora un ACK: cursore, blocchi mancanti, flags (TS_F_DONE = finito, TS_F_TAIL = l'ultimo intervallo e' il segnaposto).
void flow_ack(ts_flow* f, uint16_t node, uint32_t now, uint32_t cursor, const ts_range* r, int n, int flags);
// Scarta i riceventi che non si fanno sentire da `timeoutMs` e marca chi resta indietro (TS_LAG_MS).
void flow_expire(ts_flow* f, uint32_t now, uint32_t timeoutMs);
// Minimo cursore tra i riceventi attivi nella finestra (next se non ce ne sono).
uint32_t flow_min_cursor(const ts_flow* f);
int flow_active(const ts_flow* f);
// Quanti riceventi sono scaduti senza aver finito.
int flow_lost(const ts_flow* f);
// Quanti riceventi attivi sono in ritardo o in recupero.
int flow_behind(const ts_flow* f);
// Rimanda in attesa un ricevente che era stato messo in linea ma non ha mai cominciato a rispondere (non tiene piu' la finestra).
void flow_demote(ts_flow* f, int idx);
// Dimentica i riceventi persi (alla partenza: chi ha lasciato la sala d'attesa non deve bloccare la fine).
void flow_forget_lost(ts_flow* f);
// Sceglie il prossimo blocco da inviare. 1 se c'e' qualcosa da inviare; *kind e' TS_PICK_*.
int flow_pick(ts_flow* f, uint32_t* block, int* kind);
// Da chiamare dopo l'invio riuscito del blocco scelto (o per annullare la richiesta se non si riesce a leggerlo).
void flow_sent(ts_flow* f, uint32_t block, int kind, uint32_t now);
// 1 se il blocco e' richiesto da almeno un ricevente (serve a leggere dalla SD piu' blocchi di fila).
int flow_is_pending(const ts_flow* f, uint32_t block);
// 1 se c'e' almeno un ricevente e tutti hanno finito.
int flow_all_done(const ts_flow* f);
// Intervalli da non inviare (ordinati, non sovrapposti). Va chiamata prima di iniziare a inviare.
void flow_set_skips(ts_flow* f, const ts_range* r, int n);
// Se g cade in un intervallo da saltare, ne ritorna la fine (altrimenti g).
static inline uint32_t ts_skip_adjust(const ts_range* r, int n, uint32_t g) {
    for(int i = 0; i < n; i++) {
        if(g >= r[i].start && g < r[i].start + r[i].count) {
            return r[i].start + r[i].count;
        }
    }

    return g;
}
