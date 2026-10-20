#include <ultra64.h>
#include "nds_bottom_ui.h"
#ifdef NDS_BOTTOM_UI
#include <stdio.h>
#include <string.h>
#include "nds_include.h"
#include "game/ingame_menu.h"
#include "game/level_update.h"
#include "game/memory.h"
#include "game/camera.h"
extern u8 gDialogCharWidths[256];
#include "game/segment2.h"

/* H is the only background bank: C/D belong to ARM7 audio. 768 unique
 * 4bpp tiles (24 KiB) plus a 2 KiB map fit in its 32 KiB. */
static u16 sPixels[256 * 192 / 4] __attribute__((aligned(32)));
static int sBg;
static const u8 *sText;
static int sStart, sLines, sChoice, sMore, sCamera;

void nds_bottom_ui_camera(int status) { sCamera = status; }

static void pixel(int x, int y, int color) {
    int offset, shift;
    if ((unsigned)x >= 256 || (unsigned)y >= 192) return;
    offset = ((y >> 3) * 32 + (x >> 3)) * 16 + (y & 7) * 2 + ((x & 7) >> 2);
    shift = (x & 3) * 4;
    sPixels[offset] = (sPixels[offset] & ~(15 << shift)) | (color << shift);
}

/* The NDS asset converter expands the US 16x8 IA4 source to A5I3.
 * Its texture axes follow vertex_ia8_char: screen X reverses texture Y,
 * screen Y reverses texture X. Render the resulting upright 8x16 glyph
 * at 10x20 (dialog) or 16x32 (HUD). */
static int glyph(int x, int y, u8 c, int width, int height) {
    const u8 *tex;
    void **lut = segmented_to_virtual(main_font_lut);
    if (c == DIALOG_CHAR_SPACE) return (5 * width + 7) / 8;
    if (lut[c] == NULL) return width;
    tex = segmented_to_virtual(lut[c]);
    for (int dy = 0; dy < height; dy++) {
        for (int dx = 0; dx < width; dx++) {
            int tx = 15 - dy * 16 / height;
            int ty = 7 - dx * 8 / width;
            int v = tex[ty * 16 + tx];
            if ((v >> 3) && (v & 7)) pixel(x + dx, y + dy, v & 7);
        }
    }
    return (gDialogCharWidths[c] * width + 7) / 8;
}

static u8 encode(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    if (c == ':') return 0xE6;
    if (c == '/') return 0xD0;
    if (c == '-') return 0x9F;
    return DIALOG_CHAR_SPACE;
}

static int ascii(int x, int y, const char *str, int w, int h) {
    while (*str) x += glyph(x, y, encode(*str++), w, h);
    return x;
}

static void number(int x, int y, int value, int w, int h) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    ascii(x, y, buf, w, h);
}

void nds_bottom_ui_init(void) {
    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG);
    sBg = bgInitSub(0, BgType_Text4bpp, BgSize_T_256x256, 15, 0);
    for (int i = 0; i < 1024; i++) bgGetMapPtr(sBg)[i] = i < 768 ? i : 0;
    for (int i = 0; i < 8; i++) BG_PALETTE_SUB[i] = RGB15(i * 31 / 7, i * 31 / 7, i * 31 / 7);
    BG_PALETTE_SUB[0] = RGB15(0, 0, 0);
    memset(sPixels, 0, sizeof(sPixels));
    DC_FlushRange(sPixels, sizeof(sPixels));
    dmaCopy(sPixels, bgGetGfxPtr(sBg), sizeof(sPixels));
    bgShow(sBg);
}

void nds_bottom_ui_message(const unsigned char *text, int start, int lines,
                           int choice, int more) {
    sText = text;
    sStart = start;
    sLines = lines;
    sChoice = choice;
    sMore = more;
}

static void hud(void) {
    int flags = gHudDisplay.flags;
    if (!flags) return;
    if (sText) {
        int x = 4;
        if (flags & HUD_DISPLAY_FLAG_LIVES) {
            x = ascii(x, 4, "M ", 10, 20);
            number(x, 4, gHudDisplay.lives, 10, 20);
        }
        if (flags & HUD_DISPLAY_FLAG_COIN_COUNT) {
            ascii(80, 4, "C ", 10, 20);
            number(100, 4, gHudDisplay.coins, 10, 20);
        }
        if (flags & HUD_DISPLAY_FLAG_STAR_COUNT) {
            glyph(170, 4, DIALOG_CHAR_STAR_FILLED, 10, 20);
            number(188, 4, gHudDisplay.stars, 10, 20);
        }
        if (flags & HUD_DISPLAY_FLAG_CAMERA_AND_POWER) {
            for (int i = 0; i < 8; i++)
                for (int y = 29; y < 33; y++)
                    for (int px = 4 + i * 8; px < 10 + i * 8; px++)
                        pixel(px, y, i < gHudDisplay.wedges ? 7 : 2);
        }
        if (flags & HUD_DISPLAY_FLAG_KEYS) {
            ascii(80, 25, "K", 8, 12);
            number(92, 25, gHudDisplay.keys, 8, 12);
        }
        if (flags & HUD_DISPLAY_FLAG_TIMER) {
            char buf[20];
            unsigned t = gHudDisplay.timer;
            snprintf(buf, sizeof(buf), "%u:%02u:%02u", t / 1800, t / 30 % 60, (t % 30) * 100 / 30);
            ascii(136, 25, buf, 8, 12);
        }
        return;
    }
    if (flags & HUD_DISPLAY_FLAG_LIVES) {
        ascii(8, 8, "MARIO", 8, 16);
        number(8, 26, gHudDisplay.lives, 16, 32);
    }
    if (flags & HUD_DISPLAY_FLAG_COIN_COUNT) {
        ascii(136, 8, "COINS", 8, 16);
        number(136, 26, gHudDisplay.coins, 16, 32);
    }
    if (flags & HUD_DISPLAY_FLAG_STAR_COUNT) {
        ascii(8, 68, "STARS", 8, 16);
        number(8, 86, gHudDisplay.stars, 16, 32);
    }
    if (flags & HUD_DISPLAY_FLAG_KEYS) {
        ascii(136, 68, "KEYS", 8, 16);
        number(136, 86, gHudDisplay.keys, 16, 32);
    }
    if (flags & HUD_DISPLAY_FLAG_CAMERA_AND_POWER) {
        ascii(8, 124, "POWER", 8, 16);
        ascii(136, 124, (sCamera & CAM_STATUS_FIXED) ? "CAM FIX" :
            (sCamera & CAM_STATUS_MARIO) ? "CAM M" : "CAM L", 8, 16);
        for (int i = 0; i < 8; i++)
            for (int y = 144; y < 156; y++)
                for (int x = 8 + i * 30; x < 34 + i * 30; x++)
                    pixel(x, y, i < gHudDisplay.wedges ? 7 : 2);
    }
    if (flags & HUD_DISPLAY_FLAG_TIMER) {
        char buf[24];
        unsigned t = gHudDisplay.timer;
        snprintf(buf, sizeof(buf), "TIME %u:%02u:%02u", t / 1800, t / 30 % 60, (t % 30) * 100 / 30);
        ascii(8, 170, buf, 10, 20);
    }
}

static void message(void) {
    int x = 4, y = gHudDisplay.flags ? 40 : 4, line = 0;
    int limit = sLines;
    /* Each original page retains its newlines and A/B behaviour. The
     * original text box is ~200 logical pixels wide; 1.25x fits in 248. */
    for (int i = sStart; line < limit && sText[i] != DIALOG_CHAR_TERMINATOR; i++) {
        u8 c = sText[i];
        /* Timed subtitles can be wider than a normal dialog line. */
        if (sMore < 0 && x > 236 && c != DIALOG_CHAR_NEWLINE) {
            x = 4; y += 20; line++;
            if (line >= limit) break;
        }
        if (c == DIALOG_CHAR_NEWLINE) { x = 4; y += 20; line++; continue; }
        if (c == DIALOG_CHAR_MULTI_THE || c == DIALOG_CHAR_MULTI_YOU) {
            x = ascii(x, y, c == DIALOG_CHAR_MULTI_THE ? "the" : "you", 10, 20);
        } else if (c == DIALOG_CHAR_STAR_COUNT) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", gDialogVariable);
            x = ascii(x, y, buf, 10, 20);
        } else if (c == DIALOG_CHAR_SLASH) {
            x += 12;
        } else {
            x += glyph(x, y, c, 10, 20);
        }
    }
    if (sChoice) {
        /* Match the original Yes/No cursor under the final response line. */
        int cx = sChoice == 1 ? 8 : 78;
        int cy = y + 19;
        for (int row = 0; row < 5; row++)
            for (int col = -row; col <= row; col++) pixel(cx + col, cy + row, 7);
    }
    if (sMore >= 0) ascii(160, 174, sMore ? "A B NEXT" : "A B CLOSE", 8, 16);
}

void nds_bottom_ui_present(void) {
    memset(sPixels, 0, sizeof(sPixels));
    hud();
    if (sText) message();
    DC_FlushRange(sPixels, sizeof(sPixels));
    dmaCopy(sPixels, bgGetGfxPtr(sBg), sizeof(sPixels));
    sText = NULL;
}
#endif
