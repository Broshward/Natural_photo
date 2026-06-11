#ifndef MANUAL_CAM_H
#define MANUAL_CAM_H

#include <stdio.h>
#include "esp_err.h"

// Карта пинов платы Freenove V1695
#define CAM_XCLK_IO       15
#define CAM_PCLK_IO       13
#define CAM_HREF_IO       7  
#define CAM_VSYNC_IO      6

// Перепутанные китайские пины данных D0-D7 из вашего config.h!
#define CAM_D0_IO         11
#define CAM_D1_IO         9
#define CAM_D2_IO         8
#define CAM_D3_IO         10
#define CAM_D4_IO         12
#define CAM_D5_IO         18
#define CAM_D6_IO         17
#define CAM_D7_IO         16

/**
 * @brief Делает ручной захват кадра UXGA YUV422 напрямую через FIFO аппаратного блока LCD_CAM
 * @param buffer Указатель на выделенный буфер в 8 МБ PSRAM
 * @param expected_size Размер кадра (1600 * 1200 * 2 = 3 840 000 байт)
 */
esp_err_t take_photo(uint8_t *buffer, size_t expected_size);

void i2c_init();
esp_err_t sccb_write(uint16_t reg, uint8_t val);
uint8_t sccb_read(uint16_t reg);

#endif // MANUAL_CAM_H
