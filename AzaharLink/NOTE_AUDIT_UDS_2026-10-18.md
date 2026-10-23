# Audit di tutte le risposte UDS contro il firmware (2026-10-18, dopo Tekken)

**Perché.** Tekken usciva dalla rete 100 ms dopo il join perché `GetApplicationData` rispondeva 0 byte a un client (la risposta giusta sta nel beacon dell'host). L'utente: il problema del plugin è che non risponde a tutto ciò che i giochi si aspettano; serve un controllo sistematico di quali chiamate possono arrivare e di come rispondiamo.

**Come è stato fatto** (sola lettura, nessuna prova su console):

* **Sei revisori in parallelo**, uno per gruppo di comandi: (A) ciclo di vita e hosting; (B) client, stato della connessione, informazioni sui nodi; (C) scansione e beacon; (D) percorso dei dati (Bind, PullPacket, SendTo); (E) `ndm:u`, stato condiviso e servizi vicini; (F) cosa si sa dagli altri emulatori (Azahar, Citra, Lime3DS) e da libctru. Più un **revisore critico** che ha riletto tutto: 45 segnalazioni, **24 confermate**, 11 scartate (motivo scritto per ognuna), 12 buchi di copertura indicati.
* **Terreno di verità: il comportamento del vero modulo `nwm` del firmware** (O3DS), oltre a 3dbrew, al sorgente di libctru (`uds.c`) e a quello di Azahar. Per i punti decisivi l'ho ricontrollato io (per esempio `GetNodeInformation`: l'uscita è azzerata, la prima delle 16 caselle con l'id richiesto viene copiata, **nessun caso speciale per 0**).
* **Censimento delle chiamate reali** (tutti i log di hardware che abbiamo: Tekken, Mario Party 100, Tri Force Heroes, FFE): i giochi chiamano **21 comandi** `nwm::UDS`: `0x03 0x04 0x07 0x08 0x09 0x0a 0x0b 0x0d 0x0f 0x10 0x11 0x12 0x13 0x14 0x17 0x1a 0x1b 0x1d 0x1e 0x1f 0x21`. **Mai visti** in nessuna cattura (si esercitano solo con libctru o giochi che non abbiamo): `0x01 0x02 0x05 0x06 0x0c 0x0e 0x15 0x16 0x18 0x19 0x1c 0x20 0x22 0x23`. Un gioco che non è fra i quattro può quindi usare un percorso che non ha mai girato su una console.

## Corretti in questo giro (plugin, con test sul PC)

Tutti con un test che **fallisce se si toglie la correzione** (controllo di mutazione fatto) e un pin in `Tests/policy_check.sh` con il suo mutante.

| id | comando | cosa succedeva | ora |
| --- | --- | --- | --- |
| (Tekken) | `0x11` GetApplicationData, client e spettatore | 0 byte (il Connect vecchio non porta il dato) → il gioco usciva | il dato del beacon dell'host, come il firmware (tappa K) |
| GB-02 | `0x0D` GetNodeInformation(0) | tre risposte diverse secondo lo stato (il nostro nodo da fermi, zeri con un posto libero, **errore** con la lobby piena); Mario Party lo chiede ~57 volte al secondo | sempre successo con zeri (la casella vuota del firmware ha id 0) |
| GB-05 | `0x0B` changed_nodes di un client | con due Logoff prima che il gioco legga lo stato, il secondo **sovrascriveva** il primo bit (il firmware fa OR; l'evento di stato è a singolo colpo) → l'SDK non chiede mai del primo nodo | OR: restano tutti e due i bit |
| F1+GB-06 | `0x1E`/`0x09` Connect, client ancora in `Connecting` | l'host manda il Logoff di ogni ammissione in broadcast e il client prendeva **il primo che sentiva**: due join nello stesso round trip → stesso id nodo, la lobby non si forma (lo stesso sintomo di Azahar, PR 76 di Emerald-3DS) | il client prende solo il Logoff la cui casella contiene il **suo** nodo (seed e nome); un Logoff che non elenca la casella è accettato come prima |
| A3+E-02 | `0x10` SetApplicationData(NULL, 0) | errore (`0xE10113E9`) e il vecchio dato restava annunciato | successo, il dato si svuota (il firmware non vede il puntatore: l'unica regola è dimensione ≤ 0xC8) |
| A5+C6b | `0x1D`/`0x04` con canale 0 dopo una rete su un altro canale | `GetChannel` diceva 11 ma i beacon e i frame uscivano sul canale vecchio | `GetChannel` e frame sul canale di default |
| F2 | host, EAPoL-Start di un tipo sconosciuto | nessun nodo ammesso ma un Logoff con id 0 in broadcast (adottato dai client in `Connecting`) e l'evento di stato | ignorato |
| (critico) | `0x11` client, tabella dei beacon piena | con più di 15 trasmittenti sentiti il beacon dell'host (il più vecchio) veniva buttato via e la risposta tornava a 0 byte, cioè il guasto di Tekken | il beacon dell'host a cui ci si è uniti non è mai quello che cede il posto; e se si ripiega sulla struttura del Connect una riga di log lo dice |

Verifiche: `test_main` 167.716 controlli (110 nuovi), matrice dei comandi a 32 bit con due righe nuove (`0x0D` id 0, `0x10` con puntatore nullo), le **riproduzioni di tracce vere** (FFE host e client, Tri Force Heroes host, episodio del coperchio) **identiche byte per byte** a prima.

## Aperti: da decidere (consiglio mio sotto)

| rank | id | comando / stato | cosa vede il gioco | probabilità | consiglio |
| --- | --- | --- | --- | --- | --- |
| 1 | GB-01 | `0x0B`/`0x0D`, client in lobby da 3 o più | quando un altro **client** esce, gli altri non se ne accorgono (bitmask, `total_nodes`, evento) finché l'uscente non lascia la **stanza** (3 s). Il firmware confronta ogni beacon. | alta per Mario Party 4P, TFH 3P, MK7 | **non cambiare alla cieca**: il test `TestBeaconSyncShadow` lo fissa apposta e nella cattura di Mario Party 100 il beacon dell'host Azahar elencava i nodi `0x0003` mentre i Logoff avevano `0x0007` per 3 beacon di fila (un'applicazione dal beacon avrebbe tolto un giocatore vero). Strada: far mandare all'host **AzaharLink** un Logoff di aggiornamento dopo ogni rimozione (ora sicuro grazie a F1), e raccogliere altri dati con la riga di log «beacon sync (shadow)». Va provato con 3 console. |
| 5 | C1+F4+D9 | `0x0F`/`0x22` scansione con wlancommid/id8 | il filtro per gioco **non è applicato** (si conta soltanto): in una stanza dove gira un altro gioco, Tekken vede anche quelle lobby. 39 scansioni su 39 nei log hanno parole uguali (Tekken `00069f10/01`, TFH `00177010/02`, FFE `0016ed10/30`, MP `001c4d10/01`) e la condizione scritta nel README per accenderlo è soddisfatta | media in stanze condivise | **accenderlo** (`scan_filter_enforced_ = true`, i valori jolly `0xFFFFFFFF`/`0xFF` restano jolly); cambia un default fissato da `policy_check.sh` e non è mai stato provato su una console: lo faccio se l'utente dice sì |
| 6 | F3 | `0x1D`/`0x04` BeginHosting | torna in microsecondi; sul firmware la scansione dei canali dura centinaia di ms. Yo-kai Watch 2 (Azahar issue 170) fa partire un thread e lo interroga a tempo zero: gli serve di trovarlo ancora in corso | media solo per certi giochi, non provata per Tekken/FFE/TFH/MP | da misurare con una traccia vera; non si fa alla cieca |
| 7 | A1+GB-08 | `0x08`/`0x03` dell'host con `Initialize` versione < 0x200 o `0x1C(0)` | l'host non manda nessun frame di chiusura e il client **non ha un controllo di beacon mancanti** (`SetBeaconMissTime` scarta l'argomento): resta «connesso» finché l'host non lascia la stanza | bassa-media (solo SDK dell'epoca di lancio; Tekken è 0x200, FFE/TFH/MP 0x400) | un controllo di beacon mancanti rischia di cacciare giocatori su internet con un po' di ritardo: serve una finestra misurata (`host_beacon_age_max` nei log); intanto **si può far mandare all'host i tre Deauth sempre** (tutti i peer della stanza sono AzaharLink o Azahar) |
| 9 | A2+GB-07 | `0x1E`/`0x09` verso un host pieno o chiuso | il client aspetta 5 s e torna `0xC92113FE` (timeout) invece di `0xC8611001` (pieno) / `0xC92113FB` (bloccato) | bassa-media (TFH chiude la lobby con la maschera 6) | l'host non può rifiutare (Azahar fa un'asserzione), quindi il codice giusto non si può produrre senza un frame di rifiuto: lasciare |
| 8 | C2 | `0x0E` DecryptBeaconData (vecchio) | pretende un primo descrittore ≥ 0x108 come `0x1F`; il vecchio SDK usa la struttura da 0x3C | bassa-media (nessuna cattura lo chiama) | verificare nel firmware la dimensione vera, poi allargare |
| 11 | E-03 | chiusura di una sessione fatta da un `Initialize` rifiutato | una seconda sessione con `Initialize` no-op che si chiude spegne il servizio | bassa | regola messa apposta (TestSessionClose); lasciare |
| 12 | D5 | `0x12`/`0x1B` con `svcDuplicateHandle` fallita | si risponde successo con un handle 0 | bassa (limite di 32 eventi) | piccolo da correggere, non fatto per non toccare il percorso dei dati |
| 13–24 | D4, A4, GB-03+F5, GB-04, C7, GB-10, A7+E-01, C3, C6a, A6+C4 | SendTo(0) con spettatore, EjectSpectators senza frame, id dello spettatore, Disconnect da host, MD5 della lista nodi, byte 0x1C-0x27 dei nodi, comando 0x02, ScanOnConnection che aspetta, maschera canali, networkID casuale e contatore | tutte di probabilità **bassa** (nessun gioco noto le tocca); elenco con test pronti nel JSON dell'audit | bassa | da fare solo se un gioco le chiede |

## Scartati dal critico (11) e perché

`GB-09` (3dbrew e firmware si contraddicono sull'evento di stato di Destroy/Disconnect: non è un difetto), `D1` (ipotesi sul bit 0 dell'id di Bind: una verifica la chiude), `D2` (soglia di aggregazione non modellabile senza misure su console: contro la regola dell'utente), `D3` (cancelli di 0x15/0x20 già fissati dalla matrice), `D6`, `D7`, `D8`, `E-04`..`E-07` (scelte documentate o fuori portata).

## Cosa i revisori hanno trovato che mancava, e i test da aggiungere

* **Stati popolati**: la matrice dei comandi prova lobby da 1 o 2 nodi con un solo host Azahar; mancano host con client **e** spettatore, lobby piena, lobby chiusa, client con 3 nodi, client dopo che l'host ha smesso di mandare beacon, idle **dopo** una sessione (`Teardown` non azzera `node_info_`, quindi `GetNodeInformation(1)` risponde ancora il vecchio nodo).
* **Attributi per tipo di connessione**: il bit 0 blocca gli spettatori, il bit 1 i client; oggi `AdmissionBlocked()` non distingue.
* **Passphrase e struttura del Connect**: `0x1D`/`0x1E` non leggono mai la passphrase (una sbagliata entra) e non validano il byte «initialized» né confrontano wlancommid/id8/networkID con il beacon sentito; un host sconosciuto dà 5 s di timeout invece di `0xC88113FA`.
* **Semantica degli eventi**: tutti gli eventi sono a singolo colpo (`RESET_ONESHOT`): i test devono verificare «almeno un risveglio e lo stato letto dopo è completo», non «un risveglio per cambiamento».
* **Argomenti ai bordi** (versione 0/0x100/0x1FF/0x401/0xFFFF, max_nodes 2/16/17, canali, maschere di 0x07 con flag 0/1/2, id 0/16/17/0xFFFF di 0x05 e 0x0D, capacità 0/size-1/size/0xC8/0xC9 di 0x11, 0x14 con buffer più piccolo del frame, 0x17 con size 0 o 0x5C6).
* **Beacon grande**: una lobby da 16 nodi ha due tag cifrati; mai provata.
* **Golden mancanti**: solo FFE (client e host) e TFH (host) hanno una riproduzione di traccia vera; mancano **Tekken** (ora il test usa i byte veri del beacon, ma non una traccia completa), **Mario Party come client** di una lobby a 4 e TFH come client.
* **Controlli sul firmware da fare a tavolino** (nessuna console): dimensione del descrittore di `0x0E`; se l'evento che `Initialize` restituisce è `obj+0x10` o `+0x14`; cosa azzera la funzione di stato `0x11FBA4` allo smontaggio; cosa fa il task dopo il comando `0x02`; il controllo di ammissione contro il bit 0 per uno spettatore; cosa valida `ConnectToNetwork` nella struttura; `host_beacon_age_max` nei log esistenti per dimensionare la finestra dell'host silenzioso.

## Dati completi

Il risultato intero (45 segnalazioni con prove, test e correzione proposta per ognuna) non è pubblicato; questa nota ne è la sintesi.
