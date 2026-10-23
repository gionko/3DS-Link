# Prova su console dell'installatore «3DS Link Installer» (una sessione, esiti → decisioni scritti prima)

Cosa si prova: una console con **solo Luma e il Download Play di sistema** riceve da un'altra console il figlio «3DS Link Installer» (sotto: «Powered by Gionko · v1.0.2»),
lo vede nella lista del Download Play, lo scarica, il sistema lo avvia da solo, e **il figlio installa 3DS Link** (l'app e il plugin) sulla SD.
Tutta la catena è già provata su PC (verificatori; l'installatore: le sue decisioni, il codice che scrive i file sulla scheda e quello che decide l'avviso su boot9.bin, 591 controlli con ASan/UBSan) e **nell'emulatore** (installazione, aggiornamento, stessa versione,
versione più nuova, errori); su una console vera **non è mai stata provata** la parte che sta fra il Download Play e l'installazione: `am:net` da un
figlio, il RomFS cifrato a chiave zero letto dal figlio dell'installatore, la scrittura su SD del plugin, l'uscita verso HOME e il pacchetto regalo.
Quelle cinque cose sono le domande di questa prova. **Due cose si aggiungono (2026-10-21), e nemmeno nell'emulatore sono state viste**: la copia del CIA per l'app (sezione 3 bis) e l'avviso su `boot9.bin` con la guida (3 ter): compilano e le loro decisioni sono provate su PC, le schermate no.

## 1. Cosa serve

| Console | Cosa deve avere |
|---|---|
| **H** (host, quella che ha già 3DS Link) | 3DS Link, **un CIA solo**: `dist/3DSLink.cia` installato con FBI (o il `3DSLink.3dsx`: un'app che non è installata non ha un titolo da leggere, e allora il pacchetto si costruisce solo con `3DSLink.cia` messo in `sd:/3ds/3DSLink/dlp/`); **dalla 1.0.1 il plugin è dentro l'app**, che lo scrive da sola in `sd:/3ds/3DSLink/AzaharLink.3gx` (non c'è nessun `AzaharLink.3gx` da copiare, e il pacchetto si compone dal plugin dell'app, non da un file della scheda). **Non c'è più un `child.cia` da copiare** (`dist/dlp/` non esiste): lo costruisce l'app la prima volta che si apre *3DS Link Delivery* (`../docs/COMPOSITORE.md`; la pagina promette circa 14 secondi, 16 se deve leggere il CIA dal titolo installato: lo schermo lo dice). Serve **`boot9.bin`** sulla SD di H (senza, la voce è spenta: «needs boot9.bin»). Wi-Fi acceso (interruttore). |
| **C** (client, quella che deve ricevere) | Luma3DS (qualunque: le patch delle firme servono per installare), il Download Play di sistema, **almeno 20 MiB liberi sulla SD**, Wi-Fi acceso. Niente 3DS Link. Meglio una console senza 3DS Link mai installato (la prova «già installato» è un'altra, sotto). |

Il pacchetto che H consegna è `sd:/3ds/3DSLink/dlp/child.cia`, **composto dall'app** (variante vera, cifrata a chiave zero: la `.emu.cia` è solo per Azahar e l'app non la fa mai). `DlpLink/out/3dslink-child.cia`, il figlio di makerom, è il riferimento dei test: non si copia sulla console. Per rifare la prova da capo: togliere `sd:/3ds/3DSLink/dlp/child.cia` (e, se si vuole riprovare anche la copia, `dlp/3dslink.cia`).

## 2. Passi (circa dieci minuti)

1. Su **H**: apri 3DS Link (se è il primo avvio ti fa le domande del primo avvio: rispondi **Yes**), vai su **3DS Link Delivery**. La **prima volta** il pacchetto non c'è: compare «Preparing the package» (in alto «One time only: 3DS Link builds the package that other consoles download. It should take about N s, and it can take longer. It is built again only when 3DS Link is updated.» (una prima ipotesi: le velocità della stima non sono mai state misurate su una console; dopo un settimo del lavoro e due secondi il tempo diventa quello del passo misurato, «It takes about N s.»), lo stadio che sta girando, la barra, la percentuale e il tempo che manca; in basso «Building the package», B = Stop con una domanda; HOME e sonno spenti) e, a lavoro finito, la stanza si apre da sola.
   Poi, in alto, «Room open: waiting for a console» e i cinque passi per l'altra console. Dalla seconda volta il pacchetto c'è e la stanza si apre subito. Se compare un errore invece, dice che cosa fare (per esempio «3DS Link's own package is missing»: mettere `3DSLink.cia` in `sd:/3ds/3DSLink/dlp/`; o «The key of the SD card is missing»; o, quando `3dslink.cia` c'è ma è rovinato o di un'altra versione e non c'è la strada del titolo installato, «3DSLink.cia cannot be used»: nomina il file, dice se è rovinato o di che versione è, e offre le due strade). **Prendere nota del tempo che passa** (cronometro: da «Preparing» alla stanza aperta) e di quello che il testo prometteva: è la prima misura vera.
2. Su **C**: apri **Download Play** → **Nintendo 3DS**. Nella lista deve comparire **«3DS Link Installer»** con l'icona (catene blu e bianca, «3DS», **cornice grigia**, angoli non neri) e il nome della console H.
   Scegli quella voce e conferma. Il download dura circa 10-15 s (8 blocchi).
3. Quando finisce, il sistema avvia da solo il figlio. Su **C** compare la schermata «3DS Link Installer» (barra in alto con «Powered by Gionko»): una barra che avanza, le fasi
   «Checking the package», «Copying the plugin», «Preparing the SD card», «Installing», «Finishing», e il badge rosso **HOME off** in basso. La prima scrittura di AM è lenta (circa 2 s: la barra sta ferma, i puntini girano).
4. A fine lavoro: «3DS Link is installed» (verde, barra al 100%). Premi **A** (o B): **C** torna a HOME (dalla 1.0.2 gli altri tasti non fanno niente: prima qualunque tasto usciva).
5. Su **C**: nella HOME c'è un'icona nuova **3DS Link** (forse incartata come un regalo). Aprila: parte il primo avvio di 3DS Link.
6. Su **H**: la pagina 3DS Link Delivery mostra la console tra le «Last consoles» con «Delivered», e la stanza si rifà da sola (1-4 s).

## 3. Esiti attesi e che cosa decidere

| Cosa si vede | Significa | Decisione |
|---|---|---|
| Tutto come ai passi 1-6 | La catena funziona su hardware. | Proseguire: la ridistribuzione da C (3DS Link ricevuto dal Download Play, con la copia in `dlp/3dslink.cia`, offre a sua volta: `CHECKLIST_DA_VERIFICARE.md` O16), il testo di consenso. |
| «Preparing the package» resta fermo, o torna «The last preparation did not finish» | Il lavoro è morto a metà (console spenta, app chiusa): il `child.preparing` è rimasto. | `3DSLink-TitleDelivery.log` di H (Debug log acceso: righe «preparation of the package»); riprovare con A. |
| «The package could not be prepared» | Il compositore ha detto perché (scheda piena, memoria, un CIA dell'app di un'altra versione, ...). | La frase sullo schermo e il registro di H. |
| In lista **non** compare «3DS Link Installer» (o «Nessun software trovato») | L'host non annuncia il figlio: file sbagliato/rovinato (stessa dimensione ma contenuto diverso è già successo il 19-10) o console C non vede la stanza. | Mandami il registro di H (`3DSLink-TitleDelivery.log`, con il Debug log acceso) e l'md5 di `child.cia`. |
| Il nome è giusto ma sotto non c'è «Powered by Gionko» | Il Download Play di sistema non mostra la descrizione lunga in quella schermata (non si sa dove la mostri: mai fotografata). | Foto della lista e della schermata di conferma; nessun difetto nostro. |
| Download Play dice «errore di comunicazione» / si ferma | Radio o stanza (vedi i risultati del 18-20 ottobre). | Registro di H. Riprovare dopo la ricreazione della stanza (1-4 s). |
| Il figlio parte e dice **«This console refused access to the installer service»** (codice `0xD8E06406` o `0xD0406401`) | `am:net` non è raggiungibile da un figlio Download Play su questa Luma. | Costruire la variante con `am:net` scritto nei servizi del figlio (8 byte in `finalize_child.py`, rapporto 05 par. 3.3); la rimando in un giro. |
| «The package is damaged» (`0xC8604C10..19`) | Il RomFS del figlio non si legge bene o non corrisponde al manifesto (cifratura a chiave zero del RomFS non letta come previsto). `0xC8604C19` e' anche il rifiuto di un manifesto che nomina un titolo diverso da quello di 3DS Link (`000400000F83D100`): l'installatore cancella il titolo che il manifesto nomina (e il suo biglietto) prima di installare, quindi non installa nient'altro che 3DS Link; il registro dice «the manifest names title ...». | `installer_log.txt` di C (vedi sotto): dirà quale file e quale controllo. |
| «Not enough space on the SD card» | Meno di ~19 MiB liberi, o la scheda piena. | Liberare spazio e premere **A** (Try again). |
| «Installation failed» con altro codice AM (`0xD8A0....`) | AM rifiuta il CIA (firma, database dei titoli SD non pronto, ...). Il testo dice cosa fare (per il database: aprire HOME una volta con la scheda). | `installer_log.txt` di C: il codice e la riga prima. |
| Il figlio non parte da solo dopo il download | Il sistema non lo avvia (non si è mai visto: la sonda partiva). | Foto e registro di H. |
| Si installa ma **non** compare l'icona in HOME | HOME non ricarica la lista (mai osservato). | Spegnere e riaccendere C; se c'è, annotare che serve; se no, `installer_log.txt`. |
| Il figlio resta sulla schermata finale e non torna a HOME | `return` da `main` non basta su un figlio Download Play. | Foto; fix: `aptJumpToHomeMenu` o `APT_CloseApplication` esplicito. |

## 3 bis. La copia del CIA dell'app (`sd:/3ds/3DSLink/dlp/3dslink.cia`)

L'installatore, dopo aver messo 3DS Link sulla console (installazione, aggiornamento, versione più vecchia al posto della nuova, e anche «stessa versione già installata»), lascia una **copia** del CIA che porta nel RomFS:
`sd:/3ds/3DSLink/dlp/3dslink.cia`. È la prima fonte da cui l'app, su una console con solo Luma, compone il figlio del Download Play (`../docs/COMPOSITORE.md`). Va in un file `.part` e poi si rinomina, si rilegge e si confronta con il RomFS;
se non riesce **non** diventa mai una schermata di errore (l'app è installata): resta una riga nel registro e il lavoro va avanti. Con la risposta «lascia com'è» a una domanda non si scrive niente.

| Dopo la prova su **C** (ancora senza 3DS Link) | Atteso |
|---|---|
| Le fasi sullo schermo | dopo «Finishing» e «Checking the result» compare per un attimo **«Saving a copy of 3DS Link»** (circa 0,7 MB: meno di un secondo); la barra arriva al 100% alla fine, non prima |
| `sd:/3ds/3DSLink/dlp/3dslink.cia` | c'è, **stessa dimensione del `3dslink.cia` del pacchetto** (`dist/` o `DlpLink/out/`, circa 663 KB) e stessi byte (`md5sum` uguale); niente `3dslink.cia.part` né `.old` accanto |
| `installer_log.txt` | `the copy of the app on the card (...) is missing`, poi `app copy: ... written and read back, N bytes` |
| Rifare la stessa prova (stessa versione già installata, plugin giusto, copia giusta) | schermata «3DS Link is already installed» **senza domande e senza scritture**; nel registro `the copy of the app on the card (...) is already this one` e `app copy: ... is already this one, not written` |
| Come sopra ma **cancellando** `dlp/3dslink.cia` prima | nessuna domanda; la copia viene rifatta (si vede «Saving a copy of 3DS Link»); l'app NON viene toccata |
| Come sopra ma con la copia di **un'altra versione** (o troncata) | viene sostituita dalla giusta |
| Plugin diverso o assente (stessa versione): «Restore plugin?» | si chiede **solo** del plugin (la copia non chiede mai); con **B** non si scrive niente: né plugin né copia |
| Scheda quasi piena | l'errore «Not enough space» (come prima), che ora conta anche la copia; non resta mai un `.part` |

Decisioni: la copia manca o è diversa dopo un'installazione riuscita **e** la schermata dice «installed» → è un difetto nostro: mandami `installer_log.txt` (il registro dice errno e posizione). La copia che manca solo perché la scheda è piena → non è un difetto.

## 3 ter. L'avviso su boot9.bin e la guida «How to get boot9.bin»

Condividere (3DS Link Delivery, Titles Delivery) richiede `boot9.bin` sulla SD; usare 3DS Link no. Perciò a lavoro riuscito, **se `boot9.bin` non c'è** (né `sd:/3ds/3DSLink/` né `sd:/gm9/out/`, un file da 64 KB o 32 KB che si chiami `boot9.bin` o con «boot9» nel nome,
come `<seriale>_boot9_00.bin`), le schermate finali lo dicono **senza bloccare niente**, e un tasto apre la guida (la stessa dell'app, GodMode9 passo per passo con una schermata per passo). Con `boot9.bin` presente l'installatore **non dice niente**.

Si prova **due volte**, con la stessa console **C**: una **senza** `boot9.bin` sulla SD e una **con** (copiare un `boot9.bin` da 64 KB in `sd:/3ds/3DSLink/`).

| Cosa si vede | Atteso |
|---|---|
| **Senza** `boot9.bin`, schermata finale («3DS Link is installed», «...updated», «...replaced by the older version», «The plugin file was restored», «3DS Link is already installed») | **Schermo alto**: sotto «3DS Link is on the HOME menu.» una riga gialla «boot9.bin not found: only needed to share. See the lower screen.». **Schermo basso**: la frase «Done. Press A: ...», sotto una scheda con stanghetta gialla e il titolo «boot9.bin was not found in sdmc:/3ds/3DSLink or sdmc:/gm9/out» (le due cartelle dove la ricerca guarda davvero: il titolo va a capo su due righe, accanto al pallino) e il testo (si usa senza, serve per condividere, si può fare dopo o subito spegnendo la console, la guida dice come); in basso **due pulsanti: `Y boot9.bin guide` a sinistra e `A Go to HOME` a destra**. Il testo sta nella scheda senza uscire dallo schermo (nessun «...» a metà frase) |
| Per i primi 0,8 s dopo la comparsa dei pulsanti | pulsanti spenti; nessun tasto fa niente (nemmeno **Y**) |
| **Y** | si apre la guida: schermo alto = figura di GodMode9 col titolo («1. Start GodMode9») e al massimo tre righe sotto; schermo basso = barra «How to get boot9.bin» / «Step 1 of 8», l'elenco degli 8 passi con quello attuale acceso, piede `B Close`, `L Back`, `R Next` |
| **L** e **R**, e la **croce su / giù** (l'elenco dei passi è verticale; dal 22-10-2026) | cambiano passo (L e su indietro, R e giù avanti); **sinistra e destra non fanno niente**; non si gira dall'ultimo al primo; i pulsanti Back («Up/L») e Next («Down/R») si spengono ai due estremi. Anche toccare i tre pulsanti del piede fa lo stesso |
| **B** | chiude la guida e torna alla schermata finale **senza uscire dall'app**; per 0,8 s i pulsanti sono di nuovo spenti e nessun tasto fa niente (il B che ha chiuso la guida non può anche uscire) |
| Dopo, **A** o **B** | l'app esce verso HOME. **Y** (anche con un altro tasto nello stesso fotogramma: un tocco vicino non deve mandare via dall'app, e B chiude la guida) riapre la guida. **X, START, SELECT, L, R, croce: non fanno niente** (1.0.1 e prima: qualunque tasto usciva, e chi premeva X per Y finiva nella HOME; prova del 22-10-2026) |
| **HOME** dentro la guida | permesso (come nella schermata finale): si apre il menu HOME, si torna alla guida dov'era |
| **Con** `boot9.bin` sulla SD, stessa schermata finale | **niente di tutto questo**: nessuna riga gialla, nessuna scheda, un solo pulsante `A Go to HOME`, «Press A to go back to the HOME menu»; **Y non fa niente** (la guida non è offerta: il tasto non è sul piede) |
| Schermata di errore («Installation failed») e schermate «Nothing was changed» / «A newer 3DS Link is already installed» | **mai** l'avviso, con o senza `boot9.bin`; **Y non fa niente**; sulle schermate di errore che non si possono ritentare esce solo **B** |
| `installer_log.txt` | `boot9.bin on the SD card: found` o `not found`; con la guida: `the boot9.bin guide opened` e `... closed`; all'uscita `leave: the user pressed A` (o `B`, con i tasti e la fase) oppure `leave: aptMainLoop() returned false (the system asks the app to close)` |
| (rarissimo) la guida non si apre | nel registro `the boot9.bin guide could not be opened`; il pulsante `Y` sparisce e la scheda dice che la guida è nell'app («How to get boot9.bin»); un tasto morto non resta mai |

Decisioni: l'avviso compare **con** `boot9.bin` presente → la ricerca (`guide::Boot9Found`, `AzaharLink/ConfigApp/guide/guide_logic.hpp`) cerca nel posto sbagliato: dimmi dov'era il file. Il testo dell'avviso esce dalla scheda, si sovrappone al corpo o si taglia a metà → il tipo di carattere della console è più alto dello stand-in dell'emulatore: foto delle due schermate.
Un passo della guida dice una cosa diversa da quello che si vede in GodMode9 sulla console → foto di GodMode9 e numero del passo; si corregge il testo (`guide_logic.hpp`), non il programma.

## 4. Cosa mandarmi

Da **C**: `sd:/3ds/3DSLink/installer_log.txt` (si accoda a ogni esecuzione; non contiene niente di personale), una foto della schermata finale del figlio (**anche dell'avviso su boot9.bin e della guida, se compaiono**) e, se c'è, della HOME.
Da **H**: il tempo della preparazione (e una foto della schermata «Preparing the package» a metà) e foto della pagina 3DS Link Delivery a fine consegna e, con il **Debug log** acceso, `3DSLink-TitleDelivery.log` (le righe `MAC from the sockets service ... | from the shared page ...` dicono se le due fonti
dell'indirizzo di H coincidono: con un indirizzo sbagliato nessuna console vede la stanza). Una foto della lista del Download Play su C (nome, descrizione, icona).

## 5. Ripetere la prova sulla stessa console C

Il client che ha già un figlio con lo stesso Title ID e la stessa versione risponde «ce l'ho già» e rilancia la **copia vecchia**. Per ripetere: o si **disinstalla** il figlio su C
(titolo `000400010F83D100`, tipo «temporaneo» in NAND: con FBI → Titles → NAND) o si **alza la versione** (`python DlpLink/tools/version.py bump micro`: riscrive la versione di 3DS Link in tutti i punti; poi si ricostruisce tutto con `DlpLink/tools/build_all.sh`).
Se 3DS Link è già installato su C alla stessa versione, il figlio **non tocca l'app** e rinfresca solo il plugin e la copia del CIA in `sd:/3ds/3DSLink/dlp/3dslink.cia` («The same version is installed»): è voluto (potrebbe essere una build completa con più cose dentro).
Se su C c'è una versione **più vecchia**, la **aggiorna** (toglie la vecchia e installa la nuova); se c'è una **più nuova**, non tocca niente.

## 6. Pulizia dopo la prova

Su C: `sd:/3ds/3DSLink/` (plugin, registro) e l'app 3DS Link si tolgono da FBI (titolo `000400000F83D100`, SD) se non servono; il figlio `000400010F83D100` sparisce da solo quando
ne arriva un altro o con FBI. Niente di tutto questo tocca altro: l'installatore non scrive mai `3DSLink.cfg` né niente in `/luma`.
