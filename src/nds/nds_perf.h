#ifndef NDS_PERF_H
#define NDS_PERF_H

#define NDS_PERF_HZ (33513982u / 64u)
enum NdsPerfPhase {
    NDS_PERF_FRAME, NDS_PERF_LOGIC, NDS_PERF_RENDER, NDS_PERF_UI,
    NDS_PERF_AUDIO, NDS_PERF_SAMPLE, NDS_PERF_TEXTURE, NDS_PERF_VBLANK,
    NDS_PERF_NETWORK, NDS_PERF_PHASES
};
enum NdsPerfCounter {
    NDS_COUNT_COMMAND, NDS_COUNT_VERTEX, NDS_COUNT_POSTEST,
    NDS_COUNT_TEX_MISS, NDS_COUNT_TEX_EVICT, NDS_COUNT_TEX_BYTES,
    NDS_COUNT_AUDIO_BYTES, NDS_COUNT_COPY_PENDING, NDS_COUNT_COPY_STALL,
    NDS_COUNT_LIMIT
};
void nds_perf_init(void);
unsigned int nds_perf_now(void);
void nds_perf_record(enum NdsPerfPhase phase, unsigned int start);
void nds_perf_count(enum NdsPerfCounter counter, unsigned int value);
void nds_perf_request_dump(void);
void nds_perf_finish_frame(unsigned int start);
#endif
