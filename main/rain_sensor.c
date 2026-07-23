#include "rain_sensor.h"
#include "adc_ads1115.h"
#include "esp_log.h"

static const char *TAG = "RAIN";

#define RAIN_ADC_CHANNEL  0       /* ADS1115 AIN0 */
#define RAIN_ADC_FS       32767   /* ±4.096V PGA full-scale positive */

static bool s_ready = false;

bool rain_sensor_init(void)
{
    ESP_LOGI(TAG, "Rain sensor ready (ADS1115 AIN0)");
    s_ready = true;
    return true;
}

bool rain_sensor_read(int *rain_pct)
{
    if (!s_ready || !rain_pct) return false;

    int16_t raw = 0;
    if (!ads1115_read_channel(RAIN_ADC_CHANNEL, &raw)) {
        ESP_LOGW(TAG, "ADS1115 AIN0 read failed");
        return false;
    }

    if (raw < 0) raw = 0;

    /* dry=high voltage (~3.8V → ~30400), wet=low voltage → ~0 */
    *rain_pct = 100 - (raw * 100 / RAIN_ADC_FS);
    if (*rain_pct < 0) *rain_pct = 0;
    ESP_LOGI(TAG, "AIN0 raw=%d pct=%d", (int)raw, *rain_pct);
    return true;
}
