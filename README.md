# Tap Tap – PS Vita reimplementation

A from-scratch C reimplementation of the rhythm game's single-player mode for
the PS Vita. It does not run or translate the iOS program. It plays the
original note charts, music and per-song themes, which `tools/prep_assets.py`
converts from your own copy of the `.ipa` into `app_data/`.

**Status:** game logic and drawing are tested on a PC with a headless renderer
(see *Tests*). The Vita/SDL2 layer (`src/plat_sdl.c`), including music
streaming, **has not been compiled with VitaSDK or run on a Vita yet.**

## What is in the game

- Song list (343 tracks) in the style of a classic phone music library:
  alphabetical sections with an A-Z index, Songs / Artists / Top Scores tabs,
  a Shuffle button, and two looks to switch between (silver, or blue-grey
  with see-through section headers). Difficulty select, results screen
- Each song's own theme (119 of them): background, lane shapes, note art,
  note speed, moving/pulsing scenery, and the colour and light changes on
  streaks, multipliers and misses
- Three lanes with tap notes, hold notes and shake arrows, timed to the music
  (four lanes in the one theme that has them)
- Score, streak, 2x/3x/4x multipliers, and an 8x "revenge" mode
- A saved best for each song and difficulty, shown in the song list, on the
  difficulty screen and on the results screen
- Landscape and vertical layouts, pause

Not included: everything online (store, leaderboards, battles, chat), two-player
mode, weapons/items, the tutorial and album art. The menus
are this project's own and do not copy the original's look.

## How faithful it is

From the original files, exactly:
- every note's time, lane, hold length and shake flag, for each difficulty
- every theme's set-up. The converter runs the game's own Lua theme scripts
  against a recording stand-in for the engine (`tools/theme_dump.lua`) and
  writes down what each script creates: images and their blend modes, sprites,
  looping animations, the six lanes with their position/scale/fade curves and
  fall time (1.2 s to 2.0 s depending on the theme), feedback colours, and what
  the script changes on a streak of 10, at 2x/3x/4x/8x and on a miss

- effects the scripts schedule with timers (drifting sparkles, smoke, and so
  on): the converter runs those timers on a virtual clock for 7 minutes and
  records each short-lived sprite with the time it appears
- effects started by the streak/multiplier/miss handlers (one theme's
  "ghost" images at 2x, 3x, 4x and 8x), including anything a handler
  schedules for a few seconds later

Approximated or missing in the themes:
- the scripts' per-hit handlers are empty in every theme, so there are no
  script-made hit sparkles to copy; the hit splash is the theme's own splash
  image, named per lane or as one shared `tap_splash`
- themes that leave the lane colour to the engine's built-in feedback get no
  colour change on a hit here, only the splash
- sprites in 3-D layers (one theme's water surface) are left out
- effects a script starts in its streak, multiplier or miss handler are
  captured for the first 45 seconds after the event, then stop; an effect the
  original keeps repeating for as long as a multiplier lasts ends early here
- hold trails are a ribbon of the theme's trail image that follows the lane;
  how the original stretched that image along a hold is an estimate
- a few textures ship only in PVRTC-compressed form (the default theme's
  background light among them). `tools/pvrtc.py` decodes them; its 4-bit mode
  matches a reference image closely, its 2-bit mode could not be checked
- busy themes stack many see-through layers; the Vita may drop frames on them

This project's own choices (constants at the top of `src/game.c`):
- timing windows (60 / 120 / 180 ms) and points (100 / 75 / 50, holds 20 per 0.1 s)
- multiplier thresholds (streak 20 / 30 / 40) and the rule that shaking with a
  50 streak starts 8x mode until the next miss
- a note counts as a hold when its chart length is 0.35 s or more
- the HUD and the menu screens

## Controls

| Vita | PC build | Action |
|---|---|---|
| Touch the lower half of a lane | mouse | hit that lane (multi-touch works) |
| Left / Up or Down / Right, or Square / Triangle or Cross / Circle | A / S / D or arrows | left / middle / right lane |
| Left / Right (or Up, Down) / Square (or Triangle, Cross) / Circle | | the four lanes, left to right, in a four-lane theme |
| Shake the console, or L / R | W, or Q / E | shake (shake arrows, or start 8x) |
| START | P | pause; then Circle quits the song |
| SELECT | R | switch landscape / vertical layout |
| Up / Down, L / R | arrows | move in the song list (L/R: previous / next letter) |
| Touch the A-Z index | | jump to that letter |
| Triangle, or the tab bar | | Songs / Artists / Top Scores |
| Square, or the Shuffle tab | | pick a song at random |
| START, or the Style button | | switch between the two list looks (saved) |
| Cross / Circle | Enter / Esc | choose / back |

In the vertical layout, hold the Vita turned a quarter turn clockwise.

## What you need

### 1. The game

One file: **`Tap Tap Revenge 3.ipa`**, version 3.4.1 (bundle id
`com.tapulous.taptaprevengeIII`), about 1 GB. Nothing else from the game is
needed, and nothing in this repository comes from it.

It must be Tap Tap Revenge **3**, and it must still carry its songs: some
copies have an empty `Tracks` folder because the songs were downloaded
separately. Check before you start:

```sh
unzip -Z1 "Tap Tap Revenge 3.ipa" | grep -c taptrack.ttr2_track
```

A good copy prints **344**. Zero means the songs are missing; a much smaller
number means a partial copy. The archive should also contain
`Payload/Tap Tap.app/` with `Tracks/` and `Themes/` folders inside.

### 2. Tools on your computer

| Tool | What it is for | macOS |
|---|---|---|
| VitaSDK | compiles for the Vita | see below |
| CMake 3.16+ | runs the build | `brew install cmake` |
| Python 3.8+ with Pillow | converts the artwork and charts | `pip3 install pillow` |
| ffmpeg | converts the songs to Ogg Vorbis | `brew install ffmpeg` |
| Lua 5.3 or 5.4 | runs the game's own theme scripts | `brew install lua` |

Any of `lua`, `lua5.4`, `lua5.3`, `texlua` or `luajit` will do for the last
one; `texlua` already comes with MacTeX, so check `which texlua` before
installing anything.

The converter window in step 1 checks all of these for you and says what is
missing, so you can skip ahead and let it tell you.

VitaSDK, with the libraries this game needs:

```sh
cd ~
git clone https://github.com/vitasdk/vdpm ~/vdpm
cd ~/vdpm && ./bootstrap-vitasdk.sh
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
vdpm install sdl2 libpng zlib libvorbis libogg
```

Put those two `export` lines in your `~/.zshrc` so they survive a new
terminal. You also need about 2.5 GB of free disk space.

## Build and install

### Step 1 — convert the game data (about 15 minutes)

Put `Tap Tap Revenge 3.ipa` in this folder, then open the converter:

| | |
|---|---|
| **Windows** | double-click `Convert on Windows.bat` |
| **macOS** | double-click `Convert on macOS.command` |
| anywhere | `python3 tools/converter_gui.py` |

A window opens, finds the `.ipa` (in this folder, or on your Desktop or in
Downloads), checks that Pillow, ffmpeg and Lua are installed, and tells you
how to get any that are missing. Press **Convert** and watch the bar; it ends
with `Finished: 973 MB, 1621 files in app_data.`

On macOS the first double-click may be refused as "unidentified developer" —
right-click the file, choose Open, then Open again.

Or do the same thing from a terminal:

```sh
python3 tools/prep_assets.py "/path/to/Tap Tap Revenge 3.ipa" app_data
```

Either way you end up with an `app_data/` folder of about 973 MB and 1621
files:

```
app_data/songs.txt     the song list
app_data/charts/       343 note charts
app_data/audio/        343 songs as .ogg
app_data/themes/       120 themes
app_data/gfx/  sfx/    menu graphics, font and two sounds
app_data/icon0.png     the home screen icon, taken from the game
```

It can be interrupted and run again: songs already converted are kept. A song
whose audio will not decode is skipped and named at the end.

### Step 2 — build the VPK (about a minute)

```sh
cmake -S . -B build
cmake --build build
```

This produces **`build/ttrvita.vpk`**, about 1 MB. It holds only the program:
the songs are copied over separately in step 4, because a VPK with them
inside takes VitaShell well over an hour to unpack.

### Step 3 — install the VPK

Copy `build/ttrvita.vpk` to the Vita (USB or FTP), open it in VitaShell and
press X. It installs in a few seconds. If you are replacing an earlier build,
delete the app from the home screen first, as the Vita caches its LiveArea
page.

### Step 4 — copy the songs (once, a few minutes)

In VitaShell press SELECT for USB mode, then copy the whole `app_data` folder
onto the memory card at `ux0:data/ttrvita/`, so that you end up with:

```
ux0:data/ttrvita/app_data/songs.txt
ux0:data/ttrvita/app_data/audio/   charts/   themes/   gfx/   sfx/
```

The doubled name is right: `ux0:data` is the Vita's own folder and `app_data`
is this game's. Launch the game from the home screen.

Updating the program later is just a new VPK; the songs stay where they are.

## Options

```sh
python3 tools/prep_assets.py game.ipa app_data --limit 20   # only the first 20 songs
python3 tools/prep_assets.py game.ipa app_data --jobs 4     # how many songs to convert at once
python3 tools/prep_assets.py game.ipa app_data --no-audio   # charts and themes only, no songs
cmake -S . -B build -DEMBED_DATA=ON                         # songs inside the VPK (very slow to install)
```

### Running it on the computer instead

Useful for trying a change without a Vita. Needs SDL2, libpng and libvorbis
(`brew install sdl2 libpng libvorbis`), and uses the mouse and keyboard
columns of the controls table above.

```sh
cmake -S . -B build-pc -DDESKTOP=ON
cmake --build build-pc
TTR_DATA=app_data/ ./build-pc/ttrvita
```

## If something goes wrong

- **`VITASDK is not set`** — run the two `export` lines from above.
- **VitaShell error `0x8010113D`** — a LiveArea image is not an 8-bit indexed
  PNG. Everything in `sce_sys/` and `app_data/icon0.png` has to be saved that
  way (what `pngquant` does); the converter and the files here already are.
- **The game starts and quits** — it could not find `songs.txt`. Check the
  path in step 4, including the doubled `app_data`.
- **Undefined `sce...` symbols when linking** — your SDL2 package wants
  another stub library; add the matching `Sce..._stub` in `CMakeLists.txt`.
- **Notes feel early or late against the music** — change `AUDIO_LATENCY` in
  `src/game.c` (seconds; larger makes notes arrive later).
- **Shaking needs too much force, or triggers on its own** — change `SHAKE_G`
  near the top of `src/plat_sdl.c` (lower is more sensitive).

## Settings and scores

Both live in `ux0:data/ttrvita/settings.txt` (`ttrvita_settings.txt` in the PC
build): the screen layout, and your best run of each song at each difficulty.
"Best" is your highest-scoring run, with that run's streak and accuracy, and
it is written as soon as you beat it. Rebuilding `app_data/` with a different
set of songs renumbers them, which drops scores for songs no longer there.

## Tests

```sh
test/run_tests.sh app_data   # needs a C compiler and libpng headers
```

An auto-player hits every note of one song and the test checks the count,
streak and score; a second song is left untouched and must score zero. It also
covers pause, the vertical layout and the song list, with AddressSanitizer on.

## Layout

```
src/game.c       menus, gameplay, scoring, HUD (logical 320x480 points)
src/theme.c      loads and plays a song's theme (scenery, lanes, reactions)
src/plat.h       platform interface
src/plat_sdl.c   SDL2 backend: touch, buttons, Ogg music stream, WAV effects
src/png_load.c   PNG decoding via libpng
tools/converter_gui.py a window around prep_assets.py (the double-click apps)
tools/prep_assets.py   .ipa -> app_data/ (songs.txt, charts, audio, themes, gfx, sfx)
tools/theme_dump.lua   runs one theme script and logs what it sets up
tools/theme_compile.py turns that log into app_data/themes/<name>/theme.txt
tools/pvrtc.py         decoder for PVRTC-compressed textures
test/            headless renderer and scripted play-through
sce_sys/         LiveArea files: wallpaper, start-button image and template.xml
                 (the home screen icon comes from app_data/icon0.png). All of
                 these must be 8-bit indexed PNGs
```

## Note

The songs and art are commercial, licensed material. Keep `app_data/` and any VPK
built from it to yourself and your class; share only the source code.
