/* theme.c - loads and plays a theme file written by tools/theme_compile.py.
 *
 * A theme is a flat description of what the original Lua theme script built:
 *   textures, "shaders" (1-2 texture passes with a blend mode; a pass can be
 *   tied to revenge mode, scroll, or flip through frames), background sprites
 *   with looping animations, six note lanes (three tap, three shake) with
 *   sampled position/scale/alpha curves, feedback colours, and the changes
 *   the script makes on a streak of 10, on each multiplier and on a miss.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "theme.h"

#define NS 33                   /* samples per curve */
#define MAX_TEX 64
#define MAX_FADES 48

typedef struct { int tex, x, y, w, h; } Frame;
typedef struct { int blend, bind, nframes; float scroll_x, scroll_y, freq; Frame *frames; } Pass;
typedef struct { int npass; Pass *pass; } Shader;
typedef struct {
    int shader, visible, group;
    float x, y, w, h, sx, sy, rot, rotvel, alpha, col[3];
    float spawn;                 /* seconds into the song before it appears */
    /* timed effects (sparkles, smoke): alive for 'life' seconds from spawn,
     * drifting and fading by the rates below.  life == 0 means permanent. */
    float life, fade, avel, vx, vy, ax, ay, svx, svy;
    int phase;                   /* -1: spawn counts from the start of the song;
                                    else from the last time that event fired */
} SpriteDef;
typedef struct { SpriteDef d; float col_to[3]; double base; } Sprite;     /* d = live values; base = clock its spawn counts from */
typedef struct { int sprite, chan, loop, done; float dur, delay, loop_delay, t, v[NS]; } Anim;
typedef struct { int lane, shake, tap, hold, hold_a, splash, color_sprite; float window, x[NS], y[NS], s[NS], a[NS]; } Column;
typedef struct { int phase, type, target; float delay, v[3]; double due; } Event;   /* due < 0: not waiting */
typedef struct { int sprite, on; float from, to, dur, t; } Fade;

static PlatTex *tex[MAX_TEX];
static int n_tex;
static Shader *shaders;
static int n_shaders;
static SpriteDef *defs;
static Sprite *sprites;
static int n_sprites, *order;
static Anim *anims;
static float *anim_dur0;
static int n_anims;
static Column *cols;
static int n_cols;
static Event *events;
static int n_events;
static float fb_col[2][5][3];
static int have_fb;
static Fade fades[MAX_FADES];
static double th_clock;
static int screen_w = 320, screen_h = 480;
static float revenge_pct;
static int revenge_now;

void theme_free(void)
{
    int i, j;
    for (i = 0; i < n_tex; i++)
        plat_tex_free(tex[i]);
    for (i = 0; i < n_shaders; i++) {
        for (j = 0; j < shaders[i].npass; j++)
            free(shaders[i].pass[j].frames);
        free(shaders[i].pass);
    }
    free(shaders); free(defs); free(sprites); free(order); free(anims); free(anim_dur0); free(cols); free(events);
    shaders = NULL; defs = NULL; sprites = NULL; order = NULL; anims = NULL; anim_dur0 = NULL; cols = NULL; events = NULL;
    n_tex = n_shaders = n_sprites = n_anims = n_cols = n_events = have_fb = 0;
    screen_w = 320;
    screen_h = 480;
}

/* read "count" floats from the rest of a line */
static int floats(char *p, float *out, int count)
{
    int n = 0;
    while (n < count) {
        char *end;
        float v = strtof(p, &end);
        if (end == p) break;
        out[n++] = v;
        p = end;
    }
    return n;
}

#define BAD do { fprintf(stderr, "theme file line %d not understood: %.60s\n", lineno, line); return -1; } while (0)
static int load_file(const char *dir, FILE *fp)
{
    char line[1024], word[32], path[640];
    int lineno = 0, ver = 0, si = -1, pi = -1, fi = 0, sp = 0, an = 0, co = -1, ev = 0, ti = 0, count, i;

    while (fgets(line, sizeof line, fp)) {
        char *rest;
        lineno++;
        int used = 0;
        if (sscanf(line, "%31s%n", word, &used) != 1)
            continue;
        rest = line + used;
        count = atoi(rest);
        if (!strcmp(word, "ttrtheme")) ver = count;
        else if (!strcmp(word, "screen")) {
            int w = 0, h = 0;
            if (sscanf(rest, "%d %d", &w, &h) == 2 && w >= 240 && w <= 960 && h >= 240 && h <= 960) {
                screen_w = w;
                screen_h = h;
            }
        }
        else if (!strcmp(word, "tex")) { if (count > MAX_TEX) BAD; }
        else if (!strcmp(word, "t")) {
            char name[128];
            unsigned char *px;
            int w, h;
            if (ti >= MAX_TEX || sscanf(rest, "%127s", name) != 1) BAD;
            snprintf(path, sizeof path, "%s%s", dir, name);
            px = png_load_rgba(path, &w, &h);
            if (!px) { fprintf(stderr, "theme image missing: %s\n", path); BAD; }
            tex[ti] = plat_tex_create(px, w, h);
            free(px);
            if (!tex[ti]) BAD;
            n_tex = ++ti;
        }
        else if (!strcmp(word, "shader")) {
            if (count < 1 || count > 4096 || shaders) BAD;
            shaders = calloc((size_t)count, sizeof *shaders);
            if (!shaders) BAD;
            n_shaders = count;
        }
        else if (!strcmp(word, "s")) {
            if (++si >= n_shaders || count < 1 || count > 4) BAD;
            shaders[si].pass = calloc((size_t)count, sizeof(Pass));
            if (!shaders[si].pass) BAD;
            shaders[si].npass = count;
            pi = -1;
        }
        else if (!strcmp(word, "p")) {
            Pass *p;
            if (si < 0 || ++pi >= shaders[si].npass) BAD;
            p = &shaders[si].pass[pi];
            if (sscanf(rest, "%d %d %f %f %f %d", &p->blend, &p->bind, &p->scroll_x, &p->scroll_y, &p->freq, &p->nframes) != 6 ||
                p->nframes < 1 || p->nframes > 64) BAD;
            p->frames = calloc((size_t)p->nframes, sizeof(Frame));
            if (!p->frames) BAD;
            fi = 0;
        }
        else if (!strcmp(word, "f")) {
            Pass *p;
            Frame *f;
            if (si < 0 || pi < 0) BAD;
            p = &shaders[si].pass[pi];
            if (fi >= p->nframes) BAD;
            f = &p->frames[fi++];
            if (sscanf(rest, "%d %d %d %d %d", &f->tex, &f->x, &f->y, &f->w, &f->h) != 5 || f->tex < 0 || f->tex >= n_tex)
                BAD;
        }
        else if (!strcmp(word, "sprite")) {
            if (count < 0 || count > 8192 || defs) BAD;
            defs = calloc((size_t)count + 1, sizeof *defs);
            sprites = calloc((size_t)count + 1, sizeof *sprites);
            order = calloc((size_t)count + 1, sizeof *order);
            if (!defs || !sprites || !order) BAD;
            n_sprites = count;
            for (i = 0; i < count; i++) order[i] = i;
        }
        else if (!strcmp(word, "o")) {
            SpriteDef *d;
            float v[12];
            char *p2 = rest, *end;
            if (sp >= n_sprites) BAD;
            d = &defs[sp++];
            d->shader = (int)strtol(p2, &end, 10);
            if (end == p2 || d->shader < 0 || d->shader >= n_shaders) BAD;
            p2 = end;
            if (floats(p2, v, 12) != 12) BAD;
            d->x = v[0]; d->y = v[1]; d->w = v[2]; d->h = v[3]; d->sx = v[4]; d->sy = v[5];
            d->rot = v[6]; d->rotvel = v[7]; d->alpha = v[8]; d->col[0] = v[9]; d->col[1] = v[10]; d->col[2] = v[11];
            /* the two trailing integers */
            for (i = 0; i < 12; i++) { strtof(p2, &end); p2 = end; }
            {
                float e[11];
                int off = 0;
                if (sscanf(p2, "%d %d%n", &d->visible, &d->group, &off) != 2 || floats(p2 + off, e, 11) != 11) BAD;
                d->phase = (int)e[10];
                if (d->phase >= TH_PHASES) BAD;
                d->spawn = e[0]; d->life = e[1]; d->fade = e[2]; d->avel = e[3]; d->vx = e[4]; d->vy = e[5];
                d->ax = e[6]; d->ay = e[7]; d->svx = e[8]; d->svy = e[9];
            }
        }
        else if (!strcmp(word, "order")) {
            char *p2 = rest, *end;
            for (i = 0; i < n_sprites; i++) {
                long v = strtol(p2, &end, 10);
                if (end == p2 || v < 0 || v >= n_sprites) break;
                order[i] = (int)v;
                p2 = end;
            }
        }
        else if (!strcmp(word, "anim")) {
            if (count < 0 || count > 32768 || anims) BAD;
            anims = calloc((size_t)count + 1, sizeof *anims);
            anim_dur0 = calloc((size_t)count + 1, sizeof *anim_dur0);
            if (!anims || !anim_dur0) BAD;
            n_anims = count;
        }
        else if (!strcmp(word, "a")) {
            Anim *a;
            int off = 0;
            if (an >= n_anims) BAD;
            a = &anims[an];
            if (sscanf(rest, "%d %d %d %f %f %f%n", &a->sprite, &a->chan, &a->loop, &a->dur, &a->delay,
                       &a->loop_delay, &off) != 6 ||
                a->sprite < 0 || a->sprite >= n_sprites || a->chan < 0 || a->chan > 5 || a->dur <= 0 ||
                floats(rest + off, a->v, NS) != NS) BAD;
            anim_dur0[an++] = a->dur;
        }
        else if (!strcmp(word, "column")) {
                    if (count < 3 || count > 16 || cols) BAD;
            cols = calloc((size_t)count, sizeof *cols);
            if (!cols) BAD;
            n_cols = count;
        }
        else if (!strcmp(word, "c")) {
            Column *c;
            if (++co >= n_cols) BAD;
            c = &cols[co];
            if (sscanf(rest, "%d %d %f %d %d %d %d %d", &c->lane, &c->shake, &c->window, &c->tap, &c->hold,
                       &c->hold_a, &c->splash, &c->color_sprite) != 8 || c->lane < 0 || c->lane > 3 ||
                c->tap >= n_shaders || c->hold >= n_shaders || c->hold_a >= n_shaders || c->splash >= n_shaders ||
                c->color_sprite >= n_sprites || c->window < 0.3f || c->window > 10) BAD;
        }
        else if (word[0] == 'c' && word[1] && strchr("xysa", word[1]) && word[2] == 0 && co >= 0 && co < n_cols) {
            Column *c = &cols[co];
            float *dst = word[1] == 'x' ? c->x : word[1] == 'y' ? c->y : word[1] == 's' ? c->s : c->a;
            if (floats(rest, dst, NS) != NS) BAD;
        }
        else if (!strcmp(word, "fb")) {
            float v[15];
            int state = 0, off = 0;
            if (sscanf(rest, "%d%n", &state, &off) == 1 && state >= 0 && state < 2 && floats(rest + off, v, 15) == 15) {
                memcpy(fb_col[state], v, sizeof v);
                have_fb = 1;
            }
        }
        else if (!strcmp(word, "event")) {
            if (count < 0 || count > 4096 || events) BAD;
            events = calloc((size_t)count + 1, sizeof *events);
            if (!events) BAD;
            n_events = count;
        }
        else if (!strcmp(word, "e")) {
            Event *e;
            int off = 0;
            if (ev >= n_events) BAD;
            e = &events[ev];
            {
                float v[4] = { 0, 0, 0, 0 };
                if (sscanf(rest, "%d %d %d%n", &e->phase, &e->type, &e->target, &off) != 3) BAD;
                floats(rest + off, v, 4);
                e->delay = v[0] > 0 ? v[0] : 0;
                e->v[0] = v[1]; e->v[1] = v[2]; e->v[2] = v[3];
                e->due = -1;
            }
            if (e->phase < 0 || e->phase >= TH_PHASES || e->target < 0 ||
                e->target >= (e->type == 2 ? n_anims : n_sprites)) { e->type = -1; }
            ev++;
        }
    }
    n_events = ev;
    n_anims = an;
    n_sprites = sp;
    n_cols = co + 1;
    if (ver != 5 || n_shaders < 1 || si + 1 != n_shaders || n_cols < 3)
        return -1;
    for (i = 0; i < n_shaders; i++) {
        int j;
        if (!shaders[i].pass) return -1;
        for (j = 0; j < shaders[i].npass; j++)
            if (!shaders[i].pass[j].frames) return -1;
    }
    /* need a tap lane for each of the three positions */
    for (i = 0; i < 3; i++) {
        int j, ok = 0;
        for (j = 0; j < n_cols; j++)
            if (cols[j].lane == i && !cols[j].shake && cols[j].tap >= 0) ok = 1;
        if (!ok) return -1;
    }
    return 0;
}

int theme_load(const char *name, int level)
{
    char dir[512], path[640];
    FILE *fp;
    int r;
    theme_free();
    snprintf(dir, sizeof dir, "%sthemes/%s/", plat_data_dir(), name);
    snprintf(path, sizeof path, "%stheme%d.txt", dir, level);
    fp = fopen(path, "r");
    if (!fp) {
        snprintf(path, sizeof path, "%stheme.txt", dir);
        fp = fopen(path, "r");
    }
    if (!fp)
        return -1;
    r = load_file(dir, fp);
    fclose(fp);
    if (r != 0) {
        fprintf(stderr, "bad theme file %s\n", path);
        theme_free();
        return -1;
    }
    theme_reset();
    return 0;
}

void theme_reset(void)
{
    int i;
    for (i = 0; i < n_sprites; i++) {
        sprites[i].d = defs[i];
        sprites[i].base = defs[i].phase < 0 ? 0 : -1e9;     /* event effects wait for their event */
        memcpy(sprites[i].col_to, defs[i].col, sizeof defs[i].col);
    }
    for (i = 0; i < n_anims; i++) {
        anims[i].t = 0;
        anims[i].done = 0;
        anims[i].dur = anim_dur0[i];
    }
    for (i = 0; i < n_events; i++)
        events[i].due = -1;
    memset(fades, 0, sizeof fades);
    th_clock = 0;
    revenge_pct = 0;
    revenge_now = 0;
}

static float curve(const float *v, float p)
{
    float f;
    int i;
    if (p <= 0) return v[0];
    if (p >= 1) return v[NS - 1];
    f = p * (NS - 1);
    i = (int)f;
    return v[i] + (v[i + 1] - v[i]) * (f - i);
}

static void apply_event(const Event *e);

void theme_update(double dt, int revenge_on)
{
    int i;
    float k = (float)dt;
    th_clock += dt;
    revenge_now = revenge_on;
    revenge_pct += (revenge_on ? 1 : -1) * k / 0.4f;
    if (revenge_pct < 0) revenge_pct = 0;
    if (revenge_pct > 1) revenge_pct = 1;

    for (i = 0; i < n_sprites; i++) {
        Sprite *s = &sprites[i];
        const SpriteDef *d0 = &defs[i];
        float a = k / 0.15f;
        int c;
        if (d0->life > 0) {                     /* a timed effect: where is it in its life? */
            float age = (float)(th_clock - s->base) - d0->spawn, al;
            if (age < 0 || age > d0->life) {
                s->d.visible = 0;
                continue;
            }
            al = d0->alpha + d0->avel * age;
            al = al < 0 ? 0 : al > 1 ? 1 : al;
            if (d0->fade > 0 && age > d0->life - d0->fade)
                al *= (d0->life - age) / d0->fade;
            s->d.visible = d0->visible;
            s->d.alpha = al;
            s->d.x = d0->x + d0->vx * age + 0.5f * d0->ax * age * age;
            s->d.y = d0->y + d0->vy * age + 0.5f * d0->ay * age * age;
            s->d.sx = d0->sx + d0->svx * age;
            s->d.sy = d0->sy + d0->svy * age;
            if (s->d.sx < 0) s->d.sx = 0;
            if (s->d.sy < 0) s->d.sy = 0;
            s->d.rot = d0->rot + d0->rotvel * age;
            continue;
        }
        if (a > 1) a = 1;
        s->d.rot += s->d.rotvel * k;
        for (c = 0; c < 3; c++)
            s->d.col[c] += (s->col_to[c] - s->d.col[c]) * a;
    }
    for (i = 0; i < n_anims; i++) {
        Anim *a = &anims[i];
        SpriteDef *d = &sprites[a->sprite].d;
        float v, tt;
        if (a->done)
            continue;
        a->t += k;
        if (defs[a->sprite].life > 0) {
            if (!sprites[a->sprite].d.visible)
                continue;                       /* its timed sprite is not on screen */
            tt = (float)(th_clock - sprites[a->sprite].base) - a->delay;   /* runs on the sprite's own clock */
        } else {
            tt = a->t - a->delay;
        }
        if (tt < 0)
            continue;                           /* not started yet */
        if (a->loop) {                          /* play, rest for loop_delay, repeat */
            tt = fmodf(tt, a->dur + a->loop_delay);
            if (tt > a->dur) tt = a->dur;
        } else if (tt >= a->dur) {
            tt = a->dur;
            a->done = 1;                        /* one-shot: apply the end value once */
        }
        v = curve(a->v, tt / a->dur);
        switch (a->chan) {
        case 0: d->alpha = v; break;
        case 1: d->rot = v; break;
        case 2: d->sx = v; break;
        case 3: d->sy = v; break;
        case 4: d->x = v; break;
        default: d->y = v; break;
        }
    }
    for (i = 0; i < n_events; i++)              /* delayed changes that have come due */
        if (events[i].due >= 0 && th_clock >= events[i].due) {
            events[i].due = -1;
            apply_event(&events[i]);
        }
    for (i = 0; i < MAX_FADES; i++) {
        Fade *f = &fades[i];
        if (!f->on) continue;
        f->t += k;
        if (f->t >= f->dur) { sprites[f->sprite].d.alpha = f->to; f->on = 0; }
        else sprites[f->sprite].d.alpha = f->from + (f->to - f->from) * f->t / f->dur;
    }
}

static void apply_event(const Event *e)
{
    int j;
    switch (e->type) {
    case 0: memcpy(sprites[e->target].col_to, e->v, sizeof e->v); break;
    case 1: sprites[e->target].d.visible = e->v[0] != 0; break;
    case 2: {                                   /* animation runs at a new speed */
        Anim *a = &anims[e->target];
        float nd = e->v[0] > 0.05f ? e->v[0] : 0.05f;
        if (a->t > a->delay)
            a->t = a->delay + (a->t - a->delay) / a->dur * nd;
        a->dur = nd;
        break;
    }
    case 3:                                     /* fade a sprite's alpha */
        for (j = 0; j < n_anims; j++)           /* it replaces any one-shot fade still running */
            if (anims[j].sprite == e->target && anims[j].chan == 0 && !anims[j].loop)
                anims[j].done = 1;
        for (j = 0; j < MAX_FADES; j++)
            if (!fades[j].on || fades[j].sprite == e->target) {
                fades[j].on = 1; fades[j].sprite = e->target; fades[j].t = 0;
                fades[j].from = sprites[e->target].d.alpha; fades[j].to = e->v[1];
                fades[j].dur = e->v[0] > 0.01f ? e->v[0] : 0.01f;
                break;
            }
        break;
    default: break;
    }
}

/* A streak/multiplier/miss event: make its immediate changes, start its
 * timed effects, and drop whatever earlier events still had waiting. */
void theme_event(int phase)
{
    int i;
    for (i = 0; i < n_sprites; i++) {
        const SpriteDef *d = &defs[i];
        if (d->life <= 0 || d->phase < 0)
            continue;
        if (d->phase == phase)
            sprites[i].base = th_clock;                         /* (re)start this event's effects */
        else if (th_clock < sprites[i].base + d->spawn)
            sprites[i].base = -1e9;                             /* not out yet: cancelled */
    }
    for (i = 0; i < n_events; i++) {
        Event *e = &events[i];
        if (e->type < 0)
            continue;
        if (e->phase != phase)
            e->due = -1;
        else if (e->delay <= 0)
            apply_event(e);
        else
            e->due = th_clock + e->delay;
    }
}

static const Column *col_for(int lane, int shake)
{
    const Column *tap = NULL;
    int i;
    for (i = 0; i < n_cols; i++) {
        if (cols[i].lane != lane) continue;
        if (cols[i].shake == (shake != 0) && cols[i].tap >= 0) return &cols[i];
        if (!cols[i].shake) tap = &cols[i];
    }
    return tap ? tap : &cols[0];
}

void theme_feedback(int lane, int shake, int quality)
{
    const Column *c = col_for(lane, shake);
    if (!have_fb || c->color_sprite < 0 || quality < 0 || quality > 4)
        return;
    memcpy(sprites[c->color_sprite].col_to, fb_col[revenge_now ? 1 : 0][quality], sizeof(float) * 3);
}

static unsigned rgba(const float *c, float a)
{
    int r = (int)(c[0] * 255), g = (int)(c[1] * 255), b = (int)(c[2] * 255), al = (int)(a * 255);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    al = al < 0 ? 0 : al > 255 ? 255 : al;
    return ((unsigned)r << 24) | ((unsigned)g << 16) | ((unsigned)b << 8) | (unsigned)al;
}

/* draw every pass of a shader as a quad centred on (cx,cy); w,h <= 0 means
 * "the image's own size", multiplied by scale */
static void draw_shader(int sh, float cx, float cy, float w, float h, float sx, float sy,
                        float rot, const float *col, float alpha)
{
    static const float white[3] = { 1, 1, 1 };
    int i;
    if (sh < 0 || sh >= n_shaders)
        return;
    for (i = 0; i < shaders[sh].npass; i++) {
        const Pass *p = &shaders[sh].pass[i];
        const Frame *f = &p->frames[p->freq > 0 ? (int)(th_clock / p->freq) % p->nframes : 0];
        float a = alpha * (p->bind == 1 ? 1 - revenge_pct : p->bind == 2 ? revenge_pct : 1);
        float qw = (w > 0 ? w : f->w) * sx, qh = (h > 0 ? h : f->h) * sy;
        unsigned c = rgba(col ? col : white, a);
        if (a <= 0.004f || qw <= 0 || qh <= 0)
            continue;
        if ((p->scroll_x != 0 || p->scroll_y != 0) && rot == 0) {
            /* scrolling texture: draw the image as up to four wrapped tiles */
            float u = (float)fmod(th_clock * p->scroll_x, 1.0), v = (float)fmod(th_clock * p->scroll_y, 1.0);
            int px, py, ix, iy;
            if (u < 0) u += 1;
            if (v < 0) v += 1;
            px = (int)(u * f->w); py = (int)(v * f->h);
            for (iy = 0; iy < 2; iy++)
                for (ix = 0; ix < 2; ix++) {
                    int s_x = ix ? 0 : px, s_w = ix ? px : f->w - px;
                    int s_y = iy ? 0 : py, s_h = iy ? py : f->h - py;
                    float d_w = qw * s_w / f->w, d_h = qh * s_h / f->h;
                    float d_x = cx - qw / 2 + (ix ? qw * (f->w - px) / f->w : 0);
                    float d_y = cy - qh / 2 + (iy ? qh * (f->h - py) / f->h : 0);
                    if (s_w > 0 && s_h > 0)
                        gfx_draw(tex[f->tex], f->x + s_x, f->y + s_y, s_w, s_h, d_x + d_w / 2, d_y + d_h / 2,
                                 d_w, d_h, 0, c, p->blend);
                }
        } else {
            gfx_draw(tex[f->tex], f->x, f->y, f->w, f->h, cx, cy, qw, qh, rot, c, p->blend);
        }
    }
}

void theme_draw_scene(int group)
{
    int i;
    for (i = 0; i < n_sprites; i++) {
        const SpriteDef *d = &sprites[order[i]].d;
        if (d->group != group || !d->visible || th_clock < d->spawn)
            continue;
        draw_shader(d->shader, d->x, d->y, d->w, d->h, d->sx, d->sy, d->rot, d->col, d->alpha);
    }
}

void theme_screen(float *w, float *h) { *w = (float)screen_w; *h = (float)screen_h; }
int theme_lanes(void)                           /* 3, or 4 for the few four-lane themes */
{
    int i;
    for (i = 0; i < n_cols; i++)
        if (cols[i].lane == 3 && !cols[i].shake && cols[i].tap >= 0)
            return 4;
    return 3;
}
float theme_window(void) { return n_cols ? col_for(1, 0)->window : 1.7f; }
float theme_lane_x(int lane) { return n_cols ? col_for(lane, 0)->x[NS - 1] : 160; }
float theme_line_y(void) { return n_cols ? col_for(1, 0)->y[NS - 1] : 422; }

void theme_note_pos(int lane, int shake, float p, NotePos *out)
{
    const Column *c = col_for(lane, shake);
    if (p > 1) {                    /* past the line: carry on in the same direction */
        float over = (p - 1) * (NS - 1);
        out->x = c->x[NS - 1] + (c->x[NS - 1] - c->x[NS - 2]) * over;
        out->y = c->y[NS - 1] + (c->y[NS - 1] - c->y[NS - 2]) * over;
    } else {
        out->x = curve(c->x, p);
        out->y = curve(c->y, p);
    }
    out->scale = curve(c->s, p);
    out->alpha = curve(c->a, p);
}

void theme_draw_note(int lane, int shake, const NotePos *p, float alpha)
{
    const Column *c = col_for(lane, shake);
    draw_shader(c->tap, p->x, p->y, 0, 0, p->scale, p->scale, 0, NULL, p->alpha * alpha);
}

void theme_draw_hold(int lane, int active, const NotePos *near_end, const NotePos *far_end, float v_far, float v_near)
{
    const Column *c = col_for(lane, 0);
    int sh = active && c->hold_a >= 0 ? c->hold_a : c->hold;
    const Pass *ps;
    const Frame *f;
    float dx = near_end->x - far_end->x, dy = near_end->y - far_end->y, len = hypotf(dx, dy);
    float alpha = (near_end->alpha + far_end->alpha) / 2;
    int y0, y1;
    if (sh < 0) {                               /* theme without a trail image: use */
        sh = c->tap;                            /* a band from the middle of the note */
        v_far = 0.45f;
        v_near = 0.55f;
    }
    if (len < 0.05f || alpha <= 0.004f)
        return;
    ps = &shaders[sh].pass[0];
    f = &ps->frames[0];
    y0 = (int)(v_far * f->h);
    y1 = (int)(v_near * f->h + 0.999f);
    if (y0 < 0) y0 = 0;
    if (y1 > f->h) y1 = f->h;
    if (y1 <= y0) { if (y0 >= f->h) y0 = f->h - 1; y1 = y0 + 1; }
    /* a strip of the trail image, turned to follow the lane and scaled with
     * the same perspective as the notes */
    gfx_draw(tex[f->tex], f->x, f->y + y0, f->w, y1 - y0, (near_end->x + far_end->x) / 2,
             (near_end->y + far_end->y) / 2, f->w * (near_end->scale + far_end->scale) / 2, len + 0.5f,
             atan2f(-dx, dy) * 57.29578f, 0xffffff00u | (unsigned)((alpha > 1 ? 1 : alpha) * 255), ps->blend);
}

void theme_draw_splash(int lane, float size, float alpha)
{
    const Column *c = col_for(lane, 0);
    if (c->splash >= 0)
        draw_shader(c->splash, c->x[NS - 1], c->y[NS - 1], 0, 0, size, size, 0, NULL, alpha);
}
