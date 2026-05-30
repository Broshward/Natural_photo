#include "ota_update.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"

static const char *TAG = "ota_update";
#define BUFF_SIZE 1024

bool perform_tcp_ota(int sock, size_t ota_size) 
{
    ESP_LOGW(TAG, "Старт OTA обновления. Ожидаемый размер: %d байт", ota_size);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Не найден пассивный OTA раздел во флеш-памяти!");
        return false;
    }
    ESP_LOGI(TAG, "Пишем в раздел: %s", update_partition->label);

    esp_ota_handle_t update_handle = 0 ;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка функции esp_ota_begin: %s", esp_err_to_name(err));
        return false;
    }

    char *ota_write_data = malloc(BUFF_SIZE);
    if (!ota_write_data) {
        ESP_LOGE(TAG, "Не удалось выделить память под буфер OTA");
        esp_ota_abort(update_handle);
        return false;
    }

    size_t binary_size_received = 0;
    while (binary_size_received < ota_size) {
        int data_read = recv(sock, ota_write_data, BUFF_SIZE, 0);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Ошибка приема данных прошивки по сокету");
            break;
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "Соединение закрыто сервером");
            break;
        }

        err = esp_ota_write(update_handle, (const void *)ota_write_data, data_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Ошибка записи во флеш: %s", esp_err_to_name(err));
            break;
        }
        binary_size_received += data_read;
        ESP_LOGD(TAG, "Прошито: %d/%d байт", binary_size_received, ota_size);
    }

    free(ota_write_data);
    ESP_LOGI(TAG, "Всего принято бинарника: %d байт", binary_size_received);

    if (binary_size_received != ota_size) {
        ESP_LOGE(TAG, "Размер принятого файла не совпадает с заявленным!");
        esp_ota_abort(update_handle);
        return false;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка валидации прошивки (esp_ota_end): %s", esp_err_to_name(err));
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось переключить загрузочный раздел: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGW(TAG, "ОБНОВЛЕНИЕ УСПЕШНО ЗАВЕРШЕНО! Плата готова к перезапуску.");
    return true;
}
