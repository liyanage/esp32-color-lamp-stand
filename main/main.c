#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "alphavantage_api_key.h"
#include "pixel.h"

// Serial console boot-time config menu support
#include "configuration_menu.h"

// Test mode uses a random number instead of performing an HTTP request
#define LED_TEST_MODE 0

#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK  14

#define LED_COUNT 32
#define LED_STRIP_HEADER_SIZE_BYTES 4
// The data sheet says the end sequence is 4 bytes, but reports say
// it's depending on the number of chained LEDs so this needs
// to be adjusted for longer strips. It needs to be at least LED_COUNT / 2 bits.
#define LED_STRIP_TRAILER_SIZE_BYTES 4
#define LED_STRIP_BUFFER_SIZE_BYTES (LED_COUNT * 4 + LED_STRIP_HEADER_SIZE_BYTES + LED_STRIP_TRAILER_SIZE_BYTES)
#define LED_STRIP_BUFFER_SIZE_BITS (LED_STRIP_BUFFER_SIZE_BYTES * 8)
DRAM_ATTR char led_strip_data[LED_STRIP_BUFFER_SIZE_BYTES];

#define LED_PREAMBLE_START 0xe0
// Range 0 - 31
#define LED_BRIGHTNESS 10
#define LED_PREAMBLE (LED_PREAMBLE_START | LED_BRIGHTNESS)
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
    spi_device_handle_t spi_device_handle;
    bool timer_is_armed;
    bool have_valid_value;
    double value;
    pixel_color_t current_pixel_color;
} application_data_t;

typedef struct stock_data {
    bool have_valid_value;
    double value;
} stock_data_t;

/* prototypes */
static void periodic_timer_callback(void* arg);
void initialize_spi(application_data_t *app_data);
static void start_timer(application_data_t *app_data);
static void stop_timer(application_data_t *app_data);
esp_err_t _http_event_handle_alphavantage(esp_http_client_event_t *evt);
esp_err_t _http_event_handle_nasdaq(esp_http_client_event_t *evt);
void update_led_strip(pixel_color_t pixel_color, spi_device_handle_t spi);
static void query_stock_data_and_update_led_strip(application_data_t *app_data);
void application_transition_to_state(application_state *current_state, application_state new_state);
void get_stock_data_nasdaq(stock_data_t *stock_data);
void get_stock_data_alphavantage(stock_data_t *stock_data);


void application_transition_to_state(application_state *current_state, application_state new_state) {
    printf("*** Application state transition from %s to %s\n", application_state_label_for_value(*current_state), application_state_label_for_value(new_state));
    *current_state = new_state;
}


esp_err_t event_handler(void *ctx, system_event_t *event) {
    printf("Event handler\n");
    application_data_t *app_data = ctx;

    switch(event->event_id) {
        case SYSTEM_EVENT_STA_START:
            printf("SYSTEM_EVENT_STA_START\n");
            ESP_ERROR_CHECK(esp_wifi_connect());
            break;

        case SYSTEM_EVENT_STA_GOT_IP:
            printf("SYSTEM_EVENT_STA_GOT_IP\n");
            printf("Got IP: '%s'\n", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));

            application_transition_to_state(&app_data->state, application_state_online);
            // force an initial update
            query_stock_data_and_update_led_strip(app_data);
            start_timer(app_data);
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED:
            printf("SYSTEM_EVENT_STA_DISCONNECTED\n");
            ESP_ERROR_CHECK(esp_wifi_connect());

            application_transition_to_state(&app_data->state, application_state_offline);
            stop_timer(app_data);
            break;

        default:
            break;
    }
    return ESP_OK;
}


void app_main(void)
{
    printf("Hello world!\n");

    if (!run_configuration_menu_state_machine()) {
        printf("Unable to get configuration information, will restart in 10 seconds...\n");
        sleep(10);
        esp_restart();
    }

    static application_data_t application_data;

    initialize_spi(&application_data);

    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, &application_data));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t sta_config = {
        .sta = {
            .bssid_set = false
        }
    };
    strlcpy((char *)sta_config.sta.ssid, s_wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, s_wifi_password, sizeof(sta_config.sta.password));
    // printf("debug: WiFi credentials: %s %s\n", sta_config.sta.ssid, sta_config.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const esp_timer_create_args_t periodic_timer_args = {
            .callback = &periodic_timer_callback,
            .arg = (void *) &application_data,
            .name = "periodic"
    };

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &application_data.periodic_timer));

}

void initialize_spi(application_data_t *app_data) {
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1*1000*1000,
        .spics_io_num = -1,
        .queue_size = 1,
        .pre_cb = NULL,
    };

    ret = spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    ESP_ERROR_CHECK(ret);

    ret = spi_bus_add_device(HSPI_HOST, &devcfg, &app_data->spi_device_handle);
    ESP_ERROR_CHECK(ret);

    ret =  spi_device_acquire_bus(app_data->spi_device_handle, portMAX_DELAY);
    ESP_ERROR_CHECK(ret);    
}

static void start_timer(application_data_t *app_data) {
    if (!app_data->timer_is_armed) {
        printf("Starting timer\n");
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
        printf("Stopping timer\n");
        ESP_ERROR_CHECK(esp_timer_stop(app_data->periodic_timer));
        app_data->timer_is_armed = false;
    }
}

static void periodic_timer_callback(void* arg)
{
    int64_t time_since_boot = esp_timer_get_time();
    printf("Periodic timer called, time since boot: %lld us\n", time_since_boot);
    application_data_t *app_data = arg;
    query_stock_data_and_update_led_strip(app_data);
}

static void query_stock_data_and_update_led_strip(application_data_t *app_data) {
    printf("Querying data\n");

    stock_data_t data;
    bzero(&data, sizeof(data));

#if LED_TEST_MODE
    data.have_valid_value = true;
    data.value = esp_random() % 2 ? -1.0 : 1.0;
#else
    // get_stock_data_alphavantage(&data);
    get_stock_data_nasdaq(&data);
#endif

    pixel_color_t new_color = pixel_color_white;
    if (data.have_valid_value) {
        printf("New value %f\n", data.value);
        new_color = data.value < 0 ? pixel_color_red : pixel_color_green;
    } else {
        printf("Unable to get valid value\n");
    }

    if (pixel_color_equal(app_data->current_pixel_color, new_color)) {
        printf("No value/color change, skipping LED update\n");
        return;
    }

    int update_steps = ANIMATION_DURATION_SECONDS * ANIMATION_UPDATE_RATE_HZ;
    double update_increment = 1.0 / update_steps;
    TickType_t update_step_delay_ticks = ((TickType_t)(ANIMATION_DURATION_SECONDS * 1000) / update_steps / portTICK_PERIOD_MS);
    for (double x = 0.0; x <= 1.0; x += update_increment) {
        // pixel_color_t step_color = interpolate_pixel_color(app_data->current_pixel_color, new_color, x);
        pixel_color_t step_color = interpolate_pixel_color3(app_data->current_pixel_color, pixel_color_black, new_color, x);
        update_led_strip(step_color, app_data->spi_device_handle);
        vTaskDelay(update_step_delay_ticks);
    }

    app_data->current_pixel_color = new_color;

}

void update_led_strip(pixel_color_t pixel_color, spi_device_handle_t spi) {
    char *p = led_strip_data;

    memset(p, 0, LED_STRIP_HEADER_SIZE_BYTES);
    p += LED_STRIP_HEADER_SIZE_BYTES;
    for (int i = 0; i < LED_COUNT; i++)
    {
        *p++ = (LED_PREAMBLE_START | pixel_color.brightness);
        *p++ = pixel_color.b;
        *p++ = pixel_color.g;
        *p++ = pixel_color.r;
    }
    
    // Set trailing bytes to 0 instead of 1 as the datasheet
    // specifies because it doesn't seem to matter (it's only needed to
    // drive extra clock pulses) and setting to 0 will not light up an
    // extra LED at the end if there is one.
    memset(p, 0x00, LED_STRIP_TRAILER_SIZE_BYTES);

    // int i = 0;
    // for (char *q = led_strip_data; q < led_strip_data + LED_STRIP_BUFFER_SIZE_BYTES; q++) {
    //     printf("LED %02d %p %x\n", i++, q, *q);
    // }
    
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = LED_STRIP_BUFFER_SIZE_BITS;
    t.user = (void *)0;
    t.tx_buffer = led_strip_data;

    ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
}

void get_stock_data_alphavantage(stock_data_t *stock_data) {
    esp_http_client_config_t config = {
        .url = "https://www.alphavantage.co/query?function=ROC&symbol=AAPL&interval=5min&time_period=1&series_type=close&apikey=" ALPHAVANTAGE_API_KEY,
        .event_handler = _http_event_handle_alphavantage,
        .user_data = stock_data,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        printf("Status = %d, content_length = %d\n", esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
    } else {
        printf("Error performing HTTP request: %d\n", err);
    }
    esp_http_client_cleanup(client);
}


esp_err_t _http_event_handle_alphavantage(esp_http_client_event_t *evt) {
    stock_data_t *data = evt->user_data;

    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            printf("HTTP_EVENT_ERROR\n");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            printf("HTTP_EVENT_ON_CONNECTED\n");
            break;
        case HTTP_EVENT_HEADER_SENT:
            // printf("HTTP_EVENT_HEADER_SENT\n");
            break;
        case HTTP_EVENT_ON_HEADER:
            // printf("HTTP_EVENT_ON_HEADER\n");
            // printf("%.*s", evt->data_len, (char*)evt->data);
            break;
        case HTTP_EVENT_ON_DATA:
            // printf("HTTP_EVENT_ON_DATA, len=%d chunked=%d\n", evt->data_len, esp_http_client_is_chunked_response(evt->client));
            // printf("%.*s", evt->data_len, (char*)evt->data);
            if (!(data->have_valid_value)) {
                char *match = memmem(evt->data, evt->data_len, "\"ROC\":", strlen("\"ROC\":"));
                if (match) {
                    char *end = NULL;
                    // "ROC": "0.5454"
                    double value = strtod(match + 8, &end);
                    if (end) {
                        printf("Found ROC value %f\n", value);
                        data->have_valid_value = true;
                        data->value = value;
                    } else {
                        printf("Unable to parse value\n");
                    }
                }                
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            // printf("HTTP_EVENT_ON_FINISH\n");
            break;
        case HTTP_EVENT_DISCONNECTED:
            // printf("HTTP_EVENT_DISCONNECTED\n");
            break;
    }
    return ESP_OK;
}

void get_stock_data_nasdaq(stock_data_t *stock_data) {
    esp_http_client_config_t config = {
        .url = "https://api.nasdaq.com/api/quote/AAPL/quote-bar?assetclass=stocks",
        .event_handler = _http_event_handle_nasdaq,
        .user_data = stock_data,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Origin", "https://www.nasdaq.com");
    esp_http_client_set_header(client, "Host", "api.nasdaq.com");
    esp_http_client_set_header(client, "Referer", "https://www.nasdaq.com/market-activity/stocks/aapl/real-time");
    esp_http_client_set_header(client, "Connection", "keep-alive");
    esp_http_client_set_header(client, "Accept-Encoding", "gzip, deflate, br");
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/13.0.2 Safari/605.1.15");
    esp_http_client_set_header(client, "Accept-Language", "en-us");

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        printf("Status = %d, content_length = %d\n", esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
    } else {
        printf("Error performing HTTP request: %d\n", err);
    }
    esp_http_client_cleanup(client);
}

esp_err_t _http_event_handle_nasdaq(esp_http_client_event_t *evt) {
    stock_data_t *data = evt->user_data;

    switch(evt->event_id) {
        case HTTP_EVENT_HEADER_SENT:
        case HTTP_EVENT_ON_HEADER:
            break;

        case HTTP_EVENT_ERROR:
            printf("HTTP_EVENT_ERROR\n");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            printf("HTTP_EVENT_ON_CONNECTED\n");
            break;
        case HTTP_EVENT_ON_DATA:
            printf("HTTP_EVENT_ON_DATA, len=%d chunked=%d\n", evt->data_len, esp_http_client_is_chunked_response(evt->client));
            printf("%.*s\n", evt->data_len, (char*)evt->data);
            if (!(data->have_valid_value)) {
                char *match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":", strlen("\"deltaIndicator\":"));
                if (match) {
                    bool up_match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":\"up\"", strlen("\"deltaIndicator\":\"up\"")) != NULL;
                    bool down_match = memmem(evt->data, evt->data_len, "\"deltaIndicator\":\"down\"", strlen("\"deltaIndicator\":\"down\"")) != NULL;
                    if (up_match) {
                        printf("Found 'up' indicator\n");
                        data->have_valid_value = true;
                        data->value = 1.0;
                    } else if (down_match) {
                        printf("Found 'down' indicator\n");
                        data->have_valid_value = true;
                        data->value = -1.0;
                    } else {
                        printf("Unable to find 'up' or 'down' value:\n");
                        printf("%.*s", evt->data_len, (char*)evt->data);
                    }
                }                
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            // printf("HTTP_EVENT_ON_FINISH\n");
            break;
        case HTTP_EVENT_DISCONNECTED:
            // printf("HTTP_EVENT_DISCONNECTED\n");
            break;
    }
    return ESP_OK;
}

