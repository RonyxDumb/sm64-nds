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
