#include "sht3x.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SHT3X";

#define SHT3X_I2C_PORT I2C_NUM_0

static bool    s_ready = false;
static uint8_t s_addr  = 0;

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
    /* 1. 先探测 SHT3x, 避免先扫 0x23 (BH1750) 把它扰乱了 */
    ESP_LOGI(TAG, "探测 SHT3x (0x44/0x45)...");
    uint8_t addrs[] = {0x44, 0x45};
    esp_err_t ret;

    for (int i = 0; i < 2; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addrs[i] << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(SHT3X_I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            /* 软复位 */
            cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (addrs[i] << 1) | I2C_MASTER_WRITE, true);
            i2c_master_write_byte(cmd, 0x30, true);
            i2c_master_write_byte(cmd, 0xA2, true);
            i2c_master_stop(cmd);
            i2c_master_cmd_begin(SHT3X_I2C_PORT, cmd, pdMS_TO_TICKS(50));
            i2c_cmd_link_delete(cmd);
            vTaskDelay(pdMS_TO_TICKS(2));

            s_addr = addrs[i];
            s_ready = true;
            ESP_LOGI(TAG, "SHT3x 在 0x%02X 就绪", s_addr);
        } else {
            ESP_LOGW(TAG, "0x%02X 无响应: %s", addrs[i], esp_err_to_name(ret));
        }
    }

    /* 2. 全总线扫描 (仅做信息输出) */
    ESP_LOGI(TAG, "I2C 总线扫描 ...");
    int detected = 0;
    for (uint8_t addr = 3; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(SHT3X_I2C_PORT, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  发现: 0x%02X", addr);
            detected++;
        }
    }
    ESP_LOGI(TAG, "扫描完成, 共 %d 个设备", detected);

    if (!s_ready) {
        ESP_LOGE(TAG, "SHT3x 未找到 (0x44/0x45 均无响应)");
    }
    return s_ready;
}

bool sht3x_read(sht3x_data_t *out)
{
    if (!s_ready || !s_addr) return false;

    /* Trigger single-shot, high repeatability */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x2C, true);
    i2c_master_write_byte(cmd, 0x06, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(SHT3X_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return false;

    vTaskDelay(pdMS_TO_TICKS(20)); /* 15ms measurement time */

    uint8_t buf[6] = {0};
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, 6, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(SHT3X_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return false;

    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
        ESP_LOGW(TAG, "CRC 校验失败");
        return false;
    }

    uint16_t t_raw = (buf[0] << 8) | buf[1];
    uint16_t h_raw = (buf[3] << 8) | buf[4];

    out->temperature = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    out->humidity    = 100.0f * (float)h_raw / 65535.0f;

    return true;
}
