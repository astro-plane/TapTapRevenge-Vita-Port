/* main.c - program entry: open the platform, run the frame loop */
#include "game.h"

#ifdef __vita__
int _newlib_heap_size_user = 48 * 1024 * 1024;
#endif

int main(int argc, char **argv)
{
    int w, h;
    double last;
    PlatInput in;
    (void)argc; (void)argv;

    if (plat_init(&w, &h) != 0)
        return 1;
    if (game_init(w, h) != 0) {
        plat_shutdown();
        return 1;
    }
    last = plat_time();
    for (;;) {
        double now = plat_time();
        plat_poll(&in);
        if (!game_frame(now - last, &in))
            break;
        last = now;
    }
    game_shutdown();
    plat_shutdown();
    return 0;
}
