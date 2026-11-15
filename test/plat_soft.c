/* plat_soft.c - headless implementation of plat.h for automated tests.
 * Software renderer into a 960x544 buffer; the "music" is a clock that the
 * test advances, so note timing can be checked exactly. */
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plat.h"

#define W 960
#define H 544

struct PlatTex { unsigned char *px; int w, h; };
static unsigned char fb[W * H * 4];
int soft_render = 0, soft_live_tex = 0;
double soft_time = 0, soft_music = -1;     /* -1 = no music playing */
int soft_music_paused = 0;
char soft_log[2048];

int plat_init(int *w, int *h) { *w = W; *h = H; return 0; }
void plat_shutdown(void) {}
void plat_poll(PlatInput *in) { (void)in; }
double plat_time(void) { return soft_time; }
const char *plat_data_dir(void) { const char *e = getenv("TTR_DATA"); return e ? e : "data/"; }
const char *plat_save_file(void) { return "/tmp/ttrvita_test_settings.txt"; }

PlatTex *plat_tex_create(const unsigned char *rgba, int w, int h)
{
    PlatTex *t = malloc(sizeof *t);
    t->px = malloc((size_t)w * h * 4);
    memcpy(t->px, rgba, (size_t)w * h * 4);
    t->w = w; t->h = h;
    soft_live_tex++;
    return t;
}
void plat_tex_free(PlatTex *t) { if (t) { free(t->px); free(t); soft_live_tex--; } }

void plat_begin(void)
{
    int i;
    if (!soft_render) return;
    for (i = 0; i < W * H; i++) { fb[i*4] = 10; fb[i*4+1] = 10; fb[i*4+2] = 12; fb[i*4+3] = 255; }
}
void plat_end(void) {}

static void put(int x, int y, float r, float g, float b, float a, int add)
{
    unsigned char *d;
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    d = &fb[(y * W + x) * 4];
    if (add) {
        float v;
        v = d[0] + r * a; d[0] = (unsigned char)(v > 255 ? 255 : v);
        v = d[1] + g * a; d[1] = (unsigned char)(v > 255 ? 255 : v);
        v = d[2] + b * a; d[2] = (unsigned char)(v > 255 ? 255 : v);
    } else {
        d[0] = (unsigned char)(d[0] + (r - d[0]) * a);
        d[1] = (unsigned char)(d[1] + (g - d[1]) * a);
        d[2] = (unsigned char)(d[2] + (b - d[2]) * a);
    }
}

void plat_draw(PlatTex *t, int sx, int sy, int sw, int sh, float cx, float cy,
               float w, float h, float angle_deg, unsigned rgba, int blend)
{
    float rad = angle_deg * 3.14159265f / 180.0f, c = cosf(rad), s = sinf(rad);
    float ex = (fabsf(w * c) + fabsf(h * s)) / 2, ey = (fabsf(w * s) + fabsf(h * c)) / 2;
    float mr = (rgba >> 24) / 255.0f, mg = ((rgba >> 16) & 255) / 255.0f, mb = ((rgba >> 8) & 255) / 255.0f, ma = (rgba & 255) / 255.0f;
    int x0 = (int)floorf(cx - ex), x1 = (int)ceilf(cx + ex), y0 = (int)floorf(cy - ey), y1 = (int)ceilf(cy + ey), x, y;
    if (!soft_render || !t || w <= 0 || h <= 0) return;
    if (sx < 0 || sy < 0 || sx + sw > t->w || sy + sh > t->h) { fprintf(stderr, "SRC RECT OUT OF TEXTURE %d,%d %dx%d in %dx%d\n", sx, sy, sw, sh, t->w, t->h); return; }
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float u = (dx * c + dy * s) / w + 0.5f, v = (-dx * s + dy * c) / h + 0.5f;
            const unsigned char *p;
            if (u < 0 || v < 0 || u >= 1 || v >= 1) continue;
            p = &t->px[((sy + (int)(v * sh)) * t->w + sx + (int)(u * sw)) * 4];
            put(x, y, p[0] * mr, p[1] * mg, p[2] * mb, blend == 2 ? 1.0f : p[3] / 255.0f * ma, blend == 1);
        }
}

void plat_fill(float x, float y, float w, float h, unsigned rgba)
{
    int xi, yi;
    if (!soft_render) return;
    for (yi = (int)y; yi < (int)(y + h); yi++)
        for (xi = (int)x; xi < (int)(x + w); xi++)
            put(xi, yi, (float)(rgba >> 24), (float)((rgba >> 16) & 255), (float)((rgba >> 8) & 255), (rgba & 255) / 255.0f, 0);
}

void plat_sfx(const char *name)
{
    if (strlen(soft_log) + strlen(name) + 2 < sizeof soft_log) { strcat(soft_log, name); strcat(soft_log, " "); }
}

int plat_music_play(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "music missing: %s\n", path); return -1; }
    fclose(f);
    soft_music = 0;
    soft_music_paused = 0;
    return 0;
}
void plat_music_stop(void) { soft_music = -1; }
void plat_music_pause(int p) { soft_music_paused = p; }
double plat_music_time(void) { return soft_music < 0 ? 0 : soft_music; }
int plat_music_done(void) { return soft_music < 0; }

void soft_dump(const char *path)
{
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    img.width = W; img.height = H; img.format = PNG_FORMAT_RGBA;
    png_image_write_to_file(&img, path, 0, fb, 0, NULL);
}
