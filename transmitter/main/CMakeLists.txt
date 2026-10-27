idf_component_register(SRCS "main.c"
                            "led.c"
                            "wifi_manager.c"
                            "auth.c"
                            "file_transfer.c"
                            "web_server.c"
                            "rx_registry.c"
                            "espnow_pair.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_event esp_netif lwip
                             mbedtls esp_http_server esp_http_client
                             json driver led_strip esp_app_format esp_timer
                             mdns pair_proto esp_rom)

# Web UI: ../web bundled into one gzipped HTML, embedded in the app image
set(UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../web")
set(UI_GZ "${CMAKE_CURRENT_BINARY_DIR}/ui.html.gz")
file(GLOB UI_SRCS "${UI_DIR}/*")
idf_build_get_property(python PYTHON)
add_custom_command(OUTPUT "${UI_GZ}"
                   COMMAND "${python}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/bundle_ui.py" "${UI_DIR}" "${UI_GZ}"
                   DEPENDS ${UI_SRCS} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/bundle_ui.py"
                   VERBATIM)
add_custom_target(ui_bundle DEPENDS "${UI_GZ}")
target_add_binary_data(${COMPONENT_LIB} "${UI_GZ}" BINARY DEPENDS ui_bundle)
