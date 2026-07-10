#pragma once
#include <stdbool.h>

void ui_init(void);

/* Called periodically (e.g. every sensor loop) to update values */
void ui_update_sensor(float temp, float humidity, int light);
void ui_update_weather(const char *city, const char *weather, int high, int low, int rain_pct);
void ui_update_suggestion(const char *text);
void ui_update_mode(bool is_auto);
void ui_update_connection(bool connected);

/* Button action handler: main.c registers this to handle servo/state on UI events.
   action: "open", "close", "cw", "ccw", "manual", "auto" */
typedef void (*ui_action_handler_t)(const char *action);
void ui_set_action_handler(ui_action_handler_t handler);
