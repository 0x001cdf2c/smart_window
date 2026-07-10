#pragma once
#include "esp_err.h"

/* EK79007 7-inch 1024x600 MIPI DSI (official Function-EV-Board display)
   DSI data: FPC cable to MIPI_DSI connector (2-lane, 900 Mbps)
   Backlight: GPIO26 -> J1 pin31 -> HMI SubBoard PWM
   Reset:    GPIO27 -> J1 pin38 -> HMI SubBoard RST_LCD
*/

esp_err_t display_init(void);
esp_err_t display_lvgl_init(void);
void display_lvgl_task_start(void);
esp_err_t touch_init(void);
void display_fill(uint16_t color);
void display_test_pattern(void);

/* Cross-core LVGL mutex: must be held when calling any LVGL API from any task.
   lvgl_task (CPU1) also holds it during lv_timer_handler(). */
void display_lvgl_lock(void);
void display_lvgl_unlock(void);
