#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensor_manager.h"
#include "servo_manager.h"
#include "system_types.h"

static const char *TAG = "SMART_LOUVER_P4";

static float decide_louver_angle(const sensor_data_t *data)
{
    if (data == NULL) {
        return 90.0f;
    }

    /*
     * 优先级 1：检测到下雨，关闭百叶窗
     */
    if (data->rain.valid && data->rain.detected) {
        return 150.0f;
    }

    /*
     * 优先级 2：过热时关闭一些
     */
    if (data->thermal.valid && data->thermal.heat_percent >= 80.0f) {
        return 150.0f;
    }

    /*
     * 优先级 3：根据光照调节
     */
    if (data->light.valid) {
        if (data->light.brightness_percent < 30.0f) {
            return 30.0f;
        } else if (data->light.brightness_percent < 70.0f) {
            return 90.0f;
        } else {
            return 150.0f;
        }
    }

    return 90.0f;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Smart louver P4 system starting");

    ESP_ERROR_CHECK(sensors_init());
    ESP_ERROR_CHECK(servos_init());

    float initial_angles[SERVO_COUNT] = {
        90.0f, 90.0f, 90.0f, 90.0f, 90.0f, 90.0f
    };

    ESP_ERROR_CHECK(servos_set_angles(initial_angles));

    char sensor_json[512];
    char servo_json[256];

    while (1) {
        sensor_data_t sensor_data;

        if (sensors_read(&sensor_data) == ESP_OK) {

            ESP_LOGI(
    TAG,
    "RAW_DEBUG: light_raw=%d, rain_raw=%d, rain_detected=%s, rain_wet=%.1f, thermal_raw=%d, thermal_heat=%.1f",
    sensor_data.light.raw,
    sensor_data.rain.raw,
    sensor_data.rain.detected ? "true" : "false",
    sensor_data.rain.wetness_percent,
    sensor_data.thermal.raw,
    sensor_data.thermal.heat_percent
);

            sensors_data_to_json(
                &sensor_data,
                sensor_json,
                sizeof(sensor_json)
            );

            ESP_LOGI(TAG, "SENSOR_JSON: %s", sensor_json);

            float target_angle = decide_louver_angle(&sensor_data);

            ESP_LOGI(TAG, "Target louver angle: %.1f deg", target_angle);

            ESP_ERROR_CHECK(servo_set_angle(1, target_angle));

            servos_get_json(servo_json, sizeof(servo_json));
            ESP_LOGI(TAG, "SERVO_JSON: %s", servo_json);

        } else {
            ESP_LOGE(TAG, "Failed to read sensor data");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}