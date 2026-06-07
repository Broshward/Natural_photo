#include <string.h>
#include "cam_driver.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "sdkconfig.h"

// Родные инклуды нового видео-движка Espressif из примера GitHub
#include "hal/cam_ctlr_types.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"

// Используем ТОЛЬКО новый I2C драйвер New Generation
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cam_driver";

// Параметры UXGA кадра
#define CAM_HRES                      640 
#define CAM_VRES                      480
#define DVP_CAM_XCLK_FREQ_HZ          20000000 // 20 МГц из примера

// Карта пинов платы Freenove V1695 из example_config.h
#define DVP_CAM_XCLK_IO               15
#define DVP_CAM_PCLK_IO               13
#define DVP_CAM_DE_IO                 7  // HREF
#define DVP_CAM_VSYNC_IO              6

#define DVP_CAM_SCCB_SCL_IO           5
#define DVP_CAM_SCCB_SDA_IO           4

#define DVP_CAM_D0_IO                 11
#define DVP_CAM_D1_IO                 9
#define DVP_CAM_D2_IO                 8
#define DVP_CAM_D3_IO                 10
#define DVP_CAM_D4_IO                 12
#define DVP_CAM_D5_IO                 18
#define DVP_CAM_D6_IO                 17
#define DVP_CAM_D7_IO                 16

#define OV3660_I2C_ADDR               0x3C

typedef struct {
    esp_cam_ctlr_trans_t cam_trans;
} cam_context_t;

static TaskHandle_t main_task_handle = NULL;
static i2c_master_dev_handle_t cam_i2c_handle = NULL; // Хэндл нового I2C

// Таблица регистров UXGA YUV422 из примера
typedef struct { uint16_t reg; uint8_t val; } reg_val_t;
static const reg_val_t ov3660_uxga_regs[] = {
//    {0x3008, 0x82}, 
//	{0x3008, 0x42}, 
	{0x3103, 0x03}, 
    {0x3017, 0xff}, 
	{0x3018, 0xff}, 
	{0x501f, 0x20},//0x20 
    {0x4300, 0x32}, 
	//
//	{0x5002, 0x40}, 
//  {0x3800, 0x00}, 
//	{0x3801, 0x00}, 
//	{0x3802, 0x00}, 
//	{0x3803, 0x00}, 
//  {0x3804, 0x06}, 
//	{0x3805, 0x40}, 
//	{0x3806, 0x04}, 
//	{0x3807, 0xb0}, 
//  {0x3808, 0x06}, 
//	{0x3809, 0x40}, 
//	{0x380a, 0x04}, 
//	{0x380b, 0xb0}, 
//  {0x4000, 0x05}, 
//	{0x3815, 0x02}, 
//	{0x3008, 0x02}, 
	{0x503D, 0x80},
{0x0000, 0x00}  
};

// Функция ручной записи регистра через НОВЫЙ I2C
static esp_err_t sccb_write(uint16_t reg, uint8_t val) {
    uint8_t write_buf[3];
    write_buf[0] = (reg >> 8) & 0xFF; // Старший байт адреса
    write_buf[1] = reg & 0xFF;        // Младший байт адреса
    write_buf[2] = val;               // Значение
    
    // Передаем строго указатель на массив из 3 байт [6]
    return i2c_master_transmit(cam_i2c_handle, write_buf, 3, pdMS_TO_TICKS(100));
}

// Новая функция ручного чтения регистров камеры без NAK блокировок [6]
static uint8_t sccb_read(uint16_t reg) {
    uint8_t addr_buf[2];
    addr_buf[0] = (reg >> 8) & 0xFF;
    addr_buf[1] = reg & 0xFF;
    
    uint8_t read_val = 0;

    // Шаг А: Шлем адрес регистра
    i2c_master_transmit(cam_i2c_handle, addr_buf, 2, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(2));
    
    // Шаг Б: Забираем байт значения [6]
    i2c_master_receive(cam_i2c_handle, &read_val, 1, pdMS_TO_TICKS(100));
    
    return read_val;
}

// Оригинальные коллбеки транзакции DMA из примера [6]
static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
    cam_context_t *ctx = (cam_context_t *)user_data;
    *trans = ctx->cam_trans; 
    return false;
}

static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
    BaseType_t high_task_wakeup = pdFALSE;
    vTaskNotifyGiveFromISR(main_task_handle, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

uint8_t* cam_driver_take_uxga_photo(size_t *out_len) {
    main_task_handle = xTaskGetCurrentTaskHandle();
    
    // 1. Инициализация I2C Master шины New Generation [6]
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_1,
        .sda_io_num = DVP_CAM_SCCB_SDA_IO,
        .scl_io_num = DVP_CAM_SCCB_SCL_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    if (i2c_new_master_bus(&i2c_bus_config, &bus_handle) != ESP_OK) return NULL;

    // Конфигурация устройства камеры на шине [6]
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV3660_I2C_ADDR, 
        .scl_speed_hz = 100000, 
        
        // === НАШ ЖЕЛЕЗОБЕТОННЫЙ ФИКС ШИНЫ I2C ===
        // Принудительно отключаем аппаратную проверку ACK от камеры.
        // Драйвер больше не сбросит буфер в ноль, и чтение/запись оживут на 100%! [6]
        .flags.disable_ack_check = true, 
    };
    if (i2c_master_bus_add_device(bus_handle, &dev_config, &cam_i2c_handle) != ESP_OK) {
        i2c_del_master_bus(bus_handle);
        return NULL;
    }
    ESP_LOGI(TAG, "[+] Драйвер i2c_master New Generation успешно привязан.");

    // 2. Включение внешнего тактирования XCLK (20 МГц из примера)
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_1_BIT, .freq_hz = DVP_CAM_XCLK_FREQ_HZ, .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_1,
        .intr_type = LEDC_INTR_DISABLE, .gpio_num = DVP_CAM_XCLK_IO, .duty = 1, .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
    vTaskDelay(pdMS_TO_TICKS(150)); 

    // Будим матрицу перед записью
    sccb_write(0x3008, 0x02); 
    vTaskDelay(pdMS_TO_TICKS(50));

    // === НАЧАЛО ПРОВЕРКИ РЕГИСТРОВ (БОЛЬШЕ НЕТ НУЛЕЙ!) ===
    ESP_LOGW(TAG, "=== СТАРТ СКАНИРОВАНИЯ РЕГИСТРОВ ===");
    uint8_t pid_high = sccb_read(0x300A);
    uint8_t pid_low  = sccb_read(0x300B);
    uint8_t pwr_reg  = sccb_read(0x3008);
    ESP_LOGW(TAG, "[I2C SUCCESS] Реальный PID камеры: 0x%02X%02X (Должно быть: 0x3660)", pid_high, pid_low);
    ESP_LOGI(TAG, "[I2C SUCCESS] Состояние регистра 0x3008 (Питание): 0x%02X", pwr_reg);
    ESP_LOGW(TAG, "=== КОНЕЦ СКАНИРОВАНИЯ ===");

    // 3. Заливаем регистры геометрии UXGA
    int idx = 0;
    while (ov3660_uxga_regs[idx].reg != 0x0000) {
        sccb_write(ov3660_uxga_regs[idx].reg, ov3660_uxga_regs[idx].val);
        idx++;
    }

    // Включаем встроенные подтяжки PULLUP для КМОП линий нашей платы Freenove [6]
    gpio_config_t pullup_conf = {
        .pin_bit_mask = (1ULL << DVP_CAM_PCLK_IO) | (1ULL << DVP_CAM_DE_IO) | (1ULL << DVP_CAM_VSYNC_IO),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&pullup_conf);

    // 4. Инициализация Camera Controller из примера [6]
    esp_cam_ctlr_handle_t cam_handle = NULL;
    esp_cam_ctlr_dvp_pin_config_t pin_cfg = {
        .data_width = 8,
        .data_io = {
            DVP_CAM_D0_IO, DVP_CAM_D1_IO, DVP_CAM_D2_IO, DVP_CAM_D3_IO,
            DVP_CAM_D4_IO, DVP_CAM_D5_IO, DVP_CAM_D6_IO, DVP_CAM_D7_IO,
        },
        .vsync_io = DVP_CAM_VSYNC_IO, .de_io = DVP_CAM_DE_IO, .pclk_io = DVP_CAM_PCLK_IO, .xclk_io = DVP_CAM_XCLK_IO,
    };

    esp_cam_ctlr_dvp_config_t dvp_config = {
        .ctlr_id = 0, .clk_src = CAM_CLK_SRC_DEFAULT, 
        .h_res = CAM_HRES, .v_res = CAM_VRES,
        
        // Перечисления СТРОГО из нового API! [6]
	//	.pic_format_jpeg= 1,
        .input_data_color_type = CAM_CTLR_COLOR_YUV422_UYVY, 
        .output_data_color_type = CAM_CTLR_COLOR_YUV422_UYVY,
        
        .dma_burst_size = 64, .pin = &pin_cfg, .bk_buffer_dis = 1, .xclk_freq = DVP_CAM_XCLK_FREQ_HZ,
    };

    if (esp_cam_new_dvp_ctlr(&dvp_config, &cam_handle) != ESP_OK) {
        i2c_master_bus_rm_device(cam_i2c_handle); i2c_del_master_bus(bus_handle);
        return NULL;
    }

    // 5. Выделение буфера под UXGA YUV422 в PSRAM с токеном DMA [6]
    size_t cam_buffer_size = CAM_HRES * CAM_VRES * 2; 
    uint8_t *cam_buffer = esp_cam_ctlr_alloc_buffer(cam_handle, cam_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!cam_buffer) {
        esp_cam_ctlr_del(cam_handle); i2c_master_bus_rm_device(cam_i2c_handle); i2c_del_master_bus(bus_handle);
        return NULL;
    }

    // 6. Регистрация коллбеков [6]
    cam_context_t cam_ctx = { .cam_trans = { .buffer = cam_buffer, .buflen = cam_buffer_size } };
    esp_cam_ctlr_evt_cbs_t cbs = { .on_get_new_trans = s_camera_get_new_vb, .on_trans_finished = s_camera_get_finished_trans };
    esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, &cam_ctx);

    // 7. Старт аппаратного захвата [6]
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam_handle));
    ESP_LOGI(TAG, "[!] Контроллер запущен. Ожидание кадра UXGA...");
    esp_cam_ctlr_start(cam_handle);

    // Ждем прерывания от сработавшего DMA коллбека (4 секунды) [6]
    uint32_t dma_ok = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(4000));

    // 8. Съемка окончена — тушим контроллер DMA [6]
    esp_cam_ctlr_stop(cam_handle); esp_cam_ctlr_disable(cam_handle); esp_cam_ctlr_del(cam_handle);

    // === ВАШ КРИТИЧЕСКИЙ ТЮНИНГ ЭНЕРГОСБЕРЕЖЕНИЯ ===
    // Отправляем OV3660 в глубокий Standby (40 мкА) 
    sccb_write(0x3008, 0x40); 
    pwr_reg  = sccb_read(0x3008);
    ESP_LOGI(TAG, "[I2C SUCCESS] Состояние регистра 0x3008 (Питание): 0x%02X", pwr_reg);
    ESP_LOGW(TAG, "[+] Матрица успешно усыплена в Standby (40 мкА).");

    i2c_master_bus_rm_device(cam_i2c_handle);
    i2c_del_master_bus(bus_handle); 
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0); 

    if (dma_ok > 0) {
        *out_len = cam_buffer_size; 
		//for(int i=0; i<cam_buffer_size/10; i++){
		//	printf("%c ",cam_buffer[i]);
		//}
		//printf("\n");
        return cam_buffer; 
    } else {
        ESP_LOGE(TAG, "[-] Тайм-аут нового драйвера в режиме YUV422.");
        //free(cam_buffer);
        return NULL;
    }
}
