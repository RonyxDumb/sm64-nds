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
        variants = [[], ['-ffast-math']] if name.startswith('Painting ') else [[]]
        for flags in variants:
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra',
                            '-Wno-pointer-to-int-cast', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            *flags, str(path), '-lm', '-o', str(path.with_suffix(''))], check=True)
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

run('Fixed 16.16 float conversion bit equivalence', r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
typedef int32_t s32;typedef uint32_t u32;typedef float f32;
#define NDS_ARM_CODE
''' + function('src/engine/math_util.c', 'nds_fixed16_to_float') + r'''
static void check(u32 word){
 s32 n;memcpy(&n,&word,4);
 volatile float integer=(float)n;
 float expected=integer*(1.0f/65536.0f),actual=nds_fixed16_to_float(n);
 u32 a,b;memcpy(&a,&actual,4);memcpy(&b,&expected,4);assert(a==b);
}
int main(void){
 check(0);check(INT_MIN);check(INT_MAX);check(0xffffffffu);
 for(unsigned bit=0;bit<32;bit++)for(unsigned d=0;d<4096;d++){
  u32 n=(1u<<bit)+d;check(n);check(0u-n);
  n=(1u<<bit)-d;check(n);check(0u-n);
 }
 u32 state=0x829ab331u;
 for(unsigned i=0;i<2000000;i++){
  state^=state<<13;state^=state>>17;state^=state<<5;check(state);
 }
 return 0;
}
''')

surface = (ROOT / 'src/engine/surface_load.c').read_text()
start = surface.index('static struct Surface *read_object_surface_data(')
end = surface.index('\n#else', start)
run('Dynamic surface cache geometry and invalidation', r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
typedef int32_t s32;typedef uint32_t u32;typedef int16_t s16;
typedef uint16_t u16;typedef uint8_t u8;typedef int8_t s8;
typedef float f32;typedef s16 TerrainData;
struct Object {int unused;};
struct Surface {
 TerrainData type,force;s8 flags,room;TerrainData lowerY,upperY;
 TerrainData vertex1[3],vertex2[3],vertex3[3];
 struct {f32 x,y,z;} normal;f32 originOffset;struct Object*object;
};
#define TARGET_NDS 1
#define ARM9 1
static u32 sNdsSurfaceGeometryValid[72];
#define SURF_WARN_POOL 1
#define SURF_WARN_BAD_INDEX 4
#define DBG(...)
#define NDS_COUNT_SURFACE_HIT 0
#define NDS_COUNT_SURFACE_MISS 1
static struct Surface pool[2],*sSurfacePool=pool;
static int gSurfacesAllocated,sSurfacePoolSize=2,sTerrainNumVertices=32;
static u8 sSurfaceLoadWarnFlags;
static unsigned counters[2];
static void nds_perf_count(int n,unsigned v){counters[n]+=v;}
''' + function('src/engine/surface_load.c', 'alloc_surface')
    + function('src/engine/surface_load.c', 'read_surface_data')
    + '\n' + surface[start:end] + r'''
static struct Surface plain(TerrainData*v,TerrainData**i,int*valid){
 struct Surface saved[2];u32 validBits[72];
 memcpy(saved,pool,sizeof(pool));memcpy(validBits,sNdsSurfaceGeometryValid,sizeof(validBits));
 memset(pool,0,sizeof(pool));gSurfacesAllocated=0;
 struct Surface*p=read_surface_data(v,i);*valid=(p!=0);
 struct Surface result=p?*p:(struct Surface){0};
 memcpy(pool,saved,sizeof(pool));memcpy(sNdsSurfaceGeometryValid,validBits,sizeof(validBits));
 return result;
}
static void check(TerrainData*v,TerrainData**i){
 int valid;struct Surface expected=plain(v,i,&valid);
 gSurfacesAllocated=0;
 struct Surface*p=read_object_surface_data(v,i);assert((p!=0)==valid);
 if(p){assert(!memcmp(p,&expected,sizeof(expected)));
  /* A caller's per-object metadata must never leak into the cache. */
  p->object=(struct Object*)(uintptr_t)0x1234;p->type=100;p->flags=15;p->room=5;p->force=12;
 }
}
int main(void){
 TerrainData vertices[96],indices[3]={0,1,2};TerrainData*i=indices;
 for(int trial=0;trial<10000;trial++){
  for(int k=0;k<96;k++)vertices[k]=(rand()%4001)-2000;
  check(vertices,&i);unsigned before=counters[NDS_COUNT_SURFACE_HIT];
  check(vertices,&i);assert(counters[NDS_COUNT_SURFACE_HIT]==before+1);
  for(int k=0;k<9;k++){vertices[k]++;check(vertices,&i);check(vertices,&i);}
  indices[0]=3;indices[1]=4;indices[2]=5;check(vertices,&i);check(vertices,&i);
  indices[0]=0;indices[1]=1;indices[2]=2;
 }
 memset(vertices,0,sizeof(vertices));check(vertices,&i);check(vertices,&i);
 indices[0]=-1;check(vertices,&i);indices[0]=32;check(vertices,&i);indices[0]=0;
 vertices[0]=0;vertices[1]=0;vertices[2]=0;vertices[3]=100;vertices[4]=0;vertices[5]=0;vertices[6]=0;vertices[7]=100;vertices[8]=0;
 check(vertices,&i);gSurfacesAllocated=sSurfacePoolSize;assert(!read_object_surface_data(vertices,&i));
 assert(counters[NDS_COUNT_SURFACE_HIT]&&counters[NDS_COUNT_SURFACE_MISS]);
 return 0;
}
''')

run('Deferred matrix float export and scaling', r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
typedef int32_t s32;typedef uint32_t u32;typedef int16_t s16;typedef uint16_t u16;
typedef int64_t s64;typedef float f32;typedef f32 Mat4[4][4];typedef f32 Vec3f[3];
typedef struct {s32 m[4][4];} Mtx;
#define NDS_ARM_CODE
#define NDS_ITCM_CODE
#define FIXED_MUL16(x,y) ((s32)((((s64)(x)*(y))+0x8000)>>16))
''' + '\n'.join(function('src/engine/math_util.c', n) for n in
    ['nds_fixed16_to_float', 'mtx_get_fixed', 'mtxf_mul_fixed_raw', 'mtxf_scale_fixed']) + r'''
int main(void){
 for(int trial=0;trial<20000;trial++){
  s32 input[4][4],b[4][4],actual[4][4],expected[4][4];Mat4 floats,mirror;Mtx packed;
  for(int i=0;i<4;i++)for(int j=0;j<4;j++){
   input[i][j]=(rand()%262145)-131072;b[i][j]=(rand()%262145)-131072;
  }
  for(int i=0;i<4;i++){
   input[i][3]=b[i][3]=i==3?65536:0;
   for(int j=0;j<2;j++){
    u32 first=(u32)b[i][j*2],second=(u32)b[i][j*2+1];
    packed.m[i/2][(i%2)*2+j]=(s32)((first&0xffff0000u)|(second>>16));
    packed.m[2+i/2][(i%2)*2+j]=(s32)((first<<16)|(second&65535));
   }
  }
  mtxf_mul_fixed_raw(expected,mirror,input,&packed);
  memset(floats,0x35,sizeof(floats));mtxf_mul_fixed_raw(actual,NULL,input,&packed);
  assert(!memcmp(actual,expected,sizeof(actual)));
  Vec3f scale={(trial%7-3)/2.0f,(trial%5-2)/2.0f,(trial%11-5)/2.0f};
  mtxf_scale_fixed(actual,floats,scale);
  for(int i=0;i<4;i++)for(int j=0;j<4;j++){
   s32 want=expected[i][j];
   if(i<3)want=(s32)((((s64)want*(s32)(scale[i]*65536.0f))+0x8000)>>16);
   volatile f32 converted=(f32)want;f32 wantFloat=converted*(1.0f/65536.0f);
   assert(actual[i][j]==want);assert(!memcmp(&floats[i][j],&wantFloat,4));
  }
 }
 return 0;
}
''')

painting = (ROOT / 'src/game/paintings.c').read_text()
start = painting.index('#define NDS_PAINTING_CACHE_POINTS')
end = painting.index('/* Painting coordinates', start)
reference = (ROOT / 'tests/fixtures/nds_painting_ripple_before_cache.c').read_text().replace('calculate_ripple_at_point(', 'reference_ripple(')
point = function('src/game/paintings.c', 'painting_point_distance').replace('painting_point_distance(', 'actual_point_distance(')
run('Painting wave cache equivalence and invalidation', r'''
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <assert.h>
typedef int16_t s16;typedef uint32_t u32;typedef float f32;
#define PAINTING_SIZE 614.0
#define M_PI 3.14159265358979323846
struct Painting {f32 currRippleMag,currRippleRate,dispersionFactor,rippleTimer,rippleX,rippleY,size;};
static unsigned calculations;
''' + function('src/game/geo_misc.c', 'round_float') + reference + point + r'''
static f32 painting_point_distance(struct Painting*p,f32 x,f32 y){calculations++;return actual_point_distance(p,x,y);}
''' + function('src/game/paintings.c', 'painting_ripple_from_distance') + '\n' + painting[start:end] + r'''
int main(void){
 struct Painting p[2]={{80,0.08f,20,0,307,307,614},{60,0.1f,15,0,180,500,500}};
 for(int frame=0;frame<1000;frame++){
  struct Painting*w=&p[(frame/173)%2];w->rippleTimer=frame;w->currRippleMag=80-frame/20.0f;
  if(frame%71==0)w->rippleX+=1;
  if(frame%97==0)w->rippleY-=1;
  w->currRippleRate=0.01f+(frame%11)/100.0f;
  if(frame%113==0)w->size+=1;
  if(frame%127==0)w->dispersionFactor+=0.25f;
  painting_prepare_distances(w);
  for(int i=0;i<160;i++){
   s16 x=(i*37)%615,y=(i*91)%615;
   s16 want=reference_ripple(w,x,y);
   f32 distance=painting_cached_distance(w,i,x,y);
   assert(painting_ripple_from_distance(w,distance)==want);
   unsigned before=calculations;
   assert(painting_cached_distance(w,i,x,y)==distance&&calculations==before);
   assert(painting_cached_distance(w,i,x+1,y)==actual_point_distance(w,x+1,y));
   (void)painting_cached_distance(w,i,x,y);
  }
  assert(painting_cached_distance(w,160,1,2)==actual_point_distance(w,1,2));
 }
 return 0;
}
''')

run('Painting integer normals exact bounded arithmetic', r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
typedef int16_t s16;typedef int32_t s32;typedef uint32_t u32;typedef float Vec3f[3];
''' + function('src/game/paintings.c', 'painting_integer_normal') + r'''
int main(void){
 for(int n=0;n<200000;n++){
  s16 a[3],b[3],c[3];float expected[3],actual[3];
  for(int j=0;j<3;j++){a[j]=(rand()%2049)-1024;b[j]=(rand()%2049)-1024;c[j]=(rand()%2049)-1024;}
  float x0=a[0],y0=a[1],z0=a[2],x1=b[0],y1=b[1],z1=b[2],x2=c[0],y2=c[1],z2=c[2];
  expected[0]=(y1-y0)*(z2-z1)-(z1-z0)*(y2-y1);
  expected[1]=(z1-z0)*(x2-x1)-(x1-x0)*(z2-z1);
  expected[2]=(x1-x0)*(y2-y1)-(y1-y0)*(x2-x1);
  assert(painting_integer_normal(actual,a,b,c));
  for(int j=0;j<3;j++)assert(expected[j]==actual[j]);
 }
 s16 a[3]={1025,0,0},b[3]={0,1,0},c[3]={0,0,1};float out[3]={123,123,123};
 assert(!painting_integer_normal(out,a,b,c)&&out[0]==123);
 a[0]=-1025;assert(!painting_integer_normal(out,a,b,c));
 return 0;
}
''')
