# Super Mario 64 — Port Nintendo DS

Port di **Super Mario 64** per Nintendo DS, basato sul lavoro di **TobiasFriedly** e sul port DSi di **Hydr8gon**.

L'obiettivo di questa versione è rendere il gioco il più possibile adatto all'hardware Nintendo DS originale, mantenendo il flusso di gioco di Super Mario 64 e migliorando compatibilità, audio, avvio, NitroFS e presentazione della ROM.

> Questo progetto non include alcuna ROM di Super Mario 64. Per compilare è necessario utilizzare una copia ottenuta legalmente dalla propria cartuccia.

---

## Modifiche principali

Questa versione include varie modifiche rispetto al port originale:

- menu iniziale con logo 3D e cielo sullo schermo superiore;
- `START GAME` e `OPTIONS` sul touchscreen, con transizioni animate;
- opzioni audio e contatore FPS;
- avvio classico con logo, testa di Mario Goddard animata e File Select originale;
- tema Wing Cap nel menu iniziale, al 75% del tempo e due semitoni più basso;
- correzione del loop audio durante la pausa;
- ricerca più robusta del percorso della ROM;
- supporto migliorato per flashcart;
- banner Nintendo DS personalizzato;
- icona ROM personalizzata;

---

## Requisiti

Per compilare il progetto servono:

- una ROM di **Super Mario 64** (Regione USA);
- Docker Desktop;
- Git;

---

## Compilazione su Windows

Installare e avviare **Docker Desktop**.

Dalla cartella del progetto è possibile usare direttamente:

```bat
build_docker.bat
```

Al termine della compilazione USA, la ROM viene generata in:

```text
build/us_nds/sm64.us.nds
```

---

## Compilazione manuale con Docker

Dalla root del progetto:

```bash
docker build -t sm64ds .
```

Poi:

```bash
docker run --rm \
  --mount type=bind,source="$(pwd)",destination=/sm64 \
  sm64ds \
  make VERSION=us COMPILER=gcc -j4
```

Output:

```text
build/us_nds/sm64.us.nds
```

## Menu iniziale

Usare la croce direzionale e A, oppure toccare una voce. In `OPTIONS`, A
alterna ON/OFF, sinistra disattiva e destra attiva; B torna al menu.
START nel menu principale avvia la sequenza originale: logo, testa di Mario,
poi un secondo START apre la selezione dei salvataggi.
Audio è inizialmente ON e FPS OFF;
le scelte valgono per la sessione. Il contatore appare in basso durante il
gioco e misura i fotogrammi effettivamente presentati.

Il tema Wing Cap rallentato e abbassato di tono si usa solo nel menu iniziale;
le schermate successive e il gioco mantengono la loro musica originale.
Il menu inferiore usa due pagine VRAM, scambiate in VBlank per evitare scie
durante la navigazione. Il motore Goddard originale viene caricato da NitroFS
solo durante la sua schermata e liberato al cambio di livello. Le animazioni
sono compresse senza perdita e lette due fotogrammi alla volta per contenere
l'uso di RAM del Nintendo DS.

Il test della navigazione e delle transizioni si esegue con:

```bash
cc -std=c99 -Isrc tests/nds_title_menu.c src/nds/nds_title_state.c -o /tmp/nds-title-test
/tmp/nds-title-test
python3 tests/nds_goddard_anim.py
```

---

## Icona Nintendo DS personalizzata

La ROM utilizza un'icona banner Nintendo DS personalizzabile.

Il file finale usato da `ndstool` è:

```text
src/nds/gfx/icon.bmp
```

L'icona deve rispettare il formato banner Nintendo DS:

- 32×32 pixel;
- immagine indicizzata;
- massimo 16 indici di palette;
- indice `0` riservato alla trasparenza;
- 15 colori visibili;
- nessuna semitrasparenza.

Per la procedura completa vedere:

```text
NDS_CustomIcon.md
```

---

## Crediti

**Super Mario 64**, Nintendo.

- Super Mario 64 decompilation project.
- Hydr8gon — port Nintendo DSi.
- TobiasFriedly — port Nintendo DS originale.

Repository upstream:

- https://github.com/Hydr8gon/sm64
- https://github.com/TobiasFriedly/sm64-nds

Un ringraziamento a tutti i contributori della community di reverse engineering, homebrew e decompilazione di Super Mario 64.
## Fluidità e prestazioni

Il gioco simula SM64 a 30 Hz come l'originale: un fotogramma ogni due VBlank
del DS. Ottimizzazioni principali di questa versione:

- **pacing a scadenza fissa**: ogni fotogramma ha una scadenza ogni 2 VBlank.
  Un fotogramma in ritardo viene recuperato dai successivi (fino a 6 VBlank);
  con un intero VBlank di ritardo il fotogramma seguente aggiorna gioco e scena
  ma salta il lavoro della GPU (mai due di fila). Così i picchi isolati non
  rallentano più il gioco. Il contatore FPS conta i fotogrammi mostrati;
- **routine float software in ITCM**: le operazioni float/double di libgcc,
  usate migliaia di volte per fotogramma da tutta la logica, girano dalla
  memoria a ciclo singolo invece di mancare la cache della RAM principale;
  stessa sorte per `find_floor`, `atan2s`, l'elaborazione delle note audio a
  240 Hz e altre funzioni calde;
- **oggetti fuori inquadratura**: per il test di visibilità basta la posizione
  in spazio camera; la matrice completa (rotazione, scala, esportazione float)
  viene costruita solo per gli oggetti visibili, con valori identici;
- **ombre**: calcoli trigonometrici per ombra invece che per vertice, matrice
  semplificata, e cache delle ombre degli oggetti fermi (ricalcolate almeno
  ogni 4 fotogrammi per seguire acqua e piattaforme);
- **illuminazione dei vertici** con stato delle luci in registri e matrice
  direzionale tenuta dalla CPU (nessuna attesa della GPU via `glGetFixed`);
- **HUD inferiore**: quando cambiano solo cronometro o FPS si ridisegnano
  soltanto le ultime righe di tile.
- **minimappa inferiore**: durante la build `tools/nds_minimap_render.py`
  disegna dall'alto, con le texture, ogni area di ogni livello (128x128,
  16 KB ciascuna in NitroFS, ~0,7 MB in tutto). In gioco l'immagine dell'area
  corrente viene copiata in VRAM su un layer separato (BG1), senza copie in
  RAM; la freccia di Mario riscrive solo i suoi 9 tile sul layer dell'HUD.

Misure con l'emulatore melonDS sulle demo originali (fotogrammi stazionari,
SD con cluster da 32 KB), prima → dopo:

| Livello | Velocità di gioco | FPS mostrati | Lavoro CPU per fotogramma |
|---|---|---|---|
| Whomp's Fortress | 96.1% → 100% | 28.8 → 29.7 | 32.2 → 25.6 ms |
| Cool, Cool Mountain | 96.5% → 100% | 28.9 → 29.5 | 32.3 → 26.8 ms |
| Big Boo's Haunt | 97.5% → 100% | 29.2 → 29.7 | 28.0 → 24.2 ms |
| Jolly Roger Bay | 97.2% → 100% | 29.2 → 29.9 | 32.7 → 28.0 ms |
| Hazy Maze Cave | 96.7% → 100% | 29.0 → 29.4 | 30.4 → 25.3 ms |
| Scivolo della principessa | 92.7% → 100% | 27.8 → 29.2 | 29.8 → 22.1 ms |

La schermata con la testa di Mario (motore Goddard) resta la parte più
pesante (circa 17 FPS). L'hardware reale non è stato ancora provato: i tempi
assoluti possono differire da melonDS.

`NDS_THUMB=1` compila la logica di gioco in Thumb (codice più compatto,
meno ricariche della cache dalla RAM a 16 bit); melonDS non simula quel
vantaggio e la misura più lenta, quindi è disattivata finché non viene
provata su hardware.

### Benchmark ripetibile

`make VERSION=us COMPILER=gcc NDS_BENCH=1` avvia le demo originali dopo 20
fotogrammi sulla schermata "PRESS START" invece di 800: premendo START una
volta si ottiene un ciclo continuo di gameplay (Bowser, WF, CCM, BBH, JRB, HMC,
scivolo) utile per confrontare le build. Premere SELECT per esportare il CSV.

## Verifica delle prestazioni

Sul branch `bottom-screen-ui`, premere e rilasciare SELECT durante il gioco
salva `sm64-perf.csv` nella radice della scheda SD, se scrivibile. Il file
contiene tempi aggregati di logica, renderer, HUD/dialoghi, audio, caricamenti,
rete e fotogrammi per livello/area. Ogni esportazione sostituisce la precedente;
i dati coprono tutta la sessione. La scrittura può causare una pausa momentanea.

I tempi sono inclusivi: gli interrupt audio possono sovrapporsi alle altre fasi,
quindi non sommare le colonne delle fasi. I test host si eseguono con
`python3 tests/nds_perf_regression.py` e richiedono Python 3 e un compilatore C.
Confermano le invarianti degli helper modificati; non misurano gli FPS del DS.

Il profilo versione 3 esclude dalle righe per livello/area i fotogrammi con
caricamenti, attribuisce all'intro i fotogrammi della testa di Mario e aggiunge
le colonne `vblanks` e `presented`: velocità di gioco = `2 * frames / vblanks`,
FPS mostrati = `presented / (vblanks / 60)`. Include anche un istogramma dei
tempi per fotogramma e la ripartizione per fase dei fotogrammi oltre 34 ms.

Il profilo versione 2 separa comandi di livello, costruzione della scena,
oggetti, collisioni tra oggetti, camera, lettura dei segmenti e sottofasi audio.
La tabella finale riporta i tempi delle singole fasi per livello/area; per
ottenere i millisecondi medi dividere `total_ticks` per Timer Hz e per il numero
di fotogrammi dell'area, quindi moltiplicare per 1000. Le fasi sono annidate: per
esempio, gli attori sono inclusi negli oggetti. I totali globali possono
includere l'inizializzazione, mentre la ripartizione per area copre i fotogrammi.

I blocchi di triangoli delle display list statiche vengono predecodificati
durante la compilazione. Matrici, illuminazione e liste dinamiche restano
gestite durante il gioco. Per un confronto con lo stesso renderer e gli asset
originali usare `make VERSION=us COMPILER=gcc NDS_STATIC_DL=0`; il valore
predefinito è `1`. Il cambio di opzione rigenera automaticamente gli asset.
