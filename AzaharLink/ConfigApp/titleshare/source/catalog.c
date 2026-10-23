#include "catalog.h"

#include "catcache.h"
#include "hostcfg.h"
#include "log.h"
#include "ncchname.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint8_t catalog_kind(u64 tid) {
    switch((u32) (tid >> 32)) {
        case 0x00040000:
            return TS_KIND_APP;
        case 0x0004000e:
            return TS_KIND_UPDATE;
        case 0x0004008c:
            return TS_KIND_DLC;
        default:
            return TS_KIND_OTHER;
    }
}

// Chiave di raggruppamento: gioco, aggiornamento e DLC dello stesso gioco condividono i 24 bit alti dell'ID basso.
static u64 group_key(u64 tid) {
    if(catalog_kind(tid) == TS_KIND_OTHER) {
        return tid;
    }

    return (tid & 0xFFFFFFFFULL) >> 8;
}

// Metodo 1: lo fa il sistema (nessuna chiave); puo' non essere consentito a un programma lanciato da hbmenu.
static int read_name_fs(u64 tid, int lang, char* out, size_t size, Result* err) {
    out[0] = 0;

    u32 archivePath[4] = {(u32) (tid & 0xFFFFFFFF), (u32) (tid >> 32), MEDIATYPE_SD, 0};
    static const u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000}; // ExeFS "icon"
    FS_Path ap = {PATH_BINARY, sizeof(archivePath), archivePath};
    FS_Path fp = {PATH_BINARY, sizeof(filePath), filePath};

    Handle h = 0;
    Result r = FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, ap, fp, FS_OPEN_READ, 0);
    if(R_FAILED(r)) {
        *err = r;
        return -1;
    }

    // Intestazione + i 16 blocchi dei titoli (ognuno: descrizione breve, lunga, editore).
    static u8 smdh[8 + 16 * 0x200];
    u32 got = 0;
    r = FSFILE_Read(h, &got, 0, smdh, sizeof(smdh));
    FSFILE_Close(h);
    if(R_FAILED(r) || got != sizeof(smdh) || memcmp(smdh, "SMDH", 4) != 0) {
        *err = R_FAILED(r) ? r : -2;
        return -2;
    }

    static const u8 fallback[] = {1, 0, 2, 3, 4, 5, 8, 9};
    const u16* title = NULL;
    u8 order[1 + sizeof(fallback)];
    order[0] = (u8) (lang >= 0 && lang < 16 ? lang : 1);
    memcpy(order + 1, fallback, sizeof(fallback));
    for(size_t i = 0; i < sizeof(order); i++) {
        const u16* t = (const u16*) (smdh + 8 + 0x200 * order[i]);
        if(t[0] != 0) {
            title = t;
            break;
        }
    }

    if(title == NULL) {
        return -3;
    }

    u8 tmp[0x100] = {0};
    ssize_t n = utf16_to_utf8(tmp, title, sizeof(tmp) - 1);
    if(n <= 0) {
        return -4;
    }

    for(ssize_t i = 0; i < n; i++) {
        if(tmp[i] == '\n' || tmp[i] == '\r') {
            tmp[i] = ' ';
        }
    }

    snprintf(out, size, "%s", (char*) tmp);
    return 0;
}

// Il carattere della console non ha le lettere accentate: si riducono ad ASCII (Pokemon, non Pok?mon).
static void ascii_fold(char* s) {
    static const char* map = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"; // U+00C0..U+00FF
    unsigned char* in = (unsigned char*) s;
    char* out = s;
    while(*in) {
        if(*in < 0x80) {
            *out++ = (char) *in++;
        } else if(in[0] == 0xC3 && in[1] >= 0x80 && in[1] <= 0xBF) {
            *out++ = map[in[1] - 0x80];
            in += 2;
        } else if(in[0] == 0xE2 && in[1] == 0x80 && (in[2] == 0x98 || in[2] == 0x99)) {
            *out++ = '\'';
            in += 3;
        } else if(in[0] == 0xE2 && in[1] == 0x80 && (in[2] == 0x9C || in[2] == 0x9D)) {
            *out++ = '"';
            in += 3;
        } else if(in[0] == 0xE2 && in[1] == 0x80 && (in[2] == 0x93 || in[2] == 0x94)) {
            *out++ = '-';
            in += 3;
        } else {
            *out++ = '?';
            in++;
            while((*in & 0xC0) == 0x80) {
                in++;
            }
        }
    }

    *out = 0;
}

int catalog_read_name(ts_ctx* c, ts_catalog* cat, u64 tid, int language, char* out, size_t size) {
    static int fsWorks = -1; // -1 = non ancora provato, 0 = non funziona, 1 = funziona
    out[0] = 0;

    if(fsWorks != 0) {
        Result err = 0;
        if(read_name_fs(tid, language, out, size, &err) == 0) {
            fsWorks = 1;
            ascii_fold(out);
            return 0;
        }

        if(cat->fsError == 0) {
            cat->fsError = err;
        }

        // se il metodo non ha mai funzionato e abbiamo le chiavi, si smette di provarlo
        if(fsWorks < 0 && c->haveNcchKey) {
            fsWorks = 0;
        }
    }

    if(c->haveKeys && c->haveNcchKey && ncch_read_name(c->rk, c->keyX2C, c->ctrRoot, tid, language, out, size) == 0) {
        cat->usedNcch++;
        ascii_fold(out);
        return 0;
    }

    return -1;
}

void catalog_label(const ts_item* it, char* out, size_t size) {
    if(it->kind == TS_KIND_APP) {
        snprintf(out, size, "%s", it->name);
    } else if(it->kind == TS_KIND_UPDATE) {
        snprintf(out, size, "%.30s (aggiornamento)", it->name);
    } else if(it->kind == TS_KIND_DLC) {
        u32 low = (u32) (it->tid & 0xFF);
        if(low != 0) {
            snprintf(out, size, "%.34s (DLC %02X)", it->name, (unsigned int) low);
        } else {
            snprintf(out, size, "%.38s (DLC)", it->name);
        }
    } else {
        snprintf(out, size, "%s", it->name);
    }
}

static int cmp_games(const void* a, const void* b) {
    return strcasecmp(((const ts_game*) a)->name, ((const ts_game*) b)->name);
}

// Riepilogo della costruzione del catalogo, per il registro: il catalogo si costruisce PRIMA che il registro si apra (avvio dell'app), quindi i numeri si tengono qui
// e si scrivono all'apertura del registro di invio o ricezione (catalog_log_summary). Strumentazione: non influenza la costruzione.
static struct {
    int built;         // 1 dopo catalog_build
    int titles;        // titoli sulla SD
    int games, named, tried, usedNcch, cacheHits, cacheMisses;
    int amTried;       // 1 se si e' chiesto ad AM le versioni in un colpo solo
    Result amRes;      // risultato di AM_GetTitleInfo (se tentato)
    int verBad;        // titoli di cui AM non ha dato la versione nemmeno uno per uno (rotti: per loro la cache non si usa)
    int cacheLoaded;   // voci caricate da catalog.cache
    int cacheSaved;    // -1 = non serviva salvarla, 0 = salvata, 1 = salvataggio fallito
    Result fsError;    // primo errore del metodo "chiedi al sistema" (0 = nessuno)
} gCatLog;

void catalog_log_summary(void) {
    if(!gCatLog.built) {
        ts_log("catalogo: non costruito");
        return;
    }

    char am[64];
    if(!gCatLog.amTried) {
        snprintf(am, sizeof(am), "non_tentata");
    } else if(R_SUCCEEDED(gCatLog.amRes)) {
        snprintf(am, sizeof(am), "ok");
    } else {
        snprintf(am, sizeof(am), "errore_0x%08x/uno_per_uno(rotti=%d)", (unsigned int) gCatLog.amRes, gCatLog.verBad);
    }

    ts_log("catalogo: titoli=%d giochi=%d nomi=%d/%d dalla_cache=%d riletti=%d versioni_AM=%s cache_caricata=%d cache_salvata=%s ncch=%d fs_err=0x%08x", gCatLog.titles, gCatLog.games,
           gCatLog.named, gCatLog.tried, gCatLog.cacheHits, gCatLog.cacheMisses, am, gCatLog.cacheLoaded,
           gCatLog.cacheSaved < 0 ? "non_serviva" : (gCatLog.cacheSaved == 0 ? "si" : "FALLITO"), gCatLog.usedNcch, (unsigned int) gCatLog.fsError);
}

int catalog_build(ts_ctx* c, ts_catalog* cat, void (*progress)(int done, int total)) {
    memset(cat, 0, sizeof(*cat));
    cfguInit();
    u8 lang8 = 1;
    CFGU_GetSystemLanguage(&lang8);
    int lang = lang8;

    int n = (int) c->titleCount;
    cat->items = calloc((size_t) (n > 0 ? n : 1), sizeof(ts_item));
    cat->games = calloc((size_t) (n > 0 ? n : 1), sizeof(ts_game));
    u64* keys = calloc((size_t) (n > 0 ? n : 1), sizeof(u64));
    char (*own)[48] = calloc((size_t) (n > 0 ? n : 1), 48); // nome letto dall'icona di ogni titolo
    if(cat->items == NULL || cat->games == NULL || keys == NULL || own == NULL) {
        free(keys);
        free(own);
        catalog_free(cat);
        cfguExit();
        return -1;
    }

    // Cache dei nomi: la lettura dell'icona di ogni titolo e' lenta e si ripeteva a ogni avvio. Si rilegge solo cio' che e' nuovo o ha cambiato versione
    // (le versioni si chiedono ad AM in un colpo solo). Se non si riesce a sapere le versioni la cache non si usa.
    AM_TitleEntry* infos = calloc((size_t) (n > 0 ? n : 1), sizeof(AM_TitleEntry));
    uint8_t* verBad = calloc((size_t) (n > 0 ? n : 1), 1); // 1 = AM non ha dato la versione di questo titolo (un titolo rotto: la cache non si usa per lui)
    int haveVer = 0, amTried = 0, nVerBad = 0;
    Result amRes = 0;
    if(infos != NULL && verBad != NULL && n > 0) {
        amTried = 1;
        amRes = AM_GetTitleInfo(MEDIATYPE_SD, (u32) n, c->titleIds, infos);
        haveVer = R_SUCCEEDED(amRes);
        if(!haveVer) {
            // un solo titolo rotto (un'installazione interrotta a meta') fa rifiutare la chiamata per tutti, e senza versioni ogni icona si rileggeva a ogni avvio:
            // si chiede titolo per titolo, e solo i rotti restano senza versione
            haveVer = 1;
            for(int i = 0; i < n; i++) {
                if(R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &c->titleIds[i], &infos[i]))) {
                    verBad[i] = 1;
                    nVerBad++;
                    memset(&infos[i], 0, sizeof(infos[i]));
                }
            }
        }
    }

    ts_catcache oldCache, newCache;
    catcache_init(&oldCache, lang);
    catcache_init(&newCache, lang);
    // la cache sta nella cartella dei dati dell'app (hostcfg.h); senza cartella non si legge e non si salva
    char cachePath[HC_PATH_MAX];
    int haveCachePath = hc_data_path(cachePath, sizeof(cachePath), "catalog.cache") == 0;
    int cacheLoaded = 0;
    if(haveVer && haveCachePath) {
        cacheLoaded = catcache_load(&oldCache, cachePath);
    }

    cat->nItems = n;
    for(int i = 0; i < n; i++) {
        ts_item* it = &cat->items[i];
        it->tid = c->titleIds[i];
        it->kind = catalog_kind(it->tid);
        ctx_product_code(it->tid, it->code);
        if(it->kind == TS_KIND_APP || it->kind == TS_KIND_UPDATE || it->kind == TS_KIND_OTHER) {
            cat->nTried++;
            int verOk = haveVer && !verBad[i];
            uint16_t ver = verOk ? (uint16_t) infos[i].version : 0;
            const char* cached = verOk ? catcache_get(&oldCache, it->tid, ver) : NULL;
            if(cached != NULL) {
                snprintf(own[i], sizeof(own[i]), "%s", cached);
                cat->nNamed++;
                cat->cacheHits++;
            } else if(catalog_read_name(c, cat, it->tid, lang, own[i], sizeof(own[i])) == 0) {
                cat->nNamed++;
                cat->cacheMisses++;
            }

            if(verOk && own[i][0] != 0) {
                catcache_put(&newCache, it->tid, ver, own[i]); // solo i titoli installati adesso: quelli spariti escono dalla cache
            }
        }

        if(progress != NULL && (i % 8) == 0) {
            progress(i, n);
        }
    }

    int cacheSaved = -1;
    if(haveVer && haveCachePath && (cat->cacheMisses > 0 || newCache.n != oldCache.n)) {
        cacheSaved = catcache_save(&newCache, cachePath) == 0 ? 0 : 1;
    }

    catcache_free(&oldCache);
    catcache_free(&newCache);
    free(infos);
    free(verBad);

    // gruppi: il nome viene dal gioco, altrimenti dall'aggiornamento, altrimenti dal codice prodotto
    for(int i = 0; i < n; i++) {
        u64 k = group_key(cat->items[i].tid);
        int g = -1;
        for(int j = 0; j < cat->nGames; j++) {
            if(keys[j] == k) {
                g = j;
                break;
            }
        }

        if(g < 0) {
            g = cat->nGames++;
            keys[g] = k;
            cat->games[g].name[0] = 0;
        }

        ts_game* gm = &cat->games[g];
        if(gm->nMembers < CAT_MAX_MEMBERS) {
            gm->members[gm->nMembers++] = i;
        }
    }

    for(int g = 0; g < cat->nGames; g++) {
        ts_game* gm = &cat->games[g];

        // ordine dei membri: gioco, aggiornamento, DLC (per ID)
        for(int a = 0; a < gm->nMembers; a++) {
            for(int b = a + 1; b < gm->nMembers; b++) {
                const ts_item* x = &cat->items[gm->members[a]];
                const ts_item* y = &cat->items[gm->members[b]];
                if(x->kind > y->kind || (x->kind == y->kind && x->tid > y->tid)) {
                    int t = gm->members[a];
                    gm->members[a] = gm->members[b];
                    gm->members[b] = t;
                }
            }
        }

        const char* name = NULL;
        for(int m = 0; m < gm->nMembers && name == NULL; m++) {
            if(own[gm->members[m]][0] != 0) {
                name = own[gm->members[m]];
            }
        }

        if(name != NULL) {
            snprintf(gm->name, sizeof(gm->name), "%s", name);
        } else {
            // nessun nome disponibile (es. solo DLC senza gioco): si usa il codice prodotto, ripulito
            const char* code = cat->items[gm->members[0]].code;
            snprintf(gm->name, sizeof(gm->name), "%s", code[0] ? code : "(senza nome)");
        }

        for(int m = 0; m < gm->nMembers; m++) {
            snprintf(cat->items[gm->members[m]].name, sizeof(cat->items[0].name), "%s", gm->name);
        }
    }

    qsort(cat->games, (size_t) cat->nGames, sizeof(ts_game), cmp_games);

    free(keys);
    free(own);
    cfguExit();
    if(progress != NULL) {
        progress(n, n);
    }

    gCatLog.built = 1;
    gCatLog.titles = n;
    gCatLog.games = cat->nGames;
    gCatLog.named = cat->nNamed;
    gCatLog.tried = cat->nTried;
    gCatLog.usedNcch = cat->usedNcch;
    gCatLog.cacheHits = cat->cacheHits;
    gCatLog.cacheMisses = cat->cacheMisses;
    gCatLog.amTried = amTried;
    gCatLog.amRes = amRes;
    gCatLog.verBad = nVerBad;
    gCatLog.cacheLoaded = cacheLoaded;
    gCatLog.cacheSaved = cacheSaved;
    gCatLog.fsError = cat->fsError;
    return 0;
}

void catalog_free(ts_catalog* cat) {
    free(cat->items);
    free(cat->games);
    memset(cat, 0, sizeof(*cat));
}
