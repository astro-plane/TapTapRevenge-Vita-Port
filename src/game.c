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
#include <ctype.h>
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

/* Song covers, packed by tools/prep_assets.py into gfx/art.png: song number N
 * is tile N-1, ART_COLS tiles of ART_TILE pixels per row.  Optional: data
 * converted before covers were added has none, and gets lettered tiles. */
#define ART_TILE 64
#define ART_COLS 32
static Tex tx_art;
static unsigned char *art_have;              /* per tile: is there a cover in it? */
static int art_tiles;

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

static void art_load(void)
{
    char path[512];
    unsigned char *px;
    int w, h, i;
    snprintf(path, sizeof path, "%sgfx/art.png", plat_data_dir());
    px = png_load_rgba(path, &w, &h);
    if (!px)
        return;                              /* no covers: lettered tiles */
    art_tiles = (w / ART_TILE) * (h / ART_TILE);
    art_have = calloc((size_t)art_tiles + 1, 1);
    if (art_have) {
        for (i = 0; i < art_tiles; i++) {    /* an empty tile is see-through */
            int cx = (i % ART_COLS) * ART_TILE + ART_TILE / 2, cy = (i / ART_COLS) * ART_TILE + ART_TILE / 2;
            art_have[i] = cx < w && cy < h && px[((size_t)cy * w + cx) * 4 + 3] > 0;
        }
        tx_art.t = plat_tex_create(px, w, h);
        tx_art.w = w;
        tx_art.h = h;
    }
    free(px);
    if (!tx_art.t) {
        free(art_have);
        art_have = NULL;
        art_tiles = 0;
    }
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

typedef struct {
    char num[8]; char title[64]; char artist[48]; float length; int levels; char theme[5][28];
    char tkey[64], akey[48];                 /* sort keys: lower case, without a leading "The " */
    char tletter, aletter;                   /* their list section: 'A'..'Z', or '#' */
} Song;

static void sort_key(const char *s, char *out, size_t n, char *letter)
{
    size_t i = 0;
    if (tolower((unsigned char)s[0]) == 't' && tolower((unsigned char)s[1]) == 'h' &&
        tolower((unsigned char)s[2]) == 'e' && s[3] == ' ' && s[4])
        s += 4;
    for (; *s && i + 1 < n; s++)
        out[i++] = (char)tolower((unsigned char)*s);
    out[i] = 0;
    *letter = out[0] >= 'a' && out[0] <= 'z' ? (char)(out[0] - 'a' + 'A') : '#';
}
static Song *songs;
static int n_songs;

/* Your best run of each song at each difficulty, kept in the settings file.
 * "Best" is the highest-scoring run, with that run's streak and accuracy. */
typedef struct { long score; int streak, hit, judged; } Best;
static Best *bests;                          /* n_songs * 5, indexed song*5+level */

static Best *best_of(int song, int level)
{
    if (!bests || song < 0 || song >= n_songs || level < 0 || level > 4)
        return NULL;
    return &bests[song * 5 + level];
}

static int song_by_num(const char *num)
{
    int i;
    for (i = 0; i < n_songs; i++)
        if (!strcmp(songs[i].num, num))
            return i;
    return -1;
}
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
        sort_key(s->title, s->tkey, sizeof s->tkey, &s->tletter);
        sort_key(s->artist, s->akey, sizeof s->akey, &s->aletter);
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

static void settings_save(void);

static int list_dirty;                       /* the song list must be rebuilt (menus, below) */
static Best prev_best;                       /* what it was before this run */
static int new_best;

static void record_run(void)
{
    Best *b = best_of(sel_song, sel_level);
    if (!b)
        return;
    prev_best = *b;
    new_best = score > b->score;
    if (new_best) {
        b->score = score;
        b->streak = best_streak;
        b->hit = n_hit;
        b->judged = n_judged;
        settings_save();                     /* write now: the Vita may be switched off */
        list_dirty = 1;                      /* scores and their order changed */
    }
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
        record_run();
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

/* The song list looks like a classic phone music library: a title bar,
 * alphabetical sections with a header that sticks to the top, an A-Z index
 * down the right, rows of title and artist, and a tab bar.  Three styles:
 * SKIN_SILVER (light grey bars, a cover tile on each row), SKIN_BLUE
 * (blue-grey bars, see-through section headers, text-only rows) and
 * SKIN_DARK (near-black, cover tiles, orange highlight and accents).  Everything
 * is drawn with plain fills, so no extra artwork is needed. */

#define NAV_H    44.0f
#define TAB_H    49.0f
#define HDR_H    22.0f
#define ROW_H    44.0f
#define FOOT_H   52.0f
#define IDX_W    18.0f
#define LIST_TOP NAV_H
#define LIST_BOT (LH - TAB_H)

enum { SKIN_SILVER, SKIN_BLUE, SKIN_DARK, N_SKINS };
#define DARK_ORANGE 0xff9500ffu                /* dark style: accents and the highlight */
static int skin;                             /* saved in the settings file */

enum { V_SONGS, V_ARTISTS, V_SCORES, N_VIEWS };
static const char *view_title[N_VIEWS] = { "Songs", "Artists", "Top Scores" };

/* one line of the list: a song row (song >= 0), a section header (-1, hdr
 * is its letter) or the footer at the end (-2) */
typedef struct { int song; char hdr; float y, h; } Entry;
static Entry *ents;
static int *ord;                             /* the songs of the current view, in order */
static int n_ents, n_rows, list_view, built_view = -1, list_dirty = 1;
static float list_total;
static int touch_row = -1, idx_active, press_zone;
enum { Z_LIST, Z_NAV, Z_TAB, Z_INDEX };

static long top_best(int song)               /* the best score of any difficulty */
{
    long top = 0;
    int l;
    for (l = 0; l < 5; l++) {
        const Best *b = best_of(song, l);
        if (b && b->score > top) top = b->score;
    }
    return top;
}

static int letter_rank(char c) { return c == '#' ? 26 : c - 'A'; }

static int cmp_view;
static int song_cmp(const void *pa, const void *pb)
{
    int a = *(const int *)pa, b = *(const int *)pb, d;
    const Song *x = &songs[a], *y = &songs[b];
    if (cmp_view == V_SCORES) {
        long sa = top_best(a), sb = top_best(b);
        if (sa != sb) return sa < sb ? 1 : -1;
    }
    if (cmp_view == V_ARTISTS) {
        if ((d = letter_rank(x->aletter) - letter_rank(y->aletter))) return d;
        if ((d = strcmp(x->akey, y->akey))) return d;
    }
    if ((d = letter_rank(x->tletter) - letter_rank(y->tletter))) return d;
    if ((d = strcmp(x->tkey, y->tkey))) return d;
    return a - b;
}

static int sectioned(void) { return list_view != V_SCORES; }

static void list_build(void)
{
    int i, k, n = 0;
    char last = 0;
    float y = 0;
    if (!ents) {
        ents = malloc(sizeof *ents * (size_t)(n_songs + 30));
        ord = malloc(sizeof *ord * (size_t)(n_songs + 1));
        if (!ents || !ord) { free(ents); free(ord); ents = NULL; ord = NULL; return; }
    }
    for (i = 0; i < n_songs; i++)
        if (list_view != V_SCORES || top_best(i) > 0)
            ord[n++] = i;
    cmp_view = list_view;
    qsort(ord, (size_t)n, sizeof *ord, song_cmp);
    n_ents = 0;
    for (k = 0; k < n; k++) {
        const Song *s = &songs[ord[k]];
        char letter = list_view == V_ARTISTS ? s->aletter : s->tletter;
        Entry *e;
        if (sectioned() && letter != last) {
            e = &ents[n_ents++];
            e->song = -1; e->hdr = letter; e->y = y; e->h = HDR_H;
            y += HDR_H;
            last = letter;
        }
        e = &ents[n_ents++];
        e->song = ord[k]; e->hdr = 0; e->y = y; e->h = ROW_H;
        y += ROW_H;
    }
    ents[n_ents].song = -2; ents[n_ents].hdr = 0; ents[n_ents].y = y; ents[n_ents].h = FOOT_H;
    n_ents++;
    list_total = y + FOOT_H;
    n_rows = n;
    built_view = list_view;
    list_dirty = 0;
}

static int sel_ent(void)
{
    int e;
    for (e = 0; e < n_ents; e++)
        if (ents[e].song == sel_song && sel_song >= 0)
            return e;
    return -1;
}

static int first_row(void)
{
    int e;
    for (e = 0; e < n_ents; e++)
        if (ents[e].song >= 0) return e;
    return -1;
}

static void list_clamp(void)
{
    float max = list_total - (LIST_BOT - LIST_TOP);
    if (list_scroll > max) list_scroll = max;
    if (list_scroll < 0) list_scroll = 0;
}

static void list_show(int e)
{
    float top, bot;
    if (e < 0 || e >= n_ents) return;
    top = ents[e].y;
    if (e > 0 && ents[e - 1].song == -1) top = ents[e - 1].y;   /* show its section header too */
    else if (sectioned()) top -= HDR_H;                         /* not under the sticky header */
    bot = ents[e].y + ents[e].h;
    if (top < list_scroll) list_scroll = top;
    if (bot > list_scroll + (LIST_BOT - LIST_TOP)) list_scroll = bot - (LIST_BOT - LIST_TOP);
    list_clamp();
}

static void list_ensure(void)
{
    if (!list_dirty && built_view == list_view && ents)
        return;
    list_build();
    if (!ents) return;
    if (sel_ent() < 0 && first_row() >= 0)
        sel_song = ents[first_row()].song;
    list_clamp();
}

/* which entry is under a point of the screen (-1: none, or the sticky header) */
static int ent_at(float y)
{
    float ly = y - LIST_TOP + list_scroll;
    int e;
    if (y < LIST_TOP || y >= LIST_BOT || (sectioned() && y < LIST_TOP + HDR_H))
        return -1;
    for (e = 0; e < n_ents; e++)
        if (ly >= ents[e].y && ly < ents[e].y + ents[e].h)
            return e;
    return -1;
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

static void list_choose(void)
{
    sel_level = first_level(songs[sel_song].levels, sel_level ? sel_level : 2);
    plat_sfx("click");
    scene = SC_LEVEL;
}

static void list_set_view(int v)
{
    if (v == list_view) return;
    list_view = v;
    list_scroll = 0;
    list_vel = 0;
    list_ensure();
    list_show(sel_ent());
    plat_sfx("click");
}

static void list_random(void)
{
    if (n_rows <= 0) return;
    srand((unsigned)(now_t * 1000.0));
    sel_song = ord[rand() % n_rows];
    list_show(sel_ent());
    list_choose();
}

/* the A-Z index: touching a letter jumps to that section, or the next one */
#define IDX_TOP (LIST_TOP + 6)
#define IDX_BOT (LIST_BOT - 6)
static void index_jump(float y)
{
    int slot = (int)((y - IDX_TOP) / ((IDX_BOT - IDX_TOP) / 27)), e, last = -1;
    if (slot < 0) slot = 0;
    if (slot > 26) slot = 26;
    for (e = 0; e < n_ents; e++) {
        if (ents[e].song != -1) continue;
        last = e;
        if (letter_rank(ents[e].hdr) >= slot) break;
    }
    if (last < 0) return;
    if (e >= n_ents) e = last;
    list_scroll = ents[e].y;
    list_vel = 0;
    list_clamp();
    if (e + 1 < n_ents && ents[e + 1].song >= 0)
        sel_song = ents[e + 1].song;
}

/* the first row of the section an entry is in */
static int sec_start(int e)
{
    while (e > 0 && ents[e - 1].song >= 0) e--;
    return e;
}

static int list_move(int cur, int step)
{
    int e, k, last = cur;
    if (step == -2 || step == 2) {           /* L / R: a section back or forward */
        if (!sectioned()) step = step < 0 ? -8 : 8;
        else if (step < 0) {
            e = sec_start(cur);
            if (e != cur) return e;
            e -= 2;                          /* skip this section's header */
            return e >= 0 && ents[e].song >= 0 ? sec_start(e) : cur;
        } else {
            for (e = cur + 1; e < n_ents; e++)
                if (ents[e].song == -1 && e + 1 < n_ents && ents[e + 1].song >= 0) return e + 1;
            for (e = n_ents - 1; e > cur; e--)
                if (ents[e].song >= 0) return e;
            return cur;
        }
    }
    for (k = 0, e = cur; k < abs(step); k++) {
        do e += step < 0 ? -1 : 1; while (e >= 0 && e < n_ents && ents[e].song < 0);
        if (e < 0 || e >= n_ents) break;
        last = e;
    }
    return last;
}

static void list_update(const PlatInput *in, double dt)
{
    int step = 0, cur, e;
    list_ensure();
    if (!ents) return;
    cur = sel_ent();
    if (in->pressed & BTN_UP) { step = -1; repeat_t = 0.35; }
    if (in->pressed & BTN_DOWN) { step = 1; repeat_t = 0.35; }
    if (in->held & (BTN_UP | BTN_DOWN)) {    /* hold to keep scrolling */
        repeat_t -= dt;
        if (repeat_t <= 0) { step = (in->held & BTN_UP) ? -1 : 1; repeat_t = 0.06; }
    }
    if (in->pressed & BTN_L) step = -2;
    if (in->pressed & BTN_R) step = 2;
    if (step && cur >= 0) {
        sel_song = ents[list_move(cur, step)].song;
        list_show(sel_ent());
        list_vel = 0;
    }
    if (in->pressed & BTN_TRIANGLE) list_set_view((list_view + 1) % N_VIEWS);
    if (in->pressed & BTN_START) { skin = (skin + 1) % N_SKINS; settings_save(); }
    if (in->pressed & BTN_SQUARE) { list_random(); return; }

    if (ptr.pressed) {
        list_vel = 0;                        /* a finger on the list stops it */
        press_zone = ptr.y < LIST_TOP ? Z_NAV : ptr.y >= LIST_BOT ? Z_TAB :
                     (sectioned() && ptr.x >= LW - IDX_W - 4) ? Z_INDEX : Z_LIST;
    }
    idx_active = ptr.down && press_zone == Z_INDEX;
    if (idx_active) {
        index_jump(ptr.y);
    } else if (ptr.down && !ptr.pressed && press_zone == Z_LIST) {
        float dy = ptr.y - ptr.y0;
        list_scroll -= dy;
        ptr.y0 = ptr.y;
        if (dt > 0.0005)                     /* remember how fast the finger is moving */
            list_vel = list_vel * 0.5f + (float)(-dy / dt) * 0.5f;
        list_clamp();
    } else if (!ptr.down && list_vel != 0) { /* released: keep gliding and slow down */
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

    touch_row = -1;                          /* the row under a resting finger lights up */
    if (ptr.down && press_zone == Z_LIST && ptr.moved < 8 && (e = ent_at(ptr.y)) >= 0 && ents[e].song >= 0)
        touch_row = ents[e].song;

    if (ptr.released && ptr.moved < 8) {
        if (press_zone == Z_LIST) {
            e = ent_at(ptr.y);
            if (e >= 0 && ents[e].song >= 0) {
                sel_song = ents[e].song;
                list_choose();
                return;
            }
        } else if (press_zone == Z_NAV && ptr.x < 90) {
            scene = SC_TITLE;
            return;
        } else if (press_zone == Z_NAV && ptr.x > LW - 90) {
            skin = (skin + 1) % N_SKINS;
            settings_save();
        } else if (press_zone == Z_TAB) {
            int t = (int)(ptr.x / (LW / 4));
            if (t == 3) { list_random(); return; }
            if (t >= 0 && t < N_VIEWS) list_set_view(t);
        }
    }
    if ((in->pressed & BTN_CROSS) && sel_ent() >= 0)
        list_choose();
    else if (in->pressed & BTN_CIRCLE)
        scene = SC_TITLE;
}

/* ------------------------------------------------------- list drawing -- */

static unsigned mix(unsigned a, unsigned b, float t)
{
    unsigned r = 0;
    int sh;
    for (sh = 0; sh < 32; sh += 8) {
        float ca = (float)((a >> sh) & 255), cb = (float)((b >> sh) & 255);
        r |= (unsigned)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return r;
}

/* A filled shape built from 1-point strips, its colour going from top to
 * bottom: rounded corners of radius r, and an optional point on one side
 * (arrow > 0: pointing left, < 0: pointing right; its size is the depth). */
static void shape(float x, float y, float w, float h, float r, float arrow, unsigned top, unsigned bot)
{
    int i, n = (int)ceilf(h);
    float lap = (top & 255) == 255 && (bot & 255) == 255 ? 0.15f : 0.0f;   /* hide seams; not when see-through */
    for (i = 0; i < n; i++) {
        float yc = i + 0.5f, d, rr = 0, l, rt, sh = i == n - 1 ? h - i : 1.0f + lap;
        d = yc < r ? r - yc : yc > h - r ? yc - (h - r) : 0;
        if (d > 0) rr = r - sqrtf(fmaxf(r * r - d * d, 0));
        l = rt = rr;
        if (arrow > 0) l = arrow * fabsf(yc - h / 2) / (h / 2);
        if (arrow < 0) rt = -arrow * fabsf(yc - h / 2) / (h / 2);
        fill(x + l, y + i, w - l - rt, sh, mix(top, bot, n > 1 ? (float)i / (n - 1) : 0));
    }
}

/* text cut to max_w with "..." at the end; bold draws it twice, a hair apart */
static void text_fit(float x, float y, float size, int align, float max_w, unsigned rgba, const char *s, int bold)
{
    char buf[96];
    if (max_w > 0 && text_width(s, size) > max_w) {
        size_t n = strlen(s);
        if (n > sizeof buf - 4) n = sizeof buf - 4;
        memcpy(buf, s, n);
        for (;;) {
            while (n > 0 && buf[n - 1] == ' ') n--;
            strcpy(buf + n, "...");
            if (n == 0 || text_width(buf, size) <= max_w) break;
            n--;
        }
        s = buf;
    }
    text(x, y, size, align, 0, rgba, s);
    if (bold) text(x + size * 0.04f, y, size, align, 0, rgba, s);
}

/* text with a one-point shadow (engraved or embossed look) */
static void text_sh(float x, float y, float size, int align, float max_w, unsigned rgba, unsigned shadow, float dy, const char *s)
{
    text_fit(x, y + dy, size, align, max_w, shadow, s, 1);
    text_fit(x, y, size, align, max_w, rgba, s, 1);
}

static void nav_button(float x, float w, float arrow, const char *label)
{
    const float y = 7, h = 30;
    unsigned edge, top, bot, tc, sc;
    float a = arrow > 0 ? arrow : 0, b = arrow < 0 ? -arrow : 0, sdy;
    if (skin == SKIN_SILVER) {
        edge = 0x6b7378ffu; top = 0xdde2e6ffu; bot = 0x9ca6aeffu; tc = 0x4a5156ffu; sc = 0xffffffb0u; sdy = 1;
    } else if (skin == SKIN_DARK) {
        edge = 0x000000ffu; top = 0x3a3a3effu; bot = 0x222225ffu; tc = DARK_ORANGE; sc = 0x000000c0u; sdy = -1;
    } else if (arrow < 0) {                  /* the black "now playing" style */
        edge = 0x0a0a0affu; top = 0x4a4a4affu; bot = 0x0c0c0cffu; tc = 0xffffffffu; sc = 0x000000c0u; sdy = -1;
    } else {
        edge = 0x34465fffu; top = 0x8fa3c0ffu; bot = 0x4b6592ffu; tc = 0xffffffffu; sc = 0x1d2a3cc0u; sdy = -1;
    }
    shape(x, y, w, h, 5, arrow, edge, edge);
    shape(x + 1, y + 1, w - 2, h - 2, 4, arrow > 0 ? arrow - 1 : arrow < 0 ? arrow + 1 : 0, top, bot);
    text_sh(x + a + (w - a - b) / 2, y + 7, 14, 1, w - a - b - 6, tc, sc, sdy, label);
}

static void draw_nav(const char *title)
{
    if (skin == SKIN_SILVER) {
        shape(0, 0, LW, NAV_H, 0, 0, 0xeef0f2ffu, 0xb2bac1ffu);
        fill(0, 0, LW, 1, 0xfafbfcffu);
        fill(0, NAV_H - 1, LW, 1, 0x5e656affu);
        text_sh(LW / 2, 10, 21, 1, LW - 180, 0x4a5156ffu, 0xffffffc0u, 1, title);
    } else if (skin == SKIN_DARK) {
        shape(0, 0, LW, NAV_H, 0, 0, 0x2c2c30ffu, 0x161618ffu);
        fill(0, 0, LW, 1, 0x48484cffu);
        fill(0, NAV_H - 1, LW, 1, 0x000000ffu);
        text_sh(LW / 2, 10, 21, 1, LW - 180, 0xf2f2f2ffu, 0x000000d0u, -1, title);
    } else {
        shape(0, 0, LW, NAV_H, 0, 0, 0xb4c0d1ffu, 0x6c82a1ffu);
        fill(0, 0, LW, 1, 0xd2dae4ffu);
        fill(0, NAV_H - 1, LW, 1, 0x2d3642ffu);
        text_sh(LW / 2, 10, 21, 1, LW - 180, 0xffffffffu, 0x3a4a60d0u, -1, title);
    }
    if (skin != SKIN_BLUE) nav_button(6, 64, 11, "Back");
    else nav_button(6, 60, 0, "Back");
    nav_button(LW - 70, 64, skin == SKIN_BLUE ? -11 : 0, "Style");
}

static void draw_header(float y, char letter)
{
    char b[2];
    b[0] = letter; b[1] = 0;
    if (skin == SKIN_SILVER) {
        shape(0, y, LW, HDR_H, 0, 0, 0xb7c1c8ffu, 0xa2aeb7ffu);
        fill(0, y, LW, 1, 0x9aa6afffu);
        fill(0, y + HDR_H - 1, LW, 1, 0x8f9ba4ffu);
    } else if (skin == SKIN_DARK) {
        shape(0, y, LW, HDR_H, 0, 0, 0x2a2a2dffu, 0x1f1f22ffu);
        fill(0, y, LW, 1, 0x3a3a3effu);
        fill(0, y + HDR_H - 1, LW, 1, 0x0c0c0effu);
        text_sh(11, y + 1, 18, 0, 0, DARK_ORANGE, 0x000000c0u, 1, b);
        return;
    } else {                                 /* see-through, the rows show behind it */
        shape(0, y, LW, HDR_H, 0, 0, 0x9aa6b1d8u, 0x8593a0d8u);
        fill(0, y + HDR_H - 1, LW, 1, 0x7a8794e0u);
    }
    text_sh(11, y + 1, 18, 0, 0, 0xffffffffu, 0x55626cb0u, 1, b);
}

static void draw_art(float x, float y, int song)
{
    static const unsigned pal[8] = {
        0x8e5b4dffu, 0x4f6d8cffu, 0x6c8a4bffu, 0x8a6aa0ffu, 0xb0874affu, 0x4d8a86ffu, 0x9a4f63ffu, 0x5d5d72ffu
    };
    const char *p;
    unsigned h = 0, c;
    int i;
    char b[2] = { '?', 0 };
    for (p = songs[song].artist; *p; p++) h = h * 31 + (unsigned char)*p;
    for (p = songs[song].artist; *p; p++)
        if (isalnum((unsigned char)*p)) { b[0] = (char)toupper((unsigned char)*p); break; }
    i = atoi(songs[song].num) - 1;
    if (tx_art.t && i >= 0 && i < art_tiles && art_have[i]) {
        draw(&tx_art, (i % ART_COLS) * ART_TILE, (i / ART_COLS) * ART_TILE, ART_TILE, ART_TILE,
             x + ROW_H / 2, y + (ROW_H - 1) / 2, ROW_H, ROW_H - 1, 0, 0xffffffffu, 0);
        return;
    }
    c = pal[h & 7];
    shape(x, y, ROW_H, ROW_H - 1, 0, 0, c, mix(c, 0x000000ffu, 0.3f));
    text_fit(x + ROW_H / 2, y + 8, 26, 1, 0, 0xffffffd8u, b, 1);
}

static void draw_row(float y, int song, int lit, float right)
{
    const Song *s = &songs[song];
    unsigned tc = 0x000000ffu, ac = 0x808080ffu, sc = 0x385487ffu;
    const char *l1 = list_view == V_ARTISTS ? s->artist : s->title;
    const char *l2 = list_view == V_ARTISTS ? s->title : s->artist;
    long top = top_best(song);
    float tx = skin != SKIN_BLUE ? ROW_H + 8 : 11, sw = 0;
    char buf[24];
    if (skin == SKIN_DARK) {
        tc = 0xf2f2f2ffu; ac = 0x8e8e93ffu; sc = DARK_ORANGE;
    }
    if (lit && skin == SKIN_DARK) {          /* dark text: white on orange is hard to read */
        shape(0, y, LW, ROW_H, 0, 0, 0xffa31affu, 0xf07800ffu);
        tc = 0x140c00ffu; ac = 0x4a2a00ffu; sc = 0x140c00ffu;
    } else if (lit) {
        shape(0, y, LW, ROW_H, 0, 0, 0x058cf5ffu, 0x015ee6ffu);
        tc = ac = sc = 0xffffffffu;
    } else {
        fill(0, y + ROW_H - 1, LW, 1, skin == SKIN_DARK ? 0x2c2c2fffu : 0xd9d9d9ffu);
    }
    if (skin != SKIN_BLUE)
        draw_art(0, y, song);
    if (top > 0) {
        snprintf(buf, sizeof buf, "%ld", top);
        sw = text_width(buf, 14) + 8;
        text(right, y + 15, 14, 2, 0, sc, buf);
    }
    text_fit(tx, y + 3, 20, 0, right - tx - sw, tc, l1, 1);
    text_fit(tx, y + 24, 15, 0, right - tx - sw, ac, l2, 0);
}

static void draw_footer(float y)
{
    char buf[48];
    if (list_view == V_SCORES && n_rows == 0)
        snprintf(buf, sizeof buf, "No scores yet");
    else
        snprintf(buf, sizeof buf, "%d %s", n_rows, list_view == V_SCORES ? "Played" : n_rows == 1 ? "Song" : "Songs");
    text_fit(LW / 2, y + 14, 18, 1, 0, skin == SKIN_DARK ? 0x6e6e73ffu : 0x8c8c8cffu, buf, 0);
}

static float lit_y0 = -1, lit_y1 = -1;       /* where the highlighted row is on screen */

static void draw_index(void)
{
    float step = (IDX_BOT - IDX_TOP) / 27;
    int k;
    if (idx_active)
        shape(LW - IDX_W + 1, LIST_TOP + 2, IDX_W - 3, IDX_BOT - IDX_TOP + 8, (IDX_W - 3) / 2, 0,
              skin == SKIN_DARK ? 0xffffff30u : 0x00000038u, skin == SKIN_DARK ? 0xffffff30u : 0x00000038u);
    for (k = 0; k < 27; k++) {
        char b[2];
        b[0] = k < 26 ? (char)('A' + k) : '#'; b[1] = 0;
        float ly = IDX_TOP + k * step + (step - 11) / 2, mid = ly + 5.5f;
        unsigned c = 0x6b7885ffu;
        if (skin == SKIN_DARK)               /* orange, but dark over the orange highlight */
            c = mid >= lit_y0 && mid < lit_y1 ? 0x140c00ffu : DARK_ORANGE;
        text_fit(LW - IDX_W / 2, ly, 11, 1, 0, c, b, 1);
    }
}

static void tab_icon(int t, float cx, float y, unsigned c)
{
    switch (t) {
    case 0:                                  /* a note */
        fill(cx + 1, y + 3, 2.5f, 18, c);
        shape(cx + 1, y + 3, 8, 4, 0, 0, c, c);
        fill(cx + 7, y + 3, 2, 8, c);
        shape(cx - 7, y + 16, 10.5f, 8, 4, 0, c, c);
        break;
    case 1:                                  /* a person */
        shape(cx - 5, y + 1, 10, 11, 5, 0, c, c);
        shape(cx - 10, y + 13, 20, 12, 6, 0, c, c);
        break;
    case 2:                                  /* score bars */
        fill(cx - 10, y + 15, 5, 10, c);
        fill(cx - 2.5f, y + 8, 5, 17, c);
        fill(cx + 5, y + 2, 5, 23, c);
        break;
    default:                                 /* a die: pick one at random */
        shape(cx - 10, y + 3, 20, 20, 4, 0, c, c);
        fill(cx - 6, y + 7, 3.5f, 3.5f, 0x222222ffu);
        fill(cx + 2.5f, y + 7, 3.5f, 3.5f, 0x222222ffu);
        fill(cx - 1.75f, y + 11.25f, 3.5f, 3.5f, 0x222222ffu);
        fill(cx - 6, y + 15.5f, 3.5f, 3.5f, 0x222222ffu);
        fill(cx + 2.5f, y + 15.5f, 3.5f, 3.5f, 0x222222ffu);
        break;
    }
}

static void draw_tabs(void)
{
    static const char *lab[4] = { "Songs", "Artists", "Scores", "Shuffle" };
    float y = LIST_BOT, tw = LW / 4;
    int t;
    shape(0, y, LW, TAB_H / 2, 0, 0, 0x3e3e3effu, 0x1e1e1effu);
    fill(0, y + TAB_H / 2, LW, TAB_H / 2, 0x0a0a0affu);
    fill(0, y, LW, 1, 0x111111ffu);
    fill(0, y + 1, LW, 1, 0x555555ffu);
    for (t = 0; t < 4; t++) {
        float x = t * tw;
        int sel = t == list_view;
        unsigned ic = !sel ? 0x9a9a9affu : skin == SKIN_BLUE ? 0x3ea6ffffu : skin == SKIN_DARK ? DARK_ORANGE : 0xffffffffu;
        if (sel) shape(x + 3, y + 3, tw - 6, TAB_H - 5, 3, 0, 0x5a5a5aa0u, 0x3a3a3aa0u);
        tab_icon(t, x + tw / 2, y + 4, ic);
        text_fit(x + tw / 2, y + 32, 11, 1, 0, sel ? 0xffffffffu : 0x9a9a9affu, lab[t], 1);
    }
}

static void list_draw(void)
{
    int e, sticky = -1, nxt;
    float right;
    list_ensure();
    fill(0, 0, LW, LH, skin == SKIN_DARK ? 0x111113ffu : 0xffffffffu);
    if (!ents) return;
    right = LW - (sectioned() ? IDX_W + 4 : 10);
    lit_y0 = lit_y1 = -1;
    for (e = 0; e < n_ents; e++) {
        float y = LIST_TOP + ents[e].y - list_scroll;
        if (ents[e].song == -1 && ents[e].y <= list_scroll) sticky = e;
        if (y + ents[e].h < LIST_TOP || y > LIST_BOT) continue;
        if (ents[e].song >= 0) {
            int lit = ents[e].song == sel_song || ents[e].song == touch_row;
            if (lit && ents[e].song == sel_song) { lit_y0 = y; lit_y1 = y + ROW_H; }
            draw_row(y, ents[e].song, lit, right);
        }
        else if (ents[e].song == -1) { if (ents[e].y > list_scroll) draw_header(y, ents[e].hdr); }   /* else the sticky copy covers it */
        else draw_footer(y);
    }
    if (sticky >= 0) {                       /* the current section's header stays on top */
        float y = LIST_TOP;
        for (nxt = sticky + 1; nxt < n_ents && ents[nxt].song != -1; nxt++) ;
        if (nxt < n_ents) {
            float ny = LIST_TOP + ents[nxt].y - list_scroll;
            if (ny < LIST_TOP + HDR_H) y = ny - HDR_H;
        }
        draw_header(y, ents[sticky].hdr);
    }
    if (sectioned()) draw_index();
    draw_nav(view_title[list_view]);
    draw_tabs();
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
        {
            const Best *b = best_of(sel_song, l);
            char buf[32];
            fill(60, 190 + row * 50.0f, 200, 44, l == sel_level ? 0x2050a0ffu : 0x282838ffu);
            if (b && b->score > 0) {
                text(LW / 2, 194 + row * 50.0f, 20, 1, 0, 0xffffffffu, level_name[l]);
                snprintf(buf, sizeof buf, "best %ld", b->score);
                text(LW / 2, 216 + row * 50.0f, 13, 1, 0, 0xffd040ffu, buf);
            } else {
                text(LW / 2, 200 + row * 50.0f, 22, 1, 0, 0xffffffffu, level_name[l]);
            }
            row++;
        }
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
    if (new_best && prev_best.score > 0) {
        snprintf(buf, sizeof buf, "NEW BEST  (was %ld)", prev_best.score);
        text(LW / 2, 300, 19, 1, 0, 0xffd040ffu, buf);
    } else if (new_best) {
        text(LW / 2, 300, 19, 1, 0, 0xffd040ffu, "NEW BEST");
    } else if (prev_best.score > 0) {
        int bp = prev_best.judged ? (int)(100.0 * prev_best.hit / prev_best.judged + 0.5) : 0;
        snprintf(buf, sizeof buf, "Your best: %ld  (%d%%, streak %d)", prev_best.score, bp, prev_best.streak);
        text(LW / 2, 300, 15, 1, 0, 0x90a0c0ffu, buf);
    }
    text(LW / 2, 400, 15, 1, 0, 0x90a0c0ffu, "Tap or press X to continue");
}

/* --------------------------------------------------------- entry points -- */

/* settings.txt: the screen layout, then one "best" line per song+difficulty */
static void settings_load(void)
{
    char line[256], num[8];
    int level, streak, hit, judged, song;
    long sc;
    FILE *fp = fopen(plat_save_file(), "r");
    if (!fp)
        return;
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, "tate %d", &level) == 1) {
            tate = level != 0;
        } else if (sscanf(line, "skin %d", &level) == 1) {
            skin = level >= 0 && level < N_SKINS ? level : 0;
        } else if (sscanf(line, "best %7s %d %ld %d %d %d", num, &level, &sc, &streak, &hit, &judged) == 6) {
            Best *b;
            song = song_by_num(num);        /* a song no longer present is dropped */
            b = best_of(song, level);
            if (b && sc >= b->score) {
                b->score = sc; b->streak = streak; b->hit = hit; b->judged = judged;
            }
        }
    }
    fclose(fp);
}

static void settings_save(void)
{
    FILE *fp = fopen(plat_save_file(), "w");
    int i, l;
    if (!fp) {
        fprintf(stderr, "cannot write %s\n", plat_save_file());
        return;
    }
    fprintf(fp, "tate %d\nskin %d\n", tate, skin);
    for (i = 0; i < n_songs; i++)
        for (l = 0; l < 5; l++) {
            const Best *b = best_of(i, l);
            if (b && b->score > 0)
                fprintf(fp, "best %s %d %ld %d %d %d\n", songs[i].num, l, b->score, b->streak, b->hit, b->judged);
        }
    fclose(fp);
}

int game_init(int sw, int sh)
{
    scr_w = sw;
    scr_h = sh;
    view_setup();
    if (tex_load(&tx_bg, "background") || tex_load(&tx_hud, "hud") || tex_load(&tx_font, "font") ||
        font_load() || songs_load())
        return -1;
    art_load();
    bests = calloc((size_t)n_songs * 5 + 1, sizeof *bests);
    if (!bests)
        return -1;
    settings_load();
    view_setup();                            /* the layout may have come from the file */
    sel_song = -1;                           /* the list starts on its first row */
    list_view = V_SONGS;
    list_scroll = 0;
    list_dirty = 1;
    scene = SC_TITLE;
    return 0;
}

void game_shutdown(void)
{
    settings_save();
    plat_music_stop();
    theme_free();
    plat_tex_free(tx_bg.t); plat_tex_free(tx_hud.t); plat_tex_free(tx_font.t); plat_tex_free(tx_art.t);
    tx_art.t = NULL;
    free(art_have);
    art_have = NULL;
    art_tiles = 0;
    free(songs); free(notes); free(bests); free(ents); free(ord);
    songs = NULL; notes = NULL; bests = NULL; ents = NULL; ord = NULL;
    n_songs = n_notes = n_ents = n_rows = 0;
    built_view = -1;
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
    int e, row = -1, k = 0;
    if (songs) {                             /* the cursor's place in the list, and its song number */
        list_ensure();
        for (e = 0; e < n_ents; e++)
            if (ents[e].song >= 0) { if (ents[e].song == sel_song) { row = k; break; } k++; }
    }
    snprintf(buf, (size_t)n, "row=%d num=%s view=%d skin=%d scene=%d song=%d level=%d t=%.2f notes=%d score=%ld streak=%d best=%d mult=%d hit=%d judged=%d revenge=%d paused=%d",
             row, sel_song >= 0 && sel_song < n_songs ? songs[sel_song].num : "-", list_view, skin,
             scene, sel_song, sel_level, song_t, n_notes, score, streak, best_streak, mult, n_hit, n_judged, revenge, paused);
}
