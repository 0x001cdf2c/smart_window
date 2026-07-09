#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SERVO_COUNT 4

typedef enum {
    SERVO_MODE_MANUAL = 0,
    SERVO_MODE_AUTO   = 1,
} servo_mode_t;

/* gpios: 长度为 SERVO_COUNT 的 GPIO 数组 */
esp_err_t servo_init(const int gpios[SERVO_COUNT]);

esp_err_t servo_set_angle(float angle_deg);
float servo_get_angle(void);

void servo_set_mode(servo_mode_t mode);
servo_mode_t servo_get_mode(void);
