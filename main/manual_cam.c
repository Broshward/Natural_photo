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

static const char *TAG = "manual_cam";

static i2c_master_dev_handle_t cam_i2c_handle = NULL; // Хэндл нового I2C (для SCCB)

void enable_cam(void);
void shutdown_cam(void);
void init_cam();
void reset_cam();

// Структура аппаратного дескриптора GDMA процессора S3
typedef struct gdma_descr_s {
    struct {
        uint32_t size : 12;         // Размер буфера ноды
        uint32_t length : 12;       // Сколько реально записано
        uint32_t reversed : 6;
        uint32_t err_out : 1;
        uint32_t owner : 1;         // 1 - дескриптор принадлежит аппаратному GDMA
    } dw0;
    uint8_t *buffer;                // Указатель на физический кусок памяти в PSRAM
    struct gdma_descr_s *next;      // Ссылка на следующий дескриптор (Link List)
} gdma_descr_t;

// Переменные для хэндлов нового драйвера
static gdma_channel_handle_t dma_tx_channel = NULL; // Заглушка, передача не нужна
static gdma_channel_handle_t dma_rx_channel = NULL; // Наш рабочий канал приема
static gdma_descr_t *dma_desc_list = NULL;
static size_t desc_count = 0;

#define REG_LCD_CAM_BASE       0x60041000 
#define REG_CAM_CTRL           (REG_LCD_CAM_BASE + 0x0004)
#define REG_CAM_CTRL1          (REG_LCD_CAM_BASE + 0x0008)
#define REG_CAM_RGB_YUV        (REG_LCD_CAM_BASE + 0x000C)
#define REG_LCD_CLK            (REG_LCD_CAM_BASE + 0x0064) // Регистр тактирования всего блока

esp_err_t take_photo(uint8_t *out_buffer, size_t expected_size) {
    periph_module_enable(PERIPH_LCD_CAM_MODULE);
    periph_module_reset(PERIPH_LCD_CAM_MODULE);
	

    ESP_LOGW(TAG, "[START] Конфигурация RX-режима LCD_CAM по даташиту...");

    volatile uint32_t *cam_ctrl = (volatile uint32_t *)LCD_CAM_CAM_CTRL_REG;
    volatile uint32_t *cam_ctrl1 = (volatile uint32_t *)LCD_CAM_CAM_CTRL1_REG;
    volatile uint32_t *lcd_clk_reg = (volatile uint32_t *)LCD_CAM_LCD_CLOCK_REG;
    volatile uint32_t *cam_int_st = (volatile uint32_t *)LCD_CAM_LC_DMA_INT_ST_REG;
    volatile uint32_t *cam_int_clr = (volatile uint32_t *)LCD_CAM_LC_DMA_INT_CLR_REG;
    volatile uint32_t *cam_int_ena = (volatile uint32_t *)(LCD_CAM_LC_DMA_INT_ENA_REG);

    // -------------------------------------------------------------------------
    // ШАГ 1: Конфигурация аппаратного тактирования БЕЗ LEDC ШИМ
    // -------------------------------------------------------------------------
    // Настраиваем регистр REG_LCD_CLK (0x0064). 
    // Бит 24 = 1 -> выбираем стабильный источник тактирования PLL_F160M (160 МГц).
    // Биты 14..21 (lcd_clkm_div_num) задают базовый делитель частоты.
    // Биты 0..5 (cam_clkm_div_b) и 6..11 (cam_clkm_div_a) задают точную дробную настройку.
    // Выставляем коэффициенты так, чтобы получить чистые, стабильные 12 МГц на выходе.
    *cam_ctrl = 0; // Сбрасываем старые настройки
    *cam_ctrl |= (3 << LCD_CAM_LCD_CLK_SEL_S); // Включаем PLL_F160M
    *cam_ctrl |= (6 << LCD_CAM_LCD_CLKM_DIV_NUM_S); // Задаем делитель частоты ядра (160 МГц / 13 ~= 12.3 МГц)
	*cam_ctrl |= LCD_CAM_CAM_VS_EOF_EN;
//    *lcd_clk_reg |= (LCD_CAM_LCD_CLKM_DIV_NUM_S); 
    
    // Аппаратно заставляем блок выдавать частоту XCLK непрерывно наружу
//    *cam_ctrl |= (1 << 24); // Включаем бит cam_clk_sel непрерывного вывода тактов

    // -------------------------------------------------------------------------
    // ШАГ 2: Конфигурация сигнальных пинов и привязка к GPIO Matrix
    // -------------------------------------------------------------------------
    // Включаем GPIO 15 (XCLK) в режим обычного вывода
	    // Настраиваем полностью независимую периферию LEDC ШИМ для выдачи 12 МГц на GPIO 15
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_1_BIT, // 1 бит разрешения дает идеальный меандр 50/50
        .freq_hz = 6000000,                 // Стабильные заводские 12 МГц для OV3660
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = CAM_XCLK_IO,             // Наш физический пин GPIO 15
        .duty = 1,                           // Половина от 1-битного таймера (меандр)
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
//    gpio_reset_pin(CAM_XCLK_IO); // Очищаем любые конфликты с LEDC ШИМ!
//    gpio_set_direction(CAM_XCLK_IO, GPIO_MODE_OUTPUT);
//    esp_rom_gpio_connect_out_signal(CAM_XCLK_IO, CAM_CLK_IDX, false, false);

    gpio_reset_pin(CAM_PCLK_IO);
    gpio_set_direction(CAM_PCLK_IO, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal(CAM_PCLK_IO, CAM_PCLK_IDX, false);

    gpio_reset_pin(CAM_HREF_IO);
    gpio_set_direction(CAM_HREF_IO, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal(CAM_HREF_IO, CAM_H_SYNC_IDX, false);

    gpio_reset_pin(CAM_VSYNC_IO);
    gpio_set_direction(CAM_VSYNC_IO, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal(CAM_VSYNC_IO, CAM_V_SYNC_IDX, false);

//    int pins[] = {CAM_D0_IO, CAM_D1_IO, CAM_D2_IO, CAM_D3_IO, CAM_D4_IO, CAM_D5_IO, CAM_D6_IO, CAM_D7_IO};
//    uint64_t data_mask = 0;
//    for(int i=0; i<8; i++) data_mask |= (1ULL << pins[i]);
    
//    gpio_config_t io_conf = {
//        .pin_bit_mask = data_mask | (1ULL << CAM_PCLK_IO) | (1ULL << CAM_HREF_IO) | (1ULL << CAM_VSYNC_IO),
//        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE
//    };
//    gpio_config(&io_conf);

    // === ПРЯМАЯ КОММУТАЦИЯ GPIO MATRIX БЕЗ СБОЕВ ===
    // Направляем внутренний сигнал аппаратного генератора частоты камеры CAM_CLK_OUT_IDX
    // напрямую на нашу физическую ножку GPIO 15! Конфликт LEDC полностью испарился.

    // Подключаем шину данных на вход
//    for (int i = 0; i < 8; i++) {
//        esp_rom_gpio_connect_in_signal(pins[i], CAM_DATA_IN0_IDX + i, false);
//    }
//    esp_rom_gpio_connect_in_signal(CAM_PCLK_IO, CAM_PCLK_IDX, false);
//    esp_rom_gpio_connect_in_signal(CAM_HREF_IO, CAM_H_SYNC_IDX, false);
//    esp_rom_gpio_connect_in_signal(CAM_VSYNC_IO, CAM_V_SYNC_IDX, false);

    // Даем КМОП-генератору камеры 100 мс стабильно потикать новыми аппаратными тактами
    vTaskDelay(pdMS_TO_TICKS(100));

gpio_reset_pin(GPIO_NUM_47);
gpio_set_direction(GPIO_NUM_47, GPIO_MODE_INPUT);
gpio_set_pull_mode(GPIO_NUM_47, GPIO_PULLUP_ONLY); // Включаем подтяжку


    // -------------------------------------------------------------------------
    // ШАГ 3-6: Настройка геометрии и сброс Async FIFO по даташиту
    // -------------------------------------------------------------------------
//    *cam_ctrl1 &= ~(LCD_CAM_CAM_VH_DE_MODE_EN);  // Clear LCD_CAM_CAM_VH_DE_MODE_EN
//    *cam_ctrl1 |= (1 << 13);  // VSYNC активный низкий
//    *cam_ctrl1 &= ~(1 << 14); // HREF активный высокий
//    *cam_ctrl |= (1 << 22);   // Выставляем бит LCD_CAM_CAM_UPDATE

    // Сбрасываем асинхронный буфер и блок LCD_CAM
    *cam_ctrl1 |= LCD_CAM_CAM_RESET;  
	while(*cam_ctrl1 & LCD_CAM_CAM_RESET){
		vTaskDelay(1);
	} 
	*cam_ctrl1 |= LCD_CAM_CAM_AFIFO_RESET;
	while(*cam_ctrl1 & LCD_CAM_CAM_AFIFO_RESET){
		vTaskDelay(1);
	}
    *cam_int_ena |= (LCD_CAM_CAM_VSYNC_INT_ENA); // Включаем флаг CAM_FRM_DONE_INT

    // -------------------------------------------------------------------------
    // ШАГ 7: Настройка 16-битного построчного лимита BYTELEN и GDMA
    // -------------------------------------------------------------------------
	int width = 640;
	int height = 480;
    *cam_ctrl1 &= ~(0xFFFF); 
    *cam_ctrl1 |= (width*2 - 1); // Вшиваем длину одной строки (1600 пикселей * 2 байта)
    *cam_ctrl1 |= (LCD_CAM_CAM_START); // Включаем cam_en (LCD_CAM_CAM_START)
	*cam_ctrl1 |= LCD_CAM_CAM_UPDATE;
i2c_init();
printf("Read 3008 = %u\n", sccb_read(0x3008));

    gdma_channel_alloc_config_t rx_alloc_config = {
        .flags.isr_cache_safe = true
    };
    if (gdma_new_ahb_channel(&rx_alloc_config, &dma_tx_channel, &dma_rx_channel) != ESP_OK) {
        return ESP_FAIL;
    }

    // Нарезаем 3.8 МБ PSRAM на цепочку из 1200 дескрипторов (по 3200 байт на строку)
    size_t node_size = width*2; 
    desc_count = height;       
    dma_desc_list = heap_caps_malloc(desc_count * sizeof(gdma_descr_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    
    for (size_t i = 0; i < desc_count; i++) {
        dma_desc_list[i].dw0.size = node_size;
        dma_desc_list[i].dw0.length = 0;
        dma_desc_list[i].dw0.reversed = 0;
        dma_desc_list[i].dw0.err_out = 0;
        dma_desc_list[i].dw0.owner = 1; 
        dma_desc_list[i].buffer = out_buffer + (i * node_size); // Каждая нода берет свою строку кадра
        dma_desc_list[i].next = (i == desc_count - 1) ? NULL : &dma_desc_list[i + 1];
    }

    gdma_trigger_t trigger = { .instance_id = SOC_GDMA_TRIG_PERIPH_LCD0, .bus_id = 0};
    gdma_connect(dma_rx_channel, trigger);

    // Короткий и чистый запуск линка без лишних структур
    gdma_start(dma_rx_channel, (intptr_t)dma_desc_list);

printf("Read 3008 = %u\n", sccb_read(0x3008));
init_cam();
printf("Read 3008 = %u\n", sccb_read(0x3008));
printf("Temperaure = %u\n", sccb_read(0x6719));
printf("ID %x%x\n", sccb_read(0x300A), sccb_read(0x300B));

    // -------------------------------------------------------------------------
    // ШАГ 8-9: Запуск cam_en и ожидание флага по регистру прерываний
    // -------------------------------------------------------------------------
    ESP_LOGI(TAG, "[!] Аппаратное тактирование выдает 12 МГц. Включаем cam_en...");

    uint32_t safety_timeout = 0;
uint32_t zero_count = 0;
uint32_t one_count = 0;

    ESP_LOGW(TAG, "[!] Начинаем циклическое чтение тестовой петли VSYNC...");

    while ((*cam_int_st & (1 << 1)) == 0) {
        // Читаем физическое состояние перемычки на GPIO 47
        int test_vsync_level = gpio_get_level(GPIO_NUM_47);
        
        if (test_vsync_level == 0) {
            zero_count++;
        } else {
            one_count++;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        safety_timeout += 10;
        
        // Каждые 500 мс выводим в консоль пропорцию нулей и единиц, которые поймал процессор
        if (safety_timeout % 500 == 0) {
            ESP_LOGI(TAG, "[MONITOR] За последние 0.5 сек поймано: НУЛЕЙ=%lu, ЕДИНИЦ=%lu", zero_count, one_count);
            zero_count = 0;
            one_count = 0;
        }

        if (safety_timeout > 4000) { 
            ESP_LOGE(TAG, "[-] Аппаратный тайм-аут GDMA! Линия кадра так и не ожила.");
            gdma_stop(dma_rx_channel);
            gdma_del_channel(dma_rx_channel);
            gdma_del_channel(dma_tx_channel);
            free(dma_desc_list);
			shutdown_cam();
printf("Read 3008 = %u\n", sccb_read(0x3008));
            return ESP_ERR_TIMEOUT;
        }
    }

    // Кадр полностью в буфере! Очищаем регистр флага
    *cam_int_clr |= (1 << LCD_CAM_CAM_VSYNC_INT_CLR); 

    *cam_ctrl &= ~(1 << 21); // Гасим cam_en
    gdma_stop(dma_rx_channel);
    gdma_del_channel(dma_rx_channel);
    gdma_del_channel(dma_tx_channel);
    free(dma_desc_list);

    ESP_LOGW(TAG, "[SUCCESS] Аппаратное построчное сканирование завершено! Кадр UXGA в PSRAM!");
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
}

typedef struct { uint16_t reg; uint8_t val; } reg_val_t;
static const reg_val_t ov3660_uxga_regs[] = {
    // 1. Сброс и пробуждение
//    {0x3008, 0x82}, // Глобальный программный сброс камеры (Software Reset)
    {0x3008, 0x42}, // Включаем внутреннюю цифровую логику
    
    // 2. Настройка тактирования (PLL включен, работает стабильно от ваших 12 МГц)
    {0x303A, 0x00}, // Включаем PLL (Bypass = 0)
    {0x3103, 0x03}, // Родной системный делитель ядра
    
    // 3. Перевод ножек параллельной шины D0-D7 и синхронизации на ВЫВОД данных
    {0x3017, 0xff}, 
    {0x3018, 0xff}, 
    
    // 4. Включаем цифровой процессор камеры (ISP) без блокировок
    {0x501f, 0x01}, // !!! ФИКС: Включаем ISP конвейер (вместо 0x20, который вешал чип)
    {0x4300, 0x32}, // Формат вывода: UYVY Packed
    {0x5002, 0x40}, // Включаем цветовую YUV матрицу
    
    // 5. ГЕОМЕТРИЯ ОКНА: Настраиваем матрицу на разрешение VGA (640x480)
    // Без этих регистров счетчик строк камеры просто стоит на месте!
    {0x3800, 0x00}, {0x3801, 0x00}, // Начало X = 0
    {0x3802, 0x00}, {0x3803, 0x00}, // Начало Y = 0
    {0x3804, 0x02}, {0x3805, 0x80}, // Ширина окна (640 в hex -> 0x0280)
    {0x3806, 0x01}, {0x3807, 0xe0}, // Высота окна (480 в hex -> 0x01E0)
    {0x3808, 0x02}, {0x3809, 0x80}, // Ширина вывода (Output Width = 640)
    {0x380a, 0x01}, {0x380b, 0xe0}, // Высота вывода (Output Height = 480)
    
    {0x4000, 0x05}, // Включаем автоматическую калибровку черного (BLC)
    {0x3815, 0x02}, // Настройка полярности VSYNC/HREF
    
    // 6. ФИНАЛЬНЫЙ СТАРТ
    {0x3008, 0x02}, // Wake up! Запускаем конвейер развертки!
    {0x0000, 0x00}  
};

//const reg_val_t ov3660_uxga_regs[] = {
//    {0x3103, 0x13},
//    {0x3008, 0x42},
//    {0x3017, 0xff},
//    {0x3018, 0xff},
//    {0x302c, 0xc3},
//    {0x4740, 0x21},
//
//    {0x3611, 0x01},
//    {0x3612, 0x2d},
//
//	{0x3032, 0x00},
//    {0x3614, 0x80},
//    {0x3618, 0x00},
//    {0x3619, 0x75},
//    {0x3622, 0x80},
//    {0x3623, 0x00},
//    {0x3624, 0x03},
//    {0x3630, 0x52},
//    {0x3632, 0x07},
//    {0x3633, 0xd2},
//    {0x3704, 0x80},
//    {0x3708, 0x66},
//    {0x3709, 0x12},
//    {0x370b, 0x12},
//    {0x3717, 0x00},
//    {0x371b, 0x60},
//    {0x371c, 0x00},
//    {0x3901, 0x13},
//
//    {0x3600, 0x08},
//    {0x3620, 0x43},
//    {0x3702, 0x20},
//    {0x3739, 0x48},
//    {0x3730, 0x20},
//    {0x370c, 0x0c},
//
//    {0x3a18, 0x00},
//    {0x3a19, 0xf8},
//
//    {0x3000, 0x10},
//    {0x3004, 0xef},
//
//    {0x6700, 0x05},
//    {0x6701, 0x19},
//    {0x6702, 0xfd},
//    {0x6703, 0xd1},
//    {0x6704, 0xff},
//    {0x6705, 0xff},
//
//    {0x3c01, 0x80},
//    {0x3c00, 0x04},
//    {0x3a08, 0x00}, {0x3a09, 0x62}, //50Hz Band Width Step (10bit)
//    {0x3a0e, 0x08}, //50Hz Max Bands in One Frame (6 bit)
//    {0x3a0a, 0x00}, {0x3a0b, 0x52}, //60Hz Band Width Step (10bit)
//    {0x3a0d, 0x09}, //60Hz Max Bands in One Frame (6 bit)
//
//    {0x3a00, 0x3a},//night mode off
//    {0x3a14, 0x09},
//    {0x3a15, 0x30},
//    {0x3a02, 0x09},
//    {0x3a03, 0x30},
//
//    {0x440e, 0x08},
//    {0x4520, 0x0b},
//    {0x460b, 0x37},
//    {0x4713, 0x02},
//    {0x471c, 0xd0},
//    {0x5086, 0x00},
//
//    {0x5002, 0x00},
//    {0x501f, 0x00},
//
//    {0x3008, 0x02},
//
//    {0x5180, 0xff},
//    {0x5181, 0xf2},
//    {0x5182, 0x00},
//    {0x5183, 0x14},
//    {0x5184, 0x25},
//    {0x5185, 0x24},
//    {0x5186, 0x16},
//    {0x5187, 0x16},
//    {0x5188, 0x16},
//    {0x5189, 0x68},
//    {0x518a, 0x60},
//    {0x518b, 0xe0},
//    {0x518c, 0xb2},
//    {0x518d, 0x42},
//    {0x518e, 0x35},
//    {0x518f, 0x56},
//    {0x5190, 0x56},
//    {0x5191, 0xf8},
//    {0x5192, 0x04},
//    {0x5193, 0x70},
//    {0x5194, 0xf0},
//    {0x5195, 0xf0},
//    {0x5196, 0x03},
//    {0x5197, 0x01},
//    {0x5198, 0x04},
//    {0x5199, 0x12},
//    {0x519a, 0x04},
//    {0x519b, 0x00},
//    {0x519c, 0x06},
//    {0x519d, 0x82},
//    {0x519e, 0x38},
//
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
//    {0x5480, 0x01},
////    {0x5481, 0x05},
////    {0x5482, 0x09},
////    {0x5483, 0x10},
////    {0x5484, 0x3a},
////    {0x5485, 0x4c},
////    {0x5486, 0x5a},
////    {0x5487, 0x68},
////    {0x5488, 0x74},
////    {0x5489, 0x80},
////    {0x548a, 0x8e},
////    {0x548b, 0xa4},
////    {0x548c, 0xb4},
////    {0x548d, 0xc8},
////    {0x548e, 0xde},
////    {0x548f, 0xf0},
////    {0x5490, 0x15},
//
//    {0x5000, 0xa7},
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
//    {0x583d, 0xCF},
//
//    {0x3a0f, 0x30},
//    {0x3a10, 0x28},
//    {0x3a1b, 0x30},
//    {0x3a1e, 0x28},
//    {0x3a11, 0x60},
//    {0x3a1f, 0x14},
//
//    {0x5302, 0x28},
//    {0x5303, 0x20},
//
//    {0x5306, 0x1c}, //de-noise offset 1
//    {0x5307, 0x28}, //de-noise offset 2
//
//    {0x4002, 0xc5},
//    {0x4003, 0x81},
//    {0x4005, 0x12},
//
//    {0x5688, 0x11},
//    {0x5689, 0x11},
//    {0x568a, 0x11},
//    {0x568b, 0x11},
//    {0x568c, 0x11},
//    {0x568d, 0x11},
//    {0x568e, 0x11},
//    {0x568f, 0x11},
//
//    {0x5580, 0x06},
//    {0x5588, 0x00},
//    {0x5583, 0x40},
//    {0x5584, 0x2c},
//
//    {0x5001, 0x83}, // turn color matrix, awb and SDE
//
//    // Маркер окончания массива конфигурации
//    {0x0000, 0x00}
//};

void init_cam()
{
//enable_cam();
//vTaskDelay(pdMS_TO_TICKS(10));
reset_cam();
vTaskDelay(pdMS_TO_TICKS(10));
    int idx = 0;
    while (ov3660_uxga_regs[idx].reg != 0x0000) {
//        sccb_write(ov3660_uxga_regs[idx].reg, ov3660_uxga_regs[idx].val);
        idx++;
    }
}
