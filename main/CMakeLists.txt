idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_camera fatfs vfs esp_driver_sdmmc esp_timer esp_wifi esp_netif nvs_flash)


