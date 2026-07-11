#include "i2c_bus.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static const char *TAG = "I2C_BUS";

#define I2C_BUS_SDA     GPIO_NUM_7
#define I2C_BUS_SCL     GPIO_NUM_8

static i2c_master_bus_handle_t s_bus_handle = NULL;

esp_err_t i2c_bus_init(void)
{
    if (s_bus_handle) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,  /* auto-select */
        .sda_io_num = I2C_BUS_SDA,
        .scl_io_num = I2C_BUS_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d)", I2C_BUS_SDA, I2C_BUS_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_bus_handle;
}

void i2c_bus_recover(void)
{
    if (!s_bus_handle) {
        ESP_LOGW(TAG, "No bus to recover");
        return;
    }

    ESP_LOGI(TAG, "Recovering I2C bus (GPIO unstick, preserving bus handle)...");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2C_BUS_SDA) | (1ULL << I2C_BUS_SCL),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(I2C_BUS_SCL, 1);
    esp_rom_delay_us(10);
    if (gpio_get_level(I2C_BUS_SDA) == 0) {
        ESP_LOGW(TAG, "SDA stuck low, clocking to recover...");
        for (int i = 0; i < 10; i++) {
            gpio_set_level(I2C_BUS_SCL, 0);
            esp_rom_delay_us(10);
            gpio_set_level(I2C_BUS_SCL, 1);
            esp_rom_delay_us(10);
            if (gpio_get_level(I2C_BUS_SDA) == 1) {
                ESP_LOGI(TAG, "SDA released after %d clock pulses", i + 1);
                break;
            }
        }
    }

    /* STOP condition */
    gpio_set_level(I2C_BUS_SDA, 0);
    esp_rom_delay_us(10);
    gpio_set_level(I2C_BUS_SCL, 1);
    esp_rom_delay_us(10);
    gpio_set_level(I2C_BUS_SDA, 1);
    esp_rom_delay_us(10);

    ESP_LOGI(TAG, "I2C bus recovery complete (bus handle preserved)");
}
