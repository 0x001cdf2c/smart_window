#include "smoke_sensor.h"
#include "adc_ads1115.h"
#include "esp_log.h"

static const char *TAG = "SMOKE";

#define SMOKE_ADC_CHANNEL  1       /* ADS1115 AIN1 */
#define SMOKE_ADC_MAX      26400   /* 3.3V at ±4.096V PGA */

static bool s_ready = false;

bool smoke_sensor_init(void)
{
    ESP_LOGI(TAG, "Smoke sensor ready (ADS1115 AIN1)");
    s_ready = true;
    return true;
}

bool smoke_sensor_read(int *air_quality_pct)
{
    if (!s_ready || !air_quality_pct) return false;

    int16_t raw = 0;
    if (!ads1115_read_channel(SMOKE_ADC_CHANNEL, &raw)) {
        ESP_LOGW(TAG, "ADS1115 AIN1 read failed");
        return false;
    }

    if (raw < 0) raw = 0;
    if (raw > SMOKE_ADC_MAX) raw = SMOKE_ADC_MAX;
    *air_quality_pct = raw * 100 / SMOKE_ADC_MAX;
    ESP_LOGI(TAG, "AIN1 raw=%d  pct=%d", (int)raw, *air_quality_pct);
    return true;
}
