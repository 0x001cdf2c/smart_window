#include "servo_manager.h"

#include <math.h>
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "SERVO";

#define PWM_FREQ_HZ      50
#define PWM_DUTY_BITS    14
#define PWM_DUTY_MAX     ((1U << PWM_DUTY_BITS) - 1U)
#define PWM_PERIOD_US    20000
#define PULSE_MIN_US     500
#define PULSE_MAX_US     2500

static int          s_gpios[SERVO_COUNT];
static float        s_angle = 0.0f;
static servo_mode_t s_mode = SERVO_MODE_MANUAL;
static bool         s_initialized = false;

static float clamp_angle(float a)
{
    if (a < 0.0f)   return 0.0f;
    if (a > 180.0f) return 180.0f;
    return a;
}

static uint32_t angle_to_duty(float angle_deg)
{
    angle_deg = clamp_angle(angle_deg);
    float pulse_us = PULSE_MIN_US +
        (PULSE_MAX_US - PULSE_MIN_US) * angle_deg / 180.0f;
    return (uint32_t)(pulse_us * PWM_DUTY_MAX / PWM_PERIOD_US);
}

esp_err_t servo_init(const int gpios[SERVO_COUNT])
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed");
        return ret;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        s_gpios[i] = gpios[i];

        ledc_channel_config_t ch_cfg = {
            .gpio_num   = gpios[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = LEDC_CHANNEL_0 + i,
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = angle_to_duty(0.0f),
            .hpoint     = 0,
        };
        ret = ledc_channel_config(&ch_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LEDC channel %d (GPIO%d) config failed", i, gpios[i]);
            return ret;
        }
    }

    s_angle = 0.0f;
    s_initialized = true;
    ESP_LOGI(TAG, "Servo x%d ready on GPIO%d/GPIO%d/GPIO%d/GPIO%d (initial=0 deg)",
             SERVO_COUNT, s_gpios[0], s_gpios[1], s_gpios[2], s_gpios[3]);
    return ESP_OK;
}

esp_err_t servo_set_angle(float angle_deg)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    angle_deg = clamp_angle(angle_deg);
    uint32_t duty = angle_to_duty(angle_deg);

    for (int i = 0; i < SERVO_COUNT; i++) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0 + i, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0 + i);
    }

    s_angle = angle_deg;
    ESP_LOGI(TAG, "Angle -> %.1f deg (x%d)", angle_deg, SERVO_COUNT);
    return ESP_OK;
}

float servo_get_angle(void)
{
    return s_angle;
}

void servo_set_mode(servo_mode_t mode)
{
    s_mode = mode;
    ESP_LOGI(TAG, "Mode -> %s", mode == SERVO_MODE_AUTO ? "AUTO" : "MANUAL");
}

servo_mode_t servo_get_mode(void)
{
    return s_mode;
}
