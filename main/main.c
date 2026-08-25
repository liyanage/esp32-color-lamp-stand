#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "led_strip.h"
#include "pixel.h"
#include "usb_source.h"
#include "wifi_manager.h"

// Test mode uses a random number instead of performing an HTTP request
#define LED_TEST_MODE 0

#define LED_DATA_GPIO 13

#define LED_COUNT 32

#define ANIMATION_UPDATE_RATE_HZ 60
#define ANIMATION_DURATION_SECONDS 3.0

/* app state machine */
typedef enum application_state {
    application_state_start,
    application_state_offline,
    application_state_online,
} application_state;

#define ENUM_TO_STRING_CASE(x) case x: return #x
char *application_state_label_for_value(application_state state);
char *application_state_label_for_value(application_state state) {
    switch (state) {
        ENUM_TO_STRING_CASE(application_state_start);
        ENUM_TO_STRING_CASE(application_state_offline);
        ENUM_TO_STRING_CASE(application_state_online);
    }
    return NULL;
}

/* app state data */
typedef struct application_data {
    application_state state;
    esp_timer_handle_t periodic_timer;
    led_strip_handle_t led_strip;
    bool timer_is_armed;
    bool have_valid_value;
    double value;
    pixel_color_t current_pixel_color;
} application_data_t;

static application_data_t application_data;

typedef struct stock_data {
    bool have_valid_value;
    double value;
} stock_data_t;

/* prototypes */
static void periodic_timer_callback(void* arg);
static void initialize_led_strip(application_data_t *app_data);
static void start_timer(application_data_t *app_data);
static void stop_timer(application_data_t *app_data);
esp_err_t _http_event_handle_nasdaq(esp_http_client_event_t *evt);
static void update_led_strip(pixel_color_t pixel_color, led_strip_handle_t led_strip);
static void query_stock_data_and_update_led_strip(application_data_t *app_data);
static void wifi_status_changed(wifi_setup_status_t status);
static void show_usb_power_capability(usb_source_measurement_t measurement,
                                      led_strip_handle_t led_strip);
void application_transition_to_state(application_state *current_state, application_state new_state);
void get_stock_data_nasdaq(stock_data_t *stock_data);

static const char *LOG_TAG = "color-lamp-stand-app";
static uint8_t maximum_led_brightness = 3;
static SemaphoreHandle_t led_mutex;

void application_transition_to_state(application_state *current_state, application_state new_state) {
    ESP_LOGI(LOG_TAG, "*** Application state transition from %s to %s\n", application_state_label_for_value(*current_state), application_state_label_for_value(new_state));
    *current_state = new_state;
}


void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ESP_LOGI(LOG_TAG, "Wifi event handler, event id %d, event data %p", event_id, event_data);
    application_data_t *app_data = event_handler_arg;

    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(LOG_TAG, "WIFI_EVENT_STA_START");
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(LOG_TAG, "WIFI_EVENT_STA_CONNECTED");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(LOG_TAG, "WIFI_EVENT_STA_DISCONNECTED");
        application_transition_to_state(&app_data->state, application_state_offline);
        stop_timer(app_data);
    }    
}

void ip_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ESP_LOGI(LOG_TAG, "IP event handler, event id %d, event data %p", event_id, event_data);
    application_data_t *app_data = event_handler_arg;

    if (event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(LOG_TAG, "IP_EVENT_STA_GOT_IP");

        application_transition_to_state(&app_data->state, application_state_online);
        // force an initial update
        query_stock_data_and_update_led_strip(app_data);
        start_timer(app_data);
    } else if (0) {

    }    

}


void app_main(void)
{
    ESP_LOGI(LOG_TAG, "Starting color lamp stand");

    usb_source_measurement_t usb_source = usb_source_measure();
    maximum_led_brightness = usb_source.maximum_led_brightness;

    initialize_led_strip(&application_data);
    update_led_strip(pixel_color_black, application_data.led_strip);
    show_usb_power_capability(usb_source, application_data.led_strip);

    const esp_timer_create_args_t periodic_timer_args = {
            .callback = &periodic_timer_callback,
            .arg = (void *) &application_data,
            .name = "periodic"
    };

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &application_data.periodic_timer));

    ESP_ERROR_CHECK(wifi_manager_initialize_and_connect(wifi_status_changed));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, &application_data));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               ip_event_handler, &application_data));
    wifi_manager_start_reset_button_monitor(wifi_status_changed);

    application_transition_to_state(&application_data.state, application_state_online);
    query_stock_data_and_update_led_strip(&application_data);
    start_timer(&application_data);

}

static void wifi_status_changed(wifi_setup_status_t status)
{
    pixel_color_t color = pixel_color_black;
    switch (status) {
        case WIFI_SETUP_STATUS_CONNECTING:
            color = (pixel_color_t){.brightness = 6, .b = 0xff};
            break;
        case WIFI_SETUP_STATUS_PORTAL:
            color = (pixel_color_t){.brightness = 12, .r = 0xff, .g = 0x60};
            break;
        case WIFI_SETUP_STATUS_PORTAL_DIM:
            color = (pixel_color_t){.brightness = 3, .r = 0xff, .g = 0x60};
            break;
        case WIFI_SETUP_STATUS_TESTING:
            color = (pixel_color_t){.brightness = 12, .b = 0xff};
            break;
        case WIFI_SETUP_STATUS_TESTING_DIM:
            color = (pixel_color_t){.brightness = 3, .b = 0xff};
            break;
        case WIFI_SETUP_STATUS_FAILED:
            color = (pixel_color_t){.brightness = 8, .r = 0xff};
            break;
        case WIFI_SETUP_STATUS_CONNECTED:
            color = (pixel_color_t){.brightness = 8, .g = 0xff};
            break;
        case WIFI_SETUP_STATUS_RESETTING:
            color = (pixel_color_t){.brightness = 15, .r = 0xff};
            break;
    }
    update_led_strip(color, application_data.led_strip);
}

static void initialize_led_strip(application_data_t *app_data)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_DATA_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = true,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config,
                                             &app_data->led_strip));

    led_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(led_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
}

static void start_timer(application_data_t *app_data) {
    if (!app_data->timer_is_armed) {
        ESP_LOGI(LOG_TAG, "Starting timer");
#if LED_TEST_MODE
        ESP_ERROR_CHECK(esp_timer_start_periodic(app_data->periodic_timer, 5 * 1000000));
#else
        ESP_ERROR_CHECK(esp_timer_start_periodic(app_data->periodic_timer, 5 * 60 * 1000000));
#endif
        app_data->timer_is_armed = true;
    }
}

static void stop_timer(application_data_t *app_data) {
    if (app_data->timer_is_armed) {
        ESP_LOGI(LOG_TAG, "Stopping timer");
        ESP_ERROR_CHECK(esp_timer_stop(app_data->periodic_timer));
        app_data->timer_is_armed = false;
    }
}

static void periodic_timer_callback(void* arg)
{
    int64_t time_since_boot = esp_timer_get_time();
    ESP_LOGI(LOG_TAG, "Periodic timer called, time since boot: %lld us", time_since_boot);
    application_data_t *app_data = arg;
    query_stock_data_and_update_led_strip(app_data);
}

static void query_stock_data_and_update_led_strip(application_data_t *app_data) {
    ESP_LOGI(LOG_TAG, "Querying data");

    stock_data_t data;
    bzero(&data, sizeof(data));

#if LED_TEST_MODE
    data.have_valid_value = true;
    data.value = esp_random() % 2 ? -1.0 : 1.0;
#else
    get_stock_data_nasdaq(&data);
#endif

    pixel_color_t new_color = pixel_color_white;
    if (data.have_valid_value) {
        ESP_LOGI(LOG_TAG, "New value %f", data.value);
        new_color = data.value < 0 ? pixel_color_red : pixel_color_green;
    } else {
        ESP_LOGE(LOG_TAG, "Unable to get valid value");
    }

    if (pixel_color_equal(app_data->current_pixel_color, new_color)) {
        ESP_LOGI(LOG_TAG, "No value/color change, skipping LED update");
        return;
    }

    int update_steps = ANIMATION_DURATION_SECONDS * ANIMATION_UPDATE_RATE_HZ;
    double update_increment = 1.0 / update_steps;
    TickType_t update_step_delay_ticks = ((TickType_t)(ANIMATION_DURATION_SECONDS * 1000) / update_steps / portTICK_PERIOD_MS);
    for (double x = 0.0; x <= 1.0; x += update_increment) {
        // pixel_color_t step_color = interpolate_pixel_color(app_data->current_pixel_color, new_color, x);
        pixel_color_t step_color = interpolate_pixel_color3(app_data->current_pixel_color, pixel_color_black, new_color, x);
        update_led_strip(step_color, app_data->led_strip);
        vTaskDelay(update_step_delay_ticks);
    }

    app_data->current_pixel_color = new_color;

}

static uint8_t apply_brightness(uint8_t channel, uint8_t brightness)
{
    return ((uint16_t)channel * brightness + 15) / 31;
}

static void update_led_strip_pixels(const pixel_color_t pixels[LED_COUNT],
                                    led_strip_handle_t led_strip)
{
    xSemaphoreTake(led_mutex, portMAX_DELAY);

    for (int pixel = 0; pixel < LED_COUNT; ++pixel) {
        uint8_t brightness = pixels[pixel].brightness;
        if (brightness > maximum_led_brightness) {
            brightness = maximum_led_brightness;
        }
        uint8_t red = apply_brightness(pixels[pixel].r, brightness);
        uint8_t green = apply_brightness(pixels[pixel].g, brightness);
        uint8_t blue = apply_brightness(pixels[pixel].b, brightness);
        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, pixel, red, green, blue));
    }
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    xSemaphoreGive(led_mutex);
}

static void update_led_strip(pixel_color_t pixel_color, led_strip_handle_t led_strip)
{
    pixel_color_t pixels[LED_COUNT];
    for (int pixel = 0; pixel < LED_COUNT; ++pixel) {
        pixels[pixel] = pixel_color;
    }
    update_led_strip_pixels(pixels, led_strip);
}

static void show_usb_power_capability(usb_source_measurement_t measurement,
                                      led_strip_handle_t led_strip)
{
    if (!measurement.measurement_valid) {
        pixel_color_t error = {.brightness = 10, .r = 0xff};
        for (int flash = 0; flash < 2; ++flash) {
            update_led_strip(error, led_strip);
            vTaskDelay(pdMS_TO_TICKS(180));
            update_led_strip(pixel_color_black, led_strip);
            vTaskDelay(pdMS_TO_TICKS(180));
        }
    }

    int illuminated_pixels;
    pixel_color_t gauge_color;
    switch (measurement.current) {
        case USB_SOURCE_CURRENT_3_A:
            illuminated_pixels = LED_COUNT;
            gauge_color = (pixel_color_t){.brightness = 10, .g = 0xff};
            break;
        case USB_SOURCE_CURRENT_1_5_A:
            illuminated_pixels = LED_COUNT / 2;
            gauge_color = (pixel_color_t){.brightness = 10, .g = 0x80, .b = 0xff};
            break;
        case USB_SOURCE_CURRENT_DEFAULT:
        default:
            illuminated_pixels = LED_COUNT / 4;
            gauge_color = (pixel_color_t){.brightness = 10, .r = 0xff, .g = 0x60};
            break;
    }

    pixel_color_t pixels[LED_COUNT] = {0};
    TickType_t sweep_delay = pdMS_TO_TICKS(500 / illuminated_pixels);
    for (int pixel = 0; pixel < illuminated_pixels; ++pixel) {
        pixels[pixel] = gauge_color;
        update_led_strip_pixels(pixels, led_strip);
        vTaskDelay(sweep_delay);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint8_t starting_brightness = gauge_color.brightness;
    if (starting_brightness > maximum_led_brightness) {
        starting_brightness = maximum_led_brightness;
    }
    for (int brightness = starting_brightness; brightness >= 0; --brightness) {
        for (int pixel = 0; pixel < illuminated_pixels; ++pixel) {
            pixels[pixel].brightness = brightness;
        }
        update_led_strip_pixels(pixels, led_strip);
        vTaskDelay(pdMS_TO_TICKS(70));
    }
}

void get_stock_data_nasdaq(stock_data_t *stock_data) {
    esp_http_client_config_t config = {
        .url = "https://api.nasdaq.com/api/quote/AAPL/info?assetclass=stocks",
        .event_handler = _http_event_handle_nasdaq,
        .user_data = stock_data,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Origin", "https://www.nasdaq.com");
    esp_http_client_set_header(client, "Host", "api.nasdaq.com");
    esp_http_client_set_header(client, "Referer", "https://www.nasdaq.com/market-activity/stocks/aapl/real-time");
    esp_http_client_set_header(client, "Connection", "keep-alive");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/13.0.2 Safari/605.1.15");
    esp_http_client_set_header(client, "Accept-Language", "en-us");

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(LOG_TAG, "Status = %d, content_length = %d", esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(LOG_TAG, "Error performing HTTP request: %d", err);
    }
    esp_http_client_cleanup(client);
}

esp_err_t _http_event_handle_nasdaq(esp_http_client_event_t *evt) {
    stock_data_t *data = evt->user_data;

    switch(evt->event_id) {
        case HTTP_EVENT_HEADER_SENT:
        case HTTP_EVENT_ON_HEADER:
        case HTTP_EVENT_ON_HEADERS_COMPLETE:
        case HTTP_EVENT_ON_STATUS_CODE:
        case HTTP_EVENT_ON_CONNECTED:
        case HTTP_EVENT_ON_FINISH:
        case HTTP_EVENT_DISCONNECTED:
        case HTTP_EVENT_REDIRECT:
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(LOG_TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(LOG_TAG, "HTTP_EVENT_ON_DATA, len=%d chunked=%d", evt->data_len, esp_http_client_is_chunked_response(evt->client));
            ESP_LOGI(LOG_TAG, "%.*s\n", evt->data_len, (char*)evt->data);
            if (!(data->have_valid_value)) {
                char *match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":", strlen("\"deltaIndicator\":"));
                if (match) {
                    bool up_match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":\"up\"", strlen("\"deltaIndicator\":\"up\"")) != NULL;
                    bool down_match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":\"down\"", strlen("\"deltaIndicator\":\"down\"")) != NULL;
                    if (up_match) {
                        ESP_LOGI(LOG_TAG, "Found 'up' indicator");
                        data->have_valid_value = true;
                        data->value = 1.0;
                    } else if (down_match) {
                        ESP_LOGI(LOG_TAG, "Found 'down' indicator");
                        data->have_valid_value = true;
                        data->value = -1.0;
                    } else {
                        ESP_LOGE(LOG_TAG, "Unable to find 'up' or 'down' value:");
                        ESP_LOGE(LOG_TAG, "%.*s", evt->data_len, (char*)evt->data);
                    }
                }                
            }
            break;
    }
    return ESP_OK;
}
