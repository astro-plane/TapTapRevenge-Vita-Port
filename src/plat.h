/* plat.h - the platform layer the game is written against.
 * plat_sdl.c: SDL2 + libvorbis (PS Vita and desktop).
 * test/plat_soft.c: headless software renderer for automated tests. */
#ifndef PLAT_H
#define PLAT_H

typedef struct PlatTex PlatTex;

enum {
    BTN_UP = 1, BTN_DOWN = 2, BTN_LEFT = 4, BTN_RIGHT = 8,
    BTN_TRIANGLE = 16, BTN_CIRCLE = 32, BTN_CROSS = 64, BTN_SQUARE = 128,
    BTN_L = 256, BTN_R = 512, BTN_SELECT = 1024, BTN_START = 2048,
    BTN_SHAKE = 4096                         /* the console was shaken (pressed only) */
};

#define MAX_TOUCH 6

typedef struct {
    int n;                                   /* fingers on the screen right now */
    struct { long long id; float x, y; } t[MAX_TOUCH];   /* screen pixels   */
    int held;                                /* BTN_* currently down            */
    int pressed;                             /* BTN_* that went down this frame */
    int quit;
} PlatInput;

int plat_init(int *screen_w, int *screen_h);
void plat_shutdown(void);
void plat_poll(PlatInput *in);
double plat_time(void);
const char *plat_data_dir(void);             /* ends with '/' */
const char *plat_save_file(void);

PlatTex *plat_tex_create(const unsigned char *rgba, int w, int h);
void plat_tex_free(PlatTex *t);
void plat_begin(void);
void plat_end(void);
/* Draw the sub-rectangle (sx,sy,sw,sh) of a texture centred on (cx,cy) with
 * size (w,h) in screen pixels, rotated clockwise.  rgba is 0xRRGGBBAA.
 * blend: 0 = normal alpha, 1 = additive, 2 = opaque (no blending). */
void plat_draw(PlatTex *t, int sx, int sy, int sw, int sh, float cx, float cy,
               float w, float h, float angle_deg, unsigned rgba, int blend);
void plat_fill(float x, float y, float w, float h, unsigned rgba);

void plat_sfx(const char *name);             /* app_data/sfx/<name>.wav */
int plat_music_play(const char *path);       /* Ogg Vorbis; 0 on success */
void plat_music_stop(void);
void plat_music_pause(int paused);
double plat_music_time(void);                /* seconds of the song heard so far */
int plat_music_done(void);

unsigned char *png_load_rgba(const char *path, int *w, int *h);   /* png_load.c */

#endif
