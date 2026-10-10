#include <math.h>

#include "jpeg_decoder.h"
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

static char g_dir_path[64]; // Сюда автоматически запишется "/sdcard/photos"
static char g_file_ext[8];  // Сюда автоматически запишется ".jpg" (или ".raw")


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

void init_file_system_paths(void) {
    char dummy_path[96];
    // Генерируем тестовый путь, чтобы вытащить структуру имени файла
    snprintf(dummy_path, sizeof(dummy_path), FILE_PATTERN, MOUNT_POINT, 2026, 1, 1, 0, 0, 0, 1);
    
    // 1. Вытаскиваем путь к папке
    char *last_slash = strrchr(dummy_path, '/');
    if (last_slash) {
        *last_slash = '\0'; // Временно обрезаем строку по последний слэш
        strcpy(g_dir_path, dummy_path);
        *last_slash = '/';  // Восстанавливаем строку
    }
    
    // 2. Вытаскиваем расширение (.jpg или .raw)
    char *last_dot = strrchr(dummy_path, '.');
    if (last_dot) {
        strcpy(g_file_ext, last_dot);
    }

    ESP_LOGW("FS_INIT", "[+] Пути успешно настроены! Папка: %s, Расширение: %s", g_dir_path, g_file_ext);
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

// Функция чтения 8-битного регистра камеры через шину SCCB (I2C)
uint8_t read_sensor_reg(uint16_t reg) {
    // Получаем указатель на внутренний SCCB-драйвер esp-camera
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return 0;
    
    // В зависимости от версии esp-camera, у сенсора есть встроенный метод чтения:
    // Если его нет, используется прямая функция из sccb.h ядра драйвера
    return s->get_reg(s, reg, 0xFF); 
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
    DIR *dir = opendir(g_dir_path);
    if (!dir) {
        ESP_LOGE("SD_READ", "[-] Не удалось открыть каталог: %s", g_dir_path);
        return false;
    }

    // Вытаскиваем чистый формат имени файла из FILE_PATTERN (всё, что идет после папки)
    // Из "%s/photos/%04d%02d%02d_%02d%02d%02d_%05d.jpg" 
    // мы получим строку: "%04d%02d%02d_%02d%02d%02d_%05d.jpg"
    const char *pattern_filename_part = strrchr(FILE_PATTERN, '/') + 1;

    struct dirent *entry;
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        int y, m, d, hr, min, sec, file_idx;
        
        // Пытаемся применить оригинальную маску имени файла К КАЖДОМУ файлу на флешке!
        // sscanf вернет количество успешно заполненных переменных (у нас их 7 штук)
        if (sscanf(entry->d_name, pattern_filename_part, &y, &m, &d, &hr, &min, &sec, &file_idx) == 7) {
            
            // Если маска совпала и индекс внутри файла равен искомому — файл НАЙДЕН!
            if (file_idx == index) {
                snprintf(out_path, max_len, "%s/%s", g_dir_path, entry->d_name);
                found = true;
                break;
            }
        }
    }
    closedir(dir);

    if (found) {
        ESP_LOGI("SD_READ", "[+] Файл для индекса %d успешно найден: %s", index, out_path);
    } else {
        ESP_LOGW("SD_READ", "[-] Файл с индексом %d не найден на карте памяти", index);
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

    ESP_LOGW("SD_INDEX", "[!] RTC-RAM пуста. Сканируем папку %s на максимальный индекс...", g_dir_path);
    
    DIR *dir = opendir(g_dir_path); // Используем динамический путь!
    int max_index = 0;

    if (dir) {
        struct dirent *entry;
        char scan_pattern[16];
        // Собираем паттерн для sscanf динамически на основе расширения: "%%d%s" -> "%d.jpg"
        snprintf(scan_pattern, sizeof(scan_pattern), "%%d%s", g_file_ext);

        while ((entry = readdir(dir)) != NULL) {
            // Проверяем, что файл заканчивается на наше правильное расширение (.jpg / .raw)
            if (strstr(entry->d_name, g_file_ext) != NULL) {
                int file_idx = 0;
                // Ищем последний символ '_' в имени файла
                char *underscore = strrchr(entry->d_name, '_');
                // Считываем индекс с учетом динамического расширения
                if (underscore && sscanf(underscore + 1, scan_pattern, &file_idx) == 1) {
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

bool is_jpeg_frame_too_dark(uint8_t *jpeg_buf, size_t jpeg_len)
{
    if (jpeg_buf == NULL || jpeg_len == 0) {
        ESP_LOGE(TAG, "Неверные входные данные буфера");
        return true; 
    }

    // 1. Создаем минимальную конфигурацию, чтобы скормить её парсеру заголовков
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = jpeg_buf,
        .indata_size = jpeg_len,
        .out_format = JPEG_IMAGE_FORMAT_RGB888, 
        .out_scale = JPEG_IMAGE_SCALE_1_8, // Мы хотим получить размеры с учетом сжатия 1/8
    };

    esp_jpeg_image_output_t outimg = {0};

        // 2. Вызываем esp_jpeg_get_image_info для чтения оригинальной геометрии кадра
    if (esp_jpeg_get_image_info(&jpeg_cfg, &outimg) != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось получить информацию о кадре");
        return true;
    }

    // КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Функция вернула оригинальный размер (например, 2592х1944).
    // Так как мы декодируем в масштабе 1/8, уменьшаем размеры вручную для выделения буфера!
    uint32_t out_width = outimg.width / 8;
    uint32_t out_height = outimg.height / 8;
    
    // Округляем до кратного 8 (требование блочного декодера JPEG для корректных границ)
    out_width = (out_width + 7) & ~7;
    out_height = (out_height + 7) & ~7;

    size_t out_buf_size = out_width * out_height * 3; // Для 5МП это будет ~235 КБ вместо 14.7 МБ!

    // 3. Выделяем оперативную память под РЕАЛЬНЫЙ уменьшенный кадр
    uint8_t *out_img_buf = malloc(out_buf_size);
    if (out_img_buf == NULL) {
        ESP_LOGE(TAG, "Не удалось выделить RAM для уменьшенного кадра (%d байт)", out_buf_size);
        return true;
    }

    // Обновляем структуру конфигурации правильным буфером
    jpeg_cfg.outbuf = out_img_buf;
    jpeg_cfg.outbuf_size = out_buf_size;

    // 4. Запускаем полноценное декодирование
    esp_err_t err = esp_jpeg_decode(&jpeg_cfg, &outimg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка декодирования esp_jpeg_decode: %d", err);
        free(out_img_buf);
        return true;
    }

    // 5. Вычисляем среднюю яркость по декодированному кадру
        uint64_t total_brightness = 0;
    uint32_t total_pixels = outimg.width * outimg.height;
    uint8_t max_brightness = 0; // Переменная для поиска самого яркого пикселя

    for (uint32_t i = 0; i < total_pixels; i++) {
        uint8_t r = out_img_buf[i * 3 + 0];
        uint8_t g = out_img_buf[i * 3 + 1];
        uint8_t b = out_img_buf[i * 3 + 2];

        uint8_t brightness = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);
        total_brightness += brightness;

        // Фиксируем максимальное значение
        if (brightness > max_brightness) {
            max_brightness = brightness;
        }
    }

    free(out_img_buf);

    int avg_brightness = (int)(total_brightness / total_pixels);

    // Выводим оба параметра в лог для анализа
    ESP_LOGI(TAG, "АНАЛИЗ: Средняя яркость: %d | Максимальная яркость: %d", avg_brightness, max_brightness);

    // Логика определения ночи по двум критериям:
    // Если даже самый яркий пиксель стал темнее 50 — это гарантированно ночь.
    if (max_brightness < 50 && avg_brightness < 25) {
        ESP_LOGW(TAG, "greenhouse_cam: Зафиксирована глубокая ночь!");
        return true;
    }

    return false;
}

bool save_photo_to_sd(camera_fb_t *fb, int index) 
{
    // Принудительно создаем папку. Если она есть, шаг просто пропустится
	mkdir(g_dir_path, 0755); 

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

	init_file_system_paths(); //Файловые пути и паттерны

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
	        if (!(hardware_errors_mask & 0x02) && fb && fb->buf && fb->len > 0 && !is_jpeg_frame_too_dark(fb->buf, fb->len)) {
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
