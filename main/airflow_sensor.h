#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 风速传感器 → D0 数字脉冲 → GPIO48 (霍尔风杯, 下降沿计数+最小HIGH间隔滤磁铁位振荡, 一段低电压只算一次) */

bool airflow_sensor_init(void);
bool airflow_sensor_read(bool *has_airflow);
int16_t airflow_sensor_read_raw(void);    /* 兼容旧接口: 返回风速百分比 */
int airflow_sensor_read_pct(void);        /* 0-100% wind speed (0=无风, 永不失败) */
