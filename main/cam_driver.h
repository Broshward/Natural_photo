#ifndef CAM_DRIVER_H
#define CAM_DRIVER_H

#include <stdio.h>
#include "esp_err.h"

/**
 * @brief Инициализирует новый Camera Controller от Espressif и делает один снимок UXGA YUV422
 * @param[out] out_len Сюда запишется точный размер принятого кадра (3 840 000 байт)
 * @return Указатель на выделенный буфер в PSRAM с кадром, или NULL при сбое
 */
uint8_t* cam_driver_take_uxga_photo(size_t *out_len);

#endif // CAM_DRIVER_H
