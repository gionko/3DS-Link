#pragma once
#include <stdint.h>

// Passo di invio del mittente (blocchi al secondo). Due famiglie di modalita':
//   - fissa: il passo non cambia (0 = adattivo, 1..5 sono fisse e servono a confrontare, 6 = rampa, 7 = libera);
//   - libera: nessun passo. Si invia appena la coda del chip accetta un frame e si aspetta 100 us quando risponde "occupato": e' il chip
//     a regolare la velocita' (misura diretta del suo tetto); la finestra del ricevente limita comunque quanto si va avanti;
//   - rampa: il passo sale a gradini da TS_RAMP_START a TS_RAMP_END (un gradino ogni TS_RAMP_STEP_SEC): serve a misurare in una
//     sola prova dove la radio comincia a saturarsi (occupato/s, perdita) e dove il ricevente non regge piu';
//   - adattiva: cerca il passo che massimizza il goodput, cioe' i blocchi consegnati al secondo
//     (inviati meno quelli che i riceventi devono richiedere di nuovo).
//
// L'adattivo non abbassa il passo "per principio": prova un gradino (8%) alla volta e lo tiene solo se il goodput
// non peggiora. Cosi' funziona sia quando la perdita dipende dal carico (abbassare conviene) sia quando dipende dal
// segnale (abbassare non serve e il passo resta alto). Se il passo impostato e' sopra quello che la radio regge
// davvero, si riporta sulla capacita' misurata.
//
// Codice puro, senza dipendenze dalla console: provato su PC con modelli di canale (pace_test).

#define TS_PACE_MODES 8
#define TS_FREE_RATE 100000.0 // modalita' libera: nessun passo, ci pensa la coda di trasmissione del chip (errore "occupato")
#define TS_RAMP_START 1100.0 // modalita' rampa: parte da qui
#define TS_RAMP_STEP 100.0   // e sale di tanto
#define TS_RAMP_STEP_SEC 16.0 // ogni tanti secondi
#define TS_RAMP_END 2600.0   // fino a qui, poi resta
#define TS_PACE_PERIOD_MS 4000

typedef struct {
    int mode;      // 0 = adattivo, 1..TS_PACE_MODES-1 = fisso (vedi pace_mode_name)
    double rate;   // blocchi al secondo
    double minRate, maxRate;
    int phase;     // 0 = misura la base, 1 = prova un gradino
    int dir;       // direzione della prova: +1 sale, -1 scende, 0 = da decidere
    int hold;      // periodi da aspettare prima di una nuova prova
    double baseRate, baseGood;
    double refGood; // miglior goodput visto di recente (decade piano): impedisce che piccoli peggioramenti si sommino
    double lossEma; // perdita stimata (richieste nuove / blocchi inviati), per il registro e lo schermo
    double goodEma; // goodput stimato in blocchi al secondo
    uint32_t periods;
    double elapsed; // secondi dall'avvio (serve alla rampa)
} ts_pace;

void pace_init(ts_pace* p, int mode);
// A fine di ogni periodo: `sent` frame inviati (di ogni tipo), `req` buchi nuovi scoperti dai riceventi, `dt` secondi.
void pace_update(ts_pace* p, double sent, double req, double dt);
const char* pace_mode_name(int mode);
