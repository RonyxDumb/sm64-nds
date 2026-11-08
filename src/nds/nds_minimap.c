#include <ultra64.h>
#include "nds_bottom_ui.h"
#ifdef NDS_BOTTOM_UI
#include <stdio.h>
#include <string.h>
#include "nds_include.h"
#include "nds_minimap.h"
#include "game/area.h"
#include "game/level_update.h"
#include "game/memory.h"
#include "game/print.h"
#include "game/segment2.h"

#define MM_W (NDS_MINIMAP_SIZE * 8)
#define MM_ICON 16                      /* Mario's HUD head, 16x16 */
#define MM_HEADER 16

static int sBg1 = -1;
static int sKeyLevel = -1, sKeyArea = -1;
static int sReady, sGen, sVisible, sVramValid, sMapDirty;
static int sOriginX, sOriginZ, sScale;
static int sMarkX, sMarkY, sMarkValid;
static int sWinX, sWinY, sWinW, sWinH;       /* BG0 tiles holding the icon */
static u8 sIcon[MM_ICON][MM_ICON];
static int sIconReady;

static void path(char *buf, int size) {
    snprintf(buf, size, "nitro:/data/minimap/L%d_%d.bin", sKeyLevel, sKeyArea);
}

void nds_minimap_init(void) {
    sBg1 = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 1, 0);
    u16 *map = bgGetMapPtr(sBg1);
    for (int i = 0; i < 1024; i++) map[i] = NDS_UI_BLANK_TILE;
    for (int y = 0; y < NDS_MINIMAP_SIZE; y++)
        for (int x = 0; x < NDS_MINIMAP_SIZE; x++)
            map[(NDS_MINIMAP_ROW + y) * 32 + NDS_MINIMAP_COL + x] =
                NDS_MINIMAP_SLOT + y * NDS_MINIMAP_SIZE + x;
    bgSetPriority(sBg1, 1);
    bgHide(sBg1);
    sVramValid = 0;
    sMarkValid = 0;
}

void nds_minimap_invalidate(void) { sVramValid = 0; sMarkValid = 0; }
void nds_minimap_map_dirty(void) { sMapDirty = 1; }

static void hide(void) {
    sVisible = 0;
    if (sBg1 >= 0) bgHide(sBg1);
}

int nds_minimap_update(int allowed) {
    struct MarioState *m = gMarioState;
    if (!allowed || gCurrentArea == NULL || m == NULL || m->marioObj == NULL) {
        hide();
        return 0;
    }
    if (gCurrLevelNum != sKeyLevel || gCurrAreaIndex != sKeyArea) {
        char name[48];
        s32 header[4];
        sKeyLevel = gCurrLevelNum;
        sKeyArea = gCurrAreaIndex;
        path(name, sizeof(name));
        FILE *f = fopen(name, "rb");
        sReady = f && fread(header, 1, MM_HEADER, f) == MM_HEADER
                 && memcmp(header, "SMM1", 4) == 0 && header[3] > 0;
        if (f) fclose(f);
        if (sReady) {
            sOriginX = header[1];
            sOriginZ = header[2];
            sScale = header[3];
        }
        sGen = sGen % 0x7FFF + 1;
        sVramValid = 0;
    }
    if (!sReady) {
        hide();
        return 0;
    }
    sVisible = 1;
    return sGen;
}

/* Stream the 16 KiB image into its slots, 1 KiB at a time; DS VRAM only
 * takes halfword stores. */
static int load(unsigned short *vram) {
    static u16 chunk[512];
    char name[48];
    path(name, sizeof(name));
    FILE *f = fopen(name, "rb");
    if (!f) return 0;
    int ok = fseek(f, MM_HEADER, SEEK_SET) == 0;
    unsigned short *dst = vram + NDS_MINIMAP_SLOT * 32;
    for (int done = 0; ok && done < MM_W * MM_W; done += (int)sizeof(chunk)) {
        ok = fread(chunk, 1, sizeof(chunk), f) == sizeof(chunk);
        for (int i = 0; ok && i < 512; i++) *dst++ = chunk[i];
    }
    fclose(f);
    return ok;
}

/* The HUD's Mario head (RGBA16, bit 15 = opaque), mapped onto the bottom
 * screen's 6x6x6 colour cube like the other HUD artwork. */
static void icon_init(void) {
    static const u8 quant[32] = {
        0,0,0,0,1,1,1,1,1,1,2,2,2,2,2,2,
        3,3,3,3,3,3,4,4,4,4,4,4,5,5,5,5
    };
    void **lut = segmented_to_virtual(main_hud_lut);
    const u16 *tex = lut[GLYPH_MARIO_HEAD] ? segmented_to_virtual(lut[GLYPH_MARIO_HEAD]) : NULL;
    memset(sIcon, 0, sizeof(sIcon));
    if (!tex) return;
    for (int y = 0; y < MM_ICON; y++)
        for (int x = 0; x < MM_ICON; x++) {
            unsigned c = tex[y * MM_ICON + x];
            if (c & 0x8000)
                sIcon[y][x] = 8 + quant[c & 31] * 36 + quant[(c >> 5) & 31] * 6 + quant[(c >> 10) & 31];
        }
    sIconReady = 1;
}

/* (sx, sy) is the icon's top-left corner in screen pixels. */
static void draw_marker(unsigned short *vram, unsigned short *map, int sx, int sy) {
    static u8 buf[24][24];
    int x0 = sx >> 3, y0 = sy >> 3;
    int x1 = (sx + MM_ICON - 1) >> 3, y1 = (sy + MM_ICON - 1) >> 3;

    if (!sIconReady) icon_init();
    /* Give the previous window back to the (empty) HUD layer. */
    if (sMarkValid && !sMapDirty)
        for (int y = 0; y < sWinH; y++)
            for (int x = 0; x < sWinW; x++)
                map[(sWinY + y) * 32 + sWinX + x] = NDS_UI_BLANK_TILE;
    sWinX = x0; sWinY = y0;
    sWinW = x1 - x0 + 1; sWinH = y1 - y0 + 1;

    memset(buf, 0, sizeof(buf));
    for (int y = 0; y < MM_ICON; y++)
        for (int x = 0; x < MM_ICON; x++)
            buf[sy - y0 * 8 + y][sx - x0 * 8 + x] = sIcon[y][x];
    unsigned short *dst = vram + NDS_MINIMAP_MARKER * 32;
    for (int ty = 0; ty < sWinH; ty++)
        for (int tx = 0; tx < sWinW; tx++) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x += 2)
                    *dst++ = buf[ty * 8 + y][tx * 8 + x] | (buf[ty * 8 + y][tx * 8 + x + 1] << 8);
            map[(y0 + ty) * 32 + x0 + tx] = NDS_MINIMAP_MARKER + ty * sWinW + tx;
        }
}

void nds_minimap_present(unsigned short *vram, unsigned short *map) {
    struct MarioState *m = gMarioState;
    if (!sVisible) return;
    if (!sVramValid) {
        if (!load(vram)) {
            sReady = 0;
            hide();
            return;
        }
        sVramValid = 1;
        sMarkValid = 0;
    }
    bgShow(sBg1);

    /* Centre the icon on Mario, keeping all of it inside the map. */
    int x = ((int)m->pos[0] - sOriginX) / sScale - MM_ICON / 2;
    int y = ((int)m->pos[2] - sOriginZ) / sScale - MM_ICON / 2;
    x = x < 0 ? 0 : x > MM_W - MM_ICON ? MM_W - MM_ICON : x;
    y = y < 0 ? 0 : y > MM_W - MM_ICON ? MM_W - MM_ICON : y;
    if (sMarkValid && !sMapDirty && x == sMarkX && y == sMarkY) return;
    sMarkX = x;
    sMarkY = y;
    draw_marker(vram, map, NDS_MINIMAP_COL * 8 + x, NDS_MINIMAP_ROW * 8 + y);
    sMarkValid = 1;
    sMapDirty = 0;
}
#endif
