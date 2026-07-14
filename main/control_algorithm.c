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
    int     last_day_triggered;  /* day of year */
} s_timer;

/* Adaptive learning: ring buffer in NVS */
typedef struct {
    uint8_t hour;
    uint8_t min;
    uint8_t day_of_week;    /* 0=Sun .. 6=Sat */
    uint8_t action;         /* 0=close, 1=open */
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
    default:                    return "unknown";
    }
}

void control_set_mode(control_mode_t mode)
{
    s_mode = mode;
    ESP_LOGI(TAG, "Mode -> %s", control_mode_name(mode));
}

/* --- Env-aware --- */

env_action_t control_env_evaluate(float temp, float humidity, float light)
{
    int reasons = 0;

    if (temp > 30.0f)      reasons++;   /* too hot → open */
    if (humidity > 75.0f)  reasons++;   /* humid → open */
    if (light > 40000.0f)  reasons++;   /* too bright → close */
    if (light < 300.0f)    reasons++;   /* too dark → open (let light in) */

    if (reasons == 0) return ENV_ACTION_NONE;

    /* Count open vs close reasons more carefully */
    int open_reasons = 0, close_reasons = 0;
    if (temp > 28.0f)  open_reasons++;
    if (humidity > 70.0f) open_reasons++;
    if (light < 500.0f) open_reasons++;
    if (light > 35000.0f) close_reasons++;
    if (temp < 10.0f) close_reasons++;  /* too cold → close */

    if (open_reasons > close_reasons) return ENV_ACTION_OPEN;
    if (close_reasons > open_reasons) return ENV_ACTION_CLOSE;
    return ENV_ACTION_NONE;
}

float control_env_target_angle(float temp, float humidity, float light)
{
    /* 0 = fully open, 90 = closed, 180 = fully reverse */
    float angle = 0.0f;

    /* Temperature factor: 18-28°C is comfortable */
    if (temp > 28.0f)
        angle = clamp_f((temp - 28.0f) * 10.0f, 0.0f, 90.0f);
    else if (temp < 15.0f)
        angle = clamp_f((15.0f - temp) * 6.0f, 0.0f, 90.0f);

    /* Humidity factor: high humidity → open more */
    if (humidity > 70.0f)
        angle = clamp_f(angle + (humidity - 70.0f) * 2.0f, 0.0f, 90.0f);

    /* Light factor: too bright → close some */
    if (light > 30000.0f)
        angle = clamp_f(angle + (light - 30000.0f) / 500.0f, 0.0f, 90.0f);
    else if (light > 500.0f && light < 5000.0f)
        angle = clamp_f(angle - 10.0f, 0.0f, 90.0f);  /* nice light, open more */

    return angle;
}

/* --- User-adaptive --- */

void control_adaptive_record(const char *action)
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

    pattern_entry_t *e = &store->entries[store->count++];
    e->hour = (uint8_t)tm.tm_hour;
    e->min  = (uint8_t)tm.tm_min;
    e->day_of_week = (uint8_t)tm.tm_wday;
    e->action = (strcmp(action, "open") == 0) ? 1 : 0;

    nvs_save_patterns(store);

    /* Shift existing entries right, newest at [0] */
    int keep = s_recent_count;
    if (keep >= RECENT_OPS_MAX) keep = RECENT_OPS_MAX - 1;
    memmove(&s_recent_ops[1], &s_recent_ops[0], keep * sizeof(recent_op_t));
    if (s_recent_count < RECENT_OPS_MAX) s_recent_count++;

    recent_op_t *r = &s_recent_ops[0];
    snprintf(r->time_str, sizeof(r->time_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(r->action, sizeof(r->action), "%s", action);
    r->day_offset = 0;

    ESP_LOGI(TAG, "Recorded %s at %02d:%02d (day %d, total %d)",
             action, e->hour, e->min, e->day_of_week, store->count);
}

const recent_op_t *control_adaptive_get_recent_ops(int *out_count)
{
    if (out_count) *out_count = s_recent_count;
    return s_recent_ops;
}

void control_adaptive_predict(void)
{
    pattern_store_t *store = nvs_load_patterns();
    memset(&s_schedule, 0, sizeof(s_schedule));

    if (store->count < 5) {
        ESP_LOGI(TAG, "Not enough data for prediction (%d entries)", store->count);
        return;
    }

    /* Count actions per hour bucket */
    int open_buckets[24] = {0};
    int close_buckets[24] = {0};
    int count_buckets[24] = {0};

    for (int i = 0; i < store->count; i++) {
        int h = store->entries[i].hour;
        if (store->entries[i].action == 1) open_buckets[h]++;
        else close_buckets[h]++;
        count_buckets[h]++;
    }

    /* Pick top-2 open hours and top-2 close hours */
    int plan_idx = 0;
    for (int pick = 0; pick < 2 && plan_idx < SCHEDULE_MAX_ENTRIES; pick++) {
        int best_h = -1, best_cnt = 0;
        for (int h = 0; h < 24; h++) {
            if (open_buckets[h] > best_cnt) {
                best_cnt = open_buckets[h];
                best_h = h;
            }
        }
        if (best_h >= 0 && best_cnt > 0 && count_buckets[best_h] > 0) {
            s_schedule.entries[plan_idx].confidence =
                open_buckets[best_h] * 100 / count_buckets[best_h];
            int h = best_h % 24;
            s_schedule.entries[plan_idx].time_str[0] = (char)('0' + h / 10);
            s_schedule.entries[plan_idx].time_str[1] = (char)('0' + h % 10);
            s_schedule.entries[plan_idx].time_str[2] = ':';
            s_schedule.entries[plan_idx].time_str[3] = '3';
            s_schedule.entries[plan_idx].time_str[4] = '0';
            s_schedule.entries[plan_idx].time_str[5] = '\0';
            snprintf(s_schedule.entries[plan_idx].action, 8, "open");
            plan_idx++;
            open_buckets[best_h] = 0;  /* consumed */
        }
    }
    for (int pick = 0; pick < 2 && plan_idx < SCHEDULE_MAX_ENTRIES; pick++) {
        int best_h = -1, best_cnt = 0;
        for (int h = 0; h < 24; h++) {
            if (close_buckets[h] > best_cnt) {
                best_cnt = close_buckets[h];
                best_h = h;
            }
        }
        if (best_h >= 0 && best_cnt > 0 && count_buckets[best_h] > 0) {
            s_schedule.entries[plan_idx].confidence =
                close_buckets[best_h] * 100 / count_buckets[best_h];
            int h = best_h % 24;
            s_schedule.entries[plan_idx].time_str[0] = (char)('0' + h / 10);
            s_schedule.entries[plan_idx].time_str[1] = (char)('0' + h % 10);
            s_schedule.entries[plan_idx].time_str[2] = ':';
            s_schedule.entries[plan_idx].time_str[3] = '3';
            s_schedule.entries[plan_idx].time_str[4] = '0';
            s_schedule.entries[plan_idx].time_str[5] = '\0';
            snprintf(s_schedule.entries[plan_idx].action, 8, "close");
            plan_idx++;
            close_buckets[best_h] = 0;
        }
    }

    s_schedule.count = plan_idx;
    nvs_save_schedule();
    ESP_LOGI(TAG, "Prediction done: %d schedule entries", s_schedule.count);
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
    s_timer.last_day_triggered = -1;
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

    if (!s_timer.repeat_daily && s_timer.last_day_triggered == today)
        return;

    /* Check open time */
    if (s_timer.open_time[0] && strcmp(now_str, s_timer.open_time) == 0
        && s_timer.last_day_triggered != today) {
        ESP_LOGI(TAG, "Timer: OPEN at %s", now_str);
        if (!s_timer.repeat_daily) s_timer.last_day_triggered = today;
        if (s_timer_fire_cb) s_timer_fire_cb("open", "");
    }

    /* Check close time */
    if (s_timer.close_time[0] && strcmp(now_str, s_timer.close_time) == 0
        && s_timer.last_day_triggered != today) {
        ESP_LOGI(TAG, "Timer: CLOSE at %s", now_str);
        if (!s_timer.repeat_daily) s_timer.last_day_triggered = today;
        if (s_timer_fire_cb) s_timer_fire_cb("close", "");
    }
}
