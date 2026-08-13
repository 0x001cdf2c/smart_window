#include "airflow_sensor.h"
#include "adc_ads1115.h"
#include "esp_log.h"

static const char *TAG = "AIRFLOW";

#define AIRFLOW_ADC_CHANNEL  2       /* ADS1115 AIN2 */
#define AIRFLOW_THRESHOLD    5       /* 0.625mV, 极微弱电压即判有风 */
#define AIRFLOW_RAW_MAX      16      /* 满量程: raw=16 (~2mV)=100%, raw=8=50% */
#define AIRFLOW_GAIN         1       /* 增益 (1=原始) */

static bool s_ready = false;
static int16_t s_last_raw = 0;

bool airflow_sensor_init(void)
{
    s_ready = true;
    ESP_LOGI(TAG, "Airflow ready (ADS1115 AIN2, threshold=%d, max=%d, gain=%d)",
             AIRFLOW_THRESHOLD, AIRFLOW_RAW_MAX, AIRFLOW_GAIN);
    return true;
}

bool airflow_sensor_read(bool *has_airflow)
{
    if (!s_ready || !has_airflow) return false;

    /* 多次采样取最大值 (马达输出电压可能波动) */
    int16_t best = 0;
    for (int i = 0; i < 5; i++) {
        int16_t raw = 0;
        if (ads1115_read_channel(AIRFLOW_ADC_CHANNEL, &raw)) {
            if (raw < 0) raw = 0;
            if (raw > best) best = raw;
        }
        esp_rom_delay_us(2000);
    }

    s_last_raw = best;
    bool wind = (best >= AIRFLOW_THRESHOLD);
    int pct = best * 100 * AIRFLOW_GAIN / AIRFLOW_RAW_MAX;
    if (pct > 100) pct = 100;

    /* 每次都打印, 方便校准阈值 */
    ESP_LOGI(TAG, "AIN2 raw=%d pct=%d%% → %s", (int)best, pct,
             wind ? "有风" : "无风");

    *has_airflow = wind;
    return true;
}

int16_t airflow_sensor_read_raw(void)
{
    int16_t raw = 0;
    ads1115_read_channel(AIRFLOW_ADC_CHANNEL, &raw);
    if (raw < 0) raw = 0;
    s_last_raw = raw;
    return raw;
}

int airflow_sensor_read_pct(void)
{
    int16_t raw = 0;
    if (!ads1115_read_channel(AIRFLOW_ADC_CHANNEL, &raw)) return 0;
    if (raw < 0) raw = 0;
    s_last_raw = raw;

    int pct = (int)raw * 100 * AIRFLOW_GAIN / AIRFLOW_RAW_MAX;
    if (pct > 100) pct = 100;
    ESP_LOGI(TAG, "AIN2 raw=%d → %d%%", (int)raw, pct);
    return pct;
}
