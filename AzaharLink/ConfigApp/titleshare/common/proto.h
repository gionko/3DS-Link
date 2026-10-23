#pragma once
#include <stddef.h>
#include <stdint.h>

// Protocollo di TitleShare su UDS (wireless locale del 3DS). Tutti i campi sono little-endian.
// Un frame UDS porta al massimo 1478 byte; un frame DATA ne usa 12 (intestazione) + 4 (blocco) + 1456 (dati) = 1472.
// Il token identifica un ricevente anche se si ricollega con un altro numero di nodo UDS (0 nei frame del mittente).

#define TS_MAGIC 0x5354 // "TS"
#define TS_BLOCK 1456   // byte di dati per blocco (multiplo di 16: serve all'AES-CTR)
#define TS_HDR_SIZE 12 // magic(2) tipo(1) flags(1) sessione(4) token(2) riservato(2)
#define TS_MAX_TITLES 16
#define TS_MANIFEST_PER_FRAME 16 // voci del manifesto in un frame (con TS_MAX_TITLES 16 il manifesto e' un frame solo)
#define TS_BITMAP_BYTES ((TS_MAX_TITLES + 7) / 8)
#define TS_MAX_RANGES 100
#define TS_FRAME_MAX 1478

enum {
    TS_T_MANIFEST = 1, // mittente -> tutti: elenco dei titoli
    TS_T_READY = 2,    // ricevente -> mittente: manifesto ricevuto, pronto. flags = capacita' della sua finestra in unita' di TS_WIN_UNIT blocchi (0 = 8192)
    TS_T_START = 3,    // mittente -> tutti: inizia il trasferimento
    TS_T_DATA = 4,     // mittente -> tutti: un blocco
    TS_T_ACK = 5,      // ricevente -> mittente: cursore, blocchi mancanti (flags: bit0 = finito)
    TS_T_END = 6,      // mittente -> tutti: sessione terminata
    TS_T_ROSTER = 7,   // mittente -> tutti: quali riceventi sente e da quanto tempo (ogni secondo)
    TS_T_BYE = 8,      // ricevente -> mittente: uscita pulita (finito o annullato): lo slot si libera subito
    TS_T_ACK2 = 9,     // ricevente -> mittente (carosello): riepilogo per titolo + intervalli mancanti del titolo "focus"
    TS_T_STATE = 10,   // mittente -> tutti (carosello): titolo in corso (sostituisce START)
};

#define TS_WIN_UNIT 256 // unita' con cui il ricevente dichiara la capacita' della finestra nei flags di READY
#define TS_F_DONE 1
#define TS_F_ERR 4  // ACK2: ha finito, ma almeno un titolo e' fallito (il mittente lo mostra "con errori"; un mittente piu' vecchio non legge i flags di un ACK2)
#define TS_F_TAIL 2 // ACK: l'ultimo intervallo non e' un buco ma il segnaposto {start = primo blocco mai visto, count = spazio libero nella finestra}

typedef struct {
    uint64_t tid;
    uint64_t size;       // dimensione del CIA virtuale
    uint32_t firstBlock; // primo blocco globale del titolo
    uint32_t blocks;     // numero di blocchi (l'ultimo puo' essere parziale)
    uint8_t hasSeed;
    uint8_t seed[16];
    char code[16];       // codice prodotto, es. "CTR-P-BCEP"
    char name[48];       // nome del gioco (uguale per gioco, aggiornamento e DLC dello stesso gioco)
    uint8_t kind;        // 0 = gioco/app, 1 = aggiornamento, 2 = DLC, 3 = altro
} ts_title;

typedef struct {
    uint32_t session;
    int count;
    uint32_t totalBlocks;
    ts_title t[TS_MAX_TITLES];
    uint8_t have[TS_MAX_TITLES]; // lato ricevente: 1 se la voce e' gia' arrivata
} ts_manifest;

typedef struct {
    uint32_t start;
    uint32_t count;
} ts_range;

// Carosello (PROGETTO-CAROSELLO.md §2.4). Voce di riepilogo di un titolo nel riscontro ACK2.
enum { TS_SF_ACCEPT = 1, TS_SF_INSTALLING = 2, TS_SF_DONE = 4, TS_SF_ERROR = 8, TS_SF_PARKOK = 16 }; // PARKOK: installatore a secco, il titolo attivo parcheggia anche oltre la finestra
#define TS_ACK2_MAX_SUM TS_MAX_TITLES
#define TS_ACK2_MAX_RANGES 110
#define TS_NO_TITLE 0xFF
typedef struct {
    uint8_t title;
    uint8_t flags;   // TS_SF_*
    uint32_t remain; // blocchi ancora da avere (non installati e non disponibili)
    uint32_t first;  // primo blocco locale non disponibile
    uint32_t inst;   // blocchi gia' consegnati all'installazione (da 'inst' a 'first' e' pronto in ordine: se e' poco l'installatore e' a secco)
} ts_sum;

// Calcola blocks, firstBlock e totalBlocks a partire dalle dimensioni.
void ts_manifest_finalize(ts_manifest* m);

// Titolo, offset nel CIA e lunghezza dei dati del blocco globale g. 0 se g e' fuori intervallo.
int ts_block_info(const ts_manifest* m, uint32_t g, int* title, uint64_t* offset, size_t* len);

// Identificativo stabile della console (u16, nel campo riservato dell'intestazione): lo porta il READY. Il token cambia a ogni avvio
// dell'app, l'identificativo no: se arriva un READY con un token nuovo e lo stesso identificativo di una voce esistente, la console
// e' stata riavviata e la voce vecchia si sostituisce subito invece di aspettare che scada.
void ts_set_hdr_ident(uint8_t* frame, uint16_t ident);
uint16_t ts_hdr_ident(const uint8_t* frame);
// Calcola l'identificativo da una stringa stabile (l'ID0 della console). 0 = non disponibile.
uint16_t ts_ident_from(const char* s);

// Costruzione dei frame; ritornano la lunghezza del frame (0 se non entra in cap).
// Il manifesto si divide in piu' frame da TS_MANIFEST_PER_FRAME voci: part va da 0 a ts_manifest_parts() - 1.
int ts_manifest_parts(const ts_manifest* m);
size_t ts_pack_manifest(uint8_t* out, size_t cap, const ts_manifest* m, int part);
size_t ts_pack_simple(uint8_t* out, size_t cap, uint8_t type, uint8_t flags, uint32_t session, uint16_t token);
size_t ts_pack_data(uint8_t* out, size_t cap, uint32_t session, uint32_t block, const uint8_t* payload, size_t len);
size_t ts_pack_ack(uint8_t* out, size_t cap, uint32_t session, uint16_t token, uint8_t flags, uint32_t cursor, const ts_range* r, int n);
// ROSTER (ogni secondo, dal mittente a tutti): quanto ha inviato, a che velocita', e per ogni ricevente che sente:
// token, eta' dell'ultimo riscontro (decimi di secondo) e cursore (blocchi gia' installati/consegnati).
typedef struct {
    uint32_t senderNext;  // carosello: percentuale di completamento della stanza (0-100, 100 solo se tutti i registrati hanno finito di installare); prima: blocchi nuovi inviati
    uint32_t rateKiBs;    // velocita' recente di invio, KiB/s di dati
    uint16_t roomEtaSec;  // tempo stimato alla fine per l'ultimo ricevente (secondi, 0xFFFF = non noto)
    uint32_t senderMs;    // orologio del registro del mittente (ms) quando manda il riepilogo: serve ad allineare i due registri
    int n;
    uint16_t token[16];
    uint16_t ageDs[16];
    uint32_t cursor[16];  // carosello: percentuale installata del ricevente (0-100), calcolata dal mittente sui titoli che quel ricevente vuole
    // In coda al riepilogo (dopo le voci; un ricevente piu' vecchio non la legge e un mittente piu' vecchio non la manda: ext = 0): per ogni ricevente il
    // nodo UDS da cui parla (da cui chi legge ricava il nome della console), cosa sta facendo (0 scarica, 1 solo installa, 2 finito, 3 non risponde,
    // 4 appena entrato, 5 aspetta il suo turno, 6 finito con errori) e quanto di cio' che ha chiesto ha scaricato (0-100).
    int ext;
    uint8_t node[16];
    uint8_t state[16];
    uint8_t down[16];
} ts_roster;

size_t ts_pack_roster(uint8_t* out, size_t cap, uint32_t session, const ts_roster* r);
int ts_unpack_roster(const uint8_t* in, size_t n, ts_roster* r);

// Blocchi ancora da trasferire a partire da `from` (esclusi quelli degli intervalli saltati).
uint32_t ts_remaining_blocks(const ts_range* skip, int nSkip, uint32_t from, uint32_t total);
// READY e START portano una mappa di bit dei titoli (bit i = titolo i): cosa vuole il ricevente / cosa verra' inviato.
size_t ts_pack_bitmap(uint8_t* out, size_t cap, uint8_t type, uint8_t flags, uint32_t session, uint16_t token, int count, const uint8_t* bits);

// Lettura. ts_parse_hdr controlla magic e lunghezza minima.
int ts_parse_hdr(const uint8_t* in, size_t n, uint8_t* type, uint8_t* flags, uint32_t* session, uint16_t* token);
// Unisce un frame del manifesto a m (che deve essere azzerato la prima volta). 1 se il frame e' valido.
int ts_unpack_manifest(const uint8_t* in, size_t n, ts_manifest* m);
// 1 se tutte le voci del manifesto sono arrivate.
int ts_manifest_complete(const ts_manifest* m);
int ts_unpack_bitmap(const uint8_t* in, size_t n, int* count, uint8_t* bits);
int ts_unpack_data(const uint8_t* in, size_t n, uint32_t* block, const uint8_t** payload, size_t* len);
int ts_unpack_ack(const uint8_t* in, size_t n, uint32_t* cursor, ts_range* r, int maxRanges, int* nRanges);

// ACK2: focus = titolo di cui porta gli intervalli (TS_NO_TITLE = nessuno); freeMiB = spazio libero sulla SD (0xFFFFFFFF = ignoto);
// installed = blocchi gia' consegnati ad AM in tutto (per il riepilogo del mittente). Intervalli in blocchi locali del titolo focus.
typedef struct {
    uint8_t focus;
    uint32_t freeMiB;
    uint32_t installed;
    int nSum;
    ts_sum sum[TS_ACK2_MAX_SUM];
    int nRanges;
    ts_range r[TS_ACK2_MAX_RANGES];
} ts_ack2;
size_t ts_pack_ack2(uint8_t* out, size_t cap, uint32_t session, uint16_t token, uint8_t flags, const ts_ack2* a);
int ts_unpack_ack2(const uint8_t* in, size_t n, ts_ack2* a);

// STATE: titolo in corso (TS_NO_TITLE = nessuno), generazione (cresce a ogni cambio) e riceventi serviti.
size_t ts_pack_state(uint8_t* out, size_t cap, uint32_t session, uint8_t title, uint16_t gen, uint8_t nRecv);
int ts_unpack_state(const uint8_t* in, size_t n, uint8_t* title, uint16_t* gen, uint8_t* nRecv);

// READY con spazio libero (MiB) dopo la mappa di bit; i vecchi lettori ignorano la coda. ts_unpack_free_mib: 0xFFFFFFFF se manca.
size_t ts_pack_ready(uint8_t* out, size_t cap, uint8_t flags, uint32_t session, uint16_t token, int count, const uint8_t* bits, uint32_t freeMiB);
uint32_t ts_unpack_free_mib(const uint8_t* in, size_t n);

// Intervalli di blocchi dei titoli NON presenti in `bits` (nessuno li vuole): non vanno inviati. Intervalli adiacenti
// vengono uniti. Ritorna il numero di intervalli scritti (al massimo max).
int ts_skip_ranges(const ts_manifest* m, const uint8_t* bits, ts_range* out, int max);

static inline int ts_bit(const uint8_t* bits, int i) {
    return (bits[i >> 3] >> (i & 7)) & 1;
}

static inline void ts_set_bit(uint8_t* bits, int i, int v) {
    if(v) {
        bits[i >> 3] |= (uint8_t) (1 << (i & 7));
    } else {
        bits[i >> 3] &= (uint8_t) ~(1 << (i & 7));
    }
}
