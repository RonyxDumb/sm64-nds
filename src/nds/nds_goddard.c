#include <stdio.h>
#include <string.h>
#include "nds_goddard.h"
#include "nds_include.h"
#include "game/area.h"
#include "game/game_init.h"
#include "game/memory.h"
#include "game/sound_init.h"

struct GdElfHeader {
    u8 ident[16]; u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct GdElfProgram { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct GdElfSection { u32 name, type, flags, addr, offset, size, link, info, align, entsize; };
struct GdElfSymbol { u32 name, value, size; u8 info, other; u16 section; };
struct GdElfReloc { u32 offset, info; };
struct GdExport { const char *name; u32 address; };
extern const struct GdExport gNdsGoddardExports[];
#define GODDARD_WORK_SIZE 0xF1000
static const struct NdsGoddardApi *sApi;
volatile int gNdsGoddardStatus;
const char *volatile gNdsGoddardMissingSymbol;

static int read_at(FILE *f, u32 offset, void *out, u32 size) {
    return fseek(f, offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size;
}

static u32 resolve_symbol(u8 *image, const struct GdElfSymbol *symbol, const char *strings) {
    if (symbol->section == 0xfff1) return symbol->value;
    if (symbol->section) return (u32)image + symbol->value;
    const char *name = strings + symbol->name;
    if (!*name) return 0;
    for (const struct GdExport *e = gNdsGoddardExports; e->name; e++)
        if (!strcmp(name, e->name)) return e->address;
    gNdsGoddardMissingSymbol = name;
    return 0;
}

/* The original engine lives in the right side of the level pool only while
 * its screen is active. CLEAR_LEVEL releases code, assets and its work heap. */
static const struct NdsGoddardApi *load_module(void) {
    FILE *f = fopen("nitro:/data/goddard.so", "rb");
    struct GdElfHeader h;
    struct GdElfProgram p;
    struct GdElfSection s, stringsSection;
    u8 *image = NULL;
    u32 size = 0, symbolCount = 0;
    struct GdElfSymbol *symbols = NULL;
    const char *strings = NULL;
    gNdsGoddardStatus = -1;
    if (!f) return NULL;
    if (!read_at(f, 0, &h, sizeof(h)) || memcmp(h.ident, "\177ELF\1\1", 6) ||
        h.type != 3 || h.machine != 40 || h.phentsize != sizeof(p) ||
        h.shentsize != sizeof(s) || h.phnum > 16 || h.shnum > 128) goto fail;
    gNdsGoddardStatus = -2;
    for (int i = 0; i < h.phnum; i++) {
        if (!read_at(f, h.phoff + i * sizeof(p), &p, sizeof(p))) goto fail;
        if (p.type == 1) {
            if (p.vaddr + p.memsz < p.vaddr || p.filesz > p.memsz) goto fail;
            if (p.vaddr + p.memsz > size) size = p.vaddr + p.memsz;
        }
    }
    if (!size || size > 0x60000) goto fail;
    image = main_pool_alloc(size, MEMORY_POOL_RIGHT);
    gNdsGoddardStatus = -3;
    if (!image) goto fail;
    memset(image, 0, size);
    for (int i = 0; i < h.phnum; i++) {
        if (!read_at(f, h.phoff + i * sizeof(p), &p, sizeof(p))) goto fail;
        if (p.type == 1 && !read_at(f, p.offset, image + p.vaddr, p.filesz)) goto fail;
    }
    for (int i = 0; i < h.shnum; i++) {
        if (!read_at(f, h.shoff + i * sizeof(s), &s, sizeof(s))) goto fail;
        if (s.type == 11) {
            if (s.addr + s.size > size || s.entsize != sizeof(*symbols) || s.link >= h.shnum) goto fail;
            symbols = (void *)(image + s.addr);
            symbolCount = s.size / sizeof(*symbols);
            if (!read_at(f, h.shoff + s.link * sizeof(s), &stringsSection, sizeof(s))) goto fail;
            if (stringsSection.addr + stringsSection.size > size) goto fail;
            strings = (void *)(image + stringsSection.addr);
        }
    }
    if (!symbols || !strings) goto fail;
    gNdsGoddardStatus = -4;
    for (int i = 0; i < h.shnum; i++) {
        if (!read_at(f, h.shoff + i * sizeof(s), &s, sizeof(s))) goto fail;
        if (s.type != 9 || !(s.flags & 2)) continue;
        if (s.addr + s.size > size || s.entsize != sizeof(struct GdElfReloc)) goto fail;
        struct GdElfReloc *reloc = (void *)(image + s.addr);
        for (int j = 0; j < s.size / sizeof(*reloc); j++) {
            u32 index = reloc[j].info >> 8, type = reloc[j].info & 255;
            if (reloc[j].offset > size - 4 || index >= symbolCount) goto fail;
            u32 *dest = (void *)(image + reloc[j].offset);
            if (type == 23) *dest += (u32)image; /* R_ARM_RELATIVE */
            else if (type == 2 || type == 21 || type == 22) {
                u32 address = resolve_symbol(image, symbols + index, strings);
                if (!address) goto fail;
                if (type == 2) *dest += address; /* R_ARM_ABS32 */
                else *dest = address;           /* GLOB_DAT / JUMP_SLOT */
            } else if (type != 0) goto fail;
        }
    }
    for (int i = 0; i < symbolCount; i++) {
        if (strcmp(strings + symbols[i].name, "gNdsGoddardApi")) continue;
        const struct NdsGoddardApi *api = (void *)(image + symbols[i].value);
        if (api->version != 1 || api->size != sizeof(*api)) goto fail;
        fclose(f);
        DC_FlushRange(image, size);
        IC_InvalidateRange(image, size);
        return api;
    }
fail:
    fclose(f);
    if (image) main_pool_free(image);
    return NULL;
}

int nds_goddard_load(int scene) {
    sApi = load_module();
    if (!sApi) return 0;
    void *heap = main_pool_alloc(GODDARD_WORK_SIZE, MEMORY_POOL_LEFT);
    gNdsGoddardStatus = -5;
    if (!heap) { sApi = NULL; return 0; }
    gNdsGoddardStatus = 1;
    sApi->init(heap, GODDARD_WORK_SIZE);
    gNdsGoddardStatus = 2;
    sApi->setup();
    gNdsGoddardStatus = 3;
    sApi->make(scene);
    gNdsGoddardStatus = 4;
    return 1;
}

static void vblank_callback(void) { if (sApi) sApi->vblank(); }

Gfx *nds_goddard_render(int scene) {
    if (!sApi) return NULL;
    if (gPlayer1Controller->controllerData && !gWarpTransition.isActive)
        sApi->copyPad(gPlayer1Controller->controllerData);
    Gfx *dl = sApi->get(scene);
    gGoddardVblankCallback = vblank_callback;
    play_menu_sounds(sApi->sfx());
    return dl;
}
