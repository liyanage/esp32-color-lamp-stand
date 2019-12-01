#include <stdio.h>
#include <string.h>

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

// console support (config menu)
#include "configuration_menu.h"

#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK  14

#define LED_COUNT 1
#define LED_STRIP_HEADER_SIZE 4
#define LED_STRIP_TRAILER_SIZE 4
#define LED_STRIP_BUFFER_SIZE (LED_COUNT * 4 + LED_STRIP_HEADER_SIZE + LED_STRIP_TRAILER_SIZE)
DRAM_ATTR char led_strip_data[LED_STRIP_BUFFER_SIZE];

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

void application_transition_to_state(application_state *current_state, application_state new_state);
void application_transition_to_state(application_state *current_state, application_state new_state) {
    printf("*** Application state transition from %s to %s\n", application_state_label_for_value(*current_state), application_state_label_for_value(new_state));
    *current_state = new_state;
}

/* app state data */
typedef struct application_data {
    application_state state;
    esp_timer_handle_t periodic_timer;
    spi_device_handle_t spi_device_handle;
    int timer_is_armed;
    int have_valid_value;
    double value;
} application_data_t;


typedef struct http_request_data {
    int have_valid_value;
    double value;
} http_request_data_t;

/*
DRAM_ATTR char data1[] = {
    0, 0, 0, 0,
    0xff, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0xff
};

DRAM_ATTR char data2[] = {
    0, 0, 0, 0,
    0xff, 0xff, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff
};
*/


/* prototypes */
static void periodic_timer_callback(void* arg);
void initialize_spi(application_data_t *app_data);
static void start_timer(application_data_t *app_data);
static void stop_timer(application_data_t *app_data);
esp_err_t _http_event_handle(esp_http_client_event_t *evt);
void update_led_strip(double value, spi_device_handle_t spi);
static void update_value(application_data_t *app_data);


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
            update_value(app_data);
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


/*
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1*1000*1000,
        .mode = 0,                                //SPI mode 0
        .spics_io_num = -1,               //CS pin
        .queue_size = 1,                          //We want to be able to queue 7 transactions at a time
        .pre_cb = NULL,  //Specify pre-transfer callback to handle D/C line
        .command_bits = 0,
        .address_bits = 0
    };

    //Initialize the SPI bus
    ret = spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    ESP_ERROR_CHECK(ret);

    printf("spi before %p\n", application_data.spi_device_handle);
    ret = spi_bus_add_device(HSPI_HOST, &devcfg, &application_data.spi_device_handle);
    printf("spi after %p\n", application_data.spi_device_handle);
    ESP_ERROR_CHECK(ret);

    ret =  spi_device_acquire_bus(application_data.spi_device_handle, portMAX_DELAY);
    ESP_ERROR_CHECK(ret);

    int flag = 0;
    while (1) {
        flag = !flag;
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));       //Zero out the transaction
        t.length = 96;                     //Command is 8 bits
        t.user = (void *)0;                //D/C needs to be set to 0

        if (flag) {
            t.tx_buffer = data1;               //The data is the cmd itself        
        } else {
            t.tx_buffer = data2;               //The data is the cmd itself        
        }

        ret = spi_device_polling_transmit(application_data.spi_device_handle, &t);  //Transmit!
        assert(ret == ESP_OK);            //Should have had no issues.

        vTaskDelay(2000 / portTICK_RATE_MS);
        
    }

*/


}

void initialize_spi(application_data_t *app_data) {
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_DMA_LEN
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1*1000*1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .pre_cb = NULL,
        .command_bits = 0,
        .address_bits = 0
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
        ESP_ERROR_CHECK(esp_timer_start_periodic(app_data->periodic_timer, 5 * 60 * 1000000));
        // ESP_ERROR_CHECK(esp_timer_start_periodic(app_data->periodic_timer, 5 * 1000000));
        app_data->timer_is_armed = 1;
    }
}

static void stop_timer(application_data_t *app_data) {
    if (app_data->timer_is_armed) {
        printf("Stopping timer\n");
        ESP_ERROR_CHECK(esp_timer_stop(app_data->periodic_timer));
        app_data->timer_is_armed = 0;
    }
}

static void periodic_timer_callback(void* arg)
{
    int64_t time_since_boot = esp_timer_get_time();
    printf("Periodic timer called, time since boot: %lld us\n", time_since_boot);
    application_data_t *app_data = arg;
    update_value(app_data);
}

static void update_value(application_data_t *app_data) {
    printf("Updating value\n");

    http_request_data_t data;
    bzero(&data, sizeof(data));

    esp_http_client_config_t config = {
        .url = "https://www.alphavantage.co/query?function=ROC&symbol=AAPL&interval=60min&time_period=10&series_type=close&apikey=" ALPHAVANTAGE_API_KEY,
        .event_handler = _http_event_handle,
        .user_data = &data,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        printf("Status = %d, content_length = %d\n", esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
    }
    esp_http_client_cleanup(client);

    // data.have_valid_value = 1;
    // data.value = 0.5454;

    if (data.have_valid_value) {
        update_led_strip(data.value, app_data->spi_device_handle);
    } else {
        printf("Unable to get valid value\n");
        update_led_strip(0.0, app_data->spi_device_handle);
    }
    
    // debug
    // stop_timer(app_data);
}

void update_led_strip(double value, spi_device_handle_t spi) {
    printf("Updating LEDs with value %f\n", value);

    char *p = led_strip_data;

    memset(p, 0, LED_STRIP_HEADER_SIZE);
    p += LED_STRIP_HEADER_SIZE;
    for (int i = 0; i < LED_COUNT; i++)
    {
        *p++ = 0xff;
        if (value > 0) {
            *p++ = 0x00;
            *p++ = 0xff;
            *p++ = 0x00;
        } else if (value < 0) {
            *p++ = 0x00;
            *p++ = 0x00;
            *p++ = 0xff;
        } else {
            *p++ = 0xff;
            *p++ = 0xff;
            *p++ = 0xff;
        }
    }
    
    memset(p, 0xff, LED_STRIP_TRAILER_SIZE);

    // int i = 0;
    // for (char *q = led_strip_data; q < led_strip_data + LED_STRIP_BUFFER_SIZE; q++) {
    //     printf("LED %02d %p %x\n", i++, q, *q);
    // }
    
    esp_err_t ret;

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = LED_STRIP_BUFFER_SIZE * 8;
    t.user = (void *)0;
    t.tx_buffer = led_strip_data;

    ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
}


#define VALUE_BUFFER_LEN 80
esp_err_t _http_event_handle(esp_http_client_event_t *evt) {
    http_request_data_t *data = evt->user_data;

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
                char *match = strstr(evt->data, "\"ROC\":");
                if (match) {
                    char *end = NULL;
                    // "ROC": "0.5454"
                    double value = strtod(match + 8, &end);
                    if (end) {
                        printf("Found ROC value %f\n", value);
                        data->have_valid_value = 1;
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

