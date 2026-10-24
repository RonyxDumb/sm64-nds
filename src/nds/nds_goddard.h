#ifndef NDS_GODDARD_H
#define NDS_GODDARD_H
#include <ultra64.h>

struct NdsGoddardApi {
    u32 version, size;
    void (*init)(void *, u32);
    void (*setup)(void);
    void (*make)(s32);
    Gfx *(*get)(s32);
    void (*copyPad)(OSContPad *);
    void (*vblank)(void);
    s32 (*sfx)(void);
};
int nds_goddard_load(int scene);
Gfx *nds_goddard_render(int scene);
#endif
