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

/* The menus fill the whole screen: 480 points tall in the landscape layout,
 * the width following the screen; 320 points wide in the vertical layout,
 * the height following it.  A song switches to its theme's own size. */
static void menu_size(void)
{
    if (tate) {
        cur_w = 320.0f;
        cur_h = 320.0f * (float)scr_w / (float)scr_h;
    } else {
        cur_h = 480.0f;
        cur_w = 480.0f * (float)scr_w / (float)scr_h;
    }
    view_setup();
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
static Tex tx_hud, tx_font;

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
#ifdef __vita__
    if (!px)                                 /* the copy the build put in the VPK */
        px = png_load_rgba("app0:app_data/gfx/art.png", &w, &h);
#endif
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
static float acc_off;                        /* the last hit: seconds late (+) or early (-) */
static int acc_miss;                         /* the last judgement was a miss */
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
    acc_miss = 1;
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
    acc_off = 0;
    acc_miss = 0;
    memset(lane_was, 0, sizeof lane_was);
    memset(splash_t, 0, sizeof splash_t);
    snprintf(path, sizeof path, "%saudio/%s.ogg", plat_data_dir(), s->num);
    play_started = plat_music_play(path) == 0;
    scene = SC_PLAY;
}

static void settings_save(void);

static int list_dirty;                       /* the song list must be rebuilt (menus, below) */
static int *queue, n_queue, queue_pos = -1;  /* Play All / Shuffle: songs to play in turn (menus) */
static double next_at;                       /* when the results screen moves on to the next one */
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
    menu_size();                             /* back to the menus' size */
    paused = 0;
    scene = to_scene;
    if (to_scene == SC_RESULT)
        next_at = now_t + 8.0;               /* QUEUE_WAIT, below */
    else
        queue_pos = -1;                      /* quitting a song ends Play All / Shuffle */
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
        acc_off = (float)-d;                 /* d: note time minus now, so early is positive */
        acc_miss = 0;
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

static void queue_next(void);

/* the "skip" button on the pause screen, during Play All / Shuffle */
#define SKIP_Y (LH * 0.40f + 78)
#define SKIP_H 40.0f

static void play_update(const PlatInput *in, double dt)
{
    int i, lane;

    if (in->pressed & BTN_START) {
        paused = !paused;
        plat_music_pause(paused);
        plat_sfx("click");
    }
    if (paused) {
        if (queue_pos >= 0 && ((in->pressed & BTN_TRIANGLE) ||
            (ptr.released && ptr.moved < 8 && ptr.y >= SKIP_Y && ptr.y < SKIP_Y + SKIP_H &&
             fabsf(ptr.x - LW / 2) < 120))) {
            int keep = queue_pos;            /* skip: no result, straight to the next song */
            plat_sfx("click");
            end_song(SC_LIST);               /* (which ends the queue: put it back) */
            queue_pos = keep;
            queue_next();
            return;
        }
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

    /* HUD: the game's own, from the HUD sheet (hudmatrix.png), where the
     * original drew it: a solid bar along the top, the multiplier tube on the
     * left and the timing tube on the right.  Some themes have a mock-up of
     * it painted into their background (one even says "REMOVE THIS"); the
     * original covered that with the real thing, and so must we. */
    {
        float bx = (LW - 320) / 2, prog = chart_end > 0 ? (float)(song_t / chart_end) : 0;
        int mf = mult >= 16 ? 5 : mult >= 8 ? 4 : mult >= 2 ? mult - 1 : 0;   /* tube: empty, 2x, 3x, 4x, 8x, 16x */
        int af = 4;                                                             /* timing: 4 is spot on */
        if (acc_miss) {
            af = 0;                          /* red: a miss */
        } else if (n_hit) {
            float r = acc_off / (float)WIN_OK;   /* -1 early .. +1 late */
            af = r < -0.6f ? 1 : r < -0.3f ? 2 : r < -0.1f ? 3 : r <= 0.1f ? 4 : r <= 0.3f ? 5 : r <= 0.6f ? 6 : 7;
        }
        if (prog < 0) prog = 0;
        if (prog > 1) prog = 1;
        fill(0, 0, LW, 40, 0x000000ffu);
        draw(&tx_hud, 0, 0, 320, 40, bx + 160, 20, 320, 40, 0, 0xffffffffu, 0);
        fill(bx + 41, 34.5f, 238 * prog, 3, 0x40c0ffffu);             /* song progress, in its slot */
        snprintf(buf, sizeof buf, "%d", streak);
        text(bx + 6, 1, 19, 0, 0, 0xffffffffu, buf);
        snprintf(buf, sizeof buf, "%d", n_judged ? (int)(100.0 * n_hit / n_judged + 0.5) : 100);
        text(bx + 314, 1, 19, 2, 0, 0xffffffffu, buf);
        snprintf(buf, sizeof buf, "%ld", score);
        text(bx + 160, 3, 24, 1, 0, mult >= 8 ? 0xffd040ffu : 0xffffffffu, buf);
        draw(&tx_hud, 321 + 22 * mf, 0, 20, 109, bx + 19, 38 + 54.5f, 20, 109, 0, 0xffffffffu, 0);
        draw(&tx_hud, 321 + 22 * af, 119, 20, 119, bx + 299, 38 + 59.5f, 20, 119, 0, 0xffffffffu, 0);
        draw(&tx_hud, 500, 121, 12, 93, bx + 314, 40 + 46.5f, 12, 93, 0, 0xffffffffu, 0);   /* EARLY / LATE */
    }
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
        if (queue_pos < 0) {
            text(LW / 2, LH * 0.40f + 45, 15, 1, 0, 0xc0c0c0ffu, "START: continue    O: quit song");
        } else {                             /* Play All / Shuffle: a skip button too */
            char nb[96];
            int last = queue_pos + 1 >= n_queue;
            text(LW / 2, LH * 0.40f + 45, 15, 1, 0, 0xc0c0c0ffu, "START: continue    O: stop playing");
            fill(LW / 2 - 120, SKIP_Y, 240, SKIP_H, 0xffffffffu);
            fill(LW / 2 - 118, SKIP_Y + 2, 236, SKIP_H - 4, 0x2a2a2effu);
            text(LW / 2, SKIP_Y + 9, 18, 1, 0, 0xffffffffu, last ? "Skip (last song)" : "Skip song  (Triangle)");
            if (!last) {
                snprintf(nb, sizeof nb, "Next: %s", songs[queue[queue_pos + 1]].title);
                text(LW / 2, SKIP_Y + SKIP_H + 10, 14, 1, LW - 20, 0xa8a8a8ffu, nb);
            }
        }
    }
}

/* ---------------------------------------------------------------- menus -- */

/* The song list looks like a classic phone music library: a title bar,
 * alphabetical sections with a header that sticks to the top, an A-Z index
 * down the right, rows with a cover, and a tab bar.  Three styles:
 * SKIN_SILVER (light grey bars, a cover on each row), SKIN_BLUE (blue-grey
 * bars, see-through section headers, text-only rows) and SKIN_DARK
 * (near-black, covers, orange highlight and accents).
 *
 * Tabs: Playlists, Songs, Artists, Top Scores, and Shuffle (plays every song
 * in a random order).  An artist or a playlist opens a page of its songs
 * with Play All and Shuffle at the top; those play the songs one after
 * another (the "queue", see the results screen).  Everything is drawn with
 * plain fills, so no extra artwork is needed. */

#define NAV_H    44.0f
#define TAB_H    49.0f
#define HDR_H    22.0f
#define ROW_H    44.0f
#define FOOT_H   52.0f
#define IDX_W    18.0f
#define LIST_TOP NAV_H
#define LIST_BOT (LH - TAB_H)
#define N_TABS   5

enum { SKIN_SILVER, SKIN_BLUE, SKIN_DARK, N_SKINS };
#define DARK_ORANGE 0xff9500ffu                /* dark style: accents and the highlight */
static int skin;                             /* saved in the settings file */

enum { V_SONGS, V_ARTISTS, V_SCORES, V_PLAYLISTS, N_VIEWS };
static const char *view_title[N_VIEWS] = { "Songs", "Artists", "Top Scores", "Playlists" };
static const int tab_view[N_TABS] = { V_PLAYLISTS, V_SONGS, V_ARTISTS, V_SCORES, -1 };   /* -1: Shuffle */
static const char *tab_label[N_TABS] = { "Playlists", "Songs", "Artists", "Scores", "Shuffle" };

/* one line of the list */
enum { E_SONG, E_HDR, E_FOOT, E_ARTIST, E_PLIST, E_ACT };
enum { A_PLAY_ALL, A_SHUFFLE, A_NEW_PLIST, A_DEL_PLIST };
typedef struct {
    int kind;
    int arg;                                 /* song, playlist, or A_* action */
    int count;                               /* artist: songs; song on a playlist page: its place */
    char hdr;                                /* section header: its letter */
    float y, h;
} Entry;
static Entry *ents;
static int *ord;                             /* songs being sorted */
static int n_ents, n_rows, list_view, built_view = -1;
static float list_total;
static int touch_row = -1, idx_active, press_zone;
static char foot_text[64];
enum { Z_LIST, Z_NAV, Z_TAB, Z_INDEX };

/* page: 0 = the tab's own list, 1 = one artist's or one playlist's songs */
static int list_page, page_song = -1, page_pl = -1, edit_mode;
static float top_scroll;                     /* where the tab's list was, to go back to */
static int top_kind = -1, top_arg = -1;

/* the cursor: which item it is on, so it survives rebuilding the list */
static int cur_kind = E_SONG, cur_arg = -1;

/* playlists, saved in the settings file */
#define MAX_PL 24
#define MAX_PL_SONGS 512
typedef struct { char name[32]; int n; short song[MAX_PL_SONGS]; } Playlist;
static Playlist pls[MAX_PL];
static int n_pls;

static int pl_has(int p, int song)
{
    int k;
    for (k = 0; k < pls[p].n; k++)
        if (pls[p].song[k] == song) return 1;
    return 0;
}

static int pl_new(void)                      /* "Playlist 1", "Playlist 2"...: the first free number */
{
    int k, p;
    char name[32];
    if (n_pls >= MAX_PL) return -1;
    for (k = 1; ; k++) {
        snprintf(name, sizeof name, "Playlist %d", k);
        for (p = 0; p < n_pls && strcmp(pls[p].name, name); p++) ;
        if (p == n_pls) break;
    }
    snprintf(pls[n_pls].name, sizeof pls[0].name, "%s", name);
    pls[n_pls].n = 0;
    return n_pls++;
}

static void pl_add(int p, int song)
{
    if (p >= 0 && p < n_pls && song >= 0 && !pl_has(p, song) && pls[p].n < MAX_PL_SONGS)
        pls[p].song[pls[p].n++] = (short)song;
}

static void pl_remove_at(int p, int k)
{
    if (p < 0 || p >= n_pls || k < 0 || k >= pls[p].n) return;
    memmove(&pls[p].song[k], &pls[p].song[k + 1], sizeof pls[p].song[0] * (size_t)(pls[p].n - k - 1));
    pls[p].n--;
}

static void pl_remove_song(int p, int song)
{
    int k;
    for (k = 0; k < pls[p].n; k++)
        if (pls[p].song[k] == song) { pl_remove_at(p, k); return; }
}

static void pl_delete(int p)
{
    if (p < 0 || p >= n_pls) return;
    memmove(&pls[p], &pls[p + 1], sizeof pls[0] * (size_t)(n_pls - p - 1));
    n_pls--;
}

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

static int same_artist(int a, int b) { return !strcmp(songs[a].akey, songs[b].akey); }
static int sectioned(void) { return !list_page && (list_view == V_SONGS || list_view == V_ARTISTS); }
static int on_playlist_page(void) { return list_page && list_view == V_PLAYLISTS && page_pl >= 0 && page_pl < n_pls; }
static int selectable(int e) { return e >= 0 && e < n_ents && ents[e].kind != E_HDR && ents[e].kind != E_FOOT; }

static void ent_add(int kind, int arg, int count, char hdr, float h)
{
    Entry *e = &ents[n_ents++];
    e->kind = kind; e->arg = arg; e->count = count; e->hdr = hdr;
    e->y = list_total; e->h = h;
    list_total += h;
    if (kind == E_SONG) n_rows++;
}

/* the songs in ord[0..n) as rows, with a letter header wherever the letter changes */
static void add_songs(int n, int headers)
{
    int k;
    char last = 0;
    for (k = 0; k < n; k++) {
        char letter = songs[ord[k]].tletter;
        if (headers && letter != last) { ent_add(E_HDR, 0, 0, letter, HDR_H); last = letter; }
        ent_add(E_SONG, ord[k], k, 0, ROW_H);
    }
}

static void list_build(void)
{
    int i, k, n = 0;
    if (!ents) {
        ents = malloc(sizeof *ents * (size_t)(2 * n_songs + MAX_PL + 40));
        ord = malloc(sizeof *ord * (size_t)(n_songs + 1));
        if (!ents || !ord) { free(ents); free(ord); ents = NULL; ord = NULL; return; }
    }
    n_ents = n_rows = 0;
    list_total = 0;
    if (list_page && list_view == V_ARTISTS && page_song >= 0) {
        for (i = 0; i < n_songs; i++)
            if (same_artist(i, page_song)) ord[n++] = i;
        cmp_view = V_SONGS;
        qsort(ord, (size_t)n, sizeof *ord, song_cmp);
        ent_add(E_ACT, A_PLAY_ALL, 0, 0, ROW_H);
        ent_add(E_ACT, A_SHUFFLE, 0, 0, ROW_H);
        add_songs(n, 0);
        snprintf(foot_text, sizeof foot_text, "%d %s", n, n == 1 ? "Song" : "Songs");
    } else if (on_playlist_page()) {
        const Playlist *p = &pls[page_pl];
        if (p->n && !edit_mode) {
            ent_add(E_ACT, A_PLAY_ALL, 0, 0, ROW_H);
            ent_add(E_ACT, A_SHUFFLE, 0, 0, ROW_H);
        }
        for (k = 0; k < p->n; k++)
            ent_add(E_SONG, p->song[k], k, 0, ROW_H);
        if (edit_mode)
            ent_add(E_ACT, A_DEL_PLIST, 0, 0, ROW_H);
        if (p->n)
            snprintf(foot_text, sizeof foot_text, "%d %s", p->n, p->n == 1 ? "Song" : "Songs");
        else
            snprintf(foot_text, sizeof foot_text, "Add songs from a song's screen");
    } else if (list_view == V_PLAYLISTS) {
        ent_add(E_ACT, A_NEW_PLIST, 0, 0, ROW_H);
        for (i = 0; i < n_pls; i++)
            ent_add(E_PLIST, i, pls[i].n, 0, ROW_H);
        if (n_pls)
            snprintf(foot_text, sizeof foot_text, "%d %s", n_pls, n_pls == 1 ? "Playlist" : "Playlists");
        else
            snprintf(foot_text, sizeof foot_text, "No playlists yet");
    } else if (list_view == V_ARTISTS) {
        char last = 0;
        int artists = 0;
        for (i = 0; i < n_songs; i++) ord[n++] = i;
        cmp_view = V_ARTISTS;
        qsort(ord, (size_t)n, sizeof *ord, song_cmp);
        for (k = 0; k < n; k = i) {          /* one row per artist */
            char letter = songs[ord[k]].aletter;
            for (i = k + 1; i < n && same_artist(ord[i], ord[k]); i++) ;
            if (letter != last) { ent_add(E_HDR, 0, 0, letter, HDR_H); last = letter; }
            ent_add(E_ARTIST, ord[k], i - k, 0, ROW_H);
            artists++;
        }
        snprintf(foot_text, sizeof foot_text, "%d %s", artists, artists == 1 ? "Artist" : "Artists");
    } else {
        for (i = 0; i < n_songs; i++)
            if (list_view != V_SCORES || top_best(i) > 0) ord[n++] = i;
        cmp_view = list_view;
        qsort(ord, (size_t)n, sizeof *ord, song_cmp);
        add_songs(n, list_view == V_SONGS);
        if (list_view == V_SCORES && n == 0)
            snprintf(foot_text, sizeof foot_text, "No scores yet");
        else
            snprintf(foot_text, sizeof foot_text, "%d %s", n, list_view == V_SCORES ? "Played" : n == 1 ? "Song" : "Songs");
    }
    ent_add(E_FOOT, 0, 0, 0, FOOT_H);
    built_view = list_view;
    list_dirty = 0;
}

static int cur_ent(void)
{
    int e;
    for (e = 0; e < n_ents; e++)
        if (selectable(e) && ents[e].kind == cur_kind && ents[e].arg == cur_arg)
            return e;
    return -1;
}

static void set_cursor(int e)
{
    if (!selectable(e)) return;
    cur_kind = ents[e].kind;
    cur_arg = ents[e].arg;
    if (cur_kind == E_SONG) sel_song = cur_arg;
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
    if (e > 0 && ents[e - 1].kind == E_HDR) top = ents[e - 1].y;   /* show its section header too */
    else if (sectioned()) top -= HDR_H;                            /* not under the sticky header */
    bot = ents[e].y + ents[e].h;
    if (top < list_scroll) list_scroll = top;
    if (bot > list_scroll + (LIST_BOT - LIST_TOP)) list_scroll = bot - (LIST_BOT - LIST_TOP);
    list_clamp();
}

static void list_ensure(void)
{
    int e;
    if (!list_dirty && built_view == list_view && ents)
        return;
    list_build();
    if (!ents) return;
    if (cur_ent() < 0)
        for (e = 0; e < n_ents; e++)
            if (selectable(e)) { set_cursor(e); break; }
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

/* ---- the queue: Play All and Shuffle play songs back to back ---- */

#define QUEUE_WAIT 8.0                       /* seconds on the results screen before the next song */

static void queue_play(void)
{
    int tries;
    for (tries = 0; queue_pos >= 0 && queue_pos < n_queue && tries < n_queue; tries++) {
        sel_song = queue[queue_pos];
        cur_kind = E_SONG;                   /* back on the list, the cursor is on the last one played */
        cur_arg = sel_song;
        sel_level = first_level(songs[sel_song].levels, sel_level ? sel_level : 2);
        start_song();
        if (scene == SC_PLAY) return;
        queue_pos++;                         /* this one would not start: go on to the next */
    }
    queue_pos = -1;
    scene = SC_LIST;
}

/* all: every song, else the songs of the current page */
static void queue_start(int all, int shuffle)
{
    int e, k, n = 0;
    if (!queue) queue = malloc(sizeof *queue * (size_t)(n_songs + 1));
    if (!queue) return;
    if (all)
        for (k = 0; k < n_songs; k++) queue[n++] = k;
    else
        for (e = 0; e < n_ents; e++)
            if (ents[e].kind == E_SONG) queue[n++] = ents[e].arg;
    if (n == 0) return;
    if (shuffle) {
        srand((unsigned)(now_t * 1000.0) ^ (unsigned)n);
        for (k = n - 1; k > 0; k--) {
            int j = rand() % (k + 1), t = queue[k];
            queue[k] = queue[j];
            queue[j] = t;
        }
    }
    n_queue = n;
    queue_pos = 0;
    plat_sfx("click");
    queue_play();
}

/* ---- pages ---- */

static void list_open_page(void)
{
    top_scroll = list_scroll;
    top_kind = cur_kind;
    top_arg = cur_arg;
    list_page = 1;
    edit_mode = 0;
    list_scroll = 0;
    list_vel = 0;
    cur_kind = -1;
    list_dirty = 1;
    list_ensure();
    plat_sfx("slide");
}

static void list_back(void)
{
    if (!list_page)
        return;                              /* the top: nothing above it */
    list_page = 0;
    edit_mode = 0;
    cur_kind = top_kind;
    cur_arg = top_arg;
    list_dirty = 1;
    list_ensure();
    list_scroll = top_scroll;
    list_vel = 0;
    list_clamp();
    plat_sfx("slide");
}

static void list_set_view(int v)
{
    if (v == list_view && !list_page) return;
    list_view = v;
    list_page = 0;
    edit_mode = 0;
    list_scroll = 0;
    list_vel = 0;
    cur_kind = E_SONG;                       /* stay on the chosen song where the tab lists songs */
    cur_arg = sel_song;
    list_dirty = 1;
    list_ensure();
    list_show(cur_ent());
    plat_sfx("click");
}

static void toggle_edit(void)
{
    edit_mode = !edit_mode;
    cur_kind = -1;
    list_dirty = 1;
    list_ensure();
    plat_sfx("click");
}

/* ---- the action sheet: buttons that slide up from the bottom ---- */

enum { SH_NONE, SH_ADD, SH_DELETE };
enum { SA_NEW = -1, SA_CLOSE = -2, SA_DELETE = -3 };
#define SHEET_VIS  6                         /* buttons shown at once; more scroll */
#define SHEET_ROW  50.0f
static struct {
    int kind, n, sel, first;
    int arg[MAX_PL + 3], style[MAX_PL + 3];  /* style 0 normal, 1 red, 2 dark (close) */
    char label[MAX_PL + 3][36];
} sheet;

static void sheet_add(const char *label, int arg, int style)
{
    snprintf(sheet.label[sheet.n], sizeof sheet.label[0], "%s", label);
    sheet.arg[sheet.n] = arg;
    sheet.style[sheet.n] = style;
    sheet.n++;
}

static void sheet_open_add(void)             /* tick which playlists the chosen song is on */
{
    int p, keep = sheet.kind == SH_ADD ? sheet.sel : 0;
    sheet.kind = SH_ADD;
    sheet.n = 0;
    for (p = 0; p < n_pls; p++) sheet_add(pls[p].name, p, 0);
    if (n_pls < MAX_PL) sheet_add("New Playlist", SA_NEW, 0);
    sheet_add("Done", SA_CLOSE, 2);
    sheet.sel = keep < sheet.n ? keep : 0;
    if (sheet.first > sheet.sel) sheet.first = sheet.sel;
    if (sheet.sel >= sheet.first + SHEET_VIS) sheet.first = sheet.sel - SHEET_VIS + 1;
    if (keep == 0) sheet.first = 0;
}

static void sheet_open_delete(void)
{
    sheet.kind = SH_DELETE;
    sheet.n = sheet.sel = sheet.first = 0;
    sheet_add("Delete Playlist", SA_DELETE, 1);
    sheet_add("Cancel", SA_CLOSE, 2);
}

static float sheet_top(void)
{
    int v = sheet.n < SHEET_VIS ? sheet.n : SHEET_VIS;
    return LH - (14 + (sheet.kind == SH_ADD ? 30 : 0) + v * SHEET_ROW + 6);
}

static void sheet_activate(int i)
{
    int a = sheet.arg[i], p;
    plat_sfx("click");
    if (a == SA_CLOSE) {
        sheet.kind = SH_NONE;
    } else if (a == SA_DELETE) {
        sheet.kind = SH_NONE;
        pl_delete(page_pl);
        page_pl = -1;
        settings_save();
        list_back();
    } else if (a == SA_NEW) {
        p = pl_new();
        pl_add(p, sel_song);
        settings_save();
        sheet.sel = p >= 0 ? p : sheet.sel;
        sheet_open_add();
    } else if (a >= 0 && a < n_pls) {
        if (pl_has(a, sel_song)) pl_remove_song(a, sel_song);
        else pl_add(a, sel_song);
        settings_save();
    }
    list_dirty = 1;
}

/* 1 if the sheet is up (it takes every input then) */
static int sheet_update(const PlatInput *in)
{
    if (!sheet.kind)
        return 0;
    if ((in->pressed & BTN_UP) && sheet.sel > 0) sheet.sel--;
    if ((in->pressed & BTN_DOWN) && sheet.sel < sheet.n - 1) sheet.sel++;
    if (sheet.sel < sheet.first) sheet.first = sheet.sel;
    if (sheet.sel >= sheet.first + SHEET_VIS) sheet.first = sheet.sel - SHEET_VIS + 1;
    if (in->pressed & BTN_CROSS) {
        sheet_activate(sheet.sel);
    } else if (in->pressed & BTN_CIRCLE) {
        sheet.kind = SH_NONE;
    } else if (ptr.released && ptr.moved < 8) {
        float y0 = sheet_top() + 14 + (sheet.kind == SH_ADD ? 30 : 0);
        int j = (int)floorf((ptr.y - y0) / SHEET_ROW);
        if (ptr.y < sheet_top()) {
            sheet.kind = SH_NONE;            /* a tap above the sheet closes it */
        } else if (j >= 0 && j < SHEET_VIS && sheet.first + j < sheet.n && ptr.y - y0 - j * SHEET_ROW < 44) {
            sheet.sel = sheet.first + j;
            sheet_activate(sheet.sel);
        }
    }
    return 1;
}

/* ---- the list itself ---- */

/* the A-Z index: touching a letter jumps to that section, or the next one */
#define IDX_TOP (LIST_TOP + 6)
#define IDX_BOT (LIST_BOT - 6)
static void index_jump(float y)
{
    int slot = (int)((y - IDX_TOP) / ((IDX_BOT - IDX_TOP) / 27)), e, last = -1;
    if (slot < 0) slot = 0;
    if (slot > 26) slot = 26;
    for (e = 0; e < n_ents; e++) {
        if (ents[e].kind != E_HDR) continue;
        last = e;
        if (letter_rank(ents[e].hdr) >= slot) break;
    }
    if (last < 0) return;
    if (e >= n_ents) e = last;
    list_scroll = ents[e].y;
    list_vel = 0;
    list_clamp();
    set_cursor(e + 1);
}

/* the first item of the section an entry is in */
static int sec_start(int e)
{
    while (selectable(e - 1)) e--;
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
            return selectable(e) ? sec_start(e) : cur;
        } else {
            for (e = cur + 1; e < n_ents; e++)
                if (ents[e].kind == E_HDR && selectable(e + 1)) return e + 1;
            for (e = n_ents - 1; e > cur; e--)
                if (selectable(e)) return e;
            return cur;
        }
    }
    for (k = 0, e = cur; k < abs(step); k++) {
        do e += step < 0 ? -1 : 1; while (e >= 0 && e < n_ents && !selectable(e));
        if (e < 0 || e >= n_ents) break;
        last = e;
    }
    return last;
}

static void list_activate(int e)
{
    const Entry *en;
    if (!selectable(e)) return;
    set_cursor(e);
    en = &ents[e];
    switch (en->kind) {
    case E_SONG:
        if (edit_mode) {                     /* edit mode: a tap takes the song off the playlist */
            pl_remove_at(page_pl, en->count);
            settings_save();
            list_dirty = 1;
            plat_sfx("click");
        } else {
            list_choose();
        }
        break;
    case E_ARTIST:
        page_song = en->arg;
        list_open_page();
        break;
    case E_PLIST:
        page_pl = en->arg;
        list_open_page();
        break;
    case E_ACT:
        if (en->arg == A_PLAY_ALL) queue_start(0, 0);
        else if (en->arg == A_SHUFFLE) queue_start(0, 1);
        else if (en->arg == A_DEL_PLIST) sheet_open_delete();
        else if (en->arg == A_NEW_PLIST) {
            int p = pl_new();
            if (p >= 0) {
                settings_save();
                list_dirty = 1;
                page_pl = p;
                list_open_page();
            }
        }
        break;
    }
}

static void list_update(const PlatInput *in, double dt)
{
    int step = 0, cur, e;
    list_ensure();
    if (!ents) return;
    if (sheet_update(in)) { list_ensure(); return; }
    cur = cur_ent();
    if (in->pressed & BTN_UP) { step = -1; repeat_t = 0.35; }
    if (in->pressed & BTN_DOWN) { step = 1; repeat_t = 0.35; }
    if (in->held & (BTN_UP | BTN_DOWN)) {    /* hold to keep scrolling */
        repeat_t -= dt;
        if (repeat_t <= 0) { step = (in->held & BTN_UP) ? -1 : 1; repeat_t = 0.06; }
    }
    if (in->pressed & BTN_L) step = -2;
    if (in->pressed & BTN_R) step = 2;
    if (step && cur >= 0) {
        set_cursor(list_move(cur, step));
        list_show(cur_ent());
        list_vel = 0;
    }
    if (in->pressed & BTN_TRIANGLE) {        /* Edit on a playlist page, else the next tab */
        if (on_playlist_page()) toggle_edit();
        else {
            int t;
            for (t = 0; t < N_TABS && tab_view[t] != list_view; t++) ;
            for (t = (t + 1) % N_TABS; tab_view[t] < 0; t = (t + 1) % N_TABS) ;
            list_set_view(tab_view[t]);
        }
    }
    if (in->pressed & BTN_START) { skin = (skin + 1) % N_SKINS; settings_save(); }
    if ((in->pressed & BTN_SQUARE) && n_rows > 0 && !edit_mode) { queue_start(0, 1); return; }

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
    if (ptr.down && press_zone == Z_LIST && ptr.moved < 8 && selectable(e = ent_at(ptr.y)))
        touch_row = e;

    if (ptr.released && ptr.moved < 8) {
        if (press_zone == Z_LIST) {
            e = ent_at(ptr.y);
            if (selectable(e)) { list_activate(e); return; }
        } else if (press_zone == Z_NAV && ptr.x < 90) {
            list_back();
            return;
        } else if (press_zone == Z_NAV && ptr.x > LW - 90) {
            if (on_playlist_page()) toggle_edit();
            else { skin = (skin + 1) % N_SKINS; settings_save(); }
        } else if (press_zone == Z_TAB) {
            int t = (int)(ptr.x / (LW / N_TABS));
            if (t >= 0 && t < N_TABS) {
                if (tab_view[t] < 0) { queue_start(1, 1); return; }
                list_set_view(tab_view[t]);
            }
        }
    }
    if (in->pressed & BTN_CROSS)
        list_activate(cur_ent());
    else if (in->pressed & BTN_CIRCLE)
        list_back();
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

/* a tick mark from little squares */
static void draw_check(float x, float y, unsigned c)
{
    int i;
    for (i = 0; i < 6; i++) fill(x + i, y + 6 + i, 2.6f, 2.6f, c);
    for (i = 0; i < 12; i++) fill(x + 5 + i, y + 11 - i, 2.6f, 2.6f, c);
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

static void draw_nav(void)
{
    const char *title = view_title[list_view], *back = view_title[list_view];
    const char *right = on_playlist_page() ? (edit_mode ? "Done" : "Edit") : "Style";
    float bw;
    if (list_page && list_view == V_ARTISTS && page_song >= 0) title = songs[page_song].artist;
    if (on_playlist_page()) title = pls[page_pl].name;
    if (skin == SKIN_SILVER) {
        shape(0, 0, LW, NAV_H, 0, 0, 0xeef0f2ffu, 0xb2bac1ffu);
        fill(0, 0, LW, 1, 0xfafbfcffu);
        fill(0, NAV_H - 1, LW, 1, 0x5e656affu);
        text_sh(LW / 2, 10, 21, 1, LW - 190, 0x4a5156ffu, 0xffffffc0u, 1, title);
    } else if (skin == SKIN_DARK) {
        shape(0, 0, LW, NAV_H, 0, 0, 0x2c2c30ffu, 0x161618ffu);
        fill(0, 0, LW, 1, 0x48484cffu);
        fill(0, NAV_H - 1, LW, 1, 0x000000ffu);
        text_sh(LW / 2, 10, 21, 1, LW - 190, 0xf2f2f2ffu, 0x000000d0u, -1, title);
    } else {
        shape(0, 0, LW, NAV_H, 0, 0, 0xb4c0d1ffu, 0x6c82a1ffu);
        fill(0, 0, LW, 1, 0xd2dae4ffu);
        fill(0, NAV_H - 1, LW, 1, 0x2d3642ffu);
        text_sh(LW / 2, 10, 21, 1, LW - 190, 0xffffffffu, 0x3a4a60d0u, -1, title);
    }
    bw = text_width(back, 14) + (skin != SKIN_BLUE ? 30 : 22);
    if (bw < 60) bw = 60;
    if (bw > 84) bw = 84;
    if (list_page)                           /* only a page has somewhere to go back to */
        nav_button(6, bw, 11, back);
    nav_button(LW - 70, 64, skin == SKIN_BLUE && !on_playlist_page() ? -11 : 0, right);
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

static void tab_icon(int t, float cx, float y, unsigned c);

static void draw_art(float x, float y, int song)
{
    static const unsigned pal[8] = {
        0x8e5b4dffu, 0x4f6d8cffu, 0x6c8a4bffu, 0x8a6aa0ffu, 0xb0874affu, 0x4d8a86ffu, 0x9a4f63ffu, 0x5d5d72ffu
    };
    const char *p;
    unsigned h = 0, c;
    int i;
    char b[2] = { '?', 0 };
    if (song < 0) {                          /* an empty playlist: a note on grey */
        shape(x, y, ROW_H, ROW_H - 1, 0, 0, 0x8e8e93ffu, 0x6c6c70ffu);
        tab_icon(1, x + ROW_H / 2, y + 9, 0xffffffd8u);
        return;
    }
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

/* the round red "remove" button of edit mode */
static void draw_minus(float x, float y)
{
    shape(x, y, 22, 22, 11, 0, 0xffffffffu, 0xffffffffu);
    shape(x + 2, y + 2, 18, 18, 9, 0, 0xf2554affu, 0xc8251bffu);
    fill(x + 6, y + 9.5f, 10, 3, 0xffffffffu);
}

static void draw_row(float y, int e, int lit, float right)
{
    const Entry *en = &ents[e];
    unsigned tc = 0x000000ffu, ac = 0x808080ffu, sc = 0x385487ffu, chev = 0xa0a0a6ffu;
    float tx = 11, sw = 0, x0 = 0;
    char buf[48];
    if (skin == SKIN_DARK) {
        tc = 0xf2f2f2ffu; ac = 0x8e8e93ffu; sc = DARK_ORANGE; chev = 0x5a5a60ffu;
    }
    if (lit && skin == SKIN_DARK) {          /* dark text: white on orange is hard to read */
        shape(0, y, LW, ROW_H, 0, 0, 0xffa31affu, 0xf07800ffu);
        tc = ac = sc = chev = 0x140c00ffu; ac = 0x4a2a00ffu;
    } else if (lit) {
        shape(0, y, LW, ROW_H, 0, 0, 0x058cf5ffu, 0x015ee6ffu);
        tc = ac = sc = chev = 0xffffffffu;
    } else {
        fill(0, y + ROW_H - 1, LW, 1, skin == SKIN_DARK ? 0x2c2c2fffu : 0xd9d9d9ffu);
    }

    if (en->kind == E_ACT) {
        static const char *names[] = { "Play All", "Shuffle", "Add Playlist...", "Delete Playlist" };
        unsigned ic = lit ? tc : skin == SKIN_DARK ? DARK_ORANGE : 0x2f6fd0ffu;
        if (en->arg == A_DEL_PLIST) {
            text_fit(LW / 2, y + 11, 19, 1, 0, lit ? tc : 0xd8261cffu, names[en->arg], 1);
            return;
        }
        if (en->arg == A_PLAY_ALL) {
            int i;
            for (i = 0; i < 16; i++) fill(16, y + 14 + i, 14 * (1 - fabsf(i + 0.5f - 8) / 8), 1.15f, ic);
        } else if (en->arg == A_SHUFFLE) {
            tab_icon(4, 23, y + 8, ic);
        } else {
            fill(15, y + 20.5f, 16, 3, ic);
            fill(21.5f, y + 14, 3, 16, ic);
        }
        text_fit(44, y + 11, 19, 0, right - 44, lit ? tc : ic, names[en->arg], 1);
        return;
    }

    if (en->kind == E_SONG && edit_mode) {
        draw_minus(9, y + 11);
        x0 = 40;
    }
    if (skin != SKIN_BLUE) {
        int art = en->kind == E_PLIST ? (pls[en->arg].n ? pls[en->arg].song[0] : -1) : en->arg;
        draw_art(x0, y, art);
        tx = x0 + ROW_H + 8;
    } else {
        tx = x0 + 11;
    }

    if (en->kind == E_ARTIST || en->kind == E_PLIST) {
        const char *name = en->kind == E_ARTIST ? songs[en->arg].artist : pls[en->arg].name;
        snprintf(buf, sizeof buf, "%d %s", en->count, en->count == 1 ? "song" : "songs");
        text_fit(tx, y + 3, 20, 0, right - tx - 22, tc, name, 1);
        text_fit(tx, y + 24, 15, 0, right - tx - 22, ac, buf, 0);
        text_fit(right - 2, y + 11, 20, 2, 0, chev, ">", 1);
        return;
    }

    {                                        /* a song */
        const Song *s = &songs[en->arg];
        long top = top_best(en->arg);
        if (top > 0 && !edit_mode) {
            snprintf(buf, sizeof buf, "%ld", top);
            sw = text_width(buf, 14) + 8;
            text(right, y + 15, 14, 2, 0, sc, buf);
        }
        text_fit(tx, y + 3, 20, 0, right - tx - sw, tc, s->title, 1);
        text_fit(tx, y + 24, 15, 0, right - tx - sw, ac, s->artist, 0);
    }
}

static void draw_footer(float y)
{
    float size = text_width(foot_text, 18) > LW - 24 ? 14 : 18;
    text_fit(LW / 2, y + 14 + (18 - size) / 2, size, 1, LW - 20, skin == SKIN_DARK ? 0x6e6e73ffu : 0x8c8c8cffu, foot_text, 0);
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
        float ly = IDX_TOP + k * step + (step - 11) / 2, mid = ly + 5.5f;
        unsigned c = 0x6b7885ffu;
        b[0] = k < 26 ? (char)('A' + k) : '#'; b[1] = 0;
        if (skin == SKIN_DARK)               /* orange, but dark over the orange highlight */
            c = mid >= lit_y0 && mid < lit_y1 ? 0x140c00ffu : DARK_ORANGE;
        text_fit(LW - IDX_W / 2, ly, 11, 1, 0, c, b, 1);
    }
}

/* tab icons, in tab order: playlists, a note, a person, score bars, a die */
static void tab_icon(int t, float cx, float y, unsigned c)
{
    switch (t) {
    case 0:                                  /* lines and a note */
        fill(cx - 12, y + 4, 13, 3, c);
        fill(cx - 12, y + 10, 13, 3, c);
        fill(cx - 12, y + 16, 9, 3, c);
        fill(cx + 6, y + 3, 2.5f, 17, c);
        shape(cx + 6, y + 3, 7, 4, 0, 0, c, c);
        shape(cx - 1, y + 16, 9.5f, 8, 4, 0, c, c);
        break;
    case 1:                                  /* a note */
        fill(cx + 1, y + 3, 2.5f, 18, c);
        shape(cx + 1, y + 3, 8, 4, 0, 0, c, c);
        fill(cx + 7, y + 3, 2, 8, c);
        shape(cx - 7, y + 16, 10.5f, 8, 4, 0, c, c);
        break;
    case 2:                                  /* a person */
        shape(cx - 5, y + 1, 10, 11, 5, 0, c, c);
        shape(cx - 10, y + 13, 20, 12, 6, 0, c, c);
        break;
    case 3:                                  /* score bars */
        fill(cx - 10, y + 15, 5, 10, c);
        fill(cx - 2.5f, y + 8, 5, 17, c);
        fill(cx + 5, y + 2, 5, 23, c);
        break;
    default:                                 /* a die: shuffle */
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
    float y = LIST_BOT, tw = LW / N_TABS;
    int t;
    shape(0, y, LW, TAB_H / 2, 0, 0, 0x3e3e3effu, 0x1e1e1effu);
    fill(0, y + TAB_H / 2, LW, TAB_H / 2, 0x0a0a0affu);
    fill(0, y, LW, 1, 0x111111ffu);
    fill(0, y + 1, LW, 1, 0x555555ffu);
    for (t = 0; t < N_TABS; t++) {
        float x = t * tw;
        int sel = tab_view[t] == list_view;
        unsigned ic = !sel ? 0x9a9a9affu : skin == SKIN_BLUE ? 0x3ea6ffffu : skin == SKIN_DARK ? DARK_ORANGE : 0xffffffffu;
        if (sel) shape(x + 2, y + 3, tw - 4, TAB_H - 5, 3, 0, 0x5a5a5aa0u, 0x3a3a3aa0u);
        tab_icon(t, x + tw / 2, y + 4, ic);
        text_fit(x + tw / 2, y + 32, 10, 1, tw - 2, sel ? 0xffffffffu : 0x9a9a9affu, tab_label[t], 1);
    }
}

static void sheet_draw(void)
{
    float top, y;
    int j, v;
    if (!sheet.kind) return;
    top = sheet_top();
    v = sheet.n < SHEET_VIS ? sheet.n : SHEET_VIS;
    fill(0, 0, LW, LH, 0x00000080u);
    shape(0, top, LW, LH - top, 0, 0, 0x4a4a4cf2u, 0x1c1c1ef2u);
    fill(0, top, LW, 1, 0x8a8a8affu);
    y = top + 14;
    if (sheet.kind == SH_ADD) {
        text_fit(LW / 2, top + 9, 14, 1, LW - 60, 0xd2d2d2ffu, "Add to Playlist", 0);
        if (sheet.first > 0) text_fit(LW - 24, top + 7, 14, 1, 0, 0xd2d2d2ffu, "^", 1);
        if (sheet.first + SHEET_VIS < sheet.n) text_fit(LW - 40, top + 7, 14, 1, 0, 0xd2d2d2ffu, "more", 0);
        y += 30;
    }
    for (j = 0; j < v; j++) {
        int i = sheet.first + j;
        unsigned top_c = 0x6e6e70ffu, bot_c = 0x3a3a3cffu;
        if (sheet.style[i] == 1) { top_c = 0xef5a50ffu; bot_c = 0xb3241affu; }
        if (sheet.style[i] == 2) { top_c = 0x2a2a2cffu; bot_c = 0x050505ffu; }
        if (i == sheet.sel)
            shape(14, y - 2, LW - 28, 48, 9, 0, skin == SKIN_DARK ? DARK_ORANGE : 0x3ea6ffffu, skin == SKIN_DARK ? DARK_ORANGE : 0x3ea6ffffu);
        shape(16, y, LW - 32, 44, 8, 0, 0x101010ffu, 0x101010ffu);
        shape(17, y + 1, LW - 34, 42, 7, 0, top_c, bot_c);
        text_sh(LW / 2, y + 11, 18, 1, LW - 100, 0xffffffffu, 0x000000a0u, -1, sheet.label[i]);
        if (sheet.kind == SH_ADD && sheet.arg[i] >= 0 && pl_has(sheet.arg[i], sel_song))
            draw_check(LW - 48, y + 14, skin == SKIN_DARK ? DARK_ORANGE : 0xffffffffu);
        y += SHEET_ROW;
    }
}

static void list_draw(void)
{
    int e, sticky = -1, nxt, cur;
    float right;
    list_ensure();
    fill(0, 0, LW, LH, skin == SKIN_DARK ? 0x111113ffu : 0xffffffffu);
    if (!ents) return;
    cur = cur_ent();
    right = LW - (sectioned() ? IDX_W + 4 : 10);
    lit_y0 = lit_y1 = -1;
    for (e = 0; e < n_ents; e++) {
        float y = LIST_TOP + ents[e].y - list_scroll;
        if (ents[e].kind == E_HDR && ents[e].y <= list_scroll) sticky = e;
        if (y + ents[e].h < LIST_TOP || y > LIST_BOT) continue;
        if (ents[e].kind == E_HDR) {
            if (ents[e].y > list_scroll) draw_header(y, ents[e].hdr);   /* else the sticky copy covers it */
        } else if (ents[e].kind == E_FOOT) {
            draw_footer(y);
        } else {
            int lit = e == cur || e == touch_row;
            if (e == cur) { lit_y0 = y; lit_y1 = y + ROW_H; }
            draw_row(y, e, lit, right);
        }
    }
    if (sticky >= 0) {                       /* the current section's header stays on top */
        float y = LIST_TOP;
        for (nxt = sticky + 1; nxt < n_ents && ents[nxt].kind != E_HDR; nxt++) ;
        if (nxt < n_ents) {
            float ny = LIST_TOP + ents[nxt].y - list_scroll;
            if (ny < LIST_TOP + HDR_H) y = ny - HDR_H;
        }
        draw_header(y, ents[sticky].hdr);
    }
    if (sectioned()) draw_index();
    draw_nav();
    draw_tabs();
    sheet_draw();
}

/* ------------------------------------------- song and results screens -- */

/* Both are laid out like a classic phone's "now playing" screen: a glossy
 * black bar with the artist, title and length; the cover in the middle on
 * black; a row of transport buttons; and a slider.  On the song screen the slider picks the difficulty and the buttons
 * are previous song / play / next song.  On the results screen the slider
 * shows the share of notes hit and the middle button plays the song again. */

#define NP_BAR    44.0f                      /* top bar */
#define NP_KEYS_H 56.0f
#define NP_KEYS   (LH - NP_KEYS_H - 60.0f)   /* transport row: the bottom 116 points are its and the slider's */
#define NP_ART    (NP_KEYS - NP_BAR)         /* cover area: the rest, below the bar */
#define NP_OY     ((NP_ART - 320.0f) / 2)    /* the cover and text are laid out for 320 points of it */
#define NP_SLIDE  (NP_KEYS + NP_KEYS_H)      /* slider area, to the bottom */
#define NP_X0     44.0f                      /* slider track ends */
#define NP_X1     (LW - 44.0f)

static float knob_x = -1;                    /* the difficulty knob glides to its place */
static int slider_held;

static unsigned accent(void) { return skin == SKIN_DARK ? DARK_ORANGE : 0x2f8cffffu; }

/* a triangle from 1-point strips; dir 1 points right, -1 left */
static void tri(float x, float y, float w, float h, int dir, unsigned c)
{
    int i, n = (int)ceilf(h);
    for (i = 0; i < n; i++) {
        float yc = i + 0.5f, tw = w * (1 - fabsf(yc - h / 2) / (h / 2));
        fill(dir > 0 ? x : x + w - tw, y + i, tw, 1.15f, c);
    }
}

static void fmt_num(char *buf, size_t n, long v)  /* 1234567 -> "1,234,567" */
{
    char t[24];
    int len, i, o = 0;
    snprintf(t, sizeof t, "%ld", v < 0 ? 0 : v);
    len = (int)strlen(t);
    for (i = 0; i < len && o + 2 < (int)n; i++) {
        if (i && (len - i) % 3 == 0) buf[o++] = ',';
        buf[o++] = t[i];
    }
    buf[o] = 0;
}

/* a song's cover at any size, centred; tint darkens it (0xffffffff = as is) */
static void draw_cover(int song, float cx, float cy, float size, unsigned tint)
{
    int i = atoi(songs[song].num) - 1;
    if (tx_art.t && i >= 0 && i < art_tiles && art_have[i]) {
        /* one pixel in from the tile's edge: smoothing would pull in the neighbours */
        draw(&tx_art, (i % ART_COLS) * ART_TILE + 1, (i / ART_COLS) * ART_TILE + 1, ART_TILE - 2, ART_TILE - 2,
             cx, cy, size, size, 0, tint, 0);
    } else {
        unsigned h = 0, c;
        const char *p;
        char b[2] = { '?', 0 };
        for (p = songs[song].artist; *p; p++) h = h * 31 + (unsigned char)*p;
        for (p = songs[song].artist; *p; p++)
            if (isalnum((unsigned char)*p)) { b[0] = (char)toupper((unsigned char)*p); break; }
        c = mix(0x5d5d72ffu, 0x8e5b4dffu, (float)(h & 7) / 7);
        c = mix(c, 0x000000ffu, 1 - (float)((tint >> 24) & 255) / 255);
        shape(cx - size / 2, cy - size / 2, size, size, 0, 0, c, mix(c, 0x000000ffu, 0.3f));
        text_fit(cx, cy - size * 0.3f, size * 0.6f, 1, 0, mix(0xffffffd8u, 0x000000d8u, 1 - (float)((tint >> 24) & 255) / 255), b, 1);
    }
}

static void np_button_back(void)
{
    shape(6, 7, 44, 30, 6, 0, 0x000000ffu, 0x000000ffu);
    shape(7, 8, 42, 28, 5, 0, 0x555555ffu, 0x1c1c1cffu);
    tri(15, 13, 11, 18, -1, 0xffffffffu);
    fill(25, 18.5f, 13, 7, 0xffffffffu);
}

static void np_button_list(void)
{
    int k;
    shape(LW - 50, 7, 44, 30, 6, 0, 0x000000ffu, 0x000000ffu);
    shape(LW - 49, 8, 42, 28, 5, 0, 0x555555ffu, 0x1c1c1cffu);
    for (k = 0; k < 3; k++) {
        fill(LW - 39, 14 + k * 6.5f, 3, 3, 0xffffffffu);
        fill(LW - 34, 14 + k * 6.5f, 17, 3, 0xffffffffu);
    }
}

static void np_bar(const Song *s, const char *third)
{
    shape(0, 0, LW, NP_BAR, 0, 0, 0x3c3c3cffu, 0x0e0e0effu);
    fill(0, 0, LW, 1, 0x5c5c5cffu);
    fill(0, NP_BAR - 1, LW, 1, 0x000000ffu);
    np_button_back();
    np_button_list();
    text_fit(LW / 2, 2, 12, 1, LW - 124, 0xa8a8a8ffu, s->artist, 0);
    text_fit(LW / 2, 14, 15, 1, LW - 124, 0xffffffffu, s->title, 1);
    text_fit(LW / 2, 29, 12, 1, LW - 124, 0xa8a8a8ffu, third, 0);
}

/* the cover, on black */
static void np_art(int song, float size, float cy)
{
    fill(0, NP_BAR, LW, NP_ART, 0x000000ffu);
    draw_cover(song, LW / 2, cy, size, 0xffffffffu);
}

static void np_keys(int middle_on)
{
    float w = LW / 3, cy = NP_KEYS + NP_KEYS_H / 2;
    unsigned ic = 0xffffffffu;
    shape(0, NP_KEYS, LW, NP_KEYS_H, 0, 0, 0x3e3e3effu, 0x171717ffu);
    fill(0, NP_KEYS, LW, 1, 0x000000ffu);
    fill(0, NP_KEYS + 1, LW, 1, 0x5e5e5effu);
    fill(w, NP_KEYS + 2, 1, NP_KEYS_H - 2, 0x0a0a0affu);
    fill(w + 1, NP_KEYS + 2, 1, NP_KEYS_H - 2, 0x4a4a4affu);
    fill(2 * w, NP_KEYS + 2, 1, NP_KEYS_H - 2, 0x0a0a0affu);
    fill(2 * w + 1, NP_KEYS + 2, 1, NP_KEYS_H - 2, 0x4a4a4affu);
    tri(w / 2 - 15, cy - 8, 13, 16, -1, ic);           /* previous */
    tri(w / 2 - 2, cy - 8, 13, 16, -1, ic);
    tri(LW / 2 - 9, cy - 13, 22, 26, 1, middle_on ? (skin == SKIN_DARK ? DARK_ORANGE : ic) : 0x777777ffu);
    tri(2.5f * w - 11, cy - 8, 13, 16, 1, ic);         /* next */
    tri(2.5f * w + 2, cy - 8, 13, 16, 1, ic);
}

/* the slider: track from NP_X0 to NP_X1, filled up to fx; a knob at fx if knob */
static void np_slider(float fx, int knob)
{
    float ty = NP_SLIDE + 16, th = 9;
    shape(0, NP_SLIDE, LW, LH - NP_SLIDE, 0, 0, 0x101010ffu, 0x000000ffu);
    shape(NP_X0 - 1, ty - 1, NP_X1 - NP_X0 + 2, th + 2, (th + 2) / 2, 0, 0x000000ffu, 0x2a2a2affu);
    shape(NP_X0, ty, NP_X1 - NP_X0, th, th / 2, 0, 0xc8c8c8ffu, 0xf0f0f0ffu);
    if (fx > NP_X0 + 1)
        shape(NP_X0, ty, fx - NP_X0, th, th / 2, 0, mix(accent(), 0xffffffffu, 0.35f), mix(accent(), 0x000000ffu, 0.15f));
    if (knob) {
        shape(fx - 12, ty + th / 2 - 12, 24, 24, 12, 0, 0x4a4a4affu, 0x2a2a2affu);
        shape(fx - 11, ty + th / 2 - 11, 22, 22, 11, 0, 0xffffffffu, 0xa6a6a6ffu);
        shape(fx - 5, ty + th / 2 - 8, 10, 5, 2.5f, 0, 0xffffffc0u, 0xffffff40u);   /* shine */
    }
}

/* the difficulty levels a song has, in order, and where each sits on the track */
static int song_levels(const Song *s, int *lv)
{
    int l, n = 0;
    for (l = 0; l < 5; l++)
        if (s->levels & (1 << l)) lv[n++] = l;
    return n;
}

static float level_x(int k, int n)
{
    return n <= 1 ? (NP_X0 + NP_X1) / 2 : NP_X0 + (NP_X1 - NP_X0) * k / (n - 1);
}

/* the previous or next song in the list's current order */
static void np_step_song(int dir)
{
    int e = cur_ent(), f;
    if (e < 0) return;
    for (f = e + dir; f >= 0 && f < n_ents && ents[f].kind != E_SONG; f += dir) ;
    if (f < 0 || f >= n_ents) return;
    set_cursor(f);
    sel_level = first_level(songs[sel_song].levels, sel_level);
    list_show(f);
    knob_x = -1;
    plat_sfx("click");
}

static void level_update(const PlatInput *in)
{
    const Song *s = &songs[sel_song];
    int lv[5], n = song_levels(s, lv), k, cur = 0, dir = 0, go = 0;
    float w = LW / 3;
    if (sheet_update(in))                    /* "Add to Playlist" is up */
        return;
    if (in->pressed & BTN_SQUARE) {
        sheet_open_add();
        return;
    }
    for (k = 0; k < n; k++)
        if (lv[k] == sel_level) cur = k;
    if (in->pressed & (BTN_LEFT | BTN_UP)) dir = -1;
    if (in->pressed & (BTN_RIGHT | BTN_DOWN)) dir = 1;
    if (dir && cur + dir >= 0 && cur + dir < n) {
        sel_level = lv[cur + dir];
        plat_sfx("click");
    }
    if (in->pressed & BTN_L) np_step_song(-1);
    if (in->pressed & BTN_R) np_step_song(1);

    /* the slider: drag the knob, or tap anywhere along the track */
    if (ptr.pressed)
        slider_held = ptr.y >= NP_SLIDE && n > 1;
    if (ptr.down && slider_held) {
        int best = 0;
        for (k = 1; k < n; k++)
            if (fabsf(level_x(k, n) - ptr.x) < fabsf(level_x(best, n) - ptr.x)) best = k;
        if (lv[best] != sel_level) { sel_level = lv[best]; plat_sfx("click"); }
    }
    if (!ptr.down) slider_held = 0;

    if (ptr.released && ptr.moved < 8) {
        if (ptr.y < NP_BAR && ptr.x < 60) {
            scene = SC_LIST;
            return;
        }
        if (ptr.y < NP_BAR && ptr.x > LW - 60) {
            sheet_open_add();
            return;
        }
        if (ptr.y >= NP_KEYS && ptr.y < NP_SLIDE) {
            if (ptr.x < w) np_step_song(-1);
            else if (ptr.x < 2 * w) go = 1;
            else np_step_song(1);
        }
    }
    if (go || (in->pressed & BTN_CROSS)) {
        plat_sfx("click");
        start_song();
    } else if (in->pressed & BTN_CIRCLE) {
        scene = SC_LIST;
    }
}

static void level_draw(void)
{
    const Song *s = &songs[sel_song];
    const Best *b = best_of(sel_song, sel_level);
    int lv[5], n = song_levels(s, lv), k, cur = 0;
    char len[16], buf[64], num[24];
    float target, art_cy = NP_BAR + NP_OY + 112;
    for (k = 0; k < n; k++)
        if (lv[k] == sel_level) cur = k;
    snprintf(len, sizeof len, "%d:%02d", (int)s->length / 60, (int)s->length % 60);
    fill(0, 0, LW, LH, 0x000000ffu);
    np_art(sel_song, 176, art_cy);

    /* the "lyrics": the chosen difficulty and your best at it */
    text_sh(LW / 2, NP_BAR + NP_OY + 214, 22, 1, LW - 20, 0xffffffffu, 0x000000c0u, 1, level_name[sel_level]);
    if (b && b->score > 0) {
        int pct = b->judged ? (int)(100.0 * b->hit / b->judged + 0.5) : 0;
        fmt_num(num, sizeof num, b->score);
        snprintf(buf, sizeof buf, "Best  %s", num);
        text_sh(LW / 2, NP_BAR + NP_OY + 246, 17, 1, LW - 20, 0xffffffffu, 0x000000c0u, 1, buf);
        snprintf(buf, sizeof buf, "%d%% of notes  -  streak %d", pct, b->streak);
        text_fit(LW / 2, NP_BAR + NP_OY + 272, 14, 1, LW - 20, 0xb4b4b4ffu, buf, 0);
    } else {
        text_fit(LW / 2, NP_BAR + NP_OY + 250, 15, 1, LW - 20, 0xb4b4b4ffu, "Not played yet", 0);
    }
    np_bar(s, len);
    np_keys(1);

    /* difficulty slider, the knob gliding to the chosen level */
    target = level_x(cur, n);
    if (knob_x < 0 || slider_held) knob_x = slider_held ? fminf(fmaxf(ptr.x, NP_X0), NP_X1) : target;
    else knob_x += (target - knob_x) * 0.3f;
    np_slider(knob_x, 1);
    for (k = 0; k < n; k++) {
        int on = k == cur;
        text_fit(level_x(k, n), NP_SLIDE + 34, 11, 1, 0, on ? 0xffffffffu : 0x8a8a8affu, level_name[lv[k]], on);
    }
    sheet_draw();
}

static void queue_next(void)
{
    if (queue_pos >= 0 && queue_pos + 1 < n_queue) {
        queue_pos++;
        queue_play();
    } else {
        queue_pos = -1;
        scene = SC_LIST;
    }
}

static void result_update(const PlatInput *in)
{
    float w = LW / 3;
    int queued = queue_pos >= 0;
    if (ptr.released && ptr.moved < 8) {
        if (ptr.y < NP_BAR && (ptr.x < 60 || ptr.x > LW - 60)) { queue_pos = -1; scene = SC_LIST; return; }
        if (ptr.y >= NP_KEYS && ptr.y < NP_SLIDE) {
            if (queued) {                    /* Play All / Shuffle: again, next, next */
                if (ptr.x < w) { plat_sfx("click"); start_song(); }
                else queue_next();
            } else if (ptr.x < w) { np_step_song(-1); scene = SC_LEVEL; }
            else if (ptr.x < 2 * w) { plat_sfx("click"); start_song(); }
            else { np_step_song(1); scene = SC_LEVEL; }
            return;
        }
    }
    if (in->pressed & BTN_TRIANGLE) { plat_sfx("click"); start_song(); }
    else if (queued && (in->pressed & BTN_CROSS)) queue_next();
    else if (in->pressed & (BTN_CROSS | BTN_CIRCLE)) { queue_pos = -1; scene = SC_LIST; }
    else if (queued && queue_pos + 1 < n_queue && now_t >= next_at) queue_next();
}

static void result_draw(void)
{
    const Song *s = &songs[sel_song];
    char buf[128], num[24];
    int pct = n_judged ? (int)(100.0 * n_hit / n_judged + 0.5) : 0;
    float y = NP_BAR + NP_OY + 158;
    fill(0, 0, LW, LH, 0x000000ffu);
    np_art(sel_song, 112, NP_BAR + NP_OY + 72);

    fmt_num(num, sizeof num, score);
    text_sh(LW / 2, y, 40, 1, LW - 20, new_best ? (skin == SKIN_DARK ? DARK_ORANGE : 0xffd040ffu) : 0xffffffffu,
            0x000000c0u, 2, num);
    if (new_best && prev_best.score > 0) {
        fmt_num(num, sizeof num, prev_best.score);
        snprintf(buf, sizeof buf, "New best!  (was %s)", num);
    } else if (new_best) {
        snprintf(buf, sizeof buf, "New best!");
    } else if (prev_best.score > 0) {
        fmt_num(num, sizeof num, prev_best.score);
        snprintf(buf, sizeof buf, "Your best: %s", num);
    } else {
        buf[0] = 0;
    }
    text_sh(LW / 2, y + 48, 16, 1, LW - 20, 0xffffffffu, 0x000000c0u, 1, buf);
    snprintf(buf, sizeof buf, "Notes hit: %d of %d", n_hit, n_judged);
    text_fit(LW / 2, y + 80, 15, 1, LW - 20, 0xc8c8c8ffu, buf, 0);
    snprintf(buf, sizeof buf, "Best streak: %d", best_streak);
    text_fit(LW / 2, y + 104, 15, 1, LW - 20, 0xc8c8c8ffu, buf, 0);
    if (queue_pos >= 0) {
        snprintf(buf, sizeof buf, "%s  -  %d of %d", level_name[sel_level], queue_pos + 1, n_queue);
        np_bar(s, buf);
    } else {
        np_bar(s, level_name[sel_level]);
    }
    np_keys(1);

    /* the share of notes hit, as a filled track */
    np_slider(NP_X0 + (NP_X1 - NP_X0) * pct / 100.0f, 0);
    snprintf(buf, sizeof buf, "%d", n_hit);
    text(NP_X0 - 6, NP_SLIDE + 14, 12, 2, 0, 0xa8a8a8ffu, buf);
    snprintf(buf, sizeof buf, "%d%%", pct);
    text(NP_X1 + 6, NP_SLIDE + 14, 12, 0, 0, 0xa8a8a8ffu, buf);
    if (queue_pos >= 0 && queue_pos + 1 < n_queue) {
        int secs = (int)ceil(next_at - now_t);
        snprintf(buf, sizeof buf, "Up next in %d:  %s", secs < 0 ? 0 : secs, songs[queue[queue_pos + 1]].title);
        text_fit(LW / 2, NP_SLIDE + 34, 11, 1, LW - 20, 0xd0d0d0ffu, buf, 1);
    } else if (queue_pos >= 0) {
        text_fit(LW / 2, NP_SLIDE + 34, 11, 1, 0, 0x8a8a8affu, "That was the last song   X: song list", 0);
    } else {
        text_fit(LW / 2, NP_SLIDE + 34, 11, 1, 0, 0x8a8a8affu, "X: song list    Triangle: play again", 0);
    }
}

/* --------------------------------------------------------- entry points -- */

/* settings.txt: the screen layout, then one "best" line per song+difficulty */
static void settings_load(void)
{
    static char line[4096];
    char num[8];
    int level, streak, hit, judged, song;
    long sc;
    FILE *fp = fopen(plat_save_file(), "r");
    if (!fp)
        return;
    while (fgets(line, sizeof line, fp)) {
        if (!strncmp(line, "playlist ", 9) && n_pls < MAX_PL) {
            /* playlist <name><TAB><song number>,<song number>,... */
            char *tab = strchr(line + 9, '\t'), *p;
            Playlist *pl = &pls[n_pls];
            if (!tab) continue;
            *tab = 0;
            snprintf(pl->name, sizeof pl->name, "%.31s", line + 9);
            pl->n = 0;
            n_pls++;
            for (p = tab + 1; *p && *p != '\n'; ) {
                char n3[8];
                size_t len = strcspn(p, ",\r\n");
                snprintf(n3, sizeof n3, "%.*s", (int)(len < 7 ? len : 7), p);
                if ((song = song_by_num(n3)) >= 0)  /* a song no longer present is dropped */
                    pl_add(n_pls - 1, song);
                p += len;
                if (*p == ',') p++;
            }
        } else if (sscanf(line, "tate %d", &level) == 1) {
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
    for (i = 0; i < n_pls; i++) {
        fprintf(fp, "playlist %s\t", pls[i].name);
        for (l = 0; l < pls[i].n; l++)
            fprintf(fp, "%s%s", l ? "," : "", songs[pls[i].song[l]].num);
        fprintf(fp, "\n");
    }
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
    if (tex_load(&tx_hud, "hud") || tex_load(&tx_font, "font") ||
        font_load() || songs_load())
        return -1;
    art_load();
    bests = calloc((size_t)n_songs * 5 + 1, sizeof *bests);
    if (!bests)
        return -1;
    n_pls = 0;
    settings_load();
    menu_size();                             /* the layout may have come from the file */
    sel_song = -1;                           /* the list starts on its first row */
    cur_kind = E_SONG;
    cur_arg = -1;
    list_view = V_SONGS;
    list_page = edit_mode = 0;
    queue_pos = -1;
    sheet.kind = SH_NONE;
    list_scroll = 0;
    list_dirty = 1;
    scene = SC_LIST;                         /* straight to the song list: no title screen */
    return 0;
}

void game_shutdown(void)
{
    settings_save();
    plat_music_stop();
    theme_free();
    plat_tex_free(tx_hud.t); plat_tex_free(tx_font.t); plat_tex_free(tx_art.t);
    tx_art.t = NULL;
    free(art_have);
    art_have = NULL;
    art_tiles = 0;
    free(songs); free(notes); free(bests); free(ents); free(ord); free(queue);
    songs = NULL; notes = NULL; bests = NULL; ents = NULL; ord = NULL; queue = NULL;
    n_songs = n_notes = n_ents = n_rows = 0;
    built_view = -1;
}

int game_frame(double dt, const PlatInput *in)
{
    int was = ptr.down;

    if (dt > 0.1) dt = 0.1;
    if (dt < 0) dt = 0;
    now_t += dt;

    if ((in->pressed & BTN_SELECT) && !(scene == SC_PLAY && paused)) {
        tate = !tate;
        if (scene == SC_PLAY) view_setup();
        else menu_size();
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
    case SC_LIST:   list_update(in, dt); break;
    case SC_LEVEL:  level_update(in); break;
    case SC_PLAY:   play_update(in, dt); break;
    case SC_RESULT: result_update(in); break;
    }

    plat_begin();
    switch (scene) {
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
            if (ents[e].kind == E_SONG) {
                if (cur_kind == E_SONG && ents[e].arg == cur_arg) { row = k; break; }
                k++;
            }
    }
    snprintf(buf, (size_t)n, "row=%d num=%s view=%d page=%d edit=%d queue=%d/%d sheet=%d pls=%d pl0=%d cur=%d skin=%d scene=%d song=%d level=%d t=%.2f notes=%d score=%ld streak=%d best=%d mult=%d hit=%d judged=%d revenge=%d paused=%d",
             row, sel_song >= 0 && sel_song < n_songs ? songs[sel_song].num : "-", list_view, list_page, edit_mode,
             queue_pos, n_queue, sheet.kind, n_pls, n_pls ? pls[0].n : -1, cur_kind, skin,
             scene, sel_song, sel_level, song_t, n_notes, score, streak, best_streak, mult, n_hit, n_judged, revenge, paused);
}
