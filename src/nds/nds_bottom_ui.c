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
#include "game/print.h"
#include "game/hud.h"
#include "actors/common1.h"
extern u8 gDialogCharWidths[256];
#include "game/segment2.h"

/* H + I provide 48 KiB of sub BG VRAM. The map occupies the first
 * 2 KiB; pack only nonempty 8bpp tiles after it (tile 32 is blank).
 * C/D remain reserved for ARM7 audio. */
static u16 sPixels[256 * 192 / 2] __attribute__((aligned(32)));
static int sBg;
static const u8 *sText;
static int sStart, sLines, sChoice, sMore, sCamera;

void nds_bottom_ui_camera(int status) { sCamera = status; }

static void pixel(int x, int y, int color) {
    int offset, shift;
    if ((unsigned)x >= 256 || (unsigned)y >= 192) return;
    offset = ((y >> 3) * 32 + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1);
    shift = (x & 1) * 8;
    sPixels[offset] = (sPixels[offset] & ~(255 << shift)) | (color << shift);
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

static void upload_tiles(void) {
    u16 *vram = bgGetGfxPtr(sBg);
    u16 *map = bgGetMapPtr(sBg);
    int next = 33;
    /* All VRAM writes use halfwords; DS VRAM does not support byte stores. */
    for (int i = 0; i < 32; i++) vram[32 * 32 + i] = 0;
    for (int tile = 0; tile < 768; tile++) {
        const u16 *src = sPixels + tile * 32;
        int nonempty = 0;
        for (int j = 0; j < 32; j++) nonempty |= src[j];
        if (!nonempty || next >= 768) { map[tile] = 32; continue; }
        for (int j = 0; j < 32; j++) vram[next * 32 + j] = src[j];
        map[tile] = next++;
    }
}

void nds_bottom_ui_init(void) {
    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG);
    vramSetBankI(VRAM_I_SUB_BG_0x06208000);
    sBg = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 0, 0);
    for (int i = 0; i < 1024; i++) bgGetMapPtr(sBg)[i] = 32;
    /* Keep the proven dialog's seven gray levels. A 6x6x6 RGB cube
     * supplies the original HUD artwork with 216 additional colors. */
    for (int i = 0; i < 8; i++) BG_PALETTE_SUB[i] = RGB15(i * 31 / 7, i * 31 / 7, i * 31 / 7);
    for (int red = 0; red < 6; red++)
        for (int green = 0; green < 6; green++)
            for (int blue = 0; blue < 6; blue++)
                BG_PALETTE_SUB[8 + red * 36 + green * 6 + blue] =
                    RGB15((red * 31 + 2) / 5, (green * 31 + 2) / 5, (blue * 31 + 2) / 5);
    memset(sPixels, 0, sizeof(sPixels));
    upload_tiles();
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

/* NDS-converted RGBA16 is little endian RGB555 with bit 15 alpha.
 * HUD icons and digits are upright, unlike the dialog font. */
static void image(int x, int y, const void *address, int sw, int sh, int w, int h) {
    const u16 *texture = segmented_to_virtual(address);
    if (!address || !texture) return;
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++) {
            unsigned c = texture[(dy * sh / h) * sw + dx * sw / w];
            if (!(c & 0x8000)) continue;
            int red = ((c & 31) * 5 + 15) / 31;
            int green = (((c >> 5) & 31) * 5 + 15) / 31;
            int blue = (((c >> 10) & 31) * 5 + 15) / 31;
            pixel(x + dx, y + dy, 8 + red * 36 + green * 6 + blue);
        }
}

static void hud_char(int x, int y, int c, int size) {
    void **lut = segmented_to_virtual(main_hud_lut);
    if (c >= 0 && c < 58 && lut[c]) image(x, y, lut[c], 16, 16, size, size);
}

static void hud_number(int x, int y, int value, int size, int digits) {
    char buf[16];
    if (value < 0) value = 0;
    if (digits) snprintf(buf, sizeof(buf), "%0*d", digits, value);
    else snprintf(buf, sizeof(buf), "%d", value);
    for (int i = 0; buf[i]; i++) hud_char(x + i * size, y, buf[i] - '0', size);
}

static void counter(int x, int y, int icon, int value, int size) {
    hud_char(x, y, icon, size);
    hud_char(x + size, y + size / 4, GLYPH_MULTIPLY, size / 2);
    hud_number(x + size + size / 2, y + size / 8, value, size * 3 / 4, 0);
}

static void power_meter(int x, int y, int size) {
    void **lut = segmented_to_virtual(power_meter_health_segments_lut);
    image(x, y, texture_power_meter_left_side, 32, 64, size / 2, size);
    image(x + size / 2, y, texture_power_meter_right_side, 32, 64, size / 2, size);
    int wedges = gHudDisplay.wedges;
    if (wedges > 8) wedges = 8;
    if (wedges > 0) image(x + size / 4, y + size / 4, lut[wedges - 1], 32, 32, size / 2, size / 2);
}

static void camera(int x, int y) {
    void **lut = segmented_to_virtual(main_hud_camera_lut);
    if (sCamera == CAM_STATUS_NONE) return;
    image(x, y, lut[GLYPH_CAM_CAMERA], 16, 16, 24, 24);
    int mode = (sCamera & CAM_STATUS_FIXED) ? GLYPH_CAM_FIXED :
        (sCamera & CAM_STATUS_MARIO) ? GLYPH_CAM_MARIO_HEAD : GLYPH_CAM_LAKITU_HEAD;
    image(x + 26, y, lut[mode], 16, 16, 24, 24);
    if (sCamera & CAM_STATUS_C_UP) image(x + 6, y - 12, lut[GLYPH_CAM_ARROW_UP], 8, 8, 12, 12);
    if (sCamera & CAM_STATUS_C_DOWN) image(x + 6, y + 24, lut[GLYPH_CAM_ARROW_DOWN], 8, 8, 12, 12);
}

static void hud(void) {
    int flags = gHudDisplay.flags;
    if (!flags) return;
    if (sText) {
        if (flags & HUD_DISPLAY_FLAG_LIVES) counter(2, 4, GLYPH_MARIO_HEAD, gHudDisplay.lives, 16);
        if (flags & HUD_DISPLAY_FLAG_COIN_COUNT) counter(70, 4, GLYPH_COIN, gHudDisplay.coins, 16);
        if (flags & HUD_DISPLAY_FLAG_STAR_COUNT) counter(140, 4, GLYPH_STAR, gHudDisplay.stars, 16);
        if (flags & HUD_DISPLAY_FLAG_CAMERA_AND_POWER) power_meter(216, 0, 36);
        return;
    }
    if (flags & HUD_DISPLAY_FLAG_LIVES) counter(8, 12, GLYPH_MARIO_HEAD, gHudDisplay.lives, 32);
    if (flags & HUD_DISPLAY_FLAG_COIN_COUNT) counter(136, 12, GLYPH_COIN, gHudDisplay.coins, 32);
    if (flags & HUD_DISPLAY_FLAG_STAR_COUNT) counter(8, 60, GLYPH_STAR, gHudDisplay.stars, 32);
    /* The US HUD has no key bitmap in main_hud_lut. Only show this
     * otherwise absent counter when keys are actually held. */
    if ((flags & HUD_DISPLAY_FLAG_KEYS) && gHudDisplay.keys > 0) {
        ascii(138, 60, "KEYS", 8, 16);
        hud_number(174, 60, gHudDisplay.keys, 24, 0);
    }
    if (flags & HUD_DISPLAY_FLAG_CAMERA_AND_POWER) {
        power_meter(16, 96, 80);
        camera(166, 118);
    }
    if (flags & HUD_DISPLAY_FLAG_TIMER) {
        unsigned t = gHudDisplay.timer;
        ascii(8, 176, "TIME", 8, 16);
        hud_number(52, 172, t / 1800, 18, 0);
        hud_char(72, 172, GLYPH_APOSTROPHE, 18);
        hud_number(90, 172, t / 30 % 60, 18, 2);
        hud_char(126, 172, GLYPH_DOUBLE_QUOTE, 18);
        hud_number(144, 172, t % 30 / 3, 18, 0);
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
    upload_tiles();
    sText = NULL;
}
#endif
