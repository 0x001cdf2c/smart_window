#include "control_algorithm.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "CTRL";
static SemaphoreHandle_t s_oneshot_mutex = NULL;

#define NVS_NAMESPACE       "ctrl_algo"
#define NVS_KEY_TIMER       "timer_cfg"
#define NVS_KEY_PATTERNS    "user_pat"
#define NVS_KEY_SCHEDULE    "schedule"

#define PATTERN_MAX         300
#define PATTERN_BUCKETS     24

static control_mode_t s_mode = CONTROL_MODE_MANUAL;

/* Timer config */
static struct {
    char    open_time[6];
    char    close_time[6];
    bool    enabled;
    bool    repeat_daily;
    int     last_open_day;   /* day of year open already fired */
    int     last_close_day;  /* day of year close already fired */
} s_timer;

/* Adaptive learning: ring buffer in NVS */
typedef struct {
    uint8_t hour;
    uint8_t min;
    uint8_t day_of_week;    /* 0=Sun .. 6=Sat */
    uint8_t angle;          /* 0°=全开 .. 90°=全关 */
} pattern_entry_t;

typedef struct {
    int count;
    pattern_entry_t entries[PATTERN_MAX];
} pattern_store_t;

static schedule_plan_t s_schedule;

/* Recent operations buffer: newest at index 0 */
static recent_op_t s_recent_ops[RECENT_OPS_MAX];
static int s_recent_count = 0;

/* --- Helpers --- */

static float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void nvs_load_timer(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_timer);
    nvs_get_blob(h, NVS_KEY_TIMER, &s_timer, &sz);
    nvs_close(h);
}

static void nvs_save_timer(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_TIMER, &s_timer, sizeof(s_timer));
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_schedule(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_schedule);
    nvs_get_blob(h, NVS_KEY_SCHEDULE, &s_schedule, &sz);
    nvs_close(h);
}

static void nvs_save_schedule(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_SCHEDULE, &s_schedule, sizeof(s_schedule));
    nvs_commit(h);
    nvs_close(h);
}

static pattern_store_t *nvs_load_patterns(void)
{
    static pattern_store_t store;
    nvs_handle_t h;
    memset(&store, 0, sizeof(store));
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return &store;
    size_t sz = sizeof(store);
    nvs_get_blob(h, NVS_KEY_PATTERNS, &store, &sz);
    nvs_close(h);
    return &store;
}

static void nvs_save_patterns(const pattern_store_t *store)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_PATTERNS, store, sizeof(*store));
    nvs_commit(h);
    nvs_close(h);
}

/* --- Init --- */

void control_algorithm_init(void)
{
    s_oneshot_mutex = xSemaphoreCreateMutex();
    s_mode = CONTROL_MODE_MANUAL;
    memset(&s_timer, 0, sizeof(s_timer));
    memset(&s_schedule, 0, sizeof(s_schedule));
    nvs_load_timer();
    nvs_load_schedule();
    ESP_LOGI(TAG, "Init OK, timer=%s, schedule entries=%d",
             s_timer.enabled ? "on" : "off", s_schedule.count);
}

/* --- Mode --- */

control_mode_t control_get_mode(void) { return s_mode; }

const char *control_mode_name(control_mode_t mode)
{
    switch (mode) {
    case CONTROL_MODE_MANUAL:   return "manual";
    case CONTROL_MODE_ENV:      return "env";
    case CONTROL_MODE_ADAPTIVE: return "adaptive";
    case CONTROL_MODE_TIMER:    return "timer";
    case CONTROL_MODE_NATURAL:  return "natural";
    default:                    return "unknown";
    }
}

void control_set_mode(control_mode_t mode)
{
    s_mode = mode;
    ESP_LOGI(TAG, "Mode -> %s", control_mode_name(mode));
}

/* --- Env-aware --- */

/*
 * 环境感知模式 — 传感器驱动的开关窗决策
 *
 * 参数:
 *   temp     室内温度 (°C)
 *   temp_out 室外温度 (°C)
 *   humidity 室内湿度 (%)
 *   light    室内光照 (lux)
 *   smoke    烟雾浓度 (模拟量, ADC 原始值)
 *   rain     雨水强度 (0-100%)
 *
 * 返回:
 *   ENV_ACTION_CLOSE  — 关闭百叶窗
 *   ENV_ACTION_OPEN   — 打开百叶窗 (具体角度由 control_env_target_angle 计算)
 *   ENV_ACTION_NONE   — 维持当前角度不变
 *
 * 决策层次:
 *   1. 安全优先 — 烟雾 / 雨水触发立即关闭, 跳过后续判断
 *   2. 强制开窗 — 高温闷热 / 高湿防霉 触发立即打开
 *   3. 温差     — 空调场景模拟 (室内温度由空调维持时关窗保持空调效果)
 *   4. 粗筛     — 极端条件快速触发 (高温 / 高湿 / 强光 / 极暗)
 *   5. 加权投票 — 温和条件下多因素加权评分决定开/关
 */

env_action_t control_env_evaluate(float temp, float temp_out,
                                   float humidity, float light,
                                   float smoke, float rain)
{
    /*-----------------------------------------
    此函数只关注窗户应该变大还是变小，返回三个值之一
    ------------------------------------------*/

    /* 第一层: 安全优先 — 烟雾或雨水超标立即关闭 */
    if (smoke > 15.0f)  return ENV_ACTION_CLOSE;  /* 烟雾浓度超标, 关窗防护 */
    if (rain > 50.0f)   return ENV_ACTION_CLOSE;  /* 雨水强度 > 50%, 关窗防水 */

    float dt = temp - temp_out;   /* dt > 0 = 室外更凉, dt < 0 = 室外更热 */

    /* 第二层: 强制开窗 — 极端通风需求, 优先于空调关窗 */
    if (temp > 30.0f && dt > 3.0f)  return ENV_ACTION_OPEN;   /* 高温闷热 + 室外更凉 → 散热通风 */
    if (humidity > 85.0f)           return ENV_ACTION_OPEN;   /* 高湿防霉 → 开窗除湿 */

    /* 第三层: 空调场景 — 关窗保持空调效果 */
    if (temp < 25.0f && dt < -3.0f)  return ENV_ACTION_CLOSE;  /* 室内凉 + 室外更热 → 关窗隔热 (制冷) */
    if (temp > 20.0f && dt > 3.0f)   return ENV_ACTION_CLOSE;  /* 室内暖 + 室外更冷 → 关窗保温 (制热) */

    /* 第四层: 粗筛 — 极端条件 gate, 任一命中才进入投票, 否则维持现状 */
    int reasons = 0;
    if (temp > 30.0f)      reasons++;  /* 高温 */
    if (humidity > 75.0f)  reasons++;  /* 高湿 */
    if (light > 40000.0f)  reasons++;  /* 强光 */
    if (light < 300.0f)    reasons++;  /* 极暗 */
    if (reasons == 0) return ENV_ACTION_NONE;

    /* 第五层: 加权投票 — 温和条件下多因素加权评分 */
    int open_score = 0, close_score = 0;

    if (temp > 28.0f)       open_score += 2;   /* 偏热 → 开窗 (权重2) */
    if (humidity > 70.0f)   open_score += 2;   /* 偏湿 → 开窗 (权重2) */
    if (light < 500.0f)     open_score += 1;   /* 偏暗 → 开窗采光 (权重1) */

    if (light > 35000.0f)   close_score += 3;  /* 偏亮 → 关窗遮光 (权重3) */
    if (temp < 10.0f)       close_score += 2;  /* 偏冷 → 关窗保温 (权重2) */

    if (open_score > close_score)  return ENV_ACTION_OPEN;
    if (close_score > open_score)  return ENV_ACTION_CLOSE;
    return ENV_ACTION_NONE;  /* 平分, 维持现状 */
}

/*
 * 环境模式 —— 计算目标舵机角度
 *
 * 根据当前温度、湿度、光照三个环境参数，综合计算一个目标角度。
 * 角度约定：0° = 全开（叶片水平），90° = 全关（叶片垂直闭合）。
 * 各因子的贡献是累加的，最终结果限制在 [0, 90] 范围内。
 *
 * 参数:
 *   temp     - 当前温度 (°C)
 *   humidity - 当前相对湿度 (%)
 *   light    - 当前光照强度 (lux)
 *
 * 返回: 目标角度 (0–90)
 */
float control_env_target_angle(float temp, float humidity, float light)
{
    float angle = 0.0f;

    /*
     * 温度因子：舒适区间 15°C – 28°C。
     * 高于 28°C → 每升高 1°C 开窗 10°，线性增长，最大 90°（全开）。
     * 低于 15°C → 每降低 1°C 开窗 6°，斜率较缓（冷天开窗需求不如热天紧迫）。
     * 在 15–28°C 区间内，温度不贡献开窗角度。
     */
    if (temp > 28.0f)
        angle = clamp_f((temp - 28.0f) * 10.0f, 0.0f, 90.0f);
    else if (temp < 15.0f)
        angle = clamp_f((15.0f - temp) * 6.0f, 0.0f, 90.0f);

    /*
     * 湿度因子：高湿叠加开窗。
     * 相对湿度 > 70% → 每超出 1% 追加 2° 开窗角度。
     * 在温度因子计算的角度之上累加，上限 90°。
     */
    if (humidity > 70.0f)
        angle = clamp_f(angle + (humidity - 70.0f) * 2.0f, 0.0f, 90.0f);

    /*
     * 光照因子：
     * > 30,000 lux（强光/直射）→ 每超出 500 lux 追加 1° 关窗角度。
     *   强光会导致眩光和升温，需要关窗遮挡。
     * 500 – 5,000 lux（柔光）→ 直接减 10°（更开），适宜的自然光鼓励开窗。
     */
    if (light > 30000.0f)
        angle = clamp_f(angle + (light - 30000.0f) / 500.0f, 0.0f, 90.0f);
    else if (light > 500.0f && light < 5000.0f)
        angle = clamp_f(angle - 10.0f, 0.0f, 90.0f);

    return angle;
}

/* --- User-adaptive --- */

/* 载入演示数据: 直接写入NVS, 时间分散在最近几天 */
void control_adaptive_demo_load(const uint8_t *angles, const uint8_t *hours,
                                 const uint8_t *mins, int count)
{
    pattern_store_t *store = nvs_load_patterns();
    time_t base = time(NULL);

    for (int i = 0; i < count && store->count < PATTERN_MAX; i++) {
        /* 每条记录回退 (count - i) * 6 小时, 分散在几天内 */
        time_t t = base - (count - i) * 6 * 3600;
        struct tm tm;
        localtime_r(&t, &tm);

        /* 覆盖为指定的时分 */
        tm.tm_hour = hours[i];
        tm.tm_min  = mins[i];

        pattern_entry_t *e = &store->entries[store->count];
        memset(e, 0, sizeof(*e));
        e->hour = (uint8_t)tm.tm_hour;
        e->min  = (uint8_t)tm.tm_min;
        e->day_of_week = (uint8_t)tm.tm_wday;
        e->angle = angles[i];
        store->count++;
    }

    nvs_save_patterns(store);

    /* 同时填充 recent_ops 以更新屏幕显示 */
    s_recent_count = 0;
    int start = store->count > RECENT_OPS_MAX ? store->count - RECENT_OPS_MAX : 0;
    for (int i = start; i < store->count; i++) {
        int ri = s_recent_count;
        snprintf(s_recent_ops[ri].time_str, sizeof(s_recent_ops[ri].time_str),
                 "%02d:%02d", store->entries[i].hour, store->entries[i].min);
        uint8_t a = store->entries[i].angle;
        if (a < 15) strcpy(s_recent_ops[ri].action, "开窗");
        else if (a > 75) strcpy(s_recent_ops[ri].action, "关窗");
        else strcpy(s_recent_ops[ri].action, "半开");
        s_recent_ops[ri].angle = a;
        s_recent_ops[ri].day_offset = 0;
        s_recent_count++;
    }

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   Demo 载入完成: %d 条记录 (共%d条)            ║\n",
           count, store->count);
    printf("╚══════════════════════════════════════════════╝\n");
    fflush(stdout);
}

void control_adaptive_record(float angle)
{
    pattern_store_t *store = nvs_load_patterns();
    if (store->count >= PATTERN_MAX) {
        memmove(store->entries, store->entries + 1,
                (PATTERN_MAX - 1) * sizeof(pattern_entry_t));
        store->count = PATTERN_MAX - 1;
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    uint8_t a = (uint8_t)(angle + 0.5f);
    if (a > 90) a = 90;

    pattern_entry_t *e = &store->entries[store->count++];
    e->hour = (uint8_t)tm.tm_hour;
    e->min  = (uint8_t)tm.tm_min;
    e->day_of_week = (uint8_t)tm.tm_wday;
    e->angle = a;

    nvs_save_patterns(store);

    /* Shift recent ops, newest at [0] */
    int keep = s_recent_count;
    if (keep >= RECENT_OPS_MAX) keep = RECENT_OPS_MAX - 1;
    memmove(&s_recent_ops[1], &s_recent_ops[0], keep * sizeof(recent_op_t));
    if (s_recent_count < RECENT_OPS_MAX) s_recent_count++;

    /* Derive action label from angle */
    const char *label;
    if (a <= 15)       label = "开窗";
    else if (a >= 75)  label = "关窗";
    else              label = "半开";

    recent_op_t *r = &s_recent_ops[0];
    snprintf(r->time_str, sizeof(r->time_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(r->action, sizeof(r->action), "%s", label);
    r->angle = a;
    r->day_offset = 0;

    ESP_LOGI(TAG, "Recorded angle=%d° (%s) at %02d:%02d (total %d)",
             a, label, e->hour, e->min, store->count);
}

const recent_op_t *control_adaptive_get_recent_ops(int *out_count)
{
    if (out_count) *out_count = s_recent_count;
    return s_recent_ops;
}

/*
 * 自适应模式 — 角度预测 (时间桶)
 *
 * 算法:
 *   1. 加载历史, 少于5条则跳过
 *   2. 24小时桶, 每桶记录: 角度总和、记录数
 *   3. 每桶 = 总和/数量 = 平均习惯角度
 *   4. 选出记录数最多的4个桶 → 生成计划
 *   5. 置信度 = 该桶记录数 / 总记录数 × 100%
 */
void control_adaptive_predict(void)
{
    pattern_store_t *store = nvs_load_patterns();
    memset(&s_schedule, 0, sizeof(s_schedule));

    if (store->count < 5) {
        ESP_LOGI(TAG, "Not enough data for prediction (%d entries)", store->count);
        return;
    }

    /* 24h buckets: sum of angles + count per hour */
    float angle_sum[24] = {0};
    int count_buckets[24] = {0};

    for (int i = 0; i < store->count; i++) {
        int h = store->entries[i].hour;
        angle_sum[h] += (float)store->entries[i].angle;
        count_buckets[h]++;
    }

    /* Sort by count descending, pick top 4 */
    int order[24];
    for (int h = 0; h < 24; h++) order[h] = h;

    for (int i = 0; i < 23; i++) {
        for (int j = i + 1; j < 24; j++) {
            if (count_buckets[order[j]] > count_buckets[order[i]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    int plan_idx = 0;
    for (int i = 0; i < 24 && plan_idx < SCHEDULE_MAX_ENTRIES; i++) {
        int h = order[i];
        if (count_buckets[h] == 0) break;

        uint8_t avg_angle = (uint8_t)(angle_sum[h] / count_buckets[h] + 0.5f);
        if (avg_angle > 90) avg_angle = 90;

        const char *act;
        if (avg_angle <= 15)      act = "open";
        else if (avg_angle >= 75) act = "close";
        else                     act = "half";

        schedule_entry_t *e = &s_schedule.entries[plan_idx];
        snprintf(e->time_str, sizeof(e->time_str), "%02d:%02d", h % 24, 30);
        snprintf(e->action, sizeof(e->action), "%s", act);
        e->angle = avg_angle;
        e->confidence = count_buckets[h] * 100 / store->count;
        plan_idx++;
    }

    s_schedule.count = plan_idx;
    nvs_save_schedule();
    ESP_LOGI(TAG, "Angle prediction: %d entries", s_schedule.count);
}

/*
 * 传感器感知角度预测
 *
 * 与纯时间桶的区别:
 *   每个小时不仅平均值, 还会被当前环境偏移:
 *     - 偏热 → 角度偏移 -20° (更开)
 *     - 偏冷 → 角度偏移 +15° (更关)
 *     - 强光 → 角度偏移 +20° (更关)
 *     - 偏暗 → 角度偏移 -15° (更开)
 *     - 偏湿 → 角度偏移 -10° (更开)
 *   时间相似度传播: 同小时 1.0 → 相邻 0.5 → 远 0.1
 */
void control_adaptive_predict_sensor_aware(float temp, float humidity, float light)
{
    pattern_store_t *store = nvs_load_patterns();
    memset(&s_schedule, 0, sizeof(s_schedule));

    /* ═══ 串口头 ═══ */
    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   角度预测 (时间+温/湿/光 加权)                    ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ 历史数据: %3d 条                                   ║\n", store->count);

    if (store->count < 5) {
        printf("║ 样本不足 (%d < 5), 回退到纯时间桶                   ║\n", store->count);
        printf("╚══════════════════════════════════════════════════╝\n");
        fflush(stdout);
        control_adaptive_predict();
        return;
    }

    /* ── 传感器偏移 ── */
    float angle_offset = 0.0f;
    printf("║ 当前环境: T=%.1f C  H=%.0f%%  L=%.0f lux            ║\n",
           temp, humidity, light);
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ 传感器偏移:                                        ║\n");

    if (temp > 28.0f) {
        angle_offset -= 20.0f;
        printf("║   ✓ 偏热 (%.1f > 28) → 角度-20 (更开)             ║\n", temp);
    }
    if (temp < 15.0f) {
        angle_offset += 15.0f;
        printf("║   ✓ 偏冷 (%.1f < 15) → 角度+15 (更关)             ║\n", temp);
    }
    if (humidity > 70.0f) {
        angle_offset -= 10.0f;
        printf("║   ✓ 偏湿 (%.0f%% > 70) → 角度-10 (更开)           ║\n", humidity);
    }
    if (light > 35000.0f) {
        angle_offset += 20.0f;
        printf("║   ✓ 强光 (%.0f > 35k) → 角度+20 (更关)            ║\n", light);
    } else if (light < 500.0f) {
        angle_offset -= 15.0f;
        printf("║   ✓ 偏暗 (%.0f < 500) → 角度-15 (更开)            ║\n", light);
    }

    if (angle_offset == 0.0f) {
        printf("║   — 环境适中, 无偏移调整                            ║\n");
    }
    printf("║  角度总偏移: %+.0f                                  ║\n", angle_offset);

    /* ── 24h 桶: 加权角度和 + 权重总和 ── */
    float w_angle[24] = {0};
    float w_total[24] = {0};
    int raw_count[24] = {0};

    /* Count raw records per hour for confidence */
    for (int i = 0; i < store->count; i++) {
        raw_count[store->entries[i].hour]++;
    }

    for (int h = 0; h < 24; h++) {
        for (int i = 0; i < store->count; i++) {
            int ph = store->entries[i].hour;
            int diff = abs(h - ph);
            if (diff > 12) diff = 24 - diff;

            float time_sim;
            if (diff == 0)      time_sim = 1.0f;
            else if (diff == 1) time_sim = 0.50f;
            else if (diff == 2) time_sim = 0.25f;
            else                time_sim = 0.05f;

            w_angle[h] += (float)store->entries[i].angle * time_sim;
            w_total[h] += time_sim;
        }
    }

    /* ── 柱状图 ── */
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║ 24h 预测角度图 (O=开窗 0   C=关窗 90)              ║\n");
    printf("║                                                  ║\n");

    for (int h = 0; h < 24; h++) {
        float raw_avg = w_total[h] > 0.01f ? w_angle[h] / w_total[h] : 45.0f;
        float adj_avg = raw_avg + angle_offset;
        if (adj_avg < 0) adj_avg = 0;
        if (adj_avg > 90) adj_avg = 90;

        /* Bar: O=open(~0 ), C=close(~90), mid=mixed */
        int bar_len = (int)(adj_avg / 90.0f * 28.0f + 0.5f);
        if (bar_len > 28) bar_len = 28;
        char bar[30];
        for (int j = 0; j < 28; j++) bar[j] = (j < bar_len) ? 'C' : 'O';
        bar[28] = '\0';

        printf("║ %02d [%s] %5.0f  |%d rec                 ║\n",
               h, bar, adj_avg, raw_count[h]);
    }

    printf("╠══════════════════════════════════════════════════╣\n");

    /* ── 选 Top-4 记录最多的小时 ── */
    int order[24];
    for (int h = 0; h < 24; h++) order[h] = h;

    for (int i = 0; i < 23; i++) {
        for (int j = i + 1; j < 24; j++) {
            if (raw_count[order[j]] > raw_count[order[i]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    int plan_idx = 0;
    for (int i = 0; i < 24 && plan_idx < SCHEDULE_MAX_ENTRIES; i++) {
        int h = order[i];
        if (raw_count[h] == 0) break;

        float raw_avg = w_total[h] > 0.01f ? w_angle[h] / w_total[h] : 45.0f;
        float adj_avg = raw_avg + angle_offset;
        if (adj_avg < 0) adj_avg = 0;
        if (adj_avg > 90) adj_avg = 90;
        uint8_t a = (uint8_t)(adj_avg + 0.5f);

        const char *act;
        if (a <= 15)      act = "open";
        else if (a >= 75) act = "close";
        else             act = "half";

        schedule_entry_t *e = &s_schedule.entries[plan_idx];
        snprintf(e->time_str, sizeof(e->time_str), "%02d:%02d", h % 24, 30);
        snprintf(e->action, sizeof(e->action), "%s", act);
        e->angle = a;
        e->confidence = raw_count[h] * 100 / store->count;
        plan_idx++;
    }

    s_schedule.count = plan_idx;

    /* ── 预测表 ── */
    printf("║ 预测计划 (传感器感知 + 角度):                      ║\n");
    printf("║ ┌────┬────────┬────────┬────────┬──────┐        ║\n");
    printf("║ │  # │  时间   │  角度   │  动作   │ 置信度 │        ║\n");
    printf("║ ├────┼────────┼────────┼────────┼──────┤        ║\n");

    if (plan_idx == 0) {
        printf("║ │  — │   —    │   —    │   —    │  样本不足│      ║\n");
    } else {
        for (int i = 0; i < plan_idx; i++) {
            const char *act_label;
            if (strcmp(s_schedule.entries[i].action, "open") == 0)   act_label = "开窗";
            else if (strcmp(s_schedule.entries[i].action, "close") == 0) act_label = "关窗";
            else                                                     act_label = "半开";
            printf("║ │ %d  │ %s  │   %3d   │  %s   │  %3d%%  │        ║\n",
                   i + 1, s_schedule.entries[i].time_str,
                   s_schedule.entries[i].angle,
                   act_label,
                   s_schedule.entries[i].confidence);
        }
    }
    printf("║ └────┴────────┴────────┴────────┴──────┘        ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    fflush(stdout);

    nvs_save_schedule();
    ESP_LOGI(TAG, "Angle prediction: %d entries (T=%.1f H=%.0f L=%.0f offset=%+.0f)",
             s_schedule.count, temp, humidity, light, angle_offset);
}

const schedule_plan_t *control_adaptive_get_plan(void)
{
    return &s_schedule;
}

/* --- One-shot timer array --- */
static oneshot_t s_one_shots[ONE_SHOT_MAX];
static int s_one_shot_count = 0;
static timer_fire_cb_t s_timer_fire_cb = NULL;

void control_set_timer_fire_callback(timer_fire_cb_t cb)
{
    s_timer_fire_cb = cb;
}

int control_timer_add_one_shot(const char *time_hhmm, const char *action,
                               const char *reply)
{
    if (!s_oneshot_mutex) return -1;
    xSemaphoreTake(s_oneshot_mutex, portMAX_DELAY);
    if (s_one_shot_count >= ONE_SHOT_MAX) {
        xSemaphoreGive(s_oneshot_mutex);
        return -1;
    }
    oneshot_t *o = &s_one_shots[s_one_shot_count];
    strncpy(o->time, time_hhmm, 5);
    o->time[5] = '\0';
    strncpy(o->action, action, 7);
    o->action[7] = '\0';
    if (reply) {
        strncpy(o->reply, reply, 63);
        o->reply[63] = '\0';
    } else {
        o->reply[0] = '\0';
    }
    int idx = s_one_shot_count++;
    xSemaphoreGive(s_oneshot_mutex);
    ESP_LOGI(TAG, "One-shot #%d: %s %s reply=%s",
             idx, time_hhmm, action, reply ? reply : "");
    return idx;
}

void control_timer_remove_one_shot(int idx)
{
    if (!s_oneshot_mutex) return;
    xSemaphoreTake(s_oneshot_mutex, portMAX_DELAY);
    if (idx < 0 || idx >= s_one_shot_count) {
        xSemaphoreGive(s_oneshot_mutex);
        return;
    }
    memmove(&s_one_shots[idx], &s_one_shots[idx + 1],
            (s_one_shot_count - idx - 1) * sizeof(oneshot_t));
    s_one_shot_count--;
    xSemaphoreGive(s_oneshot_mutex);
}

int control_timer_get_one_shots(oneshot_t *out, int max_out)
{
    if (!s_oneshot_mutex) return 0;
    xSemaphoreTake(s_oneshot_mutex, portMAX_DELAY);
    int n = (s_one_shot_count < max_out) ? s_one_shot_count : max_out;
    if (out && n > 0) memcpy(out, s_one_shots, n * sizeof(oneshot_t));
    xSemaphoreGive(s_oneshot_mutex);
    return n;
}

/* --- Timer --- */

void control_timer_set(const char *open_time, const char *close_time,
                       bool enabled, bool repeat_daily)
{
    if (open_time)  strncpy(s_timer.open_time,  open_time,  5);
    if (close_time) strncpy(s_timer.close_time, close_time, 5);
    s_timer.open_time[5] = '\0';
    s_timer.close_time[5] = '\0';
    s_timer.enabled = enabled;
    s_timer.repeat_daily = repeat_daily;
    s_timer.last_open_day = -1;
    s_timer.last_close_day = -1;
    nvs_save_timer();
    ESP_LOGI(TAG, "Timer: open=%s close=%s en=%d repeat=%d",
             s_timer.open_time, s_timer.close_time, enabled, repeat_daily);
}

bool control_timer_is_enabled(void) { return s_timer.enabled; }

void control_timer_get_config(char *open_out, char *close_out,
                              bool *enabled, bool *repeat)
{
    if (open_out)  strcpy(open_out,  s_timer.open_time);
    if (close_out) strcpy(close_out, s_timer.close_time);
    if (enabled)   *enabled  = s_timer.enabled;
    if (repeat)    *repeat   = s_timer.repeat_daily;
}

void control_timer_tick(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) return;  /* clock not synced yet; avoid spurious fires */
    struct tm tm;
    localtime_r(&now, &tm);
    char now_str[6];
    snprintf(now_str, sizeof(now_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
    int today = tm.tm_yday;

    /* ── One-shot timers (voice-set) ── */
    if (s_oneshot_mutex) xSemaphoreTake(s_oneshot_mutex, portMAX_DELAY);
    for (int i = s_one_shot_count - 1; i >= 0; i--) {
        if (strcmp(now_str, s_one_shots[i].time) == 0) {
            oneshot_t fired = s_one_shots[i];
            ESP_LOGI(TAG, "One-shot timer fired: %s %s", fired.time, fired.action);
            /* Remove before firing to avoid re-fire */
            memmove(&s_one_shots[i], &s_one_shots[i + 1],
                    (s_one_shot_count - i - 1) * sizeof(oneshot_t));
            s_one_shot_count--;
            if (s_oneshot_mutex) xSemaphoreGive(s_oneshot_mutex);
            /* Route through main.c command queue for thread safety */
            if (s_timer_fire_cb) {
                s_timer_fire_cb(fired.action, fired.reply);
            }
            if (s_oneshot_mutex) xSemaphoreTake(s_oneshot_mutex, portMAX_DELAY);
        }
    }
    if (s_oneshot_mutex) xSemaphoreGive(s_oneshot_mutex);

    /* ── Daily/repeat timer ── */
    if (!s_timer.enabled) return;
    if (s_timer.open_time[0] == '\0' && s_timer.close_time[0] == '\0') return;

    /* Check open time */
    if (s_timer.open_time[0] && strcmp(now_str, s_timer.open_time) == 0
        && s_timer.last_open_day != today) {
        ESP_LOGI(TAG, "Timer: OPEN at %s", now_str);
        s_timer.last_open_day = today;
        if (s_timer_fire_cb) s_timer_fire_cb("open", "");
    }

    /* Check close time */
    if (s_timer.close_time[0] && strcmp(now_str, s_timer.close_time) == 0
        && s_timer.last_close_day != today) {
        ESP_LOGI(TAG, "Timer: CLOSE at %s", now_str);
        s_timer.last_close_day = today;
        if (s_timer_fire_cb) s_timer_fire_cb("close", "");
    }

    /* One-shot ("once") timer: disable after its configured times fire */
    if (!s_timer.repeat_daily) {
        bool open_done  = (s_timer.open_time[0] == '\0')  || (s_timer.last_open_day == today);
        bool close_done = (s_timer.close_time[0] == '\0') || (s_timer.last_close_day == today);
        if (open_done && close_done) {
            s_timer.enabled = false;
            nvs_save_timer();
            ESP_LOGI(TAG, "Timer: one-shot complete, disabled");
        }
    }
}
