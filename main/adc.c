#include "esp_adc/adc_oneshot.h" // Нужен для чтения заряда аккумулятора

// Функция чтения напряжения батареи с пина GPIO 1
uint32_t read_battery_millivolts(void) 
{
    // Делитель напряжения делит 4.2V на 2, значит на АЦП придет максимум 2.1V.
    // ESP32-S3 настраиваем на диапазон до 2.5V (аттенюация 11dB или 12dB в зависимости от ревизии IDF)
    int adc_raw = 0;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
    
    if (adc_oneshot_new_unit(&init_config, &adc1_handle) == ESP_OK) {
        adc_oneshot_chan_cfg_t config = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12, // Диапазон до ~2.6V
        };
        adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config); // GPIO 1 — это ADC1_CH0
        adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &adc_raw);
        adc_oneshot_del_unit(adc1_handle);
    }
    
    // Масштабируем: 12-битное АЦП (0-4095) преобразуем в милливольты. 
    // Напряжение на пине = (adc_raw * 2600) / 4095. Умножаем на 2 из-за резистивного делителя.
    uint32_t mv = ((uint32_t)adc_raw * 2600 * 2) / 4095;
    
    // Защита от шума: если провод не подключен, функция вернет 0
    if (mv < 2000) return 0; 
    return mv;
}
