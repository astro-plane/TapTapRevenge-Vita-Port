/* game.c - song list, gameplay, scoring and the HUD.
 *
 * Everything works in "logical points": the original 320 x 480 portrait
 * layout with y pointing down.  view_* maps that onto the Vita screen in
 * landscape (centred) or vertical ("tate", console turned clockwise).
 *
 * Taken from the original data: note times, lanes, holds and shakes (the
 * charts), and each song's scenery, lane curves, fall time and reactions
 * (its theme, see theme.c).  The scoring numbers below are this project's own.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "theme.h"

/* The logical screen.  Menus and most themes are 320 x 480 (portrait); a few
 * themes are laid out for a phone held sideways and use 480 x 320. */
static float cur_w = 320.0f, cur_h = 480.0f;
#define LW cur_w
#define LH cur_h

/* ---- tunables (approximations, not recovered from the original) ---- */
#define WIN_PERFECT 0.060        /* seconds either side of the note */
#define WIN_GOOD    0.120
#define WIN_OK      0.180
#define PTS_PERFECT 100
#define PTS_GOOD    75
#define PTS_OK      50
#define PTS_HOLD_TICK 20         /* per 0.1 s of a held note */
#define STREAK_2X 20             /* a streak of 10 only changes the lights */
#define STREAK_3X 30
#define STREAK_4X 40
#define STREAK_REVENGE 50        /* "shake for 8x" becomes available */
#define AUDIO_LATENCY 0.030      /* seconds between mixing and hearing */
#define HOLD_CAP 0.15f           /* share of the trail image used for each rounded end */
#define HOLD_PULSE 0.2            /* seconds between splash pulses while holding (themes' tapHoldEmitFrequency) */

/* ---------------------------------------------------------------- view -- */

static int scr_w, scr_h, tate;
static float vscale, vox, voy;

static int vt;                   /* drawing turned a quarter turn right now? */

static void view_setup(void)
{
    /* the vertical ("tate") option only applies to portrait content; a
     * sideways theme already fits the Vita's screen the normal way up */
    vt = tate && cur_h > cur_w;
    if (!vt) {
        vscale = fminf(scr_w / LW, scr_h / LH);
        vox = (scr_w - LW * vscale) / 2;
        voy = (scr_h - LH * vscale) / 2;
    } else {
        vscale = fminf(scr_w / LH, scr_h / LW);
        vox = (scr_w - LH * vscale) / 2;
        voy = (scr_h - LW * vscale) / 2;
    }
}

void game_l2s(float lx, float ly, float *sx, float *sy)
{
    if (!vt) {
        *sx = vox + lx * vscale;
        *sy = voy + ly * vscale;
    } else {
        *sx = vox + ly * vscale;
        *sy = scr_h - voy - lx * vscale;
    }
}

static void s2l(float sx, float sy, float *lx, float *ly)
{
    if (!vt) {
        *lx = (sx - vox) / vscale;
        *ly = (sy - voy) / vscale;
    } else {
        *ly = (sx - vox) / vscale;
        *lx = (scr_h - voy - sy) / vscale;
    }
}

/* ------------------------------------------------------------ graphics -- */

typedef struct { PlatTex *t; int w, h; } Tex;
static Tex tx_bg, tx_hud, tx_font;

typedef struct { int x, y, adv; } Glyph;
static Glyph glyphs[128];
static int font_h = 24;

static int tex_load(Tex *t, const char *name)
{
    char path[512];
    unsigned char *px;
    snprintf(path, sizeof path, "%sgfx/%s.png", plat_data_dir(), name);
    px = png_load_rgba(path, &t->w, &t->h);
    if (!px) {
        fprintf(stderr, "cannot load %s\n", path);
        return -1;
    }
    t->t = plat_tex_create(px, t->w, t->h);
    free(px);
    return t->t ? 0 : -1;
}

static int font_load(void)
{
    char path[512], line[128];
    FILE *fp;
    int c, x, y, adv, h;
    snprintf(path, sizeof path, "%sgfx/font.txt", plat_data_dir());
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, "font %*d %d", &h) == 1)
            font_h = h;
        else if (sscanf(line, "%d %d %d %d", &c, &x, &y, &adv) == 4 && c >= 0 && c < 128) {
            glyphs[c].x = x; glyphs[c].y = y; glyphs[c].adv = adv;
        }
    }
    fclose(fp);
    return 0;
}

/* part of a texture, centred on a logical position; size in logical points */
void gfx_draw(struct PlatTex *t, int sx, int sy, int sw, int sh, float cx, float cy,
              float w, float h, float rot, unsigned rgba, int blend)
{
    float x, y;
    game_l2s(cx, cy, &x, &y);
    plat_draw(t, sx, sy, sw, sh, x, y, w * vscale, h * vscale, rot + (vt ? -90.0f : 0.0f), rgba, blend);
}

static void draw(const Tex *t, int sx, int sy, int sw, int sh, float cx, float cy,
                 float w, float h, float rot, unsigned rgba, int blend)
{
    gfx_draw(t->t, sx, sy, sw, sh, cx, cy, w, h, rot, rgba, blend);
}

static void fill(float x, float y, float w, float h, unsigned rgba)
{
    float x0, y0, x1, y1, t;
    game_l2s(x, y, &x0, &y0);
    game_l2s(x + w, y + h, &x1, &y1);
    if (x1 < x0) { t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; }
    plat_fill(x0, y0, x1 - x0, y1 - y0, rgba);
}

static float text_width(const char *s, float size)
{
    float w = 0, k = size / font_h;
    for (; *s; s++)
        w += glyphs[(unsigned char)*s & 127].adv * k;
    return w;
}

/* size = line height in points; align: 0 left, 1 centre, 2 right; max_w clips */
static void text(float x, float y, float size, int align, float max_w, unsigned rgba, const char *s)
{
    float k = size / font_h, w = text_width(s, size);
    if (align == 1) x -= (w < max_w || max_w <= 0 ? w : max_w) / 2;
    if (align == 2) x -= (w < max_w || max_w <= 0 ? w : max_w);
    {
        float x0 = x;
        for (; *s; s++) {
            const Glyph *g = &glyphs[(unsigned char)*s & 127];
            float gw = g->adv * k;
            if (max_w > 0 && x + gw - x0 > max_w)
                break;
            if (*s != ' ' && g->adv > 0)
                draw(&tx_font, g->x, g->y, g->adv, font_h, x + gw / 2, y + size / 2, gw, size, 0, rgba, 0);
            x += gw;
        }
    }
}

static void draw_bars(void)
{
    /* hide whatever spilled outside the 320x480 play field */
    float fw = (vt ? LH : LW) * vscale;
    plat_fill(0, 0, vox, (float)scr_h, 0x0a0a0cff);
    plat_fill(vox + fw, 0, scr_w - (vox + fw), (float)scr_h, 0x0a0a0cff);
    if (voy > 0) {
        plat_fill(0, 0, (float)scr_w, voy, 0x0a0a0cff);
        plat_fill(0, scr_h - voy, (float)scr_w, voy, 0x0a0a0cff);
    }
}

/* --------------------------------------------------------------- songs -- */

typedef struct { char num[8]; char title[64]; char artist[48]; float length; int levels; char theme[5][28]; } Song;
static Song *songs;
static int n_songs;
static const char *level_name[5] = { "Kids", "Easy", "Medium", "Hard", "Extreme" };

static int songs_load(void)
{
    char path[512], line[512];
    FILE *fp;
    int cap = 0;
    snprintf(path, sizeof path, "%ssongs.txt", plat_data_dir());
    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "cannot open %s - run tools/prep_assets.py first\n", path);
        return -1;
    }
    while (fgets(line, sizeof line, fp)) {
        char *f[6], *p = line;
        int i;
        Song *s;
        line[strcspn(line, "\r\n")] = 0;
        f[5] = NULL;
        for (i = 0; i < 6; i++) {
            f[i] = p;
            p = strchr(p, '\t');
            if (!p) break;
            *p++ = 0;
        }
        if (i < 4)
            continue;
        if (n_songs == cap) {
            Song *ns = realloc(songs, sizeof *songs * (size_t)(cap = cap ? cap * 2 : 64));
            if (!ns) break;
            songs = ns;
        }
        s = &songs[n_songs];
        memset(s, 0, sizeof *s);
        snprintf(s->num, sizeof s->num, "%s", f[0]);
        snprintf(s->title, sizeof s->title, "%s", f[1]);
        snprintf(s->artist, sizeof s->artist, "%s", f[2]);
        s->length = (float)atof(f[3]);
        for (p = f[4]; *p; p++)
            if (*p >= '0' && *p <= '4')
                s->levels |= 1 << (*p - '0');
        /* optional sixth field: which theme each level uses, "2=Name,4=Other" */
        for (p = f[5]; p && *p; ) {
            int lv = *p - '0';
            size_t len = strcspn(p, ",");
            if (lv >= 0 && lv <= 4 && p[1] == '=' && len > 2)
                snprintf(s->theme[lv], sizeof s->theme[0], "%.*s", (int)(len - 2), p + 2);
            p += len;
            if (*p == ',') p++;
        }
        if (s->levels)
            n_songs++;
    }
    fclose(fp);
    return n_songs > 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- chart -- */

enum { N_PENDING, N_HIT, N_HOLDING, N_MISSED, N_DONE };
typedef struct { float t, hold; unsigned char lane, shake, state; float held_to; } Note;

#define MAX_LANES 4
static Note *notes;
static int n_notes, first_live;
static int n_lanes = 3;                      /* 3, or 4 with a four-lane theme */
static float chart_end;

static int chart_load(const Song *s, int level)
{
    char path[512], line[128];
    FILE *fp;
    int lv, cnt, in = 0, cap = 0;
    free(notes);
    notes = NULL;
    n_notes = first_live = 0;
    chart_end = s->length;
    snprintf(path, sizeof path, "%scharts/%s.txt", plat_data_dir(), s->num);
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    while (fgets(line, sizeof line, fp)) {
        float t, hold;
        int lane, shake;
        if (sscanf(line, "level %d %d", &lv, &cnt) == 2) {
            if (in) break;
            in = lv == level;
            if (in) {
                cap = cnt > 0 && cnt < 100000 ? cnt : 0;
                notes = calloc((size_t)cap + 1, sizeof *notes);
                if (!notes) cap = 0;
            }
        } else if (in && n_notes < cap && sscanf(line, "%f %d %d %f", &t, &lane, &shake, &hold) == 4 &&
                   lane >= 0 && lane < n_lanes) {
            Note *n = &notes[n_notes++];
            n->t = t; n->hold = hold; n->lane = (unsigned char)lane; n->shake = shake != 0;
            if (t + hold > chart_end) chart_end = t + hold;
        }
    }
    fclose(fp);
    return n_notes > 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- state -- */

enum { SC_TITLE, SC_LIST, SC_LEVEL, SC_PLAY, SC_RESULT };

static int scene, sel_song, sel_level, paused;
static float list_scroll;                    /* points scrolled from the top */
static double now_t;                         /* seconds since start          */

/* a single "pointer" for menus: first finger */
static struct { int down, pressed, released; float x, y, y0, moved; } ptr;

/* gameplay */
static double song_t;                        /* current position in the song */
static long score;
static int streak, best_streak, mult, revenge, n_hit, n_judged;
static int lane_down[MAX_LANES], lane_was[MAX_LANES];        /* lane held this frame / last   */
static double splash_t[MAX_LANES];
static double fb_t = -10; static int fb_kind; /* banner: 0 miss 1 great 2..4 = 2x/3x/4x 5 = 8x */
static double hold_tick[MAX_LANES];
static int play_started;
static int pos_lane[MAX_LANES];              /* lanes in left-to-right screen order */

static void feedback(int kind)
{
    fb_kind = kind;
    fb_t = now_t;
}

static int mult_for(int s)
{
    return s >= STREAK_4X ? 4 : s >= STREAK_3X ? 3 : s >= STREAK_2X ? 2 : 1;
}

static void on_hit(int lane, int shake, int pts, int quality)
{
    int m;
    streak++;
    n_hit++;
    n_judged++;
    if (streak > best_streak) best_streak = streak;
    if (streak == 10)
        theme_event(TH_STREAK10);
    m = revenge ? 8 : mult_for(streak);
    if (m != mult && !revenge) {
        feedback(m);                         /* 2x / 3x / 4x badge */
        theme_event(m == 2 ? TH_MULT2 : m == 3 ? TH_MULT3 : TH_MULT4);
    } else if (!shake && streak % 10 == 0) {
        feedback(1);
    }
    mult = m;
    score += (long)pts * mult;
    splash_t[lane] = now_t;
    theme_feedback(lane, shake, quality);
}

static void on_miss(int lane, int shake)
{
    n_judged++;
    if (streak >= 5 || revenge)
        feedback(0);
    streak = 0;
    mult = 1;
    revenge = 0;
    theme_event(TH_MISS);
    theme_feedback(lane, shake, Q_MISS);
}

static void start_song(void)
{
    char path[512];
    const Song *s = &songs[sel_song];
    const char *theme = s->theme[sel_level][0] ? s->theme[sel_level] : "TTRDJ";
    if (theme_load(theme, sel_level) != 0 && theme_load("TTRDJ", sel_level) != 0) {
        fprintf(stderr, "no usable theme for %s\n", s->title);
        return;
    }
    n_lanes = theme_lanes();
    if (chart_load(s, sel_level) != 0) {
        theme_free();
        return;
    }
    theme_screen(&cur_w, &cur_h);            /* sideways themes switch the layout */
    view_setup();
    /* a few themes draw the lanes in a different left-to-right order:
     * buttons follow what is on screen, not the lane numbers */
    {
        int a, b, t;
        for (a = 0; a < n_lanes; a++) pos_lane[a] = a;
        for (a = 0; a < n_lanes; a++)
            for (b = a + 1; b < n_lanes; b++)
                if (theme_lane_x(pos_lane[b]) < theme_lane_x(pos_lane[a])) {
                    t = pos_lane[a]; pos_lane[a] = pos_lane[b]; pos_lane[b] = t;
                }
    }
    score = 0;
    streak = best_streak = n_hit = n_judged = revenge = 0;
    mult = 1;
    paused = 0;
    song_t = 0;
    fb_t = -10;
    memset(lane_was, 0, sizeof lane_was);
    memset(splash_t, 0, sizeof splash_t);
    snprintf(path, sizeof path, "%saudio/%s.ogg", plat_data_dir(), s->num);
    play_started = plat_music_play(path) == 0;
    scene = SC_PLAY;
}

static void end_song(int to_scene)
{
    plat_music_stop();
    theme_free();
    cur_w = 320.0f;                          /* menus are always portrait */
    cur_h = 480.0f;
    view_setup();
    paused = 0;
    scene = to_scene;
}

/* a press in a lane (finger, lane button or shake) */
static void lane_press(int lane, int by_shake)
{
    int i;
    for (i = first_live; i < n_notes; i++) {
        Note *n = &notes[i];
        double d = n->t - song_t;
        if (d > WIN_OK)
            break;
        if (n->state != N_PENDING || n->lane != lane || d < -WIN_OK)
            continue;
        if (by_shake && !n->shake)
            continue;
        d = fabs(d);
        if (d <= WIN_PERFECT)   on_hit(lane, n->shake, PTS_PERFECT, Q_PERFECT);
        else if (d <= WIN_GOOD) on_hit(lane, n->shake, PTS_GOOD, Q_GOOD);
        else                    on_hit(lane, n->shake, PTS_OK, Q_OK);
        if (n->hold > 0) {
            n->state = N_HOLDING;
            n->held_to = (float)song_t;
            hold_tick[lane] = song_t;
        } else {
            n->state = N_HIT;
        }
        return;
    }
}

static void play_update(const PlatInput *in, double dt)
{
    int i, lane;

    if (in->pressed & BTN_START) {
        paused = !paused;
        plat_music_pause(paused);
        plat_sfx("click");
    }
    if (paused) {
        if (in->pressed & (BTN_CIRCLE | BTN_SELECT))        /* give up */
            end_song(SC_LIST);
        return;
    }
    song_t = plat_music_time() - AUDIO_LATENCY;
    theme_update(dt, revenge);

    /* which lanes are held: fingers in the lower part of the screen, or buttons */
    memset(lane_down, 0, sizeof lane_down);
    for (i = 0; i < in->n; i++) {
        float lx, ly, best = 1e9f;
        int pick = 0;
        s2l(in->t[i].x, in->t[i].y, &lx, &ly);
        if (ly < LH * 0.5f || lx < -40 || lx > LW + 40)
            continue;
        for (lane = 0; lane < n_lanes; lane++)
            if (fabsf(lx - theme_lane_x(lane)) < best) {
                best = fabsf(lx - theme_lane_x(lane));
                pick = lane;
            }
        lane_down[pick] = 1;
    }
    if (n_lanes == 4) {                      /* four lanes: two on the d-pad, two on the face buttons */
        if (in->held & BTN_LEFT) lane_down[pos_lane[0]] = 1;
        if (in->held & (BTN_UP | BTN_DOWN | BTN_RIGHT)) lane_down[pos_lane[1]] = 1;
        if (in->held & (BTN_SQUARE | BTN_TRIANGLE | BTN_CROSS)) lane_down[pos_lane[2]] = 1;
        if (in->held & BTN_CIRCLE) lane_down[pos_lane[3]] = 1;
    } else {
        if (in->held & (BTN_LEFT | BTN_SQUARE)) lane_down[pos_lane[0]] = 1;
        if (in->held & (BTN_UP | BTN_DOWN | BTN_TRIANGLE | BTN_CROSS)) lane_down[pos_lane[1]] = 1;
        if (in->held & (BTN_RIGHT | BTN_CIRCLE)) lane_down[pos_lane[2]] = 1;
    }
    for (lane = 0; lane < n_lanes; lane++)
        if (lane_down[lane] && !lane_was[lane])
            lane_press(lane, 0);

    /* shaking: move the console (any direction hits whichever arrow is due),
     * or use the shoulder buttons */
    if (in->pressed & (BTN_L | BTN_R | BTN_SHAKE)) {
        int before = n_hit;
        if (in->pressed & BTN_SHAKE)
            for (lane = 0; lane < n_lanes && n_hit == before; lane++)
                lane_press(lane, 1);
        if ((in->pressed & BTN_L) && n_hit == before) lane_press(pos_lane[0], 1);
        if ((in->pressed & BTN_R) && n_hit == before) lane_press(pos_lane[n_lanes - 1], 1);
        if ((in->pressed & (BTN_L | BTN_R)) && n_hit == before) lane_press(pos_lane[1], 1);
        if (n_hit == before && streak >= STREAK_REVENGE && !revenge) {
            revenge = 1;                     /* shake with a 50 streak: 8x until a miss */
            mult = 8;
            feedback(5);
            theme_event(TH_MULT8);
        }
    }

    for (i = first_live; i < n_notes; i++) {
        Note *n = &notes[i];
        if (n->t - song_t > WIN_OK)
            break;
        if (n->state == N_PENDING && song_t - n->t > WIN_OK) {
            n->state = N_MISSED;
            on_miss(n->lane, n->shake);
        } else if (n->state == N_HOLDING) {
            if (lane_down[n->lane] && song_t < n->t + n->hold) {
                n->held_to = (float)song_t;
                while (song_t - hold_tick[n->lane] >= 0.1) {
                    hold_tick[n->lane] += 0.1;
                    score += PTS_HOLD_TICK * mult;
                }
                if (now_t - splash_t[n->lane] >= HOLD_PULSE)   /* pulse while held, as the themes ask */
                    splash_t[n->lane] = now_t;
            } else {
                n->state = N_DONE;           /* let go, or reached the end */
            }
        }
    }
    while (first_live < n_notes && notes[first_live].state != N_PENDING &&
           notes[first_live].state != N_HOLDING && song_t - (notes[first_live].t + notes[first_live].hold) > 1.0)
        first_live++;
    memcpy(lane_was, lane_down, sizeof lane_was);

    if (song_t > chart_end + 2.5 || (plat_music_done() && song_t > 1.0) || !play_started) {
        for (i = 0; i < n_notes; i++)        /* anything never reached counts as missed */
            if (notes[i].state == N_PENDING) n_judged++;
        end_song(SC_RESULT);
    }
}

static void play_draw(void)
{
    float win = theme_window();
    int i, lane;
    char buf[64];

    theme_draw_scene(0);                     /* scenery behind the notes */

    /* hold trails first, then note heads (far ones first).  A trail is a
     * ribbon along the lane: the two ends of the trail image keep their
     * shape and the middle stretches to the length of the hold. */
    for (i = n_notes - 1; i >= first_live; i--) {
        const Note *n = &notes[i];
        float t0, t1, t, step = win / 90, cap = n->hold / 3 < 0.10f ? n->hold / 3 : 0.10f;
        if (n->hold <= 0 || n->t - song_t > win || n->state == N_MISSED || n->state == N_DONE)
            continue;
        t0 = n->state == N_HOLDING ? (float)song_t : n->t;
        t1 = n->t + n->hold;
        if (t1 > song_t + win) t1 = (float)song_t + win;
        for (t = t0; t < t1; t += step) {
            float te = t + step < t1 ? t + step : t1, v[2], tt[2];
            NotePos a, b;
            int k;
            tt[0] = te; tt[1] = t;              /* far end, near end */
            for (k = 0; k < 2; k++) {
                float u = tt[k] - n->t;         /* 0 at the head .. hold at the tail */
                if (u < cap)                 v[k] = 1.0f - HOLD_CAP * u / cap;
                else if (u > n->hold - cap)  v[k] = HOLD_CAP * (n->hold - u) / cap;
                else v[k] = 1.0f - HOLD_CAP - (1.0f - 2 * HOLD_CAP) * (u - cap) / (n->hold - 2 * cap);
            }
            theme_note_pos(n->lane, 0, 1.0f - (float)(t - song_t) / win, &a);
            theme_note_pos(n->lane, 0, 1.0f - (float)(te - song_t) / win, &b);
            theme_draw_hold(n->lane, n->state == N_HOLDING, &a, &b, v[0], v[1]);
        }
    }
    for (i = n_notes - 1; i >= first_live; i--) {
        const Note *n = &notes[i];
        NotePos p;
        if (n->t - song_t > win)
            continue;
        if (n->state == N_HIT || n->state == N_DONE)
            continue;
        if (n->state == N_HOLDING) {            /* the note head stays on the line while held */
            theme_note_pos(n->lane, 0, 1.0f, &p);
            theme_draw_note(n->lane, 0, &p, 1.0f);
            continue;
        }
        theme_note_pos(n->lane, n->shake, 1.0f - (float)(n->t - song_t) / win, &p);
        if (p.y > LH + 60 || p.y < -60 || p.x < -80 || p.x > LW + 80)
            continue;
        theme_draw_note(n->lane, n->shake, &p, n->state == N_MISSED ? 0.4f : 1.0f);
    }

    /* splash where a note was hit */
    for (lane = 0; lane < n_lanes; lane++) {
        float a = 1.0f - (float)(now_t - splash_t[lane]) / 0.25f;
        if (a > 0 && splash_t[lane] > 0)
            theme_draw_splash(lane, 1.3f - 0.3f * a, a);
    }

    theme_draw_scene(1);                     /* glows and overlays in front */

    /* HUD */
    fill(0, 0, LW, 34, 0x000000a0u);
    snprintf(buf, sizeof buf, "%ld", score);
    text(8, 6, 22, 0, 0, 0xffffffffu, buf);
    snprintf(buf, sizeof buf, "x%d", mult);
    text(LW / 2, 6, 22, 1, 0, mult >= 8 ? 0xffd040ffu : mult > 1 ? 0x60ff80ffu : 0xa0a0a0ffu, buf);
    snprintf(buf, sizeof buf, "streak %d", streak);
    text(LW - 8, 9, 16, 2, 0, 0xffffffffu, buf);
    if (chart_end > 0)
        fill(0, 34, LW * (float)(song_t / chart_end < 0 ? 0 : song_t / chart_end > 1 ? 1 : song_t / chart_end), 2, 0x40c0ffffu);
    if (streak >= STREAK_REVENGE && !revenge && fmod(now_t, 0.8) < 0.5)
        draw(&tx_hud, 384, 424, 128, 87, LW / 2, LH * 0.52f, 128, 87, 0, 0xffffffffu, 0);

    /* feedback banner */
    {
        float age = (float)(now_t - fb_t);
        if (age < 0.7f) {
            unsigned c = 0xffffff00u | (unsigned)(255 * (age < 0.4f ? 1.0f : (0.7f - age) / 0.3f));
            float k = age < 0.1f ? 0.6f + 4 * age : 1.0f;
            if (fb_kind == 0)      draw(&tx_hud, 346, 244, 62, 32, LW / 2, LH * 0.42f, 62 * k, 32 * k, 0, c, 0);
            else if (fb_kind == 1) draw(&tx_hud, 424, 242, 88, 34, LW / 2, LH * 0.42f, 88 * k, 34 * k, 0, c, 0);
            else if (fb_kind >= 2 && fb_kind <= 4)
                draw(&tx_hud, (fb_kind - 2) * 128, 424, 128, 87, LW / 2, LH * 0.42f, 128 * k, 87 * k, 0, c, 0);
            else text(LW / 2, LH * 0.38f, 40, 1, 0, 0xffd040ffu, "8x REVENGE");
        }
    }
    if (paused) {
        fill(0, 0, LW, LH, 0x000000b0u);
        text(LW / 2, LH * 0.40f, 30, 1, 0, 0xffffffffu, "PAUSED");
        text(LW / 2, LH * 0.40f + 45, 15, 1, 0, 0xc0c0c0ffu, "START: continue    O: quit song");
    }
}

/* ---------------------------------------------------------------- menus -- */

#define ROW_H 46.0f
#define LIST_TOP 44.0f

static void list_clamp(void)
{
    float max = n_songs * ROW_H - (LH - LIST_TOP);
    if (list_scroll > max) list_scroll = max;
    if (list_scroll < 0) list_scroll = 0;
}

static void list_show(int i)
{
    if (i * ROW_H < list_scroll) list_scroll = i * ROW_H;
    if ((i + 1) * ROW_H > list_scroll + (LH - LIST_TOP)) list_scroll = (i + 1) * ROW_H - (LH - LIST_TOP);
    list_clamp();
}

static int first_level(int mask, int want)
{
    int l;
    if (mask & (1 << want)) return want;
    for (l = 0; l < 5; l++)
        if (mask & (1 << l)) return l;
    return 0;
}

static double repeat_t;
static float list_vel;                       /* list glide speed, points per second */
#define LIST_FRICTION 2.2f                   /* higher = the glide stops sooner */
#define LIST_MAX_SPEED 6000.0f

static void list_update(const PlatInput *in, double dt)
{
    int step = 0;
    if (in->pressed & BTN_UP) { step = -1; repeat_t = 0.35; }
    if (in->pressed & BTN_DOWN) { step = 1; repeat_t = 0.35; }
    if (in->held & (BTN_UP | BTN_DOWN)) {    /* hold to keep scrolling */
        repeat_t -= dt;
        if (repeat_t <= 0) { step = (in->held & BTN_UP) ? -1 : 1; repeat_t = 0.06; }
    }
    if (in->pressed & BTN_L) step = -8;
    if (in->pressed & BTN_R) step = 8;
    if (step) {
        sel_song += step;
        if (sel_song < 0) sel_song = 0;
        if (sel_song >= n_songs) sel_song = n_songs - 1;
        list_show(sel_song);
    }
    if (step)
        list_vel = 0;
    if (ptr.pressed) {
        list_vel = 0;                        /* a finger on the list stops it */
    } else if (ptr.down) {
        float dy = ptr.y - ptr.y0;
        list_scroll -= dy;
        ptr.y0 = ptr.y;
        if (dt > 0.0005)                     /* remember how fast the finger is moving */
            list_vel = list_vel * 0.5f + (float)(-dy / dt) * 0.5f;
        list_clamp();
    } else if (list_vel != 0) {              /* released: keep gliding and slow down */
        float before;
        list_scroll += list_vel * (float)dt;
        before = list_scroll;
        list_clamp();
        list_vel *= expf(-LIST_FRICTION * (float)dt);
        if (fabsf(list_vel) < 8 || list_scroll != before)
            list_vel = 0;                    /* nearly stopped, or hit an end of the list */
    }
    if (list_vel > LIST_MAX_SPEED) list_vel = LIST_MAX_SPEED;
    if (list_vel < -LIST_MAX_SPEED) list_vel = -LIST_MAX_SPEED;
    if (ptr.released && ptr.moved < 8 && ptr.y > LIST_TOP) {
        int i = (int)((ptr.y - LIST_TOP + list_scroll) / ROW_H);
        if (i >= 0 && i < n_songs) {
            sel_song = i;
            in = NULL;                       /* fall through to "chosen" */
        }
    }
    if (!in || (in->pressed & BTN_CROSS)) {
        sel_level = first_level(songs[sel_song].levels, sel_level ? sel_level : 2);
        plat_sfx("click");
        scene = SC_LEVEL;
    } else if (in->pressed & BTN_CIRCLE) {
        scene = SC_TITLE;
    }
}

static void list_draw(void)
{
    int i, first = (int)(list_scroll / ROW_H);
    char buf[64];
    fill(0, 0, LW, LH, 0x101018ffu);
    for (i = first; i < n_songs && i < first + 11; i++) {
        float y = LIST_TOP + i * ROW_H - list_scroll;
        const Song *s = &songs[i];
        if (i == sel_song)
            fill(0, y, LW, ROW_H - 1, 0x2050a0ffu);
        else if (i & 1)
            fill(0, y, LW, ROW_H - 1, 0x181824ffu);
        text(10, y + 5, 19, 0, LW - 20, 0xffffffffu, s->title);
        text(10, y + 26, 14, 0, LW - 20, 0x90a0c0ffu, s->artist);
    }
    fill(0, 0, LW, LIST_TOP, 0x000000ffu);
    snprintf(buf, sizeof buf, "Select a song  (%d of %d)", sel_song + 1, n_songs);
    text(LW / 2, 12, 18, 1, 0, 0xffd040ffu, buf);
}

static void level_update(const PlatInput *in)
{
    int l, row = 0, chosen = 0, mask = songs[sel_song].levels, dir = 0;
    if (in->pressed & BTN_UP) dir = -1;
    if (in->pressed & BTN_DOWN) dir = 1;
    if (dir) {
        for (l = sel_level + dir; l >= 0 && l < 5; l += dir)
            if (mask & (1 << l)) { sel_level = l; break; }
    }
    for (l = 0; l < 5; l++) {
        if (!(mask & (1 << l)))
            continue;
        if (ptr.released && ptr.moved < 8 && ptr.y >= 190 + row * 50 && ptr.y < 234 + row * 50) {
            sel_level = l;
            chosen = 1;
        }
        row++;
    }
    if (chosen || (in->pressed & BTN_CROSS)) {
        plat_sfx("click");
        start_song();
    } else if ((in->pressed & BTN_CIRCLE) || (ptr.released && ptr.moved < 8 && ptr.y < 60)) {
        scene = SC_LIST;
    }
}

static void level_draw(void)
{
    const Song *s = &songs[sel_song];
    int l, row = 0;
    fill(0, 0, LW, LH, 0x101018ffu);
    text(12, 14, 16, 0, 0, 0x90a0c0ffu, "< back");
    text(LW / 2, 80, 24, 1, LW - 16, 0xffffffffu, s->title);
    text(LW / 2, 112, 16, 1, LW - 16, 0x90a0c0ffu, s->artist);
    text(LW / 2, 156, 15, 1, 0, 0xffd040ffu, "Choose a difficulty");
    for (l = 0; l < 5; l++) {
        if (!(s->levels & (1 << l)))
            continue;
        fill(60, 190 + row * 50.0f, 200, 44, l == sel_level ? 0x2050a0ffu : 0x282838ffu);
        text(LW / 2, 200 + row * 50.0f, 22, 1, 0, 0xffffffffu, level_name[l]);
        row++;
    }
}

static void result_draw(void)
{
    const Song *s = &songs[sel_song];
    char buf[64];
    int pct = n_judged ? (int)(100.0 * n_hit / n_judged + 0.5) : 0;
    fill(0, 0, LW, LH, 0x101018ffu);
    text(LW / 2, 50, 22, 1, LW - 16, 0xffffffffu, s->title);
    text(LW / 2, 80, 15, 1, LW - 16, 0x90a0c0ffu, level_name[sel_level]);
    snprintf(buf, sizeof buf, "%ld", score);
    text(LW / 2, 140, 44, 1, 0, 0xffd040ffu, buf);
    snprintf(buf, sizeof buf, "Notes hit: %d of %d (%d%%)", n_hit, n_judged, pct);
    text(LW / 2, 230, 17, 1, 0, 0xffffffffu, buf);
    snprintf(buf, sizeof buf, "Best streak: %d", best_streak);
    text(LW / 2, 260, 17, 1, 0, 0xffffffffu, buf);
    text(LW / 2, 400, 15, 1, 0, 0x90a0c0ffu, "Tap or press X to continue");
}

/* --------------------------------------------------------- entry points -- */

int game_init(int sw, int sh)
{
    FILE *fp;
    scr_w = sw;
    scr_h = sh;
    fp = fopen(plat_save_file(), "r");
    if (fp) {
        if (fscanf(fp, "tate %d", &tate) != 1) tate = 0;
        tate = tate != 0;
        fclose(fp);
    }
    view_setup();
    if (tex_load(&tx_bg, "background") || tex_load(&tx_hud, "hud") || tex_load(&tx_font, "font") ||
        font_load() || songs_load())
        return -1;
    scene = SC_TITLE;
    return 0;
}

void game_shutdown(void)
{
    FILE *fp = fopen(plat_save_file(), "w");
    if (fp) {
        fprintf(fp, "tate %d\n", tate);
        fclose(fp);
    }
    plat_music_stop();
    theme_free();
    plat_tex_free(tx_bg.t); plat_tex_free(tx_hud.t); plat_tex_free(tx_font.t);
    free(songs); free(notes);
    songs = NULL; notes = NULL; n_songs = n_notes = 0;
}

int game_frame(double dt, const PlatInput *in)
{
    int was = ptr.down;
    char buf[64];

    if (dt > 0.1) dt = 0.1;
    if (dt < 0) dt = 0;
    now_t += dt;

    if ((in->pressed & BTN_SELECT) && !(scene == SC_PLAY && paused)) {
        tate = !tate;
        view_setup();
    }
    ptr.down = in->n > 0;
    ptr.pressed = ptr.down && !was;
    ptr.released = !ptr.down && was;
    if (ptr.down) {
        float lx, ly;
        s2l(in->t[0].x, in->t[0].y, &lx, &ly);
        if (ptr.pressed) { ptr.y0 = ly; ptr.moved = 0; }
        else ptr.moved += fabsf(ly - ptr.y) + fabsf(lx - ptr.x);
        ptr.x = lx;
        ptr.y = ly;
    }

    switch (scene) {
    case SC_TITLE:
        if (ptr.released || (in->pressed & BTN_CROSS)) {
            plat_sfx("slide");
            scene = SC_LIST;
        } else if (in->pressed & BTN_START) {
            return 0;
        }
        break;
    case SC_LIST:   list_update(in, dt); break;
    case SC_LEVEL:  level_update(in); break;
    case SC_PLAY:   play_update(in, dt); break;
    case SC_RESULT:
        if (ptr.released || (in->pressed & (BTN_CROSS | BTN_CIRCLE)))
            scene = SC_LIST;
        break;
    }

    plat_begin();
    switch (scene) {
    case SC_TITLE:
        draw(&tx_bg, 0, 0, tx_bg.w, tx_bg.h, LW / 2, LH / 2, 512, 512, 0, 0xffffffffu, 0);
        fill(0, 120, LW, 130, 0x000000a0u);
        text(LW / 2, 135, 34, 1, 0, 0xffffffffu, "TAP TAP");
        text(LW / 2, 175, 24, 1, 0, 0xffd040ffu, "PS Vita reimplementation");
        snprintf(buf, sizeof buf, "%d songs", n_songs);
        text(LW / 2, 212, 16, 1, 0, 0xc0c0c0ffu, buf);
        if (fmod(now_t, 1.2) < 0.8)
            text(LW / 2, 300, 18, 1, 0, 0xffffffffu, "Tap the screen or press X");
        break;
    case SC_LIST:   list_draw(); break;
    case SC_LEVEL:  level_draw(); break;
    case SC_PLAY:   play_draw(); break;
    case SC_RESULT: result_draw(); break;
    }
    draw_bars();
    plat_end();
    return in->quit ? 0 : 1;
}

void game_debug(char *buf, int n)
{
    snprintf(buf, (size_t)n, "scene=%d song=%d level=%d t=%.2f notes=%d score=%ld streak=%d best=%d mult=%d hit=%d judged=%d revenge=%d paused=%d",
             scene, sel_song, sel_level, song_t, n_notes, score, streak, best_streak, mult, n_hit, n_judged, revenge, paused);
}
