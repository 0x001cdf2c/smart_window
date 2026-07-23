#pragma once
#include <stdbool.h>

void ui_init(void);

/* Called periodically (e.g. every sensor loop) to update values */
void ui_update_sensor(float t_in, float h_in, float t_out, float h_out, int light);
void ui_update_smoke(int smoke);
void ui_update_rain(int rain);
void ui_update_airflow(bool has_airflow);
void ui_update_weather(const char *city, const char *weather, int high, int low, int rain_pct);
void ui_update_mode(bool is_auto);
void ui_update_connection(bool connected);
void ui_update_mode_info(const char *mode_name, const char *detail, const char *schedule_text);
void ui_update_timer_schedule(const char *text);
void ui_update_mode_highlight(const char *mode_name);
void ui_update_adaptive_info(const char *detail,
                              const char *pred_left,
                              const char *recent_right);
/* Button action handler: main.c registers this to handle servo/state on UI events.
   action: "open", "close", "cw", "ccw", "manual", "auto" */
typedef void (*ui_action_handler_t)(const char *action);
void ui_set_action_handler(ui_action_handler_t handler);
