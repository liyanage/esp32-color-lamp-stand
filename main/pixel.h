#include <stdbool.h>
#include <stdint.h>

typedef struct pixel_color {
    uint8_t brightness : 5;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} pixel_color_t;

extern pixel_color_t pixel_color_red;
extern pixel_color_t pixel_color_green;
extern pixel_color_t pixel_color_black;
extern pixel_color_t pixel_color_white;

bool pixel_color_equal(pixel_color_t c1, pixel_color_t c2);
pixel_color_t interpolate_pixel_color(pixel_color_t c1, pixel_color_t c2, double x);
pixel_color_t interpolate_pixel_color3(pixel_color_t c1, pixel_color_t c2, pixel_color_t c3, double x);
