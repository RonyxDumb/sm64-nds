#include "nds_goddard_anim.h"

static unsigned int read16(const uint8_t *p) { return p[0] | ((unsigned int)p[1] << 8); }
static unsigned int read32(const uint8_t *p) { return read16(p) | (read16(p + 2) << 16); }

unsigned int nds_goddard_frame(const uint8_t *source, unsigned int frame, int16_t *output) {
    unsigned int count = read32(source), width = read16(source + 4);
    if (frame >= count) frame = count - 1;
    unsigned int block = frame / 16;
    const uint8_t *data = source + read32(source + 8 + block * 4);
    const uint8_t *bits = data + width * 2, *axisData = bits + width;
    unsigned int frames = count - block * 16;
    if (frames > 16) frames = 16;
    for (unsigned int axis = 0; axis < width; axis++) {
        int value = (int16_t)read16(data + axis * 2);
        unsigned int nbits = bits[axis], bit = 0;
        for (unsigned int f = 0; f < frame % 16; f++) {
            int delta = 0;
            if (nbits) {
                const uint8_t *p = axisData + (bit >> 3);
                unsigned int v = (p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16));
                delta = (v >> (bit & 7)) & ((1u << nbits) - 1);
                if (delta & (1u << (nbits - 1))) delta -= 1u << nbits;
            }
            value += delta;
            bit += nbits;
        }
        output[axis] = value;
        axisData += ((frames - 1) * nbits + 7) / 8;
    }
    return width;
}
