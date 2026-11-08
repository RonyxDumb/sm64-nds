#include <ultra64.h>
#include "nds_bottom_ui.h"
#ifdef NDS_BOTTOM_UI
#include <string.h>
#include "nds_include.h"
#include "nds_minimap.h"
#include "surface_terrains.h"
#include "engine/math_util.h"
#include "engine/surface_load.h"
#include "game/area.h"
#include "game/level_update.h"
#include "game/object_list_processor.h"

#define MM_W (NDS_MINIMAP_TW * 8)
#define MM_H (NDS_MINIMAP_TH * 8)
#define MM_LEVELS 7
/* Floors up to this far above Mario's floor count as "his" storey. */
#define MM_STOREY 700
#define MM_HYSTERESIS 150
#define MM_MARKER 5

#define MM_MIN(a, b) ((a) < (b) ? (a) : (b))
#define MM_MAX(a, b) ((a) > (b) ? (a) : (b))

#define PAL_MARIO (NDS_MINIMAP_PAL + 16)
#define PAL_WHITE (NDS_MINIMAP_PAL + 17)
#define PAL_FRAME (NDS_MINIMAP_PAL + 18)

/* Nibble per pixel: 0 empty, 1..7 height band, bit 3 = contour. */
static u8 sMap[MM_W * MM_H / 2];
static u8 sArrow[2 * MM_MARKER + 1][2 * MM_MARKER + 1];

static int sKeyLevel = -1, sKeyArea, sKeyCount;
static int sReady, sGen, sVisible, sVramValid, sMapDirty;
static int sMinY, sMaxY, sScale, sOriginX, sOriginZ, sCut;
static int sMarkX, sMarkY, sMarkDir = -1, sMarkValid;

static int band(int y) {
    int l = 1 + (y - sMinY) * MM_LEVELS / (sMaxY - sMinY + 1);
    return l < 1 ? 1 : l > MM_LEVELS ? MM_LEVELS : l;
}

static int get(int x, int y) {
    int v = sMap[(y * MM_W + x) >> 1];
    return (x & 1) ? v >> 4 : v & 15;
}

static void put(int x, int y, int v) {
    u8 *p = &sMap[(y * MM_W + x) >> 1];
    *p = (x & 1) ? (*p & 0x0F) | (v << 4) : (*p & 0xF0) | v;
}

/* Floors on or below Mario's storey win, highest first; a pixel that
 * only has floors above him (a hill top, a balcony) shows the lowest. */
static int rank(int l) {
    if (l == 0) return 0;
    return l <= sCut ? 8 + l : 8 - l;
}

static void plot(int x, int y, int l) {
    if ((unsigned)x >= MM_W || (unsigned)y >= MM_H) return;
    if (rank(l) > rank(get(x, y))) put(x, y, l);
}

/* Death planes span the void below a course and would hide its shape. */
static int is_floor(const struct Surface *s) {
    return s->normal.y > 0.01f && s->type != SURFACE_DEATH_PLANE && s->type != SURFACE_INTANGIBLE;
}

/* Q4 pixel coordinates. */
static int map_x(int x) { return (x - sOriginX) * 16 / sScale; }
static int map_z(int z) { return (z - sOriginZ) * 16 / sScale; }

static void raster(const struct Surface *s) {
    int x0 = map_x(s->vertex1[0]), y0 = map_z(s->vertex1[2]);
    int x1 = map_x(s->vertex2[0]), y1 = map_z(s->vertex2[2]);
    int x2 = map_x(s->vertex3[0]), y2 = map_z(s->vertex3[2]);
    int l = band((s->vertex1[1] + s->vertex2[1] + s->vertex3[1]) / 3);
    int minx = MM_MIN(x0, MM_MIN(x1, x2)) >> 4, maxx = MM_MAX(x0, MM_MAX(x1, x2)) >> 4;
    int miny = MM_MIN(y0, MM_MIN(y1, y2)) >> 4, maxy = MM_MAX(y0, MM_MAX(y1, y2)) >> 4;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= MM_W) maxx = MM_W - 1;
    if (maxy >= MM_H) maxy = MM_H - 1;
    /* Sub-pixel triangles still mark the pixel they fall in. */
    plot((x0 + x1 + x2) / 48, (y0 + y1 + y2) / 48, l);
    for (int py = miny; py <= maxy; py++) {
        int cy = py * 16 + 8;
        for (int px = minx; px <= maxx; px++) {
            int cx = px * 16 + 8;
            int e0 = (x1 - x0) * (cy - y0) - (y1 - y0) * (cx - x0);
            int e1 = (x2 - x1) * (cy - y1) - (y2 - y1) * (cx - x1);
            int e2 = (x0 - x2) * (cy - y2) - (y0 - y2) * (cx - x2);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
                plot(px, py, l);
        }
    }
}

/* pickCut: also choose Mario's storey (first build of an area). */
static void build(int pickCut) {
    int minX = 0x7FFF, maxX = -0x8000, minZ = 0x7FFF, maxZ = -0x8000;
    int count = 0;
    sMinY = 0x7FFF;
    sMaxY = -0x8000;
    for (int i = 0; i < gNumStaticSurfaces; i++) {
        const struct Surface *s = &sSurfacePool[i];
        if (!is_floor(s)) continue;
        const s16 *v[3] = { s->vertex1, s->vertex2, s->vertex3 };
        for (int k = 0; k < 3; k++) {
            minX = MM_MIN(minX, v[k][0]); maxX = MM_MAX(maxX, v[k][0]);
            minZ = MM_MIN(minZ, v[k][2]); maxZ = MM_MAX(maxZ, v[k][2]);
            sMinY = MM_MIN(sMinY, v[k][1]); sMaxY = MM_MAX(sMaxY, v[k][1]);
        }
        count++;
    }
    sReady = count > 0;
    if (!sReady) return;
    /* Fit the larger axis inside a 2-pixel border, keep the aspect. */
    sScale = (MM_MAX(maxX - minX, maxZ - minZ) + MM_W - 5) / (MM_W - 4) + 1;
    sOriginX = (minX + maxX) / 2 - sScale * MM_W / 2;
    sOriginZ = (minZ + maxZ) / 2 - sScale * MM_H / 2;
    if (pickCut)
        sCut = gMarioState->floor ? band((int)gMarioState->floorHeight + MM_STOREY) : MM_LEVELS;

    memset(sMap, 0, sizeof(sMap));
    for (int i = 0; i < gNumStaticSurfaces; i++)
        if (is_floor(&sSurfacePool[i])) raster(&sSurfacePool[i]);

    /* Outline the floor's edges and height steps. The band (low three
     * bits) of neighbors is unaffected by contour bits set earlier. */
    for (int y = 0; y < MM_H; y++)
        for (int x = 0; x < MM_W; x++) {
            int l = get(x, y) & 7, edge = 0;
            if (!l) continue;
            static const s8 d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4 && !edge; k++) {
                int nx = x + d[k][0], ny = y + d[k][1];
                int n = ((unsigned)nx < MM_W && (unsigned)ny < MM_H) ? get(nx, ny) & 7 : 0;
                edge = !n || l - n >= 2;
            }
            if (edge) put(x, y, l | 8);
        }
    sGen = sGen % 0x7FFF + 1;
    sVramValid = 0;
}

void nds_minimap_init(void) {
    /* Dark panel, then a green-to-sand height ramp and its contours. */
    BG_PALETTE_SUB[NDS_MINIMAP_PAL] = RGB15(2, 3, 7);
    for (int l = 1; l <= MM_LEVELS; l++) {
        int r = 4 + (26 - 4) * (l - 1) / (MM_LEVELS - 1);
        int g = 12 + (28 - 12) * (l - 1) / (MM_LEVELS - 1);
        int b = 6 + (16 - 6) * (l - 1) / (MM_LEVELS - 1);
        BG_PALETTE_SUB[NDS_MINIMAP_PAL + l] = RGB15(r, g, b);
        BG_PALETTE_SUB[NDS_MINIMAP_PAL + 8 + l] = RGB15(r / 2, g / 2, b / 2);
    }
    BG_PALETTE_SUB[NDS_MINIMAP_PAL + 8] = RGB15(1, 1, 3);
    BG_PALETTE_SUB[PAL_MARIO] = RGB15(31, 3, 3);
    BG_PALETTE_SUB[PAL_WHITE] = RGB15(31, 31, 31);
    BG_PALETTE_SUB[PAL_FRAME] = RGB15(16, 19, 26);
    sVramValid = 0;
    sMarkValid = 0;
}

void nds_minimap_invalidate(void) { sVramValid = 0; }
void nds_minimap_map_dirty(void) { sMapDirty = 1; }

int nds_minimap_update(int allowed) {
    struct MarioState *m = gMarioState;
    sVisible = 0;
    if (!allowed || gCurrentArea == NULL || m == NULL || m->marioObj == NULL
        || sSurfacePool == NULL || gNumStaticSurfaces <= 0)
        return 0;
    if (gCurrLevelNum != sKeyLevel || gCurrAreaIndex != sKeyArea
        || gNumStaticSurfaces != sKeyCount) {
        sKeyLevel = gCurrLevelNum;
        sKeyArea = gCurrAreaIndex;
        sKeyCount = gNumStaticSurfaces;
        build(1);
    } else if (sReady && m->floor) {
        /* Change storey only once clearly past a band boundary, so that
         * standing on one cannot rebuild the map every frame. */
        int y = (int)m->floorHeight + MM_STOREY;
        int cut = band(y);
        if (cut != sCut && band(y + (cut > sCut ? -MM_HYSTERESIS : MM_HYSTERESIS)) != sCut) {
            sCut = cut;
            build(0);
        }
    }
    sVisible = sReady;
    return sVisible ? sGen : 0;
}

static void arrow(int dir) {
    s16 yaw = (s16)(dir << 12);
    float fx = sins(yaw), fy = coss(yaw);
    for (int dy = -MM_MARKER; dy <= MM_MARKER; dy++)
        for (int dx = -MM_MARKER; dx <= MM_MARKER; dx++) {
            float f = dx * fx + dy * fy, s = dy * fx - dx * fy;
            if (s < 0) s = -s;
            int c = 0;
            if (f >= -3.0f && f <= 5.0f && s <= (5.0f - f) * 0.5f) c = PAL_WHITE;
            if (f >= -1.6f && f <= 3.4f && s <= (3.4f - f) * 0.42f) c = PAL_MARIO;
            sArrow[dy + MM_MARKER][dx + MM_MARKER] = c;
        }
}

static int color(int x, int y) {
    int dx = x - sMarkX + MM_MARKER, dy = y - sMarkY + MM_MARKER;
    if ((unsigned)dx <= 2 * MM_MARKER && (unsigned)dy <= 2 * MM_MARKER && sArrow[dy][dx])
        return sArrow[dy][dx];
    if (x == 0 || y == 0 || x == MM_W - 1 || y == MM_H - 1) return PAL_FRAME;
    return NDS_MINIMAP_PAL + get(x, y);
}

static void tile(unsigned short *vram, int tx, int ty) {
    if ((unsigned)tx >= NDS_MINIMAP_TW || (unsigned)ty >= NDS_MINIMAP_TH) return;
    /* DS VRAM only takes halfword stores. */
    unsigned short *dst = vram + (NDS_MINIMAP_SLOT + ty * NDS_MINIMAP_TW + tx) * 32;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x += 2)
            *dst++ = color(tx * 8 + x, ty * 8 + y) | (color(tx * 8 + x + 1, ty * 8 + y) << 8);
}

static void marker_tiles(unsigned short *vram, int cx, int cy) {
    for (int ty = (cy - MM_MARKER) >> 3; ty <= (cy + MM_MARKER) >> 3; ty++)
        for (int tx = (cx - MM_MARKER) >> 3; tx <= (cx + MM_MARKER) >> 3; tx++)
            tile(vram, tx, ty);
}

void nds_minimap_present(unsigned short *vram, unsigned short *map) {
    struct MarioState *m = gMarioState;
    if (!sVisible) return;
    int x = ((int)m->pos[0] - sOriginX) / sScale;
    int y = ((int)m->pos[2] - sOriginZ) / sScale;
    int dir = ((u16)(m->faceAngle[1] + 0x800)) >> 12;
    x = x < 1 ? 1 : x > MM_W - 2 ? MM_W - 2 : x;
    y = y < 1 ? 1 : y > MM_H - 2 ? MM_H - 2 : y;
    if (dir != sMarkDir) {
        arrow(dir);
        sMarkDir = dir;
        sMarkValid = 0;
    }
    if (!sVramValid) {
        sMarkX = x;
        sMarkY = y;
        for (int ty = 0; ty < NDS_MINIMAP_TH; ty++)
            for (int tx = 0; tx < NDS_MINIMAP_TW; tx++) tile(vram, tx, ty);
        sVramValid = sMarkValid = 1;
        sMapDirty = 1;
    } else if (!sMarkValid || x != sMarkX || y != sMarkY) {
        int ox = sMarkX, oy = sMarkY;
        sMarkX = x;
        sMarkY = y;
        marker_tiles(vram, ox, oy);
        marker_tiles(vram, x, y);
        sMarkValid = 1;
    }
    if (sMapDirty) {
        for (int ty = 0; ty < NDS_MINIMAP_TH; ty++)
            for (int tx = 0; tx < NDS_MINIMAP_TW; tx++)
                map[(NDS_MINIMAP_ROW + ty) * 32 + NDS_MINIMAP_COL + tx] =
                    NDS_MINIMAP_SLOT + ty * NDS_MINIMAP_TW + tx;
        sMapDirty = 0;
    }
}
#endif
