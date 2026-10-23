#include "amsink.h"

#include "log.h"

#include <string.h>

FS_MediaType ts_title_dest(u64 tid) {
    u16 platform = (u16) ((tid >> 48) & 0xFFFF);
    u16 category = (u16) ((tid >> 32) & 0xFFFF);
    u8 variation = (u8) (tid & 0xFF);

    // Stessa regola di FBI: DSiWare, titoli di sistema, DLP sul NAND; applicazioni, aggiornamenti e DLC sulla SD.
    return platform == 0x0003 || (platform == 0x0004 && ((category & 0x8011) != 0 || (category == 0x0000 && variation == 0x02))) ? MEDIATYPE_NAND : MEDIATYPE_SD;
}

int ts_title_installed(u64 tid) {
    AM_TitleEntry entry;
    return R_SUCCEEDED(AM_GetTitleInfo(ts_title_dest(tid), 1, &tid, &entry));
}

uint64_t ts_title_size(u64 tid) {
    AM_TitleEntry entry;
    memset(&entry, 0, sizeof(entry));
    return R_SUCCEEDED(AM_GetTitleInfo(ts_title_dest(tid), 1, &tid, &entry)) ? entry.size : 0;
}

// Comando FS "AddSeed" (header 0x087A0180), lo stesso che usa FBI dopo l'installazione.
static Result fs_add_seed(u64 tid, const void* seed) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = 0x087A0180;
    cmdbuf[1] = (u32) (tid & 0xFFFFFFFF);
    cmdbuf[2] = (u32) (tid >> 32);
    memcpy(&cmdbuf[3], seed, 16);

    Result ret = svcSendSyncRequest(*fsGetSessionHandle());
    if(R_FAILED(ret)) {
        return ret;
    }

    return cmdbuf[1];
}

// Stessa sequenza di FBI (fbi/action/installcias.c, open_dst), SEMPRE e non solo quando il titolo esiste gia': cancella il titolo e
// il suo ticket (errori ignorati se non ci sono) e, sulla SD, fa preparare/verificare il database esterno dei titoli. Senza il
// controllo del database un'installazione su una SD nuova o con il database non allineato puo' fallire. `overwrite` non cambia piu'
// nulla (resta per compatibilita'): il titolo e' stato scelto dall'utente, e chi sceglie un titolo gia' installato lo sovrascrive.
Result ts_am_begin(u64 tid, int overwrite, Handle* h) {
    (void) overwrite;
    FS_MediaType dest = ts_title_dest(tid);
    Result rt = AM_DeleteTitle(dest, tid);
    Result rk = AM_DeleteTicket(tid);
    Result rd = 0;
    if(dest == MEDIATYPE_SD) {
        rd = AM_QueryAvailableExternalTitleDatabase(NULL);
    }

    // In piu' rispetto a FBI: se un'installazione precedente dello stesso titolo e' rimasta a meta' (crash, spegnimento) AM la tiene
    // come titolo pendente; la si toglie prima di ricominciare, cosi' non serve passare da FBI. Riguarda solo questo titolo.
    Result rp = AM_DeletePendingTitle(dest, tid);
    ts_log("pulizia prima dell'installazione di %016llx: DeleteTitle 0x%08x, DeleteTicket 0x%08x, database SD 0x%08x, DeletePendingTitle 0x%08x", (unsigned long long) tid,
           (unsigned int) rt, (unsigned int) rk, (unsigned int) rd, (unsigned int) rp);
    return AM_StartCiaInstall(dest, h);
}

Result ts_am_write(Handle h, u64 offset, const void* buf, u32 size) {
    u32 written = 0;
    Result r = FSFILE_Write(h, &written, offset, buf, size, 0);
    if(R_SUCCEEDED(r) && written != size) {
        return -1;
    }

    return r;
}

Result ts_am_finish(Handle h, const ts_title* t) {
    Result r = AM_FinishCiaInstall(h);
    if(R_FAILED(r)) {
        return r;
    }

    if(t->hasSeed) {
        return fs_add_seed(t->tid, t->seed);
    }

    return 0;
}

void ts_am_cancel(Handle h) {
    AM_CancelCIAInstall(h);
}
