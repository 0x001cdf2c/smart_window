#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROL_MODE_MANUAL = 0,
    CONTROL_MODE_ENV,
    CONTROL_MODE_ADAPTIVE,
    CONTROL_MODE_TIMER,
    CONTROL_MODE_NATURAL,
} control_mode_t;

typedef enum {
    ENV_ACTION_NONE = 0,
    ENV_ACTION_OPEN,
    ENV_ACTION_CLOSE,
} env_action_t;

typedef struct {
    char time_str[8];       /* "HH:MM" */
    char action[8];         /* "open" / "close" / "half" */
    uint8_t angle;          /* target angle 0-90 */
    int  confidence;        /* 0-100% */
} schedule_entry_t;

#define SCHEDULE_MAX_ENTRIES  4

typedef struct {
    int count;
    schedule_entry_t entries[SCHEDULE_MAX_ENTRIES];
} schedule_plan_t;

/* --- Init --- */

void control_algorithm_init(void);

/* --- Mode --- */

control_mode_t control_get_mode(void);
const char    *control_mode_name(control_mode_t mode);
void           control_set_mode(control_mode_t mode);

/* --- Env-aware --- */

env_action_t   control_env_evaluate(float temp, float temp_out,
                                     float humidity, float light,
                                     float smoke, float rain);
float          control_env_target_angle(float temp, float humidity, float light);

/* --- User-adaptive (learning) --- */

void control_adaptive_record(float angle);   /* 0°=全开, 90°=全关 */
void control_adaptive_demo_load(const uint8_t *angles, const uint8_t *hours,
                                 const uint8_t *mins, int count);
void control_adaptive_predict(void);
void control_adaptive_predict_sensor_aware(float temp, float humidity, float light);
const schedule_plan_t *control_adaptive_get_plan(void);

#define RECENT_OPS_MAX 10
typedef struct {
    char time_str[8];    /* "HH:MM" */
    char action[8];      /* "开窗"/"关窗"/"半开" */
    uint8_t angle;       /* 0-90 */
    int  day_offset;     /* 0=today, 1=yesterday ... */
} recent_op_t;

const recent_op_t *control_adaptive_get_recent_ops(int *out_count);

/* --- Timer --- */

void control_timer_set(const char *open_time, const char *close_time,
                       bool enabled, bool repeat_daily);
bool control_timer_is_enabled(void);
void control_timer_get_config(char *open_out, char *close_out,
                              bool *enabled, bool *repeat);

/* Called every second from a low-priority task */
void control_timer_tick(void);

/* Callback when a timer fires: main.c registers to route to command queue */
typedef void (*timer_fire_cb_t)(const char *action, const char *reply);
void control_set_timer_fire_callback(timer_fire_cb_t cb);

/* One-shot voice timers */
#define ONE_SHOT_MAX 6
typedef struct {
    char time[6];       /* "HH:MM" */
    char action[8];     /* "open"/"close" */
    char reply[64];     /* TTS reply when fired */
} oneshot_t;

int  control_timer_add_one_shot(const char *time_hhmm, const char *action,
                                const char *reply);
void control_timer_remove_one_shot(int idx);
int  control_timer_get_one_shots(oneshot_t *out, int max_out);

#ifdef __cplusplus
}
#endif
