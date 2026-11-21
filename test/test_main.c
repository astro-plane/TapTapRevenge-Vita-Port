/* test_main.c - scripted play-through on the software renderer.
 * Plays a song perfectly with an auto-player (reads the same chart file),
 * then plays one with no input, and checks score, hits and misses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/game.h"

extern int soft_render, soft_live_tex, soft_music_paused;
extern double soft_time, soft_music;
extern char soft_log[];
void soft_dump(const char *path);

static PlatInput in;
static int prev_held;
static const char *outdir = "/tmp";
static int fails;

typedef struct { float t, hold; int lane, shake; } TN;
static TN tn[20000];
static int ntn;

static void frame(void)
{
    double dt = 1.0 / 60;
    soft_time += dt;
    if (soft_music >= 0 && !soft_music_paused) soft_music += dt;
    in.pressed = in.held & ~prev_held;
    prev_held = in.held;
    game_frame(dt, &in);
}

static void run(double s) { double t; for (t = 0; t < s; t += 1.0 / 60) frame(); }
static void press(int b) { in.held |= b; frame(); in.held &= ~b; frame(); }
static void tap(float lx, float ly)
{
    in.n = 1; game_l2s(lx, ly, &in.t[0].x, &in.t[0].y); in.t[0].id = 1; frame(); frame();
    in.n = 0; frame();
}

static void snap(const char *name)
{
    char p[512];
    soft_render = 1; frame(); soft_render = 0;
    snprintf(p, sizeof p, "%s/%s.png", outdir, name);
    soft_dump(p);
}

static long field(const char *key)
{
    char buf[512], *p;
    game_debug(buf, sizeof buf);
    p = strstr(buf, key);
    return p ? atol(p + strlen(key)) : -999;
}

static void say(const char *label)
{
    char buf[512];
    game_debug(buf, sizeof buf);
    printf("%-26s %s | sfx: %s\n", label, buf, soft_log);
    soft_log[0] = 0;
}

static void check(const char *label, int ok)
{
    if (!ok) { printf("  ** FAIL: %s\n", label); fails++; }
}

static long first_score;
static char first_num[8];

/* the chart number of the song under the list cursor ("001") */
static const char *cur_num(void)
{
    static char num[8];
    snprintf(num, sizeof num, "%03ld", field("num="));
    return num;
}
static int keep_held;      /* leave buttons down after autoplay (to photograph a hold) */

static int load_chart(const char *num, int level)
{
    char path[512], line[128];
    int lv, cnt, inlv = 0;
    FILE *f;
    snprintf(path, sizeof path, "%scharts/%s.txt", getenv("TTR_DATA"), num);
    f = fopen(path, "r");
    if (!f) return -1;
    ntn = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "level %d %d", &lv, &cnt) == 2) { if (inlv) break; inlv = lv == level; }
        else if (inlv && ntn < 20000 && sscanf(line, "%f %d %d %f", &tn[ntn].t, &tn[ntn].lane, &tn[ntn].shake, &tn[ntn].hold) == 4) ntn++;
    }
    fclose(f);
    return ntn;
}

/* play perfectly until the song ends (or max seconds) */
static void autoplay(double max_s)
{
    static const int btn3[4] = { BTN_LEFT, BTN_UP, BTN_RIGHT, 0 }, btn4[4] = { BTN_LEFT, BTN_UP, BTN_SQUARE, BTN_CIRCLE };
    const int *lane_btn = btn3;
    double stop = soft_music + max_s;
    int next = 0, release_at[4] = { 0, 0, 0, 0 }, k, snapped = 0;
    float hold_end[4] = { -1, -1, -1, -1 };
    for (k = 0; k < ntn; k++) if (tn[k].lane == 3) lane_btn = btn4;      /* a four-lane chart */
    while (field("scene=") == 3 && soft_music < stop) {
        double st = soft_music - 0.030;          /* what the game calls song time */
        in.held &= ~(BTN_L | BTN_R);
        for (k = 0; k < 4; k++) {
            if (release_at[k] && --release_at[k] == 0) in.held &= ~lane_btn[k];
            if (hold_end[k] >= 0 && st > hold_end[k]) { in.held &= ~lane_btn[k]; hold_end[k] = -1; }
        }
        while (next < ntn && tn[next].t <= st + 0.008) {
            TN *n = &tn[next++];
            if (n->shake) {
                in.held |= n->lane == 2 ? BTN_R : BTN_L;
                if (n->lane == 1 && (in.held & BTN_L) && 0) in.held |= BTN_L;
            } else {
                if (prev_held & lane_btn[n->lane]) {       /* must lift before tapping again */
                    in.held &= ~lane_btn[n->lane]; frame();
                }
                in.held |= lane_btn[n->lane];
                if (n->hold > 0) { hold_end[n->lane] = n->t + n->hold - 0.02f; release_at[n->lane] = 0; }
                else release_at[n->lane] = 3;
            }
        }
        frame();
        if (!snapped && soft_music > 30) { snapped = 1; snap("04_playing"); say("30 s in"); }
    }
    if (!keep_held) in.held = 0;
}

int main(int argc, char **argv)
{
    int w, h, notes, i;
    long sc;
    if (argc > 1) outdir = argv[1];
    remove("/tmp/ttrvita_test_settings.txt");
    plat_init(&w, &h);
    if (game_init(w, h) != 0) { printf("init failed\n"); return 1; }

    if (argc > 2) {                       /* quick look at one song: test <outdir> <song index> */
        char num[8], name[32];
        int song = atoi(argv[2]);
        run(0.5);
        /* the list is alphabetical: walk it to the song with that number */
        for (i = 0; i < 2000 && field("num=") != song + 1; i++) press(BTN_DOWN);
        press(BTN_CROSS); press(BTN_CROSS); run(0.05);
        snprintf(num, sizeof num, "%03d", song + 1);
        load_chart(num, (int)field("level="));
        keep_held = 1;
        autoplay(argc > 3 ? atof(argv[3]) : 24);
        snprintf(name, sizeof name, "song_%s", num);
        snap(name); say(name);
        game_shutdown();
        return 0;
    }
    run(0.5); snap("02_list"); say("list");
    check("starts on the song list", field("scene=") == 1);
    tap(100, 88); run(0.2);                           /* the first row, under the "A" header */
    check("tapping a row opens its song screen", field("scene=") == 2 && field("row=") == 0);
    press(BTN_CIRCLE); run(0.2);
    check("on song list", field("scene=") == 1);
    for (i = 0; i < 3; i++) press(BTN_DOWN);
    check("cursor moved", field("row=") == 3);
    for (i = 0; i < 3; i++) press(BTN_UP);
    check("cursor back on the first row", field("row=") == 0);
    snprintf(first_num, sizeof first_num, "%s", cur_num());

    /* song 1, its first listed level: perfect auto-play */
    press(BTN_CROSS); run(0.1); snap("03_level"); say("level select");
    check("on level select", field("scene=") == 2);
    press(BTN_CROSS); run(0.05);
    check("playing", field("scene=") == 3);
    notes = load_chart(first_num, (int)field("level="));
    printf("chart %s has %d notes\n", first_num, notes);
    check("notes loaded", notes > 0 && field("notes=") == notes);
    autoplay(400);
    snap("05_result"); say("result (perfect)");
    check("result screen", field("scene=") == 4);
    check("every note hit", field("hit=") == notes && field("judged=") == notes);
    check("streak equals notes", field("best=") == notes);
    sc = field("score=");
    check("score sensible", sc >= 100L * notes);
    first_score = sc;

    /* song 2: no input at all - everything should be missed */
    press(BTN_CROSS); run(0.1);
    press(BTN_DOWN); press(BTN_CROSS); run(0.1); press(BTN_CROSS);
    notes = load_chart(cur_num(), (int)field("level="));
    run(12); snap("06_misses"); say("12 s, hands off");
    in.held = BTN_START; frame(); in.held = 0; frame(); snap("07_paused"); say("paused");
    check("paused", field("paused=") == 1);
    { double m = soft_music; run(1); check("music held while paused", soft_music == m); }
    in.held = BTN_START; frame(); in.held = 0; frame();
    run(1000);
    say("song 2 finished");
    check("song 2 result", field("scene=") == 4);
    check("nothing hit", field("hit=") == 0 && field("judged=") == notes && field("score=") == 0);

    /* vertical layout */
    press(BTN_CROSS); press(BTN_SELECT); run(0.2); snap("08_tate_list");
    press(BTN_CROSS); press(BTN_CROSS);
    notes = load_chart(cur_num(), (int)field("level="));
    autoplay(20); snap("09_tate_play"); say("tate 20 s");
    check("tate hits", field("hit=") > 0 && field("hit=") == field("judged="));
    in.held = BTN_START; frame(); in.held = 0; frame();
    press(BTN_CIRCLE); run(0.1);
    check("quit to list", field("scene=") == 1);

    /* high scores: the first run is recorded, a worse run does not replace it,
     * and they survive a restart */
    {
        char line[256];
        long saved = 0;
        int found = 0;
        FILE *fp = fopen(plat_save_file(), "r");
        while (fp && fgets(line, sizeof line, fp)) {
            char num[8];
            int lv, st, hi, ju;
            long sc;
            if (sscanf(line, "best %7s %d %ld %d %d %d", num, &lv, &sc, &st, &hi, &ju) == 6 && !strcmp(num, first_num)) {
                saved = sc; found = 1;
            }
        }
        if (fp) fclose(fp);
        check("song 1's run was saved", found && saved == first_score);
        printf("saved best for song %s: %ld (run scored %ld)\n", first_num, saved, first_score);

        /* replay it badly: the stored best must not go down */
        for (i = 0; i < 400 && field("row=") > 0; i++) press(BTN_UP);
        check("back at the first song", !strcmp(cur_num(), first_num) && field("scene=") == 1);
        press(BTN_CROSS); press(BTN_CROSS); run(0.05);
        run(20); snap("10b_second_run");
        in.held = BTN_START; frame(); in.held = 0; frame();
        press(BTN_CIRCLE); run(0.2);
        game_shutdown();
        if (game_init(w, h) != 0) { printf("re-init failed\n"); return 1; }
        {
            long again = 0;
            int lv2;
            fp = fopen(plat_save_file(), "r");
            while (fp && fgets(line, sizeof line, fp)) {
                char num[8];
                int st, hi, ju;
                long sc;
                if (sscanf(line, "best %7s %d %ld %d %d %d", num, &lv2, &sc, &st, &hi, &ju) == 6 && !strcmp(num, first_num))
                    again = sc;
            }
            if (fp) fclose(fp);
            check("a worse run did not overwrite it", again == saved);
            printf("after a worse run and a restart: %ld\n", again);
        }
        press(BTN_SELECT);               /* back to landscape for the remaining shots */
        run(0.6); snap("10c_list_with_scores");
        press(BTN_CROSS); run(0.2); snap("10d_level_with_best");
        check("best shown for a played song", !strcmp(cur_num(), first_num));
        press(BTN_CIRCLE); run(0.2);
        press(BTN_TRIANGLE); press(BTN_TRIANGLE); run(0.1); snap("10e_scores_tab");
        check("scores tab lists the played songs", field("view=") == 2 && field("row=") == 0);
        press(BTN_TRIANGLE); press(BTN_TRIANGLE); run(0.1);   /* on through Playlists to Songs */
        check("tabs wrap around", field("view=") == 0);
        press(BTN_START); run(0.1); snap("10f_blue_style");
        check("style switch", field("skin=") == 1);
        press(BTN_START); run(0.1); snap("10g_dark_style");
        check("dark style", field("skin=") == 2);
        press(BTN_START); run(0.1);
        check("styles wrap around", field("skin=") == 0);
        press(BTN_CROSS); run(0.2);
        press(BTN_CIRCLE); run(0.2);
    }

    /* flick the list: it should keep gliding after the finger lifts, then stop */
    {
        long before_scene = field("scene=");
        int k;
        in.n = 1; in.t[0].id = 1;
        for (k = 0; k < 6; k++) { game_l2s(160, 400 - k * 40.0f, &in.t[0].x, &in.t[0].y); frame(); }
        in.n = 0; frame();
        snap("11_flick_a"); run(0.5); snap("11_flick_b"); run(6.0); snap("11_flick_c"); run(1.0); snap("11_flick_d");
        check("flick stays on the list", field("scene=") == before_scene && before_scene == 1);
    }

    /* scroll to the end of the list */
    for (i = 0; i < 400; i++) press(BTN_R);
    snap("10_list_end"); say("list end");

    /* artists: an artist's page, where Play All plays their songs in turn */
    press(BTN_TRIANGLE); run(0.1);
    check("artists tab", field("view=") == 1);
    snap("12_artists"); say("artists");
    press(BTN_CROSS); run(0.2); snap("12b_artist_page"); say("artist page");
    check("artist page", field("page=") == 1);
    press(BTN_CROSS); run(0.1);                       /* Play All */
    check("play all starts a song", field("scene=") == 3 && field("queue=") == 0);
    {
        long was = field("num=");
        in.held = BTN_START; frame(); in.held = 0; frame();
        snap("12a_pause_skip");
        press(BTN_TRIANGLE); run(0.1);
        check("skip from the pause screen plays the next song",
              field("scene=") == 3 && field("queue=") == 1 && field("num=") != was && field("paused=") == 0);
    }
    in.held = BTN_START; frame(); in.held = 0; frame();
    press(BTN_CIRCLE); run(0.1);
    check("quitting a song ends play all", field("scene=") == 1 && field("queue=") == -1);
    press(BTN_CIRCLE); run(0.1);
    check("back to the artists", field("page=") == 0 && field("view=") == 1);

    /* play all to the end: results move on to the next song by themselves */
    press(BTN_CROSS); run(0.1); press(BTN_CROSS); run(0.1);
    {
        long first = field("num=");
        { int k; for (k = 0; k < 100000 && field("scene=") != 4; k++) frame(); }
        run(0.5); snap("12c_queue_results"); say("queue results");
        check("results while playing all", field("scene=") == 4 && field("queue=") == 0);
        run(9);
        check("next song started by itself", field("scene=") == 3 && field("queue=") == 1 && field("num=") != first);
        in.held = BTN_START; frame(); in.held = 0; frame();
        press(BTN_CIRCLE); run(0.1);
        press(BTN_CIRCLE); run(0.1);
    }

    /* playlists: make two, put a song on them from its song screen, keep them */
    press(BTN_TRIANGLE); press(BTN_TRIANGLE); run(0.1);
    check("playlists tab", field("view=") == 3);
    press(BTN_CROSS); run(0.2); snap("13_empty_playlist");
    check("new playlist opened", field("pls=") == 1 && field("page=") == 1);
    press(BTN_CIRCLE); run(0.1); snap("13b_playlists");
    press(BTN_TRIANGLE); run(0.1);
    press(BTN_CROSS); run(0.2);
    press(BTN_SQUARE); run(0.1); snap("14_add_sheet");
    check("add sheet up", field("sheet=") == 1);
    press(BTN_CROSS); run(0.1);                       /* tick Playlist 1 */
    press(BTN_DOWN); press(BTN_CROSS); run(0.1);      /* New Playlist, with this song on it */
    snap("14b_add_sheet_ticked"); say("ticked");
    check("song on both playlists", field("pl0=") == 1 && field("pls=") == 2);
    press(BTN_CIRCLE); run(0.1);
    check("sheet closed", field("sheet=") == 0 && field("scene=") == 2);
    press(BTN_CIRCLE); run(0.1);
    game_shutdown();
    if (game_init(w, h) != 0) { printf("re-init failed\n"); return 1; }
    check("playlists kept after a restart", field("pls=") == 2 && field("pl0=") == 1);

    /* a playlist's page, and taking the song off it in edit mode */
    run(0.4);
    for (i = 0; i < 3; i++) press(BTN_TRIANGLE);      /* Songs, Artists, Scores, Playlists */
    press(BTN_DOWN); press(BTN_CROSS); run(0.2); snap("15_playlist_page"); say("playlist page");
    check("playlist page", field("page=") == 1);
    press(BTN_TRIANGLE); run(0.1); snap("15b_edit");
    check("edit mode", field("edit=") == 1);
    press(BTN_CROSS); run(0.1);
    check("song taken off", field("pl0=") == 0);
    press(BTN_CROSS); run(0.1); snap("15c_delete_sheet");
    check("delete asks first", field("sheet=") == 2);
    press(BTN_CROSS); run(0.1);
    check("playlist deleted", field("pls=") == 1 && field("page=") == 0);

    game_shutdown();
    printf("textures alive after shutdown: %d\n", soft_live_tex);
    printf(fails ? "%d CHECK(S) FAILED\n" : "all checks passed\n", fails);
    return fails != 0;
}
