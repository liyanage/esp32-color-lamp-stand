#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USB_SOURCE_CURRENT_DEFAULT,
    USB_SOURCE_CURRENT_1_5_A,
    USB_SOURCE_CURRENT_3_A,
} usb_source_current_t;

typedef struct {
    usb_source_current_t current;
    int cc1_mv;
    int cc2_mv;
    uint8_t maximum_led_brightness;
    bool measurement_valid;
} usb_source_measurement_t;

usb_source_measurement_t usb_source_measure(void);
const char *usb_source_current_name(usb_source_current_t current);
