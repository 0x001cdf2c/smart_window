#include "adc_ads1115.h"
#include "i2c_bus.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ADS1115";

#define ADS1115_ADDR  0x48

#define REG_CONVERSION  0x00
#define REG_CONFIG      0x01

#define OS_SINGLE       (1 << 15)
#define AIN0_GND  0x04
#define AIN1_GND  0x05
#define PGA_4096  (0x01 << 9)  /* 001 = ±4.096V */
#define MODE_SINGLE  (1 << 8)
#define DR_128SPS  (0x07 << 5)
#define COMP_DISABLE  (0x03 << 0)

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_ready = false;

bool ads1115_is_ready(void)
{
    return s_ready;
}

bool ads1115_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not available");
        return false;
    }

    /* Probe with timeout, retry on failure */
    bool found = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        esp_err_t probe_ret = i2c_master_probe(bus, ADS1115_ADDR, pdMS_TO_TICKS(100));
        if (probe_ret == ESP_OK) {
            found = true;
            ESP_LOGI(TAG, "Probe OK at 0x%02X (attempt %d)", ADS1115_ADDR, attempt + 1);
            break;
        }
        ESP_LOGW(TAG, "Probe attempt %d/3 at 0x%02X: %s", attempt + 1, ADS1115_ADDR,
                 esp_err_to_name(probe_ret));
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!found) {
        ESP_LOGE(TAG, "ADS1115 not found at 0x%02X after 3 attempts — "
                 "check VCC (3.3V), GND, SDA→GPIO7, SCL→GPIO8, ADDR→GND",
                 ADS1115_ADDR);
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADS1115_ADDR,
        .scl_speed_hz    = 50000,   /* slower = more reliable on shared bus */
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "Device add failed");
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ADS1115 ready at 0x%02X (AIN0=rain, AIN1=smoke, AIN2=airflow)", ADS1115_ADDR);
    return true;
}

bool ads1115_read_channel(int channel, int16_t *raw_out)
{
    if (!s_ready || !s_dev || !raw_out) return false;

    const uint16_t mux_map[] = { AIN0_GND, AIN1_GND, 0x06, 0x07 };
    if (channel < 0 || channel > 3) return false;

    uint16_t config = OS_SINGLE | (mux_map[channel] << 12)
                    | PGA_4096 | MODE_SINGLE | DR_128SPS | COMP_DISABLE;

    uint8_t cfg_buf[3] = { REG_CONFIG, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF) };

    esp_err_t ret = i2c_master_transmit(s_dev, cfg_buf, 3, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Config write ch%d: %s", channel, esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(12));

    uint8_t reg = REG_CONVERSION;
    uint8_t data[2] = {0};
    ret = i2c_master_transmit_receive(s_dev, &reg, 1, data, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read ch%d: %s", channel, esp_err_to_name(ret));
        return false;
    }

    *raw_out = (int16_t)((data[0] << 8) | data[1]);
    return true;
}
