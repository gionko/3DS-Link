#pragma once
#include <stdint.h>

// Registro degli eventi in <cartella dei dati dell'app>/3DSLink-TitleDelivery.log (hostcfg.h, logrot.h): si scrive in coda, a 64 MiB il file ruota
// (.1.log ... .7.log, 8 file al massimo) e senza l'interruttore dell'app (Debug log) non si crea nessun file. Serve a capire cosa e' successo
// quando un trasferimento si interrompe. Sicuro da usare da piu' thread.
//
// ts_log NON tocca la SD: la riga finisce in un anello in RAM e la scrive un thread a parte. Cosi' un'attesa della SD
// (per esempio mentre AM prepara un titolo grosso) non ferma mai il thread di rete. Se l'anello si riempie le righe
// nuove si perdono e il registro lo dice.
//
// Watchdog: un thread controlla i "battiti" dei cicli registrati con ts_wd_add. Se un ciclo non batte per piu' di
// un secondo scrive nel registro in quale sezione si era fermato e per quanto tempo.

void ts_log_open(const char* role);
// 1 se il registro e' aperto (Debug log acceso): chi prepara righe che costano chiamate di sistema lo chiede prima (ts_log da sola non costa niente).
int ts_log_active(void);
// Millisecondi dall'apertura del registro (la stessa base dei tempi scritti nelle righe).
uint64_t ts_log_ms(void);
void ts_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void ts_log_close(void);
// Scrive subito su SD quello che e' nell'anello (prima di chiamate che potrebbero bloccarsi: cosi' l'ultima riga arriva sul file).
void ts_log_flush(void);

// Scrive la riga "correzioni:" (quale codice ha prodotto il registro: blocco del parcheggio, politica di scrittura, soglia PARKOK, corse della cache delle ripetizioni, P1, data di
// compilazione). runsEff = corse effettive della cache dopo alloc_runs (mittente) o -1 (ricevente: non le usa).
void ts_log_corrections(int runsEff);

// Registra un ciclo da sorvegliare. Ritorna un id (>= 0) oppure -1 se non c'e' posto o il registro non e' aperto.
int ts_wd_add(const char* name);
// Da chiamare a ogni giro del ciclo; `section` e' un numero qualsiasi che dice dove si e' arrivati.
void ts_wd_beat(int id, int section);
void ts_wd_remove(int id);
