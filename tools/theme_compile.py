"""
theme_compile.py - turn the log printed by theme_dump.lua into the plain-text
theme file the game reads (see src/theme.c for the format).

Used by prep_assets.py; can also be run by hand:
    python3 theme_compile.py <log.json> <theme_dir> <common_dir> <out_dir>
"""
import json, math, os, sys
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pvrtc

NS = 33                                   # samples per curve
PHASES = ['streak10', 'mult2', 'mult3', 'mult4', 'mult8', 'miss']
MAX_SPRITES = 6000                        # cap on timed effects (sparkles and the like) per theme
BLEND = {'AlphaBlend': 0, 'AdditiveBlend': 1, 'NoBlend': 2, 'SourcePlusOneMinusSourceAlphaBlend': 0}
NOTE_LANE = {60: (0, 0), 62: (1, 0), 64: (2, 0), 66: (3, 0), 61: (0, 1), 65: (1, 1), 63: (2, 1)}


def ease(kind, p):
    p = min(1.0, max(0.0, p))
    if kind == 'EaseInQuad': return p * p
    if kind == 'EaseOutQuad': return 1 - (1 - p) ** 2
    if kind == 'EaseInSin': return 1 - math.cos(p * math.pi / 2)
    if kind == 'EaseOutSin': return math.sin(p * math.pi / 2)
    if kind in ('EaseInOutSin', 'EaseInOutQuad'): return (1 - math.cos(p * math.pi)) / 2
    return p


def num(v, d=0.0):
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else d


def vec(v, n, d):
    out = list(d)
    if isinstance(v, list):
        for i in range(min(n, len(v))):
            out[i] = num(v[i], d[i])
    return out


def sample_points(pts, n=NS):
    """pathPoints: [[t, v], ...] or a plain list of evenly spaced values."""
    if not isinstance(pts, list) or not pts:
        return None
    if isinstance(pts[0], list):
        p = sorted((num(a[0]), num(a[1])) for a in pts if isinstance(a, list) and len(a) >= 2)
    else:
        m = max(1, len(pts) - 1)
        p = [(i / m, num(v)) for i, v in enumerate(pts)]
    if not p:
        return None
    out = []
    for i in range(n):
        t = i / (n - 1)
        if t <= p[0][0]:
            out.append(p[0][1]); continue
        if t >= p[-1][0]:
            out.append(p[-1][1]); continue
        for (t0, v0), (t1, v1) in zip(p, p[1:]):
            if t0 <= t <= t1:
                out.append(v0 if t1 == t0 else v0 + (v1 - v0) * (t - t0) / (t1 - t0)); break
    return out


def sample_curve(c, start, end):
    if isinstance(c, dict):
        s = sample_points(c.get('pathPoints') or c.get('pathPointsFast'))
        if s:
            return s
        if 'startValue' in c or 'targetValue' in c:
            a, b = num(c.get('startValue'), start), num(c.get('targetValue'), end)
            return [a + (b - a) * ease(c.get('curveType'), i / (NS - 1)) for i in range(NS)]
    return [start + (end - start) * i / (NS - 1) for i in range(NS)]


def fmt(vals):
    return ' '.join('%.5g' % v for v in vals)


class Compiler:
    def __init__(self, theme_dir, search_dirs, out_dir):
        self.theme_dir, self.search, self.out = theme_dir, search_dirs, out_dir
        self.tex, self.tex_size = {}, []          # source path -> index
        self.tex_names, self.tex_src, self.used_tex = [], [], set()
        self.pvr_cache = {}

    def open_image(self, path):
        """PNG/JPEG through Pillow; PVRTC through our own decoder."""
        if path.lower().endswith(('.pvrtc', '.pvr')):
            if path not in self.pvr_cache:
                self.pvr_cache[path] = pvrtc.decode(path)
            return self.pvr_cache[path]
        return Image.open(path)

    def texture(self, name):
        if not isinstance(name, str) or not name:
            return None
        stem, ext = os.path.splitext(name)
        # a PNG/JPEG twin of the same name is preferred; a lone .pvrtc is decoded
        compressed = ext.lower() in ('.pvrtc', '.pvr')
        cands = [] if compressed else [name]
        cands += [stem + e for e in ('.png', '.jpg', '.PNG', '.JPG') if stem + e != name]
        if compressed:
            cands.append(name)
        for i, d in enumerate(self.search):
            for c in cands:
                p = os.path.join(d, c)
                if os.path.isfile(p):
                    if p in self.tex:
                        return self.tex[p]
                    try:
                        size = self.open_image(p).size
                    except Exception:
                        continue
                    self.tex[p] = len(self.tex_names)
                    self.tex_names.append(('' if i == 0 else 'c%d_' % i) + stem.replace('/', '_') + '.png')
                    self.tex_src.append(p)
                    self.tex_size.append(size)
                    return self.tex[p]
        return None

    def save_textures(self):
        os.makedirs(self.out, exist_ok=True)
        for i in sorted(self.used_tex):
            self.open_image(self.tex_src[i]).convert('RGBA').save(os.path.join(self.out, self.tex_names[i]))

    def compile(self, log):
        shaders, sh_index, layers = [], {}, []
        sprites, sp_index, anims, an_index = [], {}, [], {}
        columns, events, fb = [], [], {}
        layer_info = {}                           # name -> (is_3d, x offset, y offset)
        screen = [320, 480]
        phase = None
        t_now = 0.0                               # virtual clock while reading the timeline

        def add_shader(st):
            passes = st.get('passes')
            passes = passes if isinstance(passes, list) else [passes]
            out = []
            for p in passes:
                if not isinstance(p, dict):
                    continue
                texs = p.get('texture')
                texs = texs if isinstance(texs, list) else [texs]
                frames = []
                for t in texs:
                    path, region = (t.get('filepath'), t.get('region')) if isinstance(t, dict) else (t, None)
                    ti = self.texture(path)
                    if ti is None:
                        continue
                    w, h = self.tex_size[ti]
                    x, y, rw, rh = vec(region, 4, [0, 0, w, h]) if isinstance(region, list) else (0, 0, w, h)
                    x, y = max(0, min(w - 1, int(x))), max(0, min(h - 1, int(y)))
                    rw, rh = max(1, min(w - x, int(rw))), max(1, min(h - y, int(rh)))
                    frames.append((ti, x, y, rw, rh))
                if not frames:
                    continue
                b = p.get('bindings') or {}
                bind = 0
                if isinstance(b, dict) and b.get('input') == 'revengeModePercentage':
                    bind = 1 if b.get('transformFunction') == 'InverseTransform' else 2
                sx = sy = 0.0
                for d in p.get('deforms') or []:
                    if isinstance(d, dict) and d.get('type') == 'scrollTexture':
                        sx, sy = num(d.get('x')) * num(d.get('speed'), 1), num(d.get('y')) * num(d.get('speed'), 1)
                out.append((BLEND.get(p.get('blend'), 0), bind, sx, sy, num(p.get('animationFrequency')), frames))
            if out and st.get('name'):
                sh_index[st['name']] = len(shaders)
                shaders.append((num(st.get('sortOrder')), out))

        def add_anim(sprite_i, st, obj_id=None, base_delay=0.0):
            sp = sprites[sprite_i]
            prop, dur, loop = st.get('animatedProperty'), num(st.get('duration')), 1 if st.get('loop') else 0
            if dur <= 0:
                return
            chans = []
            if prop == 'alpha':
                s = sample_points(st.get('pathPoints') or st.get('pathPointsFast'))
                if not s:
                    a, b = num(st.get('startValue'), sp['alpha']), num(st.get('targetValue'), sp['alpha'])
                    s = [a + (b - a) * ease(st.get('curveType'), i / (NS - 1)) for i in range(NS)]
                chans.append((0, s))
            elif prop in ('rot', 'scale', 'pos'):
                base = {'rot': (None, None, 1), 'scale': (2, 3, None), 'pos': (4, 5, None)}[prop]
                cur = {'rot': [0, 0, sp['rot']], 'scale': [sp['sx'], sp['sy'], 1], 'pos': [sp['x'], sp['y'], 0]}[prop]
                sv, tv = vec(st.get('startVector'), 3, cur), vec(st.get('targetVector'), 3, cur)
                for axis, key in enumerate(('xCurve', 'yCurve', 'zCurve')):
                    ch = base[axis]
                    if ch is None:
                        continue
                    c = st.get(key)
                    s = sample_points(c.get('pathPoints')) if isinstance(c, dict) else None
                    if not s:
                        if 'startVector' not in st and 'targetVector' not in st:
                            continue
                        s = [sv[axis] + (tv[axis] - sv[axis]) * ease(st.get('curveType'), i / (NS - 1)) for i in range(NS)]
                    chans.append((ch, s))
            first = len(anims)
            for ch, s in chans:
                anims.append([sprite_i, ch, loop, dur, max(0.0, num(st.get('delay'))) + base_delay,
                              max(0.0, num(st.get('loopDelay'))) if loop else 0.0, s])
            if obj_id is not None and chans:
                an_index[obj_id] = (first, len(anims))

        for e in log:
            op = e.get('op')
            if op == 'phase':
                phase = e['name']
                t_now = 0.0
            elif op == 'time':
                t_now = num(e.get('t'))
            elif op == 'shader' and isinstance(e.get('settings'), dict):
                add_shader(e['settings'])
            elif op == 'call' and e.get('method') == 'addLayer' and e.get('args') and isinstance(e['args'][0], dict):
                la = e['args'][0]
                layers.append(la.get('name'))
                off = vec(la.get('position'), 2, [0, 0])
                layer_info[la.get('name')] = (la.get('renderingMode') == '3d', off[0], off[1])
            elif op == 'set' and e.get('key') == 'landscapeMode' and phase == 'init' and e.get('value') not in (0, False, None):
                screen = [480, 320]               # the theme is laid out for a phone held sideways
            elif op == 'screen' and e.get('landscape'):
                screen = [480, 320]
            elif op == 'call' and e.get('method') == 'setFeedbackSettings' and e.get('args'):
                pc = e['args'][0].get('pathColors') if isinstance(e['args'][0], dict) else None
                if isinstance(pc, dict):
                    fb.setdefault('revenge' if phase == 'mult8' else 'normal', pc)
                    if phase in ('init', 'miss'):
                        fb['normal'] = pc
            elif op == 'sprite' and (phase in ('init', 'timeline') or phase in PHASES) and len(sprites) < MAX_SPRITES:
                st = e.get('settings') or {}
                path = str(st.get('scenePath') or '').split('/')
                layer = path[1] if len(path) > 1 else ''
                si = sh_index.get(st.get('shader'))
                is_3d, lx, ly = layer_info.get(layer, (False, 0, 0))
                if si is None or layer == 'Score' or is_3d:
                    continue                      # (3-D layers need a real 3-D renderer: left out)
                born = 0.0 if phase == 'init' else t_now
                ev_phase = PHASES.index(phase) if phase in PHASES else -1
                life = num(st.get('lifetime'))
                if life >= 1000:
                    life = 0.0                    # "forever"
                if phase != 'init' and life <= 0:
                    continue                      # a late permanent sprite: not modelled
                fr = shaders[si][1][0][5][0]
                size = vec(st.get('size'), 2, [fr[3], fr[4]])
                scale = vec(st.get('scale'), 2, [1, 1])
                pos = vec(st.get('position'), 2, [160, 240])
                pos = [pos[0] + lx, pos[1] + ly]
                rv = vec(st.get('rotationVelocity'), 3, [0, 0, 0])
                rot = vec(st.get('rotation'), 3, [0, 0, 0])
                col = vec(st.get('color'), 3, [1, 1, 1])
                sp = dict(shader=si, x=pos[0], y=pos[1], w=size[0], h=size[1], sx=scale[0], sy=scale[1],
                          rot=rot[2], rotvel=rv[2], alpha=num(st.get('alpha'), 1.0), color=col,
                          visible=0 if st.get('visible') in (0, False) else 1,
                          spawn=born + max(0.0, num(st.get('spawnTime'))), life=life, phase=ev_phase,
                          fade=max(0.0, num(st.get('endOfLifeFadeoutTime'))), avel=num(st.get('alphaVelocity')),
                          vel=vec(st.get('velocity'), 2, [0, 0]), acc=vec(st.get('acceleration'), 2, [0, 0]),
                          svel=vec(st.get('scaleVelocity'), 2, [0, 0]),
                          layer=layers.index(layer) if layer in layers else len(layers),
                          sort=shaders[si][0], name='/'.join(path[:2] + [str(st.get('name'))]))
                sp_index[e['id']] = len(sprites)
                sprites.append(sp)
                for a in st.get('animators') or []:
                    if isinstance(a, dict):       # a timed sprite's animations start when it appears
                        add_anim(len(sprites) - 1, a, None, sp['spawn'] if life > 0 else 0.0)
            elif op == 'new' and 'Animator' in str(e.get('kind')) and e.get('parent') in sp_index:
                st = e.get('settings') or {}
                if phase == 'init':
                    add_anim(sp_index[e['parent']], st, e['id'])
                elif phase == 'timeline':
                    add_anim(sp_index[e['parent']], st, e['id'], t_now)
                elif phase in PHASES and sprites[sp_index[e['parent']]]['life'] > 0:
                    add_anim(sp_index[e['parent']], st, e['id'], t_now)      # animates one of the event's own sprites
                elif phase in PHASES and st.get('animatedProperty') == 'alpha' and 'targetValue' in st:
                    events.append((PHASES.index(phase), 3, sp_index[e['parent']], t_now, num(st.get('duration')), num(st.get('targetValue'))))
            elif op == 'column' and phase == 'init':
                st = e.get('settings') or {}
                if st.get('claimedNote') not in NOTE_LANE or num(st.get('player')) != 0:
                    continue
                lane, shake = NOTE_LANE[st['claimedNote']]
                sp_, ep_ = vec(st.get('startPosition'), 2, [160, 104]), vec(st.get('endPosition'), 2, [160, 422])
                ss, es = vec(st.get('startScale'), 2, [0, 0]), vec(st.get('endScale'), 2, [1, 1])
                path = str(st.get('scenePath') or '').split('/')
                columns.append(dict(
                    lane=lane, shake=shake, window=num(st.get('timeWindow'), 1.7),
                    tap=sh_index.get(st.get('tapShader'), -1), hold=sh_index.get(st.get('tapHoldShader'), -1),
                    hold_a=sh_index.get(st.get('tapHoldActiveShader'), -1),
                    splash=splash_for(st.get('tapSplashShader'), lane, sh_index),
                    color=str(st.get('colorChangeSpritePath')),
                    layer=layers.index(path[1]) if len(path) > 1 and path[1] in layers else len(layers),
                    x=sample_curve(st.get('positionXCurve'), sp_[0], ep_[0]),
                    y=sample_curve(st.get('positionYCurve'), sp_[1], ep_[1]),
                    s=sample_curve(st.get('scaleXCurve'), ss[0], es[0]),
                    a=sample_curve(st.get('alphaCurve'), 1, 1)))
            elif phase in PHASES and op == 'call' and e.get('kind') == 'sprite' and e.get('obj') in sp_index:
                if e.get('method') == 'setColorFromArray' and e.get('args'):
                    c = vec(e['args'][0], 3, [1, 1, 1])
                    events.append((PHASES.index(phase), 0, sp_index[e['obj']], t_now, c[0], c[1], c[2]))
            elif phase in PHASES and op == 'set':
                if e.get('kind') == 'sprite' and e.get('obj') in sp_index and e.get('key') == 'visible':
                    events.append((PHASES.index(phase), 1, sp_index[e['obj']], t_now, 1 if e.get('value') not in (0, False) else 0))
                elif e.get('obj') in an_index and e.get('key') == 'targetTime':
                    a0, a1 = an_index[e['obj']]
                    for ai in range(a0, a1):
                        events.append((PHASES.index(phase), 2, ai, t_now, max(0.05, num(e.get('value')))))

        taps = [c for c in columns if not c['shake'] and c['tap'] >= 0]
        if not {0, 1, 2} <= {c['lane'] for c in taps}:
            return None                       # not a usable three-lane theme
        col_layer = min(c['layer'] for c in taps)
        names = {s['name']: i for i, s in enumerate(sprites)}
        order = sorted(range(len(sprites)), key=lambda i: (sprites[i]['layer'], sprites[i]['sort'], i))

        used = {sp['shader'] for sp in sprites}
        for c in columns:
            used.update(v for v in (c['tap'], c['hold'], c['hold_a'], c['splash']) if v >= 0)
        smap = {old: new for new, old in enumerate(sorted(used))}
        tused = sorted({f[0] for i in used for p in shaders[i][1] for f in p[5]})
        tmap = {old: new for new, old in enumerate(tused)}
        self.used_tex.update(tused)

        L = ['ttrtheme 5', 'screen %d %d' % tuple(screen), 'tex %d' % len(tused)]
        L += ['t %s' % self.tex_names[i] for i in tused]
        L.append('shader %d' % len(used))
        for i in sorted(used):
            passes = shaders[i][1]
            L.append('s %d' % len(passes))
            for blend, bind, sx, sy, freq, frames in passes:
                L.append('p %d %d %.5g %.5g %.5g %d' % (blend, bind, sx, sy, freq, len(frames)))
                L += ['f %d %d %d %d %d' % ((tmap[f[0]],) + tuple(f[1:])) for f in frames]
        L.append('sprite %d' % len(sprites))
        for sp in sprites:
            group = 0 if (sp['layer'], sp['sort']) < (col_layer, 6) else 1
            L.append('o %d %s %d %d' % (smap[sp['shader']], fmt([sp['x'], sp['y'], sp['w'], sp['h'], sp['sx'], sp['sy'],
                     sp['rot'], sp['rotvel'], sp['alpha']] + [min(1.0, max(0.0, c)) for c in sp['color']]),
                     sp['visible'], group) + ' ' + fmt([sp['spawn'], sp['life'], sp['fade'], sp['avel']] + sp['vel'] +
                                                       sp['acc'] + sp['svel'] + [sp['phase']]))
        L.append('order ' + ' '.join(str(i) for i in order))
        L.append('anim %d' % len(anims))
        for sp, ch, loop, dur, delay, loop_delay, samples in anims:
            L.append('a %d %d %d %.5g %.5g %.5g %s' % (sp, ch, loop, dur, delay, loop_delay, fmt(samples)))
        L.append('column %d' % len(columns))
        for c in columns:
            L.append('c %d %d %.5g %d %d %d %d %d' % (c['lane'], c['shake'], c['window'], smap.get(c['tap'], -1),
                     smap.get(c['hold'], -1), smap.get(c['hold_a'], -1), smap.get(c['splash'], -1),
                     names.get(c['color'], -1)))
            for k in 'xysa':                   # the lane's sampled curves
                L.append('c%s %s' % (k, fmt(c[k])))
        for state in ('normal', 'revenge'):
            pc = fb.get(state) or fb.get('normal')
            if isinstance(pc, dict):
                cols = []
                for q in ('missColor', 'badColor', 'okColor', 'goodColor', 'perfectColor'):
                    cols += [min(1.0, max(0.0, v)) for v in vec(pc.get(q), 3, [0, 0, 0])]
                L.append('fb %d %s' % (state == 'revenge', fmt(cols)))
        L.append('event %d' % len(events))
        for ev in events:
            L.append('e %d %d %d %s' % (ev[0], ev[1], ev[2], fmt(ev[3:])))
        return '\n'.join(L) + '\n'


def splash_for(named, lane, sh_index):
    """The hit splash: the column's own shader, else whatever the theme calls
    its splash (most use tap_splash_red/green/blue, some a single tap_splash)."""
    for key in (named, 'tap_splash_' + ('red', 'green', 'blue', 'blue')[lane], 'tap_splash', 'tap_splash_generic'):
        if key in sh_index:
            return sh_index[key]
    for name, i in sh_index.items():            # any other shader named like a splash
        if 'splash' in name.lower():
            return i
    return -1


def compile_theme(log, theme_dir, search_dirs, out_dir):
    return Compiler(theme_dir, [theme_dir] + list(search_dirs), out_dir)


if __name__ == '__main__':
    log = json.load(open(sys.argv[1]))
    c = compile_theme(log, sys.argv[2], [sys.argv[3]], sys.argv[4])
    text = c.compile(log)
    if text is None:
        sys.exit('theme has no usable lanes')
    c.save_textures()
    open(os.path.join(sys.argv[4], 'theme.txt'), 'w').write(text)
    print('%d textures, %d bytes' % (len(c.used_tex), len(text)))
