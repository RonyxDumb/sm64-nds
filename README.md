# Super Mario 64 — Port Nintendo DS

Port di **Super Mario 64** per Nintendo DS, basato sul lavoro di **TobiasFriedly** e sul port DSi di **Hydr8gon**.

L'obiettivo di questa versione è rendere il gioco il più possibile adatto all'hardware Nintendo DS originale, mantenendo il flusso di gioco di Super Mario 64 e migliorando compatibilità, audio, avvio, NitroFS e presentazione della ROM.

> Questo progetto non include alcuna ROM di Super Mario 64. Per compilare è necessario utilizzare una copia ottenuta legalmente dalla propria cartuccia.

---

## Modifiche principali

Questa versione include varie modifiche rispetto al port originale:

- ripristino del flusso di avvio originale di Super Mario 64:
  - splash / intro;
  - schermata `PRESS START`;
  - File Select;
  - gioco senza selettore skin personalizzata;
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