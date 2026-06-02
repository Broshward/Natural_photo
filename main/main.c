#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// Железо и ФС
#include "esp_camera.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "ff.h"

// Сеть и OTA
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "ota_update.h"

static const char *TAG = "greenhouse_cam";

#define WIFI_SSID           "SamstillingHeimar"
#define WIFI_PASS           "HarmoniesWorlds"
#define SERVER_IP           "192.168.43.1" 
#define SERVER_PORT         8888

#define TARGET_PERIOD_SEC   600             
#define MOUNT_POINT         "/sdcard"

// Пины Freenove V1695
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4  
#define SIOC_GPIO_NUM     5  
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       11
#define Y4_GPIO_NUM       10
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       8
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

RTC_DATA_ATTR static int boot_count = 0;
static int last_sent_index = 0; 
static int server_requested_index = -1;
static bool format_requested = false;
static sdmmc_card_t* global_card_handle = NULL; 

// Битовая маска системного лога ошибок (0 — все идеально)
static uint32_t hardware_errors_mask = 0; 


static camera_config_t camera_config = {
    .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = XCLK_GPIO_NUM,
    .pin_sccb_sda = SIOD_GPIO_NUM, .pin_sccb_scl = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM, .pin_d6 = Y8_GPIO_NUM, .pin_d5 = Y7_GPIO_NUM, .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM, .pin_d2 = Y4_GPIO_NUM, .pin_d1 = Y3_GPIO_NUM, .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM, .pin_href = HREF_GPIO_NUM, .pin_pclk = PCLK_GPIO_NUM,
    .xclk_freq_hz = 24000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_YUV422, .frame_size = FRAMESIZE_XGA,
    .jpeg_quality = 12, .fb_count = 1, .grab_mode = CAMERA_GRAB_WHEN_EMPTY, .fb_location = CAMERA_FB_IN_PSRAM    
};

static esp_err_t init_sd_card(sdmmc_card_t** out_card) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = { .format_if_mount_failed = false, .max_files = 2, .allocation_unit_size = 16 * 1024 };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT; host.slot = SDMMC_HOST_SLOT_1;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; slot_config.clk = GPIO_NUM_39; slot_config.cmd = GPIO_NUM_38; slot_config.d0  = GPIO_NUM_40;
    return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, out_card);
}

uint32_t read_battery_millivolts(void) {
    int adc_raw = 0;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&init_config, &adc1_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t config = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
        adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config);
        adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &adc_raw);
        adc_oneshot_del_unit(adc1_handle);
    }
    uint32_t mv = ((uint32_t)adc_raw * 2600 * 2) / 4095;
    return (mv < 2000) ? 0 : mv;
}

uint32_t get_sd_free_space_mb(void) {
    FATFS *fs; DWORD fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) != FR_OK) return 0;
    return (fre_clust * fs->csize) / 2048;
}

void format_sd_card(void) {
    ESP_LOGW(TAG, "Низкоуровневая очистка SD...");
    MKFS_PARM format_opt = { .fmt = FM_ANY, .au_size = 0, .align = 0, .n_fat = 2, .n_root = 512 };
    if (f_mkfs("0:", &format_opt, NULL, 1024) == FR_OK) {
        boot_count = 0; last_sent_index = 0;
    }
}

camera_fb_t* take_photo(void) 
{
    // Если инициализация железного драйвера камеры провалилась - фиксируем ошибку 0x01
    if (esp_camera_init(&camera_config) != ESP_OK) {
        hardware_errors_mask |= 0x01;
        return NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(1500)); 
    for (int i = 0; i < 2; i++) {
        camera_fb_t *fb_flush = esp_camera_fb_get();
        if (fb_flush) esp_camera_fb_return(fb_flush);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    camera_fb_t *fb_real = esp_camera_fb_get();
    
    // Если кадр пустой или NULL - фиксируем ошибку 0x02
    if (!fb_real || fb_real->len == 0) {
        hardware_errors_mask |= 0x02;
    }
    return fb_real;
}

bool save_photo_to_sd(camera_fb_t *fb, int index) {
    if (!fb || fb->len == 0) return false;
    char file_path[32];
    snprintf(file_path, sizeof(file_path), "%s/%05d.raw", MOUNT_POINT, index);
    FILE *f = fopen(file_path, "wb");
    if (f == NULL) return false;
    fwrite(fb->buf, 1, fb->len, f);
    fclose(f);
    return true;
}

// --- БЛОК 2: СЕТЕВОЙ СТЭК ---

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
}

bool wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(8000));
    return (bits & WIFI_CONNECTED_BIT) ? true : false;
}

int create_connected_socket(void) {
    struct sockaddr_in dest_addr = { .sin_addr.s_addr = inet_addr(SERVER_IP), .sin_family = AF_INET, .sin_port = htons(SERVER_PORT) };
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return -1;
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) { close(sock); return -1; }
    return sock;
}

// --- БЛОК 3: РАЗДЕЛЬНЫЙ СЕТЕВОЙ ОБМЕН С МАСКОЙ ОШИБОК ---

// Раздельная функция 1: Отправка информации (Вшиваем маску аппаратных ошибок hardware_errors_mask)
bool send_info(uint32_t index) {
    int sock = create_connected_socket();
    if (sock < 0) return false;

    uint32_t bat_mv = read_battery_millivolts();
    uint32_t free_mb = global_card_handle ? get_sd_free_space_mb() : 0;

    // Пункт №3: Передаем hardware_errors_mask вместо индекса кадра в холостом пинге!
    uint32_t header[4] = { hardware_errors_mask, 0, bat_mv, free_mb };
    if (send(sock, header, sizeof(header), 0) < 0) { close(sock); return false; }

    char rx_buf[32] = {0};
    int rx_len = recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
    if (rx_len > 0) {
        if (strncmp(rx_buf, "OTA:", 4) == 0) {
            size_t ota_size = 0;
            if (sscanf(rx_buf, "OTA:%d", &ota_size) == 1 && ota_size > 0) {
                
                // Увеличиваем таймаут текущего сокета до 30 секунд для приема тяжелого файла
                struct timeval ota_timeout = { .tv_sec = 30, .tv_usec = 0 };
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &ota_timeout, sizeof(ota_timeout));
                
                ESP_LOGW(TAG, "Команда ОТА принята. Отправка READY в текущий сокет...");

                // Шлем READY прямо в этот же сокет
                send(sock, "READY", 5, 0);
                vTaskDelay(pdMS_TO_TICKS(500)); 
                
                // Запускаем прошивку, передавая ей НАШ ТЕКУЩИЙ АКТИВНЫЙ СОКЕТ sock!
                if (perform_tcp_ota(sock, ota_size)) { 
                    close(sock); 
                    vTaskDelay(pdMS_TO_TICKS(500)); 
                    esp_restart(); 
                }
            }
        } else if (strncmp(rx_buf, "FORMAT_SD", 9) == 0) {
            format_requested = true;
        } else if (rx_len == 4) {
            int32_t rx_index = -1; memcpy(&rx_index, rx_buf, 4); server_requested_index = rx_index;
        }
    }
    close(sock);
    return true;
}

bool send_file(uint8_t *buf, size_t len, uint32_t index) {
    int sock = create_connected_socket();
    if (sock < 0) return false;

    uint32_t header[4] = { index, (uint32_t)len, 0, 0 };
    if (send(sock, header, sizeof(header), 0) < 0) { close(sock); return false; }

    size_t total_sent = 0;
    while (total_sent < len) {
        size_t to_send = (len - total_sent > 4096) ? 4096 : (len - total_sent);
        int sent = send(sock, buf + total_sent, to_send, 0);
        if (sent < 0) { close(sock); return false; }
        total_sent += sent;
    }
    int32_t ack_idx = -1; recv(sock, &ack_idx, sizeof(ack_idx), 0);
    close(sock);
    return (ack_idx == (int32_t)index) ? true : false;
}

// Функция динамического поиска последнего индекса на SD-карте
int get_last_file_index_from_sd(void) {
    sdmmc_card_t* card;
    if (init_sd_card(&card) != ESP_OK) return 0;
    
    int index = 1;
    char path[64];
    struct stat st;
    
    // Перебираем имена, пока stat находит файл
    while (index < 99999) {
        snprintf(path, sizeof(path), "%s/plant_%d.raw", MOUNT_POINT, index);
        if (stat(path, &st) != 0) {
            break; // Файл не найден, значит предыдущий индекс был последним
        }
        index++;
    }
    
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    return index - 1;
}

// --- БЛОК 4: ИДЕАЛЬНЫЙ СУПЕР-ЛИНЕЙНЫЙ КОНВЕЙЕР ---
void app_main(void) {
    uint64_t session_start_us = esp_timer_get_time();
    hardware_errors_mask = 0; 
    uint8_t *backup_frame_buffer = NULL;
    size_t backup_frame_length = 0;

    gpio_config_t io_conf = { .pin_bit_mask = (1ULL << 48), .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 1 };
    gpio_config(&io_conf); gpio_set_level(48, 0);

    nvs_flash_init();

    // 1. Снимаем плановый кадр во временный буфер DMA камеры
    camera_fb_t *fb = take_photo();
    
    if (fb && fb->len > 0) {
        // === ПУНКТ №1 ЭТАЛОННОЕ РЕШЕНИЕ: КЛОНИРУЕМ БУФЕР В СВОБОДНУЮ PSRAM ===
        // heap_caps_malloc с флагом MALLOC_CAP_SPIRAM гарантированно выделит память во внешних 8 МБ чипа
        backup_frame_buffer = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
        if (backup_frame_buffer != NULL) {
            backup_frame_length = fb->len;
            // Копируем сырые байты YUV422 из памяти камеры в нашу независимую копию
            memcpy(backup_frame_buffer, fb->buf, fb->len);
            ESP_LOGI(TAG, "[+] Успешный клон кадра в PSRAM: %d байт.", backup_frame_length);
        } else {
            hardware_errors_mask |= 0x02; // Сбой памяти под клон
            ESP_LOGE(TAG, "Не удалось выделить PSRAM под копию кадра!");
        }
        
        // Камеру возвращаем системе МГНОВЕННО
        esp_camera_fb_return(fb); 
    }
    
    // ПРИНУДИТЕЛЬНО ТУШИМ КАМЕРУ! Она проработала всего 2 секунды. 
    // Линия 3.3 В разгружена, никакого нагрева и риска сжечь новую SD-карту в процессе Wi-Fi сессии!
    esp_camera_deinit(); 
    ESP_LOGI(TAG, "[+] Камера полностью обесточена. Переходим к сетевым задачам.");

    // 2. Инициализируем SD-карту один раз
    bool sd_ok = (init_sd_card(&global_card_handle) == ESP_OK);
    if (sd_ok) {
        boot_count = get_last_file_index_from_sd() + 1;
        
        // Пишем на карту, только если кадр не пустой (маска ошибок не содержит 0x02)
        if (!(hardware_errors_mask & 0x02) && backup_frame_buffer && backup_frame_length > 0) {
            camera_fb_t dummy_fb = { .buf = backup_frame_buffer, .len = backup_frame_length };
            if (!save_photo_to_sd(&dummy_fb, boot_count)) {
                hardware_errors_mask |= 0x04;
            }
        }
    } else {
        hardware_errors_mask |= 0x04; 
        boot_count++;
    }

    // 3. Сетевой блок (Запускается в любом случае, даже если камера сдохла — чтобы передать лог!)
    if (wifi_init_sta()) {
        ESP_LOGI(TAG, "Wi-Fi ОК. Передача лога ошибок: 0x%X", hardware_errors_mask);

        // Сетевой диалог А: Холостой пинг синхронизации
        server_requested_index = -1;
        send_info(boot_count);

        if (server_requested_index != -1) {
            last_sent_index = server_requested_index;
        }

        // ЗАЩИТА: Если камера или флешка выдали критическую аварию, 
        // не мучаем систему отправкой очереди, а сразу завершаем сессию
        if (!(hardware_errors_mask & 0x03)) { 
            
            // Сетевой диалог Б: Потоковая выгрузка архива истории
            for (int i = last_sent_index + 1; i <= boot_count; i++) {
                uint64_t total_elapsed_sec = (esp_timer_get_time() - session_start_us) / 1000000ULL;
                if (total_elapsed_sec >= (TARGET_PERIOD_SEC - 20)) { 
                    ESP_LOGE(TAG, "Динамический таймер прервал очередь сессии.");
                    break; 
                }

                uint8_t *file_buf = NULL; size_t file_size = 0;

                if (sd_ok && !(hardware_errors_mask & 0x04)) {
                    char file_path[32]; struct stat st;
                    snprintf(file_path, sizeof(file_path), "%s/%05d.raw", MOUNT_POINT, i);
                    if (stat(file_path, &st) == 0) {
                        file_size = st.st_size;
                        file_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                        if (file_buf) {
                            FILE *f = fopen(file_path, "rb");
                            if (f) { fread(file_buf, 1, file_size, f); fclose(f); }
                        }
                    }
                }

                if (file_buf && file_size > 0) {
                    if (!send_file(file_buf, file_size, i)) { heap_caps_free(file_buf); break; }
                    last_sent_index = i;
                    heap_caps_free(file_buf);
                    vTaskDelay(pdMS_TO_TICKS(15));
                } else if (i == boot_count && backup_frame_buffer && backup_frame_length > 0) {
                    if (send_file(backup_frame_buffer, backup_frame_length, i)) { last_sent_index = i; }
                }
            }
        }
    }

    // Освобождаем буфер клона в самом конце
    if (backup_frame_buffer != NULL) { heap_caps_free(backup_frame_buffer); }

    // --- ФИНАЛЬНЫЙ СИНХРОННЫЙ УХОД В СОН (БЕЗ МЕТОК) ---
    esp_wifi_stop();

    if (sd_ok && global_card_handle) {
        if (format_requested) { 
            format_requested = false; 
            format_sd_card(); 
        } else { 
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, global_card_handle); 
        }
    }

    gpio_hold_en(GPIO_NUM_48); 
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    int elapsed_sec = (int)((esp_timer_get_time() - session_start_us) / 1000000ULL);
    int sleep_time_sec = TARGET_PERIOD_SEC - elapsed_sec;
    if (sleep_time_sec < 15) sleep_time_sec = 15;

    ESP_LOGW(TAG, "Ухожу в глубокий сон на %d сек.", sleep_time_sec);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_time_sec * 1000000LL);
    esp_deep_sleep_start();
}
    
