#include "servo_manager.h"

#include <math.h>
#include "driver/mcpwm_timer.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_cmpr.h"
#include "driver/mcpwm_gen.h"
#include "esp_log.h"

static const char *TAG = "SERVO";

#define PWM_PERIOD_US    20000
#define PULSE_MIN_US     500
#define PULSE_MAX_US     2500

/* ESP32-P4 MCPWM Group 0: 3 operators max.
   Use 2 operators, each with 2 comparators + 2 generators → 4 servos. */
#define OP_COUNT 2
#define CH_PER_OP 2

static int          s_gpios[SERVO_COUNT];
static float        s_angle = 0.0f;
static servo_mode_t s_mode = SERVO_MODE_MANUAL;
static bool         s_initialized = false;

static mcpwm_timer_handle_t s_timer = NULL;
static mcpwm_oper_handle_t  s_opers[OP_COUNT];
static mcpwm_cmpr_handle_t  s_cmprs[SERVO_COUNT];
static mcpwm_gen_handle_t   s_gens[SERVO_COUNT];

static float clamp_angle(float a)
{
    if (a < 0.0f)   return 0.0f;
    if (a > 180.0f) return 180.0f;
    return a;
}

static uint32_t angle_to_pulse_us(float angle_deg)
{
    angle_deg = clamp_angle(angle_deg);
    return (uint32_t)(PULSE_MIN_US +
        (PULSE_MAX_US - PULSE_MIN_US) * angle_deg / 180.0f);
}

esp_err_t servo_init(const int gpios[SERVO_COUNT])
{
    esp_err_t ret;

    /* 1. One timer: 1MHz resolution, 20ms period */
    mcpwm_timer_config_t timer_cfg = {
        .group_id       = 0,
        .clk_src        = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz  = 1000000,
        .count_mode     = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks   = PWM_PERIOD_US,
    };
    ret = mcpwm_new_timer(&timer_cfg, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCPWM new timer failed");
        return ret;
    }

    /* 2. Two operators, each with 2 comparators + 2 generators → 4 servos */
    for (int op = 0; op < OP_COUNT; op++) {
        mcpwm_operator_config_t op_cfg = { .group_id = 0 };
        ret = mcpwm_new_operator(&op_cfg, &s_opers[op]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MCPWM new operator %d failed", op);
            return ret;
        }
        ret = mcpwm_operator_connect_timer(s_opers[op], s_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MCPWM connect timer op%d failed", op);
            return ret;
        }

        for (int ch = 0; ch < CH_PER_OP; ch++) {
            int idx = op * CH_PER_OP + ch;
            s_gpios[idx] = gpios[idx];

            mcpwm_comparator_config_t cmp_cfg = {};
            ret = mcpwm_new_comparator(s_opers[op], &cmp_cfg, &s_cmprs[idx]);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "MCPWM new comparator %d failed", idx);
                return ret;
            }

            mcpwm_generator_config_t gen_cfg = { .gen_gpio_num = gpios[idx] };
            ret = mcpwm_new_generator(s_opers[op], &gen_cfg, &s_gens[idx]);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "MCPWM new generator %d (GPIO%d) failed", idx, gpios[idx]);
                return ret;
            }

            /* HIGH at period start */
            mcpwm_gen_timer_event_action_t timer_action = {
                .direction = MCPWM_TIMER_DIRECTION_UP,
                .event     = MCPWM_TIMER_EVENT_EMPTY,
                .action    = MCPWM_GEN_ACTION_HIGH,
            };
            ret = mcpwm_generator_set_action_on_timer_event(s_gens[idx], timer_action);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "MCPWM timer action %d failed", idx);
                return ret;
            }

            /* LOW at compare match */
            mcpwm_gen_compare_event_action_t cmp_action = {
                .direction  = MCPWM_TIMER_DIRECTION_UP,
                .comparator = s_cmprs[idx],
                .action     = MCPWM_GEN_ACTION_LOW,
            };
            ret = mcpwm_generator_set_action_on_compare_event(s_gens[idx], cmp_action);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "MCPWM compare action %d failed", idx);
                return ret;
            }

            /* Initial: 0 deg → 500us */
            ret = mcpwm_comparator_set_compare_value(s_cmprs[idx], angle_to_pulse_us(0.0f));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "MCPWM cmp set %d failed", idx);
                return ret;
            }
        }
    }

    /* 3. Enable and start timer */
    ret = mcpwm_timer_enable(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCPWM timer enable failed");
        return ret;
    }
    ret = mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCPWM timer start failed");
        return ret;
    }

    s_angle = 0.0f;
    s_initialized = true;

    ESP_LOGI(TAG, "Servo x%d ready on GPIO%d/%d/%d/%d (MCPWM group0, 2op x2ch, init=0 deg)",
             SERVO_COUNT, s_gpios[0], s_gpios[1], s_gpios[2], s_gpios[3]);
    return ESP_OK;
}

esp_err_t servo_set_angle(float angle_deg)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    angle_deg = clamp_angle(angle_deg);
    uint32_t pulse_us = angle_to_pulse_us(angle_deg);

    for (int i = 0; i < SERVO_COUNT; i++) {
        mcpwm_comparator_set_compare_value(s_cmprs[i], pulse_us);
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
