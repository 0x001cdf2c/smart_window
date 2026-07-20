#include "smoke_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "SMOKE";

#define SMOKE_ADC_UNIT   ADC_UNIT_2
#define SMOKE_ADC_CHAN   ADC_CHANNEL_4   /* GPIO53 = ADC2_CH4 */
#define SMOKE_ADC_ATTEN  ADC_ATTEN_DB_12

static adc_oneshot_unit_handle_t s_adc = NULL;
static bool s_ready = false;

bool smoke_sensor_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = SMOKE_ADC_UNIT,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed");
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = SMOKE_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, SMOKE_ADC_CHAN, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed");
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Smoke sensor ready (ADC2_CH4, GPIO53)");
    return true;
}

bool smoke_sensor_read(int *air_quality_pct)
{
    if (!s_ready || !air_quality_pct) return false;

    int raw = 0;
    adc_oneshot_read(s_adc, SMOKE_ADC_CHAN, &raw);

    /* MQ-2: higher voltage = more smoke. 0% = clean, 100% = heavy smoke. */
    *air_quality_pct = raw * 100 / 4095;
    return true;
}

adc_oneshot_unit_handle_t smoke_sensor_get_adc_handle(void)
{
    return s_adc;
}
