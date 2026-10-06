#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include <dirent.h> // Нужен для работы с каталогами
#include <sys/time.h>
#include <time.h>

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include <sys/unistd.h>
#include <sys/stat.h>
#include "driver/rtc_io.h"

// Сеть
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#define MOUNT_POINT "/sdcard"

#include "sdkconfig.h"

#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// support IDF 5.x
#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

#include "esp_camera.h"
#include "ota_update.h"
#include "adc.h"

#if defined(CONFIG_CAMERA_AF_SUPPORT) && CONFIG_CAMERA_AF_SUPPORT
#include "esp_camera_af.h"
#endif

//#define BOARD_WROVER_KIT 1

#include "camera_pinout.h"


#define WIFI_SSID           "SamstillingHeimar"
#define WIFI_PASS           "HarmoniesWorlds"
#define SERVER_IP           "192.168.43.1" 
#define SERVER_PORT         8888

#define TARGET_PERIOD_SEC   600
#define MOUNT_POINT         "/sdcard"
#define FILE_PATTERN		"%s/photos/%04d%02d%02d_%02d%02d%02d_%05d.jpg"

#define DARK_THRESHOLD 70  // Порог темноты (0 - глубокая ночь, 255 - белый лист)
                           // Экспериментально для теплицы обычно подходит от 30 до 45
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1   //software reset will be performed
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D0 11
#define CAM_PIN_D1 9
#define CAM_PIN_D2 8
#define CAM_PIN_D3 10
#define CAM_PIN_D4 12
#define CAM_PIN_D5 18
#define CAM_PIN_D6 17
#define CAM_PIN_D7 16

static const char *TAG = "greenhouse_cam";
static int test_file_index = 1; // Стартовый индекс для тестовых фоток

static EventGroupHandle_t s_wifi_event_group;
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
static uint64_t session_start_us;



#if ESP_CAMERA_SUPPORTED
static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 24000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_5MP,    //QQVGA-UXGA, For ESP32, do not use sizes above QVGA when not JPEG. The performance of the ESP32-S series has improved a lot, but JPEG mode always gives better frame rates.

    .jpeg_quality = 12, //0-63, for OV series camera sensors, lower number means higher quality
    .fb_count = 1,       //When jpeg mode is used, if fb_count more than one, the driver will work in continuous mode.
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

static esp_err_t init_camera(void)
{
    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed");
		hardware_errors_mask |= 0x01;
        return err;
    }

    return ESP_OK;
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

#if defined(CONFIG_CAMERA_AF_SUPPORT) && CONFIG_CAMERA_AF_SUPPORT
static void maybe_init_autofocus(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGW(TAG, "AF: no sensor handle");
        return;
    }

    if (!esp_camera_af_is_supported(s)) {
        ESP_LOGI(TAG, "AF: not supported by this sensor");
        return;
    }

    esp_camera_af_config_t af_cfg = {
        .mode = ESP_CAMERA_AF_MODE_AUTO,
        .timeout_ms = CONFIG_CAMERA_AF_DEFAULT_TIMEOUT_MS,
    };

    esp_err_t ret = esp_camera_af_init(s, &af_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AF init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "AF initialized (AUTO mode)");
}
#endif
#endif

void save_test_frame_to_sd(camera_fb_t *pic) 
{

    // 2. Генерируем имя по твоему новому паттерну (пока без RTC даты, пишем нули)
    char file_path[64];
    snprintf(file_path, sizeof(file_path), "/sdcard/photos/20260925_000000_%05d.jpg", test_file_index);

    // Принудительно создаем папку photos
    mkdir("/sdcard/photos", 0755);

    ESP_LOGW(TAG, "[*] Запись кадра на флешку: %s ...", file_path);
    FILE *f = fopen(file_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "[-] Не удалось открыть файл для записи!");
    } else {
        size_t written = fwrite(pic->buf, 1, pic->len, f);
        fclose(f);
        
        if (written == pic->len) {
            ESP_LOGW(TAG, "[SUCCESS] Файл успешно сохранен! %zu байт.", written);
            test_file_index++; // Инкрементируем индекс только при успешной записи!
        } else {
            ESP_LOGE(TAG, "[-] Ошибка: записано только %zu байт из %zu", written, pic->len);
        }
    }
}

static int write_sensor_reg(uint16_t reg, uint8_t val) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return -1;
    // Вызываем скрытый метод прямой записи в I2C шину датчика
    return s->set_reg(s, reg, 0xFF, val);
}

void shutdown_greenhouse_camera(void) 
{
    ESP_LOGW("main_cam", "[!] Отправляем камеру в программный PowerDown...");
    
    // Включаем бит 6 в регистре 0x3008, полностью обесточивая матрицу и объектив!
    write_sensor_reg(0x3008, 0x40); 
}

static esp_err_t init_sd_card(sdmmc_card_t** out_card) 
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = { .format_if_mount_failed = false, .max_files = 2, .allocation_unit_size = 16 * 1024 };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT; host.slot = SDMMC_HOST_SLOT_1;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; slot_config.clk = GPIO_NUM_39; slot_config.cmd = GPIO_NUM_38; slot_config.d0  = GPIO_NUM_40;
    return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, out_card);
}

uint32_t get_sd_free_space_mb(void) 
{
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

bool find_file_by_index(int index, char *out_path, size_t max_len) 
{
    DIR *dir = opendir("/sdcard/photos");
    if (!dir) {
        ESP_LOGE("SD_READ", "[-] Не удалось открыть каталог /photos");
        return false;
    }

    struct dirent *entry;
    char suffix[16];
    // Формируем уникальный хвост файла, например: "_00421.raw"
    snprintf(suffix, sizeof(suffix), "_%05d.raw", index);

    bool found = false;

    // Сканируем файлы в папке
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        size_t suffix_len = strlen(suffix);

        // Если имя файла длиннее суффикса, проверяем совпадение с конца строки
        if (name_len >= suffix_len) {
            const char *end_of_name = entry->d_name + (name_len - suffix_len);
            if (strcmp(end_of_name, suffix) == 0) {
                // Файл найден! Записываем полный путь в буфер ответа
                snprintf(out_path, max_len, "/sdcard/photos/%s", entry->d_name);
                found = true;
                break;
            }
        }
    }
    closedir(dir);

    if (found) {
        ESP_LOGI("SD_READ", "[+] Файл для индекса %d успешно найден: %s", index, out_path);
    } else {
        ESP_LOGW("SD_READ", "[-] Файл с индексом %d (_%05d.raw) не найден на карте", index, index);
    }

    return found;
}

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

bool send_file(uint8_t *buf, size_t len, uint32_t index) 
{
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


camera_fb_t *take_photo(void)
{
#if defined(CONFIG_CAMERA_AF_SUPPORT) && CONFIG_CAMERA_AF_SUPPORT
    // Initialize autofocus if configured and supported by the sensor.
    // In menuconfig: Component config → Camera configuration → Enable autofocus support
    maybe_init_autofocus();
#endif
    // === ШАГ 1: ИНИЦИАЛИЗАЦИЯ КАМЕРЫ НА КРУГЕ ЦИКЛА ===
    ESP_LOGI(TAG, "Инициализация камеры...");
    // Вызываем родную функцию примера, которая настраивает камеру
    if (init_camera() != ESP_OK) {
        ESP_LOGE(TAG, "[-] Сбой инициализации камеры на этом круге! Пробуем через 5 секунд...");
        return NULL;
    }

    // Загрубляем качество (как мы выяснили, это убирает NO-EOI таймауты)
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_quality(s, 5); 
        vTaskDelay(pdMS_TO_TICKS(50));
    }

	ESP_LOGW(TAG, "[*] Запуск безопасного цикла захвата кадра...");
	int attempt = 0;
	uint64_t total_elapsed_sec = (esp_timer_get_time() - session_start_us) / 1000000ULL;
	while (total_elapsed_sec <= (TARGET_PERIOD_SEC - 20)) {
		attempt++;
		// === ШАГ 2: ЗАХВАТ КАДРА ===
		ESP_LOGI(TAG, "Taking picture...");
		camera_fb_t *pic = esp_camera_fb_get();

		if (pic == NULL) {
			ESP_LOGE(TAG, "[-] Ошибка: Кадр не получен (Timeout/NO-EOI).");
		} else {
			ESP_LOGI(TAG, "[SUCCESS] Picture taken! Its size was: %zu bytes", pic->len);
			
			// ПРИНУДИТЕЛЬНО ТУШИМ КАМЕРУ ПОСЛЕ ЦИКЛА
			shutdown_greenhouse_camera();
			ESP_LOGI(TAG, "[+] Камера полностью обесточена. Переходим к сетевым задачам.");

			ESP_LOGW(TAG, "[SUCCESS] Валидный кадр захвачен на попытке №%d! Размер: %d байт", attempt, pic->len);
			return pic;
		}

		ESP_LOGI(TAG, "--------------------------------------------------");
		total_elapsed_sec = (esp_timer_get_time() - session_start_us) / 1000000ULL;
	}
	return NULL;
}

int get_last_file_index_from_sd(void) 
{
    if (rtc_saved_file_index > 0) {
        ESP_LOGI("SD_INDEX", "[RTC-RAM] Индекс успешно взят из памяти процессора: %d", rtc_saved_file_index);
        return rtc_saved_file_index;
    }

    ESP_LOGW("SD_INDEX", "[!] RTC-RAM пуста. Сканируем папку photos на максимальный индекс...");
    
    DIR *dir = opendir("/sdcard/photos");
    int max_index = 0;

    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            // Ищем файлы, заканчивающиеся на ".raw"
            if (strstr(entry->d_name, ".raw") != NULL) {
                int file_idx = 0;
                // Парсим индекс из конца имени файла. Наш формат: YYYYMMDD_HHMMSS_INDEX.raw
                // Сканируем последние элементы перед точкой
                char *underscore = strrchr(entry->d_name, '_');
                if (underscore && sscanf(underscore + 1, "%d.raw", &file_idx) == 1) {
                    if (file_idx > max_index) {
                        max_index = file_idx;
                    }
                }
            }
        }
        closedir(dir);
    }
    
    rtc_saved_file_index = max_index;
    ESP_LOGW("SD_INDEX", "[SUCCESS] Последний индекс на флешке: %d. Сохранено в RTC!", max_index);
    return max_index;
}

bool is_frame_too_dark(uint8_t *yuv_buf, size_t len) 
{
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

bool save_photo_to_sd(camera_fb_t *fb, int index) 
{
    // Принудительно создаем папку. Если она есть, шаг просто пропустится
    mkdir("/sdcard/photos", 0755); 

    // 1. Получаем текущее время из встроенного RTC-счетчика ESP32-S3
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char file_path[64];
    // 2. Собираем имя по новому паттерну: путь, дата, время, индекс
    snprintf(file_path, sizeof(file_path), FILE_PATTERN, 
             MOUNT_POINT,
             (timeinfo.tm_year + 1900), 
             (timeinfo.tm_mon + 1), 
             timeinfo.tm_mday,
             timeinfo.tm_hour, 
             timeinfo.tm_min, 
             timeinfo.tm_sec,
             index);
    
    ESP_LOGI("SD_WRITE", "Запись кадра: %s", file_path);
    FILE *f = fopen(file_path, "wb");
    if (f == NULL) {
        ESP_LOGE("SD_WRITE", "[-] Ошибка создания файла! Проверьте формат карты.");
        return false;
    }
    
    size_t written = fwrite(fb->buf, 1, fb->len, f);
    fclose(f);
    
    return (written == fb->len);
}

void app_main(void) 
{
    session_start_us = esp_timer_get_time();
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



	uint32_t wakeup_mask = esp_sleep_get_wakeup_causes();
	
	int mode_prev = mode;
	// Mode calculating
	// mode = 0  Photo and save mode. Timer mode. This is ordinary mode. 
	// mode = 1  Photo mode. Button press mode. Photo & download. Этот mode для настройки камеры. Он делает фото и сразу отправляет его на смартфон. Turns on with a short press of the button
	// mode = 2  Hold button mode (Download mode). This mode uses for download files. Turns on with a long  press of the button
	//mode = 3  Greenhouse mode (Photo, saving and download mode). Это режим для теплицы. Кнопка игнорируется. Turns on with uncomment line below 
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
	camera_fb_t *fb = NULL;
	if (mode == 0 || mode == 1 || mode == 3) { 
//        int max_attempts = 15; // Даем камере до 15 попыток на автоэкспозицию

        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            s->set_quality(s, 50); // Понижаем качество (12-20), файлы станут меньше, таймауты исчезнут
            vTaskDelay(pdMS_TO_TICKS(50));
        }

		fb = take_photo(); 
    }

    // 2. Инициализируем SD-карту 
	bool sd_ok = false;
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
					ESP_LOGI(TAG, "Фото записано на сд-карту");
					rtc_saved_file_index = boot_count; 
	            } else {
					ESP_LOGE(TAG, "Ошибка записи на сд-карту");
					hardware_errors_mask |= 0x04;
				}
	        }
			else {
				ESP_LOGE(TAG, "Error mask: %x", hardware_errors_mask);
				ESP_LOGE(TAG, "fb = 0x%x", fb);
				if (fb) ESP_LOGE(TAG, "fb->buf = 0x%x", fb->buf);
				if (fb) ESP_LOGE(TAG, "fb->len = %d", fb->len);

			}
	    } else {
	        hardware_errors_mask |= 0x04; 
	    }
	}

	if (mode_prev != 0) // Это может быть, если выгрузка файлов прервалась таймером
		mode = mode_prev;
    // 3. Сетевой блок (Запускается только если нажатие на кнопку(не таймер) или теплица(mode=3))
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
                        // Увеличили размер буфера до 64 байт, так как имя файла стало длиннее!
                        char file_path[64]; 
                        struct stat st;

                        // ИСПОЛЬЗУЕМ НАШУ НОВУЮ ФУНКЦИЮ ПОИСКА:
                        // Она сканирует папку и сама запишет в file_path точный путь к файлу для индекса "i"
                        if (find_file_by_index(i, file_path, sizeof(file_path))) {
                            
                            // Если файл успешно найден на карте, проверяем его размер через stat
                            if (stat(file_path, &st) == 0) {
                                file_size = st.st_size;
                                file_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                                if (file_buf) {
                                    FILE *f = fopen(file_path, "rb");
                                    if (f) { 
                                        fread(file_buf, 1, file_size, f); 
                                        fclose(f); 
                                    }
                                }
                            }
                            ESP_LOGI(TAG, "File %s upload", file_path);
                        } else {
                            // Если файл не найден по индексу, логируем предупреждение, 
                            // протокол не упадет, плата просто продолжит работу
                            ESP_LOGW(TAG, "File with index %d not found on SD card", i);
                        }
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
	
    if (sd_ok && global_card_handle) {
        if (format_requested) { 
            format_requested = false; 
            format_sd_card(); 
			rtc_saved_file_index = 0;
        } else { 
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, global_card_handle); 
        }
    }

    // --- ФИНАЛЬНЫЙ СИНХРОННЫЙ УХОД В СОН (БЕЗ МЕТОК) ---
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
