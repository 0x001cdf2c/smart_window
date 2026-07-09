#include "bh1750.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BH1750";

#define BH1750_I2C_PORT I2C_NUM_0
#define BH1750_ADDR    0x23

/* 命令 */
#define BH1750_POWER_ON   0x01
#define BH1750_POWER_OFF  0x00
#define BH1750_RESET      0x07
#define BH1750_ONE_TIME_H 0x20  /* 单次高精度, 1lx, 120ms */

static bool s_ready = false;

bool bh1750_init(void)
{
    /* 上电 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, BH1750_POWER_ON, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BH1750_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 上电失败");
        return false;
    }

    /* 复位 */
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, BH1750_RESET, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(BH1750_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);

    s_ready = true;
    ESP_LOGI(TAG, "BH1750 就绪 (0x23, 单次高精度模式)");
    return true;
}

bool bh1750_read(float *lux)
{
    if (!s_ready) return false;

    /* 触发单次高精度测量 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, BH1750_ONE_TIME_H, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(BH1750_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return false;

    vTaskDelay(pdMS_TO_TICKS(130)); /* 高精度模式需 120ms */

    /* 读 2 字节 */
    uint8_t buf[2] = {0};
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BH1750_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, 2, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(BH1750_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return false;

    *lux = (float)((buf[0] << 8) | buf[1]) / 1.2f;
    return true;
}
