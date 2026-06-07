idf_component_register(SRCS "main.c"
					"ota_update.c"
					"adc.c"
					"cam_driver.c" 

                    INCLUDE_DIRS "."
                    REQUIRES esp_camera fatfs vfs esp_driver_sdmmc esp_timer esp_wifi esp_netif nvs_flash app_update esp_adc esp_driver_cam esp_driver_i2c)


