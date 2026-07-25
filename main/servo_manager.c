#include "servo_manager.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/mcpwm_timer.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_cmpr.h"
#include "driver/mcpwm_gen.h"
#include "esp_log.h"

static const char *TAG = "SERVO";

#define PWM_PERIOD_US    20000
#define PULSE_MIN_US     500
#define PULSE_MAX_US     2500

/* ESP32-P4 MCPWM Group 0: 3 operators, 2 channels each → 6 servos. */
#define OP_COUNT 3
#define CH_PER_OP 2
#define BLINDS_COUNT 4   /* 前4路=百叶窗, 后2路=雨棚 */

static int          s_gpios[SERVO_COUNT];
static bool         s_inverted[SERVO_COUNT];
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

            /* Initial: 90 deg */
            float init_angle = s_inverted[idx] ? (180.0f - 90.0f) : 90.0f;
            ret = mcpwm_comparator_set_compare_value(s_cmprs[idx], angle_to_pulse_us(init_angle));
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

    s_angle = 90.0f;
    s_initialized = true;

    ESP_LOGI(TAG, "Servo x%d ready on GPIO%d/%d/%d/%d/%d/%d (init=90 deg)",
             SERVO_COUNT, s_gpios[0], s_gpios[1], s_gpios[2], s_gpios[3], s_gpios[4], s_gpios[5]);
    return ESP_OK;
}

void servo_set_inverted(int idx, bool inverted)
{
    if (idx >= 0 && idx < SERVO_COUNT) {
        s_inverted[idx] = inverted;
    }
}

esp_err_t servo_set_angle(float angle_deg)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    angle_deg = clamp_angle(angle_deg);

    for (int i = 0; i < BLINDS_COUNT; i++) {
        float a = s_inverted[i] ? (180.0f - angle_deg) : angle_deg;
        mcpwm_comparator_set_compare_value(s_cmprs[i], angle_to_pulse_us(a));
    }

    s_angle = angle_deg;
    ESP_LOGI(TAG, "Angle -> %.1f deg (blinds x%d)", angle_deg, BLINDS_COUNT);
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

/* ── 自然风模式: 仅舵机21/23 (idx=1,3) 转动, 20/22 保持0°放平 ── */
#define NATURAL_WIND_BOOST_FRAC  0.4f   /* 送风时20/22向配对舵机方向移动的比例 */

static float s_natural_wind_angle = 150.0f;
static bool  s_natural_boost = false;

esp_err_t servo_set_natural_wind_angle(float angle_deg)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    angle_deg = clamp_angle(angle_deg);
    s_natural_wind_angle = angle_deg;

    static const int active[] = {1, 3};
    static const int flat[]   = {0, 2};

    /* 20/22: boost 开启时向配对舵机方向聚拢, 否则放平 */
    float boost_angle = s_natural_boost ? (angle_deg * NATURAL_WIND_BOOST_FRAC) : 0.0f;
    for (int i = 0; i < 2; i++) {
        float a = s_inverted[flat[i]] ? (180.0f - boost_angle) : boost_angle;
        mcpwm_comparator_set_compare_value(s_cmprs[flat[i]], angle_to_pulse_us(a));
    }
    for (int i = 0; i < 2; i++) {
        float a = s_inverted[active[i]] ? (180.0f - angle_deg) : angle_deg;
        mcpwm_comparator_set_compare_value(s_cmprs[active[i]], angle_to_pulse_us(a));
    }

    ESP_LOGI(TAG, "Natural-wind -> %.1f deg (21/23), 20/22=%.1f %s",
             angle_deg, boost_angle, s_natural_boost ? "boost" : "flat");
    return ESP_OK;
}

void servo_natural_wind_boost(bool enable)
{
    if (s_natural_boost == enable) return;  /* 状态未变, 跳过 */
    s_natural_boost = enable;
    servo_set_natural_wind_angle(s_natural_wind_angle);
    ESP_LOGI(TAG, "Natural-wind boost -> %s", enable ? "ON" : "OFF");
}

/* ── 雨棚独立控制 (舵机4&5) ── */
#define RAIN_SHELTER_IDX0 4
#define RAIN_SHELTER_IDX1 5
#define RAIN_SHELTER_SPEED_DELAY_MS  40   /* 每步延时, 控制旋转速度 */
#define RAIN_SHELTER_ANGLE_STEP      2.0f  /* 每步角度增量 */

static bool s_rain_expanded = false;
static float s_rain_angle = 90.0f;  /* 雨棚当前角度, 初始90° */

static void rain_shelter_move_to(float target_angle)
{
    float cur = s_rain_angle;
    float step = RAIN_SHELTER_ANGLE_STEP;

    if (cur < target_angle) {
        for (float a = cur + step; a < target_angle; a += step) {
            float a0 = s_inverted[RAIN_SHELTER_IDX0] ? (180.0f - a) : a;
            float a1 = s_inverted[RAIN_SHELTER_IDX1] ? (180.0f - a) : a;
            mcpwm_comparator_set_compare_value(s_cmprs[RAIN_SHELTER_IDX0], angle_to_pulse_us(a0));
            mcpwm_comparator_set_compare_value(s_cmprs[RAIN_SHELTER_IDX1], angle_to_pulse_us(a1));
            vTaskDelay(pdMS_TO_TICKS(RAIN_SHELTER_SPEED_DELAY_MS));
        }
    } else {
        for (float a = cur - step; a > target_angle; a -= step) {
            float a0 = s_inverted[RAIN_SHELTER_IDX0] ? (180.0f - a) : a;
            float a1 = s_inverted[RAIN_SHELTER_IDX1] ? (180.0f - a) : a;
            mcpwm_comparator_set_compare_value(s_cmprs[RAIN_SHELTER_IDX0], angle_to_pulse_us(a0));
            mcpwm_comparator_set_compare_value(s_cmprs[RAIN_SHELTER_IDX1], angle_to_pulse_us(a1));
            vTaskDelay(pdMS_TO_TICKS(RAIN_SHELTER_SPEED_DELAY_MS));
        }
    }

    /* 最后精确到位 */
    float a0 = s_inverted[RAIN_SHELTER_IDX0] ? (180.0f - target_angle) : target_angle;
    float a1 = s_inverted[RAIN_SHELTER_IDX1] ? (180.0f - target_angle) : target_angle;
    mcpwm_comparator_set_compare_value(s_cmprs[RAIN_SHELTER_IDX0], angle_to_pulse_us(a0));
    mcpwm_comparator_set_compare_value(s_cmprs[RAIN_SHELTER_IDX1], angle_to_pulse_us(a1));

    s_rain_angle = target_angle;
}

void servo_rain_shelter_set(bool expand)
{
    if (!s_initialized) return;

    /* expand=展开(90°), collapse=收起(135°) */
    float target = expand ? 90.0f : 135.0f;

    rain_shelter_move_to(target);

    s_rain_expanded = expand;
    ESP_LOGI(TAG, "Rain shelter -> %s (%.0f deg)", expand ? "展开" : "收起", target);
}

bool servo_rain_shelter_is_expanded(void)
{
    return s_rain_expanded;
}
