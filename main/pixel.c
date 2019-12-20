
#include "pixel.h"


pixel_color_t pixel_color_red = {.brightness = 20, .r = 0xff};
pixel_color_t pixel_color_green = {.brightness = 20, .g = 0xff};
pixel_color_t pixel_color_black = {.brightness = 0};
pixel_color_t pixel_color_white = {.brightness = 20, .r = 0xff, .g = 0xff, .b = 0xff};

bool pixel_color_equal(pixel_color_t c1, pixel_color_t c2) {
    return c1.brightness == c2.brightness
        && c1.r == c2.r
        && c1.g == c2.g
        && c1.b == c2.b;
}

pixel_color_t interpolate_pixel_color(pixel_color_t c1, pixel_color_t c2, double x) {
    double a = 1.0 - x;
    double b = x;
    pixel_color_t result = {
        .brightness = c1.brightness * a + c2.brightness * b,
        .r = c1.r * a + c2.r * b,
        .g = c1.g * a + c2.g * b,
        .b = c1.b * a + c2.b * b,
    };
    return result;
}

pixel_color_t interpolate_pixel_color3(pixel_color_t c1, pixel_color_t c2, pixel_color_t c3, double x) {
    pixel_color_t ca;
    pixel_color_t cb;
    if (x < 0.5) {
        ca = c1;
        cb = c2;
        x *= 2;
    } else {
        ca = c2;
        cb = c3;
        x = (x - 0.5) * 2;
    }
    return interpolate_pixel_color(ca, cb, x);
}
