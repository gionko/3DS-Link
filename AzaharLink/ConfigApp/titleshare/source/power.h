#pragma once

// Energia e ciclo di vita dell'applicazione durante un trasferimento.
//
// Chiudere la conchiglia manda la richiesta di sleep: se l'app non risponde, resta lo schermo nero (test 3 del 10.10.2026), e
// se risponde "accetto" UDS va in errore e il trasferimento cade. Durante un trasferimento quindi:
//   - si rifiuta lo sleep (aptSetSleepAllowed(false)) e si disattiva il tasto HOME (sospenderebbe il ciclo di invio);
//   - si chiama aptMainLoop() a intervalli regolari, cosi' le richieste di sistema ricevono risposta;
//   - si registrano nel log gli eventi APT (sospensione, sleep, risveglio, uscita);
//   - con SELECT si spengono e riaccendono i backlight (risparmio di batteria) senza fermare l'app.

#include <stdint.h>

// Memoria che un malloc puo' ancora ottenere: parte dello heap non ancora presa dall'allocatore + blocchi liberi gia' presi.
uint32_t ts_mem_heap_free(void);

// Scrive nel registro una riga "sistema": modello, memoria, SD, batteria, segnale Wi-Fi, conchiglia, canale UDS.
void ts_power_log_system(const char* tag);

void ts_power_begin_transfer(void);
void ts_power_end_transfer(void);
// Da chiamare a ogni giro di un ciclo lungo. 0 = il sistema chiede di chiudere l'app (per esempio POWER lungo).
int ts_power_poll(void);
void ts_power_toggle_screens(void);
int ts_power_screens_off(void);
// 1 se non serve ridisegnare gli schermi: backlight spenti con SELECT oppure conchiglia chiusa (risparmia CPU e batteria).
int ts_power_skip_drawing(void);
void ts_power_exit(void);
