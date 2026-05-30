#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Запускает процесс скачивания и прошивки нового бинарника по TCP сокету
 * @param sock Активный сокет, соединенный с сервером
 * @param ota_size Размер файла прошивки в байтах
 * @return true если обновление прошло успешно и плата готова к ребуту
 */
bool perform_tcp_ota(int sock, size_t ota_size);

#endif // OTA_UPDATE_H


