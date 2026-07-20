#include "airflow_sensor.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "AIRFLOW";

#define AIRFLOW_GPIO GPIO_NUM_4

bool airflow_sensor_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(AIRFLOW_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "Airflow sensor ready (GPIO4, digital)");
    return true;
}

bool airflow_sensor_read(bool *has_airflow)
{
    if (!has_airflow) return false;
    *has_airflow = (gpio_get_level(AIRFLOW_GPIO) == 0);
    return true;
}
