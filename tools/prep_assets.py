#!/usr/bin/env python3
"""
prep_assets.py - turn your own copy of the iOS game into the app_data/ folder
the Vita reimplementation reads.

    python3 prep_assets.py <game.ipa> <out_dir> [--limit N] [--jobs N] [--no-audio]

Writes
    out/songs.txt        one line per track: number, title, artist, seconds, levels
    out/charts/NNN.txt   the note chart for every difficulty level
    out/audio/NNN.ogg    the song, re-encoded to Ogg Vorbis (needs ffmpeg)
    out/themes/<name>/   every theme the songs use: images plus theme.txt, made
                         by running the game's own theme scripts (needs a Lua
                         5.3/5.4 interpreter: lua, lua5.4 or texlua)
    out/gfx/*.png        menu background, HUD sheet, a generated font and the song covers
    out/icon0.png        the game's own icon, for the Vita's home screen
    out/sfx/*.wav        two menu sounds

The script can be stopped and run again: songs already converted are kept.
Audio that does not decode cleanly is skipped (nothing is decrypted).

Requires Python 3.8+, Pillow, and ffmpeg on the PATH.
"""
import argparse, io, os, plistlib, shutil, subprocess, sys, tempfile, unicodedata, zipfile
from concurrent.futures import ThreadPoolExecutor
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import theme_compile

DEFAULT_THEME = 'TTRDJ'

HOLD_MIN = 0.35                       # note-on..note-off longer than this is a hold
# MIDI note -> (lane, is_shake).  The theme scripts put shake 65 in the middle and 63 on the right.
# Note 66 is a fourth lane that a few four-lane themes use.
LANE = {60: (0, 0), 62: (1, 0), 64: (2, 0), 66: (3, 0), 61: (0, 1), 65: (1, 1), 63: (2, 1)}


def ascii_text(s):
    s = unicodedata.normalize('NFKD', str(s or '')).encode('ascii', 'ignore').decode()
    return ' '.join(s.replace('\t', ' ').split()) or '?'


def unarchive(data):
    """Resolve an NSKeyedArchiver property list into plain Python objects."""
    t = plistlib.loads(data)
    objs = t['$objects']

    def res(u):
        o = objs[u.data] if isinstance(u, plistlib.UID) else u
        if isinstance(o, dict) and '$class' in o:
            cn = objs[o['$class'].data]['$classname']
            if cn in ('NSMutableArray', 'NSArray'):
                return [res(x) for x in o['NS.objects']]
            if cn in ('NSMutableDictionary', 'NSDictionary'):
                return {res(k): res(v) for k, v in zip(o['NS.keys'], o['NS.objects'])}
            if cn in ('NSMutableString', 'NSString'):
                return o.get('NS.string', '')
            if cn in ('NSMutableData', 'NSData'):
                return o.get('NS.data', b'')
            return {k: res(v) for k, v in o.items() if k != '$class'}
        return None if o == '$null' else o

    return res(t['$top']['root'])


def chart_notes(midi_file):
    """MIDI-style events -> sorted (time, lane, kind, hold_seconds)."""
    notes = []
    for tr in midi_file.get('tracks') or []:
        on = {}
        for e in tr.get('events') or []:
            note, typ = e.get('note'), e.get('type')
            if note not in LANE:
                continue
            if typ == 0 and e.get('velocity', 0) > 0:
                on[note] = len(notes)
                notes.append([float(e['time']), LANE[note][0], LANE[note][1], 0.0])
            elif typ in (0, 1) and note in on:
                n = notes[on.pop(note)]
                d = float(e['time']) - n[0]
                if n[2] == 0 and d >= HOLD_MIN:
                    n[3] = d
    notes.sort()
    return notes


def make_font(path_png, path_txt, size=22):
    try:
        font = ImageFont.load_default(size=size)
    except Exception:
        font = ImageFont.load_default()
    chars = [chr(c) for c in range(32, 127)]
    asc, desc = font.getmetrics() if hasattr(font, 'getmetrics') else (size, 4)
    cell_h = asc + desc + 2
    im = Image.new('RGBA', (512, 256), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    x, y, rows = 1, 1, []
    for ch in chars:
        adv = max(1, int(round(d.textlength(ch, font=font))))
        if x + adv + 2 > im.size[0]:
            x, y = 1, y + cell_h + 1
        d.text((x, y), ch, font=font, fill=(255, 255, 255, 255))
        rows.append('%d %d %d %d %d' % (ord(ch), x, y, adv, cell_h))
        x += adv + 2
    im.save(path_png)
    with open(path_txt, 'w') as f:
        f.write('font %d %d\n' % (len(rows), cell_h) + '\n'.join(rows) + '\n')


ART_TILE, ART_COLS = 64, 32


def build_art(z, tracks, out):
    """Every song's cover (artwork.png in its .track folder, a 64x64 TIFF despite
    the name) packed into gfx/art.png: song N at tile N-1, ART_COLS tiles per row.
    A song without one leaves its tile see-through, and the game draws a lettered
    tile there instead."""
    if not tracks:
        return
    rows = (max(t['num'] for t in tracks) + ART_COLS - 1) // ART_COLS
    sheet = Image.new('RGBA', (ART_COLS * ART_TILE, rows * ART_TILE), (0, 0, 0, 0))
    found = 0
    for t in tracks:
        try:
            im = Image.open(io.BytesIO(z.read(t['dir'] + 'artwork.png'))).convert('RGB')
        except Exception:
            continue
        if im.size != (ART_TILE, ART_TILE):     # a few are not square: fit, black bars, no stretching
            from PIL import ImageOps
            im = ImageOps.pad(im, (ART_TILE, ART_TILE), Image.LANCZOS, color=(0, 0, 0))
        i = t['num'] - 1
        sheet.paste(im, ((i % ART_COLS) * ART_TILE, (i // ART_COLS) * ART_TILE))
        found += 1
    sheet.save(os.path.join(out, 'gfx', 'art.png'))
    print('%d of %d song covers' % (found, len(tracks))); sys.stdout.flush()


def build_themes(z, app, names, wanted, out, lua):
    """wanted: {theme name: set of difficulty levels}.  Returns {(theme, level)} that compiled."""
    import json
    here = os.path.dirname(os.path.abspath(__file__))
    ok = set()
    with tempfile.TemporaryDirectory() as td:
        members = [n for n in names if n.startswith(app + 'Themes/') or n == app + 'game_defaults.cfg']
        z.extractall(td, members)
        root = os.path.join(td, app)
        common = os.path.join(root, 'Themes', 'Common.ttrTheme')
        for done_n, theme in enumerate(sorted(wanted)):
            print('[themes %d/%d] %s' % (done_n + 1, len(wanted), theme), flush=True)
            tdir = os.path.join(root, 'Themes', theme + '.ttrTheme')
            if not os.path.isfile(os.path.join(tdir, 'theme.cfg')):
                continue
            odir = os.path.join(out, 'themes', theme)
            comp = theme_compile.compile_theme(None, tdir, [common, root], odir)
            texts = {}
            for level in sorted(wanted[theme]):
                try:
                    r = subprocess.run([lua, os.path.join(here, 'theme_dump.lua'),
                                        os.path.join(root, 'game_defaults.cfg'), os.path.join(tdir, 'theme.cfg'),
                                        str(level)], cwd=tdir, capture_output=True, timeout=60,
                                       stdin=subprocess.DEVNULL)
                    text = comp.compile(json.loads(r.stdout.decode('utf-8', 'replace')))
                except Exception:
                    text = None
                if text:
                    texts[level] = text
            if not texts:
                continue
            comp.save_textures()
            same = len(set(texts.values())) == 1
            with open(os.path.join(odir, 'theme.txt'), 'w') as f:
                f.write(next(iter(texts.values())))
            for level, text in texts.items():
                path = os.path.join(odir, 'theme%d.txt' % level)
                if not same:
                    with open(path, 'w') as f:
                        f.write(text)
                elif os.path.exists(path):
                    os.remove(path)
                ok.add((theme, level))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ipa')
    ap.add_argument('out')
    ap.add_argument('--limit', type=int, default=0, help='only the first N tracks (0 = all)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    ap.add_argument('--no-audio', action='store_true')
    ap.add_argument('--max-seconds', type=float, default=0, help='stop starting new songs after this long')
    a = ap.parse_args()
    import time
    t_start = time.time()

    out = a.out
    for d in ('charts', 'audio', 'gfx', 'sfx'):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg and not a.no_audio:
        sys.exit('ffmpeg not found on the PATH (install it, or pass --no-audio)')

    z = zipfile.ZipFile(a.ipa)
    names = z.namelist()
    app = next((n for n in names if n.startswith('Payload/') and n.endswith('.app/')), None)
    if not app:
        app = names[0].split('.app/')[0] + '.app/' if '.app/' in names[0] else sys.exit('not an .ipa')

    # ---- graphics: default theme + HUD sheet, re-saved as real RGBA PNGs ----
    gfx = {'background': 'Themes/TTRDJ.ttrTheme/background.png', 'hud': 'Themes/Common.ttrTheme/hudmatrix.png'}
    for name, src in gfx.items():
        Image.open(io.BytesIO(z.read(app + src))).convert('RGBA').save(os.path.join(out, 'gfx', name + '.png'))
    make_font(os.path.join(out, 'gfx', 'font.png'), os.path.join(out, 'gfx', 'font.txt'))

    # ---- the app's own icon, for the Vita home screen ("skittle") ----------
    icon = None
    for cand in ('iTunesArtwork', app + 'iTunesArtwork', app + 'Icon@2x.png', app + 'Icon.png'):
        if cand not in names:
            continue
        try:
            im = Image.open(io.BytesIO(z.read(cand))).convert('RGB')
        except Exception:
            continue
        if icon is None or im.size[0] > icon.size[0]:
            icon = im
    if icon is not None:
        # 128x128, and 8-bit indexed: the Vita refuses to install a package
        # whose LiveArea images are plain 24-bit PNGs (error 0x8010113D).
        small = icon.resize((128, 128), Image.LANCZOS)
        small.convert('P', palette=Image.ADAPTIVE, colors=256).save(os.path.join(out, 'icon0.png'))
        print('  icon from %dx%d artwork' % icon.size)
    if ffmpeg:
        for name, src in {'click': 'TTRRotatingControlClick.wav', 'slide': 'menu_slide.wav'}.items():
            dst = os.path.join(out, 'sfx', name + '.wav')
            if app + src in names and not os.path.exists(dst):
                with tempfile.NamedTemporaryFile(suffix='.wav') as tf:
                    tf.write(z.read(app + src)); tf.flush()
                    subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-i', tf.name, '-ac', '1',
                                    '-ar', '44100', '-c:a', 'pcm_s16le', dst], stdin=subprocess.DEVNULL)

    # ---- tracks ----
    dirs = sorted({n.split('.track/')[0] + '.track/' for n in names if '.track/' in n and '/Tracks/' in n})
    tracks = []
    for d in dirs:
        audio = next((d + f for f in ('audio.m4c', 'audio.m4a', 'audio.mp3') if d + f in names), None)
        if d + 'taptrack.ttr2_track' not in names or not audio:
            continue
        try:
            info = plistlib.loads(z.read(d + 'info.plist'))
            if 'data' in info:
                info = plistlib.loads(info['data'])
        except Exception:
            info = {}
        lt = info.get('levelThemes') if isinstance(info.get('levelThemes'), dict) else {}
        tracks.append({'dir': d, 'audio': audio, 'theme': str(info.get('themeName') or DEFAULT_THEME),
                       'level_themes': {str(k): str(v) for k, v in lt.items()},
                       'title': ascii_text(info.get('kTTRDownloadableItemTitleKey')),
                       'artist': ascii_text(info.get('kTTRDownloadableItemSubtitleKey'))})
    tracks.sort(key=lambda t: (t['artist'].lower(), t['title'].lower(), t['dir']))
    if a.limit:
        tracks = tracks[:a.limit]
    for i, t in enumerate(tracks):
        t['num'] = i + 1
    print('%d tracks found' % len(tracks)); sys.stdout.flush()

    build_art(z, tracks, out)

    import threading
    zlock = threading.Lock()
    counter = {'n': 0}

    def do_track(t):
        num = '%03d' % t['num']
        chart = os.path.join(out, 'charts', num + '.txt')
        ogg = os.path.join(out, 'audio', num + '.ogg')
        try:
            if True:                      # charts are cheap: always rewrite them
                with zlock:
                    raw = z.read(t['dir'] + 'taptrack.ttr2_track')
                root = unarchive(raw)
                files = root if isinstance(root, dict) and 'tracks' not in root else {3: root}
                lines, end = [], 0.0
                for lv in sorted(files, key=lambda k: int(k)):
                    notes = chart_notes(files[lv])
                    if not notes:
                        continue
                    lines.append('level %d %d' % (int(lv), len(notes)))
                    lines += ['%.4f %d %d %.3f' % tuple(n) for n in notes]
                    end = max(end, notes[-1][0] + notes[-1][3])
                if not lines:
                    return 'no notes'
                with open(chart + '.tmp', 'w') as f:
                    f.write('end %.3f\n' % end + '\n'.join(lines) + '\n')
                os.replace(chart + '.tmp', chart)
            if a.no_audio or (os.path.exists(ogg) and os.path.getsize(ogg) > 0):
                return None
            if a.max_seconds and time.time() - t_start > a.max_seconds:
                return 'later'
            with tempfile.TemporaryDirectory() as td:
                src = os.path.join(td, 'in.bin')
                with zlock:
                    data = z.read(t['audio'])
                with open(src, 'wb') as f:
                    f.write(data)
                tmp = os.path.join(td, 'out.ogg')
                r = subprocess.run([ffmpeg, '-y', '-v', 'error', '-xerror', '-i', src, '-vn', '-ac', '2',
                                    '-ar', '44100', '-c:a', 'libvorbis', '-q:a', '2', tmp],
                                   stdin=subprocess.DEVNULL, capture_output=True)
                if r.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) < 1000:
                    return 'audio does not decode'
                shutil.copyfile(tmp, ogg + '.part')
                os.replace(ogg + '.part', ogg)
            return None
        except Exception as e:           # one bad track must not stop the rest
            return 'error: %s' % e

    def do_track_counted(t):
        r = do_track(t)
        with zlock:
            counter['n'] += 1
            print('[songs %d/%d] %s - %s%s' % (counter['n'], len(tracks), t['artist'], t['title'],
                                               '' if not r or r == 'later' else '  (%s)' % r), flush=True)
        return r

    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        results = list(ex.map(do_track_counted, tracks))

    # ---- themes: run each theme script once per difficulty level it is used at ----
    def theme_for(t, level):
        return t['level_themes'].get(str(level)) or t['theme'] or DEFAULT_THEME

    levels_of = {}
    for t in tracks:
        chart = os.path.join(out, 'charts', '%03d.txt' % t['num'])
        if os.path.exists(chart):
            with open(chart) as c:
                levels_of[t['num']] = [int(l.split()[1]) for l in c if l.startswith('level ')]
    wanted = {DEFAULT_THEME: {0, 1, 2, 3, 4}}
    for t in tracks:
        for lv in levels_of.get(t['num'], []):
            wanted.setdefault(theme_for(t, lv), set()).add(lv)
    lua = next((p for p in map(shutil.which, ('lua5.4', 'lua5.3', 'lua', 'texlua', 'luajit')) if p), None)
    themes_ok = set()
    if lua:
        themes_ok = build_themes(z, app, names, wanted, out, lua)
        print('%d themes built' % len({n for n, _ in themes_ok})); sys.stdout.flush()
    if not any(n == DEFAULT_THEME for n, _ in themes_ok):
        sys.exit('could not build the default theme - is a Lua 5.3/5.4 interpreter (lua, texlua) installed?')

    # ---- index: only songs that are completely converted ----
    ok, pending, skipped = 0, 0, []
    with open(os.path.join(out, 'songs.txt.tmp'), 'w') as f:
        for t, r in zip(tracks, results):
            num = '%03d' % t['num']
            chart = os.path.join(out, 'charts', num + '.txt')
            ogg = os.path.join(out, 'audio', num + '.ogg')
            if r == 'later':
                pending += 1
                continue
            if r or not os.path.exists(chart) or (not a.no_audio and not os.path.exists(ogg)):
                skipped.append('%s - %s: %s' % (t['artist'], t['title'], r or 'missing output'))
                continue
            with open(chart) as c:
                head = c.read().split('\n')
            end = float(head[0].split()[1])
            levels = ','.join(l.split()[1] for l in head if l.startswith('level '))
            themes = ','.join('%d=%s' % (lv, theme_for(t, lv) if (theme_for(t, lv), lv) in themes_ok else DEFAULT_THEME)
                              for lv in levels_of.get(t['num'], []))
            f.write('%s\t%s\t%s\t%.1f\t%s\t%s\n' % (num, t['title'], t['artist'], end, levels, themes))
            ok += 1
    os.replace(os.path.join(out, 'songs.txt.tmp'), os.path.join(out, 'songs.txt'))
    print('%d songs ready, %d still to convert, %d skipped' % (ok, pending, len(skipped)))
    for s in skipped:
        print('  skipped:', s)
    if pending:
        print('run the same command again to continue')


if __name__ == '__main__':
    main()
