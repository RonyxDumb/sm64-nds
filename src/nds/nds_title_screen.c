#include <ultra64.h>
#include "nds_bottom_ui.h"
#ifdef NDS_BOTTOM_UI
#include "nds_include.h"
#include "nds_title_screen.h"
#include "audio/external.h"
#include "engine/graph_node.h"
#include "game/memory.h"
#include "game/geo_misc.h"
#include "game/segment2.h"
#include "game/segment7.h"
#include "types.h"
#include "sm64.h"
#include "geo_commands.h"

struct NdsTitleState gNdsTitleState;
volatile unsigned char gNdsAudioEnabled = 1;
unsigned char gNdsShowFps;
volatile unsigned char gNdsFps;
volatile unsigned char gNdsTitleBgm;
extern u8 gNdsAudioDisabled;
static int sSceneFade;
static unsigned int sHeldDirection, sRepeat;

int nds_title_active(void) {
    return gNdsTitleState.phase != NDS_TITLE_HIDDEN;
}

int nds_title_begin(UNUSED short arg, UNUSED int unused) {
    nds_title_state_begin(&gNdsTitleState, gNdsAudioEnabled && !gNdsAudioDisabled, gNdsShowFps);
    sSceneFade = sRepeat = sHeldDirection = 0;
    gNdsTitleBgm = 1;
    setBrightness(3, -16);
    return 0;
}

void nds_title_leave(void) {
    if (!nds_title_active()) return;
    gNdsTitleState.phase = NDS_TITLE_HIDDEN;
    gNdsTitleBgm = 0;
    sSceneFade = NDS_TITLE_FADE_FRAMES;
}

/* Apply brightness after the freshly submitted frame reaches VBlank. */
void nds_title_frame_end(void) {
    if (nds_title_active()) setBrightness(3, nds_title_brightness(&gNdsTitleState));
    else if (sSceneFade) setBrightness(3, -(--sSceneFade));
}

int nds_title_update(void) {
    struct NdsTitleState *s = &gNdsTitleState;
    /* osContGetReadData already scanned this frame's buttons and touchscreen. */
    unsigned int down = keysDown(), held = keysHeld();
    unsigned int direction = held & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    if (direction != sHeldDirection) { sHeldDirection = direction; sRepeat = 0; }
    else if (direction && ++sRepeat >= 12 && (sRepeat - 12) % 4 == 0) down |= direction;
    unsigned int actions = 0;
    if (down & KEY_UP) actions |= NDS_TITLE_UP;
    if (down & KEY_DOWN) actions |= NDS_TITLE_DOWN;
    if (down & KEY_LEFT) actions |= NDS_TITLE_LEFT;
    if (down & KEY_RIGHT) actions |= NDS_TITLE_RIGHT;
    if (down & KEY_A) actions |= NDS_TITLE_CONFIRM;
    if (down & KEY_B) actions |= NDS_TITLE_BACK;
    if (down & KEY_START) actions |= NDS_TITLE_START;
    if ((down & KEY_TOUCH) && s->phase == NDS_TITLE_READY &&
        s->pageFrame == (s->page ? NDS_TITLE_SLIDE_FRAMES : 0)) {
        touchPosition p;
        touchRead(&p);
        if (p.py >= 54 && p.py < 92) {
            if (s->page) s->optionRow = 0; else s->homeRow = 0;
            actions = NDS_TITLE_CONFIRM;
        } else if (p.py >= 96 && p.py < 136) {
            if (s->page) s->optionRow = 1; else s->homeRow = 1;
            actions = NDS_TITLE_CONFIRM;
        } else if (s->page && p.py >= 158) actions = NDS_TITLE_BACK;
    }
    int oldPage = s->page, oldRow = s->page ? s->optionRow : s->homeRow;
    int oldPhase = s->phase;
    nds_title_state_step(s, actions);
    if (gNdsAudioDisabled) s->audio = 0;
    gNdsAudioEnabled = s->audio;
    gNdsShowFps = s->fps;
    if (oldPhase != NDS_TITLE_FADE_OUT && s->phase == NDS_TITLE_FADE_OUT)
        play_sound(SOUND_MENU_STAR_SOUND, gGlobalSoundSource);
    else if (oldPage != s->page || oldRow != (s->page ? s->optionRow : s->homeRow) ||
             s->pulse[0] == 8 || s->pulse[1] == 8)
        play_sound(SOUND_MENU_CHANGE_SELECT, gGlobalSoundSource);
    return s->phase == NDS_TITLE_HANDOFF ? 100 : 0;
}

int nds_title_update_level(UNUSED short arg, UNUSED int unused) {
    return nds_title_update();
}

int nds_title_handoff(UNUSED short arg, UNUSED int unused) {
    nds_title_leave();
    return 0;
}

Gfx *geo_nds_title_skybox(s32 context, UNUSED struct GraphNode *node, UNUSED void *data) {
    if (context != GEO_CONTEXT_RENDER) return NULL;
    extern const u8 *const water_skybox_ptrlist[];
    const u8 *const *textures = segmented_to_virtual(water_skybox_ptrlist);
    /* Show the panorama's sky at native texture resolution, with just a thin
     * ocean horizon. Gameplay keeps its original camera-facing skybox. */
    Gfx *dl = alloc_display_list((5 + 45 * 7) * sizeof(*dl)), *p = dl;
    Mtx *ortho = alloc_display_list(sizeof(*ortho));
    Vtx *vertices = alloc_display_list(45 * 4 * sizeof(*vertices));
    if (!dl || !ortho || !vertices) return NULL;
    guOrtho(ortho, 0, 320, 0, 240, 0, 3, 1);
    gSPDisplayList(p++, dl_skybox_begin);
    gSPMatrix(p++, ortho, G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
    gSPDisplayList(p++, dl_skybox_tile_tex_settings);
    int offset = (gNdsTitleState.ticks / 16) % 320;
    int drift = offset % 40, firstColumn = offset / 40;
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 9; col++) {
            Vtx *v = vertices + (row * 9 + col) * 4;
            int x = col * 40 - drift, y = 240 - row * 56;
            make_vertex(v, 0, x, y, -1, 0, 0, 255, 255, 255, 255);
            make_vertex(v, 1, x, y - 56, -1, 0, 31 << 5, 255, 255, 255, 255);
            make_vertex(v, 2, x + 40, y - 56, -1, 31 << 5, 31 << 5, 255, 255, 255, 255);
            make_vertex(v, 3, x + 40, y, -1, 31 << 5, 0, 255, 255, 255, 255);
            gLoadBlockTexture(p++, 32, 32, G_IM_FMT_RGBA,
                              textures[row * 10 + (firstColumn + col) % 8]);
            gSPVertex(p++, v, 4, 0);
            gSPDisplayList(p++, dl_draw_quad_verts_0123);
        }
    }
    gSPDisplayList(p++, dl_skybox_end);
    gSPEndDisplayList(p);
    return dl;
}

Gfx *geo_nds_title_logo(s32 context, struct GraphNode *node, UNUSED void *data) {
    if (context != GEO_CONTEXT_RENDER) return NULL;
    Mtx *matrix = alloc_display_list(sizeof(*matrix));
    Gfx *dl = alloc_display_list(4 * sizeof(*dl));
    if (!matrix || !dl) return NULL;
    node->flags = (node->flags & 0xFF) | (LAYER_OPAQUE << 8);
    guTranslate(matrix, 6.0f, 420.0f, 0.0f);
    gSPMatrix(dl, matrix, G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);
    gSPDisplayList(dl + 1, &intro_seg7_dl_logo);
    gSPPopMatrix(dl + 2, G_MTX_MODELVIEW);
    gSPEndDisplayList(dl + 3);
    return dl;
}
#endif
