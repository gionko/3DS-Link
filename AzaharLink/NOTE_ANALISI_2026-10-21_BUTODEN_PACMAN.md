# Analisi 2026-10-21: Dragon Ball Z Extreme Butōden tra Gionko e Friend-A, Pac-Man & Galaga Dimensions e Ridge Racer 3D

Solo analisi, nessuna modifica al codice (richiesta dell'utente). Aggiornata il 2026-10-22 con i dati completi di Friend-A (sezione 5). Fonti: `test-butoden-friend-a-gionko-fails-21-10-2026/{gionko,friend-a}/` (registri `AzaharLink.log` e `AzaharLink.debug.log`, ini, cache dei titoli) e le due foto in `test-pacman-ce-friend-a/`. Le due console hanno il plugin «0.1.17, built Oct 20 2026 19:23:13» (la build precedente alla rinumerazione 1.0.0), `debug=1` tutte e due, stanza «Room-X» (server `10.139.0.1` per Gionko, `203.0.113.10` per Friend-A: stessa stanza vista da due reti), canale radio 11.

## 1. Come leggere i due registri (sono lo stesso test visto dai due lati)
I due registri non si leggono in parallelo a prima vista: l'orologio di Friend-A e' avanti di un'ora e le due sessioni sono partite a 50 minuti di distanza. Si allineano con i tempi del plugin: **tempo di Friend-A = tempo di Gionko + 577,26 s**. Prova: l'ingresso di Friend-A come client e' a 57,966 s nel registro di Gionko («a client joined») e a 635,226 s nel suo; le uscite cadono a 148,83 / 725,97 s, a 279,26 / 856,40 s, e le due scansioni e l'ingresso del terzo tentativo combaciano al decimo di secondo.

## 2. Che cosa e' successo (tre tentativi)
| # | chi ospita | chi entra | entra a (Gionko) | cade a (Gionko) | dopo | chi chiude e come |
|---|---|---|---|---|---|---|
| A1 | Gionko | Friend-A | 57,97 s | 148,83 s | 91 s | Friend-A: il gioco chiama `DisconnectNetwork` 5,0 s dopo l'ultimo pacchetto dell'host |
| A2 | Gionko | Friend-A | 234,96 s | 279,26 s | 44 s | Friend-A: idem, 5,5 s dopo l'ultimo pacchetto dell'host |
| A3 | Friend-A | Gionko | 506,43 s | 520,05 s | 14 s | Gionko: il suo gioco chiama `DisconnectNetwork` |

**La connessione, a livello di rete, riesce ogni volta**: la scansione trova l'host (`scan heard … 00169610/02`), `ConnectToNetwork -> 0` e i due nodi sono nella rete (`nodes 2/2`). Non c'e' nessun errore di autenticazione, nessun rifiuto, nessun codice d'errore UDS. Quello che l'utente vede come «errore» e' la caduta dopo 14-91 s, decisa dal gioco.

## 3. Che cosa fa il gioco sulla rete (protocollo «TKAP», canale dati 1)
Dai pacchetti del registro di debug (`FRAME tx/rx … ch1`): ogni pacchetto comincia con `54 4B 41 50` («TKAP»), poi versione, numero di sequenza, dimensione e tipo (1 = dal client, 2 = dall'host). Si legge cosi' (strumenti di lettura: `test-butoden-friend-a-gionko-fails-21-10-2026/tools/tkap_timeline.py` per i pacchetti di una finestra di tempo e `tkap_gaps.py` per i buchi di ogni tentativo):
1. il client manda ogni 170 ms un pacchetto tipo 1 da 72 byte con il suo nome («Friend-A») finche' l'host non risponde;
2. l'host risponde con un pacchetto da 24 byte (tipo 2), poi 88, 144, 232 e 144 byte: l'elenco dei giocatori;
3. regime: l'host manda ogni secondo circa un pacchetto da 184 byte (lo stato della lobby), il client risponde ogni secondo con uno da 24 byte (`18 00 01 00 08 00 01 00 08 0e ff ff`: un battito);
4. quando qualcuno agisce nella lobby c'e' un cambio di stato (pacchetto da 184 byte con `…027c0007`), il client risponde con `18 00 01 00 06 00 10 00 NN 00 58 00` (NN cresce: 02, 03, 04).
Il trasporto e' sano: sequenze contigue (nessun pacchetto perso nei due sensi per decine di secondi), RTT con la stanza 8-48 ms, perdita 0,3-1,1%.

## 4. Perche' cade: tre fatti diversi, il primo e' il piu' pesante

### 4.1 Il processo del gioco si blocca per 5-10 secondi (A1 e A3), con `debug=1`
* **A1**: l'ultimo pacchetto dell'host parte a 143,74 s e poi non ne parte piu' nessuno; Friend-A lo aspetta 5,0 s e chiude (148,83 s). Nel registro di Gionko le statistiche che escono ogni 5 s qui hanno un buco: 144,51 → 154,26 (9,7 s); nella finestra 149,4-154,3 `frame callback 0.0` e solo 70 chiamate IPC del gioco (di solito 2.700-5.000 in 5 s): **il gioco di Gionko era fermo**.
* **A3**: il client (Gionko) manda un pacchetto a 506,76 s e il successivo a 515,55 s (**8,8 s dopo**); nella finestra 509,9-514,97 `frame callback 0.0` e 244 chiamate IPC; le statistiche escono con 3 s di ritardo. Poi il suo gioco chiude (520,05 s).
* Altre finestre ferme nello stesso registro: 196-201 s (`frame callback 0.1`), 217 s (0,4), e la stessa cosa si vede **anche quando nessun client e' collegato** (196-201 s: l'host era solo): non e' legata allo scambio di pacchetti.
* Il plugin lo dice da solo sullo schermo: «SD card busy 253% / 425% / 270%, capture lost 12-27% (the log slows the game)». Il registro di debug di Gionko pesa 11,8 MB (Friend-A 17,6 MB) in 9-19 minuti, con anche 5.499 record persi e 92 marcatori LOST.
* E' lo stesso quadro gia' visto con Tekken a `debug=1` (blocchi del gioco con la cattura accesa, scheda SD a 35-60 KB/s): il gioco si ferma quando la scheda e' occupata dalla cattura. Il meccanismo preciso (il gioco legge dalla stessa SD? un blocco nel file system che ferma tutti i thread, anche quello delle statistiche?) non e' provato.
**Conclusione 4.1: con `debug=1` DBZ non e' giocabile in rete perche' ogni blocco di 5 s fa scadere il timeout di 5 s dell'altro lato.** E il test fatto per cercare un errore ha prodotto l'errore.

**Esito (utente, 2026-10-22): con il debug spento si gioca** (meglio di Tekken, peggio di Street Fighter IV): la causa principale e' confermata. Il lag di Tekken resta aperto; l'utente rimanda un'analisi approfondita (non e' il router).

### 4.2 A2: nessun blocco, ma l'host smette di parlare dopo un suo cambio di stato
A 273,685 s l'host manda il pacchetto di stato `…027c0007` (184 byte, seq 136), il client risponde (273,794 s, `…0600 1000 0300 5800`) e poi l'host **non manda piu' nulla per 5,5 s** (nessuna chiamata `SendTo`, comando 0x17, nel registro) mentre continua a ricevere i battiti del client e il suo gioco gira (`frame callback` normale, 725 chiamate UDS in 13 s = il solito sondaggio ogni 20 ms). Friend-A scade a 5,5 s e chiude. A differenza di A1 (dove il cambio di stato l'aveva chiesto il client con un pacchetto da 36 byte e l'host aveva continuato con battiti da 208 byte), qui il cambio l'ha iniziato l'host (e' la schermata in cui l'host sta scegliendo qualcosa?). **Non so se e' un comportamento normale del gioco (l'host aspetta un'azione dell'utente e in quella schermata non manda battiti: su una console vera il client avrebbe lo stesso timeout?) o un difetto nostro.** Serve sapere che cosa c'era sullo schermo di Gionko a circa 4 minuti dall'avvio del gioco (22:21 sul suo orologio), e una prova senza `debug=1` (che toglie 4.1 dal quadro).

### 4.3 Una gara nel `Bind` perde il primo pacchetto dell'host (A1): 4,2 s di ritardo, non fatale
Nel registro di Friend-A: `frame for unbound data channel 1 from node 1 (dropped 1 so far)` a 635,226 s, tre millisecondi **prima** del `Bind` del suo gioco (635,229 s). Il primo pacchetto TKAP dell'host (24 byte, seq 0) arriva insieme alla risposta di associazione, il nostro motore lo scarta perche' il canale non e' ancora legato, e il gioco dell'host lo ritrasmette solo dopo 4,2 s (da 57,976 a 62,222 s); intanto il client manda 25 volte il suo pacchetto di presentazione senza risposta. In A2 la gara non e' stata persa. **Ritardo senza danno qui, ma e' il tipo di ritardo che fa scadere un timeout piu' corto**; sulla console vera lo stesso scarto esiste per canali non legati, ma la temporizzazione e' piu' larga.

### 4.4 Rumore, non causa: i frame delle altre reti della stanza
Nella stanza c'erano anche due console con Final Fantasy Explorers (Friend-D, Friend-E) e due PC: il registro mostra decine di frame al secondo di altre reti. Il client li lascia passare («passed on, the client filter only counts») e il suo motore li scarta come «frame for unbound data channel 243 from node 1» (8 ogni 5 s, per tutta la sessione): il traffico estraneo e' sul canale 243 (probabilmente FFE) e DBZ usa l'1, quindi non si toccano. Rischio teorico: due giochi della stessa stanza sullo stesso numero di canale e dello stesso nodo si mescolerebbero, perche' il client non filtra per rete. Costo: 35 frame/s ricevuti e scartati, 2-4% di un core. Non spiega nulla di quello che e' successo.

### 4.5 Escluso
La stanza e la rete (RTT, perdita, nessun silenzio del server), il canale radio (11 per tutti), la scansione, l'associazione, la versione del plugin (la stessa su tutte e due le console), la memoria (il gioco ha 56 MB liberi), i limiti del plugin (`uds delivered/routed`, nessun `dropped` rilevante salvo il caso 4.3).

## 5. Pac-Man & Galaga Dimensions, Ridge Racer 3D e Ocarina of Time 3D: lo stesso errore all'avvio
Dati: tre foto `ErrDisp` in `test-pacman-ce-friend-a/`, `AzaharLink.mem`/`.log`/`.debug.log`/`.sleep.log` della console di Friend-A (New 3DS, plugin 0.1.17 del 20 ottobre) e, dal 2026-10-22, i due giochi Ocarina of Time 3D (EUR) e Ridge Racer 3D (W), a disposizione (Pac-Man no). Schermata `ErrDisp` «generic», **codice 0xE0A01839** in tutte e tre:

| gioco | processo | Title ID | indirizzo |
|---|---|---|---|
| Pac-Man & Galaga Dimensions | `pandg` (id 88) | 0004000000045D00 | 0x00123778 |
| Ridge Racer 3D | `RIDGE RA` (id 53 e 62: due tentativi) | 0004000000033B00 | 0x00352FB0 (uguale nei due) |
| The Legend of Zelda: Ocarina of Time 3D | `CtrApp` (id 39) | 0004000000033600 | 0x004660F4 |

**Il codice**: `0xE0A01839` = livello 28 (uso), riassunto 5 (stato non valido), modulo 6 (OS), descrizione 57 (0x39). **Correzione**: nella versione precedente di questa nota l'avevo identificato con l'`ERR_INVALID_ADDRESS_STATE` di Citra/Azahar. Era sbagliato: quel risultato, nel sorgente di Azahar (`src/core/hle/kernel/errors.h`), ha descrizione 1013 e vale `0xE0A01BF5`; la descrizione 57 non e' nella tabella del kernel di Azahar, e quindi non si puo' leggere il codice dalla tabella dell'emulatore. Quello che segue viene dai giochi.

### Che cosa si vede nei giochi (non un'ipotesi)
* **L'indirizzo dell'errore e' un indirizzo di ritorno**, non quello della chiamata di sistema: e' l'istruzione subito dopo la chiamata alla funzione che lancia l'errore fatale. Vale per tutti e tre i giochi.
* **Le sequenze dei tre giochi sono la stessa**: una funzione dell'SDK che avvia un **thread**. Chiama una funzione che prepara la pila del thread e chiama `svcCreateThread` e ne restituisce il risultato. Poi il gioco guarda il risultato: se il riassunto e' «OutOfResource» (`(r & 0x7E00000) == 0x600000`) lo accetta; se il livello e' «status», «success» o «info» lo accetta; **qualunque altro errore e' fatale**. Quindi tutti e tre i giochi muoiono perche' **`svcCreateThread` restituisce 0xE0A01839** (OS, stato non valido, 57) per uno dei primi thread che creano.
* **Non e' un conflitto di indirizzi di memoria**: la mia ipotesi precedente (il blocco soc da 128 KB a `0x0DFE0000` dentro un intervallo chiesto con indirizzo fisso) non regge: in OoT c'e' **una sola** chiamata `svcControlMemory` e **una sola** `svcMapMemoryBlock` in tutto il codice, e l'errore non arriva da li'. Si ritira. (Resta vero che gli SDK di quel periodo hanno un allocatore di intervalli proprio: `0x0E000000` + 32 MB e `0x10000000` + 64 MB; ma non e' li' che il gioco cade.)
* Il gioco **tollera** la mancanza di risorse (troppi thread: `OutOfResource` livello status): quindi il limite di 36 thread, che il plugin gia' usa in parte (6 thread al momento della partenza), da solo non spiega l'errore: darebbe un altro codice, che questo punto del gioco accetta.
* Memoria all'avvio (`AzaharLink.mem`) e blocchi del Plugin Loader sono uguali a quelli dei giochi che partono; il Plugin Loader di Luma da solo non basta a spiegarlo (per OoT3D esistono molti plugin CTRPF che girano).
* Il plugin e' partito in tutti e tre (`AzaharLink.mem` ha le tre partenze) e il gioco muore **entro 5 s** (il registro scrive solo dopo circa 5 s; per questo non c'e' nessuna sessione di questi tre nei registri).

### Dati di OoT3D utili per la prova
Exheader: `System mode: prod (AppMemory: 64MB)`, `Ideal processor 0`, `Affinity mask 1`, priorita' del thread principale 48, `Handle table size 0x200`, memory type APPLICATION, `Name: CtrApp`; codice `.text` `0x100000` (3.971.496 byte), `.ro` `0x4CA000`, `.data` `0x4F9000`, `.bss` `0x6C3E8`, pila `0x4000`. Ridge Racer 3D: stessa famiglia (`.text` `0x100000`, 2.600.652 byte).

### Che cosa puo' far rispondere a `svcCreateThread` con «stato non valido» (ipotesi, da provare)
Il kernel risponde a `svcCreateThread` con errori che dipendono da: priorita' (richiesta e consentita dal limite di risorse), **numero di processore richiesto** (0/1/2/3: le regole per i core 2 e 3 e per il core 1 dipendono dal tipo di processo e dal limite di tempo CPU del core di sistema), limite dei thread, e dalla pila del thread. Il plugin cambia: i thread che il processo ha gia' (framework + i nostri: rete, scrittura del registro, cattura, breadcrumb), la priorita' dei nostri thread, e puo' cambiare quando il gioco vede le risposte di APT. Non so quale di questi tocca il caso: il kernel dell'emulatore non da' 57 per nessuno, quindi l'emulatore non lo mostra.

### Prove per decidere (dalla piu' economica; la 1 e' una build di diagnosi, non un cambio del prodotto)
1. **Registrare le `svcCreateThread` del gioco nei primi secondi, in modo sincrono su file** (il plugin gia' agganciava i punti `svc 0x32` del gioco: aggiungere `svc 0x08`): argomenti (punto d'ingresso, pila, priorita', numero di processore) e risultato dei primi 40 thread, scritti come il `.mem` (subito, senza passare dal registro che parte a 5 s). Dice quale thread fallisce e con quali parametri: e' la prova che chiude la questione, e serve a tutti e tre i giochi.
2. **Titolo spento in *Enable Titles*** (Friend-A): se parte senza plugin, il plugin c'entra (gia' molto probabile dai tre errori uguali).
3. **Un plugin CTRPF vuoto** (stessa struttura, nessun thread nostro, nessun aggancio) per OoT3D: se parte, la causa e' in quello che aggiungiamo; se non parte, e' il Plugin Loader di Luma di questa console.
4. Provare gli stessi giochi sulla console di Gionko (New 2DS XL) e con la build precedente del plugin: se funzionavano prima, la differenza e' in una modifica della tappa O o P (per esempio la priorita' o il numero dei thread).
5. Per sapere in futuro cosa e' successo: il registro sincrono dei primi secondi (prova 1) resta anche dopo.

## 6. Esperimenti da fare prima di toccare codice
1. **DBZ senza `debug=1` su tutte e due le console**, stanza con solo loro due (nessun FFE, nessun PC), sessione corta: se la lobby regge piu' di due minuti, 4.1 e' la causa principale. Se cade lo stesso, la causa e' 4.2/4.3 (UDS) e si riprende con un registro corto (30-60 s) fatto con `debug=1` e copiato subito.
2. Per A2: scrivere che cosa si vedeva sullo schermo di Gionko quando l'host ha cambiato stato (lobby, scelta del personaggio, «attesa dell'avversario»?).
3. Pac-Man, Ridge Racer e Ocarina of Time come al punto 5 (le prove 2-4 non richiedono nessuna build; la 1 e' una build di diagnosi).

## 7. Che cosa si potrebbe implementare dopo (non ora)
* Il blocco da 5-10 s con `debug=1`: tenere la cattura lontana dalla scheda mentre una sessione di rete e' viva (un buffer in memoria da svuotare solo a gioco in pausa, o un «debug leggero» per le prove di rete: gia' in agenda dalle note su Tekken), e dire sullo schermo che con `debug=1` i giochi con timeout corti non reggono.
* La gara nel `Bind` (4.3): tenere per qualche decina di millisecondi i frame per un canale non ancora legato subito dopo l'associazione e consegnarli al `Bind` (non e' quello che fa l'hardware, ma rende innocua la corsa).
* Il filtro per rete dei frame del client (4.4), prima che due giochi diversi sullo stesso canale si mescolino.
* Pac-Man, Ridge Racer e Ocarina of Time: da decidere dopo le prove del punto 5 (la correzione dipende da quale parametro di `svcCreateThread` fa rispondere 57: priorita', core, numero di thread).
* Il registro sincrono dei primi secondi, con le `svcCreateThread` del gioco (punto 5.1).

## 8. Cose da chiedere agli utenti
Di Pac-Man il CIA (OoT3D e Ridge Racer 3D sono gia' stati letti); che cosa c'era sullo schermo dell'host a 4 minuti (A2); se i tre giochi sono mai partiti con questo plugin; il modello della console di Friend-A e la versione di Luma (non e' nei registri del plugin, vedi `NOTE_LUMA.md`); a Gionko, se Pac-Man, Ridge Racer e Ocarina of Time partono sulla sua.
