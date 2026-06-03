#include "esp_adc/adc_oneshot.h" // Нужен для чтения заряда аккумулятора
#include "driver/gpio.h"

// Функция чтения напряжения батареи с пина GPIO 1
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

uint32_t read_battery_millivolts(void) {
    int adc_raw = 0;
    int voltage_mv = 0; 
    
    // Сбрасываем пин GPIO 14 в исходное чистое состояние
    gpio_reset_pin(GPIO_NUM_1);
    gpio_set_pull_mode(GPIO_NUM_1, GPIO_FLOATING); // Полностью отключаем внутренние подтяжки чипа
    
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&init_config, &adc1_handle) != ESP_OK) return 0;
    
    // Настраиваем диапазон до 2.6V (аттенюация 12dB)
    adc_oneshot_chan_cfg_t config = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
    
    // КРИТИЧЕСКИЙ ПЕРЕНОС: GPIO 14 — это аппаратно ADC1_CHANNEL_13!
    adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config);
    
    // Читаем чистый сигнал с 13-го канала
    adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &adc_raw); 

    // 2. Включаем встроенную аппаратную калибровку чипа ESP32-S3
    adc_cali_handle_t cali_handle = NULL;
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    // Если калибровочная таблица успешно создана — пересчитываем сырые попугаи в реальные милливольты
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
        adc_cali_raw_to_voltage(cali_handle, adc_raw, &voltage_mv);
        adc_cali_delete_scheme_curve_fitting(cali_handle); // Чистим память
    }
    
    adc_oneshot_del_unit(adc1_handle);

    // 3. Масштабируем с учетом вашего делителя напряжения!
    // Если резисторы по 33 кОм (делят ровно на 2), умножаем voltage_mv на 2
    uint32_t real_battery_mv = (uint32_t)voltage_mv * 2;
    
    // Если провод не припаян или шумит, возвращаем 0
    if (real_battery_mv < 2000) return 0; 
    return real_battery_mv;
}

