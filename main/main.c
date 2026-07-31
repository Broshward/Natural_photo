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
#include <sys/time.h>

// Железо и ФС
#include "esp_camera.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "ff.h"
#include "driver/sdspi_host.h" // ВКЛЮЧАЕМ ДРАЙВЕР SPI ДЛЯ КАРТЫ ПАМЯТИ
#include "driver/spi_common.h"
#include "driver/rtc_io.h"

// Сеть и OTA
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "ota_update.h"

#include "adc.h"

static const char *TAG = "greenhouse_cam";

#define WIFI_SSID           "SamstillingHeimar"
#define WIFI_PASS           "HarmoniesWorlds"
#define SERVER_IP           "192.168.43.1" 
#define SERVER_PORT         8888

#define TARGET_PERIOD_SEC   600
#define MOUNT_POINT         "/sdcard"
#define FILE_PATTERN		"%s/photos/%05d.raw"

#define DARK_THRESHOLD 70  // Порог темноты (0 - глубокая ночь, 255 - белый лист)
                           // Экспериментально для теплицы обычно подходит от 30 до 45


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
RTC_DATA_ATTR static int mode = 0;
RTC_DATA_ATTR static uint64_t time_to_sleep_enter = 0;
RTC_DATA_ATTR static int rtc_saved_file_index = 0; // Текущий индекс файла

static camera_config_t camera_config = {
    .pin_pwdn = -1, 
	.pin_reset = -1,
	.pin_xclk = XCLK_GPIO_NUM,
    .pin_sccb_sda = SIOD_GPIO_NUM,
	.pin_sccb_scl = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM,
	.pin_d6 = Y8_GPIO_NUM,
	.pin_d5 = Y7_GPIO_NUM,
	.pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
	.pin_d2 = Y4_GPIO_NUM,
	.pin_d1 = Y3_GPIO_NUM,
	.pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM, 
	.pin_href = HREF_GPIO_NUM,
	.pin_pclk = PCLK_GPIO_NUM,
    .xclk_freq_hz = 24000000,
	.ledc_timer = LEDC_TIMER_0,
	.ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_YUV422,
	.frame_size = FRAMESIZE_SXGA,
    .jpeg_quality = 12,
	.fb_count = 1,
	.grab_mode = CAMERA_GRAB_WHEN_EMPTY,
	.fb_location = CAMERA_FB_IN_PSRAM    
};

static esp_err_t init_sd_card(sdmmc_card_t** out_card) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = { .format_if_mount_failed = false, .max_files = 2, .allocation_unit_size = 16 * 1024 };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT; host.slot = SDMMC_HOST_SLOT_1;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; slot_config.clk = GPIO_NUM_39; slot_config.cmd = GPIO_NUM_38; slot_config.d0  = GPIO_NUM_40;
    return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, out_card);
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

// Вспомогательная функция прямой записи в регистр OV3660 через встроенное SCCB API библиотеки
static int write_sensor_reg(uint16_t reg, uint8_t val) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return -1;
    // Вызываем скрытый метод прямой записи в I2C шину датчика
    return s->set_reg(s, reg, 0xFF, val); 
}
static uint8_t read_sensor_reg(uint16_t reg) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE("sensor_debug", "Датчик камеры не инициализирован!");
        return 0;
    }
    // Вызываем скрытый метод чтения: передаем указатель на сенсор, адрес регистра и маску битов (0xFF)
    return s->get_reg(s, reg, 0xFF);
}

void shutdown_greenhouse_camera(void) {
    ESP_LOGW("main_cam", "[!] Отправляем камеру в программный PowerDown...");
    
    // Включаем бит 6 в регистре 0x3008, полностью обесточивая матрицу и объектив!
    write_sensor_reg(0x3008, 0x40); 
}

camera_fb_t* take_photo(void) {
    if (esp_camera_init(&camera_config) != ESP_OK) {
        hardware_errors_mask |= 0x01;
        return NULL;
    }
    
//	ESP_LOGI(TAG, "Temperature of camera %u", read_sensor_reg(0x6719));
//	ESP_LOGI(TAG, "Compression enable %u", read_sensor_reg(0x3821));
//	ESP_LOGI(TAG, "Compression MODE %u", read_sensor_reg(0x4713));
//	ESP_LOGI(TAG, "SCCB_ID %u", read_sensor_reg(0x4713));
//
//    vTaskDelay(pdMS_TO_TICKS(200)); // Даем стабилизироваться кадрам

    // Прогреваем матрицу
    for (int i = 0; i < 2; i++) {
        camera_fb_t *fb_flush = esp_camera_fb_get();
        if (fb_flush) esp_camera_fb_return(fb_flush);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    camera_fb_t *fb_real = esp_camera_fb_get();
    
    if (fb_real) {
        ESP_LOGW("main_cam", "[SUCCESS] Кадр успешно захвачен! Вес файла: %d байт.", fb_real->len);
    } else {
        hardware_errors_mask |= 0x02;
    }
    
    return fb_real;    
}

bool save_photo_to_sd(camera_fb_t *fb, int index) 
{
    // Принудительно создаем папку. Если она уже есть, операционная система просто пропустит этот шаг
    mkdir("/sdcard/photos", 0755); 

    char file_path[64];
    snprintf(file_path, sizeof(file_path), FILE_PATTERN, MOUNT_POINT, index);
    
    ESP_LOGI("SD_WRITE", "Запись файла: %s", file_path);
    FILE *f = fopen(file_path, "wb");
    if (f == NULL) {
        ESP_LOGE("SD_WRITE", "[-] Ошибка создания файла! Проверьте формат карты.");
        return false;
    }
    
    size_t written = fwrite(fb->buf, 1, fb->len, f);
    fclose(f);
    
    return (written == fb->len);
}

// --- БЛОК 2: СЕТЕВОЙ СТЭК ---

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) 
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
}

bool wifi_init_sta(void) 
{
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

int create_connected_socket(void) 
{
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
bool send_info() 
{
    int sock = create_connected_socket();
    if (sock < 0) return false;

    uint32_t bat_mv = read_battery_millivolts();
    uint32_t free_mb = global_card_handle ? get_sd_free_space_mb() : 0;

    // Пункт №3: Передаем hardware_errors_mask вместо индекса кадра в холостом пинге!
    uint32_t header[5] = { hardware_errors_mask, 0, bat_mv, free_mb, rtc_saved_file_index };
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
			ESP_LOGW(TAG, "FORMAT SD-карты...");
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
    // Если в RTC-памяти уже лежит сохраненный индекс больше нуля,
    // значит мы проснулись по таймеру или кнопке. Просто возвращаем его без сканирования флешки!
    if (rtc_saved_file_index > 0) {
        ESP_LOGI("SD_INDEX", "[RTC-RAM] Индекс успешно взят из памяти процессора: %d", rtc_saved_file_index);
        return rtc_saved_file_index;
    }

    // Если там оказался 0 (самый первый старт системы при подаче питания) — 
    // запускаем ваш проверенный, честный построчный сканер stat, но ищем уже внутри папки photos!
    ESP_LOGW("SD_INDEX", "[!] RTC-RAM пуста. Запускаем однократное аппаратное сканирование папки photos...");
    
    int index = 1;
    char path[64];
    struct stat st;
    
    while (index < 99999) {
        // Ищем файлы строго внутри нашей новой выделенной папки photos
        snprintf(path, sizeof(path), "%s/photos/%05d.raw", MOUNT_POINT, index);
        if (stat(path, &st) != 0) {
            break; // Файл не найден, значит предыдущий индекс был последним
        }
        index++;
    }
    
    // Вычисляем финальный индекс
    int final_index = index - 1;
    
    // Запоминаем его в RTC-RAM, чтобы больше никогда сюда не заходить!
    rtc_saved_file_index = final_index;
    
    ESP_LOGW("SD_INDEX", "[SUCCESS] Сканирование завершено. Последний файл на флешке: %d. Индекс сохранен в RTC!", final_index);
    return final_index;
}

//Функция анализа освещённости кадра
bool is_frame_too_dark(uint8_t *yuv_buf, size_t len) {
    uint64_t total_brightness = 0;
    size_t y_pixel_count = 0;

    // Шаг по буферу: YUV422 хранит байты как Y0, U0, Y1, V0
    // Нам нужен каждый 2-й байт, начиная с индекса 0 (это Y0, Y1, Y2...)
    for (size_t i = 0; i < len; i += 2) {
        total_brightness += yuv_buf[i];
        y_pixel_count++;
    }

    if (y_pixel_count == 0) return true; // На всякий случай

    // Считаем среднюю яркость кадра
    uint8_t average_brightness = (uint8_t)(total_brightness / y_pixel_count);

    // Логируем в консоль для подбора идеального порога
    ESP_LOGI("CAM_BRIGHT", "Средняя яркость кадра: %u (Порог: %d)", average_brightness, DARK_THRESHOLD);

    // Если средняя яркость меньше порога — кадр слишком темный
    if (average_brightness < DARK_THRESHOLD) {
        return true; 
    }

    return false; // Кадр нормальный, можно сохранять
}

// --- БЛОК 4: ИДЕАЛЬНЫЙ СУПЕР-ЛИНЕЙНЫЙ КОНВЕЙЕР ---
void app_main(void) {
    uint64_t session_start_us = esp_timer_get_time();
	ESP_LOGW(TAG, "Begin time %lu", session_start_us);

    hardware_errors_mask = 0; 

    gpio_config_t o_conf = { .pin_bit_mask = (1ULL << 48), .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 1 };
    gpio_config(&o_conf); gpio_set_level(48, 0);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );



	camera_fb_t *fb = NULL;
	bool sd_ok = false;
	uint32_t wakeup_mask = esp_sleep_get_wakeup_causes();
	
	int mode_prev = mode;
	// Mode calculating
	// mode = 0  Photo and save mode. Timer mode. This is ordinary mode. 
	// mode = 1  Photo mode. Button press mode. Photo & download. Этот mode для настройки камеры. Он делает фото и сразу отправляет его на смартфон. Turns on with a short press of the button
	// mode = 2  Hold button mode (Download mode). This mode uses for download files. Turns on with a long  press of the button
	// mode = 3  Greenhouse mode (Photo, saving and download mode). Это режим для теплицы. Кнопка игнорируется. Turns on with uncomment line below 
	ESP_LOGI(TAG, "PIN 0 is %d", gpio_get_level(0));
	if (wakeup_mask & (1 << ESP_SLEEP_WAKEUP_TIMER)) // По таймеру 
		mode = 0; // Timer mode (Photo and save mode). This is ordinary mode.  
	else if (wakeup_mask & (1 << ESP_SLEEP_WAKEUP_EXT0)) {
		if (gpio_get_level(0)) {
			mode = 1; // Button mode (Download mode) This mode uses for download files. 
			boot_count = 1;
		} else {
			mode = 2; // Hold button mode (Setting mode). Этот mode для настройки камеры. Только фото и отправка  
			while (!gpio_get_level(0)) {vTaskDelay(pdMS_TO_TICKS(100));}
		}
	}
	mode = 3; //Greenhouse mode (Photo, saving and download mode). Это режим для теплицы. Кнопка игнорируется. Включается только так.
	ESP_LOGW(TAG, "The mode is %d", mode);

	// 1. Снимаем плановый кадр во временный буфер
	if (mode == 0 || mode == 1 || mode == 3) { 
		fb = take_photo();
		// ПРИНУДИТЕЛЬНО ТУШИМ КАМЕРУ! 
		shutdown_greenhouse_camera();
		ESP_LOGI(TAG, "[+] Камера полностью обесточена. Переходим к сетевым задачам.");
	}

    // 2. Инициализируем SD-карту 
	if (mode==0 || mode == 2 || mode == 3) {
	    esp_err_t sd_status = init_sd_card(&global_card_handle);
		if (sd_status == ESP_OK) {
			ESP_LOGI(TAG, "Карта смонтирована!");
			sd_ok=true;
		} else {
			ESP_LOGE(TAG, "SD-card mount error: %d", sd_status);
			sd_ok = false;
		}
	    if (sd_ok) {
	        boot_count = get_last_file_index_from_sd() + 1;
			ESP_LOGI(TAG, "[+] Last file in the card %d", boot_count-1);
	        
	        // Пишем на карту, только если кадр не пустой и маска ошибок не содержит 0x02 и если кадр не слишком тёмный!!!
	        if (!(hardware_errors_mask & 0x02) && fb && fb->buf && fb->len > 0 && !is_frame_too_dark(fb->buf, fb->len)) {
	            if (save_photo_to_sd(fb, boot_count)) {
					rtc_saved_file_index = boot_count; 
	            } else {
					hardware_errors_mask |= 0x04;
				}
	        }
	    } else {
	        hardware_errors_mask |= 0x04; 
	    }
	}

	if (mode_prev != 0) // Это может быть, если выгрузка файлов прервалась таймером
		mode = mode_prev;
    // 3. Сетевой блок (Запускается только если нажатие на кнопку(не таймер) или теплица(mode=3))
	//if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER) {
	if(mode == 1 || mode == 2 || mode == 3) {
		if (wifi_init_sta()) {
			ESP_LOGI(TAG, "Wi-Fi ОК. Передача лога ошибок: 0x%X", hardware_errors_mask);

			// Сетевой диалог А: Холостой пинг синхронизации
			send_info();

			if (server_requested_index != -1) {
				last_sent_index = server_requested_index;
			}
			if (mode == 1)
				last_sent_index = -1;

			// ЗАЩИТА: Если камера или флешка выдали критическую аварию, 
			// не мучаем систему отправкой очереди, а сразу завершаем сессию
			if (!(hardware_errors_mask & 0x03)) { 
				// Сетевой диалог Б: Потоковая выгрузка архива истории
				bool mode_change = true;
				for (int i = last_sent_index + 1; i <= boot_count; i++) {
					uint64_t total_elapsed_sec = (esp_timer_get_time() - session_start_us) / 1000000ULL;
					if (total_elapsed_sec >= (TARGET_PERIOD_SEC - 20)) { 
						ESP_LOGE(TAG, "Динамический таймер прервал очередь сессии.");
						mode_change = false;
						break; 
					}

					uint8_t *file_buf = NULL; size_t file_size = 0;

					if (sd_ok && !(hardware_errors_mask & 0x04)) {
						char file_path[32]; struct stat st;
						snprintf(file_path, sizeof(file_path), FILE_PATTERN, MOUNT_POINT, i);

						if (stat(file_path, &st) == 0) {
							file_size = st.st_size;
							file_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
							if (file_buf) {
								FILE *f = fopen(file_path, "rb");
								if (f) { fread(file_buf, 1, file_size, f); fclose(f); }
							}
						}
						ESP_LOGI(TAG, "File %s upload", file_path);
					}

					if (file_buf && file_size > 0) {
						if (!send_file(file_buf, file_size, i)) { 
							heap_caps_free(file_buf); 
							break; 
						}
						last_sent_index = i;
						heap_caps_free(file_buf);
						vTaskDelay(pdMS_TO_TICKS(15));
					} else if (i == boot_count && fb && fb->buf && fb->len > 0) {
						if (send_file(fb->buf, fb->len, i)) { last_sent_index = i; }
					}
				}
				if (mode_change) mode = 0;
			}
			esp_wifi_stop();
		}
	}

	//Буфер камеры больше не нужен
	if (mode == 0 || mode == 1 || mode == 3)
	{
		esp_camera_fb_return(fb); 
		esp_camera_deinit(); 
	}	
    // --- ФИНАЛЬНЫЙ СИНХРОННЫЙ УХОД В СОН (БЕЗ МЕТОК) ---

    if (sd_ok && global_card_handle) {
        if (format_requested) { 
            format_requested = false; 
            format_sd_card(); 
			rtc_saved_file_index = 0;
        } else { 
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, global_card_handle); 
        }
    }

    gpio_hold_en(GPIO_NUM_48); 
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // Включаем внутреннюю подтяжку pull-up через RTC-модуль, чтобы она работала во сне
    rtc_gpio_init(0);
    rtc_gpio_set_direction(0, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(0);
    rtc_gpio_pulldown_dis(0);

    int elapsed_sec = (int)((esp_timer_get_time() - session_start_us) / 1000000ULL);
    int sleep_time_sec = TARGET_PERIOD_SEC - elapsed_sec;
    if (sleep_time_sec < 15) sleep_time_sec = 15;

    ESP_LOGW(TAG, "Ухожу в глубокий сон на %d сек.", sleep_time_sec);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_time_sec * 1000000LL);
	if (mode == 0 || mode == 1) esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
}
