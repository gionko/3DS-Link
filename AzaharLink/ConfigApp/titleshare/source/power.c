#include "power.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "net.h"

static aptHookCookie gCookie;
static int gHooked, gLcdOk, gScreensOff;
static uint64_t gLastHomeLog, gLastSys;
static int gPtmu, gMcu;
static volatile int gShell = -1;
static Thread gThread, gLogThread;
static volatile int gThreadStop;

// Una chiamata di sistema che dura a lungo si scrive nel registro (solo con il Debug log acceso): una volta l'interfaccia si e' fermata ~10 s nello stesso momento
// in cui il ricevente cercava la stanza, e il registro non diceva in quale chiamata aspettava.
static void note_slow(const char* what, uint64_t since) {
    const uint64_t ms = ts_now_ms() - since;
    if(ms >= 300) {
        ts_log("chiamata lenta: %s %llu ms", what, (unsigned long long) ms);
    }
}

static int battery(char* out, size_t size) {
    u8 lvl = 0, chg = 0;
    int haveChg = gPtmu && R_SUCCEEDED(PTMU_GetBatteryChargeState(&chg));
    if(gMcu && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&lvl))) {
        snprintf(out, size, "%u%%%s", (unsigned int) lvl, haveChg ? (chg ? " in carica" : " a batteria") : "");
        return 1;
    }

    if(gPtmu && R_SUCCEEDED(PTMU_GetBatteryLevel(&lvl))) {
        snprintf(out, size, "livello %u/5%s", (unsigned int) lvl, haveChg ? (chg ? " in carica" : " a batteria") : "");
        return 1;
    }

    snprintf(out, size, "n.d.");
    return 0;
}

uint32_t ts_mem_heap_free(void) {
    struct mallinfo mi = mallinfo();
    uint32_t total = envGetHeapSize();
    uint32_t arena = (uint32_t) mi.arena;
    return (total > arena ? total - arena : 0) + (uint32_t) mi.fordblks;
}

// canInitLcd: solo il thread principale puo' aprire il servizio dei backlight (il thread del controllo periodico lo trova gia' aperto o lo salta).
static void log_system(const char* tag, int canInitLcd) {
    if(!ts_log_active()) {
        return; // niente registro: nessuna delle ~10 chiamate di sistema che servono a scrivere la riga
    }

    static const char* models[] = {"3DS", "3DS XL", "New 3DS", "2DS", "New 3DS XL", "New 2DS XL"};
    u8 model = 0xFF;
    if(R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemModel(&model);
        cfguExit();
    }

    FS_ArchiveResource sd;
    memset(&sd, 0, sizeof(sd));
    FSUSER_GetSdmcArchiveResource(&sd);
    struct mallinfo mi = mallinfo();
    u8 ch = 0, shell = 0xFF;
    {
        const uint64_t tUds = ts_now_ms();
        udsGetChannel(&ch);
        note_slow("udsGetChannel", tUds);
    }

    if(gPtmu) {
        PTMU_GetShellState(&shell);
    }

    char bat[40];
    battery(bat, sizeof(bat));
    u32 brTop = 0, brBot = 0;
    u8 volt = 0;
    int haveBr = 0;
    if(!gLcdOk && canInitLcd && R_SUCCEEDED(gspLcdInit())) {
        gLcdOk = 1;
    }

    if(gLcdOk) {
        haveBr = R_SUCCEEDED(GSPLCD_GetBrightness(GSPLCD_SCREEN_TOP, &brTop)) && R_SUCCEEDED(GSPLCD_GetBrightness(GSPLCD_SCREEN_BOTTOM, &brBot));
    }

    int haveV = gMcu && R_SUCCEEDED(MCUHWC_GetBatteryVoltage(&volt));
    ts_log("sistema (%s): modello %s | heap totale %.1f MiB, libero %.1f MiB (cache %d%% = %.1f MiB) | lineare totale %.1f MiB | RAM app %.1f/%.1f MiB liberi, linear %.1f MiB liberi, heap usato %.1f liberi %.1f MiB | SD libera %.2f su %.2f GiB (cluster %u B) | canale UDS %u | batteria %s (tensione grezza %s%u) | luminosita' sup %s%u inf %s%u | wifi %u/3 | conchiglia %u",
           tag, model < 6 ? models[model] : "?", (double) envGetHeapSize() / 1048576.0, (double) ts_mem_heap_free() / 1048576.0, TS_CACHE_PCT, (double) ts_mem_heap_free() * TS_CACHE_PCT / 100.0 / 1048576.0, (double) envGetLinearHeapSize() / 1048576.0, (double) osGetMemRegionFree(MEMREGION_APPLICATION) / 1048576.0, (double) osGetMemRegionSize(MEMREGION_APPLICATION) / 1048576.0,
           (double) linearSpaceFree() / 1048576.0, (double) mi.uordblks / 1048576.0, (double) mi.fordblks / 1048576.0, (double) sd.freeClusters * sd.clusterSize / 1073741824.0,
           (double) sd.totalClusters * sd.clusterSize / 1073741824.0, (unsigned int) sd.clusterSize, (unsigned int) ch, bat, haveV ? "" : "n.d. ", haveV ? (unsigned int) volt : 0, haveBr ? "" : "n.d. ", haveBr ? (unsigned int) brTop : 0, haveBr ? "" : "n.d. ", haveBr ? (unsigned int) brBot : 0,
           (unsigned int) osGetWifiStrength(), (unsigned int) shell);
}

void ts_power_log_system(const char* tag) {
    log_system(tag, 1);
}

// Lo stato della conchiglia si chiede qui, in un thread a parte: l'interfaccia (che chiama ts_power_poll a ogni giro) non aspetta piu' la chiamata di sistema e legge solo il risultato
// (gShell). Il thread non ferma mai il trasferimento: dorme fra un giro e l'altro.
static void power_thread(void* arg) {
    (void) arg;
    uint64_t lastShell = 0;
    while(!gThreadStop) {
        uint64_t now = ts_now_ms();
        // stato della conchiglia: si registra ogni cambiamento (serve a capire cosa succede quando la si chiude)
        if(gPtmu && now - lastShell >= 500) {
            lastShell = now;
            u8 sh = 0;
            Result r = PTMU_GetShellState(&sh);
            note_slow("PTMU_GetShellState", now);
            if(R_SUCCEEDED(r) && sh != gShell) {
                ts_log("conchiglia: %s (stato %u)", sh ? "aperta" : "chiusa", (unsigned int) sh);
                gShell = sh;
            }
        }

        svcSleepThread(50ULL * 1000000ULL);
    }
}

// La riga "sistema" periodica ha un thread suo (solo con il Debug log acceso): chiede la SD e il servizio della rete, e puo' aspettare anche secondi (la SD occupata da AM, una
// ricerca della stanza): la conchiglia non aspetta con lei.
static void sys_log_thread(void* arg) {
    (void) arg;
    while(!gThreadStop) {
        const uint64_t now = ts_now_ms();
        if(now - gLastSys >= 30000) {
            gLastSys = now;
            log_system("periodico", 0);
            note_slow("riga sistema (periodico)", now);
        }

        svcSleepThread(250ULL * 1000000ULL);
    }
}

static void power_thread_stop(void) {
    gThreadStop = 1;
    if(gThread != NULL) {
        threadJoin(gThread, U64_MAX);
        threadFree(gThread);
        gThread = NULL;
    }

    if(gLogThread != NULL) {
        threadJoin(gLogThread, U64_MAX);
        threadFree(gLogThread);
        gLogThread = NULL;
    }
}

static void on_apt(APT_HookType hook, void* param) {
    (void) param;
    static const char* names[APTHOOK_COUNT] = {"sospesa", "ripresa", "in sleep", "risvegliata", "in uscita"};
    ts_log("APT: app %s", hook >= 0 && hook < APTHOOK_COUNT ? names[hook] : "?");
    if(hook == APTHOOK_ONWAKEUP || hook == APTHOOK_ONRESTORE) {
        gScreensOff = 0; // il sistema riaccende i backlight
    }
}

static void screens(int on) {
    if(!gLcdOk && R_SUCCEEDED(gspLcdInit())) {
        gLcdOk = 1;
    }

    if(!gLcdOk) {
        return;
    }

    if(on) {
        GSPLCD_PowerOnAllBacklights();
        gScreensOff = 0;
    } else {
        GSPLCD_PowerOffAllBacklights();
        gScreensOff = 1;
    }
}

static void log_versions(void) {
    char v[40] = "?";
    OS_VersionBin nver, cver;
    if(R_SUCCEEDED(cfguInit())) {
        osGetSystemVersionDataString(&nver, &cver, v, sizeof(v));
        cfguExit();
    }

    s64 luma = 0;
    svcGetSystemInfo(&luma, 0x10000, 0); // versione di Luma3DS (0 se non c'e')
    ts_log("versioni: sistema %s | Luma 0x%08x | kernel 0x%08x | firm 0x%08x", v, (unsigned int) luma, (unsigned int) osGetKernelVersion(), (unsigned int) osGetFirmVersion());
}

void ts_power_begin_transfer(void) {
    if(!gHooked) {
        aptHook(&gCookie, on_apt, NULL);
        gHooked = 1;
    }

    if(!gPtmu && R_SUCCEEDED(ptmuInit())) {
        gPtmu = 1;
    }

    if(!gMcu && R_SUCCEEDED(mcuHwcInit())) {
        gMcu = 1;
    }

    gShell = -1;
    aptSetSleepAllowed(false);
    aptSetHomeAllowed(false);
    ts_log("APT: sleep e HOME disattivati per il trasferimento (ptmu %d, mcu %d)", gPtmu, gMcu);
    log_versions();
    ts_power_log_system("avvio");
    gLastSys = ts_now_ms();
    if(gThread == NULL) {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        gThreadStop = 0;
        gThread = threadCreate(power_thread, NULL, 32 * 1024, prio < 0x3F ? prio + 1 : prio, -2, false); // (un po' meno importante dell'interfaccia)
        if(gThread == NULL) {
            ts_log("controllo della conchiglia: thread non avviato (la conchiglia non si controlla)");
        }
    }

    if(gLogThread == NULL && ts_log_active()) {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        gThreadStop = 0;
        gLogThread = threadCreate(sys_log_thread, NULL, 32 * 1024, prio < 0x3F ? prio + 1 : prio, -2, false);
    }
}

void ts_power_end_transfer(void) {
    power_thread_stop();
    if(gScreensOff) {
        screens(1);
    }

    gShell = -1; // lo stato della conchiglia vale solo durante un trasferimento: altrimenti il menu potrebbe restare senza disegno

    aptSetSleepAllowed(true);
    aptSetHomeAllowed(true);
}

int ts_power_poll(void) {
    const uint64_t t0 = ts_now_ms();
    int run = aptMainLoop() ? 1 : 0;
    note_slow("aptMainLoop", t0);
    // (lo stato della conchiglia e la riga "sistema" periodica li chiede power_thread)
    if(aptCheckHomePressRejected()) {
        const uint64_t now = ts_now_ms();
        uint64_t t = now;
        if(t - gLastHomeLog > 2000) {
            gLastHomeLog = t;
            ts_log("APT: tasto HOME ignorato durante il trasferimento");
        }
    }

    return run;
}

void ts_power_toggle_screens(void) {
    screens(gScreensOff ? 1 : 0);
}

int ts_power_screens_off(void) {
    return gScreensOff;
}

int ts_power_skip_drawing(void) {
    return gScreensOff || gShell == 0;
}

void ts_power_exit(void) {
    power_thread_stop();
    if(gScreensOff) {
        screens(1);
    }

    if(gLcdOk) {
        gspLcdExit();
        gLcdOk = 0;
    }

    if(gHooked) {
        aptUnhook(&gCookie);
        gHooked = 0;
    }

    if(gPtmu) {
        ptmuExit();
        gPtmu = 0;
    }

    if(gMcu) {
        mcuHwcExit();
        gMcu = 0;
    }
}
