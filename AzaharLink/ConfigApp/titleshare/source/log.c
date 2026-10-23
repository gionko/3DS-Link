#include "log.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "buildcfg.h"
#include "hostcfg.h"
#include "logrot.h"
#include "net.h"
#include "store.h"

#define LOG_RING (128 * 1024)
#define LOG_FLUSH_MAX (32 * 1024)
#define WD_SLOTS 4

static FILE* gLog;
static uint64_t gT0;
static uint32_t gBytes; // dimensione del file corrente (a LOGROT_MAX_BYTES il file ruota: logrot.h)
static LightLock gLock;
static LightLock gFlushLock; // un solo thread alla volta in flush_once (usa un buffer statico)
static int gInit;
static volatile int gStop;
static int gOpen;

// anello in RAM
static char gRing[LOG_RING];
static uint32_t gHead, gTail; // scrittura / lettura (indici che crescono)
static uint32_t gDropped;

static Thread gFlushTh, gWdTh;

typedef struct {
    char name[16];
    volatile int used;
    volatile uint32_t beat;
    volatile int sect;
    int stalled;
    uint32_t peak;
    uint32_t reportedAt;
} wd_t;

static wd_t gWd[WD_SLOTS];

static void ring_put(const char* s, size_t n) {
    if(n > LOG_RING - (gHead - gTail)) {
        gDropped++;
        return;
    }

    for(size_t i = 0; i < n; i++) {
        gRing[(gHead + i) % LOG_RING] = s[i];
    }

    gHead += (uint32_t) n;
}

static void put_line(const char* fmt, va_list ap) {
    char line[700];
    char buf[760];
    vsnprintf(line, sizeof(line), fmt, ap);

    uint64_t t = ts_now_ms() - gT0;
    int n = snprintf(buf, sizeof(buf), "%7llu.%03u  %s\n", (unsigned long long) (t / 1000), (unsigned int) (t % 1000), line);
    if(n <= 0) {
        return;
    }

    if((size_t) n >= sizeof(buf)) {
        n = (int) sizeof(buf) - 1;
    }

    LightLock_Lock(&gLock);
    if(gOpen) {
        ring_put(buf, (size_t) n);
    }

    LightLock_Unlock(&gLock);
}

uint64_t ts_log_ms(void) {
    return gInit ? ts_now_ms() - gT0 : 0;
}

int ts_log_active(void) {
    return gInit && gOpen;
}

void ts_log(const char* fmt, ...) {
    if(!gInit || !gOpen) {
        return; // registro spento (o non ancora aperto): nessun costo
    }

    va_list ap;
    va_start(ap, fmt);
    put_line(fmt, ap);
    va_end(ap);
}

// Il file `index` del registro (0 = quello corrente), nella cartella dei dati dell'app. Ritorna 0 se il nome c'e'.
static int log_file_name(char* out, size_t size, int index) {
    return logrot_name(out, size, hc_data_dir(), index);
}

// Apre il file corrente in coda (un nuovo avvio continua il file di prima, come il registro del plugin) e ne legge la dimensione.
static void log_open_current(void) {
    char path[HC_PATH_MAX];
    gLog = NULL;
    gBytes = 0;
    if(log_file_name(path, sizeof(path), 0) != 0) {
        return;
    }

    gLog = fopen(path, "a");
    if(gLog != NULL && fseek(gLog, 0, SEEK_END) == 0) {
        long size = ftell(gLog);
        gBytes = size > 0 ? (uint32_t) size : 0;
    }
}

// Il file corrente ha raggiunto la dimensione massima: il piu' vecchio si cancella, gli altri salgono di un posto, quello corrente diventa .1 e se
// ne apre uno nuovo (il piano e' di logrot.c). Chiamata dove si scrive su file (flush_once) o all'apertura, mai da due thread insieme.
static void log_rotate(void) {
    if(gLog != NULL) {
        fclose(gLog);
        gLog = NULL;
    }

    logrot_op ops[LOGROT_MAX_OPS];
    int count = logrot_plan(ops, LOGROT_MAX_OPS);
    for(int i = 0; i < count; i++) {
        char from[HC_PATH_MAX], to[HC_PATH_MAX];
        if(log_file_name(from, sizeof(from), ops[i].from) != 0) {
            continue;
        }

        if(ops[i].remove) {
            remove(from);
        } else if(log_file_name(to, sizeof(to), ops[i].to) == 0) {
            rename(from, to);
        }
    }

    log_open_current();
    if(gLog != NULL) {
        fputs("3DS Link title delivery log (il file di prima e' .1.log)\n", gLog);
        fflush(gLog);
    }
}

// Scrive su file quello che c'e' nell'anello. Ritorna i byte scritti.
static uint32_t flush_once(void) {
    static char tmp[LOG_FLUSH_MAX];
    uint32_t n = 0;

    LightLock_Lock(&gLock);
    n = gHead - gTail;
    if(n > sizeof(tmp)) {
        n = sizeof(tmp);
    }

    for(uint32_t i = 0; i < n; i++) {
        tmp[i] = gRing[(gTail + i) % LOG_RING];
    }

    gTail += n;
    uint32_t dropped = gDropped;
    gDropped = 0;
    LightLock_Unlock(&gLock);

    if(dropped != 0) {
        ts_log("registro: %u righe perse (anello pieno, la SD non scrive)", (unsigned int) dropped);
    }

    if(n == 0 || gLog == NULL) {
        return 0;
    }

    uint64_t t0 = ts_now_ms();
    fwrite(tmp, 1, n, gLog);
    fflush(gLog);
    gBytes += n;
    uint32_t dt = (uint32_t) (ts_now_ms() - t0);
    if(dt > 500) {
        ts_log("registro: scrittura su SD lenta, %u ms per %u byte", (unsigned int) dt, (unsigned int) n);
    }

    if(logrot_due(gBytes)) {
        log_rotate();
    }

    return n;
}

void ts_log_flush(void) {
    if(!gInit || !gOpen) {
        return;
    }

    // se il thread del registro sta scrivendo (la SD puo' fermarsi per decine di secondi) non si aspetta oltre 300 ms: meglio perdere questo flush
    int got = 0;
    for(int i = 0; i < 30 && !got; i++) {
        got = LightLock_TryLock(&gFlushLock) == 0;
        if(!got) {
            svcSleepThread(10ULL * 1000000ULL);
        }
    }

    if(got) {
        flush_once();
        LightLock_Unlock(&gFlushLock);
    }
}

static void flush_fn(void* arg) {
    (void) arg;
    while(!gStop) {
        LightLock_Lock(&gFlushLock);
        flush_once();
        LightLock_Unlock(&gFlushLock);
        svcSleepThread(200ULL * 1000000ULL);
    }
}

static void wd_fn(void* arg) {
    (void) arg;
    while(!gStop) {
        uint32_t nowTk = (uint32_t) (svcGetSystemTick() >> 14);
        for(int i = 0; i < WD_SLOTS; i++) {
            wd_t* w = &gWd[i];
            if(!w->used) {
                continue;
            }

            uint32_t ageTk = nowTk - w->beat;
            uint32_t age = ageTk > 0x7FFFFFFFu ? 0 : (uint32_t) ((uint64_t) ageTk * 16384ULL * 1000ULL / SYSCLOCK_ARM11);

            if(age >= 1000) {
                if(!w->stalled) {
                    w->stalled = 1;
                    w->reportedAt = age;
                    w->peak = age;
                    ts_log("WATCHDOG %s: nessun battito da %u ms, ultima sezione %d", w->name, (unsigned int) age, w->sect);
                } else {
                    if(age > w->peak) {
                        w->peak = age;
                    }

                    if(age >= w->reportedAt + 5000) {
                        w->reportedAt = age;
                        ts_log("WATCHDOG %s: ancora fermo, %u ms, sezione %d", w->name, (unsigned int) age, w->sect);
                    }
                }
            } else if(w->stalled) {
                w->stalled = 0;
                ts_log("WATCHDOG %s: ripreso, fermo per circa %u ms (ultima sezione prima della ripresa %d)", w->name, (unsigned int) w->peak, w->sect);
            }
        }

        svcSleepThread(250ULL * 1000000ULL);
    }
}

int ts_wd_add(const char* name) {
    if(!gInit || !gOpen) {
        return -1;
    }

    for(int i = 0; i < WD_SLOTS; i++) {
        if(!gWd[i].used) {
            wd_t* w = &gWd[i];
            memset(w, 0, sizeof(*w));
            snprintf(w->name, sizeof(w->name), "%s", name);
            w->beat = (uint32_t) (svcGetSystemTick() >> 14);
            w->used = 1;
            return i;
        }
    }

    return -1;
}

void ts_wd_beat(int id, int section) {
    if(id < 0 || id >= WD_SLOTS) {
        return;
    }

    gWd[id].sect = section;
    gWd[id].beat = (uint32_t) (svcGetSystemTick() >> 14); // nessuna divisione: unita' di 16384 tick (~61 us), il watchdog le converte
}

void ts_wd_remove(int id) {
    if(id >= 0 && id < WD_SLOTS) {
        gWd[id].used = 0;
    }
}

void ts_log_open(const char* role) {
    if(!gInit) {
        LightLock_Init(&gLock);
        LightLock_Init(&gFlushLock);
        gInit = 1;
    }

    if(gOpen) {
        ts_log_close();
    }

    gT0 = ts_now_ms();
    gBytes = 0;
    gHead = gTail = 0;
    gDropped = 0;
    gStop = 0;
    memset(gWd, 0, sizeof(gWd));
    // l'interruttore e' uno solo, quello dell'app (Debug log): spento, il registro non crea nessun file e non avvia nessun thread
    if(!hc_log_enabled() || hc_data_dir()[0] == 0) {
        return;
    }

    mkdir(hc_data_dir(), 0777);
    log_open_current();
    if(gLog == NULL) {
        return;
    }

    if(logrot_due(gBytes)) {
        log_rotate(); // (un file rimasto grande da un avvio precedente)
    }

    if(gLog == NULL) {
        return;
    }

    fprintf(gLog, "3DS Link title delivery log (%s)\n", role);
    fprintf(gLog, "compilato il %s %s\n", __DATE__, __TIME__);
    time_t now = time(NULL);
    fprintf(gLog, "ora di sistema all'apertura: %s", ctime(&now)); // ctime termina con a capo
    fflush(gLog);

    LightLock_Lock(&gLock);
    gOpen = 1;
    LightLock_Unlock(&gLock);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    gFlushTh = threadCreate(flush_fn, NULL, 32 * 1024, prio + 1, -2, false);
    gWdTh = threadCreate(wd_fn, NULL, 16 * 1024, prio > 0x19 ? prio - 1 : prio, -2, false);
    if(gFlushTh == NULL) {
        ts_log("registro: thread di scrittura non creato, si scrive solo alla chiusura");
    }
}

// Dichiara nel registro quale codice lo ha prodotto (strumentazione, nessuna decisione cambia). I valori sono quelli di COMPILAZIONE: questo file e store.c / sender.c / main.c
// si ricompilano a ogni make (vedi il Makefile), quindi la riga non resta mai quella di una build vecchia. blocco_parcheggio, politica_scrittura e p1 sono sempre accesi
// nel codice: la riga serve a distinguere un registro di questo codice da uno di quello precedente (che non ha la riga).
void ts_log_corrections(int runsEff) {
    char eff[16];
    if(runsEff >= 0) {
        snprintf(eff, sizeof(eff), "%d", runsEff);
    } else {
        snprintf(eff, sizeof(eff), "-");
    }

    ts_log("correzioni: blocco_parcheggio=1 politica_scrittura=1 parkok75=%d corse_cache=%d corse_effettive=%s p1=1 strumentazione=1 compilato=\"%s %s\"", (int) TS_PARKOK_SOTTO_75, (int) TS_SND_RUNS, eff,
           __DATE__, __TIME__);
}

void ts_log_close(void) {
    if(!gInit || !gOpen) {
        return;
    }

    gStop = 1;
    if(gWdTh != NULL) {
        threadJoin(gWdTh, U64_MAX);
        threadFree(gWdTh);
        gWdTh = NULL;
    }

    if(gFlushTh != NULL) {
        threadJoin(gFlushTh, U64_MAX);
        threadFree(gFlushTh);
        gFlushTh = NULL;
    }

    while(flush_once() != 0) {
    }

    LightLock_Lock(&gLock);
    gOpen = 0;
    LightLock_Unlock(&gLock);

    if(gLog != NULL) {
        fclose(gLog);
        gLog = NULL;
    }
}
