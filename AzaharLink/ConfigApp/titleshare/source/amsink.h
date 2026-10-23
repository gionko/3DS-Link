#pragma once
#include <3ds.h>
#include <stdint.h>

#include "proto.h"

// Installazione di un titolo su questa console passando dal servizio AM, come fa FBI (installcias.c).

FS_MediaType ts_title_dest(u64 tid);

// 1 se il titolo e' gia' installato sulla SD (o sul NAND per i titoli di sistema).
int ts_title_installed(u64 tid);

// Dimensione che il titolo gia' installato occupa (0 se non c'e'): lo spazio che si libera sovrascrivendolo.
uint64_t ts_title_size(u64 tid);

// Prepara l'installazione. Con overwrite cancella prima il titolo e il suo ticket (come FBI).
Result ts_am_begin(u64 tid, int overwrite, Handle* h);

// Scrive i byte del CIA all'offset indicato (sempre in avanti).
Result ts_am_write(Handle h, u64 offset, const void* buf, u32 size);

// Conclude l'installazione e aggiunge il seed se il titolo lo usa.
Result ts_am_finish(Handle h, const ts_title* t);

void ts_am_cancel(Handle h);
