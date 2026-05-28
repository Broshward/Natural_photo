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

// Периферия и ФС
#include "esp_camera.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"

// Сетевой стек
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

static const char *TAG = "greenhouse_cam";

#define WIFI_SSID           "SamstillingHeimar" // Если раздаете со смартфона - укажите имя точки телефона
#define WIFI_PASS           "HarmoniesWorlds"
#define SERVER_IP           "192.168.43.82"  // Для Android-модема IP телефона обычно ВСЕГДА 192.168.43.1
#define SERVER_PORT         8888

#define TARGET_PERIOD_SEC   600             // 10 минут
#define MOUNT_POINT         "/sdcard"
#define INDEX_FILE_PATH     "/sdcard/last_sent.txt"

// Пины Freenove V1695
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
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
static int s_retry_num = 0;

RTC_DATA_ATTR static int boot_count = 0;
static int last_sent_index = 0; 
static int server_requested_index = -1; 
static uint64_t global_start_time = 0; // Время старта всей большой сессии

static camera_config_t camera_config = {
    .pin_pwdn = PWDN_GPIO_NUM, .pin_reset = RESET_GPIO_NUM, .pin_xclk = XCLK_GPIO_NUM,
    .pin_sccb_sda = SIOD_GPIO_NUM, .pin_sccb_scl = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM, .pin_d6 = Y8_GPIO_NUM, .pin_d5 = Y7_GPIO_NUM, .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM, .pin_d2 = Y4_GPIO_NUM, .pin_d1 = Y3_GPIO_NUM, .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM, .pin_href = HREF_GPIO_NUM, .pin_pclk = PCLK_GPIO_NUM,
    .xclk_freq_hz = 24000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_YUV422, .frame_size = FRAMESIZE_XGA,
    .jpeg_quality = 12, .fb_count = 1, .grab_mode = CAMERA_GRAB_WHEN_EMPTY, .fb_location = CAMERA_FB_IN_PSRAM    
};

static esp_err_t init_sd_card(sdmmc_card_t** out_card) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = { .format_if_mount_failed = false, .max_files = 3, .allocation_unit_size = 16 * 1024 };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT; host.slot = SDMMC_HOST_SLOT_1;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; slot_config.clk = GPIO_NUM_39; slot_config.cmd = GPIO_NUM_38; slot_config.d0  = GPIO_NUM_40;
    return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, out_card);
}

void write_last_sent_index_to_sd(int index) {
    sdmmc_card_t* card;
    if (init_sd_card(&card) == ESP_OK) {
        FILE *f = fopen(INDEX_FILE_PATH, "w");
        if (f) { fprintf(f, "%d", index); fclose(f); }
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    }
}

int read_last_sent_index_from_sd(void) {
    sdmmc_card_t* card;
    int index = 0;
    if (init_sd_card(&card) == ESP_OK) {
        FILE *f = fopen(INDEX_FILE_PATH, "r");
        if (f) { if (fscanf(f, "%d", &index) != 1) index = 0; fclose(f); }
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    }
    return index;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 3) { s_retry_num++; esp_wifi_connect(); } 
        else { xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT); }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void network_stack_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

bool wifi_power_on(void) {
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(6000));
    return (bits & WIFI_CONNECTED_BIT) ? true : false;
}

bool send_buffer_to_server(uint8_t *buf, size_t len, int index) {
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return false;

    struct timeval timeout = { .tv_sec = 4, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) { close(sock); return false; }

    uint32_t header[2] = { (uint32_t)index, (uint32_t)len };
    if (send(sock, header, sizeof(header), 0) < 0) { close(sock); return false; }

    if (len > 0) {
        size_t total_sent = 0;
        while (total_sent < len) {
            size_t to_send = (len - total_sent > 4096) ? 4096 : (len - total_sent);
            int sent = send(sock, buf + total_sent, to_send, 0);
            if (sent < 0) { close(sock); return false; }
            total_sent += sent;
        }
    }

    int32_t rx_index = -1;
    int rx_len = recv(sock, &rx_index, sizeof(rx_index), 0);
    if (rx_len == sizeof(rx_index)) {
        server_requested_index = rx_index;
    }

    close(sock);
    return true;
}

int get_last_file_index_from_sd(void) {
    sdmmc_card_t* card;
    if (init_sd_card(&card) != ESP_OK) return 0;
    int index = 1; char path[32]; struct stat st;
    while (index < 99999) {
        snprintf(path, sizeof(path), "%s/%05d.raw", MOUNT_POINT, index); // Пятизначный формат %05d
        if (stat(path, &st) != 0) break;
        index++;
    }
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    return index - 1;
}

// Функция ДИНАМИЧЕСКОГО ночного режима (включается только при глубоких сумерках)
void apply_smart_camera_settings(void) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->id.PID != OV3660_PID) return;

    s->set_whitebal(s, 1);       
    s->set_exposure_ctrl(s, 1);  
    s->set_gain_ctrl(s, 1);      
    s->set_agc_gain(s, 30);      

    // Читаем текущее значение автоматической выдержки матрицы. 
    // Если на улице темно, внутренние регистры AEC будут на максимуме.
    // Если темно — включаем аппаратный ночной удлиненный режим. Если светло — жестко выключаем пересвет!
    int current_aec = s->get_reg(s, 0x3501, 0xff); 
    if (current_aec > 0x80) { 
        ESP_LOGW(TAG, "[!] Обнаружены сумерки. Активация Night Mode.");
        s->set_reg(s, 0x3a00, 0xff, 0x04); // Медленный FPS, длинное накопление света
        s->set_reg(s, 0x3a14, 0xff, 0x03); 
        s->set_reg(s, 0x3a15, 0xff, 0x00); 
        s->set_ae_level(s, 2); 
    } else {
        ESP_LOGI(TAG, "[+] День. Дневной режим зафиксирован (Защита от пересветов).");
        s->set_reg(s, 0x3a00, 0xff, 0x00); // Обычный быстрый затвор
        s->set_ae_level(s, 0); 
    }
}

// Вынесенная функция съемки планового кадра посреди очереди
void take_scheduled_photo(void) {
    boot_count++;
    ESP_LOGW(TAG, "[!!!] Наступило время планового снимка. Съемка кадра №%d по графику...", boot_count);
    
    // Временно тушим Wi-Fi, чтобы не шумел по питанию при съемке
    esp_wifi_stop(); 
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_camera_init(&camera_config);
    //apply_smart_camera_settings();
    vTaskDelay(pdMS_TO_TICKS(1200)); 

    for (int i = 0; i < 2; i++) {
        camera_fb_t *fb_flush = esp_camera_fb_get();
        if (fb_flush) esp_camera_fb_return(fb_flush);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        sdmmc_card_t* card;
        if (init_sd_card(&card) == ESP_OK) {
            char file_path[32];
            snprintf(file_path, sizeof(file_path), "%s/%05d.raw", MOUNT_POINT, boot_count); // Формат 00046.raw
            FILE *f = fopen(file_path, "wb");
            if (f != NULL) { fwrite(fb->buf, 1, fb->len, f); fclose(f); }
            esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
            ESP_LOGI(TAG, "Плановый кадр %05d.raw успешно добавлен на SD карту.", boot_count);
        }
        esp_camera_fb_return(fb);
    }
    // Возвращаем Wi-Fi обратно для продолжения трансляции очереди
    wifi_power_on(); 
}

void app_main(void) {
    global_start_time = esp_timer_get_time();
	gpio_config_t io_conf = { .pin_bit_mask = (1ULL << 48), .mode = GPIO_MODE_OUTPUT, .pull_up_en = 0, .pull_down_en = 1 };
	gpio_config(&io_conf); gpio_set_level(48, 0);
	nvs_flash_init();
	network_stack_init();
	if (boot_count == 0) {
		boot_count = get_last_file_index_from_sd();
	}
	last_sent_index = read_last_sent_index_from_sd();
	// Снимаем первый стартовый кадр
	take_scheduled_photo();
	// Запускаем сессию Wi-Fi
    esp_wifi_stop(); 
	if (wifi_power_on()) {
		ESP_LOGI(TAG, "[+] Сеть активна. Синхронизация индексов с сервером...");
		uint8_t dummy = 0;
		send_buffer_to_server(&dummy, 0, boot_count);
		if (server_requested_index != -1) {
			last_sent_index = server_requested_index;
			write_last_sent_index_to_sd(last_sent_index);
		}
		// ПОТОКОВЫЙ ЦИКЛ БЕЗ СНА: гоним файлы, пока очередь не опустеет совсем
		while (last_sent_index < boot_count) {
			// КРИТИЧЕСКИЙ ПРЕДОХРАНИТЕЛЬ ХРОНОМЕТРАЖА:
			// Если мы непрерывно шлем историю уже почти 10 минут (590 секунд) —
			// мы должны ПРЯМО СЕЙЧАС сделать новое плановое фото, не ложась спать!
			uint64_t total_running_time_sec = (esp_timer_get_time() - global_start_time) / 1000000ULL;
			if (total_running_time_sec >= (TARGET_PERIOD_SEC - 10)) {
				take_scheduled_photo(); // Сделает снимок, увеличит boot_count, и цикл while продолжится дальше!
				global_start_time = esp_timer_get_time(); // Сбрасываем таймер периода на новый круг
			}
			int next_file_to_send = last_sent_index + 1;
			uint8_t *file_buf = NULL;
			size_t file_size = 0;
			sdmmc_card_t *card;
			if (init_sd_card(&card) == ESP_OK) {
				char file_path[32]; struct stat st;
				snprintf(file_path, sizeof(file_path), "%s/%05d.raw", MOUNT_POINT, next_file_to_send);
				if (stat(file_path, &st) == 0) {
					file_size = st.st_size;
					file_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
					if (file_buf) {
						FILE *f = fopen(file_path, "rb");
						if (f) { fread(file_buf, 1, file_size, f); fclose(f); }
						else { heap_caps_free(file_buf); file_buf = NULL; }
					}
				}
				esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
			}
			if (file_buf && file_size > 0) {
				// Если сокет открыт, пускаем файл по сети
				if (send_buffer_to_server(file_buf, file_size, next_file_to_send)) {
					last_sent_index = next_file_to_send;
					write_last_sent_index_to_sd(last_sent_index);
					ESP_LOGI(TAG, "[->] Накопленный файл %05d.raw успешно передан.", next_file_to_send);
				} else {
					ESP_LOGE(TAG, "[-] Сервер отключился. Очередь заморожена до следующей сессии.");
					heap_caps_free(file_buf); break;
				}
				heap_caps_free(file_buf);
				vTaskDelay(pdMS_TO_TICKS(30)); // Максимальная скорость прокачки пакетов
			} else {
				// Если файла физически нет на флешке (например битый индекс), сдвигаем очередь вперед
				last_sent_index = next_file_to_send;
			}
		}
	} else {
		ESP_LOGE(TAG, "[-] Сеть недоступна. Кадр %05d остался в очереди.", boot_count);
	}
	// Сюда плата попадет ТОЛЬКО тогда, когда вся очередь будет успешно отправлена!
	esp_wifi_stop();
	gpio_hold_en(GPIO_NUM_48);
	esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
	uint64_t session_time_us = esp_timer_get_time() - global_start_time;
	int session_time_sec = (int)(session_time_us / 1000000ULL);
	int actual_sleep_time_sec = TARGET_PERIOD_SEC - session_time_sec;
	if (actual_sleep_time_sec < 15) actual_sleep_time_sec = 15;
	ESP_LOGW(TAG, "[*] Все хвосты очереди закрыты! Отработано: %d сек. Сон на: %d сек.",
	session_time_sec, actual_sleep_time_sec);
	esp_sleep_enable_timer_wakeup((uint64_t)actual_sleep_time_sec * 1000000LL);
	esp_deep_sleep_start();
}
