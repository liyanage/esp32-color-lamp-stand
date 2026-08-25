#include "wifi_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_NAMESPACE "storage"
#define WIFI_SSID_KEY "wifi_ssid"
#define WIFI_PASSWORD_KEY "wifi_password"
#define WIFI_SSID_SIZE 33
#define WIFI_PASSWORD_SIZE 65
#define WIFI_CONNECT_ATTEMPTS 3
#define WIFI_CONNECT_TIMEOUT_MS 45000
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BOOT_BUTTON_HOLD_MS 5000
#define PORTAL_IP "192.168.4.1"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1

typedef enum {
    PORTAL_STATE_READY,
    PORTAL_STATE_TESTING,
    PORTAL_STATE_FAILED,
    PORTAL_STATE_CONNECTED,
} portal_state_t;

static const char *LOG_TAG = "wifi-manager";
static EventGroupHandle_t s_wifi_events;
static wifi_setup_status_callback_t s_status_callback;
static esp_netif_t *s_ap_netif;
static bool s_portal_active;
static bool s_initial_connecting;
static bool s_connect_requested;
static bool s_pending_credentials;
static unsigned int s_connect_attempts;
static portal_state_t s_portal_state = PORTAL_STATE_READY;
static char s_pending_ssid[WIFI_SSID_SIZE];
static char s_pending_password[WIFI_PASSWORD_SIZE];
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

extern const unsigned char provisioning_html_start[] asm("_binary_provisioning_html_start");
extern const unsigned char provisioning_html_end[] asm("_binary_provisioning_html_end");

static void set_portal_state(portal_state_t state)
{
    portENTER_CRITICAL(&s_state_lock);
    s_portal_state = state;
    portEXIT_CRITICAL(&s_state_lock);
}

static portal_state_t get_portal_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    portal_state_t state = s_portal_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), LOG_TAG, "Unable to erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

static bool load_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t stored_ssid_size = ssid_size;
    size_t stored_password_size = password_size;
    esp_err_t ssid_result = nvs_get_str(handle, WIFI_SSID_KEY, ssid, &stored_ssid_size);
    esp_err_t password_result = nvs_get_str(handle, WIFI_PASSWORD_KEY, password, &stored_password_size);
    nvs_close(handle);
    return ssid_result == ESP_OK && password_result == ESP_OK && ssid[0] != '\0';
}

static esp_err_t save_pending_credentials(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle), LOG_TAG,
                        "Unable to open Wi-Fi settings");
    esp_err_t err = nvs_set_str(handle, WIFI_SSID_KEY, s_pending_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_PASSWORD_KEY, s_pending_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void erase_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_key(handle, WIFI_SSID_KEY);
    nvs_erase_key(handle, WIFI_PASSWORD_KEY);
    nvs_commit(handle);
    nvs_close(handle);
}

static void make_station_config(wifi_config_t *config, const char *ssid,
                                const char *password)
{
    memset(config, 0, sizeof(*config));
    size_t ssid_length = strnlen(ssid, WIFI_SSID_SIZE - 1);
    memcpy(config->sta.ssid, ssid, ssid_length);
    strlcpy((char *)config->sta.password, password, sizeof(config->sta.password));
    config->sta.threshold.authmode = WIFI_AUTH_OPEN;
}

static void restart_after_provisioning(void *unused)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static void wifi_event_handler(void *unused, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_connect_requested) {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;
        ESP_LOGW(LOG_TAG, "Wi-Fi disconnected, reason %d", event->reason);

        if (s_portal_active) {
            if (s_pending_credentials) {
                s_pending_credentials = false;
                set_portal_state(PORTAL_STATE_FAILED);
            }
            return;
        }

        if (s_initial_connecting) {
            ++s_connect_attempts;
            if (s_connect_attempts >= WIFI_CONNECT_ATTEMPTS) {
                xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
                return;
            }
        }
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        s_initial_connecting = false;

        if (s_portal_active && s_pending_credentials) {
            if (save_pending_credentials() == ESP_OK) {
                s_pending_credentials = false;
                set_portal_state(PORTAL_STATE_CONNECTED);
                xTaskCreate(restart_after_provisioning, "wifi-restart", 2048, NULL, 5, NULL);
            } else {
                s_pending_credentials = false;
                set_portal_state(PORTAL_STATE_FAILED);
            }
        }
    }
}

static void url_decode(char *value)
{
    char *source = value;
    char *destination = value;
    while (*source != '\0') {
        if (*source == '+') {
            *destination++ = ' ';
            ++source;
        } else if (*source == '%' && source[1] != '\0' && source[2] != '\0') {
            char encoded[3] = {source[1], source[2], '\0'};
            *destination++ = (char)strtoul(encoded, NULL, 16);
            source += 3;
        } else {
            *destination++ = *source++;
        }
    }
    *destination = '\0';
}

static esp_err_t portal_page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    size_t page_size = provisioning_html_end - provisioning_html_start;
    return httpd_resp_send(request, (const char *)provisioning_html_start, page_size);
}

static esp_err_t redirect_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t send_json_escaped(httpd_req_t *request, const char *text)
{
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "\"", 1), LOG_TAG,
                        "Unable to send JSON");
    for (const unsigned char *character = (const unsigned char *)text; *character != '\0'; ++character) {
        char escaped[7];
        const char *chunk = (const char *)character;
        size_t chunk_size = 1;
        if (*character == '\\' || *character == '\"') {
            escaped[0] = '\\';
            escaped[1] = (char)*character;
            chunk = escaped;
            chunk_size = 2;
        } else if (*character < 0x20) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *character);
            chunk = escaped;
            chunk_size = 6;
        }
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, chunk, chunk_size), LOG_TAG,
                            "Unable to send JSON");
    }
    return httpd_resp_send_chunk(request, "\"", 1);
}

static int compare_access_points(const void *left, const void *right)
{
    const wifi_ap_record_t *a = left;
    const wifi_ap_record_t *b = right;
    return b->rssi - a->rssi;
}

static esp_err_t networks_handler(httpd_req_t *request)
{
    if (get_portal_state() == PORTAL_STATE_TESTING) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "Connection test in progress");
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Wi-Fi scan failed");
    }

    uint16_t count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&count));
    if (count > 20) {
        count = 20;
    }
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (count > 0 && records == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&count, records));
    qsort(records, count, sizeof(*records), compare_access_points);

    httpd_resp_set_type(request, "application/json");
    httpd_resp_send_chunk(request, "[", 1);
    bool first = true;
    for (uint16_t index = 0; index < count; ++index) {
        if (records[index].ssid[0] == '\0') {
            continue;
        }
        if (!first) {
            httpd_resp_send_chunk(request, ",", 1);
        }
        first = false;
        httpd_resp_send_chunk(request, "{\"ssid\":", HTTPD_RESP_USE_STRLEN);
        send_json_escaped(request, (const char *)records[index].ssid);
        char details[64];
        snprintf(details, sizeof(details), ",\"rssi\":%d,\"open\":%s}",
                 records[index].rssi,
                 records[index].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        httpd_resp_send_chunk(request, details, HTTPD_RESP_USE_STRLEN);
    }
    free(records);
    httpd_resp_send_chunk(request, "]", 1);
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t connect_handler(httpd_req_t *request)
{
    if (get_portal_state() == PORTAL_STATE_TESTING) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "Connection test in progress");
    }

    if (request->content_len <= 0 || request->content_len >= 512) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form submission");
    }

    char body[512];
    size_t received = 0;
    while (received < request->content_len) {
        int result = httpd_req_recv(request, body + received, request->content_len - received);
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }
    body[received] = '\0';

    char ssid[WIFI_SSID_SIZE * 3] = {0};
    char password[WIFI_PASSWORD_SIZE * 3] = {0};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Choose a Wi-Fi network");
    }
    httpd_query_key_value(body, "password", password, sizeof(password));
    url_decode(ssid);
    url_decode(password);
    if (strlen(ssid) > WIFI_SSID_SIZE - 1 || strlen(password) > WIFI_PASSWORD_SIZE - 2) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Wi-Fi name or password is too long");
    }

    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    strlcpy(s_pending_password, password, sizeof(s_pending_password));

    wifi_config_t station_config;
    make_station_config(&station_config, s_pending_ssid, s_pending_password);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    if (err != ESP_OK) {
        set_portal_state(PORTAL_STATE_FAILED);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Unable to configure Wi-Fi");
    }
    s_pending_credentials = true;
    set_portal_state(PORTAL_STATE_TESTING);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_pending_credentials = false;
        set_portal_state(PORTAL_STATE_FAILED);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Unable to start connection test");
    }

    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"state\":\"testing\"}");
}

static esp_err_t status_handler(httpd_req_t *request)
{
    portal_state_t state = get_portal_state();
    const char *response;
    if (state == PORTAL_STATE_TESTING) {
        response = "{\"state\":\"testing\",\"message\":\"Testing the connection…\"}";
    } else if (state == PORTAL_STATE_FAILED) {
        response = "{\"state\":\"failed\",\"message\":\"Unable to connect. Check the password and try again.\"}";
    } else if (state == PORTAL_STATE_CONNECTED) {
        response = "{\"state\":\"connected\",\"message\":\"Connected. The lamp is restarting…\"}";
    } else {
        response = "{\"state\":\"ready\",\"message\":\"Choose a network to continue.\"}";
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 12;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), LOG_TAG, "Unable to start setup web server");

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = portal_page_handler},
        {.uri = "/api/networks", .method = HTTP_GET, .handler = networks_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/connect", .method = HTTP_POST, .handler = connect_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = portal_page_handler},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = portal_page_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = portal_page_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = portal_page_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler},
    };
    for (size_t index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[index]), LOG_TAG,
                            "Unable to register setup route");
    }
    return ESP_OK;
}

static void dns_server_task(void *unused)
{
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(LOG_TAG, "Unable to create captive DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(LOG_TAG, "Unable to bind captive DNS socket");
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t packet[512];
    while (true) {
        struct sockaddr_in client;
        socklen_t client_size = sizeof(client);
        int packet_size = recvfrom(socket_fd, packet, sizeof(packet) - 16, 0,
                                   (struct sockaddr *)&client, &client_size);
        if (packet_size < 12) {
            continue;
        }

        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0x00;
        packet[7] = 0x01;
        packet[8] = packet[9] = packet[10] = packet[11] = 0;
        const uint8_t answer[] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
            0x00, 0x3c, 0x00, 0x04, 192, 168, 4, 1,
        };
        memcpy(packet + packet_size, answer, sizeof(answer));
        sendto(socket_fd, packet, packet_size + sizeof(answer), 0,
               (struct sockaddr *)&client, client_size);
    }
}

static esp_err_t start_config_portal(void)
{
    ESP_LOGI(LOG_TAG, "Starting Wi-Fi setup portal");
    s_portal_active = true;
    s_connect_requested = false;
    s_initial_connecting = false;
    set_portal_state(PORTAL_STATE_READY);

    esp_wifi_stop();
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    char access_point_name[33];
    snprintf(access_point_name, sizeof(access_point_name), "ColorLamp-Setup-%02X%02X",
             mac[4], mac[5]);

    wifi_config_t access_point_config = {0};
    strlcpy((char *)access_point_config.ap.ssid, access_point_name,
            sizeof(access_point_config.ap.ssid));
    access_point_config.ap.ssid_len = strlen(access_point_name);
    access_point_config.ap.channel = 1;
    access_point_config.ap.max_connection = 4;
    access_point_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), LOG_TAG,
                        "Unable to enter setup mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &access_point_config), LOG_TAG,
                        "Unable to configure setup access point");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), LOG_TAG, "Unable to start setup access point");
    ESP_RETURN_ON_ERROR(start_http_server(), LOG_TAG, "Unable to start setup portal");
    xTaskCreate(dns_server_task, "captive-dns", 4096, NULL, 4, NULL);

    ESP_LOGI(LOG_TAG, "Connect to %s and open http://%s", access_point_name, PORTAL_IP);
    bool bright = false;
    while (true) {
        portal_state_t state = get_portal_state();
        if (s_status_callback != NULL) {
            if (state == PORTAL_STATE_TESTING) {
                s_status_callback(bright ? WIFI_SETUP_STATUS_TESTING : WIFI_SETUP_STATUS_TESTING_DIM);
            } else if (state == PORTAL_STATE_FAILED) {
                s_status_callback(WIFI_SETUP_STATUS_FAILED);
            } else if (state == PORTAL_STATE_CONNECTED) {
                s_status_callback(WIFI_SETUP_STATUS_CONNECTED);
            } else {
                s_status_callback(bright ? WIFI_SETUP_STATUS_PORTAL : WIFI_SETUP_STATUS_PORTAL_DIM);
            }
        }
        bright = !bright;
        vTaskDelay(pdMS_TO_TICKS(350));
    }
    return ESP_OK;
}

static void reset_button_task(void *parameter)
{
    (void)parameter;
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    TickType_t pressed_at = 0;
    while (true) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            if (pressed_at == 0) {
                pressed_at = xTaskGetTickCount();
            } else if ((xTaskGetTickCount() - pressed_at) * portTICK_PERIOD_MS >= BOOT_BUTTON_HOLD_MS) {
                if (s_status_callback != NULL) {
                    s_status_callback(WIFI_SETUP_STATUS_RESETTING);
                }
                erase_credentials();
                vTaskDelay(pdMS_TO_TICKS(700));
                esp_restart();
            }
        } else {
            pressed_at = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t wifi_manager_initialize_and_connect(wifi_setup_status_callback_t status_callback)
{
    s_status_callback = status_callback;
    ESP_RETURN_ON_ERROR(initialize_nvs(), LOG_TAG, "Unable to initialize NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), LOG_TAG, "Unable to initialize network interfaces");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), LOG_TAG, "Unable to create event loop");

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), LOG_TAG, "Unable to initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        LOG_TAG, "Unable to register Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        LOG_TAG, "Unable to register IP events");

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char ssid[WIFI_SSID_SIZE] = {0};
    char password[WIFI_PASSWORD_SIZE] = {0};
    if (!load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        return start_config_portal();
    }

    if (status_callback != NULL) {
        status_callback(WIFI_SETUP_STATUS_CONNECTING);
    }
    wifi_config_t station_config;
    make_station_config(&station_config, ssid, password);

    s_connect_attempts = 0;
    s_initial_connecting = true;
    s_connect_requested = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), LOG_TAG, "Unable to enter station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station_config), LOG_TAG,
                        "Unable to configure saved Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), LOG_TAG, "Unable to start Wi-Fi");

    EventBits_t result = xEventGroupWaitBits(s_wifi_events,
                                              WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                              pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((result & WIFI_CONNECTED_BIT) != 0) {
        ESP_LOGI(LOG_TAG, "Connected to saved Wi-Fi network %s", ssid);
        return ESP_OK;
    }
    ESP_LOGW(LOG_TAG, "Saved Wi-Fi unavailable; opening setup portal");
    return start_config_portal();
}

void wifi_manager_start_reset_button_monitor(wifi_setup_status_callback_t status_callback)
{
    s_status_callback = status_callback;
    xTaskCreate(reset_button_task, "wifi-reset-button", 3072, NULL, 5, NULL);
}
