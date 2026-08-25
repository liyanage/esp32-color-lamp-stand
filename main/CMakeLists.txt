idf_component_register(
    SRCS "main.c" "pixel.c" "usb_source.c" "wifi_manager.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "provisioning.html"
    REQUIRES esp_adc esp_driver_gpio esp_driver_spi esp_event esp_http_client
             esp_http_server esp_netif esp_timer esp_wifi lwip mbedtls nvs_flash
)
