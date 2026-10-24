#ifndef NDS_GODDARD_ANIM_H
#define NDS_GODDARD_ANIM_H
#include <stdint.h>
#define NDS_GODDARD_ANIM_PACKED 0x80
unsigned int nds_goddard_frame(const uint8_t *source, unsigned int frame, int16_t *output);
#endif
