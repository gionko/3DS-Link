#pragma once
#include <stddef.h>
#include <stdint.h>

#include "flow.h"
#include "proto.h"

// La stanza del mittente: il controllo di flusso (flow) piu' cio' che ogni ricevente ha dichiarato (titoli voluti, finestra) e i titoli
// inviati nel giro in corso. Codice senza dipendenze dalla console: lo usano sia il mittente sia le prove su PC (room_test).
// Prima di far partire un giro a stanza senza nessuno in linea si aspetta un poco: chi conferma quasi insieme (nuovi READY a meno di
// TS_LAP_QUIET_MS l'uno dall'altro) parte nello stesso giro invece di aspettare quello dopo. Al massimo TS_LAP_MAX_WAIT_MS dal primo in attesa.
#define TS_LAP_QUIET_MS 0 // nessuna attesa: chi entra nei primi secondi si recupera subito ("late join veloce", PROGETTO-CAROSELLO.md), non serve aspettare
#define TS_LAP_MAX_WAIT_MS 15000

typedef struct {
    ts_flow f;
    uint32_t lastEntryAt;                          // ultimo ricevente nuovo entrato (ms)
    uint32_t lateSince;                            // da quando c'e' chi aspetta e nessuno in linea (0 = no)
    uint32_t nAcks;                                // riscontri accettati (totale)
    uint8_t want[TS_MAX_CLIENTS][TS_BITMAP_BYTES]; // titoli voluti da ogni ricevente (stesso indice di f.cl)
    uint32_t cap[TS_MAX_CLIENTS];                  // finestra dichiarata da ogni ricevente, in blocchi
    uint16_t tok[TS_MAX_CLIENTS];                  // identificativo a cui si riferiscono want e cap (cambia se lo slot viene riusato)
    uint32_t inlineSince[TS_MAX_CLIENTS];          // da quando e' in linea senza aver mai mandato un riscontro (0 = non applicabile)
    uint16_t byeTok[4];                            // ultimi che hanno detto addio (un READY ancora in volo non deve farli rientrare)
    uint32_t byeAt[4];
    uint16_t byeIdent[4];                          // identificativo della console che ha salutato (un rientro entro 60 s riparte senza attese)
    int byeNext;
    uint16_t ident[TS_MAX_CLIENTS];                // identificativo stabile della console (0 = non noto): riconosce un ricevente riavviato
    uint8_t ready[TS_MAX_CLIENTS];                 // 1 dopo il primo READY
    uint8_t acked[TS_MAX_CLIENTS];                 // 1 dopo il primo riscontro (ha cominciato a ricevere)
    uint8_t lapUnion[TS_BITMAP_BYTES];             // titoli che il giro in corso invia
    int lapValid;
} ts_room;

// READY di un ricevente (anche a giro in corso): lo registra e prende cio' che vuole e la finestra, finche' non e' in linea.
// `ident` e' l'identificativo stabile della console (0 = non noto): se un'altra voce ha lo stesso `ident` e un token diverso, quella console
// e' stata riavviata e la voce vecchia si toglie subito (il token vecchio va in *replaced, altrimenti 0).
// Ritorna 1 se e' la prima volta che lo si sente, 0 se e' gia' noto, -1 se la stanza e' piena o il READY non e' valido,
// -2 se e' un READY in ritardo di qualcuno che ha appena detto addio (ignorato).
// Dopo che un ricevente ha cominciato (ha mandato un riscontro) il READY non rinnova piu' il suo "ultimo segnale": a dire se e' vivo sono i riscontri.
int room_ready(ts_room* R, const ts_manifest* m, uint16_t token, uint16_t ident, uint8_t flags, const uint8_t* bits, uint32_t now, uint16_t* replaced);
// Uscita pulita di un ricevente (BYE): lo slot si libera subito. 1 se lo conosceva.
int room_bye(ts_room* R, uint16_t token, uint32_t now);
// Riscontro. Ignorato se il ricevente non ha mai mandato il READY. 1 se accettato.
int room_ack(ts_room* R, uint16_t token, uint32_t now, uint32_t cursor, const ts_range* r, int n, int flags);
// Unione dei titoli voluti dai riceventi registrati; con onlyInline solo da quelli in linea che hanno ancora da ricevere. Ritorna quanti.
int room_union(const ts_room* R, int count, uint8_t* bits, int onlyInline);
// 1 se tutti i titoli voluti dal ricevente idx sono nel giro in corso (e ne vuole almeno uno): puo' agganciarsi senza aspettare.
int room_ride_ok(const ts_room* R, int idx);
// Toglie chi e' entrato ma non ha mai mandato un riscontro e non si fa sentire da timeoutMs (se n'e' andato prima di cominciare).
// Ritorna quanti ne ha tolti; al massimo max indici in `dropped` (identificativi).
int room_drop_unstarted(ts_room* R, uint32_t now, uint32_t timeoutMs, uint16_t* dropped, int max);
// Toglie chi ha finito e non si fa piu' sentire da timeoutMs (e' uscito dall'app): libera lo slot per chi entra dopo. Ritorna quanti ne ha tolti.
int room_drop_finished(ts_room* R, uint32_t now, uint32_t timeoutMs);
// Un ricevente messo in linea che da `timeoutMs` non manda nessun riscontro (manda solo READY: non riceve lo stato del giro, o si e' piantato)
// tratterrebbe la finestra di tutti: lo si rimanda in attesa. Ritorna quanti ne ha rimandati; in `demoted` i loro identificativi.
int room_demote_silent(ts_room* R, uint32_t now, uint32_t timeoutMs, uint16_t* demoted, int max);
// Controlla se deve partire un nuovo giro (flow_update_lap). Se parte: calcola l'unione dei titoli e gli intervalli da saltare, li passa al
// flusso e li scrive in skip (max maxSkip). Ritorna 1 se e' partito un giro; *pos = blocco di partenza, *w = finestra, *nTitles = titoli inviati.
int room_update_lap(ts_room* R, const ts_manifest* m, uint32_t now, ts_range* skip, int maxSkip, int* nSkip, uint32_t* pos, uint32_t* w, int* nTitles);
