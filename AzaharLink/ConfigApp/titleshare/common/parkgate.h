#pragma once
#include <stdint.h>

// Blocco del thread di parcheggio durante le scritture di AM. La SD e' una sola: mentre AM raccoglie e scrive un gruppo di blocchi (o apre, scrive la
// prima volta, chiude un titolo) il parcheggio non deve scriverci sopra, perche' nelle prove su console AM passava da 2 a 0,5 MiB/s con il parcheggio
// in parallelo. Prima il blocco copriva solo i 150 ms DOPO una scrittura: una scrittura piu' lunga (o una raccolta dal file piu' lunga) trovava il
// parcheggio libero, e la clausola dei 20 s si misurava dall'ultimo passo del parcheggio invece che dall'inizio dell'operazione di AM.
//
//   - Il thread di installazione scrive lo stato: `from` = istante (ms, 32 bit, mai 0) in cui e' iniziata l'operazione di AM in corso, 0 = AM non
//     sta lavorando. Va da quando comincia a RACCOGLIERE un gruppo (dal primo blocco letto dal file o preso dalla finestra) fino alla fine di
//     ts_am_write; per begin, prima scrittura e finish fino alla loro fine. `until` = fine della coda di 150 ms che segue ogni operazione di AM che
//     ha lavorato (scrittura, apertura, chiusura). `limit` = dopo quanti ms dall'inizio l'operazione in corso non tiene piu' fermo il parcheggio.
//     Con l'installatore a secco (nessun blocco raccolto, nessuna scrittura) `from` e' 0: la SD e' libera ed e' giusto parcheggiare.
//   - Il thread di parcheggio decide con ts_amgate_hold. Un'unica parola da 32 bit (`from`) dice se AM lavora e da quando, quindi non c'e' da tenere
//     coerenti due variabili; `limit` e `until` si scrivono PRIMA di `from` (all'inizio) o di azzerarlo (alla fine), con una barriera di memoria nel
//     mezzo, e il parcheggio le legge nell'ordine inverso.
//
// Tipi a 32 bit allineati (letture e scritture atomiche su ARM11), nessun uint64 condiviso; gli accessi sono atomici "rilassati" (__atomic_*: stesse
// istruzioni ldr/str di un volatile, ma senza corsa sui dati in senso C11, quindi ThreadSanitizer le accetta). Codice puro: provato su PC
// (share/tests/parkgate_test.c).

#ifdef __3DS__
#include <3ds.h>
#define TS_DMB() __dmb()
#else
#define TS_DMB() __sync_synchronize()
#endif

#define TS_AM_TAIL_MS 150          // il parcheggio resta fermo ancora questo tempo dopo la fine di un'operazione di AM che ha lavorato
#define TS_AM_STUCK_MS 20000       // un'operazione di AM piu' lunga di cosi' non tiene fermo il parcheggio (SD bloccata, AM impiccato): si scrive comunque
#define TS_AM_STUCK_FIRST_MS 60000 // lo stesso per la PRIMA scrittura di un titolo: AM prepara tutto lo spazio e per un titolo da GiB dura normalmente fino a ~22 s
#define TS_AM_SLACK_MS 10000       // quanto un `until` puo' superare il `now` letto dal parcheggio (orologio letto un po' prima, thread sospeso): oltre non e' una coda vera

#define TS_GATE_LD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define TS_GATE_ST(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)

typedef struct {
    uint32_t from;   // 0 = AM non lavora; altrimenti inizio (ms) dell'operazione in corso
    uint32_t until;  // fine della coda dopo l'ultima operazione di AM che ha lavorato (ms)
    uint32_t limit;  // ms dall'inizio dopo i quali l'operazione in corso non tiene piu' fermo il parcheggio
} ts_amgate;

// Prima dell'avvio dei thread: AM non lavora e nessuna coda attiva (`until` un millisecondo nel passato).
static inline void ts_amgate_init(ts_amgate* g, uint32_t now) {
    g->from = 0;
    g->until = now - 1u;
    g->limit = TS_AM_STUCK_MS;
}

// Thread di installazione: comincia un'operazione di AM (raccolta di un gruppo, begin, finish). limitMs = TS_AM_STUCK_MS, o TS_AM_STUCK_FIRST_MS se nel gruppo
// c'e' la prima scrittura del titolo.
static inline void ts_amgate_begin(ts_amgate* g, uint32_t now, uint32_t limitMs) {
    TS_GATE_ST(&g->limit, limitMs);
    TS_DMB(); // `limit` visibile prima che `from` sia diverso da 0
    TS_GATE_ST(&g->from, now ? now : 1u);
}

// Thread di installazione: l'operazione e' finita. `tailMs` > 0 se AM ha lavorato (scrittura, apertura, chiusura: coda di 150 ms), 0 per una raccolta a vuoto.
static inline void ts_amgate_end(ts_amgate* g, uint32_t now, uint32_t tailMs) {
    if(tailMs) {
        TS_GATE_ST(&g->until, now + tailMs);
    }

    TS_DMB(); // `until` visibile prima che `from` torni a 0
    TS_GATE_ST(&g->from, 0u);
}

// Thread di parcheggio: valuta il blocco e dice anche PERCHE' (il registro conta i casi: strumentazione, nessuna decisione cambia). Il parcheggio resta fermo mentre
// AM lavora e per la coda dopo un'operazione, salvo:
//   - valvola: coda di parcheggio al 75% (altrimenti si scartano blocchi);
//   - operazione di AM in corso da piu' di `limit` ms (20 s; 60 s per la prima scrittura) dal suo INIZIO, scritto dall'installatore.
// `until` conta come coda solo se e' nel futuro di non piu' di TS_AM_TAIL_MS + TS_AM_SLACK_MS: un `until` vecchio di 2^31 ms o piu' (25 giorni senza
// operazioni di AM) non deve sembrare nel futuro.
// Ritorna TS_GATE_FREE (AM non lavora e nessuna coda: niente da trattenere), TS_GATE_HELD (deve restare fermo), TS_GATE_VALVE (AM lavora o c'e' la coda, ma la coda di
// parcheggio e' al 75%: scrive comunque) o TS_GATE_CLAUSE (operazione di AM oltre il limite: scrive comunque). La valvola ha la precedenza sulla clausola.
enum { TS_GATE_FREE = 0, TS_GATE_HELD = 1, TS_GATE_VALVE = 2, TS_GATE_CLAUSE = 3 };

static inline int ts_amgate_check(const ts_amgate* g, uint32_t now, uint32_t queued, uint32_t slots) {
    uint32_t from = TS_GATE_LD(&g->from);
    TS_DMB();
    uint32_t until = TS_GATE_LD(&g->until);
    uint32_t limit = TS_GATE_LD(&g->limit);
    int working = from != 0;
    int tail = !working && (uint32_t) (until - now - 1u) < (uint32_t) (TS_AM_TAIL_MS + TS_AM_SLACK_MS);
    if(!working && !tail) {
        return TS_GATE_FREE;
    }

    if(queued >= slots / 4 * 3) {
        return TS_GATE_VALVE;
    }

    if(working && (int32_t) (now - from) > (int32_t) limit) {
        return TS_GATE_CLAUSE;
    }

    return TS_GATE_HELD;
}

// Thread di parcheggio: 1 = deve restare fermo, 0 = puo' scrivere (vedi ts_amgate_check).
static inline int ts_amgate_hold(const ts_amgate* g, uint32_t now, uint32_t queued, uint32_t slots) {
    return ts_amgate_check(g, now, queued, slots) == TS_GATE_HELD;
}
