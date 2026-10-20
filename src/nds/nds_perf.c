#include <stdio.h>
#include <string.h>
#include "nds_include.h"
#include "nds_perf.h"
#include "game/level_update.h"
#include "game/area.h"

struct PerfPhase { unsigned long long total; unsigned int calls, max; };
struct PerfArea { unsigned long long total; unsigned int frames, max, slow; int level, area; };
static struct PerfPhase sPhases[NDS_PERF_PHASES];
static unsigned int sCounters[NDS_COUNT_LIMIT];
static struct PerfArea sAreas[80];
static int sAreaCount;
static volatile int sReady, sDump;
static const char *const sNames[] = {
    "frame", "logic_inclusive", "renderer_inclusive", "bottom_ui_inclusive",
    "audio_irq", "sample_io_inclusive", "texture_upload", "vblank_irq",
    "network_inclusive"
};

void nds_perf_init(void) {
    /* ARM9 timer 0 is optional FPS; timer 3 belongs to Wi-Fi.
     * ARM7 has a separate timer bank and keeps its audio timer 0. */
    TIMER1_CR = 0; TIMER2_CR = 0;
    TIMER1_DATA = 0; TIMER2_DATA = 0;
    TIMER2_CR = TIMER_ENABLE | TIMER_CASCADE;
    TIMER1_CR = TIMER_ENABLE | TIMER_DIV_64;
    sReady = 1;
}

unsigned int nds_perf_now(void) {
    unsigned hi, lo, again;
    if (!sReady) return 0;
    do { hi = TIMER2_DATA; lo = TIMER1_DATA; again = TIMER2_DATA; } while (hi != again);
    return (hi << 16) | lo;
}

void nds_perf_record(enum NdsPerfPhase phase, unsigned int start) {
    unsigned ticks = nds_perf_now() - start;
    struct PerfPhase *p = &sPhases[phase];
    p->total += ticks; p->calls++;
    if (ticks > p->max) p->max = ticks;
}

void nds_perf_count(enum NdsPerfCounter counter, unsigned int value) {
    sCounters[counter] += value;
}

void nds_perf_request_dump(void) { sDump = 1; }

static void dump(void) {
    struct PerfPhase phases[NDS_PERF_PHASES];
    unsigned counters[NDS_COUNT_LIMIT];
    struct PerfArea areas[80];
    int n;
    int irq = enterCriticalSection();
    memcpy(phases, sPhases, sizeof(phases));
    memcpy(counters, sCounters, sizeof(counters));
    memcpy(areas, sAreas, sizeof(areas)); n = sAreaCount;
    leaveCriticalSection(irq);
    FILE *f = fopen("fat:/sm64-perf.csv", "w");
    if (!f) f = fopen("sd:/sm64-perf.csv", "w");
    if (!f) return;
    fprintf(f, "# Timer Hz,%u\n# Inclusive phases overlap IRQs; do not sum phase totals.\n", NDS_PERF_HZ);
    fprintf(f, "phase,calls,total_ticks,max_ticks\n");
    for (int i = 0; i < NDS_PERF_PHASES; i++)
        fprintf(f, "%s,%u,%llu,%u\n", sNames[i], phases[i].calls, phases[i].total, phases[i].max);
    fprintf(f, "counter,value\n");
    static const char *const names[] = {"dl_commands","emitted_vertices","position_tests","texture_misses","texture_evictions","texture_bytes","sample_bytes","remote_submissions","remote_late_polls"};
    for (int i = 0; i < NDS_COUNT_LIMIT; i++) fprintf(f, "%s,%u\n", names[i], counters[i]);
    fprintf(f, "level,area,frames,total_ticks,max_ticks,frames_over_34ms\n");
    for (int i = 0; i < n; i++) fprintf(f, "%d,%d,%u,%llu,%u,%u\n", areas[i].level, areas[i].area, areas[i].frames, areas[i].total, areas[i].max, areas[i].slow);
    fclose(f);
}

void nds_perf_finish_frame(unsigned int start) {
    unsigned ticks = nds_perf_now() - start;
    nds_perf_record(NDS_PERF_FRAME, start);
    int i;
    for (i = 0; i < sAreaCount; i++) if (sAreas[i].level == gCurrLevelNum && sAreas[i].area == gCurrAreaIndex) break;
    if (i == sAreaCount && i < 80) { sAreaCount++; sAreas[i].level = gCurrLevelNum; sAreas[i].area = gCurrAreaIndex; }
    if (i < 80) {
        struct PerfArea *p = &sAreas[i];
        p->frames++; p->total += ticks;
        if (ticks > p->max) p->max = ticks;
        if (ticks > NDS_PERF_HZ * 34u / 1000u) p->slow++;
    }
    /* Disk I/O is explicit, on SELECT release, and is excluded from
     * the sampled frame. Never write from the audio/VBlank IRQ. */
    if (sDump) { sDump = 0; dump(); }
}
