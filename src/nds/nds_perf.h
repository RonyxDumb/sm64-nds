#ifndef NDS_PERF_H
#define NDS_PERF_H

#define NDS_PERF_HZ (33513982u / 64u)
enum NdsPerfPhase {
    NDS_PERF_FRAME, NDS_PERF_LOGIC, NDS_PERF_RENDER, NDS_PERF_UI,
    NDS_PERF_AUDIO, NDS_PERF_SAMPLE, NDS_PERF_TEXTURE, NDS_PERF_VBLANK,
    NDS_PERF_NETWORK,
    NDS_PERF_SCRIPT, NDS_PERF_SCENE, NDS_PERF_OBJECTS, NDS_PERF_TERRAIN,
    NDS_PERF_COLLISIONS, NDS_PERF_ACTORS, NDS_PERF_OBJECT_CLEANUP,
    NDS_PERF_CAMERA, NDS_PERF_LEVEL_IO, NDS_PERF_AUDIO_SEQUENCES,
    NDS_PERF_AUDIO_GAME, NDS_PERF_AUDIO_PUBLISH, NDS_PERF_WAIT,
    NDS_PERF_PAINTING_WAVE, NDS_PERF_PAINTING_NORMALS, NDS_PERF_PAINTING_DRAW,
    NDS_PERF_PHASES
};
enum NdsPerfCounter {
    NDS_COUNT_COMMAND, NDS_COUNT_VERTEX, NDS_COUNT_POSTEST,
    NDS_COUNT_TEX_MISS, NDS_COUNT_TEX_EVICT, NDS_COUNT_TEX_BYTES,
    NDS_COUNT_AUDIO_BYTES, NDS_COUNT_COPY_PENDING, NDS_COUNT_COPY_STALL,
    NDS_COUNT_SURFACE_HIT, NDS_COUNT_SURFACE_MISS,
    NDS_COUNT_COMPILED_BATCH,
    NDS_COUNT_LIMIT
};
void nds_perf_init(void);
unsigned int nds_perf_now(void);
unsigned int nds_perf_begin_frame(void);
void nds_perf_record(enum NdsPerfPhase phase, unsigned int start);
void nds_perf_count(enum NdsPerfCounter counter, unsigned int value);
void nds_perf_request_dump(void);
void nds_perf_finish_frame(unsigned int start);
#ifdef TARGET_NDS
#define NDS_PERF_START(t) unsigned t = nds_perf_now()
#define NDS_PERF_END(p, t) nds_perf_record((p), (t))
#else
#define NDS_PERF_START(t)
#define NDS_PERF_END(p, t) ((void)0)
#endif
#endif
