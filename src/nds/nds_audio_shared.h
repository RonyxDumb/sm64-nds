#ifndef NDS_AUDIO_SHARED_H
#define NDS_AUDIO_SHARED_H
#include <stdint.h>
#define NDS_AUDIO_CHANNELS 16
struct NdsAudioNote {
    uint32_t ndsSourceFull, ndsSourceHalf;
    float frequency;
    uint16_t targetVolLeft, targetVolRight;
    uint16_t ndsRepeatFull, ndsLengthFull, ndsRepeatHalf, ndsLengthHalf;
    uint8_t enabled, ndsMuted, ndsLoop, ndsSeq;
    uint32_t reserved;
};
_Static_assert(sizeof(struct NdsAudioNote) == 32, "Audio packet must fit one cache line");
#endif
