#pragma once
#include <3ds.h>
#include <stdint.h>

#include "ctx.h"
#include "proto.h"

enum { TS_R_SCAN = 0, TS_R_CONNECT, TS_R_WAIT_MANIFEST, TS_R_CHOOSE, TS_R_WAIT_START, TS_R_RECEIVE, TS_R_DONE, TS_R_ERROR };

enum { TS_RES_PENDING = 0, TS_RES_OK = 1, TS_RES_SKIPPED = 2, TS_RES_ERROR = 3 };

typedef struct {
    int phase;
    ts_manifest m;
    int haveManifest;
    uint8_t exists[TS_MAX_TITLES];      // 1 se il titolo e' gia' installato su questa console
    uint64_t oldSize[TS_MAX_TITLES];    // spazio occupato dalla versione gia' installata (si libera sovrascrivendola), 0 se non c'e'
    uint8_t n3dsOnly[TS_MAX_TITLES];    // 1 se il titolo e' solo per New 3DS e questa console e' un Old 3DS (FBI chiede conferma)
    uint8_t want[TS_BITMAP_BYTES];      // titoli che questo ricevente vuole (la UI li modifica nella fase TS_R_CHOOSE)
    uint8_t result[TS_MAX_TITLES];
    uint32_t err[TS_MAX_TITLES];
    int curTitle;
    uint32_t cursor, total;
    double mibps;
    uint64_t recv, dups, outOfWindow;
    uint32_t amLastMs, amMaxMs; // durata dell'ultima e della piu' lunga scrittura verso AM
    int reconnects;             // quante volte si e' perso il collegamento e si e' rientrati
    uint32_t needBlocks;        // blocchi che questo ricevente deve ottenere (esclusi gli intervalli saltati)
    uint32_t dlBlocks;          // blocchi scaricati (arrivati e memorizzati)
    uint32_t instBlocks;        // blocchi gia' consegnati all'installazione
    double etaSec;              // tempo stimato alla fine per questo ricevente (-1 = non ancora stimabile)
    double senderEtaSec;        // tempo stimato alla fine per l'ultimo ricevente della stanza (dal mittente, -1 = non noto)
    int allDone;                // 1 = ho installato tutti i titoli voluti senza errori (solo allora le percentuali arrivano al 100%)
    uint32_t senderNext;        // carosello: completamento della stanza, 0-100 (media dei riceventi registrati; 100 solo se tutti hanno finito di installare)
    uint32_t senderKiBs;        // velocita' di invio dichiarata dal mittente
    int senderHears;            // 1 = il mittente mi sente, 0 = no, -1 = non ancora noto
    uint32_t dataAgeMs;         // da quanto non arrivano dati
    int nOthers;                // altri riceventi noti
    uint32_t otherCursor[16];   // percentuale installata degli altri riceventi (0-100, calcolata dal mittente sui loro titoli)
    uint8_t otherNode[16];      // nodo UDS di ciascuno (0 = non noto: mittente piu' vecchio): dal nodo l'app ricava il nome della console
    uint8_t otherState[16];     // cosa sta facendo: 0 scarica, 1 solo installa, 2 finito, 3 non risponde, 4 appena entrato, 5 aspetta il suo turno, 6 finito con errori, 255 = non noto
    uint8_t otherDown[16];      // quanto di cio' che ha chiesto ha scaricato (0-100)
    int channel;                // canale Wi-Fi della stanza (1, 6 o 11), 0 = non noto
    int reconnecting;           // 1 = il collegamento e' perso e si cerca la stanza: il servizio UDS e' occupato dalla ricerca, lo schermo non gli fa domande (i nomi restano quelli noti)
    uint64_t curInstBytes;      // byte del titolo in installazione (st.curTitle) gia' consegnati ad AM: l'app ne ricava "installing 37%" sulla sua riga
    uint32_t freeMiB;           // spazio libero sulla SD letto dal thread del parcheggio (MiB, 0xFFFFFFFF = non ancora letto): lo schermo non lo legge da se' (puo' bloccarsi con la SD occupata)
    int roomFull;               // 1 = rientrando, la scansione trova la stanza ma e' piena: non ci si collega (roomNodes riceventi presenti su roomMax)
    int roomNodes, roomMax;     // riceventi presenti / massimo, dal beacon della stanza (0 = non noto)
    char msg[96];
} ts_recv_stats;

enum { TS_RUI_NONE = 0, TS_RUI_CONFIRM = 1, TS_RUI_CANCEL = 2 };

// Cerca le reti di TitleShare. *nets va liberato con free(). 0 se riuscito.
int receiver_scan(udsNetworkScanInfo** nets, size_t* total);
// Come receiver_scan, ma solo la rete di quel mittente (indirizzo MAC); NULL = tutte. La riconnessione usa questa: deve tornare alla stessa stanza.
int receiver_scan_host(udsNetworkScanInfo** nets, size_t* total, const uint8_t* hostMac);

// Descrizione di una rete trovata: nome dell'ospite, contenuto offerto (con il nome del primo titolo: intero se il mittente lo manda, altrimenti i suoi 15 caratteri con "..."),
// canale Wi-Fi e riceventi presenti / massimo (0 = non noto). channel e nodes/max possono essere NULL.
void receiver_describe(const udsNetworkScanInfo* net, char* host, size_t hostSize, uint8_t* titles, uint64_t* totalBytes, char* firstName, size_t firstNameSize, int* channel, int* nodes,
                       int* maxNodes);

// Entra nella rete, riceve il manifesto, lascia scegliere i titoli (la UI modifica `want` e conferma) e li installa.
// `ui` viene chiamata circa 10 volte al secondo. Ritorna 0 se tutto e' andato a buon fine, 1 se qualche titolo e' fallito.
int receiver_run(ts_ctx* c, const udsNetworkScanInfo* net, int (*ui)(void* arg, ts_recv_stats* st), void* uiArg);
