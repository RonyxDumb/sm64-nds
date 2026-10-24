#ifndef NDS_TITLE_SCREEN_H
#define NDS_TITLE_SCREEN_H

enum NdsTitlePhase { NDS_TITLE_HIDDEN, NDS_TITLE_FADE_IN, NDS_TITLE_READY,
                     NDS_TITLE_FADE_OUT, NDS_TITLE_HANDOFF };
enum NdsTitleAction {
    NDS_TITLE_UP = 1, NDS_TITLE_DOWN = 2, NDS_TITLE_LEFT = 4,
    NDS_TITLE_RIGHT = 8, NDS_TITLE_CONFIRM = 16, NDS_TITLE_BACK = 32,
    NDS_TITLE_START = 64
};
#define NDS_TITLE_FADE_FRAMES 16
#define NDS_TITLE_SLIDE_FRAMES 10

struct NdsTitleState {
    int phase, fade, page, pageFrame, homeRow, optionRow;
    int cursorY, pulse[2], audio, fps;
    unsigned int ticks;
};

void nds_title_state_begin(struct NdsTitleState *state, int audio, int fps);
int nds_title_state_step(struct NdsTitleState *state, unsigned int actions);
int nds_title_slide(const struct NdsTitleState *state);
int nds_title_brightness(const struct NdsTitleState *state);

extern struct NdsTitleState gNdsTitleState;
extern volatile unsigned char gNdsAudioEnabled;
extern unsigned char gNdsShowFps;
extern volatile unsigned char gNdsFps;

int nds_title_begin(short arg, int unused);
int nds_title_update(void);
int nds_title_active(void);
void nds_title_leave(void);
void nds_title_frame_end(void);

#endif
