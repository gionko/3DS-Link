#pragma once
#include <3ds.h>
#include <stdint.h>

// Orologio monotono in millisecondi dall'avvio della console. osGetTime NON va usato per le durate: e' l'ora di
// sistema e a volte fa passi indietro (visti nei log del 10-11.10.2026), il che con differenze senza segno
// produce intervalli enormi.
static inline uint64_t ts_now_ms(void) {
    return svcGetSystemTick() * 1000ULL / SYSCLOCK_ARM11;
}

// Parametri della rete UDS di TitleShare, uguali per mittente e ricevente.

#define TS_WLANCOMM_ID 0x00545348u // "TSH"
#define TS_DATA_CHANNEL 1
#define TS_SHARED_MEM 0x40000
#define TS_HOST_RECV_BUF 0x8000
#define TS_CLIENT_RECV_BUF 0x30000
#define TS_PACE_BPS (1.85 * 1048576.0) // velocita' massima di invio (byte di dati al secondo): lascia radio libera
#define TS_NET_NODES 8                  // nodi della rete UDS, il mittente compreso: 7 riceventi al massimo (la tabella del mittente, TS_S_MAX, ha un posto in piu' per un rientro)
// Finestra di ricezione, in blocchi: quanto si puo' scaricare in anticipo sull'installazione (un blocco = 1456 byte).
// Ogni ricevente tiene TS_CACHE_PCT% della propria memoria libera (fra TS_WINDOW_MIN e TS_WINDOW_MAX) e la dichiara nel READY;
// il mittente usa il minimo fra i riceventi presenti all'avvio. A ~1300 blocchi/s: 8192 = 6,4 s, 32768 = 25 s di stallo di AM coperti.
#define TS_WINDOW_MIN 4096
#define TS_WINDOW_MAX 65280 // il massimo che il READY puo' dichiarare (255 unita' da TS_WIN_UNIT = 256 blocchi, ~90 MiB)
#define TS_CACHE_PCT 75     // percentuale della memoria libera (heap) usata per la finestra del ricevente e per l'anello del mittente
#define TS_RING_MIN 2048
#define TS_RING_MAX 65536
#define TS_RECONNECT_MAX_MS 1800000 // il ricevente cerca il mittente fino a 30 minuti (il mittente toglie dalla stanza chi tace da TS_S_LOST_MS: al rientro rientra, se c'e' posto)
#define TS_PASSPHRASE "titleshare v1 3f9a1c"

#define TS_APPDATA_MAGIC 0x31485354u // "TSH1"

// Dati nel beacon della rete: chi cerca vede cosa viene offerto senza collegarsi. I primi 29 byte sono quelli di sempre (un ricevente piu' vecchio legge solo quelli:
// il nome del primo titolo, tagliato a 15 caratteri); `firstName` e' il nome intero e viene dopo (un mittente piu' vecchio non lo manda: chi legge usa i 15 caratteri).
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t titles;
    uint64_t totalBytes;
    char firstCode[16];
    char firstName[48];
} ts_appdata;
#define TS_APPDATA_OLD_SIZE 29 // (4 + 1 + 8 + 16) la dimensione prima di firstName
