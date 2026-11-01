idf_component_register(SRCS "fw_update.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update bootloader_support esp_timer)
