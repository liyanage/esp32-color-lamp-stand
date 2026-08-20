idf_component_register(
    SRCS "main.c" "configuration_menu.c" "pixel.c" "usb_source.c"
    INCLUDE_DIRS "."
    REQUIRES esp_adc esp_driver_spi esp_event esp_http_client
             esp_netif esp_timer esp_wifi mbedtls nvs_flash
    PRIV_REQUIRES console
)
