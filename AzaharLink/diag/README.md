# Diagnostica dei giochi che si chiudono all'avvio (2026-10-23)

**Il difetto**: sulla New 3DS XL di Friend-A, con il plugin acceso, *Pac-Man & Galaga Dimensions*, *Ridge Racer 3D* e *The Legend of Zelda: Ocarina of Time 3D* si chiudono entro 5 secondi con `ErrDisp` `0xE0A01839` (livello «uso», riassunto «stato non valido», modulo OS, descrizione 57). Dal codice dei giochi (`NOTE_ANALISI_2026-10-21_BUTODEN_PACMAN.md`, sezione 5) il codice è il risultato **della `svcCreateThread` del gioco stesso**, per uno dei suoi primi thread. Il registro del plugin non dice niente perché il thread che lo scrive parte dopo circa 5 s.

**Che cosa si sa dal codice di Luma3DS** (letto il 2026-10-23, `LumaTeam/Luma3DS`): (1) l'applicazione ha il tempo CPU del core 1 a 0 finché non lo imposta `APT:SetApplicationCpuTimeLimit` (`sysmodules/pm/source/reslimit.c`); (2) i titoli «1.0» (descrittore del tempo CPU a 0 nell'exheader) partono in modalità `single`, in cui **un solo thread dell'applicazione può stare sul core 1**, e Luma gli mette il tempo CPU a 10000 all'avvio (un errore del suo codice applica il primo valore della tabella a tutti i titoli fra `0x205` e `0x562`: Ocarina `0x336`, Ridge Racer `0x33B`, Pac-Man `0x45D` ci sono); (3) l'opzione di Luma «Redirect app. syscore threads to core2» (New 3DS, `app_syscore_threads_on_core_2` in `/luma/config.ini`) fa andare sul core 2 i thread che l'applicazione chiede sul core 1 (`k11_extension/source/svc/CreateThread.c`), e in quel caso `pm` non imposta il tempo CPU. I tre giochi sono tutti dei primi mesi della 3DS; i giochi che funzionano con il plugin sulla stessa console sono più recenti. **Non è provato** che il difetto sia questo: la diagnostica serve a saperlo.

## Prova 0 - senza nessuna build nuova (due minuti, Friend-A)

Con la 1.0.2 (o la 0.1.17) già sulla console:

1. Nell'app 3DS Link spegni l'interruttore **3DS Link** (OFF; i titoli restano *abilitati* in *Enable Titles*): il plugin viene caricato ma resta fuori dal gioco (nessun buffer dei socket, nessun gancio).
2. Avvia *Ocarina of Time 3D* (o gli altri due).

| Esito | Che cosa vuol dire | Che cosa si fa |
|---|---|---|
| **Si chiude lo stesso** | non c'entrano il buffer dei socket né i ganci: è il Plugin Loader di Luma / il framework su questa console | si guarda `/luma/config.ini` e la versione di Luma di Friend-A (sotto); un plugin vuoto o un altro plugin CTRPF per lo stesso gioco dice se è solo di questa console |
| **Parte** | la causa è in quello che 3DS Link fa partendo (buffer dei socket o ganci) | prova 2 con i due interruttori |

## Prova 1 - il file di Luma (un minuto, nessuna build)

Friend-A manda `/luma/config.ini` (un file di testo) e una foto di *Settings* > *Installation check* di 3DS Link (la riga *Luma3DS* ha la versione). Si guarda `app_syscore_threads_on_core_2`.

## Prova 2 - la build di diagnostica `AzaharLink-diag.3gx` (una sessione)

Si costruisce con `bash AzaharLink/diag/build_diag.sh` (da Git Bash, con Docker): è il plugin consegnato più `-DAZL_BOOT_DIAG`, in una **copia** delle cartelle (`diag/work`), senza toccare `Build/` né il Makefile del prodotto. **Non va in `dist/` e non è un file dell'app.** Il codice è in `Sources/ctr/boot_diag.cpp` e `Includes/ctr/boot_diag.hpp`; senza il simbolo ogni funzione è vuota e il plugin consegnato è lo stesso codice (provato: stessa dimensione, 1.011.866 byte).

Che cosa scrive, **subito su scheda e riga per riga** (`/3ds/3DSLink/AzaharLink.boot.log`, rifatto a ogni avvio): le informazioni di Luma che un plugin può leggere (versione, bit di configurazione, **bit 3 = il «Redirect app. syscore threads to core2»**, bandiere del Plugin Loader, se la ridirezione è disattivata); **tutti** i limiti di risorse del processo, con corrente e massimo, compresi `priority` e **`cputime`** (il registro normale non li dice); i thread del processo con priorità e core; e per dieci secondi una riga ogni volta che cambiano il numero dei thread, il commit o il tempo CPU, con i thread nuovi (e una riga al secondo). In più **copia subito su scheda** le righe che il plugin ha già (il normale `AzaharLink.log`, con `debug=1`), che prima restavano in memoria.

Come si usa (Friend-A, o Gionko su una console sua con lo stesso gioco):

1. Copia `AzaharLink-diag.3gx` in `SD:/luma/plugins/<TID del gioco>/AzaharLink.3gx` **al posto** di quello che c'è (TID di Ocarina of Time 3D EUR `0004000000033600`, Ridge Racer 3D `0004000000033B00`, Pac-Man & Galaga Dimensions `0004000000045D00`). **Non aprire 3DS Link fra la copia e la prova**: l'app ripristina il suo plugin dove lo trova diverso.
2. Con `debug=1` nell'ini (come ce l'ha Friend-A) avvia il gioco, fallo chiudere, e manda `AzaharLink.boot.log` e `AzaharLink.log` da `SD:/3ds/3DSLink/`.
3. Se vuoi dividere ancora: crea sulla SD un file **vuoto** `/3ds/3DSLink/diag-no-hooks` (il plugin prende il buffer dei socket ma non tocca il gioco) o `diag-no-soc` (niente buffer: il plugin resta OFF per quell'avvio, come con `enabled=0`) e riavvia il gioco. Si tolgono cancellando il file.

### Come si legge `AzaharLink.boot.log` (decisioni scritte prima)

| Riga | Se dice... | Allora |
|---|---|---|
| `luma: config bits` | bit 3 acceso (`0x8` nel valore) e `thread redirection disabled = 0` | Luma sposta i thread del core 1 sul core 2: il difetto è in quel meccanismo. Si prova a spegnere l'opzione nel menu di avvio di Luma (tenere SELECT all'avvio); se i giochi partono, la causa è quella e si decide se il plugin deve fare qualcosa (avvisarlo nell'*Installation check* e nel README) |
| `limits` → `cputime` | `0/…` all'inizio e nessun thread sul core 1 nell'elenco | il gioco chiede il core 1 senza tempo CPU: nessuna colpa del plugin, ma va capito perché con il plugin va diversamente |
| `threads` all'inizio | un thread con `ideal core 1` che non è del gioco | quel thread (del framework o di Luma) occupa l'unico posto del core 1 dei titoli «single»: si guarda chi è (priorità, pila) e si decide |
| `watch ... new thread` | il gioco crea alcuni thread e poi il file si ferma | l'ultimo thread creato dice quale chiamata fallisce; il successivo (che manca) è quello che il kernel rifiuta |
| nessuna riga dopo `---- before ...` | il processo muore prima ancora di `after the plugin's start-up` | il difetto è dentro `EarlyInit` (buffer dei socket o ganci): provare `diag-no-soc` poi `diag-no-hooks` |
| `svcGetThreadList failed` | il processo non può chiamarla | restano i limiti e la riga `watch` col conteggio dei thread |

Quando si sa la causa si decide la correzione (non prima) e, se serve, la si porta nel prodotto con una versione nuova di tutto (regola dell'unica versione).
