#include <ultra64.h>
#include "nds_bottom_ui.h"
#ifdef NDS_BOTTOM_UI
#include <stdio.h>
#include <string.h>
#include "nds_include.h"
#include "nds_minimap.h"
#include "engine/math_util.h"
#include "game/area.h"
#include "game/level_update.h"

#define MM_W (NDS_MINIMAP_SIZE * 8)
#define MM_R 6                          /* arrow radius in pixels */
#define MM_HEADER 16

#define PAL_MARIO 248
#define PAL_WHITE 249
#define PAL_SHADOW 250

static int sBg1 = -1;
static int sKeyLevel = -1, sKeyArea = -1;
static int sReady, sGen, sVisible, sVramValid, sMapDirty;
static int sOriginX, sOriginZ, sScale;
static int sMarkX, sMarkY, sMarkDir = -1, sMarkValid;
static int sWinX, sWinY, sWinW, sWinH;       /* BG0 tiles holding the arrow */

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
    BG_PALETTE_SUB[PAL_MARIO] = RGB15(31, 2, 2);
    BG_PALETTE_SUB[PAL_WHITE] = RGB15(31, 31, 31);
    BG_PALETTE_SUB[PAL_SHADOW] = RGB15(2, 2, 4);
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

/* Red arrow with a white rim. */
static int arrow_pixel(int dx, int dy, float fx, float fy) {
    float f = dx * fx + dy * fy, s = dy * fx - dx * fy;
    if (s < 0) s = -s;
    if (f >= -1.8f && f <= 4.2f && s <= (4.2f - f) * 0.55f) return PAL_MARIO;
    if (f >= -3.6f && f <= 6.2f && s <= (6.2f - f) * 0.62f) return PAL_WHITE;
    return 0;
}

static void draw_marker(unsigned short *vram, unsigned short *map, int sx, int sy) {
    static u8 buf[24][24];
    s16 yaw = (s16)(sMarkDir << 12);
    float fx = sins(yaw), fy = coss(yaw);
    int x0 = (sx - MM_R - 1) >> 3, y0 = (sy - MM_R - 1) >> 3;
    int x1 = (sx + MM_R + 1) >> 3, y1 = (sy + MM_R + 1) >> 3;

    /* Give the previous window back to the (empty) HUD layer. */
    if (sMarkValid && !sMapDirty)
        for (int y = 0; y < sWinH; y++)
            for (int x = 0; x < sWinW; x++)
                map[(sWinY + y) * 32 + sWinX + x] = NDS_UI_BLANK_TILE;
    sWinX = x0; sWinY = y0;
    sWinW = x1 - x0 + 1; sWinH = y1 - y0 + 1;

    /* The arrow plus a one-pixel drop shadow towards the lower right. */
    memset(buf, 0, sizeof(buf));
    for (int dy = -MM_R - 1; dy <= MM_R + 1; dy++)
        for (int dx = -MM_R - 1; dx <= MM_R + 1; dx++) {
            int px = sx + dx - x0 * 8, py = sy + dy - y0 * 8;
            if ((unsigned)px >= 24 || (unsigned)py >= 24) continue;
            int c = arrow_pixel(dx, dy, fx, fy);
            if (c) buf[py][px] = c;
            else if (arrow_pixel(dx - 1, dy - 1, fx, fy)) buf[py][px] = PAL_SHADOW;
        }
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

    /* Keep the whole arrow and its shadow inside the map. */
    int x = ((int)m->pos[0] - sOriginX) / sScale;
    int y = ((int)m->pos[2] - sOriginZ) / sScale;
    int dir = ((u16)(m->faceAngle[1] + 0x800)) >> 12;
    x = x < MM_R + 1 ? MM_R + 1 : x > MM_W - MM_R - 2 ? MM_W - MM_R - 2 : x;
    y = y < MM_R + 1 ? MM_R + 1 : y > MM_W - MM_R - 2 ? MM_W - MM_R - 2 : y;
    if (sMarkValid && !sMapDirty && x == sMarkX && y == sMarkY && dir == sMarkDir) return;
    sMarkX = x;
    sMarkY = y;
    sMarkDir = dir;
    draw_marker(vram, map, NDS_MINIMAP_COL * 8 + x, NDS_MINIMAP_ROW * 8 + y);
    sMarkValid = 1;
    sMapDirty = 0;
}
#endif
