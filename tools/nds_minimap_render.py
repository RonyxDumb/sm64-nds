#!/usr/bin/env python3
"""Pre-render top-down minimaps of every area of a level for the NDS port.

Each area's level geometry (the display lists of its geo layout) is drawn
from above with its textures and vertex lighting, in software, into a
128x128 image. The image is dithered to the bottom screen's 6x6x6 colour
cube and written in 8bpp tile order, ready to be copied to sub BG VRAM:

    "SMM1", s32 originX, s32 originZ, s32 unitsPerPixel, 256 tiles * 64 bytes

The scale fits the area's collision floors, which is where Mario can go.
Only Python's standard library is needed (PNG textures are decoded here).
Usage: nds_minimap_render.py LEVEL_DIR OUT_DIR [--png]
"""
import glob
import os
import re
import struct
import sys
import zlib

N = 128           # output size in pixels
SS = 2            # supersampling factor
R = N * SS
BG_INDEX = 8 + 1                    # navy
FRAME_INDEX = 8 + 3 * 36 + 3 * 6 + 4

LAYERS = ['LAYER_FORCE', 'LAYER_OPAQUE', 'LAYER_OPAQUE_DECAL', 'LAYER_OPAQUE_INTER',
          'LAYER_ALPHA', 'LAYER_TRANSPARENT', 'LAYER_TRANSPARENT_DECAL',
          'LAYER_TRANSPARENT_INTER']


# --- PNG (8-bit grey+alpha or RGBA, as written by the asset extractor) ---

def read_png(path):
    data = open(path, 'rb').read()
    pos, idat = 8, b''
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', chunk[:10])
        elif kind == b'IDAT':
            idat += chunk
        pos += 12 + length
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    if depth != 8:
        raise ValueError('unsupported PNG depth')
    raw = zlib.decompress(idat)
    stride = w * channels
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    pixels = []
    for line in rows:
        for x in range(w):
            p = line[x * channels:(x + 1) * channels]
            if channels == 1: pixels.append((p[0], p[0], p[0], 255))
            elif channels == 2: pixels.append((p[0], p[0], p[0], p[1]))
            elif channels == 3: pixels.append((p[0], p[1], p[2], 255))
            else: pixels.append(tuple(p))
    return w, h, pixels


# --- Source indexing ---

def strip_comments(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def num(s):
    s = s.strip()
    return int(s, 16) if s.lower().startswith(('0x', '-0x')) else int(s)


class Index:
    def __init__(self, files):
        self.vtx, self.gfx, self.lights, self.tex, self.geo, self.col = {}, {}, {}, {}, {}, {}
        vtx_re = re.compile(r'\{\{\{\s*(-?\w+),\s*(-?\w+),\s*(-?\w+)\s*\},\s*-?\w+,\s*\{\s*(-?\w+),\s*(-?\w+)\s*\},'
                            r'\s*\{\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\s*\}\s*\}\s*\}')
        for path in files:
            text = strip_comments(open(path, errors='replace').read())
            for m in re.finditer(r'Vtx\s+(\w+)\[\]\s*=\s*\{(.*?)\};', text, re.S):
                self.vtx[m.group(1)] = [tuple(num(g) for g in v.groups()) for v in vtx_re.finditer(m.group(2))]
            for m in re.finditer(r'Gfx\s+(\w+)\[\]\s*=\s*\{(.*?)\};', text, re.S):
                cmds = []
                for cm in re.finditer(r'(gs\w+)\((.*?)\)\s*,\s*$', m.group(2), re.M):
                    cmds.append((cm.group(1), split_args(cm.group(2))))
                self.gfx[m.group(1)] = cmds
            for m in re.finditer(r'Lights1\s+(\w+)\s*=\s*gdSPDefLights1\(([^)]*)\)', text):
                self.lights[m.group(1)] = [num(a) for a in split_args(m.group(2))]
            for m in re.finditer(r'Texture\s+(\w+)\[\]\s*=\s*\{\s*#include\s*"([^"]+)\.inc\.c"', text):
                self.tex[m.group(1)] = m.group(2) + '.png'
            for m in re.finditer(r'GeoLayout\s+(\w+)\[\]\s*=\s*\{(.*?)\};', text, re.S):
                self.geo[m.group(1)] = m.group(2)
            for m in re.finditer(r'Collision\s+(\w+)\[\]\s*=\s*\{(.*?)\};', text, re.S):
                self.col[m.group(1)] = m.group(2)


# --- Collision bounds ---

def floor_bounds(body):
    verts = [tuple(int(v) for v in m) for m in
             re.findall(r'COL_VERTEX\(\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\)', body)]
    lo = [1 << 30, 1 << 30]
    hi = [-(1 << 30), -(1 << 30)]
    skip = False
    for m in re.finditer(r'COL_TRI_INIT\(\s*(\w+)|COL_TRI(?:_SPECIAL)?\(\s*(\d+),\s*(\d+),\s*(\d+)', body):
        if m.group(1):
            skip = m.group(1) in ('SURFACE_DEATH_PLANE', 'SURFACE_INTANGIBLE')
            continue
        if skip:
            continue
        a, b, c = (verts[int(m.group(i))] for i in (2, 3, 4))
        ny = (b[2] - a[2]) * (c[0] - b[0]) - (b[0] - a[0]) * (c[2] - b[2])
        nx = (b[1] - a[1]) * (c[2] - b[2]) - (b[2] - a[2]) * (c[1] - b[1])
        nz = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])
        if ny <= 0.01 * (nx * nx + ny * ny + nz * nz) ** 0.5:
            continue
        for v in (a, b, c):
            lo[0] = min(lo[0], v[0]); hi[0] = max(hi[0], v[0])
            lo[1] = min(lo[1], v[2]); hi[1] = max(hi[1], v[2])
    return None if lo[0] > hi[0] else (lo, hi)


# --- Display lists to triangles ---

class State:
    def __init__(self):
        self.tex = None
        self.tex_on = False
        self.lighting = True
        self.ambient = (0x66, 0x66, 0x66)
        self.diffuse = (0xff, 0xff, 0xff)
        self.combine = 'G_CC_SHADE'
        self.clamp = (False, False)
        self.mirror = (False, False)
        self.verts = [None] * 64


def identity(v):
    return v


def walk_dl(index, name, state, layer, out, depth=0, xform=identity):
    if depth > 16 or name not in index.gfx:
        return
    for cmd, args in index.gfx[name]:
        if cmd == 'gsSPVertex':
            parts = args[0].split('+')
            base = parts[0].strip()
            off = num(parts[1]) if len(parts) > 1 else 0
            src = index.vtx.get(base, [])
            n, v0 = num(args[1]), num(args[2])
            for i in range(n):
                state.verts[v0 + i] = xform(src[off + i]) if off + i < len(src) else None
        elif cmd in ('gsSP1Triangle', 'gsSP2Triangles'):
            tris = [args[0:3]] if cmd == 'gsSP1Triangle' else [args[0:3], args[4:7]]
            for t in tris:
                vs = [state.verts[num(i)] for i in t]
                if None not in vs:
                    out.append((layer, vs, state.tex if state.tex_on else None, state.combine,
                                state.lighting, state.ambient, state.diffuse, state.clamp, state.mirror))
        elif cmd == 'gsDPSetTextureImage':
            state.tex = args[3]
        elif cmd == 'gsDPLoadTextureBlock':
            state.tex = args[0]
            state.clamp = ('CLAMP' in args[6], 'CLAMP' in args[7])
            state.mirror = ('MIRROR' in args[6] and 'NOMIRROR' not in args[6],
                            'MIRROR' in args[7] and 'NOMIRROR' not in args[7])
        elif cmd == 'gsDPSetTile' and len(args) >= 12 and args[4] == 'G_TX_RENDERTILE':
            cms, cmt = args[9], args[6]
            state.clamp = ('CLAMP' in cms, 'CLAMP' in cmt)
            state.mirror = ('MIRROR' in cms and 'NOMIRROR' not in cms,
                            'MIRROR' in cmt and 'NOMIRROR' not in cmt)
        elif cmd == 'gsSPTexture':
            state.tex_on = 'G_ON' in args[-1]
        elif cmd == 'gsSPSetGeometryMode' and 'G_LIGHTING' in args[0]:
            state.lighting = True
        elif cmd == 'gsSPClearGeometryMode' and 'G_LIGHTING' in args[0]:
            state.lighting = False
        elif cmd == 'gsSPLight':
            m = re.match(r'&\s*(\w+)\.(\w)', args[0])
            if m and m.group(1) in index.lights:
                l = index.lights[m.group(1)]
                if m.group(2) == 'a':
                    state.ambient = tuple(l[0:3])
                else:
                    state.diffuse = tuple(l[3:6])
        elif cmd == 'gsDPSetCombineMode':
            state.combine = args[0]
        elif cmd in ('gsSPDisplayList', 'gsSPBranchList'):
            walk_dl(index, args[0], state, layer, out, depth + 1, xform)
            if cmd == 'gsSPBranchList':
                return
        elif cmd == 'gsSPEndDisplayList':
            return


def walk_geo(index, name, out, seen, pos=None, yaw=0):
    if name in seen or name not in index.geo:
        return
    seen.add(name)
    scale = 1.0
    for m in re.finditer(r'(GEO_\w+)\(([^\n]*)\)\s*,', index.geo[name]):
        cmd, args = m.group(1), split_args(m.group(2))
        if cmd == 'GEO_BILLBOARD':
            return                       # camera-facing cards: nothing from above
        if cmd == 'GEO_SCALE' and len(args) == 2:
            scale = num(args[1]) / 65536.0
        elif cmd == 'GEO_DISPLAY_LIST' and len(args) == 2:
            layer = LAYERS.index(args[0]) if args[0] in LAYERS else 1
            walk_dl(index, args[1], State(), layer, out, xform=placement(pos, yaw, scale))
        elif cmd == 'GEO_BRANCH':
            walk_geo(index, args[1], out, seen, pos, yaw)


def placement(pos, yaw, scale):
    """Object models: scale, rotate about Y (degrees, as in level scripts),
    translate. The yaw is kept on the vertex to rotate lighting normals."""
    if pos is None:
        return identity
    import math
    sn, cs = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    def xform(v):
        x, y, z = v[0] * scale, v[1] * scale, v[2] * scale
        return (pos[0] + x * cs + z * sn, pos[1] + y, pos[2] - x * sn + z * cs) + tuple(v[3:9]) + (yaw,)
    return xform


# --- Software rasterizer ---

LIGHT = (0.57735, 0.57735, 0.57735)


def shade(v, lighting, ambient, diffuse):
    if not lighting:
        return (v[5], v[6], v[7], v[8])
    n = [(c - 256 if c > 127 else c) / 127.0 for c in v[5:8]]
    if len(v) > 9 and v[9]:
        import math
        sn, cs = math.sin(math.radians(v[9])), math.cos(math.radians(v[9]))
        n = [n[0] * cs + n[2] * sn, n[1], -n[0] * sn + n[2] * cs]
    d = max(0.0, n[0] * LIGHT[0] + n[1] * LIGHT[1] + n[2] * LIGHT[2])
    return tuple(min(255, ambient[i] + diffuse[i] * d) for i in range(3)) + (v[8],)


class Canvas:
    def __init__(self, origin_x, origin_z, scale):
        self.ox, self.oz, self.scale = origin_x, origin_z, scale
        self.color = [None] * (R * R)
        self.depth = [-1e9] * (R * R)
        self.textures = {}

    def texture(self, index, sym):
        if sym not in self.textures:
            path = index.tex.get(sym)
            try:
                self.textures[sym] = read_png(path) if path and os.path.exists(path) else None
            except (ValueError, KeyError, zlib.error):
                self.textures[sym] = None
        return self.textures[sym]

    def draw(self, index, tri):
        layer, vs, tex_sym, combine, lighting, ambient, diffuse, clamp, mirror = tri
        a, b, c = vs
        ny = (b[2] - a[2]) * (c[0] - b[0]) - (b[0] - a[0]) * (c[2] - b[2])
        if ny <= 0:
            return                       # faces down or is vertical
        k = SS / self.scale
        p = [((v[0] - self.ox) * k, (v[2] - self.oz) * k) for v in vs]
        area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0])
        if area == 0:
            return
        tex = self.texture(index, tex_sym) if tex_sym else None
        if combine in ('G_CC_SHADE', 'G_CC_SHADEFADEA'):
            tex = None
        decal_tex = 'DECAL' in combine
        cols = [shade(v, lighting, ambient, diffuse) for v in vs]
        transparent = layer >= 5
        alpha_test = layer == 4
        decal = layer in (2, 6)
        x0 = max(0, int(min(q[0] for q in p)))
        x1 = min(R - 1, int(max(q[0] for q in p)) + 1)
        y0 = max(0, int(min(q[1] for q in p)))
        y1 = min(R - 1, int(max(q[1] for q in p)) + 1)
        inv = 1.0 / area
        for py in range(y0, y1 + 1):
            cy = py + 0.5
            for px in range(x0, x1 + 1):
                cx = px + 0.5
                w0 = ((p[1][0] - cx) * (p[2][1] - cy) - (p[1][1] - cy) * (p[2][0] - cx)) * inv
                w1 = ((p[2][0] - cx) * (p[0][1] - cy) - (p[2][1] - cy) * (p[0][0] - cx)) * inv
                w2 = 1.0 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                y = w0 * a[1] + w1 * b[1] + w2 * c[1]
                i = py * R + px
                if y < self.depth[i] - (12 if decal else 0):
                    continue
                r = w0 * cols[0][0] + w1 * cols[1][0] + w2 * cols[2][0]
                g = w0 * cols[0][1] + w1 * cols[1][1] + w2 * cols[2][1]
                bl = w0 * cols[0][2] + w1 * cols[1][2] + w2 * cols[2][2]
                al = (w0 * cols[0][3] + w1 * cols[1][3] + w2 * cols[2][3]) / 255.0
                if tex:
                    tw, th, tp = tex
                    s = (w0 * a[3] + w1 * b[3] + w2 * c[3]) / 32.0
                    t = (w0 * a[4] + w1 * b[4] + w2 * c[4]) / 32.0
                    s, t = int(s // 1), int(t // 1)
                    s = min(max(s, 0), tw - 1) if clamp[0] else (
                        (tw - 1 - s % tw) if mirror[0] and (s // tw) & 1 else s % tw)
                    t = min(max(t, 0), th - 1) if clamp[1] else (
                        (th - 1 - t % th) if mirror[1] and (t // th) & 1 else t % th)
                    tr, tg, tb, ta = tp[t * tw + s]
                    if decal_tex:
                        r, g, bl = tr, tg, tb
                    else:
                        r, g, bl = r * tr / 255.0, g * tg / 255.0, bl * tb / 255.0
                    al *= ta / 255.0
                if alpha_test and not transparent and al < 0.5:
                    continue
                if transparent:
                    old = self.color[i]
                    if old is not None:
                        r = old[0] + (r - old[0]) * al
                        g = old[1] + (g - old[1]) * al
                        bl = old[2] + (bl - old[2]) * al
                    elif al < 0.25:
                        continue
                else:
                    self.depth[i] = y
                self.color[i] = (r, g, bl)

    def finish(self):
        bg = (0, 0, 255 * 1 / 5)
        img = []
        for y in range(N):
            for x in range(N):
                acc = [0.0, 0.0, 0.0]
                for dy in range(SS):
                    for dx in range(SS):
                        c = self.color[(y * SS + dy) * R + x * SS + dx] or bg
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]
                img.append(tuple(v / (SS * SS) for v in acc))
        return img


BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]


def quantize(img):
    """Floyd-Steinberg (serpentine) onto the 6x6x6 cube, with damped error
    so flat areas stay calm instead of turning into a checkerboard."""
    work = [list(c) for c in img]
    out = bytearray(N * N)
    for y in range(N):
        xs = range(N) if y % 2 == 0 else range(N - 1, -1, -1)
        d = 1 if y % 2 == 0 else -1
        for x in xs:
            i = y * N + x
            if x in (0, N - 1) or y in (0, N - 1):
                out[i] = FRAME_INDEX
                continue
            c = work[i]
            q = [min(5, max(0, int(round(v / 51.0)))) for v in c]
            out[i] = 8 + q[0] * 36 + q[1] * 6 + q[2]
            err = [(c[k] - q[k] * 51) * 0.8 for k in range(3)]
            for dx, dy, w in ((d, 0, 7), (-d, 1, 3), (0, 1, 5), (d, 1, 1)):
                nx, ny = x + dx, y + dy
                if 0 < nx < N - 1 and 0 < ny < N - 1:
                    t = work[ny * N + nx]
                    for k in range(3):
                        t[k] += err[k] * w / 16.0
    return out


def tiles(indices):
    data = bytearray()
    for ty in range(N // 8):
        for tx in range(N // 8):
            for y in range(8):
                row = (ty * 8 + y) * N + tx * 8
                data += indices[row:row + 8]
    return data


def level_numbers():
    nums, n = {}, 0
    for line in open('levels/level_defines.h'):
        m = re.match(r'\s*(DEFINE_LEVEL|STUB_LEVEL)\((.*)\)', line)
        if m:
            n += 1
            args = split_args(m.group(2))
            if m.group(1) == 'DEFINE_LEVEL':
                nums[args[3]] = n
    return nums


def write_png(path, indices):
    def rgb(i):
        if i < 8:
            return (i * 255 // 7,) * 3
        i -= 8
        return tuple(round(v * 255 / 5) for v in (i // 36, i // 6 % 6, i % 6))
    raw = b''.join(b'\0' + bytes(c for x in range(N) for c in rgb(indices[y * N + x])) for y in range(N))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', N, N, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    level_dir, out_dir = sys.argv[1].rstrip('/'), sys.argv[2]
    png = '--png' in sys.argv
    level = os.path.basename(level_dir)
    number = level_numbers().get(level)
    os.makedirs(out_dir, exist_ok=True)
    if number is None or not os.path.exists(os.path.join(level_dir, 'script.c')):
        return
    files = glob.glob(os.path.join(level_dir, '**', '*.c'), recursive=True) + glob.glob('bin/*.c')
    index = Index(files)
    script = strip_comments(open(os.path.join(level_dir, 'script.c')).read())
    scripts = {m.group(1): m.group(2) for m in
               re.finditer(r'LevelScript\s+(\w+)\[\]\s*=\s*\{(.*?)\};', script, re.S)}
    models = {m.group(1): ('geo', m.group(2)) for m in
              re.finditer(r'LOAD_MODEL_FROM_GEO\(\s*(\w+)\s*,\s*(\w+)\s*\)', script)}
    models.update({m.group(1): ('dl', m.group(2), m.group(3)) for m in
                   re.finditer(r'LOAD_MODEL_FROM_DL\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)', script)})

    def objects(body, seen):
        for m in re.finditer(r'OBJECT(?:_WITH_ACTS)?\s*\(([^;]*?)\)\s*,\s*$|JUMP_LINK\(\s*(\w+)\s*\)', body, re.M):
            if m.group(2):
                if m.group(2) in scripts and m.group(2) not in seen:
                    seen.add(m.group(2))
                    yield from objects(scripts[m.group(2)], seen)
                continue
            args = split_args(m.group(1))
            if args[0] in models:
                yield models[args[0]], [num(a) for a in args[1:7]]
    for m in re.finditer(r'AREA\(\s*(\d+)\s*,\s*(\w+)\s*\)(.*?)END_AREA', script, re.S):
        area, geo = int(m.group(1)), m.group(2)
        terrain = re.search(r'TERRAIN\(\s*(\w+)\s*\)', m.group(3))
        bounds = floor_bounds(index.col[terrain.group(1)]) if terrain and terrain.group(1) in index.col else None
        if bounds is None:
            continue
        lo, hi = bounds
        scale = (max(hi[0] - lo[0], hi[1] - lo[1]) + N - 5) // (N - 4) + 1
        origin_x = (lo[0] + hi[0]) // 2 - scale * N // 2
        origin_z = (lo[1] + hi[1]) // 2 - scale * N // 2
        tris = []
        walk_geo(index, geo, tris, set())
        # Level-specific models placed by the script (WF's fortress parts,
        # platforms...): drawn where they start.
        placed = list(objects(m.group(3), set()))
        # Collision "special objects" place more level geometry pieces;
        # their yaw is in 1/256ths of a turn.
        if terrain and terrain.group(1) in index.col:
            for sm in re.finditer(r'SPECIAL_OBJECT(_WITH_YAW)?\(\s*special_level_geo_(\w\w)\s*,\s*(-?\d+)\s*,'
                                  r'\s*(-?\d+)\s*,\s*(-?\d+)\s*(?:,\s*(-?\d+))?', index.col[terrain.group(1)]):
                model = models.get('MODEL_LEVEL_GEOMETRY_' + sm.group(2))
                if model:
                    yaw = int(sm.group(6) or 0) * 360 / 256
                    placed.append((model, [int(sm.group(3)), int(sm.group(4)), int(sm.group(5)), 0, yaw, 0]))
        for model, a in placed:
            if model[0] == 'geo':
                walk_geo(index, model[1], tris, set(), (a[0], a[1], a[2]), a[4])
            else:
                layer = LAYERS.index(model[2]) if model[2] in LAYERS else 1
                walk_dl(index, model[1], State(), layer, tris, xform=placement((a[0], a[1], a[2]), a[4], 1.0))
        canvas = Canvas(origin_x, origin_z, scale)
        for layer in range(8):
            for tri in tris:
                if tri[0] == layer:
                    canvas.draw(index, tri)
        indices = quantize(canvas.finish())
        base = os.path.join(out_dir, 'L%d_%d' % (number, area))
        with open(base + '.bin', 'wb') as f:
            f.write(b'SMM1' + struct.pack('<iii', origin_x, origin_z, scale) + tiles(indices))
        if png:
            write_png(base + '.png', indices)


if __name__ == '__main__':
    main()
