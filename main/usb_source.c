#include "usb_source.h"

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define CC1_ADC_CHANNEL ADC_CHANNEL_3
#define CC2_ADC_CHANNEL ADC_CHANNEL_4
#define CC_ADC_ATTENUATION ADC_ATTEN_DB_12
#define CC_SAMPLE_COUNT 32

// The 100k/100k dividers halve the USB-C CC voltages. These thresholds sit
// between the ranges specified for default, 1.5 A, and 3 A source current.
#define CC_1_5_A_THRESHOLD_MV 325
#define CC_3_A_THRESHOLD_MV 620

#define DEFAULT_CURRENT_MAX_BRIGHTNESS 3
#define CURRENT_1_5_A_MAX_BRIGHTNESS 30
#define CURRENT_3_A_MAX_BRIGHTNESS 31

static const char *LOG_TAG = "usb-source";

static bool create_calibration(adc_channel_t channel, adc_cali_handle_t *handle)
{
    adc_cali_curve_fitting_config_t config = {
        .unit_id = ADC_UNIT_1,
        .chan = channel,
        .atten = CC_ADC_ATTENUATION,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&config, handle) == ESP_OK;
}

static int read_average_mv(adc_oneshot_unit_handle_t adc, adc_channel_t channel,
                           adc_cali_handle_t calibration)
{
    int64_t total_mv = 0;
    for (int sample = 0; sample < CC_SAMPLE_COUNT; ++sample) {
        int raw = 0;
        int voltage_mv = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(adc, channel, &raw));
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(calibration, raw, &voltage_mv));
        total_mv += voltage_mv;
    }
    return total_mv / CC_SAMPLE_COUNT;
}

const char *usb_source_current_name(usb_source_current_t current)
{
    switch (current) {
        case USB_SOURCE_CURRENT_1_5_A:
            return "1.5 A";
        case USB_SOURCE_CURRENT_3_A:
            return "3 A";
        case USB_SOURCE_CURRENT_DEFAULT:
        default:
            return "USB default";
    }
}

usb_source_measurement_t usb_source_measure(void)
{
    usb_source_measurement_t result = {
        .current = USB_SOURCE_CURRENT_DEFAULT,
        .maximum_led_brightness = DEFAULT_CURRENT_MAX_BRIGHTNESS,
    };

    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &adc));

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = CC_ADC_ATTENUATION,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, CC1_ADC_CHANNEL, &channel_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, CC2_ADC_CHANNEL, &channel_config));

    adc_cali_handle_t cc1_calibration = NULL;
    adc_cali_handle_t cc2_calibration = NULL;
    bool cc1_calibrated = create_calibration(CC1_ADC_CHANNEL, &cc1_calibration);
    bool cc2_calibrated = create_calibration(CC2_ADC_CHANNEL, &cc2_calibration);

    if (cc1_calibrated && cc2_calibrated) {
        result.cc1_mv = read_average_mv(adc, CC1_ADC_CHANNEL, cc1_calibration);
        result.cc2_mv = read_average_mv(adc, CC2_ADC_CHANNEL, cc2_calibration);
        int active_cc_mv = result.cc1_mv > result.cc2_mv ? result.cc1_mv : result.cc2_mv;

        if (active_cc_mv >= CC_3_A_THRESHOLD_MV) {
            result.current = USB_SOURCE_CURRENT_3_A;
            result.maximum_led_brightness = CURRENT_3_A_MAX_BRIGHTNESS;
        } else if (active_cc_mv >= CC_1_5_A_THRESHOLD_MV) {
            result.current = USB_SOURCE_CURRENT_1_5_A;
            result.maximum_led_brightness = CURRENT_1_5_A_MAX_BRIGHTNESS;
        }
    } else {
        ESP_LOGW(LOG_TAG, "ADC calibration unavailable; using the conservative USB-default limit");
    }

    if (cc1_calibration != NULL) {
        ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(cc1_calibration));
    }
    if (cc2_calibration != NULL) {
        ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(cc2_calibration));
    }
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc));

    ESP_LOGI(LOG_TAG, "CC1=%d mV, CC2=%d mV, advertised current=%s, LED brightness cap=%u/31",
             result.cc1_mv, result.cc2_mv, usb_source_current_name(result.current),
             result.maximum_led_brightness);
    return result;
}
