# TEST UNICO — Download Play per 3DS Link (una sessione, tutte le risposte)

I test costano: questo è costruito per rispondere a **tutto ciò che a tavolino non si può sapere**, in una sola serata, con una sola build
dell'host (`DlpRawHost-MENU27.3dsx`, con un menu di **preset** incorporato: **niente cambi di file sulla SD tra un giro e l'altro**) e una sola sonda
(`ChildProbe.cia`, il figlio che misura). Ogni giro ha un esito scritto prima, e dall'esito segue una decisione.

## Cosa risponde (in ordine di peso)

| # | Domanda | Giro | Decide |
|---|---|---|---|
| Q1 | Il client di sistema accetta una rete con identità Download Play **ma con 8 nodi e 77 byte di appdata** (cioè la stanza di 3DS Link mascherata)? | 2 | **rete unica sì/no** (stanza e Download Play insieme, senza finestre) |
| Q2 | Le console **vecchie** (11.13 / 11.15, Luma 10.x) scaricano e avviano il figlio con il beacon **pulito** (senza i byte di Zelda)? | 3 | se il bootstrap vale "su qualunque console Luma" |
| Q3 | Un nodo che si comporta come un ricevente 3DS Link trova la rete dal magic nell'appdata (`TSH1`), entra come nodo normale, scambia frame, **mentre** un client di sistema scarica? | 4 | classificazione dei nodi; riceventi 3DS Link e Download Play sulla stessa rete |
| Q4 | Il carico del carosello (frame sul canale 3, 200 e 1500 al secondo (1500 = circa 2 MiB/s, il carosello vero)) rallenta o rompe il download del client di sistema? Quanto? | 4 / 4b | se serve dare priorità, o se si può tenere acceso tutto |
| Q5 | Frame estranei sul **canale dati 1** (quello che oggi usa 3DS Link) fanno ignorare l'host al client di sistema? | 4c | se 3DS Link deve spostarsi su un canale ≥ 3 |
| Q6 | Due o tre client di sistema insieme: cosa succede (beacon durante la distribuzione, ingresso in ritardo)? | 5 | se serve una coda o basta il comportamento attuale |
| Q7 | Dal figlio DLP: `romfsInit`, lettura del RomFS cifrato a chiave zero, `am:net` + installazione di un CIA, citro2d e font, tempi, console/Luma/NAND, `plg:ldr` raggiungibile, **lettura del proprio .app dalla NAND**, scrittura di 2,6 MB su SD | 1 (e ogni consegna) | il figlio vero (installatore) e come una console con 3DS Link ricevuto da DLP può **ridistribuirlo senza ricomporre niente** |
| Q8 | HOME: il pacchetto regalo compare dopo che un'app installa un CIA? HOME premuto durante l'installazione viene ignorato? | 1 | schermata finale del figlio |
| Q9 | Quanto tempo passa tra «stanza ricreata» e «titolo di nuovo in lista» sul client? (piano di ripiego con finestre) | 2 | parametri dell'alternanza |
| Q10 | Il figlio temporaneo resta dopo un riavvio? un secondo figlio lo sostituisce? | 6 | pulizia e regola di versione |

## Cosa copiare dove

* **Host H** (la console che distribuisce; la A): copiare la cartella **`3ds/DlpTest/`** (la prepari tu: la demo e il figlio) su `sdmc:/3ds/DlpTest/`
  (due file: `DlpRawHost-MENU27.3dsx` e `child.cia`, che è la sonda). **Non** deve esistere `host.cfg` in quella cartella (se c'è, il menu non compare).
  Consigliato: cancellare i vecchi `rawhost_log.txt` per avere un log pulito.
* **Nodo «peer» P** (giri 4, 4b, 4c): una quarta/quinta console con solo `DlpRawHost-MENU27.3dsx` in `sdmc:/3ds/DlpTest/`.
* **Client**: **niente da copiare**. Servono: console con Luma, ≥ 8 MB liberi sulla SD, e (solo per il giro 6) FBI.
* Ruoli suggeriti: H = A · N = B (sistema recente, Luma 13.3.3) · O1 = C (11.13, Luma 10.1.1) · O2 = D (11.15, Luma 10.2.1) · P = la New 3DS XL (o qualunque altra).

## Avvio dell'host

hbmenu → `DlpTestRawHost` → compare il **menu dei preset** (su/giù, **A** = avvia, START = esce). Tasti durante il giro: **A** = ricrea la stanza,
**B** = chiude la stanza, **START** = esce. Il preset scelto resta scritto nel log. Sul client: **Download Play** di sistema, si tocca il titolo che compare
(il nome è quello della sonda), si conferma. A fine scarico il sistema avvia da solo il figlio-sonda: si guarda lo schermo, si preme un tasto **solo quando dice di
essere finito**.

## I giri

**Attenzione ai numeri**: «GIRO n» è il giro di prova; «preset n» è la voce del menu dell'host. Preset: **0** baseline (come la build provata) · **1** rete unica · **2** solo beacon pulito (3 nodi, per le console vecchie) · **3** rete unica + carico 200 frame/s · **4** rete unica + carico 1500 frame/s · **5** più client insieme · **6** PEER canale 3 · **7** PEER canale 1.

### GIRO 1 — BASELINE, console recente (≈ 3 min) · host: preset 0 · client: N
Serve a leggere le misure del figlio con il beacon provato. **Foto** (7): lista con il titolo, conferma, barra di scarico, schermata del sistema dopo lo scarico,
schermata della sonda a metà, schermata della sonda finita, **HOME dopo l'uscita** (c'è un'icona nuova? lampeggia? il pacchetto regalo?).
**Una volta, durante l'installazione, premere HOME** e annotare se viene ignorato.
**Dopo l'uscita**: dalla HOME si puo' aprire il pacchetto regalo e lanciare l'app installata (e' il vecchio «DlpTest Host», fa da finto 3DS Link), ma **non premere R** in quell'app: chiama il `dlp:SRVR` di sistema e manda in panico il kernel (visto 4 volte, schermata rossa di Luma con processo `dlp`; non e' un guasto nostro e non danneggia niente, ma spegne la console).
Esito atteso: sonda verde, `probe_log.txt` pieno. Nessuna decisione dipende dall'esito del beacon qui: è il riferimento (tempo di contenuto di H).

### GIRO 2 — RETE UNICA, console recente (≈ 4 min) · host: preset 1 · client: N
Prima di toccare il titolo su N: lasciare N sulla lista, premere **A** sull'host **due volte**, a ~10 s una dall'altra. Annotare per ciascuna: dopo quanti secondi
il titolo sparisce dalla lista di N e dopo quanti ricompare (anche a occhio, o un video: basta). Poi scaricare normalmente. A fine consegna **B** sull'host.
* **PASSA** = il titolo compare, scarica, si avvia, la sonda scrive il log. → il client di sistema tollera 8 nodi + appdata 77 byte + beacon pulito. **Rete unica possibile.**
* **FALLISCE** = non compare / compare ma non parte. → ripetere con il preset 2 (solo beacon pulito) e, se passa, con il preset 0: il punto
  che rompe è `max_nodes=8` o `appdata`, non il beacon pulito. Annotare cosa mostra N (la foto del messaggio è oro).

### GIRO 3 — CONSOLE VECCHIE (≈ 3 min ciascuna) · host: preset 1 · client: O1, poi O2
Stessa procedura. Prima di avviare, sull'host aspettare «hosting» nel log a schermo.
* O1 e O2 passano → il beacon pulito vale anche su 11.13/11.15 e il figlio gira su Luma 10.x.
* Se uno fallisce: stesso client con il preset 2; se fallisce ancora, con il preset 0 (beacon di Zelda). Il primo che passa dice dove sta il problema
  (rete unica / beacon pulito / niente da fare su quel firmware).

### GIRO 4 — NODO «ALLA 3DS LINK» + CARICO (≈ 5 min) · host: preset 3 · peer: preset 6 · client: N
Ordine: avviare H (preset 3), poi **P** (preset 6) e **aspettare sullo schermo di P le righe `peer: connected ...` e poi `peer: +2s ...`**; solo allora N apre Download Play e scarica.
Su P annotare cosa dicono le righe `rx ... tx ...` mentre N scarica. A fine, **START su P**, **B su H**.
* **PASSA** = P trova la rete (magic `TSH1`), si collega, vede i frame dell'host sul canale 3 (rx > 0), N scarica e avvia comunque, la riga `summary` dell'host dà un tempo
  di contenuto non peggiore del 30 % rispetto al giro 2.
* Il nodo di P viene messo da parte come `foreign` sul log dell'host (non risponde come client DLP): è il comportamento voluto.

### GIRO 4b — CARICO PESANTE · host: preset 4 (1500 frame/s) · peer: preset 6 · client: N (≈ 4 min, solo se il 4 passa)
Come sopra. Dice se un carosello pieno strozza il Download Play e di quanto (tempo di contenuto, righe `noise: sent .. refused ..`).

### GIRO 4c — FRAME ESTRANEI SUL CANALE 1 · host: preset 3 · peer: preset 7 · client: N (≈ 4 min, solo se il 4 passa)
P manda 50 frame/s sul canale 1 (lo stesso del beacon). Se N perde l'host, non scarica o scarica molto peggio → 3DS Link dovrà spostarsi su un canale ≥ 3.

### GIRO 5 — PIÙ CLIENT INSIEME (≈ 5 min) · host: preset 5 · client: N, O1, O2 (quanti ce ne sono)
Tutti aprono Download Play **prima**; poi si toccano i titoli «più o meno insieme». Annotare chi entra, chi resta fermo e per quanto, chi finisce per primo.
Il log dell'host dice il resto (`late join`, `status:`, `summary`).

### GIRO 6 — IL FIGLIO TEMPORANEO (nessuna radio, ≈ 3 min) · client: N
Con FBI (solo guardare): `Titles` → cercare `0004000100FD5100` (figlio-sonda). Spegnere N, riaccendere, riguardare. Se si ha voglia: consegnare il figlio di Tetris
(`log-test-app/tetris_child.cia` come `child.cia` dell'host, preset 0) e riguardare: il figlio-sonda c'è ancora? Annotare, non cancellare niente.

## Cosa riportarmi

* Da H (e da P): `sdmc:/3ds/DlpTest/rawhost_log.txt` (tutti i giri in un solo file, con intestazione del giro).
* Da ogni client usato: `sdmc:/3ds/ChildProbe/probe_log.txt` (si **accoda** a ogni consegna) e, dal primo che lo produce, `sdmc:/3ds/ChildProbe/self.app`.
* Le foto del giro 1 (e di qualunque messaggio d'errore), e due righe di appunti: i secondi del giro 2, chi ha passato o no, cosa ha mostrato HOME.
* Se compare una schermata rossa di Luma: la foto e `sdmc:/luma/dumps/arm11/crash_dump_*.dmp`.

## Come leggo i risultati

| Se... | Allora |
|---|---|
| GIRO 2 e GIRO 4 passano e 4c non dà problemi | **Rete unica**: la stanza nasce con l'identità Download Play, un solo host, niente finestre; 3DS Link cambia canale se 4c ha dato problemi |
| GIRO 2 e 4 passano, 4c rompe | rete unica + 3DS Link su canale ≥ 3 (patch al modulo, già descritta nel rapporto 01 par. 4.3) |
| GIRO 2 passa, 4b strozza | rete unica ma con priorità alla stanza o carosello rallentato mentre c'è un client Download Play |
| GIRO 2 fallisce (con i preset 2 e 0 si isola la causa) | **alternanza**: finestra Download Play a stanza vuota (M4/M2 del rapporto 01), tempi dal giro 2 |
| GIRO 3 fallisce su un firmware | quel firmware resta fuori dal bootstrap, o beacon alternativo (con frame 4); si decide col dato del preset 0 |
| Q7: `self.app` identico al figlio consegnato | una console che ha ricevuto 3DS Link da DLP può **ridistribuire senza ricomporre**: basta copiare il proprio .app e avvolgerlo in una CIA |
| Q7: `am:net`/`romfsInit` falliscono | il figlio va rivisto prima di costruire altro (ripiego: 8 byte nell'ACI, rapporto 05 par. 3.3) |

## Dopo la sessione: roba da ripulire (si aggiunge a `DlpTest/README.md` → «DA PULIRE»)

* Titoli: `0004000100FD5100` (figlio-sonda, temporaneo) e, sulle console client, `0004000000FD1100` (il CIA «interno» che la sonda installa sulla SD, titolo **e** ticket: con FBI).
* File: `sdmc:/3ds/ChildProbe/` (log, `self.app`, copie di prova, tutto cancellabile), `sdmc:/3ds/DlpTest/` (host, peer).
* La sonda **non** scrive in `sdmc:/3ds/3DSLink/` (controllato nel sorgente: il file «plugin» finto va in `ChildProbe/`).

---

## AGGIORNAMENTO 2026-10-18 sera: il caso delle console vecchie (nuovi preset 8-11)

Visto sul campo: con il beacon di Zelda (preset 0) la console vecchia **chiede di aggiornare il firmware**: il beacon annuncia «sistema 11.17.0» e lei e' piu' indietro.
Col beacon pulito (versione 0, niente frame 4) non chiede niente ma **si collega alla rete e non risponde mai**, in «attendi». Quindi la versione annunciata conta.
Il host ora ha `cver=` (versione di sistema annunciata, frame 0 +0x22 e voci del frame 4) e `f4=` (frame 4 si/no). Nuovi preset, **console vecchia da sola come unico client**:

| Preset | Annuncia | Frame 4 | Serve a |
|---|---|---|---|
| 8 | 11.0.0 (0x2C00) | si | caso principale: un host «piu' vecchio» di qualunque client non dovrebbe far chiedere aggiornamenti |
| 9 | 11.0.0 | no | se l'8 si pianta: il frame 4 era il problema? |
| 10 | 11.13.0 (0x2CD0) | si | se 8 e 9 falliscono: versione UGUALE a quella della console (come la B col 11.17.0) |
| 11 | 11.0.0 + rete unica (8 nodi, appdata) | si | il candidato finale: da provare su vecchia **e** su B |

Ordine: vecchia con 8; se passa, vecchia con 11 e **B con 11** (deve funzionare anche su una console piu' nuova dell'host). Se 8 si pianta: 9, poi 10.
Dimmi quale console vecchia e' (C 11.13 o D 11.15) e cosa scrive sullo schermo.

### Aggiunta (sera tardi): il beacon pulito su console vecchia passa, ma la rete unica + beacon sempre acceso (preset 5) no

Visto: preset 5 (rete unica, 8 nodi, appdata 77, beacon durante la distribuzione) = la vecchia si collega e non risponde mai; col beacon pulito semplice scarica tutto.
Il preset 1 e' il 5 senza «beacon durante la distribuzione» e passa sulla B. **Sulla vecchia, da sola come client**: preset **1**; se passa il colpevole era il beacon sempre acceso (o l'essere il secondo client);
se si pianta, i preset **12** (solo 8 nodi) e **13** (solo appdata 77) dicono quale dei due parametri la vecchia non digerisce.

### Aggiunta (notte): il SECONDO client resta in «attendi» (preset 14 e 15)

Visto due volte (preset 5: vecchia dopo B; preset 1: B dopo la vecchia): il **primo** client risponde ad `auth` in 0,1-0,2 s e scarica; il **secondo**, di qualunque console, si collega
alla rete (stesso nodo 2) ma non manda un solo pacchetto (85 s di silenzio). Non e' la versione del beacon: e' lo stato dell'host (rete o canale dati) dopo il primo client.
Finora ogni prova ricreava la rete a mano tra una consegna e l'altra, per questo non si era visto.
* **Preset 14 (REBIND)**: finito il primo client, l'host ricollega solo il canale dati; la rete resta (e con lei i nodi estranei: e' la forma compatibile con la rete unica).
* **Preset 15 (REARM)**: finito il primo client, l'host distrugge e ricrea la rete (1-4 s): se anche il 14 si pianta, questo dovrebbe passare.
Procedura: host sul 14; client 1 (vecchia) scarica; **senza toccare l'host** client 2 (B) scarica. Se B si pianta: chiudi, riavvia l'host sul 15 e ripeti. Il log ora scrive anche l'esito di ogni `auth` inviato
(`auth tx #n -> rc`) e il nome utente del nodo che entra.

### Aggiunta (2026-10-19): stanza che non cade + un client alla volta con attesa nella lobby (preset 17 e 18)

Decisioni dell'utente: l'host si comporta come un host Nintendo (niente ottimizzazioni ne' broadcast inventati), la stanza non deve cadere mai (il «gioco» non parte, quindi nessuno la chiude), i client se ne vanno da soli,
8 nodi = host + 7, la velocita' non e' importante e non devono partire tutti insieme.
* **Preset 17 (PERSIST)**: un client alla volta, chi entra mentre un altro scarica **aspetta nella lobby** («attendi», come aspetta il via dell'host nel DLP vero) e parte quando l'altro ha finito. La rete **non** viene ricreata: ogni nodo uscito viene espulso (`udsEjectClient`), le connessioni vengono riaperte e il canale dati ricollegato. Beacon sempre acceso.
* **Preset 18 (QUEUE)**: uguale, ma a fine coda (nessun client presente) la rete viene ricreata, come il 15. E' il ripiego che sappiamo funzionare.
* Prove (stesse due console): **(a)** vecchia scarica, e **dopo che e' uscita** entra la B (secondo ingresso dopo una partenza: e' quello che col 14 non andava). **(b)** tocca il titolo su tutte e due quasi insieme: la seconda deve aspettare e poi scaricare, senza che la prima si blocchi.
  Se (a) fallisce sul 17 e (b) funziona sul 18, la stanza ricreata a fine coda e' la regola. Nel log: `queue: node N starts now`, `udsEjectClient(N) after it left -> 0x...`.

### Aggiunta (2026-10-19, sera): scopo ridotto all'essenziale + ritrasmissione selettiva (build MENU-20)

Scopo del Download Play in 3DS Link: **uno vede 3DS Link in Download Play, lo tocca, parte l'installazione, se ne va.** Le persone entrano, scaricano e vanno via; nessuna opzione avanzata.
Il menu ora ha solo: **0** base, **1** rete unica, **6** nodo extra, **15** rete ricreata per client, **17** stanza fissa un client alla volta, **18** coda + rete ricreata a coda vuota. Parte sul 17.
* **Ritrasmissione selettiva (sempre accesa)**: quando un blocco e' incompleto il client manda una mappa dei frammenti mancanti; l'host rimanda solo quelli. L'ordine dei bit non e' documentato: l'host prova 5 interpretazioni in ordine, tiene la prima che fa cambiare la mappa, e se nessuna funziona torna a rimandare il blocco intero.
  Su PC (simulatore di perdita, figlio da 2,6 MB, dati identici in tutti i casi): perdita 1% → **7-9 s invece di 21,4 s** (1861 frammenti spediti contro 3480); perdita 3% → 8,6 s contro 26,4 s. Con perdita 0 nessuna differenza (6,2 s).
  Nel log su console: `selective resend: block B, N of 182 fragments (interpretation h)` e `selective resend worked: bitmap interpretation h confirmed` (oppure `gives up`).
* **Banda**: `gap_us` e' la pausa fra due frammenti (default 3000 = ~25% della banda con il carosello acceso; 0 = il piu' veloce possibile). Nella libreria sara' un parametro cambiabile a runtime: pieno quando nessuno scarica con il delivery, 25% quando il delivery e' in corso.
