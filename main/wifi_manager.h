#pragma once

#include "esp_err.h"

typedef enum {
    WIFI_SETUP_STATUS_CONNECTING,
    WIFI_SETUP_STATUS_PORTAL,
    WIFI_SETUP_STATUS_PORTAL_DIM,
    WIFI_SETUP_STATUS_TESTING,
    WIFI_SETUP_STATUS_TESTING_DIM,
    WIFI_SETUP_STATUS_FAILED,
    WIFI_SETUP_STATUS_CONNECTED,
    WIFI_SETUP_STATUS_RESETTING,
} wifi_setup_status_t;

typedef void (*wifi_setup_status_callback_t)(wifi_setup_status_t status);

esp_err_t wifi_manager_initialize_and_connect(wifi_setup_status_callback_t status_callback);
void wifi_manager_start_reset_button_monitor(wifi_setup_status_callback_t status_callback);
