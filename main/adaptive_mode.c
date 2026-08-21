/*
 * adaptive_mode.c — 用户自适应模式实现 (气候先验矩阵 + 优先级瀑布)
 *
 * 决策层次: 雨 > 保温 > 用户习惯 > 气候矩阵后验 (argmax 组合)
 * 气候类型由本地传感器判定 (温度/湿度/风速), 不依赖云端。
 *
 * 贯穿规则: 检测到雨水 (rain > 阈值) → 雨棚必展开, 直到无雨。
 */

#include "adaptive_mode.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ADAPTIVE";

/* ── 气候先验矩阵 [CLIMATE_COUNT][4] = {通风, 防风, 采光, 遮光} ── */
static const float PRIOR_MATRIX[CLIMATE_COUNT][4] = {
    /* 通风    防风    采光    遮光 */
    { 0.80f,  0.45f,  0.40f,  0.85f },  /* 炎热湿润 */
    { 0.55f,  0.70f,  0.35f,  0.90f },  /* 炎热干燥 */
    { 0.80f,  0.45f,  0.60f,  0.75f },  /* 夏热冬冷 */
    { 0.75f,  0.40f,  0.75f,  0.45f },  /* 温和湿润 */
    { 0.25f,  0.80f,  0.80f,  0.25f },  /* 寒冷干燥 */
    { 0.20f,  0.90f,  0.70f,  0.30f },  /* 寒冷湿润 */
    { 0.65f,  0.95f,  0.40f,  0.80f },  /* 沿海强风 */
};

static const char *const CLIMATE_NAMES[CLIMATE_COUNT] = {
    "炎热湿润", "炎热干燥", "夏热冬冷", "温和湿润",
    "寒冷干燥", "寒冷湿润", "沿海强风",
};

static const char *const STRATEGY_NAMES[7] = {
    "防雨", "保温", "用户习惯", "通风采光", "通风遮光", "防风采光", "防风遮光",
};

/* ── 阈值 ── */
#define RAIN_EXPAND_THRESHOLD   50.0f   /* 雨水 > 50% → 防雨 */
#define WIND_COASTAL_THRESHOLD  60.0f   /* 风速 > 60% → 沿海强风 */
#define HUMID_THRESHOLD         60.0f   /* 湿度 >= 60% → 湿润 */
#define HOT_TEMP_THRESHOLD      28.0f   /* 室外 >= 28℃ → 炎热 */
#define COLD_TEMP_THRESHOLD     10.0f   /* 室外 <= 10℃ → 寒冷 */
#define INSULATE_TEMP_IN_LO     23.0f   /* 保温: 内温 23-27 */
#define INSULATE_TEMP_IN_HI     27.0f
#define INSULATE_TEMP_OUT_LO    15.0f   /* 保温: 外温 <15 或 >30 */
#define INSULATE_TEMP_OUT_HI    30.0f
#define HABIT_WINDOW_SEC        150     /* 用户习惯: 预测时间 ±150s (5分钟窗口) */

static float clampf(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* 季节 (北半球): 0=春 1=夏 2=秋 3=冬 */
static int get_season(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    int m = tm.tm_mon + 1;   /* 1-12 */
    if (m >= 3 && m <= 5)  return 0;  /* 春 */
    if (m >= 6 && m <= 8)  return 1;  /* 夏 */
    if (m >= 9 && m <= 11) return 2;  /* 秋 */
    return 3;                         /* 冬 */
}

/* 本地气候判定 (7 选 1) */
static climate_type_t classify_climate(const adaptive_input_t *in)
{
    /* 传感器未就绪 (室外温湿度均 0) → 兜底 夏热冬冷 */
    if (in->temp_out == 0.0f && in->humi_out == 0.0f) {
        return CLIMATE_HOT_SUMMER_COLD_WINTER;
    }

    if (in->wind_pct >= WIND_COASTAL_THRESHOLD) {
        return CLIMATE_COASTAL_WINDY;
    }

    bool humid = (in->humi_out >= HUMID_THRESHOLD);

    if (in->temp_out >= HOT_TEMP_THRESHOLD) {
        return humid ? CLIMATE_HOT_HUMID : CLIMATE_HOT_DRY;
    }
    if (in->temp_out <= COLD_TEMP_THRESHOLD) {
        return humid ? CLIMATE_COLD_HUMID : CLIMATE_COLD_DRY;
    }
    /* 温和: 湿润 → 温和湿润; 干燥 → 兜底 夏热冬冷 */
    return humid ? CLIMATE_MILD_HUMID : CLIMATE_HOT_SUMMER_COLD_WINTER;
}

/* 先验矩阵 + 季节修正 + 传感器修正, 输出到 out[4] */
static void build_matrix(const adaptive_input_t *in, climate_type_t climate,
                         time_t now, float out[4])
{
    for (int i = 0; i < 4; i++) {
        out[i] = PRIOR_MATRIX[climate][i];
    }

    /* 季节修正 */
    int season = get_season(now);
    if (season == 1) {           /* 夏: 通风+0.15 防风+0.05 采光-0.10 遮光+0.20 */
        out[0] += 0.15f; out[1] += 0.05f; out[2] -= 0.10f; out[3] += 0.20f;
    } else if (season == 3) {    /* 冬: 通风-0.15 防风+0.10 采光+0.20 遮光-0.10 */
        out[0] -= 0.15f; out[1] += 0.10f; out[2] += 0.20f; out[3] -= 0.10f;
    }
    /* 春/秋: 中性, 不修正 */

    /* 传感器权重修正 */
    if (in->light_lux > 10000.0f)      { out[3] += 0.10f; out[2] -= 0.10f; }
    else if (in->light_lux < 500.0f)   { out[2] += 0.10f; out[3] -= 0.10f; }

    if (in->wind_pct > 50.0f)          { out[1] += 0.10f; out[0] -= 0.05f; }

    if (in->temp_out >= HOT_TEMP_THRESHOLD)      { out[0] += 0.10f; out[1] -= 0.05f; }
    else if (in->temp_out <= COLD_TEMP_THRESHOLD){ out[0] -= 0.10f; out[1] += 0.10f; }

    for (int i = 0; i < 4; i++) {
        out[i] = clampf(out[i]);
    }
}

/* 由矩阵后验得到 通风/防风 与 采光/遮光 的 argmax 组合 */
static strategy_t matrix_strategy(const float m[4])
{
    bool vent  = (m[0] >= m[1]);   /* 通风胜 */
    bool light = (m[2] >= m[3]);   /* 采光胜 */

    if (vent && light)      return STRATEGY_VENT_LIGHT;
    if (vent && !light)     return STRATEGY_VENT_SHADE;
    if (!vent && light)     return STRATEGY_WINDPROOF_LIGHT;
    return STRATEGY_WINDPROOF_SHADE;
}

/* 由矩阵得到百叶窗目标角度 (0=全开, 90=全关) */
static float matrix_blinds_angle(const float m[4])
{
    float vs = m[0] + m[1];
    float ls = m[2] + m[3];
    float vent_score  = (vs > 0.0f) ? (m[0] / vs) : 0.5f;
    float light_score = (ls > 0.0f) ? (m[2] / ls) : 0.5f;
    float openness = 0.5f * vent_score + 0.5f * light_score;  /* 0..1 */
    return 90.0f * (1.0f - openness);
}

/* 用户习惯: 当前时间是否落在某条预测的 ±150s 窗口内 */
static bool match_habit(const schedule_plan_t *plan, time_t now, float *angle_out)
{
    if (!plan || plan->count <= 0) return false;

    struct tm tm;
    localtime_r(&now, &tm);
    int now_sec = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;

    for (int i = 0; i < plan->count; i++) {
        int hh, mm;
        if (sscanf(plan->entries[i].time_str, "%d:%d", &hh, &mm) != 2) continue;
        int entry_sec = hh * 3600 + mm * 60;

        int diff = now_sec - entry_sec;
        if (diff < 0) diff = -diff;
        if (diff > 43200) diff = 86400 - diff;   /* 跨零点 */

        if (diff <= HABIT_WINDOW_SEC) {
            *angle_out = (float)plan->entries[i].angle;
            return true;
        }
    }
    return false;
}

void adaptive_mode_init(void)
{
    ESP_LOGI(TAG, "自适应模式初始化: 7 气候 × 4 动作 先验矩阵");
    for (int c = 0; c < CLIMATE_COUNT; c++) {
        ESP_LOGI(TAG, "  %-10s 通风%.2f 防风%.2f 采光%.2f 遮光%.2f",
                 CLIMATE_NAMES[c],
                 PRIOR_MATRIX[c][0], PRIOR_MATRIX[c][1],
                 PRIOR_MATRIX[c][2], PRIOR_MATRIX[c][3]);
    }
    ESP_LOGI(TAG, "  季节/传感器修正 + 每5s算矩阵, 每5min复核模式 (自学习)");
}

bool adaptive_mode_rain_should_expand(float rain_pct)
{
    return rain_pct > RAIN_EXPAND_THRESHOLD;
}

adaptive_decision_t adaptive_mode_evaluate(const adaptive_input_t *in,
                                           const schedule_plan_t *plan,
                                           time_t now)
{
    adaptive_decision_t d;
    memset(&d, 0, sizeof(d));

    d.climate = classify_climate(in);
    d.climate_name = CLIMATE_NAMES[d.climate];

    /* 1. 防雨 (最高优先级, 贯穿规则) */
    if (adaptive_mode_rain_should_expand(in->rain_pct)) {
        d.strategy = STRATEGY_RAIN;
        d.blinds_angle = 90.0f;          /* 防雨: 百叶窗全关 */
        d.shelter_action = SHELTER_EXPAND;
        d.strategy_name = STRATEGY_NAMES[STRATEGY_RAIN];
        build_matrix(in, d.climate, now, d.matrix);
        return d;
    }

    /* 2. 保温: 内温 23-27 且 外温 <15 或 >30 */
    if (in->temp_in >= INSULATE_TEMP_IN_LO && in->temp_in <= INSULATE_TEMP_IN_HI &&
        (in->temp_out <= INSULATE_TEMP_OUT_LO || in->temp_out >= INSULATE_TEMP_OUT_HI)) {
        d.strategy = STRATEGY_INSULATE;
        d.blinds_angle = 90.0f;          /* 保温: 全关 */
        d.shelter_action = SHELTER_HOLD; /* 雨棚不管 */
        d.strategy_name = STRATEGY_NAMES[STRATEGY_INSULATE];
        build_matrix(in, d.climate, now, d.matrix);
        return d;
    }

    /* 3. 用户习惯: 预测时间 ±5分钟窗口 */
    float habit_angle = 0.0f;
    if (match_habit(plan, now, &habit_angle)) {
        d.strategy = STRATEGY_USER_HABIT;
        d.blinds_angle = habit_angle;
        d.shelter_action = SHELTER_HOLD; /* 雨棚不管 */
        d.strategy_name = STRATEGY_NAMES[STRATEGY_USER_HABIT];
        build_matrix(in, d.climate, now, d.matrix);
        return d;
    }

    /* 4. 气候矩阵后验 (argmax 组合) */
    build_matrix(in, d.climate, now, d.matrix);
    d.strategy = matrix_strategy(d.matrix);
    d.blinds_angle = matrix_blinds_angle(d.matrix);
    d.shelter_action = SHELTER_HOLD;     /* 雨棚不管 */
    d.strategy_name = STRATEGY_NAMES[d.strategy];

    return d;
}
