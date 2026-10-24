#include <assert.h>
#include <stdio.h>
#include "nds/nds_title_screen.h"

static void frames(struct NdsTitleState *s, int count) {
    while (count--) nds_title_state_step(s, 0);
}

int main(void) {
    struct NdsTitleState s;
    nds_title_state_begin(&s, 1, 0);
    assert(s.audio == 1 && s.fps == 0 && nds_title_brightness(&s) == -16);
    for (int i = 0; i < NDS_TITLE_FADE_FRAMES; i++) {
        nds_title_state_step(&s, NDS_TITLE_CONFIRM);
        assert(s.page == 0 && s.phase != NDS_TITLE_FADE_OUT);
        assert(nds_title_brightness(&s) == i + 1 - 16);
    }
    assert(s.phase == NDS_TITLE_READY);
    nds_title_state_step(&s, NDS_TITLE_DOWN);
    nds_title_state_step(&s, NDS_TITLE_CONFIRM);
    assert(s.page == 1 && s.audio == 1);
    int previous = nds_title_slide(&s);
    while (s.pageFrame < NDS_TITLE_SLIDE_FRAMES) {
        nds_title_state_step(&s, NDS_TITLE_CONFIRM);
        assert(s.audio == 1); /* opening A must not toggle Audio */
        assert(nds_title_slide(&s) >= previous);
        previous = nds_title_slide(&s);
    }
    assert(nds_title_slide(&s) == 256);
    nds_title_state_step(&s, NDS_TITLE_CONFIRM);
    assert(s.audio == 0 && s.fps == 0);
    nds_title_state_step(&s, NDS_TITLE_LEFT);
    assert(s.audio == 0);
    nds_title_state_step(&s, NDS_TITLE_RIGHT);
    assert(s.audio == 1);
    nds_title_state_step(&s, NDS_TITLE_DOWN);
    nds_title_state_step(&s, NDS_TITLE_CONFIRM);
    assert(s.fps == 1 && s.audio == 1);
    nds_title_state_step(&s, NDS_TITLE_RIGHT);
    assert(s.fps == 1);
    nds_title_state_step(&s, NDS_TITLE_CONFIRM);
    assert(s.fps == 0 && s.audio == 1);
    nds_title_state_step(&s, NDS_TITLE_RIGHT);
    assert(s.fps == 1);
    nds_title_state_step(&s, NDS_TITLE_BACK);
    frames(&s, NDS_TITLE_SLIDE_FRAMES);
    assert(s.page == 0 && nds_title_slide(&s) == 0);
    assert(s.fps == 1 && s.homeRow == 1);
    nds_title_state_step(&s, NDS_TITLE_CONFIRM);
    frames(&s, NDS_TITLE_SLIDE_FRAMES);
    nds_title_state_step(&s, NDS_TITLE_START);
    frames(&s, NDS_TITLE_SLIDE_FRAMES);
    assert(s.phase == NDS_TITLE_READY && !s.page);
    nds_title_state_step(&s, NDS_TITLE_START);
    assert(s.phase == NDS_TITLE_FADE_OUT);
    for (int i = 1; i <= NDS_TITLE_FADE_FRAMES; i++) {
        nds_title_state_step(&s, NDS_TITLE_BACK | NDS_TITLE_CONFIRM);
        assert(nds_title_brightness(&s) == -i);
    }
    assert(s.phase == NDS_TITLE_HANDOFF);
    assert(s.audio == 1 && s.fps == 1);
    puts("Title menu: fade timing, slide, independent toggles and input isolation passed.");
}
