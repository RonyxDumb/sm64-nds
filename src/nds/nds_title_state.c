#include "nds_title_screen.h"

void nds_title_state_begin(struct NdsTitleState *s, int audio, int fps) {
    *s = (struct NdsTitleState) { 0 };
    s->phase = NDS_TITLE_FADE_IN;
    s->cursorY = 68 * 256;
    s->audio = !!audio;
    s->fps = !!fps;
}

int nds_title_slide(const struct NdsTitleState *s) {
    int t = s->pageFrame * 256 / NDS_TITLE_SLIDE_FRAMES;
    return t * t * (768 - 2 * t) / 65536;
}

int nds_title_brightness(const struct NdsTitleState *s) {
    if (s->phase == NDS_TITLE_FADE_IN) return s->fade - 16;
    if (s->phase == NDS_TITLE_FADE_OUT) return -s->fade;
    if (s->phase == NDS_TITLE_HANDOFF) return -16;
    return 0;
}

int nds_title_state_step(struct NdsTitleState *s, unsigned int actions) {
    int changed = 0;
    s->ticks++;
    for (int i = 0; i < 2; i++) if (s->pulse[i]) s->pulse[i]--;
    if (s->phase == NDS_TITLE_FADE_IN) {
        if (++s->fade >= NDS_TITLE_FADE_FRAMES) {
            s->phase = NDS_TITLE_READY;
            s->fade = 0;
        }
    } else if (s->phase == NDS_TITLE_FADE_OUT) {
        if (++s->fade >= NDS_TITLE_FADE_FRAMES) s->phase = NDS_TITLE_HANDOFF;
    } else if (s->phase == NDS_TITLE_READY) {
        int settled = s->pageFrame == (s->page ? NDS_TITLE_SLIDE_FRAMES : 0);
        if ((actions & NDS_TITLE_BACK) && s->page) s->page = 0;
        else if (settled && !s->page) {
            if (actions & (NDS_TITLE_UP | NDS_TITLE_DOWN)) s->homeRow ^= 1;
            if ((actions & NDS_TITLE_START) ||
                ((actions & NDS_TITLE_CONFIRM) && s->homeRow == 0)) {
                s->phase = NDS_TITLE_FADE_OUT;
                s->fade = 0;
            } else if ((actions & NDS_TITLE_CONFIRM) && s->homeRow == 1) {
                s->page = 1;
            }
        } else if (settled && s->page) {
            if (actions & NDS_TITLE_START) s->page = 0;
            else {
                if (actions & (NDS_TITLE_UP | NDS_TITLE_DOWN)) s->optionRow ^= 1;
                int *value = s->optionRow ? &s->fps : &s->audio;
                int previous = *value;
                if (actions & NDS_TITLE_LEFT) *value = 0;
                else if (actions & NDS_TITLE_RIGHT) *value = 1;
                else if (actions & NDS_TITLE_CONFIRM) *value ^= 1;
                if (*value != previous) {
                    s->pulse[s->optionRow] = 8;
                    changed = 1;
                }
            }
        }
    }
    if (s->page && s->pageFrame < NDS_TITLE_SLIDE_FRAMES) s->pageFrame++;
    if (!s->page && s->pageFrame > 0) s->pageFrame--;
    int homeY = 68 + s->homeRow * 42;
    int optionsY = 72 + s->optionRow * 42;
    int slide = nds_title_slide(s);
    int targetY = homeY * (256 - slide) + optionsY * slide;
    s->cursorY += (targetY - s->cursorY) / 2;
    return changed;
}
