#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SERVO_COUNT 6

/**
 * @brief 初始化六路舵机 PWM
 */
esp_err_t servos_init(void);

/**
 * @brief 设置一个叶片的角度
 *
 * @param blade_id 叶片编号，范围 1～6
 * @param angle_deg 目标角度，范围 0～180°
 */
esp_err_t servo_set_angle(
    uint8_t blade_id,
    float angle_deg
);

/**
 * @brief 同时设置六个叶片角度
 */
esp_err_t servos_set_angles(
    const float angles_deg[SERVO_COUNT]
);

/**
 * @brief 获取当前六个叶片角度
 */
esp_err_t servos_get_angles(
    float angles_deg[SERVO_COUNT]
);

/**
 * @brief 获取舵机状态 JSON
 */
esp_err_t servos_get_json(
    char *json_buffer,
    size_t buffer_size
);