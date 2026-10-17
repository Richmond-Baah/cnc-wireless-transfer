idf_component_register(SRCS "main.c"
                            "led.c"
                            "wifi_manager.c"
                            "auth.c"
                            "file_transfer.c"
                            "web_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_event esp_netif lwip
                             mbedtls esp_http_server esp_http_client
                             spiffs json driver led_strip esp_app_format esp_timer)

spiffs_create_partition_image(spiffs ../spiffs_data FLASH_IN_PROJECT)
