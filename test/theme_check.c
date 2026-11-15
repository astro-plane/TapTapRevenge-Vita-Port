/* theme_check.c - load every theme/level pair that songs.txt refers to and
 * report any the game cannot read.  Usage: theme_check (with TTR_DATA set) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plat.h"
#include "../src/theme.h"

void gfx_draw(struct PlatTex *t, int sx, int sy, int sw, int sh, float cx, float cy,
              float w, float h, float rot, unsigned rgba, int blend)
{
    plat_draw(t, sx, sy, sw, sh, cx, cy, w, h, rot, rgba, blend);
}

int main(void)
{
    char path[512], line[512], seen[4096][40];
    int n_seen = 0, bad = 0, ok = 0, w, h, i;
    FILE *fp;
    plat_init(&w, &h);
    snprintf(path, sizeof path, "%ssongs.txt", plat_data_dir());
    fp = fopen(path, "r");
    if (!fp) { printf("no songs.txt\n"); return 1; }
    while (fgets(line, sizeof line, fp)) {
        char *p = line, *tok;
        int tabs = 0;
        line[strcspn(line, "\r\n")] = 0;
        while (tabs < 5 && (p = strchr(p, '\t')) != NULL) { p++; tabs++; }
        if (tabs < 5 || !p) { printf("song line without themes: %.20s\n", line); bad++; continue; }
        for (tok = strtok(p, ","); tok; tok = strtok(NULL, ",")) {
            int dup = 0;
            for (i = 0; i < n_seen; i++) if (!strcmp(seen[i], tok)) dup = 1;
            if (dup || n_seen >= 4096 || strlen(tok) < 3 || strlen(tok) >= 40) continue;
            strcpy(seen[n_seen++], tok);
            if (theme_load(tok + 2, tok[0] - '0') != 0) { printf("FAILED: %s\n", tok); bad++; }
            else {
                NotePos np, np2;
                theme_update(0.5, 0); theme_event(TH_STREAK10); theme_event(TH_MULT8); theme_event(TH_MISS);
                theme_feedback(1, 0, Q_PERFECT); theme_update(0.5, 1);
                theme_note_pos(0, 1, 0.5f, &np); theme_note_pos(1, 0, 0.4f, &np2); theme_draw_scene(0); theme_draw_note(2, 1, &np, 1);
                theme_draw_hold(1, 1, &np, &np2, 0.2f, 0.4f); theme_draw_splash(0, 1, 1); theme_draw_scene(1);
                if (theme_window() < 0.5f || theme_line_y() < 200 || theme_lane_x(0) == theme_lane_x(2)) {
                    printf("ODD GEOMETRY: %s window %.2f line %.0f lanes %.0f %.0f %.0f\n", tok, theme_window(),
                           theme_line_y(), theme_lane_x(0), theme_lane_x(1), theme_lane_x(2));
                    bad++;
                }
                ok++;
            }
            theme_free();
        }
    }
    fclose(fp);
    printf("%d theme/level pairs loaded, %d problems\n", ok, bad);
    return bad != 0;
}
