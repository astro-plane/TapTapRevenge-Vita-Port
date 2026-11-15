#!/usr/bin/env python3
"""
converter_gui.py - a small window for turning your copy of the game into the
app_data folder, so you do not have to use a terminal.

Put the .ipa in the same folder as this project and run:

    Windows   double-click  Convert on Windows.bat
    macOS     double-click  Convert on macOS.command
    anywhere  python3 tools/converter_gui.py

It finds the .ipa, checks that the tools it needs are installed, runs
tools/prep_assets.py and shows its progress.  Everything it does can also be
done by hand; see README.md.

Needs nothing beyond Python itself (tkinter ships with Python on Windows and
macOS).  The conversion itself needs Pillow, ffmpeg and a Lua interpreter,
and the window tells you which of those are missing and how to get them.
"""
import os
import platform
import queue
import re
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LUA_NAMES = ('lua5.4', 'lua5.3', 'lua', 'texlua', 'luajit')
MAC, WIN = sys.platform == 'darwin', os.name == 'nt'


# ----------------------------------------------------------------- checks --
# (plain functions, so they can be tested without opening a window)

def search_folders(folder):
    """Where to look for the game: this folder first, then the obvious places."""
    home = os.path.expanduser('~')
    out = [folder, os.path.dirname(folder), os.path.join(home, 'Desktop'), os.path.join(home, 'Downloads')]
    seen, keep = set(), []
    for d in out:
        if d and os.path.isdir(d) and d not in seen:
            seen.add(d)
            keep.append(d)
    return keep


def find_ipa(folder):
    """Any .ipa in those folders, newest first."""
    hits = []
    for d in search_folders(folder):
        try:
            hits += [os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith('.ipa')]
        except OSError:
            pass
    return sorted(hits, key=lambda p: -os.path.getmtime(p))


def have_pillow():
    try:
        import PIL                                    # noqa: F401
        return True
    except Exception:
        return False


def find_lua():
    for n in LUA_NAMES:
        p = shutil.which(n)
        if p:
            return p
    return None


def requirements():
    """[(name, ok, what it is for, how to get it)] - ordered as shown."""
    py = '%d.%d' % sys.version_info[:2]
    if WIN:
        ff_fix = 'In PowerShell:  winget install Gyan.FFmpeg'
        lua_fix = 'In PowerShell:  winget install DEVCOM.Lua'
    elif MAC:
        ff_fix = 'In Terminal:  brew install ffmpeg'
        lua_fix = 'In Terminal:  brew install lua'
    else:
        ff_fix = 'Install ffmpeg with your package manager'
        lua_fix = 'Install lua5.4 with your package manager'
    return [
        ('Python 3.8+', sys.version_info >= (3, 8), 'runs everything', 'You have %s' % py),
        ('Pillow', have_pillow(), 'reads and writes the artwork', 'Press Install Pillow below'),
        ('ffmpeg', bool(shutil.which('ffmpeg')), 'converts the songs to Ogg Vorbis', ff_fix),
        ('Lua 5.3/5.4', bool(find_lua()), "runs the game's own theme scripts", lua_fix),
    ]


def check_ipa(path):
    """(ok, message) - is this the right .ipa, and does it still have songs?"""
    import zipfile
    if not path or not os.path.isfile(path):
        return False, 'Choose the game file (.ipa).'
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
    except Exception as e:
        return False, 'Not readable as an .ipa: %s' % e
    tracks = sum(1 for n in names if n.endswith('taptrack.ttr2_track'))
    themes = sum(1 for n in names if n.endswith('.ttrTheme/theme.cfg'))
    if not any('/Tracks/' in n for n in names):
        return False, 'No Tracks folder inside: this does not look like Tap Tap Revenge 3.'
    if tracks == 0:
        return False, 'The songs are missing from this copy (0 charts inside).'
    return True, '%d songs and %d themes found inside.' % (tracks, themes)


def output_size(folder):
    n = total = 0
    for dirpath, _, files in os.walk(folder):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(dirpath, f))
                n += 1
            except OSError:
                pass
    return n, total


def human(n):
    return '%.1f GB' % (n / 1e9) if n >= 1e9 else '%d MB' % round(n / 1e6)


def open_folder(path):
    try:
        if MAC:
            subprocess.Popen(['open', path])
        elif WIN:
            os.startfile(path)                        # noqa: B606  (Windows only)
        else:
            subprocess.Popen(['xdg-open', path])
    except Exception:
        pass


# -------------------------------------------------------------------- GUI --

def main():
    import tkinter as tk
    from tkinter import filedialog, ttk

    win = tk.Tk()
    win.title('Tap Tap Vita - data converter')
    win.minsize(680, 520)
    pad = dict(padx=14, pady=6)

    state = {'proc': None, 'running': False}
    lines = queue.Queue()

    ipa_var = tk.StringVar(value=(find_ipa(ROOT) or [''])[0])
    out_var = tk.StringVar(value=os.path.join(ROOT, 'app_data'))
    status_var = tk.StringVar(value='')

    tk.Label(win, text='Turns your copy of Tap Tap Revenge 3 into the app_data folder the Vita game reads.',
             anchor='w', justify='left').pack(fill='x', **pad)

    # --- the game file ---
    box = tk.LabelFrame(win, text='1. The game')
    box.pack(fill='x', **pad)
    row = tk.Frame(box)
    row.pack(fill='x', padx=10, pady=(8, 2))
    tk.Entry(row, textvariable=ipa_var).pack(side='left', fill='x', expand=True)

    def choose_ipa():
        p = filedialog.askopenfilename(title='Choose the game .ipa',
                                       filetypes=[('iOS app', '*.ipa'), ('All files', '*.*')])
        if p:
            ipa_var.set(p)
            refresh()

    tk.Button(row, text='Choose...', command=choose_ipa).pack(side='left', padx=(8, 0))
    ipa_note = tk.Label(box, text='', anchor='w', justify='left', fg='#555')
    ipa_note.pack(fill='x', padx=10, pady=(0, 8))

    # --- requirements ---
    rbox = tk.LabelFrame(win, text='2. Tools needed')
    rbox.pack(fill='x', **pad)
    rows = []
    for _ in range(4):
        r = tk.Frame(rbox)
        r.pack(fill='x', padx=10, pady=1)
        mark = tk.Label(r, text=' ', width=2)
        mark.pack(side='left')
        name = tk.Label(r, text='', width=14, anchor='w')
        name.pack(side='left')
        note = tk.Label(r, text='', anchor='w', fg='#555')
        note.pack(side='left', fill='x', expand=True)
        rows.append((mark, name, note))
    rrow = tk.Frame(rbox)
    rrow.pack(fill='x', padx=10, pady=(4, 8))
    pillow_btn = tk.Button(rrow, text='Install Pillow', command=lambda: install_pillow())
    pillow_btn.pack(side='left')
    tk.Button(rrow, text='Check again', command=lambda: refresh()).pack(side='left', padx=8)

    # --- output + run ---
    obox = tk.LabelFrame(win, text='3. Convert')
    obox.pack(fill='x', **pad)
    row2 = tk.Frame(obox)
    row2.pack(fill='x', padx=10, pady=(8, 2))
    tk.Label(row2, text='Creates:').pack(side='left')
    tk.Entry(row2, textvariable=out_var).pack(side='left', fill='x', expand=True, padx=8)

    def choose_out():
        p = filedialog.askdirectory(title='Where to create app_data')
        if p:
            out_var.set(os.path.join(p, 'app_data'))

    tk.Button(row2, text='Choose...', command=choose_out).pack(side='left')

    row3 = tk.Frame(obox)
    row3.pack(fill='x', padx=10, pady=(6, 8))
    go_btn = tk.Button(row3, text='Convert', width=14, command=lambda: start())
    go_btn.pack(side='left')
    stop_btn = tk.Button(row3, text='Stop', width=8, state='disabled', command=lambda: stop())
    stop_btn.pack(side='left', padx=8)
    open_btn = tk.Button(row3, text='Open app_data', state='disabled',
                         command=lambda: open_folder(out_var.get()))
    open_btn.pack(side='left')

    bar = ttk.Progressbar(obox, mode='determinate', maximum=1000)
    bar.pack(fill='x', padx=10, pady=(0, 4))
    tk.Label(obox, textvariable=status_var, anchor='w').pack(fill='x', padx=10, pady=(0, 8))

    # --- log ---
    lbox = tk.LabelFrame(win, text='Details')
    lbox.pack(fill='both', expand=True, **pad)
    log = tk.Text(lbox, height=10, wrap='none')
    sb = tk.Scrollbar(lbox, command=log.yview)
    log.configure(yscrollcommand=sb.set, state='disabled')
    sb.pack(side='right', fill='y')
    log.pack(side='left', fill='both', expand=True)

    def say(text):
        log.configure(state='normal')
        log.insert('end', text.rstrip() + '\n')
        log.see('end')
        log.configure(state='disabled')

    # --- wiring ---
    def refresh():
        ok, msg = check_ipa(ipa_var.get())
        ipa_note.configure(text=msg, fg='#1a7f37' if ok else '#b00020')
        reqs = requirements()
        for (mark, name, note), (rn, rok, what, fix) in zip(rows, reqs):
            mark.configure(text='OK' if rok else 'X', fg='#1a7f37' if rok else '#b00020')
            name.configure(text=rn)
            note.configure(text=('%s' % what) if rok else '%s - %s' % (what, fix),
                           fg='#555' if rok else '#b00020')
        pillow_btn.configure(state='disabled' if have_pillow() else 'normal')
        ready = ok and all(r[1] for r in reqs)
        go_btn.configure(state='normal' if (ready and not state['running']) else 'disabled')
        if not state['running']:
            status_var.set('Ready. Press Convert.' if ready
                           else 'Fix the items marked X above, then press Check again.')
        out = out_var.get()
        open_btn.configure(state='normal' if os.path.isdir(out) else 'disabled')

    def install_pillow():
        say('Installing Pillow...')
        def work():
            r = subprocess.run([sys.executable, '-m', 'pip', 'install', '--user', 'pillow'],
                               capture_output=True, text=True)
            lines.put(('log', (r.stdout or '') + (r.stderr or '')))
            lines.put(('refresh', None))
        threading.Thread(target=work, daemon=True).start()

    def start():
        ok, _ = check_ipa(ipa_var.get())
        if not ok or state['running']:
            return
        state['running'] = True
        go_btn.configure(state='disabled')
        stop_btn.configure(state='normal')
        bar.configure(value=0)
        status_var.set('Starting...')
        say('Converting %s' % ipa_var.get())

        cmd = [sys.executable, '-u', os.path.join(HERE, 'prep_assets.py'), ipa_var.get(), out_var.get()]

        def work():
            try:
                p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1, cwd=ROOT)
                state['proc'] = p
                for line in p.stdout:
                    lines.put(('line', line))
                p.wait()
                lines.put(('done', p.returncode))
            except Exception as e:
                lines.put(('line', 'could not start the converter: %s\n' % e))
                lines.put(('done', 1))

        threading.Thread(target=work, daemon=True).start()

    def stop():
        p = state.get('proc')
        if p and p.poll() is None:
            p.terminate()
            say('Stopped. Press Convert to carry on where it left off.')

    song_re = re.compile(r'^\[songs (\d+)/(\d+)\]')
    theme_re = re.compile(r'^\[themes (\d+)/(\d+)\]')

    def pump():
        try:
            while True:
                kind, payload = lines.get_nowait()
                if kind == 'log':
                    say(payload)
                elif kind == 'refresh':
                    refresh()
                elif kind == 'line':
                    m = song_re.match(payload)
                    t = theme_re.match(payload)
                    if m:
                        done, total = int(m.group(1)), int(m.group(2))
                        bar.configure(value=900 * done / max(1, total))
                        status_var.set('Songs: %d of %d' % (done, total))
                    elif t:
                        done, total = int(t.group(1)), int(t.group(2))
                        bar.configure(value=900 + 100 * done / max(1, total))
                        status_var.set('Themes: %d of %d' % (done, total))
                        say(payload)
                    else:
                        say(payload)
                elif kind == 'done':
                    state['running'] = False
                    state['proc'] = None
                    stop_btn.configure(state='disabled')
                    bar.configure(value=1000 if payload == 0 else bar['value'])
                    if payload == 0 and os.path.isdir(out_var.get()):
                        n, total = output_size(out_var.get())
                        status_var.set('Finished: %s, %d files in app_data.' % (human(total), n))
                        say('Done. Copy this app_data folder to ux0:data/ttrvita/ on the Vita.')
                    else:
                        status_var.set('Did not finish - see the details above.')
                    refresh()
        except queue.Empty:
            pass
        win.after(120, pump)

    refresh()
    found = find_ipa(ROOT)
    if not found:
        say('No .ipa found. Put the game file in this folder (%s) and press Check again,' % ROOT)
        say('or press Choose... to point at it wherever it is.')
    else:
        say('Found %s in %s' % (os.path.basename(found[0]), os.path.dirname(found[0])))
        if len(found) > 1:
            say('(%d other .ipa files nearby; use Choose... to pick a different one)' % (len(found) - 1))
    pump()
    win.mainloop()


if __name__ == '__main__':
    if '--check' in sys.argv:                         # headless self-test
        print('looked in:', search_folders(ROOT))
        print('ipa files found:', [os.path.basename(p) for p in find_ipa(ROOT)])
        for n, ok, what, fix in requirements():
            print('%-12s %-3s %s' % (n, 'OK' if ok else 'X', what if ok else fix))
        for p in sys.argv[1:]:
            if p.lower().endswith('.ipa'):
                print(os.path.basename(p), '->', check_ipa(p))
        sys.exit(0)
    main()
