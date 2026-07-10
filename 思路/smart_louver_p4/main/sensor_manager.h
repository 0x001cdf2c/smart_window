#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "system_types.h"

/**
 * @brief 初始化传感器模块
 */
esp_err_t sensors_init(void);

/**
 * @brief 获取一次格式化后的传感器数据
 *
 * @param[out] data 数据输出地址
 */
esp_err_t sensors_read(sensor_data_t *data);

/**
 * @brief 将已有传感器数据转换成 JSON
 */
esp_err_t sensors_data_to_json(
    const sensor_data_t *data,
    char *json_buffer,
    size_t buffer_size
);

/**
 * @brief 读取传感器并直接返回固定格式 JSON
 */
esp_err_t sensors_get_json(
    char *json_buffer,
    size_t buffer_size
);