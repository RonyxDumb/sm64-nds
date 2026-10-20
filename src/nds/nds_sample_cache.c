#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nds_include.h"
#include "nds_sample_cache.h"
#include "nds_perf.h"
#include "nds_arm7_copy.h"

#include "audio/internal.h"
#include "audio/data.h"
#include "audio/load.h"

#define SC_MAX_BANKS      48
#define SC_MAX_BLOBS      16
#define SC_MAX_ENTRIES    256
#define SC_MAX_REFS       512
#define SC_ENTRY_NONE     0xFFFF
#define SC_NODE_NONE      0xFFFF

#define SC_MISS_RING      64
#define SC_QUARANTINE_LEN 24
#define SC_QUARANTINE_TICKS 4

#define SC_ARENA_MAX      0x60000

#define SC_ARENA_MIN      0x8000
#define SC_ARENA_STEP     0x4000
#define SC_HEAP_RESERVE   0x4000

#define SC_FRAME_BUDGET   0x4000
#define SC_PREFETCH_RESERVE_SHIFT 3

enum { SC_ABSENT = 0, SC_LOADING = 1, SC_RESIDENT = 2, SC_FAILED = 3 };

#define SC_F_IN_MISS_RING 0x01
#define SC_F_IN_PREFETCH  0x02

struct ScEntry {
    u32 fileOff;
    u32 size;
    u8 *addr;
    u32 lastUsed;
    u32 halfOff;

    u8 state;
    u8 flags;
    u16 pad;
};

struct ScFreeBlock {
    u8 *addr;
    u32 size;
    u16 next;
};

struct ScQuarantine {
    u8 *addr;
    u32 size;
    u32 tick;
};

volatile u32 gNdsAudioTick;

volatile u8 gNdsScacheFail;
volatile u16 gNdsScacheEntries;
volatile u32 gNdsScacheArena;
volatile u32 gNdsScacheRefs;
volatile u32 gNdsCntResolve;
volatile u32 gNdsCntResolveHit;
volatile u32 gNdsCntResolveMiss;
volatile u32 gNdsCntResolveBadIdx;
volatile u32 gNdsCntResolveNoSnd;
volatile u32 gNdsCntLoads;
volatile u32 gNdsCntChanScript;
volatile u32 gNdsCntSetLayer;
volatile u32 gNdsCntAllocNote;

static struct ScEntry sEntries[SC_MAX_ENTRIES];
static u16 sEntryCount;

static u32 sBankBlobOff[SC_MAX_BANKS];
static u16 sBankRefStart[SC_MAX_BANKS];
static u16 sBankRefCount[SC_MAX_BANKS];
static u16 sBankRefs[SC_MAX_REFS];
static s32 sBankCount;

static u32 sBlobOff[SC_MAX_BLOBS];
static u32 sBlobLen[SC_MAX_BLOBS];
static u16 sBlobEntryStart[SC_MAX_BLOBS];
static u16 sBlobEntryCount[SC_MAX_BLOBS];
static s32 sBlobCount;

static u8 *sArena;
static u32 sArenaSize;
static u32 sTotalCache;
static u32 sFreeBytes;
static u32 sQuarBytes;
static struct ScFreeBlock sFreeNodes[SC_MAX_ENTRIES + SC_QUARANTINE_LEN + 8];
static u16 sFreeHead = SC_NODE_NONE;
static u16 sFreeNodePool = SC_NODE_NONE;

static struct ScQuarantine sQuarantine[SC_QUARANTINE_LEN];
static u8 sQuarHead, sQuarCount;

static volatile u16 sMissRing[SC_MISS_RING];
static volatile u8 sMissHead, sMissTail;

static u16 sPrefetchQueue[SC_MAX_ENTRIES];
static u16 sPrefetchHead, sPrefetchTail;

static u16 sLoadEntry = SC_ENTRY_NONE;
static u32 sLoadDone;

static u8 sStage[NDS_COPY_MAX_CHUNK] __attribute__((aligned(32)));

static FILE *sTblFile;
static long sTblPosition = -1;
static u8 sReady;

#define SC_IS_REMOTE(a) ((u32) (a) >= 0x03000000)

static u8 sRemotePending;
static u32 sRemoteDest, sRemoteLen, sRemoteStarted;

static int remote_copy_begin(u32 dst, u32 len) {
    struct NdsCopyCmd cmd;
    /* Only the main-thread loader owns sStage, and it stays immutable
     * until ARM7 acknowledges. There is no polling loop on ARM9. */
    if (sRemotePending) return 0;
    while (fifoCheckValue32(NDS_COPY_FIFO_CHANNEL)) fifoGetValue32(NDS_COPY_FIFO_CHANNEL);
    DC_FlushRange(sStage, len);
    cmd.dst = dst; cmd.src = (u32)sStage; cmd.len = len;
    if (!fifoSendDatamsg(NDS_COPY_FIFO_CHANNEL, sizeof(cmd), (u8 *)&cmd)) return 0;
    sRemoteDest = dst; sRemoteLen = len; sRemoteStarted = nds_perf_now();
    sRemotePending = 1;
    nds_perf_count(NDS_COUNT_COPY_PENDING, 1);
    return 1;
}

static int remote_copy_poll(void) {
    if (!fifoCheckValue32(NDS_COPY_FIFO_CHANNEL)) {
        if (nds_perf_now() - sRemoteStarted > NDS_PERF_HZ / 20)
            nds_perf_count(NDS_COUNT_COPY_STALL, 1);
        return 0;
    }
    unsigned ack = fifoGetValue32(NDS_COPY_FIFO_CHANNEL);
    sRemotePending = 0;
    return ack == sRemoteDest ? 1 : -1;
}

static struct { u32 blob; u32 off; } sInitRefs[SC_MAX_REFS];
static u32 sInitOffsets[SC_MAX_ENTRIES];

extern u8 *gSoundDataADSR;
extern u8 *gSoundDataRaw;
void nds_debug_printf(const char *fmt, ...);

#define RD32(p) (*(const u32 *) (const void *) (p))
#define SC_ALIGN(x) (((x) + 15) & ~(u32) 15)

static u16 node_take(void) {
    u16 n = sFreeNodePool;

    if (n == SC_NODE_NONE) {
        return SC_NODE_NONE;
    }
    sFreeNodePool = sFreeNodes[n].next;
    return n;
}

static void node_give(u16 n) {
    sFreeNodes[n].next = sFreeNodePool;
    sFreeNodePool = n;
}

static void free_block(u8 *addr, u32 size) {
    u16 prev = SC_NODE_NONE;
    u16 cur = sFreeHead;

    sFreeBytes += size;
    while (cur != SC_NODE_NONE && sFreeNodes[cur].addr < addr) {
        prev = cur;
        cur = sFreeNodes[cur].next;
    }
    if (prev != SC_NODE_NONE && sFreeNodes[prev].addr + sFreeNodes[prev].size == addr) {
        sFreeNodes[prev].size += size;
        if (cur != SC_NODE_NONE && sFreeNodes[prev].addr + sFreeNodes[prev].size == sFreeNodes[cur].addr) {
            sFreeNodes[prev].size += sFreeNodes[cur].size;
            sFreeNodes[prev].next = sFreeNodes[cur].next;
            node_give(cur);
        }
        return;
    }
    if (cur != SC_NODE_NONE && addr + size == sFreeNodes[cur].addr) {
        sFreeNodes[cur].addr = addr;
        sFreeNodes[cur].size += size;
        return;
    }
    {
        u16 n = node_take();
        if (n == SC_NODE_NONE) {
            /* Metadata pool exhausted: keep the allocator consistent. */
            sFreeBytes -= size;
            return;
        }
        sFreeNodes[n].addr = addr;
        sFreeNodes[n].size = size;
        sFreeNodes[n].next = cur;
        if (prev == SC_NODE_NONE) {
            sFreeHead = n;
        } else {
            sFreeNodes[prev].next = n;
        }
    }
}

static u8 *alloc_block(u32 size) {
    u16 prev = SC_NODE_NONE;
    u16 cur = sFreeHead;

    size = SC_ALIGN(size);
    while (cur != SC_NODE_NONE) {
        struct ScFreeBlock *b = &sFreeNodes[cur];
        if (b->size >= size) {
            u8 *addr = b->addr;
            if (b->size == size) {
                if (prev == SC_NODE_NONE) {
                    sFreeHead = b->next;
                } else {
                    sFreeNodes[prev].next = b->next;
                }
                node_give(cur);
            } else {
                b->addr += size;
                b->size -= size;
            }
            sFreeBytes -= size;
            return addr;
        }
        prev = cur;
        cur = b->next;
    }
    return NULL;
}

static void quarantine_flush(void) {
    while (sQuarCount > 0) {
        struct ScQuarantine *q = &sQuarantine[sQuarHead];
        if ((u32) (gNdsAudioTick - q->tick) < SC_QUARANTINE_TICKS) {
            break;
        }
        free_block(q->addr, q->size);
        sQuarBytes -= q->size;
        sQuarHead = (sQuarHead + 1) % SC_QUARANTINE_LEN;
        sQuarCount--;
    }
}

static int quarantine_push(u8 *addr, u32 size) {
    u8 slot;
    if (sQuarCount == SC_QUARANTINE_LEN) {
        quarantine_flush();
        if (sQuarCount == SC_QUARANTINE_LEN) {
            return 0;
        }
    }
    slot = (sQuarHead + sQuarCount) % SC_QUARANTINE_LEN;
    sQuarantine[slot].addr = addr;
    sQuarantine[slot].size = SC_ALIGN(size);
    sQuarantine[slot].tick = gNdsAudioTick;
    sQuarBytes += sQuarantine[slot].size;
    sQuarCount++;
    return 1;
}

static int entry_in_use(u16 idx) {
    s32 i;
    if (gNotes == NULL) {
        return 0;
    }
    for (i = 0; i < gMaxSimultaneousNotes; i++) {
        if (gNotes[i].enabled && gNotes[i].ndsEntry == idx) {
            return 1;
        }
    }
    return 0;
}

static int evict_one(void) {
    u16 best = SC_ENTRY_NONE;
    u32 bestAge = 0;
    u16 i;

    /* One LRU pass. Entries currently used by a Note are never candidates. */
    for (i = 0; i < sEntryCount; i++) {
        struct ScEntry *e = &sEntries[i];
        if (e->state == SC_RESIDENT && !entry_in_use(i)) {
            u32 age = gNdsAudioTick - e->lastUsed;
            if (best == SC_ENTRY_NONE || age > bestAge) {
                best = i;
                bestAge = age;
            }
        }
    }
    if (best == SC_ENTRY_NONE) {
        return 0;
    }
    {
        struct ScEntry *e = &sEntries[best];
        u8 *addr = e->addr;
        if (addr == NULL || !quarantine_push(addr, e->size)) {
            return 0;
        }
        e->addr = NULL;
        e->halfOff = 0;
        e->state = SC_ABSENT;
        return 1;
    }
}

static u8 *reserve_space(u32 size, int allowEvict) {
    u32 aligned = SC_ALIGN(size);
    for (;;) {
        u8 *addr = alloc_block(aligned);
        if (addr != NULL) {
            return addr;
        }
        quarantine_flush();
        addr = alloc_block(aligned);
        if (addr != NULL) {
            return addr;
        }
        if (!allowEvict || !evict_one()) {
            return NULL;
        }
    }
}

static int tbl_read_at(u32 offset, void *dst, u32 len) {
    if (sTblFile == NULL || dst == NULL || len == 0) {
        return 0;
    }

    if (sTblPosition != (long) offset) {
        if (fseek(sTblFile, (long) offset, SEEK_SET) != 0) {
            sTblPosition = -1;
            return 0;
        }
        sTblPosition = (long) offset;
    }

    if (fread(dst, 1, len, sTblFile) != len) {
        sTblPosition = -1;
        return 0;
    }
    sTblPosition += (long) len;
    return 1;
}

static void load_failed(struct ScEntry *e) {
    /* Called only after a failed read/send or a received ACK: no remote
     * DMA/copy is still allowed to own this allocation or sStage. */
    free_block(e->addr, SC_ALIGN(e->size));
    e->addr = NULL; e->state = SC_FAILED;
    sLoadEntry = SC_ENTRY_NONE;
}

static u32 load_pump(u32 budget) {
    if (sLoadEntry == SC_ENTRY_NONE || sLoadEntry >= sEntryCount || !budget) return 0;
    struct ScEntry *e = &sEntries[sLoadEntry];
    if (sRemotePending) {
        int result = remote_copy_poll();
        if (!result) return 0;
        if (result < 0) { load_failed(e); return 0; }
        sLoadDone += sRemoteLen;
    }
    if (sLoadDone >= e->size) {
        e->lastUsed = gNdsAudioTick; e->state = SC_RESIDENT;
        sLoadEntry = SC_ENTRY_NONE; gNdsCntLoads++;
        return 0;
    }
    u32 want = e->size - sLoadDone;
    if (want > budget) want = budget;
    int remote = SC_IS_REMOTE(e->addr);
    u32 chunk = remote ? NDS_COPY_MAX_CHUNK : 0x1000;
    if (want > chunk) want = chunk;
    u8 *dst = remote ? sStage : e->addr + sLoadDone;
    if (!tbl_read_at(e->fileOff + sLoadDone, dst, want)) { load_failed(e); return want; }
    nds_perf_count(NDS_COUNT_AUDIO_BYTES, want);
    if (!sLoadDone) e->halfOff = RD32(dst);
    if (remote) {
        if (!remote_copy_begin((u32)e->addr + sLoadDone, want)) load_failed(e);
    } else {
        DC_FlushRange(dst, want);
        sLoadDone += want;
        if (sLoadDone >= e->size) {
            e->lastUsed = gNdsAudioTick; e->state = SC_RESIDENT;
            sLoadEntry = SC_ENTRY_NONE; gNdsCntLoads++;
        }
    }
    return want;
}

static int load_begin(u16 idx, int allowEvict) {
    struct ScEntry *e;
    u8 *addr;

    if (idx >= sEntryCount) {
        return 0;
    }
    e = &sEntries[idx];
    if (e->state != SC_ABSENT || sLoadEntry != SC_ENTRY_NONE) {
        return 0;
    }
    addr = reserve_space(e->size, allowEvict);
    if (addr == NULL) {
        return 0;
    }
    e->addr = addr;
    e->state = SC_LOADING;
    sLoadEntry = idx;
    sLoadDone = 0;
    return 1;
}

static s32 blob_index(u32 blobOff) {
    s32 k;
    for (k = 0; k < sBlobCount; k++) {
        if (sBlobOff[k] == blobOff) {
            return k;
        }
    }
    return -1;
}

static u32 entry_lookup(s32 blobIdx, u32 fileOff) {
    u16 lo;
    u16 hi;
    u16 end;

    if (blobIdx < 0 || blobIdx >= sBlobCount) {
        return SC_ENTRY_NONE;
    }
    lo = sBlobEntryStart[blobIdx];
    hi = lo + sBlobEntryCount[blobIdx];
    end = hi;
    while (lo < hi) {
        u16 mid = (lo + hi) / 2;
        if (sEntries[mid].fileOff < fileOff) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < end && sEntries[lo].fileOff == fileOff) {
        return lo;
    }
    return SC_ENTRY_NONE;
}

u32 nds_scache_entry_for(s32 bankId, u32 blobRelOffset) {
    s32 blobIdx;
    if (!sReady || bankId < 0 || bankId >= sBankCount) {
        return SC_ENTRY_NONE;
    }
    blobIdx = blob_index(sBankBlobOff[bankId]);
    if (blobIdx < 0) {
        return SC_ENTRY_NONE;
    }
    return entry_lookup(blobIdx, sBankBlobOff[bankId] + blobRelOffset);
}

static int queue_miss(u16 idx) {
    u8 head;
    u8 next;
    struct ScEntry *e;

    if (idx >= sEntryCount) {
        return 0;
    }
    e = &sEntries[idx];
    if (e->state != SC_ABSENT || (e->flags & SC_F_IN_MISS_RING)) {
        return e->state != SC_ABSENT;
    }

    head = sMissHead;
    next = (u8) ((head + 1) % SC_MISS_RING);
    if (next == sMissTail) {
        return 0;
    }
    e->flags |= SC_F_IN_MISS_RING;
    sMissRing[head] = idx;
    sMissHead = next;
    return 1;
}

static int queue_prefetch(u16 idx) {
    struct ScEntry *e;

    if (idx >= sEntryCount) {
        return 0;
    }
    e = &sEntries[idx];
    if (e->state != SC_ABSENT || (e->flags & SC_F_IN_PREFETCH)) {
        return e->state != SC_ABSENT;
    }
    if ((u16) (sPrefetchTail - sPrefetchHead) >= SC_MAX_ENTRIES) {
        return 0;
    }
    e->flags |= SC_F_IN_PREFETCH;
    sPrefetchQueue[sPrefetchTail % SC_MAX_ENTRIES] = idx;
    sPrefetchTail++;
    return 1;
}

void nds_scache_note_resolve(struct Note *note) {
    struct AudioBankSample *sample;
    struct ScEntry *e;
    u8 *addr;
    u32 idx, ls, le, half;

    note->ndsSourceFull = 0;
    note->ndsSourceHalf = 0;
    note->ndsEntry = SC_ENTRY_NONE;

    gNdsCntResolve++;
    if (!sReady || note->sound == NULL || note->sound->sample == NULL) {
        gNdsCntResolveNoSnd++;
        return;
    }
    sample = note->sound->sample;
    idx = (u32) (uintptr_t) sample->sampleAddr;
    if (idx >= sEntryCount) {
        gNdsCntResolveBadIdx++;
        return;
    }
    e = &sEntries[idx];
    if (e->state != SC_RESIDENT) {

        note->ndsEntry = idx;
        gNdsCntResolveMiss++;
        if (e->state == SC_ABSENT) {
            queue_miss((u16) idx);
        }
        return;
    }
    gNdsCntResolveHit++;
    addr = e->addr;
    if (addr == NULL) {
        return;
    }
    e->lastUsed = gNdsAudioTick;

    ls = sample->loop->start;
    le = sample->loop->end;
    half = e->halfOff;

    note->ndsEntry = idx;
    note->ndsLoop = (sample->loop->count != 0);
    note->ndsRepeatFull = ls / sizeof(u32) + 1;
    note->ndsLengthFull = (le - ls) / sizeof(u32) + 1;
    if (half != 0) {
        note->ndsRepeatHalf = ls / 2 / sizeof(u32) + 1;
        note->ndsLengthHalf = (le - ls) / 2 / sizeof(u32) + 1;
        note->ndsSourceHalf = (u32) addr + half + 4;
    }
    note->ndsSourceFull = (u32) addr + 4;
}

void nds_scache_retry_pending(void) {
    s32 i;
    if (!sReady || gNotes == NULL) {
        return;
    }
    for (i = 0; i < gMaxSimultaneousNotes; i++) {
        struct Note *note = &gNotes[i];
        if (note->enabled && note->ndsSourceFull == 0 && note->ndsEntry < sEntryCount) {
            struct ScEntry *e = &sEntries[note->ndsEntry];
            if (e->state == SC_RESIDENT) {
                nds_scache_note_resolve(note);
                if (note->ndsSourceFull != 0) {
                    note->ndsSeq++;
                }
            } else if (e->state == SC_ABSENT) {
                /* Recover automatically if the miss ring was previously full. */
                queue_miss(note->ndsEntry);
            }
        }
    }
}

void nds_scache_bank_loaded(s32 bankId) {
    if (!sReady || bankId < 0 || bankId >= sBankCount) return;
    unsigned start = sBankRefStart[bankId], count = sBankRefCount[bankId];
    /* This hook may run inside the audio IRQ. Queue bank warming instead
     * of reading hundreds of KiB from the filesystem in that interrupt. */
    for (unsigned i = 0; i < count; i++) {
        u16 idx = sBankRefs[start + i];
        if (idx >= sEntryCount) continue;
        if (sEntries[idx].state == SC_RESIDENT) sEntries[idx].lastUsed = gNdsAudioTick;
        else queue_prefetch(idx);
    }
}

void nds_scache_update(void) {
    if (!sReady) return;
    u32 start = nds_perf_now(), budget = SC_FRAME_BUDGET;
    quarantine_flush();
    /* The deadline is checked between bounded reads. A filesystem read
     * itself is synchronous and can exceed it on a slow flashcart. */
    while (budget && nds_perf_now() - start < NDS_PERF_HZ / 500) {
        if (sLoadEntry == SC_ENTRY_NONE) {
            u16 idx = SC_ENTRY_NONE;
            int urgent = 0;
            while (sMissTail != sMissHead) {
                u16 next = sMissRing[sMissTail];
                sMissTail = (sMissTail + 1) % SC_MISS_RING;
                sEntries[next].flags &= ~SC_F_IN_MISS_RING;
                if (sEntries[next].state == SC_ABSENT) { idx = next; urgent = 1; break; }
            }
            while (idx == SC_ENTRY_NONE && sPrefetchHead != sPrefetchTail) {
                u16 next = sPrefetchQueue[sPrefetchHead++ % SC_MAX_ENTRIES];
                struct ScEntry *e = &sEntries[next];
                e->flags &= ~SC_F_IN_PREFETCH;
                if (e->state != SC_ABSENT) continue;
                if (sFreeBytes + sQuarBytes < (sTotalCache >> SC_PREFETCH_RESERVE_SHIFT) + SC_ALIGN(e->size)) continue;
                idx = next;
            }
            if (idx == SC_ENTRY_NONE) break;
            if (!load_begin(idx, urgent)) continue;
        }
        u32 used = load_pump(budget);
        budget -= used;
        if (sRemotePending) break;
        if (!used && sLoadEntry != SC_ENTRY_NONE) break;
    }
}

s32 nds_scache_init(void) {
    u16 bankCount, tblCount;
    s32 refCount = 0;
    s32 b, k;

    sReady = 0;
    gNdsScacheFail = 0;
    sEntryCount = 0;
    sBlobCount = 0;
    sMissHead = 0;
    sMissTail = 0;
    sPrefetchHead = 0;
    sPrefetchTail = 0;
    sQuarHead = 0;
    sQuarCount = 0;
    sQuarBytes = 0;
    sLoadEntry = SC_ENTRY_NONE;
    sLoadDone = 0;
    sTblPosition = -1;
    sRemotePending = 0;

    memset(sEntries, 0, sizeof(sEntries));
    memset(sBankRefs, 0xFF, sizeof(sBankRefs));
    memset((void *) sMissRing, 0xFF, sizeof(sMissRing));
    memset(sPrefetchQueue, 0xFF, sizeof(sPrefetchQueue));
    if (gSoundDataADSR == NULL || gSoundDataRaw == NULL) {
        gNdsScacheFail = 1;
        return -1;
    }

    bankCount = *(u16 *) (gSoundDataADSR + 2);
    tblCount = *(u16 *) (gSoundDataRaw + 2);
    if (bankCount == 0 || bankCount != tblCount || bankCount > SC_MAX_BANKS) {
        gNdsScacheFail = 2;
        return -1;
    }
    sBankCount = bankCount;

    for (b = 0; b < sBankCount; b++) {
        u32 off = RD32(gSoundDataRaw + 4 + b * 8);
        u32 len = RD32(gSoundDataRaw + 4 + b * 8 + 4);
        sBankBlobOff[b] = off;
        if (blob_index(off) < 0) {
            if (sBlobCount == SC_MAX_BLOBS) {
                gNdsScacheFail = 3;
                return -1;
            }
            sBlobOff[sBlobCount] = off;
            sBlobLen[sBlobCount] = len;
            sBlobCount++;
        }
    }

    for (b = 0; b < sBankCount; b++) {
        u32 ctlOff = RD32(gSoundDataADSR + 4 + b * 8);
        const u8 *chunk = gSoundDataADSR + ctlOff;
        u32 numInst = RD32(chunk + 0);
        u32 numDrums = RD32(chunk + 4);
        const u8 *body = chunk + 0x10;
        u32 blobOff = sBankBlobOff[b];
        u32 drumsOff = RD32(body);
        u32 d, ins, s;

        sBankRefStart[b] = refCount;

#define ADD_BANK_REF(sampleStructOff)                                         \
        {                                                                     \
            u32 addr_ = RD32(body + (sampleStructOff) + 4);                   \
            s32 r_;                                                           \
            for (r_ = sBankRefStart[b]; r_ < refCount; r_++) {                \
                if (sInitRefs[r_].off == addr_) {                             \
                    break;                                                    \
                }                                                             \
            }                                                                 \
            if (r_ == refCount) {                                             \
                if (refCount >= SC_MAX_REFS) {                                \
                    gNdsScacheFail = 4;                                       \
                    return -1;                                                \
                }                                                             \
                sInitRefs[refCount].blob = blobOff;                           \
                sInitRefs[refCount].off = addr_;                              \
                refCount++;                                                   \
            }                                                                 \
        }

        if (drumsOff != 0) {
            for (d = 0; d < numDrums; d++) {
                u32 dOff = RD32(body + drumsOff + d * 4);
                if (dOff != 0) {
                    u32 soundSample = RD32(body + dOff + 4);
                    if (soundSample != 0) {
                        ADD_BANK_REF(soundSample);
                    }
                }
            }
        }
        for (ins = 0; ins < numInst; ins++) {
            u32 iOff = RD32(body + 4 + ins * 4);
            if (iOff == 0) {
                continue;
            }
            for (s = 0; s < 3; s++) {
                u32 soundSample = RD32(body + iOff + 8 + s * 8);
                if (soundSample != 0) {
                    ADD_BANK_REF(soundSample);
                }
            }
        }
#undef ADD_BANK_REF
        sBankRefCount[b] = refCount - sBankRefStart[b];
    }

    sEntryCount = 0;
    for (k = 0; k < sBlobCount; k++) {
        u16 n = 0;
        s32 r;
        u16 a;
        for (r = 0; r < refCount; r++) {
            u16 j;
            if (sInitRefs[r].blob != sBlobOff[k]) {
                continue;
            }
            for (j = 0; j < n; j++) {
                if (sInitOffsets[j] == sInitRefs[r].off) {
                    break;
                }
            }
            if (j == n) {
                if (sEntryCount + n >= SC_MAX_ENTRIES) {
                    gNdsScacheFail = 6;
                    return -1;
                }
                sInitOffsets[n++] = sInitRefs[r].off;
            }
        }
        for (a = 1; a < n; a++) {
            u32 v = sInitOffsets[a];
            s32 p = a - 1;
            while (p >= 0 && sInitOffsets[p] > v) {
                sInitOffsets[p + 1] = sInitOffsets[p];
                p--;
            }
            sInitOffsets[p + 1] = v;
        }
        sBlobEntryStart[k] = sEntryCount;
        sBlobEntryCount[k] = n;
        for (a = 0; a < n; a++) {
            struct ScEntry *e = &sEntries[sEntryCount + a];
            u32 next = (a + 1 < n) ? sInitOffsets[a + 1] : sBlobLen[k];
            if (sInitOffsets[a] >= next || next > sBlobLen[k]) {
                gNdsScacheFail = 5;
                return -1;
            }
            e->fileOff = sBlobOff[k] + sInitOffsets[a];
            e->size = next - sInitOffsets[a];
            e->addr = NULL;
            e->state = SC_ABSENT;
            e->flags = 0;
            e->lastUsed = 0;
        }
        sEntryCount += n;
    }

    for (b = 0; b < sBankCount; b++) {
        u16 i;
        s32 blobIdx = blob_index(sBankBlobOff[b]);
        for (i = 0; i < sBankRefCount[b]; i++) {
            s32 r = sBankRefStart[b] + i;
            u32 idx = entry_lookup(blobIdx, sInitRefs[r].blob + sInitRefs[r].off);
            sBankRefs[r] = (idx == SC_ENTRY_NONE) ? SC_ENTRY_NONE : (u16) idx;
        }
    }

    sTblFile = fopen("nitro:/sound/sound_data.tbl", "rb");
    if (sTblFile == NULL) {
        gNdsScacheFail = 7;
        return -1;
    }
    sTblPosition = 0;
    {
        u32 size;
        for (size = SC_ARENA_MAX; size >= SC_ARENA_MIN; size -= SC_ARENA_STEP) {
            u8 *mem = (u8 *) malloc(size);
            if (mem != NULL) {
                void *probe = malloc(SC_HEAP_RESERVE);
                if (probe == NULL) {
                    free(mem);
                    continue;
                }
                free(probe);
                sArena = mem;
                sArenaSize = size & ~(u32) 15;
                break;
            }
        }
    }
    if (sArena == NULL) {
        fclose(sTblFile);
        sTblFile = NULL;
        gNdsScacheFail = 8;
        return -1;
    }

    {
        u16 n;
        u16 total = (u16) (sizeof(sFreeNodes) / sizeof(sFreeNodes[0]));
        for (n = 0; n < total; n++) {
            sFreeNodes[n].next = (u16) ((n + 1 < total) ? n + 1 : SC_NODE_NONE);
        }
        sFreeNodePool = 0;
        sFreeHead = SC_NODE_NONE;
        sFreeBytes = 0;
        free_block((u8 *) SC_ALIGN((u32) sArena), sArenaSize - 16);
        sTotalCache = sArenaSize - 16;

        free_block((u8 *) NDS_WRAM_SAMPLE_BASE, NDS_WRAM_SAMPLE_SIZE);
        sTotalCache += NDS_WRAM_SAMPLE_SIZE;
    }

    nds_debug_printf("scache: %d entries, cache %luK\n", (int) sEntryCount,
                     (unsigned long) (sTotalCache >> 10));
    gNdsScacheEntries = sEntryCount;
    gNdsScacheArena = sTotalCache;
    gNdsScacheRefs = (u32) refCount;
    sReady = 1;
    return 0;
}

void nds_scache_add_region(u32 base, u32 size) {
    if (!sReady || size < 32) {
        return;
    }
    free_block((u8 *) base, size & ~(u32) 15);
    sTotalCache += size & ~(u32) 15;
    gNdsScacheArena = sTotalCache;
}
