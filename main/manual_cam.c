#include <stdio.h>
#include <string.h>
#include "manual_cam.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Легальный системный драйвер прямого доступа к памяти ядра ESP-IDF v5.x
#include "esp_private/gdma.h" 
#include "soc/lcd_cam_reg.h"
#include "soc/gpio_sig_map.h"
#include "driver/i2c_master.h"
#include "esp_private/periph_ctrl.h" // КРИТИЧЕСКИ ВАЖНО: Дает доступ к включению тактов ядра
#include "soc/gpio_reg.h"

static const char *TAG = "pure_hardware_cam";

static i2c_master_dev_handle_t cam_i2c_handle = NULL; // Хэндл нового I2C (для SCCB)

void enable_cam(void);
void shutdown_cam(void);
void init_cam();
void reset_cam();


esp_err_t take_photo(uint8_t *out_buffer, size_t expected_size) {
    ESP_LOGW(TAG, "[START] Запуск чистого ручного захвата кадра без периферии!");

    // 1. НАСТРОЙКА ЧИСТОГО ШИМ ДЛЯ XCLK (12 МГц через LEDC остается)
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_1_BIT, .freq_hz = 6000000, .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_1,
        .intr_type = LEDC_INTR_DISABLE, .gpio_num = CAM_XCLK_IO, .duty = 1, .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
    vTaskDelay(pdMS_TO_TICKS(150)); // Даем камере стабильно прогреться тактами
init_cam();

    // 2. НАСТРОЙКА ВСЕХ ПИНОВ ШИНЫ КАМЕРЫ КАК ОБЫЧНЫЕ ЦИФРОВЫЕ ВХОДЫ
    int pins[] = {CAM_D0_IO, CAM_D1_IO, CAM_D2_IO, CAM_D3_IO, CAM_D4_IO, CAM_D5_IO, CAM_D6_IO, CAM_D7_IO};
    uint64_t data_mask = 0;
    for(int i=0; i<8; i++) data_mask |= (1ULL << pins[i]);
    
    gpio_config_t io_conf = {
        .pin_bit_mask = data_mask | (1ULL << CAM_PCLK_IO) | (1ULL << CAM_HREF_IO) | (1ULL << CAM_VSYNC_IO),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // Указатель на 32-битный аппаратный регистр, где лежат живые уровни ВСЕХ ножек процессора одновременно!
    volatile uint32_t *gpio_in_reg = (volatile uint32_t *)GPIO_IN_REG;

    // Считаем параметры под наше VGA разрешение (640х480)
    size_t total_bytes_to_read = 2048 * 1536 * 2; // 614 400 байт кадра
    size_t bytes_collected = 0;
	uint32_t flags = 0;
	size_t hrefs_collecteg = 0;

    // 3. ЖДЕМ СИНХРОНИЗАЦИЮ: НАЧАЛО НОВОГО КАДРА (VSYNC)
    ESP_LOGI(TAG, "[!] Ждём VSYNC."); 
    
    uint32_t vsync_timeout = 0;
    // Ждем, пока на линии VSYNC висит единица (камера заканчивает старый кадр)
    while(gpio_get_level(CAM_VSYNC_IO) == 1) {
        esp_rom_delay_us(1);
        if(++vsync_timeout > 16000000) { // Исключаем зависание
            ESP_LOGE(TAG, "[-] Камера не отвечает по VSYNC!");
            return ESP_ERR_TIMEOUT;
        }
    }

    ESP_LOGI(TAG, "[!] VSYNC пришёл. Ждём начало кадра...");
    // Выключаем планировщик FreeRTOS, чтобы операционная система не отвлекала 
    vTaskSuspendAll(); 
    // Ждем спад в ноль — это железный сигнал: «Внимание, пошел новый кадр!»
    while(gpio_get_level(CAM_VSYNC_IO) == 0) {
        esp_rom_delay_us(1);
    }

    //ESP_LOGW(TAG, "[!] Кадр пошел! Включаем высокоскоростной цикл чтения ножек...");
    // наш процессор своими делами во время наносекундного съема пикселей!
    // 4. ГЛАВНЫЙ СУПЕР-ЦИКЛ: ХАКЕРСКИЙ BIT-BANGING
    while (1) {
        
        // Ждем, когда линия HREF поднимется в единицу (пошла валидная строка пикселей)
        // Если HREF упал в 0, мы просто пропускаем паузу между строками
        if (gpio_get_level(CAM_HREF_IO) == 1) {
            
            // Ждем передний фронт пиксельного такта PCLK (переход из 0 в 1)
            // Именно в этот момент камера выставляет честный вольтаж на пины D0-D7
            while (gpio_get_level(CAM_PCLK_IO) == 0) {
                // Крутим пустой микро-цикл ожидания такта
            }

            // --- МОМЕНТ ИСТИНЫ: ЧИТАЕМ ВСЕ ПИНЫ ЗА 1 ТАКТ ЯДРА ---
            uint32_t live_gpio_state = *gpio_in_reg;

            // Расшифровываем биты. Мы вытягиваем состояние каждой ножки из общего регистра
            // и бережно склеиваем их обратно в один готовый 8-битный байт пикселя!
            uint8_t pixel_byte = 
                (((live_gpio_state >> 11) & 0x01) << 0) | // D0 (GPIO 11) -> бит 0
                (((live_gpio_state >> 9)  & 0x01) << 1) | // D1 (GPIO 9)  -> бит 1
                (((live_gpio_state >> 8)  & 0x01) << 2) | // D2 (GPIO 8)  -> бит 2 (уже на месте, просто маскируем)
                (((live_gpio_state >> 10) & 0x01) << 3) | // D3 (GPIO 10) -> бит 3
                (((live_gpio_state >> 12) & 0x01) << 4) | // D4 (GPIO 12) -> бит 4
                (((live_gpio_state >> 18) & 0x01) << 5) | // D5 (GPIO 18) -> бит 5
                (((live_gpio_state >> 17) & 0x01) << 6) | // D6 (GPIO 17) -> бит 6
                (((live_gpio_state >> 16) & 0x01) << 7);  // D7 (GPIO 16) -> бит 7
            // Кладем добытый байт прямо в нашу PSRAM
            out_buffer[bytes_collected] = pixel_byte;
            bytes_collected++;

            // Ждем, пока такт PCLK опустится обратно в ноль, чтобы не прочесть один и тот же байт дважды
            while (gpio_get_level(CAM_PCLK_IO) == 1) {
                // Ждем спад такта
            }
        } else {
			hrefs_collecteg++;
            // Если мы вылетели из строки (HREF == 0), проверяем, не кончился ли кадр по VSYNC
			while(gpio_get_level(CAM_HREF_IO) == 0){
				if (gpio_get_level(CAM_VSYNC_IO) == 0) {
					// Камера завершила кадр раньше, чем мы ожидали — выходим из цикла!
					flags |= 1;
					break;
				}
			}
			if (flags & 1) break;
        }
    }

    // Возвращаем операционную систему к жизни
    xTaskResumeAll();
	if (flags & 1){
		ESP_LOGW(TAG, "[!] Камера завершила кадр раньше, чем мы ожидали!");
	}
	//shutdown_cam();
    ESP_LOGW(TAG, "[SUCCESS] Ручной хардварный кадр завершен! Процессор собрал %d байт.", bytes_collected);
    ESP_LOGW(TAG, "[SUCCESS] Ручной хардварный кадр завершен! Количество строк %d.", hrefs_collecteg);
    return ESP_OK;
}


// Функция ручной записи регистра через НОВЫЙ I2C
esp_err_t sccb_write(uint16_t reg, uint8_t val) 
{
    uint8_t write_buf[3];
    write_buf[0] = (reg >> 8) & 0xFF; // Старший байт адреса
    write_buf[1] = reg & 0xFF;        // Младший байт адреса
    write_buf[2] = val;               // Значение
    
    // Передаем строго указатель на массив из 3 байт [6]
    return i2c_master_transmit(cam_i2c_handle, write_buf, 3, pdMS_TO_TICKS(100));
}

// Новая функция ручного чтения регистров камеры без NAK блокировок [6]
uint8_t sccb_read(uint16_t reg) 
{
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

#define DVP_CAM_SCCB_SCL_IO           5
#define DVP_CAM_SCCB_SDA_IO           4
#define OV3660_I2C_ADDR               0x3C
void i2c_init()
{
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
    if (i2c_new_master_bus(&i2c_bus_config, &bus_handle) != ESP_OK) return;

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
        return;
    }
    ESP_LOGI(TAG, "[+] Драйвер i2c_master New Generation успешно привязан.");
}

void enable_cam(void) {
    sccb_write(0x3008, 0x00); 
}

void shutdown_cam(void) {
    ESP_LOGW("main_cam", "[!] Отправляем OV3660 в программный Standby (40 мкА)...");
    
    // Включаем бит 6 в регистре 0x3008, полностью обесточивая матрицу и объектив!
    sccb_write(0x3008, 0x40); 
}

void reset_cam()
{
    sccb_write(0x3008, 0x82); 
	vTaskDelay(pdMS_TO_TICKS(2));
}

typedef struct { uint16_t reg; uint8_t val; } reg_val_t;
//static const reg_val_t ov3660_uxga_regs[] = {
//    // 1. Сброс и пробуждение
////    {0x3008, 0x82}, // Глобальный программный сброс камеры (Software Reset)
//    {0x3008, 0x42}, // Включаем внутреннюю цифровую логику
//    
//    // 2. Настройка тактирования (PLL включен, работает стабильно от ваших 12 МГц)
//    {0x303A, 0x00}, // Включаем PLL (Bypass = 0)
//    {0x3103, 0x03}, // Родной системный делитель ядра
//    
//    // 3. Перевод ножек параллельной шины D0-D7 и синхронизации на ВЫВОД данных
//    {0x3017, 0xff}, 
//    {0x3018, 0xff}, 
//    
//    // 4. Включаем цифровой процессор камеры (ISP) без блокировок
//    {0x501f, 0x01}, // !!! ФИКС: Включаем ISP конвейер (вместо 0x20, который вешал чип)
//    {0x4300, 0x32}, // Формат вывода: UYVY Packed
//    {0x5002, 0x40}, // Включаем цветовую YUV матрицу
//    
//    // 5. ГЕОМЕТРИЯ ОКНА: Настраиваем матрицу на разрешение VGA (640x480)
//    // Без этих регистров счетчик строк камеры просто стоит на месте!
//    {0x3800, 0x00}, {0x3801, 0x00}, // Начало X = 0
//    {0x3802, 0x00}, {0x3803, 0x00}, // Начало Y = 0
//    {0x3804, 0x02}, {0x3805, 0x80}, // Ширина окна (640 в hex -> 0x0280)
//    {0x3806, 0x01}, {0x3807, 0xe0}, // Высота окна (480 в hex -> 0x01E0)
//    {0x3808, 0x02}, {0x3809, 0x80}, // Ширина вывода (Output Width = 640)
//    {0x380a, 0x01}, {0x380b, 0xe0}, // Высота вывода (Output Height = 480)
//    
//    {0x4000, 0x05}, // Включаем автоматическую калибровку черного (BLC)
//    {0x3815, 0x02}, // Настройка полярности VSYNC/HREF
//    
//    // 6. ФИНАЛЬНЫЙ СТАРТ
//    {0x3008, 0x02}, // Wake up! Запускаем конвейер развертки!
//    {0x0000, 0x00}  
//};

const reg_val_t ov3660_uxga_regs[] = {
	{0x3103, 0x13}, //From predevider 0x11 
//    {0x3008, 0x42}, 
    {0x3017, 0xff},	
    {0x3018, 0xff},
    {0x302c, 0xc3}, // Pad driving strength 4x
    {0x4740, 0x21},	// VSYNC active high

	// my section
//	{0x303A, 0x80}, // PLL bypass
	{0x303B, 0x02}, // PLL multiplier
	{0x303d, 0x33}, // PLL divider
	{0x501f, 0x00}, // YUV422
//	{0x460C, 0x01}, 
//	{0x3824, 0x04},

//    {0x3a18, 0x00}, // AEC gain CEILING
//    {0x3a19, 0xf8}, // AEC GAIN CEILING 
//
//    {0x3000, 0x10}, //Reset for Individual Blocks
//    {0x3004, 0xef}, // Clock Enable Control for Individual Blocks
//
//    {0x6700, 0x05}, // Temperature Sensor Control Registers 
//    {0x6701, 0x19}, // Temperature Sensor Control Registers
//    {0x6702, 0xfd}, // Temperature Sensor Control Registers
//    {0x6703, 0xd1}, // Temperature Sensor Control Registers
//    {0x6704, 0xff}, // Temperature Sensor Control Registers
//    {0x6705, 0xff}, // Temperature Sensor Control Registers
//
//	//anti-flicker section
//    {0x3c01, 0x80}, // sigmadelta/5060Hz detector
//    {0x3c00, 0x04},// sigmadelta/5060Hz detector
//    {0x3a08, 0x00}, {0x3a09, 0x62}, //50Hz Band Width Step (10bit)
//    {0x3a0e, 0x08}, //50Hz Max Bands in One Frame (6 bit)
//    {0x3a0a, 0x00}, {0x3a0b, 0x52}, //60Hz Band Width Step (10bit)
//    {0x3a0d, 0x09}, //60Hz Max Bands in One Frame (6 bit)
//
//    {0x3a00, 0x3a},//night mode off
//    {0x3a14, 0x09}, // 50Hz Maximum Exposure Output Limit
//    {0x3a15, 0x30}, // 50Hz Maximum Exposure Output Limit
//    {0x3a02, 0x09}, // 60Hz Maximum Exposure Output Limit
//    {0x3a03, 0x30}, // 60Hz Maximum Exposure Output Limit
//
//    {0x440e, 0x08}, // COMPRESSION CTRL0E
//    {0x4713, 0x02}, // Compression mode select
//
//    {0x501f, 0x00}, // Format Control (YUV422)
//
//	
//	// AWB
//    {0x5180, 0xff}, // AWB CONTROL 00
//    {0x5181, 0xf2}, // AWB CONTROL 01
//    {0x5182, 0x00}, // AWB CONTROL 02
//    {0x5183, 0x14},	// AWB CONTROL 03
//    {0x5184, 0x25}, // AWB CONTROL 04
//    {0x5185, 0x24}, // AWB CONTROL 05
//    {0x5186, 0x16}, // Advanced AWB Control
//    {0x5187, 0x16}, // Advanced AWB Control    
//    {0x5188, 0x16}, // Advanced AWB Control    
//    {0x5189, 0x68}, // Advanced AWB Control    
//    {0x518a, 0x60}, // Advanced AWB Control    
//    {0x518b, 0xe0}, // Advanced AWB Control    
//    {0x518c, 0xb2}, // Advanced AWB Control    
//    {0x518d, 0x42}, // Advanced AWB Control    
//    {0x518e, 0x35}, // Advanced AWB Control    
//    {0x518f, 0x56}, // Advanced AWB Control    
//    {0x5190, 0x56}, // Advanced AWB Control    
//    {0x5191, 0xf8}, // AWB top limit
//    {0x5192, 0x04}, // AWB bottom limit
//    {0x5193, 0x70}, // Red limit
//    {0x5194, 0xf0}, // Green limit
//    {0x5195, 0xf0}, // Blue limit
//    {0x5196, 0x03}, // AWB CONTROL 22
//    {0x5197, 0x01}, // AWB CONTROL 23 (Local limit)
//    {0x519e, 0x38}, // AWB CONTROL 30
//
//	// CMX control registers
//    {0x5381, 0x1d},
//    {0x5382, 0x60},
//    {0x5383, 0x03},
//    {0x5384, 0x0c},
//    {0x5385, 0x78},
//    {0x5386, 0x84},
//    {0x5387, 0x7d},
//    {0x5388, 0x6b},
//    {0x5389, 0x12},
//    {0x538a, 0x01},
//    {0x538b, 0x98},
//
//	//gamma control registers
//    {0x5480, 0x01},
//	//    {0x5481, 0x05},
//	//    {0x5482, 0x09},
//	//    {0x5483, 0x10},
//	//    {0x5484, 0x3a},
//	//    {0x5485, 0x4c},
//	//    {0x5486, 0x5a},
//	//    {0x5487, 0x68},
//	//    {0x5488, 0x74},
//	//    {0x5489, 0x80},
//	//    {0x548a, 0x8e},
//	//    {0x548b, 0xa4},
//	//    {0x548c, 0xb4},
//	//    {0x548d, 0xc8},
//	//    {0x548e, 0xde},
//	//    {0x548f, 0xf0},
//	//    {0x5490, 0x15},
//
//	//	ISP general control registers
//    {0x5000, 0xa7},
//
//	// LENC control registers
//	// GREEN MATRIX
//    {0x5800, 0x0C},
//    {0x5801, 0x09},
//    {0x5802, 0x0C},
//    {0x5803, 0x0C},
//    {0x5804, 0x0D},
//    {0x5805, 0x17},
//    {0x5806, 0x06},
//    {0x5807, 0x05},
//    {0x5808, 0x04},
//    {0x5809, 0x06},
//    {0x580a, 0x09},
//    {0x580b, 0x0E},
//    {0x580c, 0x05},
//    {0x580d, 0x01},
//    {0x580e, 0x01},
//    {0x580f, 0x01},
//    {0x5810, 0x05},
//    {0x5811, 0x0D},
//    {0x5812, 0x05},
//    {0x5813, 0x01},
//    {0x5814, 0x01},
//    {0x5815, 0x01},
//    {0x5816, 0x05},
//    {0x5817, 0x0D},
//    {0x5818, 0x08},
//    {0x5819, 0x06},
//    {0x581a, 0x05},
//    {0x581b, 0x07},
//    {0x581c, 0x0B},
//    {0x581d, 0x0D},
//    {0x581e, 0x12},
//    {0x581f, 0x0D},
//    {0x5820, 0x0E},
//    {0x5821, 0x10},
//    {0x5822, 0x10},
//    {0x5823, 0x1E},
//	// Blue & RED Matrix
//    {0x5824, 0x53},
//    {0x5825, 0x15},
//    {0x5826, 0x05},
//    {0x5827, 0x14},
//    {0x5828, 0x54},
//    {0x5829, 0x25},
//    {0x582a, 0x33},
//    {0x582b, 0x33},
//    {0x582c, 0x34},
//    {0x582d, 0x16},
//    {0x582e, 0x24},
//    {0x582f, 0x41},
//    {0x5830, 0x50},
//    {0x5831, 0x42},
//    {0x5832, 0x15},
//    {0x5833, 0x25},
//    {0x5834, 0x34},
//    {0x5835, 0x33},
//    {0x5836, 0x24},
//    {0x5837, 0x26},
//    {0x5838, 0x54},
//    {0x5839, 0x25},
//    {0x583a, 0x15},
//    {0x583b, 0x25},
//    {0x583c, 0x53},
//	// LENC BR OFFSET
//    {0x583d, 0xCF},
//
//	//step of the exposure/gain adjustmen
//    {0x3a0f, 0x30},
//    {0x3a10, 0x28},
//
//	// Shaman settings
//    {0x3a1b, 0x30},
//    {0x3a1e, 0x28},
//    {0x3a11, 0x60},
//    {0x3a1f, 0x14},
//
//	
//    {0x5302, 0x28}, // CIP SHARPENMT OFFSET1
//    {0x5303, 0x20}, // CIP SHARPENMT OFFSET2
//
//    {0x5306, 0x1c}, //de-noise offset 1
//    {0x5307, 0x28}, //de-noise offset 2
//
//	// black level calibration (BLC)
//    {0x4002, 0xc5}, 
//    {0x4003, 0x81},
//    {0x4005, 0x12},
//
//	// average control
//    {0x5688, 0x11},
//    {0x5689, 0x11},
//    {0x568a, 0x11},
//    {0x568b, 0x11},
//    {0x568c, 0x11},
//    {0x568d, 0x11},
//    {0x568e, 0x11},
//    {0x568f, 0x11},
//
//	// auto color saturation adjust
//    {0x5580, 0x06},
//    {0x5588, 0x00},
//    {0x5583, 0x40},
//    {0x5584, 0x2c},
//
//    {0x5001, 0x83}, // turn color matrix, awb and SDE

    {0x503D, 0x80}, // TEST PATTERN
    {0x3008, 0x02}, 
    // Маркер окончания массива конфигурации
    {0x0000, 0x00}
};

void init_cam()
{
	i2c_init();
	reset_cam();
    int idx = 0;
    while (ov3660_uxga_regs[idx].reg != 0x0000) {
        sccb_write(ov3660_uxga_regs[idx].reg, ov3660_uxga_regs[idx].val);
		//vTaskDelay(1);
        idx++;
    }
	printf("Read 3008 = %u\n", sccb_read(0x3008));
	printf("Temperaure = %u\n", sccb_read(0x6719));
	printf("ID %x%x\n", sccb_read(0x300A), sccb_read(0x300B));
}
