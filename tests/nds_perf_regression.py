"""Host regression checks against production helpers; no ROM or SDK required."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def function(path, name):
    source = (ROOT / path).read_text()
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def run(name, source):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'test.c'
        path.write_text(source)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Wno-pointer-to-int-cast', '-fsanitize=undefined',
                        str(path), '-o', str(path.with_suffix(''))], check=True)
        subprocess.run([str(path.with_suffix(''))], check=True)
    print(name + ': PASS')

run('ARM7 frequency cache', r'''
#include <assert.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef float f32;
static f32 sLastFrequency[16]; static u16 sLastTimer[16];
static unsigned divisions;
static u16 sound_freq(unsigned f) { divisions++; return (u16)(-16756991 / (int)f); }
#define SOUND_FREQ(f) sound_freq(f)
''' + function('src/nds/arm7/nds_audio.c', 'calculate_freq') + r'''
int main(void) {
 for (int ch=0; ch<16; ch++) for (int n=1; n<2000; n++) {
  float f=n/333.0f; unsigned hz=f*32000; if(hz>65535)hz=65535;
  u16 expected=(u16)(-16756991/(int)hz);
  assert(calculate_freq(ch,f)==expected);
  unsigned before=divisions;
  assert(calculate_freq(ch,f)==expected); assert(divisions==before);
 }
 assert(calculate_freq(0,0)==(u16)-16756991);
 return 0;
}
''')

run('Deferred sample transfer ownership', r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define NDS_PERF_HZ 523656u
#define NDS_COPY_FIFO_CHANNEL 1
#define NDS_COUNT_COPY_PENDING 0
#define NDS_COUNT_COPY_STALL 1
#define NDS_COUNT_AUDIO_BYTES 2
#define SC_ENTRY_NONE 65535
#define SC_FAILED 3
#define SC_RESIDENT 2
#define SC_ALIGN(n) (n)
#define SC_IS_REMOTE(a) 1
#define NDS_COPY_MAX_CHUNK 8192
#define RD32(p) 123u
struct NdsCopyCmd {u32 dst,src,len;};
struct ScEntry {u8 *addr;u32 size,fileOff,halfOff,lastUsed;int state;};
static struct ScEntry sEntries[1];
static unsigned sEntryCount=1; static u16 sLoadEntry; static u32 sLoadDone,gNdsAudioTick,gNdsCntLoads;
static u8 sStage[8192],sRemotePending;
static u32 sRemoteDest,sRemoteLen,sRemoteStarted,clockTicks;
static int ackReady,sendOK=1,readOK=1,reads,frees,submissions;
static u32 ack;
static unsigned nds_perf_now(void){return clockTicks;}
static void nds_perf_count(int c,unsigned n){(void)c;(void)n;}
static int fifoCheckValue32(int c){(void)c;return ackReady;}
static u32 fifoGetValue32(int c){(void)c;ackReady=0;return ack;}
static void DC_FlushRange(void*p,u32 n){(void)p;(void)n;}
static int fifoSendDatamsg(int c,int n,u8*p){(void)c;(void)n;(void)p;submissions++;return sendOK;}
static void free_block(u8*p,u32 n){(void)p;(void)n;frees++;}
static int tbl_read_at(u32 off,void*p,u32 n){reads++;memset(p,(int)(off/8192+1),n);return readOK;}
''' + '\n'.join(function('src/nds/nds_sample_cache.c', n) for n in
                 ['remote_copy_begin', 'remote_copy_poll', 'load_failed', 'load_pump']) + r'''
int main(void){
 sEntries[0]=(struct ScEntry){.addr=(u8*)0x03000000,.size=20000,.state=1};
 assert(load_pump(16384)==8192); assert(sRemotePending && reads==1);
 assert(sLoadDone==0 && sStage[0]==1);
 for(int i=0;i<100;i++){clockTicks+=1000;assert(load_pump(16384)==0);}
 assert(reads==1 && frees==0 && submissions==1 && sStage[0]==1);
 ack=sRemoteDest;ackReady=1;
 assert(load_pump(16384)==8192);assert(sLoadDone==8192 && sStage[0]==2);
 ack=sRemoteDest;ackReady=1;
 assert(load_pump(16384)==3616);assert(sLoadDone==16384);
 ack=sRemoteDest;ackReady=1;
 assert(load_pump(16384)==0);assert(sEntries[0].state==SC_RESIDENT);
 assert(sLoadEntry==SC_ENTRY_NONE && gNdsCntLoads==1 && frees==0);
 sLoadEntry=0;sLoadDone=0;sEntries[0].state=1;readOK=0;
 assert(load_pump(100)==100);assert(frees==1 && sEntries[0].state==SC_FAILED);
 return 0;
}
''')

run('Projected batch coordinates and matrix restoration', r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t s32;
typedef struct { short ob[3],tc[2]; unsigned char cn[4]; } Vtx_t;
typedef Vtx_t Vtx;
typedef struct {s32 m[16];} m4x4;
#define DTCM_BSS
#define ITCM_CODE
#define GL_MODELVIEW 0
#define GL_PROJECTION 1
#define NDS_COUNT_POSTEST 0
static Vtx vertex_buffer[16];static s32 sProjected[16][4];
static int use_color,use_texture,texture_scale_s=32768,texture_scale_t=32768;
static int mode,depth[2],tests,emitted,unique;
static m4x4 matrices[2],saved[2],current;
static s32 result[4];static const Vtx_t **refs;static int override,zvalue;
static void nds_perf_count(int c,unsigned n){(void)c;unique=n;}
static void PosTest(int x,int y,int z){
 tests++;assert(depth[0]==0&&depth[1]==0);
 result[0]=x*13+y*7;result[1]=y*11-z*3;result[2]=z*17+x;result[3]=4096+x-y;
}
#define PosTestXresult() result[0]
#define PosTestYresult() result[1]
#define PosTestZresult() result[2]
#define PosTestWresult() result[3]
static void glMatrixMode(int m){mode=m;}
static void glPushMatrix(void){assert(depth[mode]++==0);saved[mode]=matrices[mode];}
static void glPopMatrix(int n){assert(n==1&&--depth[mode]==0);matrices[mode]=saved[mode];}
static void glLoadIdentity(void){memset(&matrices[mode],0,sizeof(m4x4));}
static void glLoadMatrix4x4(const m4x4*p){current=matrices[mode]=*p;}
static void gl_color_tinted(int r,int g,int b){(void)r;(void)g;(void)b;}
static void glTexCoord2t16(int s,int t){(void)s;(void)t;}
static void glVertex3v16(int x,int y,int z){
 const Vtx_t*v=refs[emitted++];
 assert(x==4096&&y==4096&&z==4096);
 assert(current.m[0]==v->ob[0]*13+v->ob[1]*7);
 assert(current.m[5]==v->ob[1]*11-v->ob[2]*3);
 assert(current.m[10]==(override?zvalue:v->ob[2]*17+v->ob[0]-48));
 assert(current.m[15]==4096+v->ob[0]-v->ob[1]);
}
''' + function('src/nds/nds_renderer.c', 'draw_projected') + r'''
int main(void){
 static const Vtx_t *v[96]; refs=v;
 for(int trial=0;trial<2000;trial++){
  for(int i=0;i<16;i++)for(int j=0;j<3;j++)vertex_buffer[i].ob[j]=(rand()%60001)-30000;
  int count=3*(1+rand()%32);unsigned mask=0;
  for(int i=0;i<count;i++){unsigned slot=rand()%16;v[i]=&vertex_buffer[slot];mask|=1u<<slot;}
  override=trial%2;use_color=trial%3;use_texture=trial%5;zvalue=trial;
  tests=emitted=0;memset(matrices,0x35,sizeof(matrices));m4x4 before[2];memcpy(before,matrices,sizeof(before));
  draw_projected(v,count,8,override,zvalue);
  assert(emitted==count&&tests==__builtin_popcount(mask)&&unique==tests);
  assert(!memcmp(before,matrices,sizeof(before))&&mode==GL_MODELVIEW);
 }
 return 0;
}
''')

# Compare actual register output with the pre-snapshot ARM7 backend. The
# fixture is an unchanged reference, not a second implementation of the packer.
import re
old_audio = (ROOT / 'tests/fixtures/nds_audio_before_snapshot.c').read_text()
new_audio = (ROOT / 'src/nds/arm7/nds_audio.c').read_text()
def backend(source, prefix):
    source = re.sub(r'^#include.*$', '', source, flags=re.M)
    for symbol in ('high_freqs', 'chan_seq', 'chan_inited', 'sLastFrequency',
                   'sLastTimer', 'calculate_freq', 'calculate_vol_pan', 'play_notes'):
        source = re.sub(r'\b' + symbol + r'\b', prefix + symbol, source)
    return source
run('Compact audio snapshot hardware output', r'''
#include <stdint.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef float f32;
#define BUS_CLOCK 33513982
#define BIT(n) (1u<<(n))
#define SOUND_VOL(n) (n)
#define SOUND_PAN(n) ((n)<<16)
#define SCHANNEL_ENABLE (1u<<31)
#define SOUND_REPEAT (1u<<27)
#define SOUND_ONE_SHOT (2u<<27)
#define SOUND_FORMAT_ADPCM (2u<<29)
struct Hardware {u32 cr,source,repeat,length;u16 timer;};
static struct Hardware reference[16],actual[16],*hw;
#define SCHANNEL_CR(i) hw[i].cr
#define SCHANNEL_SOURCE(i) hw[i].source
#define SCHANNEL_REPEAT_POINT(i) hw[i].repeat
#define SCHANNEL_LENGTH(i) hw[i].length
#define SCHANNEL_TIMER(i) hw[i].timer
struct Note {
 unsigned char unused[256];
 u32 ndsSourceFull,ndsSourceHalf;f32 frequency;
 u16 targetVolLeft,targetVolRight,ndsRepeatFull,ndsLengthFull,ndsRepeatHalf,ndsLengthHalf;
 u8 enabled:1;u8 spare:7;u8 ndsMuted,ndsLoop,ndsSeq;
};
''' + (ROOT / 'src/nds/nds_audio_shared.h').read_text()
    + function('src/nds/main.c', 'nds_pack_audio_note')
    + backend(old_audio, 'old_') + backend(new_audio, 'new_') + r'''
int main(void) {
 static struct Note notes[20];static struct NdsAudioNote snapshot[16];
 assert(sizeof(snapshot)==512);
 for(int frame=0;frame<20000;frame++){
  for(int i=0;i<16;i++){
   struct Note*n=&notes[i];
   if(rand()%5==0){
    n->enabled=rand()%2;n->ndsSeq++;n->ndsMuted=rand()%2;
    n->ndsSourceFull=(rand()%4)?0x2000000+((rand()%32768)*4):0;
    n->ndsSourceHalf=(rand()%2)?n->ndsSourceFull+0x10000:0;
    n->ndsRepeatFull=rand()%65536;n->ndsLengthFull=rand()%65536;
    n->ndsRepeatHalf=rand()%65536;n->ndsLengthHalf=rand()%65536;
    n->ndsLoop=rand()%2;
   }
   n->frequency=(rand()%160001)/32000.0f;
   n->targetVolLeft=rand()%65536;n->targetVolRight=rand()%65536;
   nds_pack_audio_note(&snapshot[i],n);
   /* Simulate a channel finishing a one-shot between updates. */
   if(rand()%20==0){reference[i].cr&=~SCHANNEL_ENABLE;actual[i].cr&=~SCHANNEL_ENABLE;}
  }
  hw=reference;old_play_notes(notes);
  hw=actual;new_play_notes(snapshot);
  assert(!memcmp(reference,actual,sizeof(actual)));
 }
 return 0;
}
''')

perf = (ROOT / 'src/nds/nds_perf.c').read_text()
perf = re.sub(r'^#include.*$', '', perf, flags=re.M)
run('Per-area phase timing and CSV export', r'''
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>
static unsigned TIMER1_CR,TIMER2_CR,TIMER1_DATA,TIMER2_DATA;
#define TIMER_ENABLE 1
#define TIMER_CASCADE 2
#define TIMER_DIV_64 4
static int gCurrLevelNum=24,gCurrAreaIndex=1,exportFD=-1;
static int enterCriticalSection(void){return 0;}
static void leaveCriticalSection(int v){(void)v;}
static FILE *test_fopen(const char *path,const char *mode){
 (void)path;(void)mode;FILE*f=tmpfile();assert(f);exportFD=dup(fileno(f));assert(exportFD>=0);return f;
}
#define fopen test_fopen
''' + (ROOT / 'src/nds/nds_perf.h').read_text() + perf + r'''
static void clock_set(unsigned ticks){TIMER1_DATA=ticks&65535;TIMER2_DATA=ticks>>16;}
int main(void){
 nds_perf_init();
 clock_set(100);nds_perf_record(NDS_PERF_LEVEL_IO,0);unsigned start=nds_perf_begin_frame();
 clock_set(120);nds_perf_record(NDS_PERF_ACTORS,100);
 clock_set(130);nds_perf_record(NDS_PERF_AUDIO,120);
 clock_set(200);nds_perf_finish_frame(start);
 assert(sAreas[0].frames==1&&sAreas[0].total==100);
 assert(sAreas[0].phases[NDS_PERF_LEVEL_IO]==0);
 assert(sAreas[0].phases[NDS_PERF_ACTORS]==20&&sAreas[0].phases[NDS_PERF_AUDIO]==10);
 clock_set(300);nds_perf_finish_frame(200);
 assert(sAreas[0].frames==2&&sAreas[0].phases[NDS_PERF_ACTORS]==20);
 gCurrAreaIndex=2;
 clock_set(0xfffffff0u);start=nds_perf_now();clock_set(16);
 nds_perf_record(NDS_PERF_SCENE,start);nds_perf_finish_frame(start);
 assert(sAreas[1].total==32&&sAreas[1].phases[NDS_PERF_SCENE]==32);
 sPaused=1;clock_set(99);nds_perf_record(NDS_PERF_AUDIO,16);sPaused=0;
 assert(sPhases[NDS_PERF_AUDIO].total==10);
 nds_perf_request_dump();clock_set(116);nds_perf_finish_frame(16);
 assert(!sPaused&&exportFD>=0);
 FILE*f=fdopen(exportFD,"r");assert(f);rewind(f);char buf[32768];size_t n=fread(buf,1,sizeof(buf)-1,f);buf[n]=0;fclose(f);
 assert(strstr(buf,"# Profile version,2\n"));
 assert(strstr(buf,"level,area,phase,total_ticks\n"));
 assert(strstr(buf,"24,1,actors_inclusive,20\n"));
 assert(strstr(buf,"24,2,scene_build_inclusive,32\n"));
 return 0;
}
''')
