#include "sht3x.h"
#include "i2c_bus.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SHT3X";

static bool                     s_ready = false;
static i2c_master_dev_handle_t  s_dev   = NULL;

static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int j = 0; j < len; j++) {
        crc ^= data[j];
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
    }
    return crc;
}

bool sht3x_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return false;
    }

    uint8_t addrs[] = {0x44, 0x45};

    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = addrs[i],
            .scl_speed_hz    = 100000,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
            continue;
        }

        /* Probe: send soft reset command, check for ACK */
        uint8_t reset_cmd[2] = {0x30, 0xA2};
        esp_err_t ret = i2c_master_transmit(dev, reset_cmd, 2, pdMS_TO_TICKS(100));
        if (ret == ESP_OK) {
            s_dev = dev;
            s_ready = true;
            vTaskDelay(pdMS_TO_TICKS(2));
            ESP_LOGI(TAG, "SHT3x found at 0x%02X", addrs[i]);
            break;
        }
        /* Wrong address, remove device */
        i2c_master_bus_rm_device(dev);
    }

    if (!s_ready) {
        ESP_LOGW(TAG, "SHT3x not found (0x44/0x45 both unresponsive)");
        return false;
    }

    ESP_LOGI(TAG, "SHT3x ready");
    return true;
}

bool sht3x_read(sht3x_data_t *out)
{
    if (!s_ready || !s_dev) return false;

    /* Trigger single-shot measurement, high repeatability */
    uint8_t cmd[2] = {0x2C, 0x06};
    esp_err_t ret = i2c_master_transmit(s_dev, cmd, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return false;

    vTaskDelay(pdMS_TO_TICKS(20));

    /* Read 6 bytes: T(2) + CRC + RH(2) + CRC */
    uint8_t buf[6] = {0};
    ret = i2c_master_receive(s_dev, buf, 6, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return false;

    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
        ESP_LOGW(TAG, "CRC check failed");
        return false;
    }

    uint16_t t_raw = (buf[0] << 8) | buf[1];
    uint16_t h_raw = (buf[3] << 8) | buf[4];

    out->temperature = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    out->humidity    = 100.0f * (float)h_raw / 65535.0f;

    return true;
}
