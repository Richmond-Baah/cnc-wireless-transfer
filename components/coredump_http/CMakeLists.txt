idf_component_register(SRCS "coredump_http.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server espcoredump spi_flash)
