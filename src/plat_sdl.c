/* plat_sdl.c - SDL2 implementation of plat.h for the PS Vita (VitaSDK's SDL2
 * port) and for desktop builds.  Music is streamed from Ogg Vorbis with
 * libvorbisfile inside the audio callback; sound effects are WAV files.
 *
 * The song clock (plat_music_time) counts the samples handed to the audio
 * device and interpolates between callbacks, so notes stay locked to the
 * music even if the frame rate drops. */
#include <SDL.h>
#include <vorbis/vorbisfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#ifdef __vita__
#include <psp2/motion.h>
#endif
#include "plat.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define MIX_FREQ 44100
#define MAX_SOUNDS 16
#define MAX_VOICES 6

/* Shake detection (Vita motion sensor).  SHAKE_G is the sudden change in
 * acceleration, in g, that counts as a shake: lower = more sensitive.
 * Around 0.15 a light flick is enough; 0.5 needs a firm shake. */
#define SHAKE_G 0.18f
#define SHAKE_COOLDOWN 0.15          /* seconds before another shake can register */

struct PlatTex { SDL_Texture *t; };
typedef struct { char name[32]; Sint16 *pcm; Uint32 frames; int failed; } Sound;
typedef struct { const Sound *s; Uint32 pos; } Voice;

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_AudioDeviceID audio;
static SDL_Joystick *joy;
static int win_w = SCREEN_W, win_h = SCREEN_H;
static char data_dir[512], save_file[512];

static Sound sounds[MAX_SOUNDS];
static int n_sounds;
static Voice voices[MAX_VOICES];

/* music state, shared with the audio callback (guarded by SDL_LockAudioDevice) */
static OggVorbis_File vf;
static int music_open, music_paused, music_eof, music_channels = 2;
static long music_rate = MIX_FREQ;
static Uint64 music_frames;          /* song frames handed to the device */
static Uint64 cb_counter;            /* performance counter at the last callback */
static Sint16 dec_buf[4096 * 2];

/* touch */
typedef struct { SDL_FingerID id; float x, y; int on; } Finger;
static Finger fingers[MAX_TOUCH];
static int mouse_down;
static float mouse_x, mouse_y;
static int held_buttons;

/* ------------------------------------------------------------- audio -- */

static void mix_cb(void *ud, Uint8 *stream, int len)
{
    Sint16 *out = (Sint16 *)stream;
    int frames = len / 4, done = 0, v, i;
    (void)ud;
    memset(stream, 0, (size_t)len);

    if (music_open && !music_paused && !music_eof) {
        while (done < frames) {
            int want = frames - done, bs, got;
            long n;
            if (want > 4096) want = 4096;
            n = ov_read(&vf, (char *)dec_buf, want * music_channels * 2, 0, 2, 1, &bs);
            if (n <= 0) {
                if (n == 0) music_eof = 1;   /* end of song; a negative value is a hole, keep going */
                if (n == 0) break;
                continue;
            }
            got = (int)(n / (2 * music_channels));
            for (i = 0; i < got; i++) {
                Sint16 l = dec_buf[i * music_channels];
                Sint16 r = music_channels > 1 ? dec_buf[i * music_channels + 1] : l;
                out[(done + i) * 2] = l;
                out[(done + i) * 2 + 1] = r;
            }
            done += got;
        }
        music_frames += (Uint64)done;
    }
    cb_counter = SDL_GetPerformanceCounter();

    for (v = 0; v < MAX_VOICES; v++) {
        Voice *vo = &voices[v];
        if (!vo->s)
            continue;
        for (i = 0; i < frames && vo->pos < vo->s->frames; i++, vo->pos++) {
            int s = vo->s->pcm[vo->pos] / 2;
            int l = out[i * 2] + s, r = out[i * 2 + 1] + s;
            out[i * 2] = (Sint16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            out[i * 2 + 1] = (Sint16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        if (vo->pos >= vo->s->frames)
            vo->s = NULL;
    }
}

int plat_music_play(const char *path)
{
    OggVorbis_File nv;
    vorbis_info *vi;
    plat_music_stop();
    if (ov_fopen(path, &nv) != 0) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    vi = ov_info(&nv, -1);
    if (!vi || vi->rate != MIX_FREQ || vi->channels < 1 || vi->channels > 2) {
        fprintf(stderr, "%s: needs 44.1 kHz mono/stereo (rerun prep_assets.py)\n", path);
        ov_clear(&nv);
        return -1;
    }
    SDL_LockAudioDevice(audio);
    vf = nv;
    music_channels = vi->channels;
    music_rate = vi->rate;
    music_frames = 0;
    music_eof = 0;
    music_paused = 0;
    cb_counter = SDL_GetPerformanceCounter();
    music_open = 1;
    SDL_UnlockAudioDevice(audio);
    return 0;
}

void plat_music_stop(void)
{
    if (!audio || !music_open)
        return;
    SDL_LockAudioDevice(audio);
    music_open = 0;
    ov_clear(&vf);
    SDL_UnlockAudioDevice(audio);
}

void plat_music_pause(int p)
{
    SDL_LockAudioDevice(audio);
    music_paused = p;
    SDL_UnlockAudioDevice(audio);
}

double plat_music_time(void)
{
    double t, since;
    Uint64 frames, cb;
    int running;
    SDL_LockAudioDevice(audio);
    frames = music_frames;
    cb = cb_counter;
    running = music_open && !music_paused && !music_eof;
    SDL_UnlockAudioDevice(audio);
    t = (double)frames / (double)music_rate;
    /* the last buffer handed over is still being played: walk forward in it */
    since = (double)(SDL_GetPerformanceCounter() - cb) / (double)SDL_GetPerformanceFrequency();
    if (running) {
        if (since > 0.05) since = 0.05;
        t += since;
    }
    return t - 1024.0 / MIX_FREQ;        /* minus one device buffer still queued */
}

int plat_music_done(void)
{
    int d;
    SDL_LockAudioDevice(audio);
    d = !music_open || music_eof;
    SDL_UnlockAudioDevice(audio);
    return d;
}

static Sound *sound_get(const char *name)
{
    char path[640];
    SDL_AudioSpec spec;
    SDL_AudioCVT cvt;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    Sound *s;
    int i;
    for (i = 0; i < n_sounds; i++)
        if (!strcmp(sounds[i].name, name))
            return sounds[i].failed ? NULL : &sounds[i];
    if (n_sounds == MAX_SOUNDS)
        return NULL;
    s = &sounds[n_sounds++];
    SDL_snprintf(s->name, sizeof s->name, "%s", name);
    s->failed = 1;
    SDL_snprintf(path, sizeof path, "%ssfx/%s.wav", data_dir, name);
    if (!SDL_LoadWAV(path, &spec, &buf, &len))
        return NULL;
    if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, 1, MIX_FREQ) < 0) {
        SDL_FreeWAV(buf);
        return NULL;
    }
    cvt.len = (int)len;
    cvt.buf = SDL_malloc((size_t)len * (size_t)cvt.len_mult + 16);
    if (!cvt.buf) {
        SDL_FreeWAV(buf);
        return NULL;
    }
    memcpy(cvt.buf, buf, len);
    SDL_FreeWAV(buf);
    if (cvt.needed && SDL_ConvertAudio(&cvt) < 0) {
        SDL_free(cvt.buf);
        return NULL;
    }
    s->pcm = (Sint16 *)cvt.buf;
    s->frames = (Uint32)(cvt.needed ? cvt.len_cvt : (int)len) / 2;
    s->failed = 0;
    return s;
}

void plat_sfx(const char *name)
{
    Sound *s;
    int v, pick = 0;
    if (!audio || !name || !(s = sound_get(name)))
        return;
    SDL_LockAudioDevice(audio);
    for (v = 0; v < MAX_VOICES; v++)
        if (!voices[v].s) { pick = v; break; }
    voices[pick].s = s;
    voices[pick].pos = 0;
    SDL_UnlockAudioDevice(audio);
}

/* -------------------------------------------------------------- video -- */

int plat_init(int *screen_w, int *screen_h)
{
    SDL_AudioSpec want, have;
    const char *env;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
#ifdef __vita__
    /* The songs live on the memory card rather than inside the app: a VPK
     * holding ~1 GB takes VitaShell well over an hour to unpack, while
     * copying the folder over USB takes a few minutes.  app0: is still used
     * when the data was built into the VPK (cmake -DEMBED_DATA=ON). */
    mkdir("ux0:data", 0777);
    mkdir("ux0:data/ttrvita", 0777);
    SDL_snprintf(save_file, sizeof save_file, "ux0:data/ttrvita/settings.txt");
    {
        static const char *where[2] = { "ux0:data/ttrvita/app_data/", "app0:app_data/" };
        char probe[640];
        int i;
        SDL_snprintf(data_dir, sizeof data_dir, "%s", where[0]);
        for (i = 0; i < 2; i++) {
            FILE *f;
            SDL_snprintf(probe, sizeof probe, "%ssongs.txt", where[i]);
            f = fopen(probe, "r");
            if (f) {
                fclose(f);
                SDL_snprintf(data_dir, sizeof data_dir, "%s", where[i]);
                break;
            }
        }
        if (i == 2)
            fprintf(stderr, "no songs.txt found - copy the app_data folder to ux0:data/ttrvita/app_data/\n");
    }
    (void)env;
#else
    env = getenv("TTR_DATA");
    SDL_snprintf(data_dir, sizeof data_dir, "%s", env && env[0] ? env : "app_data/");
    if (data_dir[0] && data_dir[strlen(data_dir) - 1] != '/')
        SDL_strlcat(data_dir, "/", sizeof data_dir);
    SDL_snprintf(save_file, sizeof save_file, "ttrvita_settings.txt");
#endif
    /* keep touches as touches: no synthetic mouse events */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    win = SDL_CreateWindow("Tap Tap (Vita reimplementation)", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_GetRendererOutputSize(ren, &win_w, &win_h);
    *screen_w = win_w;
    *screen_h = win_h;
    if (SDL_NumJoysticks() > 0)
        joy = SDL_JoystickOpen(0);

    SDL_zero(want);
    want.freq = MIX_FREQ;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix_cb;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio) {
        fprintf(stderr, "no audio device: %s\n", SDL_GetError());
        return -1;                   /* the game is timed by the music */
    }
    SDL_PauseAudioDevice(audio, 0);
#ifdef __vita__
    sceMotionStartSampling();
#endif
    return 0;
}

void plat_shutdown(void)
{
    int i;
#ifdef __vita__
    sceMotionStopSampling();
#endif
    plat_music_stop();
    if (audio)
        SDL_CloseAudioDevice(audio);
    for (i = 0; i < n_sounds; i++)
        SDL_free(sounds[i].pcm);
    if (joy)
        SDL_JoystickClose(joy);
    if (ren)
        SDL_DestroyRenderer(ren);
    if (win)
        SDL_DestroyWindow(win);
    SDL_Quit();
}

static int finger_slot(SDL_FingerID id, int make)
{
    int i;
    for (i = 0; i < MAX_TOUCH; i++)
        if (fingers[i].on && fingers[i].id == id)
            return i;
    if (make)
        for (i = 0; i < MAX_TOUCH; i++)
            if (!fingers[i].on) {
                fingers[i].on = 1;
                fingers[i].id = id;
                return i;
            }
    return -1;
}

#ifdef __vita__
/* 1 when the console has just been jolted.  Gravity and slow tilting are
 * removed with a running average, so only quick movement counts. */
static int shake_poll(void)
{
    static float avg[3];
    static int primed;
    static double last;
    SceMotionState st;
    float a[3], d = 0;
    double now;
    int i;
    if (sceMotionGetState(&st) < 0)
        return 0;
    a[0] = st.acceleration.x; a[1] = st.acceleration.y; a[2] = st.acceleration.z;
    if (!primed) {
        memcpy(avg, a, sizeof avg);
        primed = 1;
        return 0;
    }
    for (i = 0; i < 3; i++) {
        float hp = a[i] - avg[i];
        avg[i] += hp * 0.15f;
        d += hp * hp;
    }
    now = plat_time();
    if (sqrtf(d) > SHAKE_G && now - last > SHAKE_COOLDOWN) {
        last = now;
        return 1;
    }
    return 0;
}
#endif

/* Vita buttons as numbered by SDL's Vita joystick driver */
static int joy_bit(int b)
{
    static const int map[12] = { BTN_TRIANGLE, BTN_CIRCLE, BTN_CROSS, BTN_SQUARE, BTN_L, BTN_R,
                                 BTN_DOWN, BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_SELECT, BTN_START };
    return b >= 0 && b < 12 ? map[b] : 0;
}

static int key_bit(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: case SDLK_a: return BTN_LEFT;
    case SDLK_RIGHT: case SDLK_d: return BTN_RIGHT;
    case SDLK_s: return BTN_CROSS;          /* middle lane on the keyboard */
    case SDLK_RETURN: case SDLK_SPACE: return BTN_CROSS;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return BTN_CIRCLE;
    case SDLK_q: return BTN_L;
    case SDLK_e: return BTN_R;
    case SDLK_r: return BTN_SELECT;
    case SDLK_p: return BTN_START;
    case SDLK_w: return BTN_SHAKE;
    default: return 0;
    }
}

void plat_poll(PlatInput *in)
{
    SDL_Event e;
    int i, before = held_buttons, slot;
    in->quit = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            in->quit = 1;
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
            /* front screen only: the rear touch pad is an indirect device */
            if (SDL_GetTouchDeviceType(e.tfinger.touchId) != SDL_TOUCH_DEVICE_DIRECT)
                break;
            slot = finger_slot(e.tfinger.fingerId, e.type == SDL_FINGERDOWN);
            if (slot >= 0) {
                fingers[slot].x = e.tfinger.x * win_w;
                fingers[slot].y = e.tfinger.y * win_h;
            }
            break;
        case SDL_FINGERUP:
            slot = finger_slot(e.tfinger.fingerId, 0);
            if (slot >= 0)
                fingers[slot].on = 0;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) {
                mouse_down = e.type == SDL_MOUSEBUTTONDOWN;
                mouse_x = (float)e.button.x;
                mouse_y = (float)e.button.y;
            }
            break;
        case SDL_MOUSEMOTION:
            if (e.motion.which != SDL_TOUCH_MOUSEID) {
                mouse_x = (float)e.motion.x;
                mouse_y = (float)e.motion.y;
            }
            break;
        case SDL_KEYDOWN:
            if (!e.key.repeat)
                held_buttons |= key_bit(e.key.keysym.sym);
            break;
        case SDL_KEYUP:
            held_buttons &= ~key_bit(e.key.keysym.sym);
            break;
        case SDL_JOYBUTTONDOWN:
            held_buttons |= joy_bit(e.jbutton.button);
            break;
        case SDL_JOYBUTTONUP:
            held_buttons &= ~joy_bit(e.jbutton.button);
            break;
        default:
            break;
        }
    }
    in->n = 0;
    for (i = 0; i < MAX_TOUCH; i++)
        if (fingers[i].on) {
            in->t[in->n].id = (long long)fingers[i].id;
            in->t[in->n].x = fingers[i].x;
            in->t[in->n].y = fingers[i].y;
            in->n++;
        }
    if (mouse_down && in->n < MAX_TOUCH) {
        in->t[in->n].id = -1;
        in->t[in->n].x = mouse_x;
        in->t[in->n].y = mouse_y;
        in->n++;
    }
    in->held = held_buttons;
    in->pressed = held_buttons & ~before;
#ifdef __vita__
    if (shake_poll())
        in->pressed |= BTN_SHAKE;
#endif
}

double plat_time(void)
{
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

const char *plat_data_dir(void) { return data_dir; }
const char *plat_save_file(void) { return save_file; }

PlatTex *plat_tex_create(const unsigned char *rgba, int w, int h)
{
    PlatTex *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    t->t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t->t || SDL_UpdateTexture(t->t, NULL, rgba, w * 4) != 0) {
        if (t->t)
            SDL_DestroyTexture(t->t);
        free(t);
        return NULL;
    }
    return t;
}

void plat_tex_free(PlatTex *t)
{
    if (t) {
        SDL_DestroyTexture(t->t);
        free(t);
    }
}

void plat_begin(void)
{
    SDL_SetRenderDrawColor(ren, 10, 10, 12, 255);
    SDL_RenderClear(ren);
}

void plat_end(void)
{
    SDL_RenderPresent(ren);
}

void plat_draw(PlatTex *t, int sx, int sy, int sw, int sh, float cx, float cy,
               float w, float h, float angle_deg, unsigned rgba, int blend)
{
    SDL_Rect src;
    SDL_FRect dst;
    if (!t || w <= 0 || h <= 0)
        return;
    src.x = sx; src.y = sy; src.w = sw; src.h = sh;
    dst.x = cx - w / 2; dst.y = cy - h / 2; dst.w = w; dst.h = h;
    SDL_SetTextureBlendMode(t->t, blend == 1 ? SDL_BLENDMODE_ADD : blend == 2 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(t->t, (Uint8)(rgba >> 24), (Uint8)(rgba >> 16), (Uint8)(rgba >> 8));
    SDL_SetTextureAlphaMod(t->t, (Uint8)rgba);
    SDL_RenderCopyExF(ren, t->t, &src, &dst, angle_deg, NULL, SDL_FLIP_NONE);
}

void plat_fill(float x, float y, float w, float h, unsigned rgba)
{
    SDL_FRect rc;
    rc.x = x; rc.y = y; rc.w = w; rc.h = h;
    SDL_SetRenderDrawColor(ren, (Uint8)(rgba >> 24), (Uint8)(rgba >> 16), (Uint8)(rgba >> 8), (Uint8)rgba);
    SDL_RenderFillRectF(ren, &rc);
}
