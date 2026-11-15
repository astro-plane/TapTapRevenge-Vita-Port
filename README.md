# Tap Tap – PS Vita reimplementation

A from-scratch C reimplementation of the rhythm game's single-player mode for
the PS Vita. It does not run or translate the iOS program. It plays the
original note charts, music and per-song themes, which `tools/prep_assets.py`
converts from your own copy of the `.ipa` into `data/`.

**Status:** game logic and drawing are tested on a PC with a headless renderer
(see *Tests*). The Vita/SDL2 layer (`src/plat_sdl.c`), including music
streaming, **has not been compiled with VitaSDK or run on a Vita yet.**

## What is in the game

- Song list (343 tracks), difficulty select, results screen
- Each song's own theme (119 of them): background, lane shapes, note art,
  note speed, moving/pulsing scenery, and the colour and light changes on
  streaks, multipliers and misses
- Three lanes with tap notes, hold notes and shake arrows, timed to the music
  (four lanes in the one theme that has them)
- Score, streak, 2x/3x/4x multipliers, and an 8x "revenge" mode
- Landscape and vertical layouts, pause

Not included: everything online (store, leaderboards, battles, chat), two-player
mode, weapons/items, the tutorial, album art, and saved high scores. The menus
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
  script-made hit sparkles to copy; the hit splash is the theme's splash image
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
| Up / Down, L / R | arrows | move in the song list (L/R jump 8) |
| Cross / Circle | Enter / Esc | choose / back |

In the vertical layout, hold the Vita turned a quarter turn clockwise.

## Build

Needs VitaSDK with these packages: `vdpm install sdl2 libpng zlib libvorbis libogg`

```sh
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
cmake -S . -B build
cmake --build build          # -> build/ttrvita.vpk
```

### Putting it on the Vita

The VPK holds only the program (a few hundred kB) and installs in seconds.
The songs are copied across separately, once:

1. Install `build/ttrvita.vpk` with VitaShell.
2. Connect the Vita over USB (in VitaShell, press SELECT) and copy the whole
   `data` folder to `ux0:data/ttrvita/`, so that the game finds
   `ux0:data/ttrvita/data/songs.txt`.

Copying about 1 GB over USB takes a few minutes. Updating the program later is
just a new VPK; the songs stay where they are. To change which songs are on
the Vita, rebuild `data/` and copy it over again.

For a smaller set of songs, regenerate `data/` with `--limit N`.

`cmake -S . -B build -DEMBED_DATA=ON` packs the songs into the VPK instead.
That makes a ~1 GB VPK which VitaShell needs well over an hour to unpack, so
it is off by default.

To regenerate `data/` (needs Python 3, Pillow, ffmpeg and a Lua 5.3/5.4
interpreter such as `lua` or `texlua`; about 15 minutes):

```sh
python3 tools/prep_assets.py "/path/to/game.ipa" data
```

The script can be stopped and rerun; finished songs are kept. Audio that does
not decode as-is is skipped; nothing is decrypted.

Shake sensitivity is `SHAKE_G` near the top of `src/plat_sdl.c` (lower = a
lighter shake registers).

If notes feel early or late against the music on the Vita, change
`AUDIO_LATENCY` in `src/game.c` (seconds; larger = notes arrive later).

## Tests

```sh
test/run_tests.sh data        # needs a C compiler and libpng headers
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
tools/prep_assets.py   .ipa -> data/ (songs.txt, charts, audio, themes, gfx, sfx)
tools/theme_dump.lua   runs one theme script and logs what it sets up
tools/theme_compile.py turns that log into data/themes/<name>/theme.txt
tools/pvrtc.py         decoder for PVRTC-compressed textures
test/            headless renderer and scripted play-through
sce_sys/         placeholder LiveArea images (the home screen icon comes from
                 data/icon0.png when prep_assets.py has made it)
```

## Note

The songs and art are commercial, licensed material. Keep `data/` and any VPK
built from it to yourself and your class; share only the source code.
