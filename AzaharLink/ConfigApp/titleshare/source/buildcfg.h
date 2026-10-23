#pragma once

// Impostazioni di compilazione che il registro dichiara all'apertura (riga "correzioni:", vedi ts_log_corrections in log.c), cosi' ogni registro dice quale codice lo ha
// prodotto. I valori di base stanno qui e nei file indicati; si cambiano con il Makefile (make SND_RUNS=n, make PARKOK75=n), mai con una seconda definizione altrove.
//   - TS_PARKOK_SOTTO_75: common/store.h (di base 0 = spento; make PARKOK75=1 lo accende).
//   - TS_SND_RUNS: qui sotto.

// Corse della cache delle ripetizioni del mittente (common/runcache.[ch], usata da share/source/sender.c): 1-64, ciascuna di 16 blocchi = 22,75 KiB.
// Di base 16 (364 KiB): con 3-4 riceventi (le prove su console) 64 non da' tempo in piu' (+-1-2%) e manda il 2-5% di frame in piu' (un mittente piu' veloce, con gli ACK
// ancora in volo, ripete di piu'). 64 (1,42 MiB, l'1,6% dello heap di una New 3DS) serve a 7 riceventi con perdite diverse e costanti: l'ultimo finisce il 6-16% prima
// che con 16 corse, perche' i blocchi persi di nuovo tornano a distanza di secondi. make SND_RUNS=64 per quel caso. Se la memoria non basta alloc_runs dimezza fino a 2.
#ifndef TS_SND_RUNS
#define TS_SND_RUNS 16
#endif
