#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

// console support (config menu)
#include "linenoise/linenoise.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"

#include "configuration_menu.h"

char s_wifi_ssid[WIFI_CREDENTIAL_BUFFER_SIZE];
char s_wifi_password[WIFI_CREDENTIAL_BUFFER_SIZE];

typedef enum configuration_state {
    configuration_state_start,
    configuration_state_error,
    configuration_state_check_for_nvs_configuration_data,
    configuration_state_waiting_for_menu_choice,
    configuration_state_restarting,
    configuration_state_querying_for_config_menu,
    configuration_state_starting_config_menu,
    configuration_state_running_config_menu,
    configuration_state_querying_for_wifi_credentials,
    configuration_state_success
} configuration_state;

#define ENUM_TO_STRING_CASE(x) case x: return #x
char *configuration_state_label_for_value(configuration_state state);
char *configuration_state_label_for_value(configuration_state state) {
    switch (state) {
        ENUM_TO_STRING_CASE(configuration_state_start);
        ENUM_TO_STRING_CASE(configuration_state_error);
        ENUM_TO_STRING_CASE(configuration_state_check_for_nvs_configuration_data);
        ENUM_TO_STRING_CASE(configuration_state_waiting_for_menu_choice);
        ENUM_TO_STRING_CASE(configuration_state_restarting);
        ENUM_TO_STRING_CASE(configuration_state_querying_for_config_menu);
        ENUM_TO_STRING_CASE(configuration_state_starting_config_menu);
        ENUM_TO_STRING_CASE(configuration_state_running_config_menu);
        ENUM_TO_STRING_CASE(configuration_state_querying_for_wifi_credentials);
        ENUM_TO_STRING_CASE(configuration_state_success);
    }
    return NULL;
}

#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASSWORD "wifi_password"
#define CONFIGURATION_MENU_TIMEOUT_SECONDS 2

bool open_nvs_handle(nvs_handle *handle);
void configuration_transition_to_state(configuration_state *current_state, configuration_state new_state);
bool read_nvs_config_wifi_credentials(nvs_handle my_handle);


bool run_configuration_menu_state_machine(void) {
    esp_err_t err = ESP_OK;
    configuration_state state = configuration_state_start;

    while (1) {
        if (state == configuration_state_start) {
            // Initialize NVS
            err = nvs_flash_init();
            if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                printf("Failed to initialize NVS, erasing and retrying\n");
                // NVS partition was truncated and needs to be erased
                // Retry nvs_flash_init
                ESP_ERROR_CHECK(nvs_flash_erase());
                err = nvs_flash_init();
            }

            if (err != ESP_OK) {
                printf("Failed to initialize NVS\n");
                configuration_transition_to_state(&state, configuration_state_error);
                continue;
            }

            configuration_transition_to_state(&state, configuration_state_check_for_nvs_configuration_data);
        } else if (state == configuration_state_check_for_nvs_configuration_data) {
            nvs_handle my_handle = NULL;
            if (!open_nvs_handle(&my_handle)) {
                configuration_transition_to_state(&state, configuration_state_error);
                continue;
            }

            bool did_find_nvs_wifi_data = read_nvs_config_wifi_credentials(my_handle);

            bool did_find_all_required_nvs_configuration_data = did_find_nvs_wifi_data /* && did_find_XXXX_data */;

            if (did_find_all_required_nvs_configuration_data) {
                configuration_transition_to_state(&state, configuration_state_querying_for_config_menu);
            } else {
                printf("Forcing configuration mode\n");
                configuration_transition_to_state(&state, configuration_state_starting_config_menu);
            }

            nvs_close(my_handle);            
        } else if (state == configuration_state_querying_for_config_menu) {
            bool should_enter_configuration_menu = false;
            for (int i = 0; i < CONFIGURATION_MENU_TIMEOUT_SECONDS && !should_enter_configuration_menu; i++) {
                printf("Press any key within the next %d seconds to enter configuration mode\n", CONFIGURATION_MENU_TIMEOUT_SECONDS - i);
                uint8_t ch = fgetc(stdin);
                if (ch != 255) {
                    printf("Key press detected\n");
                    should_enter_configuration_menu = true;
                    continue;
                }
                sleep(1);
            }
            if (should_enter_configuration_menu) {
                configuration_transition_to_state(&state, configuration_state_starting_config_menu);
            } else {
                configuration_transition_to_state(&state, configuration_state_success);
            }
        } else if (state == configuration_state_starting_config_menu) {
            /* Disable buffering on stdin */
            setvbuf(stdin, NULL, _IONBF, 0);

            printf("Entering configuration mode\n");
            sleep(1);

            /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
            esp_vfs_dev_uart_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
            /* Move the caret to the beginning of the next line on '\n' */
            esp_vfs_dev_uart_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

            /* Configure UART. Note that REF_TICK is used so that the baud rate remains
            * correct while APB frequency is changing in light sleep mode.
            */
            const uart_config_t uart_config = {
                    .baud_rate = 115200,
                    .data_bits = UART_DATA_8_BITS,
                    .parity = UART_PARITY_DISABLE,
                    .stop_bits = UART_STOP_BITS_1,
                    .use_ref_tick = true
            };
            ESP_ERROR_CHECK(uart_param_config(0, &uart_config));
            /* Install UART driver for interrupt-driven reads and writes */
            ESP_ERROR_CHECK(uart_driver_install(0, 256, 0, 0, NULL, 0));
            esp_vfs_dev_uart_use_driver(0);

            linenoiseSetDumbMode(1);
            configuration_transition_to_state(&state, configuration_state_running_config_menu);
        } else if (state == configuration_state_running_config_menu) {
            printf("\nChoose a setting to change:\n");
            printf("w - WiFi settings\n");
            printf("r - Reboot\n");
            char *line = linenoise("> ");
            printf("\n");
            if (line == NULL) {
                continue;
            }
            char menu_choice_letter = line[0];
            linenoiseFree(line);

            switch (menu_choice_letter) {
                case 'w':
                    configuration_transition_to_state(&state, configuration_state_querying_for_wifi_credentials);
                    break;

                case 'r':
                    configuration_transition_to_state(&state, configuration_state_restarting);
                    break;
            }
        } else if (state == configuration_state_querying_for_wifi_credentials) {
            nvs_handle my_handle = NULL;
            if (!open_nvs_handle(&my_handle)) {
                configuration_transition_to_state(&state, configuration_state_error);
                continue;
            }

            bool did_update_wifi_credentials = false;
            char *line = linenoise("WiFi SSID: ");
            printf("\n");
            if (line) {
                strlcpy(s_wifi_ssid, line, WIFI_CREDENTIAL_BUFFER_SIZE);
                linenoiseFree(line);
                line = linenoise("WiFi Password: ");
                printf("\n");
                if (line) {
                    strlcpy(s_wifi_password, line, WIFI_CREDENTIAL_BUFFER_SIZE);
                    linenoiseFree(line);
                    printf("Updating WiFi credentials in NVS\n");
                    err = nvs_set_str(my_handle, NVS_KEY_WIFI_SSID, s_wifi_ssid);
                    if (err == ESP_OK) {
                        err = nvs_set_str(my_handle, NVS_KEY_WIFI_PASSWORD, s_wifi_password);
                    }
                    if (err == ESP_OK) {
                        did_update_wifi_credentials = true;
                    }
                }
            }
            if (did_update_wifi_credentials) {
                printf("Successfully updated WiFi credentials in NVS\n");
            } else {
                printf("Did not update WiFi credentials in NVS\n");
            }
            configuration_transition_to_state(&state, configuration_state_running_config_menu);
            nvs_close(my_handle);
        } else if (state == configuration_state_success) {
            printf("Configuration succeeded\n");
            return true;
        } else if (state == configuration_state_error) {
            printf("Configuration failed\n");
            configuration_transition_to_state(&state, configuration_state_restarting);
        } else if (state == configuration_state_restarting) {
            printf("Restarting...\n");
            sleep(1);
            esp_restart();
        } else {
            printf("Unknown configuration state %d\n", state);
            configuration_transition_to_state(&state, configuration_state_error);
        }
    }   
}

bool read_nvs_config_wifi_credentials(nvs_handle my_handle) {
    printf("Reading WiFi info from NVS ...\n");
    bool did_find_nvs_wifi_data = false;
    size_t buffer_length = WIFI_CREDENTIAL_BUFFER_SIZE;
    bzero(s_wifi_ssid, WIFI_CREDENTIAL_BUFFER_SIZE);
    bzero(s_wifi_password, WIFI_CREDENTIAL_BUFFER_SIZE);
    esp_err_t err = nvs_get_str(my_handle, NVS_KEY_WIFI_SSID, s_wifi_ssid, &buffer_length);
    if (err == ESP_OK) {
        buffer_length = WIFI_CREDENTIAL_BUFFER_SIZE;
        err = nvs_get_str(my_handle, NVS_KEY_WIFI_PASSWORD, s_wifi_password, &buffer_length);
        if (err == ESP_OK) {
            did_find_nvs_wifi_data = true;
        }
    }

    if (did_find_nvs_wifi_data) {
        printf("Found WiFi info in NVS, SSID = %s\n", s_wifi_ssid);
    } else {
        printf("Did not find WiFi info in NVS: %s\n", esp_err_to_name(err));
    }
    return did_find_nvs_wifi_data;
}

bool query_float_value(char *prompt, float *out_value) {
    bool did_get_value = false;
    char *line = linenoise(prompt);
    printf("\n");
    if (line) {
        char *endptr;
        float value = strtof(line, &endptr);
        if (endptr != line) {
            *out_value = value;
            did_get_value = true;
        } else {
            printf("Unable to parse input as floating point: %s\n", line);
        }
        linenoiseFree(line);
    }
    return did_get_value;
}

bool open_nvs_handle(nvs_handle *handle) {
    printf("Opening Non-Volatile Storage (NVS) handle...\n");
    esp_err_t err = nvs_open("storage", NVS_READWRITE, handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

void configuration_transition_to_state(configuration_state *current_state, configuration_state new_state) {
    // printf("*** Configuration state transition from %s to %s\n", configuration_state_label_for_value(*current_state), configuration_state_label_for_value(new_state));
    *current_state = new_state;
}