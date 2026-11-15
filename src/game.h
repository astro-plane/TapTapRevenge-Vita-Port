#ifndef GAME_H
#define GAME_H
#include "plat.h"

int game_init(int screen_w, int screen_h);          /* 0 on success        */
int game_frame(double dt, const PlatInput *in);     /* 0 when it wants out */
void game_shutdown(void);
void game_l2s(float lx, float ly, float *sx, float *sy);   /* logical -> screen */
void game_debug(char *buf, int n);

#endif
