#include "sensor_manager.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "esp_adc/adc_oneshot.h"

static const char *TAG = "SENSOR_MANAGER";

/*
 * 当前接线：
 * 光照 AO   -> GPIO20 -> ADC1_CHANNEL_4
 * 雨滴 AO   -> GPIO21 -> ADC1_CHANNEL_5
 * 热敏 AO   -> GPIO22 -> ADC1_CHANNEL_6
 */
#define ADC_UNIT_USED        ADC_UNIT_1

#define LIGHT_ADC_CHANNEL    ADC_CHANNEL_4
#define RAIN_ADC_CHANNEL     ADC_CHANNEL_5
#define THERMAL_ADC_CHANNEL  ADC_CHANNEL_6

#define ADC_RAW_MAX          4095.0f

/*
 * 雨滴判断：
 * 当前 rain_raw 大多数在 10~15，偶尔掉到 0~5。
 * 所以先用 raw 阈值直接判断是否下雨。
 *
 * rain_raw <= 5 认为检测到雨滴；
 * rain_raw > 5 认为没有下雨。
 */
#define RAIN_RAW_THRESHOLD   5

/*
 * 热敏模块暂时还没精确标定。
 * 当前先按 raw 越大表示越热处理。
 */
#define THERMAL_RAW_COLD     300.0f
#define THERMAL_RAW_HOT      3400.0f
#define THERMAL_OVERHEAT_PERCENT 70.0f

static adc_oneshot_unit_handle_t s_adc1_handle = NULL;

static float clamp_percent(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }

    if (value > 100.0f) {
        return 100.0f;
    }

    return value;
}

static float map_percent_rising(float raw, float raw_min, float raw_max)
{
    if (raw_max == raw_min) {
        return 0.0f;
    }

    float percent = (raw - raw_min) * 100.0f / (raw_max - raw_min);
    return clamp_percent(percent);
}

static esp_err_t config_adc_channel(adc_channel_t channel)
{
    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };

    return adc_oneshot_config_channel(
        s_adc1_handle,
        channel,
        &channel_config
    );
}

esp_err_t sensors_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_USED,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t ret = adc_oneshot_new_unit(&init_config, &s_adc1_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC unit: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = config_adc_channel(LIGHT_ADC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config light ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = config_adc_channel(RAIN_ADC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config rain ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = config_adc_channel(THERMAL_ADC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config thermal ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Sensor ADC initialized: light=GPIO20, rain=GPIO21, thermal=GPIO22");

    return ESP_OK;
}

esp_err_t sensors_read(sensor_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int light_raw = 0;
    int rain_raw = 0;
    int thermal_raw = 0;

    esp_err_t ret = adc_oneshot_read(
        s_adc1_handle,
        LIGHT_ADC_CHANNEL,
        &light_raw
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read light ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = adc_oneshot_read(
        s_adc1_handle,
        RAIN_ADC_CHANNEL,
        &rain_raw
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read rain ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = adc_oneshot_read(
        s_adc1_handle,
        THERMAL_ADC_CHANNEL,
        &thermal_raw
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read thermal ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    data->timestamp_ms = esp_timer_get_time() / 1000;

    /*
     * 光照：
     * 当前先按 ADC 原始比例换算成百分比。
     * JSON 里仍然输出 light_raw。
     */
    data->light.raw = light_raw;
    data->light.brightness_percent = clamp_percent(
        (light_raw / ADC_RAW_MAX) * 100.0f
    );
    data->light.valid = true;

    /*
     * 雨滴：
     * 不再使用 3400/300 的湿润度映射。
     * 现在直接用 rain_raw 判断是否下雨。
     */
    data->rain.raw = rain_raw;
    data->rain.detected = (rain_raw <= RAIN_RAW_THRESHOLD);
    data->rain.wetness_percent = data->rain.detected ? 100.0f : 0.0f;
    data->rain.valid = true;

    /*
     * 热敏：
     * 暂时按 raw 越大表示越热处理。
     * 后面如果换成 SHT30/SHT31，这里可以整体替换成真实温度。
     */
    data->thermal.raw = thermal_raw;
    data->thermal.heat_percent = map_percent_rising(
        (float)thermal_raw,
        THERMAL_RAW_COLD,
        THERMAL_RAW_HOT
    );
    data->thermal.overheated = data->thermal.heat_percent >= THERMAL_OVERHEAT_PERCENT;
    data->thermal.valid = true;

    return ESP_OK;
}

esp_err_t sensors_data_to_json(
    const sensor_data_t *data,
    char *json_buffer,
    size_t buffer_size
)
{
    if (data == NULL || json_buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * temp:
     * 当前热敏模块还没有精确标定，先输出 20~40°C 的演示值。
     */
    int temp = 20 + (int)(data->thermal.heat_percent * 0.2f + 0.5f);

    /*
     * rain:
     * true  表示检测到下雨/雨滴板有水
     * false 表示未检测到下雨
     */
    const char *rain = data->rain.detected ? "true" : "false";

    /*
     * light:
     * 当前继续输出光照 ADC 原始值。
     */
    int light = data->light.raw;

    int written = snprintf(
        json_buffer,
        buffer_size,
        "{"
            "\"temp\":%d,"
            "\"rain\":%s,"
            "\"light\":%d"
        "}",
        temp,
        rain,
        light
    );

    if (written < 0 || written >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t sensors_get_json(
    char *json_buffer,
    size_t buffer_size
)
{
    sensor_data_t data;

    esp_err_t ret = sensors_read(&data);
    if (ret != ESP_OK) {
        return ret;
    }

    return sensors_data_to_json(&data, json_buffer, buffer_size);
}