#include "servo_manager.h"

#include <stdbool.h>
#include <stdio.h>

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "SERVO_MANAGER";

/*
 * 六个叶片对应的 GPIO。
 * 换开发板时主要修改这里。
 */
static const int SERVO_GPIO[SERVO_COUNT] = {
    7,
    8,
    23,
    21,
    22,
    20
};

/*
 * SG90 常用控制参数。
 * 第一轮测试不要带动机械叶片，先空载测试。
 */
#define SERVO_PWM_FREQUENCY_HZ      50
#define SERVO_MIN_PULSE_US          500
#define SERVO_MAX_PULSE_US          2500
#define SERVO_PWM_PERIOD_US         20000

#define SERVO_DUTY_BITS             14
#define SERVO_DUTY_MAX              ((1U << SERVO_DUTY_BITS) - 1U)

static float s_current_angles[SERVO_COUNT] = {
    90.0f,
    90.0f,
    90.0f,
    90.0f,
    90.0f,
    90.0f
};

static bool s_initialized = false;

static float clamp_angle(float angle_deg)
{
    if (angle_deg < 0.0f) {
        return 0.0f;
    }

    if (angle_deg > 180.0f) {
        return 180.0f;
    }

    return angle_deg;
}

static uint32_t angle_to_duty(float angle_deg)
{
    angle_deg = clamp_angle(angle_deg);

    float pulse_us =
        SERVO_MIN_PULSE_US +
        (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) *
        angle_deg / 180.0f;

    float duty =
        pulse_us *
        SERVO_DUTY_MAX /
        SERVO_PWM_PERIOD_US;

    return (uint32_t)duty;
}

esp_err_t servos_init(void)
{
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };

    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer");
        return err;
    }

    uint32_t initial_duty = angle_to_duty(90.0f);

    for (int i = 0; i < SERVO_COUNT; i++) {
        ledc_channel_config_t channel_config = {
            .gpio_num = SERVO_GPIO[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)(LEDC_CHANNEL_0 + i),
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = initial_duty,
            .hpoint = 0
        };

        err = ledc_channel_config(&channel_config);

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to configure servo %d on GPIO %d",
                i + 1,
                SERVO_GPIO[i]
            );

            return err;
        }
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Six servo channels initialized");

    return ESP_OK;
}

esp_err_t servo_set_angle(
    uint8_t blade_id,
    float angle_deg
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (blade_id < 1 || blade_id > SERVO_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    angle_deg = clamp_angle(angle_deg);

    int index = blade_id - 1;
    ledc_channel_t channel =
        (ledc_channel_t)(LEDC_CHANNEL_0 + index);

    uint32_t duty = angle_to_duty(angle_deg);

    esp_err_t err = ledc_set_duty(
        LEDC_LOW_SPEED_MODE,
        channel,
        duty
    );

    if (err != ESP_OK) {
        return err;
    }

    err = ledc_update_duty(
        LEDC_LOW_SPEED_MODE,
        channel
    );

    if (err != ESP_OK) {
        return err;
    }

    s_current_angles[index] = angle_deg;

    ESP_LOGI(
        TAG,
        "Blade %u angle set to %.1f deg",
        blade_id,
        angle_deg
    );

    return ESP_OK;
}

esp_err_t servos_set_angles(
    const float angles_deg[SERVO_COUNT]
)
{
    if (angles_deg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t blade_id = 1;
         blade_id <= SERVO_COUNT;
         blade_id++) {

        esp_err_t err = servo_set_angle(
            blade_id,
            angles_deg[blade_id - 1]
        );

        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t servos_get_angles(
    float angles_deg[SERVO_COUNT]
)
{
    if (angles_deg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        angles_deg[i] = s_current_angles[i];
    }

    return ESP_OK;
}

esp_err_t servos_get_json(
    char *json_buffer,
    size_t buffer_size
)
{
    if (json_buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(
        json_buffer,
        buffer_size,

        "{"
            "\"type\":\"servo_status\","
            "\"version\":\"1.0\","
            "\"success\":true,"
            "\"angles_deg\":["
                "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f"
            "]"
        "}",

        s_current_angles[0],
        s_current_angles[1],
        s_current_angles[2],
        s_current_angles[3],
        s_current_angles[4],
        s_current_angles[5]
    );

    if (written < 0) {
        return ESP_FAIL;
    }

    if ((size_t)written >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}