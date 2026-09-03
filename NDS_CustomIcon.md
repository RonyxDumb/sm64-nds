# NDS_CustomIcon

Guida per impostare un'icona personalizzata nella ROM Nintendo DS di questo progetto.

La soluzione utilizzata in questo port evita la conversione tramite GRIT/GRF per il banner e passa direttamente a `ndstool` un **BMP indicizzato 32×32**.

---

## Formato richiesto

L'icona finale deve essere:

```text
32×32 pixel
BMP indicizzato
16 indici di palette totali
15 colori visibili
indice 0 = trasparenza
```

Il file finale deve trovarsi qui:

```text
src/nds/gfx/icon.bmp
```

Il file sorgente può essere:

```text
src/nds/gfx/icon.png
```

---

## 1. Preparare la nuova immagine

Copiare la propria immagine personalizzata in:

```text
src/nds/gfx/icon.png
```

Può essere anche più grande di 32×32.

È consigliato utilizzare:

- soggetto semplice;
- contrasto elevato;
- pochi dettagli;
- sfondo trasparente;
- soggetto ben centrato.

Le icone Nintendo DS sono molto piccole, quindi dettagli troppo fini verranno persi.

---

## 2. Creare una versione 32×32

Da WSL / Ubuntu, entrare nella root del progetto:

```bash
cd /mnt/c/Users/pipin/Desktop/sm64-nds
```

Creare una versione 32×32 mantenendo le proporzioni:

```bash
magick src/nds/gfx/icon.png \
  -filter Lanczos \
  -resize 32x32 \
  -background none \
  -gravity center \
  -extent 32x32 \
  src/nds/gfx/icon_32.png
```

Verificare:

```bash
magick identify src/nds/gfx/icon_32.png
```

Deve apparire:

```text
32x32
```

---

## 3. Convertire in BMP indicizzato Nintendo DS

È necessario creare un BMP con:

- palette indicizzata;
- indice 0 usato per la trasparenza;
- massimo 15 colori visibili.

Usare Python + Pillow:

```bash
python3 - <<'PY'
from PIL import Image

SRC = "src/nds/gfx/icon_32.png"
DST = "src/nds/gfx/icon.bmp"

img = Image.open(SRC).convert("RGBA")
alpha = img.getchannel("A")

rgb = Image.new("RGB", (32, 32), (255, 255, 255))
rgb.paste(img.convert("RGB"), mask=alpha)

q = rgb.quantize(
    colors=15,
    method=Image.Quantize.MEDIANCUT,
    dither=Image.Dither.FLOYDSTEINBERG
)

out = Image.new("P", (32, 32), 0)

qpal = q.getpalette()

# Palette index 0: trasparenza.
# Il magenta può comparire nei visualizzatori BMP,
# ma nel menu DS l'indice 0 viene trattato come trasparente.
palette = [255, 0, 255]
palette += qpal[:15 * 3]
palette += [0] * (768 - len(palette))

out.putpalette(palette)

qpx = q.load()
apx = alpha.load()
opx = out.load()

for y in range(32):
    for x in range(32):
        if apx[x, y] < 128:
            opx[x, y] = 0
        else:
            opx[x, y] = qpx[x, y] + 1

out.save(DST, format="BMP")

used = sorted(set(out.getdata()))

print("Creato:", DST)
print("Dimensione:", out.size)
print("Indici usati:", used)
PY
```

Se Pillow non è installato:

```bash
sudo apt install python3-pil
```

---

## 4. Controllare il file generato

Verificare:

```bash
magick identify src/nds/gfx/icon.bmp
```

Deve risultare:

```text
32x32
```

Per aprirlo da WSL:

```bash
explorer.exe src/nds/gfx/icon.bmp
```

### Lo sfondo magenta è normale

Aprendo il BMP su Windows può apparire uno sfondo magenta.

Non è un errore.

Il magenta è semplicemente il colore assegnato alla **palette index 0**.

Nel banner Nintendo DS:

```text
index 0 = trasparenza
```

quindi il magenta non dovrebbe essere visibile nel menu della console.

---

## 5. Verificare gli indici della palette

Usare:

```bash
python3 - <<'PY'
from PIL import Image

im = Image.open("src/nds/gfx/icon.bmp").convert("P")

indices = sorted(set(im.getdata()))

print("Indici utilizzati:", indices)
print("Numero indici:", len(indices))
print("Pixel trasparenti/index 0:", list(im.getdata()).count(0))
PY
```

Il risultato deve utilizzare solo valori compresi tra:

```text
0 ... 15
```

Non devono esserci più di 16 indici.

---

## 6. Configurazione del Makefile

Il Makefile deve utilizzare direttamente il BMP.

La configurazione deve contenere:

```make
NDS_ICON_SRC := src/nds/gfx/icon.png
NDS_ICON     := src/nds/gfx/icon.bmp
```

La lista delle normali texture PNG deve continuare a escludere `icon.png`:

```make
PNG_FILES := $(filter-out $(NDS_ICON_SRC),$(foreach dir,$(GFX_DIRS),$(wildcard $(dir)/*.png)))
```

---

## 7. Non usare GRIT per il banner icon

La vecchia pipeline utilizzava:

```text
icon.png
↓
GRIT
↓
icon.grf
↓
ndstool
```

In questo progetto quella soluzione ha prodotto icone con palette corrotta.

Per questo l'icona banner viene ora passata direttamente come BMP.

Non devono essere presenti regole come:

```make
$(NDS_ICON): $(NDS_ICON_SRC)
	$(GRIT) ...
```

e non serve:

```text
src/nds/gfx/icon.grit
```

Il file può essere rimosso:

```bash
rm -f src/nds/gfx/icon.grit
```

Non serve nemmeno:

```text
build/us_nds/icon.grf
```

---

## 8. Regola ROM

Il target ROM deve continuare a passare `$(NDS_ICON)` a `ndstool`.

Esempio:

```make
$(ROM): $(ARM7) $(ARM9) $(NDS_NITROFS_FILES) $(NDS_NITROFS_DATA) $(NDS_ICON)
	@$(PRINT) "$(GREEN)Building ROM: $(BLUE)$@ $(NO_COL)\n"
	$(V)$(NDSTOOL) -c $@ \
		-9 $(ARM9) \
		-7 $(ARM7) \
		-b $(NDS_ICON) "$(NDS_TITLE);$(NDS_SUBTITLE1);$(NDS_SUBTITLE2)" \
		-d $(NITROFS_DIR)
	$(V)dd if=/dev/zero of=$@ bs=1 seek=18 count=1 conv=notrunc 2>/dev/null
	$(V)$(NDSTOOL) -f $@
```

Dato che:

```make
NDS_ICON := src/nds/gfx/icon.bmp
```

il comando finale utilizzerà:

```text
-b src/nds/gfx/icon.bmp
```

---

## 9. Pulire i vecchi output

Prima di ricompilare dopo aver cambiato icona:

```bash
rm -f build/us_nds/icon.grf
rm -f build/us_nds/sm64.us.nds
```

---

## 10. Compilare

Su Windows utilizzare:

```bat
build_docker.bat
```

Il progetto compila dentro Docker.

Non usare direttamente:

```bash
make
```

da Ubuntu/WSL se il toolchain devkitPro non è installato localmente.

---

## 11. Output

Per la versione USA:

```text
build/us_nds/sm64.us.nds
```

A questo punto il menu Nintendo DS dovrebbe mostrare la nuova icona personalizzata.

---

## Workflow completo

```text
icon.png
   ↓
ImageMagick
   ↓
icon_32.png
   ↓
Python + Pillow
   ↓
icon.bmp
32×32
palette indicizzata
15 colori + trasparenza
   ↓
ndstool
   ↓
sm64.us.nds
```

---

## Note sulla qualità

Il banner Nintendo DS utilizza una risoluzione molto ridotta.

Per ottenere un buon risultato:

- evitare immagini fotografiche troppo complesse;
- usare un soggetto grande;
- evitare testo molto piccolo;
- evitare dettagli sottili;
- mantenere il volto o il logo al centro;
- preferire colori molto distinguibili;
- verificare il risultato su hardware reale.

Un'immagine che appare molto dettagliata ad alta risoluzione può risultare molto meno leggibile una volta ridotta a 32×32.
