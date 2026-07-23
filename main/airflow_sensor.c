#include "airflow_sensor.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "AIRFLOW";

#define AIRFLOW_GPIO GPIO_NUM_48

bool airflow_sensor_init(void)
{
    /* No internal pulls — rely on sensor's own output driver.
       Many airflow sensor modules have built-in pull-ups and
       adding the ESP internal ~45k pull-up can create a divider
       that prevents the pin from crossing the logic threshold. */
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(AIRFLOW_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* Rapid-poll for 100ms at boot to catch the initial state */
    int high_cnt = 0, low_cnt = 0;
    int64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < 100000) {
        if (gpio_get_level(AIRFLOW_GPIO)) high_cnt++; else low_cnt++;
    }
    ESP_LOGI(TAG, "GPIO48 boot scan: HIGH=%d LOW=%d → %s",
             high_cnt, low_cnt, low_cnt > 0 ? "有风" : "无风");

    return true;
}

bool airflow_sensor_read(bool *has_airflow)
{
    if (!has_airflow) return false;

    /* Read 3 times with 1ms gap, take majority to debounce */
    int low = 0;
    for (int i = 0; i < 3; i++) {
        if (gpio_get_level(AIRFLOW_GPIO) == 0) low++;
        if (i < 2) esp_rom_delay_us(1000);
    }

    bool wind = (low >= 2);  /* majority vote for LOW = wind */
    static bool last_wind = false;
    if (wind != last_wind) {
        ESP_LOGI(TAG, "GPIO48 changed: %s → %s",
                 last_wind ? "有风" : "无风", wind ? "有风" : "无风");
        last_wind = wind;
    }
    *has_airflow = wind;
    return true;
}
