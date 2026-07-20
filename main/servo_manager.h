#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SERVO_COUNT 6

typedef enum {
    SERVO_MODE_MANUAL = 0,
    SERVO_MODE_AUTO   = 1,
} servo_mode_t;

/* gpios: 长度为 SERVO_COUNT 的 GPIO 数组 */
esp_err_t servo_init(const int gpios[SERVO_COUNT]);

/* 设置指定舵机的反转模式: idx=0~5, inverted=true 时角度取反 (180-角度) */
void servo_set_inverted(int idx, bool inverted);

esp_err_t servo_set_angle(float angle_deg);
float servo_get_angle(void);

void servo_set_mode(servo_mode_t mode);
servo_mode_t servo_get_mode(void);

/* 雨棚控制 (舵机4&5): expand=true 展开, expand=false 收起 */
void servo_rain_shelter_set(bool expand);
bool servo_rain_shelter_is_expanded(void);
