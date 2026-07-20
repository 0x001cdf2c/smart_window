#include "sht3x.h"
#include "i2c_bus.h"
#include "soft_i2c.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SHT3X";

/* Indoor: hardware I2C bus (GPIO7/8) */
static bool                     s_hw_ready = false;
static i2c_master_dev_handle_t  s_hw_dev   = NULL;

/* Outdoor: software I2C (GPIO5=SDA, GPIO6=SCL) — same 0x44 addr, separate bus */
static soft_i2c_t s_sw_bus;
static bool       s_sw_ready = false;
#define SW_SDA  5
#define SW_SCL  6
#define SHT3X_ADDR  0x44

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
    /* ── Indoor: hardware I2C ── */
    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (bus) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = SHT3X_ADDR,
            .scl_speed_hz    = 100000,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &s_hw_dev) == ESP_OK) {
            uint8_t reset_cmd[2] = {0x30, 0xA2};
            if (i2c_master_transmit(s_hw_dev, reset_cmd, 2, pdMS_TO_TICKS(100)) == ESP_OK) {
                s_hw_ready = true;
                vTaskDelay(pdMS_TO_TICKS(2));
                ESP_LOGI(TAG, "室内 SHT3x at 0x%02X (HW I2C GPIO7/8)", SHT3X_ADDR);
            } else {
                i2c_master_bus_rm_device(s_hw_dev);
                s_hw_dev = NULL;
            }
        }
    }

    /* ── Outdoor: software I2C (separate GPIO5/6) ── */
    soft_i2c_init(&s_sw_bus, SW_SDA, SW_SCL);
    uint8_t reset_cmd[2] = {0x30, 0xA2};
    if (soft_i2c_write(SHT3X_ADDR, reset_cmd, 2)) {
        s_sw_ready = true;
        vTaskDelay(pdMS_TO_TICKS(2));
        ESP_LOGI(TAG, "室外 SHT3x at 0x%02X (SW I2C GPIO%d/%d)", SHT3X_ADDR, SW_SDA, SW_SCL);
    } else {
        ESP_LOGW(TAG, "室外 SHT3x not found on GPIO%d/%d", SW_SDA, SW_SCL);
    }

    ESP_LOGI(TAG, "SHT3x: 室内=%s 室外=%s",
             s_hw_ready ? "OK" : "NO", s_sw_ready ? "OK" : "NO");
    return s_hw_ready || s_sw_ready;
}

static bool read_hw(sht3x_data_t *out)
{
    if (!s_hw_ready || !out || !s_hw_dev) return false;

    uint8_t cmd[2] = {0x2C, 0x06};
    if (i2c_master_transmit(s_hw_dev, cmd, 2, pdMS_TO_TICKS(100)) != ESP_OK) return false;

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t buf[6] = {0};
    if (i2c_master_receive(s_hw_dev, buf, 6, pdMS_TO_TICKS(100)) != ESP_OK) return false;

    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) return false;

    uint16_t t_raw = (buf[0] << 8) | buf[1];
    uint16_t h_raw = (buf[3] << 8) | buf[4];
    out->temperature = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    out->humidity    = 100.0f * (float)h_raw / 65535.0f;
    return true;
}

static bool read_sw(sht3x_data_t *out)
{
    if (!s_sw_ready || !out) return false;

    uint8_t cmd[2] = {0x2C, 0x06};
    if (!soft_i2c_write(SHT3X_ADDR, cmd, 2)) return false;

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t buf[6] = {0};
    if (!soft_i2c_read(SHT3X_ADDR, buf, 6)) return false;

    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) return false;

    uint16_t t_raw = (buf[0] << 8) | buf[1];
    uint16_t h_raw = (buf[3] << 8) | buf[4];
    out->temperature = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    out->humidity    = 100.0f * (float)h_raw / 65535.0f;
    return true;
}

bool sht3x_read(sht3x_slot_t slot, sht3x_data_t *out)
{
    if (slot == SHT3X_INDOOR)  return read_hw(out);
    if (slot == SHT3X_OUTDOOR) return read_sw(out);
    return false;
}
