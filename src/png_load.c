/* png_load.c - decode a PNG file to a malloc'd RGBA8 buffer (libpng >= 1.6) */
#include <png.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

unsigned char *png_load_rgba(const char *path, int *w, int *h)
{
    png_image img;
    unsigned char *buf;

    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path))
        return NULL;
    img.format = PNG_FORMAT_RGBA;
    buf = malloc(PNG_IMAGE_SIZE(img));
    if (!buf) {
        png_image_free(&img);
        return NULL;
    }
    if (!png_image_finish_read(&img, NULL, buf, 0, NULL)) {
        png_image_free(&img);
        free(buf);
        return NULL;
    }
    *w = (int)img.width;
    *h = (int)img.height;
    return buf;
}
