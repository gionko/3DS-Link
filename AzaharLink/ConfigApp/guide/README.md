# guide — «Come ottenere boot9.bin», un modulo solo per l'app e per l'installatore

Una mini guida con una schermata di GodMode9 per ogni passo (ritagliata, con una freccia e il tasto da premere disegnati sopra) e al massimo tre righe sotto. Il progetto è in `DlpLink/docs/GUIDA-BOOT9.md`; la usano **l'app 3DS Link** (dove spiega `boot9.bin`, e dalla pagina 3DS Link Delivery mentre la stanza è attiva) e **l'installatore del figlio Download Play** (a installazione finita, se sulla SD non c'è `boot9.bin`).

| File | Cosa |
|---|---|
| `guide.hpp` | il contratto: `guide::Viewer` (`Open`, `Update(tasti, tocco)`, `DrawTop`, `DrawBottom`, `Close`) e `guide::Boot9Found()` |
| `guide_logic.hpp` | quello che non vuole una console: i passi (titoli, righe, immagine), `Move` (L / croce su indietro, R / croce giù avanti, croce sinistra e destra niente, B chiude, niente giro), dove sono i tre pulsanti del piede, le parole che dicono (`Up/L Back`, `Down/R Next`) e cosa vale un tocco, le due cartelle e le regole di nome e dimensione di `boot9.bin`. Provato su PC da `Tests/settings_test/guide_test.cpp` |
| `guide.cpp` | il visore (citro2d): una sola immagine in memoria per volta, il disegno, la guardia sulla SD per `Boot9Found` |
| `gfx/stepN.png`, `stepN.t3s` | le immagini dei passi 1..7 (il passo 8 è solo testo) e il file che `tex3ds` legge; li fa `DlpLink/tools/make_guide.py`, l'uscita si versiona |
| `src/snap_*.png` | gli screenshot di GodMode9 v2.1.1 da cui vengono i ritagli (copie: il repository non dipende da `DlpTest/`) |

## Come lo prende un programma
Il suo `Makefile` (devkitPro) deve avere, oltre ai file propri:
* `SOURCES` con la cartella `guide` (compila `guide.cpp`);
* `INCLUDES` con la cartella `guide` **e con la cartella `source` del programma**: `guide.cpp` include `gfx.hpp`, e quello giusto è quello del programma (le funzioni `gfx::` sono le stesse nell'app e nel figlio; i `ui::` no, per questo il modulo disegna da sé barra, pulsanti e bordo con lo stesso aspetto);
* `GRAPHICS := <cartella>/guide/gfx` con `GFXBUILD := $(BUILD)`: la regola `%.t3x` di devkitPro trasforma ogni `stepN.t3s` in `stepN.t3x` e lo lega come simbolo `stepN_t3x` (+ `stepN_t3x_end`).
Un programma senza immagini non si collega (simboli mancanti): chi non le vuole definisce `-DGUIDE_NO_PICTURES` e `Open()` dice di no. Provato in Docker con un Makefile di prova derivato da quelli veri (app e figlio): compila senza avvisi con `-Wall -Wextra`, si collega e fa il `.3dsx`.

## Come si usa (il visore è una schermata a sé)
```cpp
guide::Viewer viewer;                                   // un membro del programma o della pagina
if (!viewer.Open()) { /* niente memoria per la prima immagine: dirlo, non mostrare uno schermo vuoto */ }
// ogni giro: i tasti premuti in questo giro e, se il pennino e' sceso in questo giro, dove (altrimenti -1, -1)
if (!viewer.Update(down, tap_x, tap_y)) { /* chiuso (B o tocco su Close): Close() e' gia' fatto, si torna alla propria schermata */ }
// nel frame: gfx::BeginTop(); viewer.DrawTop(); gfx::BeginBottom(); viewer.DrawBottom();
viewer.Close();                                         // prima di gfx::Shutdown(), se il visore e' ancora aperto
```
* Dentro il visore la croce su / giù servono a cambiare passo (l'elenco dei passi è verticale; L / R fanno lo stesso, sinistra / destra niente): una pagina che usa su / giù per le sue righe non deve passare lo stesso tasto al visore che apre nello stesso giro (`pages_guide.cpp` è una pagina spinta sopra l'altra e aggiornata un giro dopo, quindi non lo fa).
* Gli schermi devono essere accesi: il visore non tocca le retroilluminazioni (la pagina 3DS Link Delivery apre la guida solo a schermi accesi, come per ogni altro tasto: con SELECT spenti conta solo SELECT).
* `Boot9Found()` è uno sguardo veloce (nome e dimensione) alle due cartelle di un 3DS Link installato come CIA: `sdmc:/3ds/3DSLink` e `sdmc:/gm9/out` (`boot9.bin`, o un nome con «boot9» come `NEG..._boot9_00.bin`). **Serve all'installatore.** L'app usa il suo, esatto (`setup::FindBoot9`: anche la cartella da cui è partita e il percorso scelto, e controlla il contenuto).

## La guida non deve mentire
L'ultimo passo dice che il dump finisce in `gm9/out` sulla SD e che 3DS Link lo trova lì da solo: è vero perché `ConfigApp/source/paths.cpp` cerca `boot9.bin` accanto all'app, nella sua cartella e in `sdmc:/gm9/out` (e li' anche i nomi alla GodMode9 2.x). Le due cartelle di `guide_logic.hpp` (`kBoot9AppDir`, `kBoot9GodModeDir`) devono dire quello che dicono `kDefaultAppDir` e `kGodModeOutDir` di `paths.hpp` (e le regole di nome e dimensione le stesse righe di `IsBoot9Name` e `Boot9SizeOk` di `paths.cpp`): `guide_test` ne verifica i valori, `Tests/policy_check.sh` fissa che le righe siano uguali nei due posti.

## Rifare le immagini
`python DlpLink/tools/make_guide.py` (Windows, Pillow; `--check` fa girare anche `tex3ds`, nel Docker se non c'è, e scrive la dimensione di ogni t3x). Deterministico: due giri danno gli stessi byte. Ogni immagine è un ritaglio (al massimo 196x82 pixel dello screenshot) con frecce e cornice disegnate alla dimensione dei pixel di GodMode9, poi ingrandito 2x senza smussare e mostrato 1:1; `-f rgb565 -z auto`: 1,3 – 2,7 KB l'una (tutte e sette 11,7 KB; in memoria, una per volta, 128 o 256 KB di memoria lineare). Se cambia GodMode9, si cambiano gli screenshot in `src/`, i numeri del ritaglio in `make_guide.py` e le righe dei passi in `guide_logic.hpp`.
