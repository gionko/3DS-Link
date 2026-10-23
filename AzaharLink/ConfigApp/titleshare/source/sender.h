#pragma once
#include <3ds.h>
#include <stdint.h>

#include "ctx.h"
#include "flow.h"
#include "sched.h"
#include "proto.h"

typedef struct {
    int phase;            // 0 = sala aperta (nessuno da servire), 1 = trasferimento in corso, 2 = chiusa, 3 = errore
    int connected;        // nodi collegati alla rete (esclusa questa console)
    int ready;            // riceventi registrati (hanno risposto "pronto")
    int wanted;           // titoli richiesti da almeno un ricevente
    uint32_t lap;         // numero del giro in corso (0 = nessuno ancora partito)
    int nInline, nLate;   // riceventi in linea / che aspettano il giro successivo
    uint32_t total;       // blocchi totali
    uint32_t next;        // blocchi nuovi gia' inviati
    uint64_t repairs;
    double mibps;         // velocita' sui blocchi nuovi
    double paceRate;      // passo di invio corrente (blocchi al secondo)
    double paceLossPct;   // perdita stimata (buchi nuovi / frame inviati), in percentuale
    int paceMode;         // modalita' del passo (vedi pace_mode_name)
    int nClients;
    uint32_t cursor[TS_MAX_CLIENTS];
    uint8_t done[TS_MAX_CLIENTS];
    uint8_t state[TS_MAX_CLIENTS];  // 0 = in linea, 1 = aspetta il giro, 2 = in ritardo/recupero, 3 = finito (anche con errori)
    uint16_t ageDs[TS_MAX_CLIENTS]; // da quanto il mittente non sente il riscontro del ricevente (decimi di secondo)
    double etaSec;                  // tempo stimato alla fine dell'invio (-1 = non ancora stimabile)
    int curTitle;                   // titolo in invio (indice nel manifesto), -1 se nessuno
    // Percentuali del carosello (definizioni in common/sched.h): tutte sui titoli voluti da ciascun ricevente; 100% solo quando e' davvero tutto fatto.
    int pctRoom;                    // completamento della stanza = installato totale / voluto totale dei riceventi registrati
    double roomEta;                 // tempo stimato alla fine per l'ultimo ricevente (-1 = non noto)
    uint8_t pctDown[TS_MAX_CLIENTS];// scaricato (il ricevente ha i blocchi: finestra, coda, file o gia' installati)
    uint8_t pctInst[TS_MAX_CLIENTS];// installato (consegnato ad AM)
    uint8_t cstate[TS_MAX_CLIENTS]; // 0 scarica, 1 solo installa (ha tutti i blocchi), 2 finito, 3 non risponde, 4 appena entrato (nessun riscontro),
                                    // 5 aspetta il suo turno (gli mancano blocchi ma il titolo in invio non e' fra quelli che gli servono), 6 finito con errori
    int channel;                    // canale Wi-Fi della stanza (1, 6 o 11), 0 = non noto
    double cEta[TS_MAX_CLIENTS];    // tempo stimato per quel ricevente (-1 = non noto)
    // Per lo schermo del mittente (sola lettura, nessuna decisione cambia): chi e' ciascun ricevente e cosa gli manca.
    char cname[TS_MAX_CLIENTS][32];                    // nome della console (nome utente UDS), "" se non noto
    uint8_t cNode[TS_MAX_CLIENTS];                     // nodo UDS da cui parla (0 = non noto o non piu' nella rete)
    uint8_t cwant[TS_MAX_CLIENTS][TS_BITMAP_BYTES];    // titoli voluti
    uint8_t cTitleDown[TS_MAX_CLIENTS][TS_MAX_TITLES]; // per titolo voluto: scaricato, 0-100
    uint8_t cTitleInst[TS_MAX_CLIENTS][TS_MAX_TITLES]; // per titolo voluto: installato, 0-100
    uint32_t cFreeMiB[TS_MAX_CLIENTS];                 // spazio libero dichiarato (0xFFFFFFFF = ignoto)
    uint16_t cEff[TS_MAX_CLIENTS];                     // efficienza recente (per mille)
    uint32_t cDownBlocks[TS_MAX_CLIENTS];              // blocchi che il ricevente ha scaricato, sui titoli che vuole (la velocita' si ricava dalla differenza nel tempo)
    uint32_t cInstBlocks[TS_MAX_CLIENTS];              // blocchi che ha installato, sui titoli che vuole
    char msg[96];
} ts_sender_stats;

enum { TS_UI_NONE = 0, TS_UI_START = 1 /* non piu' usato: la stanza e' sempre aperta */, TS_UI_CANCEL = 2 };

// L'app puo' dare lei la percentuale della stanza e il tempo che manca (li calcola meglio: vede che cosa serve a ogni ricevente, titolo per titolo):
// finche' non li da, o con pct < 0 / etaSec < -1, valgono quelli del modulo. Vanno ai riceventi nel riepilogo (ROSTER) e si chiamano dal ritorno `ui`.
void sender_set_room_estimate(int pct, double etaSec);

// Modalita' del passo di invio, da scegliere in sala d'attesa: 0 = adattivo, le altre sono fisse (per confrontarle).
int sender_pace_mode(void);
void sender_pace_next(void);

// Controlla un titolo e ne ricava dimensione e seed. 0 se riuscito, altrimenti scrive il motivo in err.
int sender_check_title(ts_ctx* c, u64 tid, ts_title* t, char* err, size_t errSize);

// Compone il manifesto con i titoli (gia' controllati con sender_check_title e completati con nome e tipo).
int sender_make_manifest(const ts_title* t, int n, ts_manifest* m);

// Apre la stanza e la tiene aperta: i riceventi entrano e scaricano quando vogliono (nei limiti degli slot), il mittente trasmette
// a giri da solo. `ui` viene chiamata circa 4 volte al secondo; B la chiude. Ritorna 0 se chiusa dall'utente.
int sender_run(ts_ctx* c, ts_manifest* m, int (*ui)(void* arg, const ts_sender_stats* st), void* uiArg);
