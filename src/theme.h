/* theme.h - per-song themes: the scenery, lane shapes and reactions that the
 * original game's theme scripts set up, loaded from app_data/themes/<name>/. */
#ifndef THEME_H
#define THEME_H

typedef struct { float x, y, scale, alpha; } NotePos;

enum { TH_STREAK10, TH_MULT2, TH_MULT3, TH_MULT4, TH_MULT8, TH_MISS, TH_PHASES };
enum { Q_MISS, Q_BAD, Q_OK, Q_GOOD, Q_PERFECT };

int theme_load(const char *name, int level);      /* 0 on success */
void theme_free(void);
void theme_reset(void);                           /* back to the start-of-song state */
void theme_update(double dt, int revenge_on);
void theme_event(int phase);
void theme_feedback(int lane, int shake, int quality);
void theme_draw_scene(int group);                 /* 0: behind the notes, 1: in front */

void theme_screen(float *w, float *h);            /* 320x480, or 480x320 for sideways themes */
int theme_lanes(void);                            /* 3 or 4 */
float theme_window(void);                         /* seconds a note is on screen */
float theme_lane_x(int lane);                     /* where each lane meets the line */
float theme_line_y(void);
void theme_note_pos(int lane, int shake, float progress, NotePos *out);
void theme_draw_note(int lane, int shake, const NotePos *p, float alpha);
/* one piece of a hold trail, from 'near' (closer to the line) to 'far';
 * v_far..v_near (0..1) pick the part of the trail image this piece shows */
void theme_draw_hold(int lane, int active, const NotePos *near_end, const NotePos *far_end, float v_far, float v_near);
void theme_draw_splash(int lane, float size, float alpha);

/* provided by game.c: draw part of a texture in logical 320x480 points */
struct PlatTex;
void gfx_draw(struct PlatTex *t, int sx, int sy, int sw, int sh, float cx, float cy,
              float w, float h, float rot, unsigned rgba, int blend);

#endif
