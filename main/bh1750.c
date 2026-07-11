#include "bh1750.h"
#include "i2c_bus.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BH1750";

#define BH1750_ADDR    0x23

/* Commands */
#define BH1750_POWER_ON   0x01
#define BH1750_RESET      0x07
#define BH1750_ONE_TIME_H 0x20  /* Single-shot high-res, 1lx, 120ms */

static bool                     s_ready = false;
static i2c_master_dev_handle_t  s_dev   = NULL;

bool bh1750_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return false;
    }

    /* Add device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BH1750_ADDR,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add BH1750 device");
        return false;
    }

    /* Power on */
    uint8_t pwr = BH1750_POWER_ON;
    if (i2c_master_transmit(s_dev, &pwr, 1, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 power-on failed");
        return false;
    }

    /* Reset */
    uint8_t rst = BH1750_RESET;
    i2c_master_transmit(s_dev, &rst, 1, pdMS_TO_TICKS(100));

    s_ready = true;
    ESP_LOGI(TAG, "BH1750 ready (0x23, single-shot high-res mode)");
    return true;
}

bool bh1750_read(float *lux)
{
    if (!s_ready || !s_dev) return false;

    /* Trigger single-shot high-res measurement */
    uint8_t cmd = BH1750_ONE_TIME_H;
    if (i2c_master_transmit(s_dev, &cmd, 1, pdMS_TO_TICKS(100)) != ESP_OK)
        return false;

    vTaskDelay(pdMS_TO_TICKS(130)); /* High-res mode needs 120ms */

    /* Read 2 bytes */
    uint8_t buf[2] = {0};
    if (i2c_master_receive(s_dev, buf, 2, pdMS_TO_TICKS(100)) != ESP_OK)
        return false;

    *lux = (float)((buf[0] << 8) | buf[1]) / 1.2f;
    return true;
}
