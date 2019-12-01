#include "sdkconfig.h"

// console support (config menu)
#include "linenoise/linenoise.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"

#define WIFI_CREDENTIAL_BUFFER_SIZE 80

extern char s_wifi_ssid[WIFI_CREDENTIAL_BUFFER_SIZE];
extern char s_wifi_password[WIFI_CREDENTIAL_BUFFER_SIZE];

bool run_configuration_menu_state_machine(void);

