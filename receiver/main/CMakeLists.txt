idf_component_register(SRCS "main.c"
    "led.c"
    "wifi_manager.c"
    "usb_msc.c"
    "storage_manager.c"
    "state_machine.c"
    "file_receiver.c"
    "sd_card.c"
    "mdns_responder.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif lwip
    mbedtls esp_http_server esp_http_client fatfs vfs wear_levelling
    esp_tinyusb driver json led_strip sdmmc spi_flash mdns esp_app_format)